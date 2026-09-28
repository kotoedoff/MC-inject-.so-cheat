#pragma once
#include <jni.h>
#include "snapshot.h"

namespace voltage {
namespace jvm {

void start_background_probe();
// True once the producer thread resolved all JNI ids (menu shows "disconnected"
// state before that). Never call JNI entry points before this is true.
bool jvm_armed();
// Called from the RENDER thread (swap hook) with the current viewport:
// fetches camera pos/rot + real FOV (useFovSetting, like the game's world
// projection) and builds the perspective matrix. blaze3d asserts require
// the render thread, so this must NOT be called from the background producer.
bool refresh_camera(SnapCam& out, int vp_w, int vp_h);
// TriggerBot tick: resolves mc.hitResult -> entity and attacks on the render
// thread when cooldown/distance/toggles allow. No-op unless enabled & armed.
void triggerbot_tick();
// AimAssist tick: silent yaw/pitch pull toward the best living target on the
// render thread. Movement keys untouched. No-op unless enabled & armed.
void aimassist_tick();
// AutoSprint: keeps player sprinting; legit reset on hit (WTap-lite).
void autosprint_tick();
void autosprint_note_hit();
int created_vm_count();
JavaVM* first_vm();
void dump_entities_loop(JavaVM* vm, JNIEnv* env);

} // namespace jvm
} // namespace voltage
