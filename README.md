# voltage

Native `.so` inject cheat for Minecraft Java 1.21.4 (Vanilla + Fabric). C++17, no Java mods involved.

## How it works

`libvoltage.so` is loaded into the game process via `LD_PRELOAD` (or `gdb dlopen` into a running game). Once inside it:
- finds the JVM (`JNI_GetCreatedJavaVMs`), waits out bootstrap via JVMTI, resolves Mojang/intermediary mappings (auto-detected profile);
- polls entities + camera through JNI;
- hooks `glXSwapBuffers` (caught through `dlsym` interposition, since LWJGL loads libGL itself) and draws ESP + ImGui ClickGUI over the frame;
- TriggerBot / AimAssist / AutoSprint act through JNI calls on the render thread.

## Platform

Linux only (x86_64, Wayland/X11). Windows port is straightforward (~20 min): swap `LD_PRELOAD`/`dlsym` plumbing for DLL injection + MinHook, the JNI/ESP core is platform-agnostic.

## Build & run

```bash
./scripts/build.sh                 # build/libvoltage.so
./scripts/run_test.sh              # smoke test
./scripts/launch_mc.sh Steve       # vanilla 1.21.4 + inject
./scripts/launch_mc_fabric.sh Steve # Fabric 1.21.4 + inject
./scripts/inject_remote.sh <pid>   # inject into a running game
```

`tail -f /tmp/voltage.log` for telemetry. Test in singleplayer first.
