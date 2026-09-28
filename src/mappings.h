#pragma once
// Dual-mapping name tables for 1.21.4:
//  - vanilla: Mojang production obfuscation (flk, gga, dA, ...)
//  - fabric : FabricMC intermediary (net/minecraft/class_310, method_23317, ...)
// Runtime auto-detect happens in jvm.cpp via JVMTI GetLoadedClasses:
//   Lflk; present                -> vanilla
//   Lnet/minecraft/class_310;    -> fabric
// Names here mirror docs/1.21.4_jni_names.md + the intermediary tiny map from
// net.fabricmc:intermediary:1.21.4 (checked against the local launcher jar).
#include <cstdint>

namespace voltage {
namespace map {

enum class Profile : uint8_t { Vanilla = 0, Fabric = 1 };

struct Names {
  // classes (JNI internal names, '/' separators)
  const char* mc;          // net.minecraft.client.Minecraft      (flk / class_310)
  const char* level;       // ClientLevel                          (gga / class_638)
  const char* player;      // LocalPlayer                          (gkx / class_746)
  const char* entity;      // Entity                               (bum / class_1297)
  const char* living;      // LivingEntity                         (bvi / class_1309)
  const char* playerBase;  // Player (abstract)                    (coy / class_1657)
  const char* component;   // Component                            (wp  / class_2561)
  const char* gameRenderer;// GameRenderer                         (glq / class_757)
  const char* camera;      // Camera                               (fks / class_4184)
  const char* vec3;        // Vec3                                 (fbb / class_243)
  const char* aabb;        // AABB                                 (faw / class_238)
  const char* gameMode;    // MultiPlayerGameMode                  (ggk / class_636)
  const char* hitResult;   // HitResult                            (faz / class_239)
  const char* entityHit;   // EntityHitResult                      (fay / class_3966)
  const char* hand;        // InteractionHand / Hand               (bsk / class_1268)
  // Minecraft members
  const char* getInstance; // static ()LMc;
  const char* fLevel; const char* fPlayer;
  const char* fGameRenderer; const char* fGameMode; const char* fHitResult;
  const char* mGetMainCamera;   // GameRenderer.getMainCamera()
  const char* mGetFov;          // GameRenderer.getFov(Camera,F,Z)F
  const char* mGetEntities;     // ClientLevel.entitiesForRendering()
  // Entity members
  const char* mGetX; const char* mGetY; const char* mGetZ;
  const char* mAlive;
  const char* mName;            // ()LComponent;
  const char* mGetString;       // Component.getString()LString; — vanilla: "getString", fabric: method_54160
  const char* mGetBox;          // ()LAABB;
  const char* mSwing;           // LivingEntity.swing(Hand)V
  const char* mCooldown;        // Player.getAttackCooldownProgress(F)F
  const char* mAttack;          // MultiPlayerGameMode.attack(Player,Entity)V
  const char* mHitEntity;       // EntityHitResult.getEntity()LEntity;
  const char* fMainHand;        // InteractionHand.MAIN_HAND static — vanilla: "MAIN_HAND", fabric: field_5808
  // AutoSprint members (Entity base class, both are overrides on LivingEntity)
  const char* mSetSprinting;    // Entity.setSprinting(Z)V
  const char* mIsSprinting;     // Entity.isSprinting()Z
  const char* mJumpFromGround;  // LivingEntity.jumpFromGround()V
  // OnlyCrit (vanilla crit rule): fallDistance>0, airborne, dry, no sprint
  const char* fFallDistance;    // Entity.fallDistance float field
  const char* mOnGround;        // Entity onGround()Z (mojmap) / isOnGround (yarn)
  const char* mTouchWater;      // Entity isInWater()Z (mojmap) / isTouchingWater (yarn)
  // AimAssist: direct rotation write on the render thread
  const char* mSetYaw;          // Entity.setYRot(F)V (mojmap) / setYaw (yarn)
  const char* mSetPitch;        // Entity.setXRot(F)V (mojmap) / setPitch (yarn)
  const char* mGetVel;          // Entity.getVelocity()LVec3; (aim prediction)
  // Camera members
  const char* mGetPosition;     // ()LVec3;
  const char* mGetXRot; const char* mGetYRot;
  // Vec3 fields
  const char* vX; const char* vY; const char* vZ;
  // AABB fields
  const char* bMinX; const char* bMinY; const char* bMinZ;
  const char* bMaxX; const char* bMaxY; const char* bMaxZ;
};

// Vanilla 1.21.4 (Mojang production obfuscation).
static constexpr Names kVanilla = {
  /*mc*/          "flk",
  /*level*/       "gga",
  /*player*/      "gkx",
  /*entity*/      "bum",
  /*living*/      "bvi",
  /*playerBase*/  "coy",
  /*component*/   "wp",
  /*gameRenderer*/"glq",
  /*camera*/      "fks",
  /*vec3*/        "fbb",
  /*aabb*/        "faw",
  /*gameMode*/    "ggk",
  /*hitResult*/   "faz",
  /*entityHit*/   "fay",
  /*hand*/        "bsk",
  /*getInstance*/ "Q",       // ()Lflk;
  /*fLevel*/      "s",       // Lgga;
  /*fPlayer*/     "t",       // Lgkx;
  /*fGameRenderer*/ "j",     // Lglq;
  /*fGameMode*/     "r",     // Lggk;
  /*fHitResult*/    "w",     // Lfaz;
  /*mGetMainCamera*/ "k",    // ()Lfks;
  /*mGetFov*/        "a",    // (Lfks;FZ)F
  /*mGetEntities*/   "d",    // ()Ljava/lang/Iterable;
  /*mGetX*/ "dA", /*mGetY*/ "dC", /*mGetZ*/ "dG",
  /*mAlive*/  "bL",
  /*mName*/   "al",
  /*mGetString*/ "getString",
  /*mGetBox*/ "cR",
  /*mSwing*/     "a",        // LivingEntity.a(Lbsk;)V — bvi.a
  /*mCooldown*/  "v",        // Player.v(F)F
  /*mAttack*/    "a",        // MultiPlayerGameMode.a(Lcoy;Lbum;)V — ggk.a
  /*mHitEntity*/ "a",        // EntityHitResult.a()Lbum; — fay.a
  /*fMainHand*/  "MAIN_HAND", // enum constant kept unobfuscated in production
  /*mSetSprinting*/   "h",   // Entity.h(Z)V — verified via client.txt
  /*mIsSprinting*/    "cj",  // Entity.cj()Z
  /*mJumpFromGround*/ "s",   // LivingEntity.s()V
  /*fFallDistance*/   "Z",   // Entity fallDistance float field
  /*mOnGround*/       "aJ",  // Entity.onGround()Z
  /*mTouchWater*/     "bj",  // Entity.isInWater()Z (yarn: isTouchingWater, same method)
  /*mSetYaw*/         "v",   // Entity.setYRot(F)V (yarn: setYaw)
  /*mSetPitch*/       "w",   // Entity.setXRot(F)V (yarn: setPitch)
  /*mGetVel*/         "dy",  // Entity.getVelocity()Lfbb;
  /*mGetPosition*/ "b",      // ()Lfbb;
  /*mGetXRot*/ "d", /*mGetYRot*/ "e",
  /*vX*/ "d", /*vY*/ "e", /*vZ*/ "f",
  /*bMinX*/ "a", /*bMinY*/ "b", /*bMinZ*/ "c",
  /*bMaxX*/ "d", /*bMaxY*/ "e", /*bMaxZ*/ "f",
};

// Fabric 1.21.4 (intermediary). Class names are net/minecraft/class_NNNN;
// methods/fields are method_NNNNN / field_NNNNN. Component.getString() is
// also unobfuscated here. Hand.MAIN_HAND = field_5810 (bsk.b).
static constexpr Names kFabric = {
  /*mc*/          "net/minecraft/class_310",
  /*level*/       "net/minecraft/class_638",
  /*player*/      "net/minecraft/class_746",
  /*entity*/      "net/minecraft/class_1297",
  /*living*/      "net/minecraft/class_1309",
  /*playerBase*/  "net/minecraft/class_1657",
  /*component*/   "net/minecraft/class_2561",
  /*gameRenderer*/"net/minecraft/class_757",
  /*camera*/      "net/minecraft/class_4184",
  /*vec3*/        "net/minecraft/class_243",
  /*aabb*/        "net/minecraft/class_238",
  /*gameMode*/    "net/minecraft/class_636",
  /*hitResult*/   "net/minecraft/class_239",
  /*entityHit*/   "net/minecraft/class_3966",
  /*hand*/        "net/minecraft/class_1268",
  /*getInstance*/ "method_1551",
  /*fLevel*/      "field_1687",
  /*fPlayer*/     "field_1724",
  /*fGameRenderer*/ "field_1773",
  /*fGameMode*/     "field_1761",
  /*fHitResult*/    "field_1765",  // crosshairTarget (Lfaz; -> class_239)
  /*mGetMainCamera*/ "method_19418",
  /*mGetFov*/        "method_3196",
  /*mGetEntities*/   "method_18112",
  /*mGetX*/ "method_23317", /*mGetY*/ "method_23318", /*mGetZ*/ "method_23321",
  /*mAlive*/  "method_5805",
  /*mName*/   "method_5477",   // on class_1297? declared further up the hierarchy — resolved via subclass lookup
  /*mGetString*/ "method_54160", // Component.getString()Ljava/lang/String; (NOT the (I) overload method_10858)
  /*mGetBox*/ "method_33332",  // bum.ax -> method_33332 (Entity.getBoundingBox, 1.21.4)
  /*mSwing*/     "method_6104",// LivingEntity.swing(Hand)
  /*mCooldown*/  "method_7261",// Player.getAttackCooldownProgress
  /*mAttack*/    "method_2918",// ClientPlayerInteractionManager.attackEntity
  /*mHitEntity*/ "method_17782",
  /*fMainHand*/  "field_5808", // Hand.MAIN_HAND (official obf bsk.a)
  /*mSetSprinting*/   "method_5728",
  /*mIsSprinting*/    "method_5624",
  /*mJumpFromGround*/ "method_6043",
  /*fFallDistance*/   "field_6017",
  /*mOnGround*/       "method_24828",
  /*mTouchWater*/     "method_5799",
  /*mSetYaw*/         "method_36456",
  /*mSetPitch*/       "method_36457",
  /*mGetVel*/         "method_18798",
  /*mGetPosition*/ "method_19326",
  /*mGetXRot*/ "method_19329", /*mGetYRot*/ "method_19330",
  /*vX*/ "field_1352", /*vY*/ "field_1351", /*vZ*/ "field_1350",
  /*bMinX*/ "field_1323", /*bMinY*/ "field_1322", /*bMinZ*/ "field_1321",
  /*bMaxX*/ "field_1320", /*bMaxY*/ "field_1325", /*bMaxZ*/ "field_1324",
};

inline const Names& names(Profile p) {
  return p == Profile::Fabric ? kFabric : kVanilla;
}

// Static runtime descriptors (shared by both profiles where identical).
// Signatures that reference obfuscated classes must be built per-profile:
inline const char* mcSig(Profile p)    { return p == Profile::Fabric ? "Lnet/minecraft/class_310;" : "Lflk;"; }
inline const char* levelSig(Profile p) { return p == Profile::Fabric ? "Lnet/minecraft/class_638;" : "Lgga;"; }
inline const char* playerSig(Profile p){ return p == Profile::Fabric ? "Lnet/minecraft/class_746;" : "Lgkx;"; }
inline const char* grSig(Profile p)    { return p == Profile::Fabric ? "Lnet/minecraft/class_757;" : "Lglq;"; }
inline const char* gmSig(Profile p)    { return p == Profile::Fabric ? "Lnet/minecraft/class_636;" : "Lggk;"; }
inline const char* hitSig(Profile p)   { return p == Profile::Fabric ? "Lnet/minecraft/class_239;" : "Lfaz;"; }
inline const char* ehitSig(Profile p)  { return p == Profile::Fabric ? "Lnet/minecraft/class_3966;" : "Lfay;"; }
inline const char* camSig(Profile p)   { return p == Profile::Fabric ? "Lnet/minecraft/class_4184;" : "Lfks;"; }
inline const char* vecSig(Profile p)   { return p == Profile::Fabric ? "Lnet/minecraft/class_243;" : "Lfbb;"; }
inline const char* aabbSig(Profile p)  { return p == Profile::Fabric ? "Lnet/minecraft/class_238;" : "Lfaw;"; }
inline const char* compSig(Profile p)  { return p == Profile::Fabric ? "Lnet/minecraft/class_2561;" : "Lwp;"; }
inline const char* handSig(Profile p)  { return p == Profile::Fabric ? "Lnet/minecraft/class_1268;" : "Lbsk;"; }
inline const char* entSig(Profile p)   { return p == Profile::Fabric ? "Lnet/minecraft/class_1297;" : "Lbum;"; }
inline const char* plSig(Profile p)    { return p == Profile::Fabric ? "Lnet/minecraft/class_1657;" : "Lcoy;"; }
// The gate class JVMTI looks for before touching FindClass.
inline const char* gateSig(Profile p)  { return p == Profile::Fabric ? "Lnet/minecraft/class_310;" : "Lflk;"; }

} // namespace map
} // namespace voltage
