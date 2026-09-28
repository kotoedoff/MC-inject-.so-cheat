// voltage.so entry point: runs before the host program's main()
// when loaded via LD_PRELOAD, or on-demand via gdb `call dlopen` (v2 inject).
// Architecture v2: the library must tolerate BOTH load paths:
//  - LD_PRELOAD: constructor runs years before the JVM/GL exist; the
//    background probe thread waits for the game.
//  - remote dlopen (gdb attach): constructor runs INSIDE a live game on a
//    stopped-process gdb thread; the probe thread starts afterwards and the
//    swap hook engages on the next rendered frame.
#include "log.h"
#include "jvm.h"
#include "gl_hook.h"
#include <unistd.h>
#include <limits.h>
#include <string>

__attribute__((constructor)) static void voltage_init() {
  char exe[PATH_MAX] = "?";
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) exe[n] = '\0';
  VLOG("inject", "loaded into %s", exe);
  voltage::gl::probe();
  voltage::jvm::start_background_probe();
}

// Called by scripts/eject_remote.sh via gdb BEFORE dlclose: puts the game back
// into a vanilla input state. Swap hook self-checks cfg flags, so after this
// call the ESP/menu/TriggerBot stop drawing immediately.
extern "C" __attribute__((visibility("default"))) int voltage_eject() {
  voltage::gl::on_eject();
  VLOG("inject", "eject requested");
  return 0;
}

__attribute__((destructor)) static void voltage_fini() {
  VLOG("inject", "unloaded");
}
