// ClickGUI: ImGui overlay rendered inside the swap hook.
//
// Vendor: third_party/imgui (ocornut/imgui v1.91.8, core files only).
// Backend: hand-written OpenGL 3.3-core draw pass + GLFW input interception
// done in gl_hook.cpp's dlsym interposer (menu input hooks below).
//
// Golden rules honored here:
//  - full GL state save/restore around our draw pass;
//  - no EGL/GLFW symbol fakery (only key/mouse CALLBACK INSTALL fns are
//    intercepted in dlsym — never window/swap/context functions);
//  - while the menu is open, key/click events are consumed so the game does
//    not process them; mouse motion is always observed, consumed only when open.
#include "menu.h"
#include "log.h"
#include "jvm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "imgui.h"

namespace voltage {
namespace menu {

namespace {
State& g = state();
const char* kConfigPath = "config.json";

// GLFW key codes -> ImGuiKey (subset the menu needs).
ImGuiKey glfw_to_imgui(int key) {
  switch (key) {
    case 256: return ImGuiKey_Escape;      // GLFW_KEY_ESCAPE
    case 257: return ImGuiKey_Enter;       // GLFW_KEY_ENTER
    case 258: return ImGuiKey_Tab;         // GLFW_KEY_TAB
    case 259: return ImGuiKey_Backspace;   // GLFW_KEY_BACKSPACE
    case 260: return ImGuiKey_Insert;      // GLFW_KEY_INSERT
    case 261: return ImGuiKey_Delete;      // GLFW_KEY_DELETE
    case 262: return ImGuiKey_RightArrow;  // GLFW_KEY_RIGHT
    case 263: return ImGuiKey_LeftArrow;   // GLFW_KEY_LEFT
    case 264: return ImGuiKey_DownArrow;   // GLFW_KEY_DOWN
    case 265: return ImGuiKey_UpArrow;     // GLFW_KEY_UP
    case 266: return ImGuiKey_PageUp;      // GLFW_KEY_PAGE_UP
    case 267: return ImGuiKey_PageDown;    // GLFW_KEY_PAGE_DOWN
    case 268: return ImGuiKey_Home;        // GLFW_KEY_HOME
    case 269: return ImGuiKey_End;         // GLFW_KEY_END
    case 340: return ImGuiKey_ModShift;    // GLFW_KEY_LEFT_SHIFT
    case 341: return ImGuiKey_ModCtrl;     // GLFW_KEY_LEFT_CONTROL
    case 342: return ImGuiKey_ModAlt;      // GLFW_KEY_LEFT_ALT
    case 343: return ImGuiKey_ModSuper;    // GLFW_KEY_LEFT_SUPER
    case 344: return ImGuiKey_ModShift;    // GLFW_KEY_RIGHT_SHIFT
    case 345: return ImGuiKey_ModCtrl;     // GLFW_KEY_RIGHT_CONTROL
    case 346: return ImGuiKey_ModAlt;      // GLFW_KEY_RIGHT_ALT
    case 347: return ImGuiKey_ModSuper;    // GLFW_KEY_RIGHT_SUPER
    default: break;
  }
  if (key >= 32 && key <= 96) return (ImGuiKey)key;  // space..grave: letters/digits
  return ImGuiKey_None;
}

void build_font() {
  ImGuiIO& io = ImGui::GetIO();
  const char* candidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  };
  bool ok = false;
  for (const char* f : candidates) {
    FILE* t = std::fopen(f, "rb");
    if (!t) continue;
    std::fclose(t);
    io.Fonts->AddFontFromFileTTF(f, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    ok = true;
    VLOG("menu", "font: %s (ASCII + Cyrillic)", f);
    break;
  }
  if (!ok) io.Fonts->AddFontDefault();
  unsigned char* px = nullptr; int w = 0, h = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  // MC/Sodium leave pixel-store state dirty (ROW_LENGTH etc.) — a sheared
  // upload here means permanently garbled menu glyphs. Save/set/restore all.
  GLint ps_align = 4, ps_rowlen = 0, ps_skip_p = 0, ps_skip_r = 0, ps_imgh = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &ps_align);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &ps_rowlen);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &ps_skip_p);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &ps_skip_r);
  glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &ps_imgh);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
  glGenTextures(1, &g.font_tex);
  glBindTexture(GL_TEXTURE_2D, g.font_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glPixelStorei(GL_UNPACK_ALIGNMENT, ps_align);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, ps_rowlen);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, ps_skip_p);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, ps_skip_r);
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, ps_imgh);
  VLOG("menu", "font atlas %dx%d tex=%u", w, h, (unsigned)g.font_tex);
  io.Fonts->SetTexID((ImTextureID)(intptr_t)g.font_tex);
}

bool build_pipeline() {
  const char* vs =
    "#version 150 core\n"
    "uniform mat4 ProjMtx;\n"
    "in vec2 Position;\n in vec2 UV;\n in vec4 Color;\n"
    "out vec2 Frag_UV; out vec4 Frag_Color;\n"
    "void main(){ Frag_UV = UV; Frag_Color = Color;"
    " gl_Position = ProjMtx * vec4(Position.xy, 0.0, 1.0); }\n";
  const char* fs =
    "#version 150 core\n"
    "uniform sampler2D Texture;\n"
    "in vec2 Frag_UV; in vec4 Frag_Color;\n"
    "out vec4 Out_Color;\n"
    "void main(){ Out_Color = Frag_Color * texture(Texture, Frag_UV.st); }\n";
  auto sh = [&](GLenum t, const char* src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char buf[512] = {0};
      glGetShaderInfoLog(s, sizeof(buf) - 1, nullptr, buf);
      VLOG("menu", "shader compile failed: %s", buf);
    }
    return s;
  };
  GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
  g.prog = glCreateProgram();
  glAttachShader(g.prog, v);
  glAttachShader(g.prog, f);
  glBindAttribLocation(g.prog, 0, "Position");
  glBindAttribLocation(g.prog, 1, "UV");
  glBindAttribLocation(g.prog, 2, "Color");
  glLinkProgram(g.prog);
  GLint ok = 0;
  glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
  if (!ok) { VLOG("menu", "menu program link failed"); return false; }
  g.u_proj = glGetUniformLocation(g.prog, "ProjMtx");
  g.u_tex = glGetUniformLocation(g.prog, "Texture");
  glGenBuffers(1, &g.vbo);
  glGenBuffers(1, &g.ibo);
  glGenVertexArrays(1, &g.vao);
  return true;
}

void draw_pane_esp() {
  cfg::Config& c = cfg::get();
  ImGui::TextUnformatted("ESP");
  ImGui::Separator();
  ImGui::Checkbox("enabled##esp", &c.esp_enabled);
  ImGui::BeginDisabled(!c.esp_enabled);
  ImGui::Checkbox("boxes", &c.esp_boxes);
  ImGui::Checkbox("names + distance", &c.esp_names);
  ImGui::SliderFloat("max dist##esp", &c.esp_max_dist, 8.0f, 256.0f, "%.0f m");
  ImGui::EndDisabled();
}

void draw_pane_aimassist() {
  cfg::Config& c = cfg::get();
  ImGui::TextUnformatted("AimAssist (rotation only, move freely)");
  ImGui::Separator();
  ImGui::Checkbox("enabled##aim", &c.aimassist_enabled);
  ImGui::BeginDisabled(!c.aimassist_enabled);
  ImGui::SliderFloat("range##aim", &c.aimassist_range, 1.0f, 10.0f, "%.1f m");
  ImGui::SliderFloat("speed##aim", &c.aimassist_speed, 1.0f, 20.0f, "%.1f");
  ImGui::SliderFloat("smooth##aim", &c.aimassist_smooth, 1.0f, 20.0f, "%.1f");
  ImGui::SliderFloat("fov##aim", &c.aimassist_fov, 10.0f, 360.0f, "%.0f deg");
  ImGui::Checkbox("players##aim", &c.aimassist_players);
  ImGui::Checkbox("mobs##aim", &c.aimassist_mobs);
  ImGui::EndDisabled();
}

void draw_pane_triggerbot() {
  cfg::Config& c = cfg::get();
  ImGui::TextUnformatted("TriggerBot");
  ImGui::Separator();
  ImGui::Checkbox("enabled##tb", &c.triggerbot_enabled);
  ImGui::BeginDisabled(!c.triggerbot_enabled);
  ImGui::SliderFloat("cooldown", &c.triggerbot_cooldown, 0.80f, 1.0f, "%.2f");
  ImGui::SliderFloat("range##tb", &c.triggerbot_range, 2.0f, 4.5f, "%.1f m");
  ImGui::Checkbox("only crits##tb", &c.triggerbot_only_crits);
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextUnformatted("AutoSprint");
  ImGui::Separator();
  ImGui::Checkbox("enabled##sprint", &c.autosprint_enabled);
  ImGui::BeginDisabled(!c.autosprint_enabled);
  ImGui::Checkbox("sprint reset on hit (WTap)", &c.autosprint_wtap);
  ImGui::Checkbox("hop on hit", &c.autosprint_jump);
  ImGui::SliderInt("resume ticks", &c.autosprint_resume_ticks, 1, 10);
  ImGui::EndDisabled();
}

void draw_status() {
  ImGui::Text("jvm: %s", voltage::jvm::jvm_armed() ? "armed" : "resolving...");
  ImGui::Text("RShift / Insert: open-close menu");
  ImGui::Text("config: %s", kConfigPath);
}

} // namespace

bool available() { return menu_ok(); }
bool menu_ok() { return state().ready; }
bool want_mouse() { return cfg::get().menu_open && state().ready; }
bool want_keyboard() { return cfg::get().menu_open && state().ready; }

State& state() { static State s; return s; }

bool init() {
  if (g.ready) return true;
  if (g.failed) return false;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // don't litter the game dir with imgui.ini
  ImGui::StyleColorsDark();
  build_font();
  if (!build_pipeline()) { g.failed = true; return false; }
  g.ready = true;
  cfg_load();
  VLOG("menu", "ImGui ClickGUI ready (v%s)", ImGui::GetVersion());
  return true;
}

void render_frame(int fb_w, int fb_h) {
  if (!g.ready) {
    if (!init()) return;
  }
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)fb_w, (float)fb_h);
  io.DeltaTime = 1.0f / 60.0f;
  io.MouseDrawCursor = cfg::get().menu_open;
  io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;

  ImGui::NewFrame();
  if (cfg::get().menu_open) {
    // Opaque dark theme: the default style has semi-transparent window bg
    // which looks broken over a bright game scene.
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.GrabRounding = 4.0f;
    st.WindowPadding = ImVec2(10, 10);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.97f);
    st.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.30f, 0.55f, 1.00f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.75f, 0.45f, 1.00f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.60f, 0.95f, 1.00f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.24f, 0.34f, 1.00f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.32f, 0.46f, 1.00f);
    st.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);

    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("voltage 1.21.4", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("##tabs")) {
      if (ImGui::BeginTabItem("ESP")) { draw_pane_esp(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("TriggerBot")) { draw_pane_triggerbot(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("AimAssist")) { draw_pane_aimassist(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Info")) { draw_status(); ImGui::EndTabItem(); }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("save config")) cfg_save();
    ImGui::SameLine();
    if (ImGui::Button("close")) cfg::get().menu_open = false;
    ImGui::End();
  }
  ImGui::Render();

  ImDrawData* dd = ImGui::GetDrawData();
  if (!dd || dd->TotalIdxCount <= 0) return;

  // ---- GL state save ----
  GLint prev_prog = 0, prev_vao = 0, prev_vbo = 0, prev_ibo = 0, prev_tex = 0, prev_ab = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_ibo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_ab);
  GLboolean was_blend = glIsEnabled(GL_BLEND);
  GLboolean was_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean was_cull = glIsEnabled(GL_CULL_FACE);
  GLint prev_sc[4] = {0, 0, 0, 0};
  GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_SCISSOR_BOX, prev_sc);
  GLint prev_viewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glViewport(0, 0, fb_w, fb_h);

  const float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x;
  const float T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
  float ortho[16] = {
    2.0f / (R - L), 0.0f, 0.0f, 0.0f,
    0.0f, 2.0f / (T - B), 0.0f, 0.0f,
    0.0f, 0.0f, 0.5f, 0.0f,
    (R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f,
  };

  glUseProgram(g.prog);
  glUniform1i(g.u_tex, 0);
  glUniformMatrix4fv(g.u_proj, 1, GL_FALSE, ortho);
  glBindVertexArray(g.vao);
  glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, pos));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, uv));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, col));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);

  glActiveTexture(GL_TEXTURE0);
  ImVec2 clip_off = dd->DisplayPos;
  for (int n = 0; n < dd->CmdListsCount; n++) {
    const ImDrawList* cl = dd->CmdLists[n];
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cl->VtxBuffer.Size * sizeof(ImDrawVert),
                 cl->VtxBuffer.Data, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)cl->IdxBuffer.Size * sizeof(ImDrawIdx),
                 cl->IdxBuffer.Data, GL_STREAM_DRAW);
    for (int ci = 0; ci < cl->CmdBuffer.Size; ci++) {
      const ImDrawCmd* cmd = &cl->CmdBuffer[ci];
      if (cmd->UserCallback) { cmd->UserCallback(cl, cmd); continue; }
      float x0 = cmd->ClipRect.x - clip_off.x, y0 = cmd->ClipRect.y - clip_off.y;
      float x1 = cmd->ClipRect.z - clip_off.x, y1 = cmd->ClipRect.w - clip_off.y;
      if (x1 <= x0 || y1 <= y0) continue;
      glScissor((int)x0, (int)((float)fb_h - y1), (int)(x1 - x0), (int)(y1 - y0));
      glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->GetTexID());
      glDrawElements(GL_TRIANGLES, (GLsizei)cmd->ElemCount,
                     sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                     (void*)(intptr_t)cmd->IdxOffset);
    }
  }
  glDisable(GL_SCISSOR_TEST);

  // ---- GL state restore ----
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
  glScissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
  if (was_depth) glEnable(GL_DEPTH_TEST);
  if (was_cull) glEnable(GL_CULL_FACE);
  if (!was_blend) glDisable(GL_BLEND);
  glActiveTexture(prev_ab);
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prev_ibo);
  glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
  glBindVertexArray(prev_vao);
  glUseProgram(prev_prog);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

void shutdown() {
  if (!g.ready) return;
  if (g.font_tex) glDeleteTextures(1, &g.font_tex);
  if (g.vao) glDeleteVertexArrays(1, &g.vao);
  if (g.vbo) glDeleteBuffers(1, &g.vbo);
  if (g.ibo) glDeleteBuffers(1, &g.ibo);
  if (g.prog) glDeleteProgram(g.prog);
  ImGui::DestroyContext();
  g = State{};
}

// ---- input plumbling ----
// glfwSetKeyCallback-style fakery is deliberately NOT used (LWJGL does not
// install GLFW callbacks and chokes on foreign ones). Input arrives via the
// polled path in gl_hook.cpp (poll_menu_input). These helpers stay for the
// future LWJGL-safety-tested char/keyboard path.

bool key_event(int key, int scancode, int action, int mods) {
  if (!g.ready) return false;
  (void)scancode;
  if (!cfg::get().menu_open) return false;
  ImGuiKey k = glfw_to_imgui(key);
  if (k == ImGuiKey_None) return true;
  bool down = (action == 1 || action == 2 /* REPEAT */);
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Shift, (mods & 0x0001) != 0);
  io.AddKeyEvent(ImGuiMod_Ctrl, (mods & 0x0002) != 0);
  io.AddKeyEvent(ImGuiMod_Alt, (mods & 0x0004) != 0);
  io.AddKeyEvent(ImGuiMod_Super, (mods & 0x0008) != 0);
  io.AddKeyEvent(k, down);
  return true;
}

bool char_event(unsigned int codepoint) {
  if (!cfg::get().menu_open || !g.ready) return false;
  ImGui::GetIO().AddInputCharacter(codepoint);
  return true;
}

bool mouse_button_event(int button, int action, int /*mods*/) {
  if (!cfg::get().menu_open || !g.ready) return false;
  ImGui::GetIO().AddMouseButtonEvent(button, action == 1 || action == 2);
  return true;  // clicks go to the menu only
}

bool cursor_pos_event(double x, double y) {
  if (!g.ready) return false;
  ImGui::GetIO().AddMousePosEvent((float)x, (float)y);
  return cfg::get().menu_open;  // motion consumed only while menu open
}

bool scroll_event(double dx, double dy) {
  if (!cfg::get().menu_open || !g.ready) return false;
  ImGui::GetIO().AddMouseWheelEvent((float)dx, (float)dy);
  return true;
}

// ---- config persistence (config.json, tiny hand-rolled parser) ----

void cfg_save() {
  FILE* f = std::fopen(kConfigPath, "w");
  if (!f) { VLOG("menu", "cfg save failed: %s", kConfigPath); return; }
  cfg::Config& c = cfg::get();
  std::fprintf(f,
    "{\n"
    "  \"esp_enabled\": %s,\n"
    "  \"esp_boxes\": %s,\n"
    "  \"esp_names\": %s,\n"
    "  \"esp_max_dist\": %.1f,\n"
    "  \"triggerbot_enabled\": %s,\n"
    "  \"triggerbot_cooldown\": %.2f,\n"
    "  \"triggerbot_range\": %.1f,\n"
    "  \"triggerbot_only_crits\": %s,\n"
    "  \"aimassist_enabled\": %s,\n"
    "  \"aimassist_range\": %.1f,\n"
    "  \"aimassist_speed\": %.1f,\n"
    "  \"aimassist_smooth\": %.1f,\n"
    "  \"aimassist_fov\": %.1f,\n"
    "  \"aimassist_players\": %s,\n"
    "  \"aimassist_mobs\": %s,\n"
    "  \"autosprint_enabled\": %s,\n"
    "  \"autosprint_wtap\": %s,\n"
    "  \"autosprint_jump\": %s,\n"
    "  \"autosprint_resume_ticks\": %d\n"
    "}\n",
    c.esp_enabled ? "true" : "false",
    c.esp_boxes ? "true" : "false",
    c.esp_names ? "true" : "false",
    (double)c.esp_max_dist,
    c.triggerbot_enabled ? "true" : "false",
    (double)c.triggerbot_cooldown,
    (double)c.triggerbot_range,
    c.triggerbot_only_crits ? "true" : "false",
    c.aimassist_enabled ? "true" : "false",
    (double)c.aimassist_range,
    (double)c.aimassist_speed,
    (double)c.aimassist_smooth,
    (double)c.aimassist_fov,
    c.aimassist_players ? "true" : "false",
    c.aimassist_mobs ? "true" : "false",
    c.autosprint_enabled ? "true" : "false",
    c.autosprint_wtap ? "true" : "false",
    c.autosprint_jump ? "true" : "false",
    c.autosprint_resume_ticks);
  std::fclose(f);
  VLOG("menu", "config saved: %s", kConfigPath);
}

static void cfg_apply(const std::string& k, const std::string& v) {
  cfg::Config& c = cfg::get();
  bool b = (v == "true");
  float fv = (float)std::atof(v.c_str());
  if (k == "esp_enabled") c.esp_enabled = b;
  else if (k == "esp_boxes") c.esp_boxes = b;
  else if (k == "esp_names") c.esp_names = b;
  else if (k == "esp_max_dist") c.esp_max_dist = fv;
  else if (k == "triggerbot_enabled") c.triggerbot_enabled = b;
  else if (k == "triggerbot_cooldown") c.triggerbot_cooldown = fv;
  else if (k == "triggerbot_range") c.triggerbot_range = fv;
  else if (k == "triggerbot_only_crits") c.triggerbot_only_crits = b;
  else if (k == "aimassist_enabled") c.aimassist_enabled = b;
  else if (k == "aimassist_range") c.aimassist_range = fv;
  else if (k == "aimassist_speed") c.aimassist_speed = fv;
  else if (k == "aimassist_smooth") c.aimassist_smooth = fv;
  else if (k == "aimassist_fov") c.aimassist_fov = fv;
  else if (k == "aimassist_players") c.aimassist_players = b;
  else if (k == "aimassist_mobs") c.aimassist_mobs = b;
  else if (k == "autosprint_enabled") c.autosprint_enabled = b;
  else if (k == "autosprint_wtap") c.autosprint_wtap = b;
  else if (k == "autosprint_jump") c.autosprint_jump = b;
  else if (k == "autosprint_resume_ticks") c.autosprint_resume_ticks = (int)fv;
}

void cfg_load() {
  FILE* f = std::fopen(kConfigPath, "r");
  if (!f) return;  // first run: keep defaults
  char buf[4096];
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';
  std::string s(buf);
  size_t pos = 0;
  while ((pos = s.find('"', pos)) != std::string::npos) {
    size_t k0 = pos + 1;
    size_t k1 = s.find('"', k0);
    if (k1 == std::string::npos) break;
    std::string key = s.substr(k0, k1 - k0);
    size_t colon = s.find(':', k1);
    if (colon == std::string::npos) break;
    size_t v0 = s.find_first_not_of(" \t", colon + 1);
    if (v0 == std::string::npos) break;
    size_t v1 = s.find_first_of(",\n}", v0);
    if (v1 == std::string::npos) break;
    std::string val = s.substr(v0, v1 - v0);
    if (!val.empty() && val.front() == '"') val = val.substr(1, val.size() - 2);
    cfg_apply(key, val);
    pos = v1;
  }
  cfg::get().menu_loaded_config = true;
  VLOG("menu", "config loaded: %s", kConfigPath);
}

} // namespace menu
} // namespace voltage
