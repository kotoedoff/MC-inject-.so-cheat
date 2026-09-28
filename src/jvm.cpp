#include "jvm.h"
#include "log.h"
#include "snapshot.h"
#include "mappings.h"
#include "config.h"
#include <atomic>
#include <dlfcn.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unistd.h>
#include <jvmti.h>

namespace voltage {
namespace jvm {

namespace {
using GetVMsFn = jint (*)(JavaVM**, jsize, jsize*);
GetVMsFn resolve_get_created_vms() {
  // JNI_GetCreatedJavaVMs lives in libjvm.so, already loaded inside MC process.
  // RTLD_DEFAULT searches all loaded objects.
  void* sym = dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
  return reinterpret_cast<GetVMsFn>(sym);
}
} // namespace

int created_vm_count() {
  auto fn = resolve_get_created_vms();
  if (!fn) return -1;
  JavaVM* buf[4]; jsize n = 0;
  if (fn(buf, 4, &n) != JNI_OK) return -1;
  return (int)n;
}

JavaVM* first_vm() {
  auto fn = resolve_get_created_vms();
  if (!fn) return nullptr;
  JavaVM* buf[4]; jsize n = 0;
  if (fn(buf, 4, &n) != JNI_OK || n < 1) return nullptr;
  return buf[0];
}

// Get-or-attach a JNIEnv for the CALLING thread. The render thread is a bare
// pthread created by GLFW/LWJGL: it is NOT attached to the JVM, so plain
// GetEnv returns JNI_EDETACHED and every JNI call would be a silent no-op.
// This is the root cause that killed render-thread camera/TriggerBot before.
static JNIEnv* env_or_attach(JavaVM* vm) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env) return env;
  if (vm->AttachCurrentThread((void**)&env, nullptr) == JNI_OK && env) return env;
  return nullptr;
}

void start_background_probe() {
  std::thread([] {
    VLOG("jvm", "probe thread started, waiting for JVM...");
    for (int i = 0; i < 60; i++) {
      int n = created_vm_count();
      if (n > 0) {
        VLOG("jvm", "found %d JVM(s)", n);
        JavaVM* vm = first_vm();
        if (!vm) { VLOG("jvm", "first_vm() null"); return; }
        JNIEnv* env = env_or_attach(vm);
        if (!env) { VLOG("jvm", "no env for probe thread"); return; }
        VLOG("jvm", "probe thread JNIEnv ready, jni version=0x%x", (unsigned)env->GetVersion());
        dump_entities_loop(vm, env);
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    VLOG("jvm", "no JVM found after 60s (normal for non-java test processes)");
  }).detach();
}

// ---- 1.21.4 vanilla + Fabric entity dump -----------------------------------
// Names come from docs/1.21.4_jni_names.md (vanilla obf) and the FabricMC
// intermediary tiny map (fabric). Every fallible JNI call is followed by
// exception_ok(); entity iteration is capped and local refs are released.
namespace {
constexpr int kMaxDump = 30;

bool exception_ok(JNIEnv* env, const char* what) {
  if (env->ExceptionCheck()) {
    VLOG("jvm", "JNI exception at %s (cleared)", what);
    env->ExceptionClear();
    return false;
  }
  return true;
}

// Resolved JNI ids, published by the producer loop, consumed by refresh_camera
// on the render thread. Global refs must only be released on the producer thread.
struct Res {
  map::Profile profile = map::Profile::Vanilla;
  jclass mcCls = nullptr; jmethodID getInstance = nullptr;
  jfieldID fLevel = nullptr; jfieldID fPlayer = nullptr;
  jmethodID mEntities = nullptr;
  jmethodID mGetX = nullptr, mGetY = nullptr, mGetZ = nullptr;
  jmethodID mAlive = nullptr, mName = nullptr, mGetString = nullptr;
  jclass playerCls = nullptr;
  jclass livingCls = nullptr;      // global ref: LivingEntity gate (aim/trigger targets)
  jclass entityCls = nullptr;      // global ref: gate for ALL per-entity JNI (stale-object SIGSEGV guard)
  jmethodID mHasNext = nullptr, mNext = nullptr, mIterator = nullptr, mClassName = nullptr;
  jfieldID fGameRenderer = nullptr;
  jmethodID mGetMainCamera = nullptr, mGetPosition = nullptr;
  jmethodID mGetXRot = nullptr, mGetYRot = nullptr, mGetFov = nullptr;
  jfieldID vX = nullptr, vY = nullptr, vZ = nullptr;
  jmethodID mGetBox = nullptr;
  jfieldID bMinX = nullptr, bMinY = nullptr, bMinZ = nullptr;
  jfieldID bMaxX = nullptr, bMaxY = nullptr, bMaxZ = nullptr;
  // TriggerBot members (optional: triggerbot stays off if unresolved)
  jfieldID fHitResult = nullptr;
  jfieldID fGameMode = nullptr;
  jclass entityHitCls = nullptr;  // global ref: IsInstanceOf gate before getEntity()
  jmethodID mHitEntity = nullptr;   // EntityHitResult.getEntity()
  jmethodID mAttack = nullptr;      // GameMode.attack(Player, Entity)
  jmethodID mSwing = nullptr;       // LivingEntity.swing(Hand)
  jmethodID mCooldown = nullptr;    // Player.getAttackCooldownProgress(F)F
  jclass handCls = nullptr;         // InteractionHand / Hand
  jfieldID fMainHand = nullptr;     // MAIN_HAND static
  // AutoSprint members
  jmethodID mSetSprinting = nullptr;  // Entity.setSprinting(Z)V
  jmethodID mIsSprinting = nullptr;   // Entity.isSprinting()Z
  jmethodID mJumpFromGround = nullptr;// LivingEntity.jumpFromGround()V
  // OnlyCrit (vanilla rule): fallDistance>0, airborne, dry, no sprint
  jfieldID fFallDistance = nullptr;   // Entity.fallDistance float
  jmethodID mOnGround = nullptr;      // Entity onGround()/isOnGround()Z
  jmethodID mTouchWater = nullptr;    // Entity isInWater()/isTouchingWater()Z
  // AimAssist: direct rotation write, render thread only
  jmethodID mSetYaw = nullptr;        // Entity.setYRot(F)V / setYaw(F)V
  jmethodID mSetPitch = nullptr;      // Entity.setXRot(F)V / setPitch(F)V
  jmethodID mGetVel = nullptr;        // Entity.getVelocity()LVec3; (prediction)
  jclass rsCls = nullptr;
  jmethodID mGetProj = nullptr;
  jfieldID pM[16] = {};
};
Res g_res;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_armed{false};
// Tripwire: set on any triggerbot-chain exception; cleared on world exit/menu
// so rejoining re-arms. A bad JNI receiver segfaults the JVM — never retry blindly.
std::atomic<bool> g_tb_tripped{false};

// Game classloader: plain FindClass only sees the system loader and goes blind
// under Fabric (KnotClassLoader). Resolve game classes through the loader of
// the already-loaded gate class instead. Works for both profiles; java/*
// bootstrap classes keep using plain FindClass.
jobject g_loader = nullptr;      // global ref to the game's ClassLoader
jmethodID g_loadClass = nullptr; // ClassLoader.loadClass(String)

bool init_game_loader(JavaVM* vm, JNIEnv* env, const char* gateSig) {
  jvmtiEnv* jvmti = nullptr;
  if (vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0) != JNI_OK || !jvmti) return false;
  jint n = 0; jclass* arr = nullptr;
  bool ok = false;
  if (jvmti->GetLoadedClasses(&n, &arr) == JVMTI_ERROR_NONE && arr) {
    for (int i = 0; i < n && !ok; i++) {
      char* sig = nullptr; char* gen = nullptr;
      if (jvmti->GetClassSignature(arr[i], &sig, &gen) == JVMTI_ERROR_NONE) {
        if (sig && std::strcmp(sig, gateSig) == 0) {
          jobject ldr = nullptr;
          if (jvmti->GetClassLoader(arr[i], &ldr) == JVMTI_ERROR_NONE && ldr) {
            g_loader = env->NewGlobalRef(ldr);
            jclass cl = env->FindClass("java/lang/ClassLoader");
            if (cl && !env->ExceptionCheck())
              g_loadClass = env->GetMethodID(cl, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
            if (cl) env->DeleteLocalRef(cl);
            ok = g_loader && g_loadClass && !env->ExceptionCheck();
            env->ExceptionClear();
          }
        }
        if (sig) jvmti->Deallocate((unsigned char*)sig);
        if (gen) jvmti->Deallocate((unsigned char*)gen);
      }
    }
    jvmti->Deallocate((unsigned char*)arr);
  }
  if (ok) VLOG("jvm", "game classloader captured");
  else VLOG("jvm", "game classloader NOT captured (FindClass fallback)");
  return ok;
}

// Load (link, never init) a game class by internal name (slashes).
// Returns local ref or null with a cleared exception.
jclass gameClass(JNIEnv* env, const char* internal) {
  if (g_loader && g_loadClass) {
    std::string dotted(internal);
    for (char& c : dotted) if (c == '/') c = '.';
    jstring name = env->NewStringUTF(dotted.c_str());
    if (!name) { env->ExceptionClear(); return nullptr; }
    jobject cls = env->CallObjectMethod(g_loader, g_loadClass, name);
    env->DeleteLocalRef(name);
    if (env->ExceptionCheck() || !cls) { env->ExceptionClear(); return nullptr; }
    return (jclass)cls;
  }
  // Fallback: system loader (works on vanilla -cp, blind under Fabric).
  jclass cls = env->FindClass(internal);
  if (env->ExceptionCheck() || !cls) { env->ExceptionClear(); return nullptr; }
  return cls;
}
} // namespace

bool jvm_armed() { return g_armed.load(std::memory_order_acquire); }

// Camera/AABB ids resolved LATE (they can fail during the 20s grace: e.g.
// fks/class_4184 is not resolvable from GetMethodID's descriptor until the
// game actually constructs the camera). Retry lazily from either thread.
static void retry_camera_resolve(JNIEnv* env, Res& r);

// Scan already-loaded classes (JVMTI, side-effect free) and pick the mapping
// profile: Fabric intermediary class_310 or vanilla obfuscated flk.
map::Profile detect_profile(JavaVM* vm) {
  jvmtiEnv* jvmti = nullptr;
  if (vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0) != JNI_OK || !jvmti)
    return map::Profile::Vanilla;
  jint n = 0; jclass* arr = nullptr;
  if (jvmti->GetLoadedClasses(&n, &arr) != JVMTI_ERROR_NONE || !arr)
    return map::Profile::Vanilla;
  bool flk = false, fabric = false;
  for (int i = 0; i < n && !(flk && fabric); i++) {
    char* sig = nullptr; char* gen = nullptr;
    if (jvmti->GetClassSignature(arr[i], &sig, &gen) == JVMTI_ERROR_NONE) {
      if (sig) {
        if (std::strcmp(sig, "Lflk;") == 0) flk = true;
        else if (std::strcmp(sig, "Lnet/minecraft/class_310;") == 0) fabric = true;
        jvmti->Deallocate((unsigned char*)sig);
      }
      if (gen) jvmti->Deallocate((unsigned char*)gen);
    }
  }
  jvmti->Deallocate((unsigned char*)arr);
  return fabric && !flk ? map::Profile::Fabric : map::Profile::Vanilla;
}

void dump_entities_loop(JavaVM* vm, JNIEnv* env) {
  auto publish_res = [&](Res& r) {
    g_res = r;
    g_ready.store(true, std::memory_order_release);
  };

  auto try_resolve = [&](Res& r) -> bool {
    env->ExceptionClear();
    const map::Names& N = map::names(r.profile);
    const char* MC = map::mcSig(r.profile);
    jclass mc = gameClass(env, N.mc);
    if (env->ExceptionCheck() || !mc) { env->ExceptionClear(); return false; }
    r.mcCls = (jclass)env->NewGlobalRef(mc); env->DeleteLocalRef(mc);
    // static Minecraft.getInstance(): ()LMinecraft;
    std::string getInstanceSig = std::string("()") + MC;
    r.getInstance = env->GetStaticMethodID(r.mcCls, N.getInstance, getInstanceSig.c_str());
    r.fLevel = env->GetFieldID(r.mcCls, N.fLevel, map::levelSig(r.profile));
    r.fPlayer = env->GetFieldID(r.mcCls, N.fPlayer, map::playerSig(r.profile));
    if (env->ExceptionCheck() || !r.getInstance || !r.fLevel || !r.fPlayer) { env->ExceptionClear(); return false; }
    jclass lvl = gameClass(env, N.level);
    r.mEntities = lvl ? env->GetMethodID(lvl, N.mGetEntities, "()Ljava/lang/Iterable;") : nullptr;
    if (lvl) env->DeleteLocalRef(lvl);
    jclass ent = gameClass(env, N.entity);
    if (ent) {
      r.mGetX = env->GetMethodID(ent, N.mGetX, "()D");
      r.mGetY = env->GetMethodID(ent, N.mGetY, "()D");
      r.mGetZ = env->GetMethodID(ent, N.mGetZ, "()D");
      r.mAlive = env->GetMethodID(ent, N.mAlive, "()Z");
      // 1.21.4: Entity.getName() lives on the deepest class in the tiny map
      // (vanilla: bso/ComponentHolder, fabric: class_1275), not on bum itself.
      // Resolve on the entity class first, then fall back to a descriptor-
      // verified lookup on the ComponentHolder class.
      std::string nameSig = std::string("()") + map::compSig(r.profile);
      r.mName = env->GetMethodID(ent, N.mName, nameSig.c_str());
      env->ExceptionClear();
      if (!r.mName) {
        const char* holder = r.profile == map::Profile::Fabric ? "net/minecraft/class_1275" : "bso";
        jclass hc = gameClass(env, holder);
        if (hc && !env->ExceptionCheck()) {
          r.mName = env->GetMethodID(hc, N.mName, nameSig.c_str());
          env->ExceptionClear();
        }
        if (hc) env->DeleteLocalRef(hc);
      }
      env->DeleteLocalRef(ent);
    }
    if (env->ExceptionCheck() || !r.mEntities || !r.mGetX || !r.mGetY || !r.mGetZ) { env->ExceptionClear(); return false; }
    // Component.getString(): vanilla keeps the name, fabric intermediary is method_54160 (N.mGetString).
    jclass comp = gameClass(env, N.component);
    r.mGetString = comp ? env->GetMethodID(comp, N.mGetString, "()Ljava/lang/String;") : nullptr;
    if (comp) env->DeleteLocalRef(comp);
    jclass pl = gameClass(env, N.playerBase);
    if (pl && !env->ExceptionCheck()) { r.playerCls = (jclass)env->NewGlobalRef(pl); }
    env->ExceptionClear();
    if (pl) env->DeleteLocalRef(pl);
    jclass lv = gameClass(env, N.living);
    if (lv && !env->ExceptionCheck()) { r.livingCls = (jclass)env->NewGlobalRef(lv); }
    env->ExceptionClear();
    if (lv) env->DeleteLocalRef(lv);
    jclass ec = gameClass(env, N.entity);
    if (ec && !env->ExceptionCheck()) { r.entityCls = (jclass)env->NewGlobalRef(ec); }
    env->ExceptionClear();
    if (ec) env->DeleteLocalRef(ec);
    jclass itc = env->FindClass("java/util/Iterator");
    r.mHasNext = itc ? env->GetMethodID(itc, "hasNext", "()Z") : nullptr;
    r.mNext = itc ? env->GetMethodID(itc, "next", "()Ljava/lang/Object;") : nullptr;
    if (itc) env->DeleteLocalRef(itc);
    jclass ite = env->FindClass("java/lang/Iterable");
    r.mIterator = ite ? env->GetMethodID(ite, "iterator", "()Ljava/util/Iterator;") : nullptr;
    if (ite) env->DeleteLocalRef(ite);
    jclass clc = env->FindClass("java/lang/Class");
    r.mClassName = clc ? env->GetMethodID(clc, "getName", "()Ljava/lang/String;") : nullptr;
    if (clc) env->DeleteLocalRef(clc);
    if (env->ExceptionCheck() || !r.mHasNext || !r.mNext || !r.mIterator) { env->ExceptionClear(); return false; }
    // camera + AABB (non-fatal: ESP degrades, dump still works)
    env->ExceptionClear();
    r.fGameRenderer = env->GetFieldID(r.mcCls, N.fGameRenderer, map::grSig(r.profile));
    env->ExceptionClear();
    jclass gr = gameClass(env, N.gameRenderer);
    if (gr && !env->ExceptionCheck()) {
      r.mGetMainCamera = env->GetMethodID(gr, N.mGetMainCamera, (std::string("()") + map::camSig(r.profile)).c_str());
      env->ExceptionClear();
    }
    if (gr) env->DeleteLocalRef(gr);
    jclass cam = gameClass(env, N.camera);
    if (cam && !env->ExceptionCheck()) {
      r.mGetPosition = env->GetMethodID(cam, N.mGetPosition, (std::string("()") + map::vecSig(r.profile)).c_str());
      r.mGetXRot = env->GetMethodID(cam, N.mGetXRot, "()F");
      r.mGetYRot = env->GetMethodID(cam, N.mGetYRot, "()F");
      env->ExceptionClear();
    }
    if (cam) env->DeleteLocalRef(cam);
    jclass vec = gameClass(env, N.vec3);
    if (vec && !env->ExceptionCheck()) {
      r.vX = env->GetFieldID(vec, N.vX, "D");
      r.vY = env->GetFieldID(vec, N.vY, "D");
      r.vZ = env->GetFieldID(vec, N.vZ, "D");
      env->ExceptionClear();
    }
    if (vec) env->DeleteLocalRef(vec);
    jclass gr2 = gameClass(env, N.gameRenderer);
    if (gr2 && !env->ExceptionCheck()) {
      // getFov(Camera,float,boolean): (LCamera;FZ)F
      std::string fovSig = std::string("(") + map::camSig(r.profile) + "FZ)F";
      r.mGetFov = env->GetMethodID(gr2, N.mGetFov, fovSig.c_str());
      env->ExceptionClear();
    }
    if (gr2) env->DeleteLocalRef(gr2);
    jclass box = gameClass(env, N.aabb);
    if (box && !env->ExceptionCheck()) {
      r.bMinX = env->GetFieldID(box, N.bMinX, "D"); r.bMinY = env->GetFieldID(box, N.bMinY, "D");
      r.bMinZ = env->GetFieldID(box, N.bMinZ, "D"); r.bMaxX = env->GetFieldID(box, N.bMaxX, "D");
      r.bMaxY = env->GetFieldID(box, N.bMaxY, "D"); r.bMaxZ = env->GetFieldID(box, N.bMaxZ, "D");
      env->ExceptionClear();
    }
    if (box) env->DeleteLocalRef(box);
    jclass ent2 = gameClass(env, N.entity);
    if (ent2 && !env->ExceptionCheck()) {
      r.mGetBox = env->GetMethodID(ent2, N.mGetBox, (std::string("()") + map::aabbSig(r.profile)).c_str());
      env->ExceptionClear();
    }
    if (ent2) env->DeleteLocalRef(ent2);
    // TriggerBot members (optional).
    r.fHitResult = env->GetFieldID(r.mcCls, N.fHitResult, map::hitSig(r.profile));
    r.fGameMode = env->GetFieldID(r.mcCls, N.fGameMode, map::gmSig(r.profile));
    env->ExceptionClear();
    jclass ehit = gameClass(env, N.entityHit);
    if (ehit && !env->ExceptionCheck()) {
      r.mHitEntity = env->GetMethodID(ehit, N.mHitEntity, (std::string("()") + map::entSig(r.profile)).c_str());
      if (!env->ExceptionCheck() && r.mHitEntity && !r.entityHitCls)
        r.entityHitCls = (jclass)env->NewGlobalRef(ehit);
      env->ExceptionClear();
    }
    if (ehit) env->DeleteLocalRef(ehit);
    jclass gm = gameClass(env, N.gameMode);
    if (gm && !env->ExceptionCheck()) {
      std::string atkSig = std::string("(") + map::plSig(r.profile) + map::entSig(r.profile) + ")V";
      r.mAttack = env->GetMethodID(gm, N.mAttack, atkSig.c_str());
      env->ExceptionClear();
    }
    if (gm) env->DeleteLocalRef(gm);
    jclass living = gameClass(env, N.living);
    if (living && !env->ExceptionCheck()) {
      std::string swingSig = std::string("(") + map::handSig(r.profile) + ")V";
      r.mSwing = env->GetMethodID(living, N.mSwing, swingSig.c_str());
      env->ExceptionClear();
    }
    if (living) env->DeleteLocalRef(living);
    jclass plc = gameClass(env, N.playerBase);
    if (plc && !env->ExceptionCheck()) {
      r.mCooldown = env->GetMethodID(plc, N.mCooldown, "(F)F");
      env->ExceptionClear();
    }
    if (plc) env->DeleteLocalRef(plc);
    jclass hand = gameClass(env, N.hand);
    if (hand && !env->ExceptionCheck()) {
      r.fMainHand = env->GetStaticFieldID(hand, N.fMainHand, map::handSig(r.profile));
      if (env->ExceptionCheck()) { r.fMainHand = nullptr; env->ExceptionClear(); }
      r.handCls = (jclass)env->NewGlobalRef(hand);
    }
    env->ExceptionClear();
    if (hand) env->DeleteLocalRef(hand);
    // AutoSprint: Entity.setSprinting(Z)V / isSprinting()Z on bum; jump on bvi.
    // All live on classes whose descriptors are primitive — no preload needed.
    {
      jclass e3 = gameClass(env, N.entity);
      if (e3 && !env->ExceptionCheck()) {
        r.mSetSprinting = env->GetMethodID(e3, N.mSetSprinting, "(Z)V");
        r.mIsSprinting = env->GetMethodID(e3, N.mIsSprinting, "()Z");
        env->ExceptionClear();
        env->DeleteLocalRef(e3);
      }
      jclass l2 = gameClass(env, N.living);
      if (l2 && !env->ExceptionCheck()) {
        r.mJumpFromGround = env->GetMethodID(l2, N.mJumpFromGround, "()V");
        env->ExceptionClear();
        env->DeleteLocalRef(l2);
      }
      // OnlyCrit + AimAssist live on the Entity base class (primitive sigs only).
      jclass e4 = gameClass(env, N.entity);
      if (e4 && !env->ExceptionCheck()) {
        r.fFallDistance = env->GetFieldID(e4, N.fFallDistance, "F");
        r.mOnGround = env->GetMethodID(e4, N.mOnGround, "()Z");
        r.mTouchWater = env->GetMethodID(e4, N.mTouchWater, "()Z");
        r.mSetYaw = env->GetMethodID(e4, N.mSetYaw, "(F)V");
        r.mSetPitch = env->GetMethodID(e4, N.mSetPitch, "(F)V");
        if (r.vX) r.mGetVel = env->GetMethodID(e4, N.mGetVel, (std::string("()") + map::vecSig(r.profile)).c_str());
        env->ExceptionClear();
        env->DeleteLocalRef(e4);
      }
    }
    // RenderSystem lives unobfuscated in the vanilla jar
    jclass rs = gameClass(env, "com/mojang/blaze3d/systems/RenderSystem");
    if (rs && !env->ExceptionCheck()) {
      r.mGetProj = env->GetStaticMethodID(rs, "getProjectionMatrix", "()Lorg/joml/Matrix4f;");
      if (!env->ExceptionCheck()) r.rsCls = (jclass)env->NewGlobalRef(rs);
      env->ExceptionClear();
    }
    if (rs) env->DeleteLocalRef(rs);
    if (r.mGetProj) {
      jclass joml = gameClass(env, "org/joml/Matrix4f");
      if (joml && !env->ExceptionCheck()) {
        // JOML declares fields column-by-column: m00,m10,m20,m30 | m01,m11,... | ...
        const char* names[] = {"m00","m10","m20","m30","m01","m11","m21","m31",
                               "m02","m12","m22","m32","m03","m13","m23","m33"};
        for (int i = 0; i < 16; i++) {
          r.pM[i] = env->GetFieldID(joml, names[i], "F");
          if (env->ExceptionCheck()) { r.pM[i] = nullptr; env->ExceptionClear(); }
        }
      }
      if (joml) env->DeleteLocalRef(joml);
      env->ExceptionClear();
    }
    env->ExceptionClear();
    return true;
  };

  Res r;
  bool armed = false;
  bool gate_seen = false;
  int waits = 0;
  uint64_t cycle = 0;
  for (;;) {
    // Fast tick: ESP/aim need fresh positions (~20 Hz). Verbose dump stays at ~5s.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cycle++;
    bool verbose = (cycle % 100 == 0);  // every ~5s: ent[...] dump lines
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!armed) {
      // Gate 1 (no side effects): is the MC client class already loaded BY THE
      // GAME itself? JVMTI GetLoadedClasses never initializes anything, unlike
      // FindClass (which would force <clinit> on OUR thread -> boot crash).
      if (!gate_seen) {
        map::Profile prof = detect_profile(vm);
        const char* gate = map::gateSig(prof);
        jvmtiEnv* jvmti = nullptr;
        bool seen = false;
        if (vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0) == JNI_OK && jvmti) {
          jint n = 0; jclass* arr = nullptr;
          if (jvmti->GetLoadedClasses(&n, &arr) == JVMTI_ERROR_NONE && arr) {
            for (int i = 0; i < n && !seen; i++) {
              char* sig = nullptr; char* gen = nullptr;
              if (jvmti->GetClassSignature(arr[i], &sig, &gen) == JVMTI_ERROR_NONE) {
                if (sig && std::strcmp(sig, gate) == 0) seen = true;
                if (sig) jvmti->Deallocate((unsigned char*)sig);
                if (gen) jvmti->Deallocate((unsigned char*)gen);
              }
            }
            jvmti->Deallocate((unsigned char*)arr);
          }
        }
        if (!seen) {
          if (++waits % 6 == 1) VLOG("jvm", "waiting for game bootstrap (gate class %s not loaded yet)...", gate);
          continue;
        }
        r.profile = prof;
        gate_seen = true;
        VLOG("jvm", "detected profile: %s (gate %s seen)",
             prof == map::Profile::Fabric ? "FABRIC (intermediary)" : "VANILLA (obfuscated)", gate);
        // Gate 2: gate class exists but its <clinit>/bootstrap may still run on
        // main. Grace period before our first FindClass (which would init it).
        VLOG("jvm", "game classes loading, grace 20s for bootstrap to settle...");
        std::this_thread::sleep_for(std::chrono::seconds(20));
        // Capture the game's own classloader (Knot under Fabric) so JNI sees
        // game classes. ClassLoader.loadClass links without initializing.
        init_game_loader(vm, env, gate);
      }
      if (!try_resolve(r)) { VLOG("jvm", "resolve retry (game still starting?)"); continue; }
      publish_res(r);
      armed = true;
      g_armed.store(true, std::memory_order_release);
      VLOG("jvm", "entity dump armed (every 5s, cap %d). Join a singleplayer world!", kMaxDump);
    }
    jclass mcCls = r.mcCls; jmethodID getInstance = r.getInstance;
    jfieldID fLevel = r.fLevel;
    jmethodID mEntities = r.mEntities;
    jmethodID mGetX = r.mGetX, mGetY = r.mGetY, mGetZ = r.mGetZ;
    jmethodID mAlive = r.mAlive, mName = r.mName, mGetString = r.mGetString;
    jclass playerCls = r.playerCls;
    jmethodID mHasNext = r.mHasNext, mNext = r.mNext, mIterator = r.mIterator, mClassName = r.mClassName;
    jobject mc = env->CallStaticObjectMethod(mcCls, getInstance);
    if (!exception_ok(env, "getInstance call") || !mc) { VLOG("jvm", "mc null (game starting?)"); continue; }
    jobject level = env->GetObjectField(mc, fLevel);
    if (!exception_ok(env, "level field")) { env->DeleteLocalRef(mc); continue; }
    if (!level) {
      static int menu_n = 0;
      if (++menu_n % 100 == 1) VLOG("jvm", "level=null (menu / not in world yet)");
      { // world closed -> kill stale boxes immediately
        Snapshot& s = snapshot();
        std::lock_guard<std::mutex> lock(s.m);
        s.ents.clear();
        s.cam.valid = false;
        s.in_world = false;
        s.tick++;
      }
      if (g_tb_tripped.exchange(false))
        VLOG("tb", "tripwire reset on world exit, triggerbot re-armed");
      env->DeleteLocalRef(mc);
      continue;
    }
    // Camera snapshot: producer keeps a recent value for fallback; the
    // render thread refreshes TRUTH via refresh_camera() at frame time.
    SnapCam scam;
    {
      std::lock_guard<std::mutex> _lg(snapshot().m);
      scam = snapshot().cam;
      scam.valid = false;
      scam.proj_ok = false;
    }
    // Producer-side late resolve too (once): camera ids may be missing while
    // the world is still loading; retry_camera_resolve is defined below but
    // operates on the same Res& so we can call it from here as well.
    if (!r.mGetMainCamera || !r.mGetPosition || !r.vX || !r.mGetFov)
      retry_camera_resolve(env, r);
    if (r.fGameRenderer && r.mGetMainCamera && r.mGetPosition && r.vX &&
        r.mGetXRot && r.mGetYRot && r.mGetFov) {
      env->ExceptionClear();
      jobject gr = env->GetObjectField(mc, r.fGameRenderer);
      if (!env->ExceptionCheck() && gr) {
        jobject cam = env->CallObjectMethod(gr, r.mGetMainCamera);
        if (!env->ExceptionCheck() && cam) {
          jobject pos = env->CallObjectMethod(cam, r.mGetPosition);
          if (!env->ExceptionCheck() && pos) {
            scam.x = env->GetDoubleField(pos, r.vX);
            scam.y = env->GetDoubleField(pos, r.vY);
            scam.z = env->GetDoubleField(pos, r.vZ);
            env->DeleteLocalRef(pos);
          }
          if (!env->ExceptionCheck()) {
            scam.pitch = env->CallFloatMethod(cam, r.mGetXRot);
            scam.yaw = env->CallFloatMethod(cam, r.mGetYRot);
          }
          if (!env->ExceptionCheck()) {
            // getFov lives on GameRenderer: (LCamera;FZ)F
            scam.fov = env->CallFloatMethod(gr, r.mGetFov, cam, 1.0f, JNI_FALSE);
            if (!env->ExceptionCheck() && scam.fov > 1.0f && scam.fov < 179.0f) scam.valid = true;
          }
          env->DeleteLocalRef(cam);
        }
        env->DeleteLocalRef(gr);
      }
      env->ExceptionClear();
    }
    // Producer no longer fetches the projection matrix (would violate
    // RenderSystem.assertOnRenderThread). The render thread does that.
    jobject iterable = env->CallObjectMethod(level, mEntities);
    if (!exception_ok(env, "entitiesForRendering") || !iterable) {
      env->DeleteLocalRef(level); env->DeleteLocalRef(mc); continue;
    }
    jobject it = env->CallObjectMethod(iterable, mIterator);
    if (!exception_ok(env, "iterator()") || !it) {
      env->DeleteLocalRef(iterable); env->DeleteLocalRef(level); env->DeleteLocalRef(mc); continue;
    }

    // Local player reference (to mark & skip self).
    jobject selfPlayer = env->GetObjectField(mc, r.fPlayer);
    env->ExceptionClear();

    int count = 0, shown = 0;
    std::vector<SnapEnt> stage;
    stage.reserve(128);
    for (int guard = 0; guard < 2000; guard++) {
      jboolean has = env->CallBooleanMethod(it, mHasNext);
      if (!exception_ok(env, "hasNext") || !has) break;
      jobject e = env->CallObjectMethod(it, mNext);
      if (!exception_ok(env, "next") || !e) break;
      count++;
      // Snapshot (up to 128): AABB + flags for the ESP renderer.
      if ((int)stage.size() < 128) {
        env->ExceptionClear();
        SnapEnt se;
        se.minX = env->CallDoubleMethod(e, mGetX) - 0.3;
        se.minY = env->CallDoubleMethod(e, mGetY);
        se.minZ = env->CallDoubleMethod(e, mGetZ) - 0.3;
        se.maxX = se.minX + 0.6; se.maxY = se.minY + 1.8; se.maxZ = se.minZ + 0.6;
        bool bok = !env->ExceptionCheck();
        if (bok && r.mGetBox && r.bMinX) {
          jobject b = env->CallObjectMethod(e, r.mGetBox);
          if (!env->ExceptionCheck() && b) {
            se.minX = env->GetDoubleField(b, r.bMinX); se.minY = env->GetDoubleField(b, r.bMinY);
            se.minZ = env->GetDoubleField(b, r.bMinZ); se.maxX = env->GetDoubleField(b, r.bMaxX);
            se.maxY = env->GetDoubleField(b, r.bMaxY); se.maxZ = env->GetDoubleField(b, r.bMaxZ);
            bok = !env->ExceptionCheck();
            env->DeleteLocalRef(b);
          }
        }
        if (bok) {
          se.alive = !mAlive || env->CallBooleanMethod(e, mAlive);
          bok = !env->ExceptionCheck();
        }
        if (bok) {
          se.player = playerCls && env->IsInstanceOf(e, playerCls);
          se.self = selfPlayer && env->IsSameObject(e, selfPlayer);
          se.living = r.livingCls && env->IsInstanceOf(e, r.livingCls);
          if (bok && r.mGetVel && r.vX) {  // velocity for aim prediction
            jobject vv = env->CallObjectMethod(e, r.mGetVel);
            if (!env->ExceptionCheck() && vv) {
              se.vx = env->GetDoubleField(vv, r.vX);
              se.vy = env->GetDoubleField(vv, r.vY);
              se.vz = env->GetDoubleField(vv, r.vZ);
              env->DeleteLocalRef(vv);
            }
            if (env->ExceptionCheck()) { se.vx = se.vy = se.vz = 0; env->ExceptionClear(); }
          }
          if (verbose && r.mName && r.mGetString) {  // names change rarely: 5s cadence
            jobject comp = env->CallObjectMethod(e, mName);
            if (!env->ExceptionCheck() && comp) {
              jstring js = (jstring)env->CallObjectMethod(comp, r.mGetString);
              if (!env->ExceptionCheck() && js) {
                const char* c = env->GetStringUTFChars(js, nullptr);
                if (c) { std::strncpy(se.name, c, sizeof(se.name) - 1); }
                env->ReleaseStringUTFChars(js, c);
                env->DeleteLocalRef(js);
              }
              env->DeleteLocalRef(comp);
            }
          }
          env->ExceptionClear();
          stage.push_back(se);
        } else env->ExceptionClear();
      }
      if (verbose && shown < kMaxDump) {
        jdouble x = env->CallDoubleMethod(e, mGetX);
        jdouble y = env->CallDoubleMethod(e, mGetY);
        jdouble z = env->CallDoubleMethod(e, mGetZ);
        jboolean alive = mAlive ? env->CallBooleanMethod(e, mAlive) : JNI_TRUE;
        bool ok = exception_ok(env, "pos/alive");
        const char* cname = "?";
        jstring jcn = nullptr;
        if (ok && mClassName) {
          jclass ec = env->GetObjectClass(e);
          jstring n = (jstring)env->CallObjectMethod(ec, mClassName);
          if (exception_ok(env, "className") && n) {
            jcn = n;
            cname = env->GetStringUTFChars(jcn, nullptr);
          }
          env->DeleteLocalRef(ec);
        }
        bool isPlayer = playerCls && env->IsInstanceOf(e, playerCls);
        // name (may be null for some entities)
        std::string nm = "-";
        if (ok && mName) {
          jobject comp = env->CallObjectMethod(e, mName);
          if (exception_ok(env, "getName") && comp && mGetString) {
            jstring js = (jstring)env->CallObjectMethod(comp, mGetString);
            if (exception_ok(env, "getString") && js) {
              const char* c = env->GetStringUTFChars(js, nullptr);
              if (c) nm = c;
              env->ReleaseStringUTFChars(js, c);
              env->DeleteLocalRef(js);
            }
            env->DeleteLocalRef(comp);
          } else if (comp) env->DeleteLocalRef(comp);
        }
        if (ok) {
          VLOG("jvm", "ent[%d] cls=%s player=%d alive=%d xyz=%.1f %.1f %.1f name=%s",
               shown, cname ? cname : "?", (int)isPlayer, (int)alive, x, y, z, nm.c_str());
          shown++;
        }
        if (jcn) { env->ReleaseStringUTFChars(jcn, cname); env->DeleteLocalRef(jcn); }
      }
      env->DeleteLocalRef(e);
    }
    if (verbose) VLOG("jvm", "world entities: %d (shown %d)", count, shown);
    Snapshot& s = snapshot();
    bool do_log = false;
    {
      std::lock_guard<std::mutex> lock(s.m);
      s.ents.swap(stage);
      s.cam = scam;
      s.in_world = true;  // level != null on this tick
      s.tick++;
      s.snap_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      do_log = scam.valid && (s.tick < 30 || cycle % 600 == 0);
    }
    if (do_log)
      VLOG("jvm", "cam snapshot ok: pos=%.1f %.1f %.1f yaw=%.1f pitch=%.1f fov=%.1f",
           scam.x, scam.y, scam.z, scam.yaw, scam.pitch, scam.fov);
    if (selfPlayer) env->DeleteLocalRef(selfPlayer);
    env->DeleteLocalRef(it);
    env->DeleteLocalRef(iterable);
    env->DeleteLocalRef(level);
    env->DeleteLocalRef(mc);
  }
}

// ---- render-thread camera + projection fetch (called from the swap hook) ----
// Resolution results are published by the producer loop (g_res/g_ready);
// the render thread is a bare pthread, so we ATTACH it on first use.
// Camera/AABB ids resolved LATE (they can fail during the 20s grace: e.g.
// fks/class_4184 is not resolvable from GetMethodID's descriptor until the
// game actually constructs the camera). Retry lazily on the render thread.
static void retry_camera_resolve(JNIEnv* env, Res& r) {
  static std::atomic<int> tries{0};
  int t = ++tries;
  const map::Names& N = map::names(r.profile);
  env->ExceptionClear();
  if (!r.mGetMainCamera) {
    // Preload every class mentioned in the descriptor: GetMethodID throws
    // NoClassDefFoundError when a descriptor class is not yet loadable from
    // THIS class loader context (flk->glq->fks chain). FindClass('fks') first
    // materializes it, then the method lookup succeeds.
    jclass camc = gameClass(env, N.camera);
    if (camc) env->DeleteLocalRef(camc);
    env->ExceptionClear();
    jclass gr = gameClass(env, N.gameRenderer);
    if (!gr || env->ExceptionCheck()) {
      if (t % 20 == 1) VLOG("jvm", "retry: FindClass(%s) failed", N.gameRenderer);
      env->ExceptionClear();
    } else {
      r.mGetMainCamera = env->GetMethodID(gr, N.mGetMainCamera, (std::string("()") + map::camSig(r.profile)).c_str());
      if (env->ExceptionCheck()) {
        if (t % 20 == 1) {
          VLOG("jvm", "retry: GetMethodID(%s) threw (preloaded %s), exception:", N.mGetMainCamera, N.camera);
          env->ExceptionDescribe();  // one-shot diagnostic to /tmp/voltage.log
        }
        env->ExceptionClear();
      }
    }
    if (gr) env->DeleteLocalRef(gr);
  }
  if (!r.mGetPosition || !r.mGetXRot || !r.mGetYRot) {
    jclass vecp = gameClass(env, N.vec3);  // preload descriptor class
    if (vecp) env->DeleteLocalRef(vecp);
    env->ExceptionClear();
    jclass cam = gameClass(env, N.camera);
    if (cam && !env->ExceptionCheck()) {
      if (!r.mGetPosition) r.mGetPosition = env->GetMethodID(cam, N.mGetPosition, (std::string("()") + map::vecSig(r.profile)).c_str());
      if (!r.mGetXRot) r.mGetXRot = env->GetMethodID(cam, N.mGetXRot, "()F");
      if (!r.mGetYRot) r.mGetYRot = env->GetMethodID(cam, N.mGetYRot, "()F");
      env->ExceptionClear();
    }
    if (cam) env->DeleteLocalRef(cam);
  }
  if (!r.vX || !r.vY || !r.vZ) {
    jclass vec = gameClass(env, N.vec3);
    if (vec && !env->ExceptionCheck()) {
      if (!r.vX) r.vX = env->GetFieldID(vec, N.vX, "D");
      if (!r.vY) r.vY = env->GetFieldID(vec, N.vY, "D");
      if (!r.vZ) r.vZ = env->GetFieldID(vec, N.vZ, "D");
      env->ExceptionClear();
    }
    if (vec) env->DeleteLocalRef(vec);
  }
  if (!r.mGetFov) {
    jclass gr2 = gameClass(env, N.gameRenderer);
    if (gr2 && !env->ExceptionCheck()) {
      std::string fovSig = std::string("(") + map::camSig(r.profile) + "FZ)F";
      r.mGetFov = env->GetMethodID(gr2, N.mGetFov, fovSig.c_str());
      env->ExceptionClear();
    }
    if (gr2) env->DeleteLocalRef(gr2);
  }
  if (!r.mGetBox || !r.bMinX) {
    jclass box = gameClass(env, N.aabb);
    if (box && !env->ExceptionCheck()) {
      if (!r.bMinX) r.bMinX = env->GetFieldID(box, N.bMinX, "D");
      if (!r.bMinY) r.bMinY = env->GetFieldID(box, N.bMinY, "D");
      if (!r.bMinZ) r.bMinZ = env->GetFieldID(box, N.bMinZ, "D");
      if (!r.bMaxX) r.bMaxX = env->GetFieldID(box, N.bMaxX, "D");
      if (!r.bMaxY) r.bMaxY = env->GetFieldID(box, N.bMaxY, "D");
      if (!r.bMaxZ) r.bMaxZ = env->GetFieldID(box, N.bMaxZ, "D");
      env->ExceptionClear();
    }
    if (box) env->DeleteLocalRef(box);
    jclass ent2 = gameClass(env, N.entity);
    if (ent2 && !env->ExceptionCheck()) {
      r.mGetBox = env->GetMethodID(ent2, N.mGetBox, (std::string("()") + map::aabbSig(r.profile)).c_str());
      env->ExceptionClear();
    }
    if (ent2) env->DeleteLocalRef(ent2);
  }
  // TriggerBot ids can hit the same late-load wall — retry them here too.
  if (!r.fHitResult || !r.fGameMode) {
    r.fHitResult = env->GetFieldID(r.mcCls, N.fHitResult, map::hitSig(r.profile));
    r.fGameMode = env->GetFieldID(r.mcCls, N.fGameMode, map::gmSig(r.profile));
    env->ExceptionClear();
  }
  if (!r.mHitEntity) {
    jclass ehit = gameClass(env, N.entityHit);
    if (ehit && !env->ExceptionCheck()) {
      r.mHitEntity = env->GetMethodID(ehit, N.mHitEntity, (std::string("()") + map::entSig(r.profile)).c_str());
      if (!env->ExceptionCheck() && r.mHitEntity && !r.entityHitCls)
        r.entityHitCls = (jclass)env->NewGlobalRef(ehit);
      env->ExceptionClear();
    }
    if (ehit) env->DeleteLocalRef(ehit);
  }
  if (!r.mAttack) {
    jclass gm = gameClass(env, N.gameMode);
    if (gm && !env->ExceptionCheck()) {
      std::string atkSig = std::string("(") + map::plSig(r.profile) + map::entSig(r.profile) + ")V";
      r.mAttack = env->GetMethodID(gm, N.mAttack, atkSig.c_str());
      env->ExceptionClear();
    }
    if (gm) env->DeleteLocalRef(gm);
  }
  if (!r.mSwing) {
    jclass living = gameClass(env, N.living);
    if (living && !env->ExceptionCheck()) {
      std::string swingSig = std::string("(") + map::handSig(r.profile) + ")V";
      r.mSwing = env->GetMethodID(living, N.mSwing, swingSig.c_str());
      env->ExceptionClear();
    }
    if (living) env->DeleteLocalRef(living);
  }
  if (!r.mCooldown) {
    jclass plc = gameClass(env, N.playerBase);
    if (plc && !env->ExceptionCheck()) {
      r.mCooldown = env->GetMethodID(plc, N.mCooldown, "(F)F");
      env->ExceptionClear();
    }
    if (plc) env->DeleteLocalRef(plc);
  }
  if (!r.fMainHand || !r.handCls) {
    jclass hand = gameClass(env, N.hand);
    if (hand && !env->ExceptionCheck()) {
      if (!r.fMainHand) r.fMainHand = env->GetStaticFieldID(hand, N.fMainHand, map::handSig(r.profile));
      if (env->ExceptionCheck()) { r.fMainHand = nullptr; env->ExceptionClear(); }
      if (!r.handCls) r.handCls = (jclass)env->NewGlobalRef(hand);
    }
    env->ExceptionClear();
    if (hand) env->DeleteLocalRef(hand);
  }
  if (!r.mSetSprinting || !r.mIsSprinting || !r.mJumpFromGround) {
    jclass e3 = gameClass(env, N.entity);
    if (e3 && !env->ExceptionCheck()) {
      if (!r.mSetSprinting) r.mSetSprinting = env->GetMethodID(e3, N.mSetSprinting, "(Z)V");
      if (!r.mIsSprinting) r.mIsSprinting = env->GetMethodID(e3, N.mIsSprinting, "()Z");
      env->ExceptionClear();
      env->DeleteLocalRef(e3);
    }
    jclass l3 = gameClass(env, N.living);
    if (l3 && !env->ExceptionCheck()) {
      if (!r.mJumpFromGround) r.mJumpFromGround = env->GetMethodID(l3, N.mJumpFromGround, "()V");
      env->ExceptionClear();
      env->DeleteLocalRef(l3);
    }
  }
  if (!r.fFallDistance || !r.mOnGround || !r.mTouchWater || !r.mSetYaw || !r.mSetPitch) {
    jclass e4 = gameClass(env, N.entity);
    if (e4 && !env->ExceptionCheck()) {
      if (!r.fFallDistance) r.fFallDistance = env->GetFieldID(e4, N.fFallDistance, "F");
      if (!r.mOnGround) r.mOnGround = env->GetMethodID(e4, N.mOnGround, "()Z");
      if (!r.mTouchWater) r.mTouchWater = env->GetMethodID(e4, N.mTouchWater, "()Z");
      if (!r.mSetYaw) r.mSetYaw = env->GetMethodID(e4, N.mSetYaw, "(F)V");
      if (!r.mSetPitch) r.mSetPitch = env->GetMethodID(e4, N.mSetPitch, "(F)V");
      if (!r.mGetVel && r.vX) r.mGetVel = env->GetMethodID(e4, N.mGetVel, (std::string("()") + map::vecSig(r.profile)).c_str());
      env->ExceptionClear();
      env->DeleteLocalRef(e4);
    }
  }
  env->ExceptionClear();
}

bool refresh_camera(SnapCam& out, int vp_w, int vp_h) {
  static std::atomic<bool> logged_fail{false};
  if (!g_ready.load(std::memory_order_acquire)) return false;
  Res& r = g_res;

  JavaVM* vm = first_vm();
  if (!vm) return false;
  JNIEnv* env = env_or_attach(vm);
  if (!env) {
    if (!logged_fail.exchange(true)) VLOG("jvm", "refresh_camera: cannot attach render thread to JVM");
    return false;
  }

  env->ExceptionClear();
  jobject mc = env->CallStaticObjectMethod(r.mcCls, r.getInstance);
  if (env->ExceptionCheck() || !mc) { env->ExceptionClear(); return false; }
  // Late-resolve camera ids if the grace-time resolve missed them.
  static std::atomic<bool> retried{false};
  if (!r.mGetMainCamera || !r.mGetPosition || !r.vX || !r.mGetFov) {
    retry_camera_resolve(env, r);
    if (!retried.exchange(true) && r.mGetMainCamera && r.mGetPosition && r.vX && r.mGetFov)
      VLOG("jvm", "camera ids late-resolved OK on render thread");
  }
  SnapCam scam;
  bool ok = false;
  if (r.fGameRenderer && r.mGetMainCamera && r.mGetPosition && r.vX && r.mGetXRot &&
      r.mGetYRot && r.mGetFov) {
    jobject gr = env->GetObjectField(mc, r.fGameRenderer);
    if (!env->ExceptionCheck() && gr) {
      jobject cam = env->CallObjectMethod(gr, r.mGetMainCamera);
      if (!env->ExceptionCheck() && cam) {
        jobject pos = env->CallObjectMethod(cam, r.mGetPosition);
        if (!env->ExceptionCheck() && pos) {
          scam.x = env->GetDoubleField(pos, r.vX);
          scam.y = env->GetDoubleField(pos, r.vY);
          scam.z = env->GetDoubleField(pos, r.vZ);
          env->DeleteLocalRef(pos);
        }
        if (!env->ExceptionCheck()) {
          scam.pitch = env->CallFloatMethod(cam, r.mGetXRot);
          scam.yaw = env->CallFloatMethod(cam, r.mGetYRot);
          // REAL fov with effects, same args as the game's own world projection
          // (GameRenderer builds perspective(getFov(cam,tick,true), aspect, ...)).
          // NOTE: RenderSystem.getProjectionMatrix() is unusable here: at swap
          // time it already holds the GUI ortho matrix, not the world one.
          scam.fov = env->CallFloatMethod(gr, r.mGetFov, cam, 1.0f, JNI_TRUE);
          if (!env->ExceptionCheck() && scam.fov > 1.0f && scam.fov < 179.0f && vp_w > 0 && vp_h > 0) {
            // perspective(fov_deg, aspect, near=0.05, far=10000), column-major
            float f = 1.0f / std::tan(scam.fov * (float)M_PI / 360.0f);
            float aspect = (float)vp_w / (float)vp_h;
            for (int i = 0; i < 16; i++) scam.proj[i] = 0.0f;
            scam.proj[0] = f / aspect;
            scam.proj[5] = f;
            scam.proj[10] = (10000.0f + 0.05f) / (0.05f - 10000.0f);
            scam.proj[11] = -1.0f;
            scam.proj[14] = (2.0f * 10000.0f * 0.05f) / (0.05f - 10000.0f);
            scam.proj_ok = true;
            ok = true;
          }
        }
        env->DeleteLocalRef(cam);
      }
      env->DeleteLocalRef(gr);
    }
  }
  env->ExceptionClear();
  env->DeleteLocalRef(mc);
  scam.valid = ok;
  // Throttled in-world diagnostics: what does the render thread actually see?
  static std::atomic<uint64_t> diag_n{0};
  static std::atomic<uint64_t> fail_n{0};
  if (ok) {
    if (++diag_n % 120 == 1)
      VLOG("jvm", "rt cam diag: pos=%.1f %.1f %.1f yaw=%.1f pitch=%.1f fov=%.1f",
           scam.x, scam.y, scam.z, scam.yaw, scam.pitch, scam.fov);
  } else if (++fail_n % 600 == 1) {
    VLOG("jvm", "rt cam FAIL: gr=%d cam=%d pos=%d rot=%d fov=%d",
         r.fGameRenderer ? 1 : 0, r.mGetMainCamera ? 1 : 0, r.mGetPosition ? 1 : 0,
         (r.mGetXRot && r.mGetYRot) ? 1 : 0, r.mGetFov ? 1 : 0);
  }
  if (ok) out = scam;
  return ok;
}

// ---- AutoSprint + WTap-lite sprint reset (render thread, per tick call) ----
// When enabled: keep player sprinting while moving forward. On an attack
// (flagged by triggerbot_tick / autosprint_note_hit): release sprint for a
// few ticks (legit server-side knockback boost), optionally hop, then resume.
static std::atomic<bool> g_hit_flag{false};
static int g_resume_timer = 0;

void autosprint_note_hit() { g_hit_flag.store(true, std::memory_order_release); }

void autosprint_tick() {
  if (!cfg::get().autosprint_enabled) return;
  if (!g_ready.load(std::memory_order_acquire)) return;
  Res& r = g_res;
  if (!r.mSetSprinting || !r.mIsSprinting) return;
  JavaVM* vm = first_vm();
  if (!vm) return;
  JNIEnv* env = env_or_attach(vm);
  if (!env) return;
  env->ExceptionClear();
  jobject mc = env->CallStaticObjectMethod(r.mcCls, r.getInstance);
  if (env->ExceptionCheck() || !mc) { env->ExceptionClear(); return; }
  jobject player = env->GetObjectField(mc, r.fPlayer);
  if (!player) { env->ExceptionClear(); env->DeleteLocalRef(mc); return; }

  bool hit = g_hit_flag.exchange(false);
  // OnlyCrits compatibility: a forced sprint denies vanilla crits forever
  // (and WTap only releases ON hit -> deadlock). While airborne with OnlyCrits
  // armed, drop sprint instead of holding it so falling hits can crit.
  cfg::Config& ac = cfg::get();
  if (ac.triggerbot_enabled && ac.triggerbot_only_crits && r.mOnGround) {
    env->ExceptionClear();
    jboolean ground = env->CallBooleanMethod(player, r.mOnGround);
    if (!env->ExceptionCheck() && ground == JNI_FALSE) {
      jboolean sprinting = r.mIsSprinting ? env->CallBooleanMethod(player, r.mIsSprinting) : JNI_FALSE;
      if (!env->ExceptionCheck() && sprinting) {
        env->CallVoidMethod(player, r.mSetSprinting, JNI_FALSE);
        static std::atomic<uint64_t> unspr{0};
        if (++unspr % 300 == 1) VLOG("sprint", "released for OnlyCrits (airborne)");
      }
      env->ExceptionClear();
      env->DeleteLocalRef(player);
      env->DeleteLocalRef(mc);
      return;
    }
    env->ExceptionClear();
  }
  if (hit && cfg::get().autosprint_wtap) {
    // Legit reset: drop sprint now, resume after N ticks.
    env->CallVoidMethod(player, r.mSetSprinting, JNI_FALSE);
    env->ExceptionClear();
    g_resume_timer = cfg::get().autosprint_resume_ticks > 0 ? cfg::get().autosprint_resume_ticks : 1;
    if (cfg::get().autosprint_jump && r.mJumpFromGround) {
      env->CallVoidMethod(player, r.mJumpFromGround);
      env->ExceptionClear();
    }
  }
  if (g_resume_timer > 0) {
    g_resume_timer--;
  } else {
    jboolean sprinting = env->CallBooleanMethod(player, r.mIsSprinting);
    if (!env->ExceptionCheck() && !sprinting) {
      env->CallVoidMethod(player, r.mSetSprinting, JNI_TRUE);
      env->ExceptionClear();
    } else env->ExceptionClear();
  }
  env->DeleteLocalRef(player);
  env->DeleteLocalRef(mc);
}

// ---- TriggerBot: called on the render thread from the swap hook ----
// Mirrors pasta TriggerBot.java: mc.hitResult instanceof EntityHitResult ->
// target = getEntity(); gate on attack cooldown >= threshold, distance and
// config toggles; attack via gameMode.attack(player, entity) + player.swing().
void triggerbot_tick() {
  static std::atomic<bool> logged_unarmed{false};
  if (!cfg::get().triggerbot_enabled) return;
  // Tripwire: if the attack chain ever throws, stay off until world rejoin.
  // A single bad JNI receiver segfaults the whole JVM (hs_err) — never retry blindly.
  if (g_tb_tripped.load(std::memory_order_acquire)) return;
  if (!g_ready.load(std::memory_order_acquire)) {
    if (!logged_unarmed.exchange(true)) VLOG("tb", "not armed yet (still resolving JNI ids)");
    return;
  }
  logged_unarmed.store(false);
  Res& r = g_res;
  if (!r.fHitResult || !r.fGameMode || !r.mHitEntity || !r.entityHitCls || !r.mAttack || !r.mSwing || !r.mCooldown || !r.fMainHand) {
    static std::atomic<bool> logged_missing{false};
    if (!logged_missing.exchange(true)) VLOG("tb", "triggerbot JNI ids incomplete, disabled");
    return;
  }
  JavaVM* vm = first_vm();
  if (!vm) return;
  JNIEnv* env = env_or_attach(vm);
  if (!env) return;

  cfg::Config& c = cfg::get();
  env->ExceptionClear();
  jobject mc = env->CallStaticObjectMethod(r.mcCls, r.getInstance);
  if (env->ExceptionCheck() || !mc) { env->ExceptionClear(); return; }
  jobject level = env->GetObjectField(mc, r.fLevel);
  jobject player = env->GetObjectField(mc, r.fPlayer);
  jobject gameMode = env->GetObjectField(mc, r.fGameMode);
  if (!level || !player || !gameMode) {
    env->ExceptionClear();
    if (level) env->DeleteLocalRef(level);
    if (player) env->DeleteLocalRef(player);
    if (gameMode) env->DeleteLocalRef(gameMode);
    env->DeleteLocalRef(mc);
    return;
  }
  jobject hit = env->GetObjectField(mc, r.fHitResult);
  jobject target = nullptr;
  if (hit) {
    // NEVER call EntityHitResult.getEntity() on a non-entity HitResult
    // (BlockHitResult/Miss): cross-class methodID dispatch corrupts the
    // receiver vtable slot and segfaults the render thread (hs_err observed).
    if (r.mHitEntity && r.entityHitCls && env->IsInstanceOf(hit, r.entityHitCls)) {
      target = env->CallObjectMethod(hit, r.mHitEntity);
      if (env->ExceptionCheck()) { target = nullptr; env->ExceptionClear(); }
    } else env->ExceptionClear();
    env->DeleteLocalRef(hit);
  }
  bool attacked = false;
  if (target) {
    // self-guard: never attack our own player object
    if (env->IsSameObject(target, player)) { /* skip */ }
    // entity-guard: getEntity() must yield a real Entity before ANY method
    // call on it (stale/wrong receivers segfault the JVM, hs_err observed).
    else if (!r.entityCls || !env->IsInstanceOf(target, r.entityCls)) {
      VLOG("tb", "target failed Entity check, tripping");
      g_tb_tripped.store(true, std::memory_order_release);
      env->ExceptionClear();
    }
    else {
      jfloat cd = env->CallFloatMethod(player, r.mCooldown, 0.5f);
      if (!env->ExceptionCheck() && cd >= c.triggerbot_cooldown) {
        // OnlyCrit (vanilla rule): falling + airborne + dry + no sprint.
        // Skips climbing/lava/vehicle/blindness edge cases (rare, reference has them).
        // NOTE: Entity.fallDistance reads 0.00 on the client even mid-air
        // (server-authoritative in 1.21), so falling is measured directly as
        // vertical velocity vy=(y-prevY)/dt < -0.3 instead of falld>0.
        if (c.triggerbot_only_crits) {
          bool crit_ok = false;
          static std::atomic<uint64_t> crit_n{0};
          static double prev_py = 0;
          static uint64_t prev_ms = 0;
          if (r.mOnGround && r.mTouchWater && r.mIsSprinting && r.mGetY) {
            env->ExceptionClear();
            jdouble py = env->CallDoubleMethod(player, r.mGetY);
            jboolean ground = env->CallBooleanMethod(player, r.mOnGround);
            jboolean wet = env->CallBooleanMethod(player, r.mTouchWater);
            jboolean sprint = env->CallBooleanMethod(player, r.mIsSprinting);
            if (!env->ExceptionCheck()) {
              uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();
              double vy = 0.0;
              if (prev_ms && now_ms > prev_ms) {
                double dt = (double)(now_ms - prev_ms) / 1000.0;
                if (dt > 0.001 && dt < 0.5) vy = (py - prev_py) / dt;
              }
              prev_py = py; prev_ms = now_ms;
              crit_ok = (vy < -0.3) && (ground == JNI_FALSE) && (wet == JNI_FALSE) && (sprint == JNI_FALSE);
              if (++crit_n % 120 == 1)
                VLOG("tb", "crit sample: vy=%.2f ground=%d wet=%d sprint=%d -> %s",
                     vy, (int)ground, (int)wet, (int)sprint, crit_ok ? "HIT" : "hold");
            } else env->ExceptionClear();
          } else if (++crit_n % 120 == 1) {
            VLOG("tb", "crit ids missing: ground=%d wet=%d sprint=%d getY=%d",
                 r.mOnGround ? 1 : 0, r.mTouchWater ? 1 : 0,
                 r.mIsSprinting ? 1 : 0, r.mGetY ? 1 : 0);
          }
          if (!crit_ok) {
            env->DeleteLocalRef(target);
            env->DeleteLocalRef(player);
            env->DeleteLocalRef(gameMode);
            env->DeleteLocalRef(level);
            env->DeleteLocalRef(mc);
            return;
          }
        }
        // distance gate (entity pos via Entity.getX/getY/getZ)
        jdouble ex = env->CallDoubleMethod(target, r.mGetX);
        jdouble ey = env->CallDoubleMethod(target, r.mGetY);
        jdouble ez = env->CallDoubleMethod(target, r.mGetZ);
        if (env->ExceptionCheck()) {
          VLOG("tb", "target pos read threw, tripping");
          g_tb_tripped.store(true, std::memory_order_release);
          env->ExceptionClear();
        }
        if (!env->ExceptionCheck()) {
          jdouble px = env->CallDoubleMethod(player, r.mGetX);
          jdouble py = env->CallDoubleMethod(player, r.mGetY);
          jdouble pz = env->CallDoubleMethod(player, r.mGetZ);
          if (!env->ExceptionCheck()) {
            double dx = ex - px, dy = ey - py, dz = ez - pz;
            double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist <= c.triggerbot_range) {
              env->CallVoidMethod(gameMode, r.mAttack, player, target);
              if (env->ExceptionCheck()) {
                VLOG("tb", "attack() threw, tripping until world rejoin");
                g_tb_tripped.store(true, std::memory_order_release);
                env->ExceptionClear();
              }
              if (!env->ExceptionCheck()) {
                jobject mainHand = env->GetStaticObjectField(r.handCls, r.fMainHand);
                if (!env->ExceptionCheck() && mainHand) {
                  env->CallVoidMethod(player, r.mSwing, mainHand);
                  env->ExceptionClear();
                } else env->ExceptionClear();
                attacked = true;
                autosprint_note_hit();  // sprint reset on every triggerbot hit
              } else env->ExceptionClear();
            }
          } else env->ExceptionClear();
        } else env->ExceptionClear();
      } else env->ExceptionClear();
    }
    env->DeleteLocalRef(target);
  } else env->ExceptionClear();
  if (attacked) {
    static std::atomic<uint64_t> hits{0};
    VLOG("tb", "attack #%llu cd>=%.2f range<=%.1f",
         (unsigned long long)++hits, (double)c.triggerbot_cooldown, (double)c.triggerbot_range);
  }
  env->DeleteLocalRef(player);
  env->DeleteLocalRef(gameMode);
  env->DeleteLocalRef(level);
  env->DeleteLocalRef(mc);
}

// ---- AimAssist: silent rotation pull on the render thread ------------------
// Mirrors pasta AimAssist (crosshair priority): picks the living target with
// the smallest angular offset inside the engage cone, lerps yaw/pitch toward
// its chest point. ONLY touches rotation — movement keys stay 100% free.
namespace {
inline double wrap_deg(double a) {
  while (a > 180.0) a -= 360.0;
  while (a < -180.0) a += 360.0;
  return a;
}
} // namespace

void aimassist_tick() {
  cfg::Config& c = cfg::get();
  if (!c.aimassist_enabled) return;
  if (!g_ready.load(std::memory_order_acquire)) return;
  if (g_tb_tripped.load(std::memory_order_acquire)) return;  // shared tripwire
  Res& r = g_res;
  if (!r.mSetYaw || !r.mSetPitch) {
    static std::atomic<bool> logged_noaim{false};
    if (!logged_noaim.exchange(true)) VLOG("aim", "rotation ids missing, aim disabled");
    return;
  }
  // Frame dt (seconds) for framerate-independent smoothing.
  static auto last_t = std::chrono::steady_clock::now();
  auto now_t = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now_t - last_t).count();
  last_t = now_t;
  if (dt <= 0.0) dt = 1.0 / 60.0;
  if (dt > 0.25) dt = 0.25;  // tab-out / hitch guard
  // Snapshot ents (20 Hz) + fresh camera truth (this thread) + snap age.
  Snapshot& s = snapshot();
  std::vector<SnapEnt> ents;
  uint64_t snap_ms = 0;
  {
    std::lock_guard<std::mutex> lock(s.m);
    if (s.tick == 0 || !s.in_world) return;
    ents = s.ents;
    snap_ms = s.snap_ms;
  }
  SnapCam cam;
  {
    std::lock_guard<std::mutex> lock(s.m);
    cam = s.cam;
  }
  if (!cam.valid) return;
  // Extrapolate target positions by velocity * snapshot age (prediction lite).
  // Without this, moving targets visibly step every producer tick (jerk).
  uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      now_t.time_since_epoch()).count();
  double age = snap_ms ? (double)(now_ms - snap_ms) / 1000.0 : 0.0;
  if (age < 0.0) age = 0.0;
  if (age > 0.30) age = 0.30;

  const double kDeg = 180.0 / 3.14159265358979323846;
  double best_ang = 1e30;
  double best_tyaw = 0, best_tpitch = 0, best_dist = 0;
  const char* best_name = "?";
  for (auto& e : ents) {
    if (!e.alive || e.self || !e.living) continue;
    if (e.player && !c.aimassist_players) continue;
    if (!e.player && !c.aimassist_mobs) continue;
    double ex = (e.minX + e.maxX) * 0.5 + e.vx * age;
    double ey = e.minY + (e.maxY - e.minY) * 0.9 + e.vy * age;  // chest/head point (ref)
    double ez = (e.minZ + e.maxZ) * 0.5 + e.vz * age;
    double dx = ex - cam.x, dy = ey - cam.y, dz = ez - cam.z;
    double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist > (double)c.aimassist_range || dist < 0.05) continue;
    double tyaw = std::atan2(-dx, dz) * kDeg;
    double tpitch = std::atan2(-dy, std::sqrt(dx * dx + dz * dz)) * kDeg;
    double dyaw = std::fabs(wrap_deg(tyaw - (double)cam.yaw));
    double dpitch = std::fabs(tpitch - (double)cam.pitch);
    double ang = std::sqrt(dyaw * dyaw + dpitch * dpitch);
    if (ang > (double)c.aimassist_fov * 0.5) continue;
    if (ang < best_ang) {
      best_ang = ang;
      best_dist = dist;
      best_tyaw = tyaw; best_tpitch = tpitch;
      best_name = e.name;
    }
  }
  if (best_ang > 1e29) return;

  // Smoothing (ref formula), scaled to real frame dt so 20 fps feels like 60:
  // per-frame factor k applied over dt seconds: k_frame = 1-(1-k)^(dt*60).
  // base=(speed/10)*0.3, divider=1+(smooth/10)*2, far (>15 deg) pulls 1.8x.
  double base = ((double)c.aimassist_speed / 10.0) * 0.3;
  if (base < 0.01) base = 0.01;
  if (base > 1.0) base = 1.0;
  double divider = 1.0 + ((double)c.aimassist_smooth / 10.0) * 2.0;
  if (divider < 1.0) divider = 1.0;
  double dyw = wrap_deg(best_tyaw - (double)cam.yaw);
  double dpi = best_tpitch - (double)cam.pitch;
  double fyw = std::fabs(dyw) > 15.0 ? base * 1.8 : std::max(base * (std::fabs(dyw) / 15.0), base * 0.1);
  double fpi = std::fabs(dpi) > 15.0 ? base * 1.8 : std::max(base * (std::fabs(dpi) / 15.0), base * 0.1);
  if (fyw > 1.0) fyw = 1.0;
  if (fpi > 1.0) fpi = 1.0;
  double kyw = 1.0 - std::pow(1.0 - fyw / divider, dt * 60.0);
  double kpi = 1.0 - std::pow(1.0 - fpi / divider, dt * 60.0);
  double newYaw = (double)cam.yaw + dyw * kyw;
  double newPitch = (double)cam.pitch + dpi * kpi;
  // Built-in eye-pleasing guard (always on, no toggle): micro-deadzone kills
  // sub-pixel jitter around the lock point; a slew limiter caps per-frame
  // rotation so no single frame ever snaps, whatever speed/smooth/fps are.
  {
    double fdyw = wrap_deg(newYaw - (double)cam.yaw);
    double fdpi = newPitch - (double)cam.pitch;
    if (std::fabs(fdyw) < 0.05 && std::fabs(fdpi) < 0.05) {
      return;  // already on target: touch nothing (no JNI done yet, nothing to free)
    }
    constexpr double kMaxDegPerSec = 540.0;
    double cap = kMaxDegPerSec * dt;
    if (std::fabs(fdyw) > cap) newYaw = (double)cam.yaw + (fdyw > 0 ? cap : -cap);
    if (std::fabs(fdpi) > cap) newPitch = (double)cam.pitch + (fdpi > 0 ? cap : -cap);
  }
  if (newPitch > 90.0) newPitch = 90.0;
  if (newPitch < -90.0) newPitch = -90.0;

  JavaVM* vm = first_vm();
  if (!vm) return;
  JNIEnv* env = env_or_attach(vm);
  if (!env) return;
  env->ExceptionClear();
  jobject mc = env->CallStaticObjectMethod(r.mcCls, r.getInstance);
  if (env->ExceptionCheck() || !mc) { env->ExceptionClear(); return; }
  jobject player = env->GetObjectField(mc, r.fPlayer);
  if (env->ExceptionCheck() || !player) {
    env->ExceptionClear();
    env->DeleteLocalRef(mc);
    return;
  }
  // player is LivingEntity: setYRot/setPitch resolved on the Entity base.
  env->CallVoidMethod(player, r.mSetYaw, (jfloat)newYaw);
  if (!env->ExceptionCheck()) env->CallVoidMethod(player, r.mSetPitch, (jfloat)newPitch);
  if (env->ExceptionCheck()) {
    VLOG("aim", "setYaw/setPitch threw, tripping aim until rejoin");
    g_tb_tripped.store(true, std::memory_order_release);
    env->ExceptionClear();
  }
  static std::atomic<uint64_t> locks{0};
  if (++locks % 300 == 1)
    VLOG("aim", "locked %s d=%.1fm ang=%.1f (speed=%.1f smooth=%.1f)",
         best_name, best_dist, best_ang,
         (double)c.aimassist_speed, (double)c.aimassist_smooth);
  env->DeleteLocalRef(player);
  env->DeleteLocalRef(mc);
}

} // namespace jvm
} // namespace voltage
