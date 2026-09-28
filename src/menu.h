#pragma once
// ClickGUI (ImGui) interface. Implementation in menu.cpp.
// The menu draws inside the swap hook on the render thread; input arrives
// via gl_hook.cpp's dlsym-interposed GLFW callback installers.
#include "config.h"
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

namespace voltage {
namespace menu {

struct State {
  bool ready = false;
  bool failed = false;
  GLuint prog = 0, vao = 0, vbo = 0, ibo = 0;
  GLint u_proj = -1, u_tex = -1;
  GLuint font_tex = 0;
  double last_time = 0.0;
};

State& state();
bool menu_ok();
bool available();

// True while the menu should swallow game input.
bool want_mouse();
bool want_keyboard();

// Render the overlay (creates context lazily). Call inside the swap hook.
void render_frame(int fb_w, int fb_h);
void shutdown();

// Input hooks (return true = consumed, do not forward to the game).
bool key_event(int key, int scancode, int action, int mods);
bool char_event(unsigned int codepoint);
bool mouse_button_event(int button, int action, int mods);
bool cursor_pos_event(double x, double y);
bool scroll_event(double dx, double dy);

// Persistence (config.json).
void cfg_save();
void cfg_load();
const char* config_path();

} // namespace menu
} // namespace voltage
