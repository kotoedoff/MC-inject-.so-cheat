#pragma once
// GL/GLFW swap-buffers hook. Interception via dlsym interposition (+ plain
// LD_PRELOAD overrides). v2: supports late dlopen into a running game and a
// clean on_eject() before dlclose.
namespace voltage {
namespace gl {

void probe();      // log which swap functions are visible in this process
void on_eject();   // restore vanilla state before the lib is dlclosed

} // namespace gl
} // namespace voltage
