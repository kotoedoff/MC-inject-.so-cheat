#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gl_hook.h"
#include "log.h"
#include "config.h"
#include "snapshot.h"
#include "esp.h"
#include "font.h"
#include "jvm.h"
#include "menu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <mutex>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

// Opaque types: no X11/GLFW/EGL headers needed for pass-through signatures.
typedef struct _XDisplay Display;
typedef unsigned long GLXDrawable;
typedef struct GLFWwindow GLFWwindow;
typedef void* EGLDisplay;
typedef void* EGLSurface;

namespace voltage {
namespace gl {

void probe() {
  // These lookups must NOT crash when libs are absent (e.g. in `sleep` test).
  void* glx = dlsym(RTLD_DEFAULT, "glXSwapBuffers");
  void* glfw = dlsym(RTLD_DEFAULT, "glfwSwapBuffers");
  void* egl = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  VLOG("gl", "visible: glXSwapBuffers=%p glfwSwapBuffers=%p eglSwapBuffers=%p",
       glx, glfw, egl);
  if (!glx && !glfw && !egl) {
    VLOG("gl", "no swap fn visible (expected in non-GL test processes)");
  }
}

namespace {

using GlfwSwapFn = void (*)(GLFWwindow*);
using GlxSwapFn = void (*)(Display*, GLXDrawable);
using EglSwapFn = int (*)(EGLDisplay, EGLSurface);

GlfwSwapFn real_glfw = nullptr;
GlxSwapFn real_glx = nullptr;
EglSwapFn real_egl = nullptr;
std::mutex resolve_m;
bool backend_logged = false;

// real dlsym, bootstrapped via dlvsym (we don't interpose dlvsym, so no recursion).
using DlsymFn = void* (*)(void*, const char*);
DlsymFn real_dlsym = nullptr;

void ensure_real_dlsym() {
  if (!real_dlsym)
    real_dlsym = reinterpret_cast<DlsymFn>(::dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
}

// Only glXSwapBuffers is intercepted via dlsym: it is the proven present path
// on this setup (XWayland+GLX). Menu input is NOT grabbed from dlsym: LWJGL 3
// does not install GLFW callbacks and faking them crashes the game — the menu
// instead polls glfwGetKey/glfwGetCursorPos/glfwGetMouseButton every frame.
// eglSwapBuffers/glfwSwapBuffers are deliberately FORWARDED untouched:
// returning a non-NULL wrapper for them makes availability probes (NULL-checks)
// spuriously succeed and can switch GLFW to a broken EGL path -> boot stall.
bool is_swap_name(const char* n) {
  return n && !std::strcmp(n, "glXSwapBuffers");
}

// Bisect flag: VOLTAGE_NO_GL=1 disables all GL/dlsym/swap interception,
// leaving only the JNI entity dump. Decides whether the GL hook stalls boot.
bool gl_enabled() {
  static int v = -1;
  if (v < 0) {
    const char* e = std::getenv("VOLTAGE_NO_GL");
    v = (e && e[0] == '1') ? 0 : 1;
    if (!v) VLOG("gl", "GL hook DISABLED via VOLTAGE_NO_GL=1 (JNI dump only)");
  }
  return v == 1;
}

// Bisect level 2: VOLTAGE_NO_DLSYM=1 keeps the swap-symbol overrides but
// disables the dlsym() override (GLFW then resolves real swap fns itself).
bool dlsym_hook_enabled() {
  static int v = -1;
  if (v < 0) {
    const char* e = std::getenv("VOLTAGE_NO_DLSYM");
    v = (e && e[0] == '1') ? 0 : 1;
    if (!v) VLOG("gl", "dlsym hook DISABLED via VOLTAGE_NO_DLSYM=1");
  }
  return v == 1;
}

// LWJGL loads its natives (libglfw.so) RTLD_LOCAL: RTLD_NEXT/DEFAULT can't see
// their symbols. Fall back to explicit handles (NOLOAD = reuse, never load anew).
std::string find_maps_lib(const char* needle) {
  FILE* f = std::fopen("/proc/self/maps", "r");
  if (!f) return "";
  char* line = nullptr;
  size_t n = 0;
  std::string out;
  // maps format: "addr perms offset dev inode <pathname...>" — the pathname
  // itself may contain SPACES (e.g. ".../versions/Fabric 1.21.4/natives/..."),
  // so rfind(' ') truncation breaks it. Skip exactly 5 fields instead.
  while (::getline(&line, &n, f) > 0) {
    const char* c = line;
    for (int i = 0; i < 5; i++) {
      while (*c == ' ' || *c == '\t') c++;
      while (*c && *c != ' ' && *c != '\t' && *c != '\n') c++;
    }
    while (*c == ' ' || *c == '\t') c++;
    std::string path(c);
    while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
    if (path.size() >= std::strlen(needle) &&
        path.compare(path.size() - std::strlen(needle), std::strlen(needle), needle) == 0) {
      out = path;
      break;
    }
  }
  std::free(line);
  std::fclose(f);
  return out;
}

template <typename F>
F resolve_real(const char* name) {
  std::lock_guard<std::mutex> l(resolve_m);
  static bool logged_ok = false, logged_fail = false;
  void* p = real_dlsym ? real_dlsym(RTLD_NEXT, name) : nullptr;
  if (!p && real_dlsym) p = real_dlsym(RTLD_DEFAULT, name);
  if (!p) {
    void* h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (h && real_dlsym) p = real_dlsym(h, name);
  }
  if (!p) {
    void* h = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (h && real_dlsym) p = real_dlsym(h, name);
  }
  if (!p) {
    // Re-scan until found (first attempt may run before libglfw is mapped);
    // cache only the non-empty hit.
    static std::string glfw;
    if (glfw.empty()) glfw = find_maps_lib("libglfw.so");
    if (!glfw.empty()) {
      void* h = dlopen(glfw.c_str(), RTLD_LAZY | RTLD_NOLOAD);
      if (h && real_dlsym) p = real_dlsym(h, name);
    }
  }
  if (p && !logged_ok) { logged_ok = true; VLOG("gl", "real %s = %p", name, p); }
  if (!p && !logged_fail) { logged_fail = true; VLOG("gl", "real %s unresolved yet (retrying silently)", name); }
  return reinterpret_cast<F>(p);
}

// ---- minimal line renderer (GL 3.2+ core) ----
GLuint prog = 0, vao = 0, vbo = 0;
GLint u_res = -1;
bool draw_ok = true;  // false after a compile failure (log once, never retry)

// ---- text renderer: 8x8 atlas, ASCII + Cyrillic ----
GLuint tprog = 0, tvao = 0, tvbo = 0, tatlas = 0;
GLint tu_res = -1, tu_tex = -1;
bool text_ok = true;

void build_atlas() {
  using namespace font;
  const int cols = 16, rows = (kGlyphs + cols - 1) / cols;
  std::vector<uint8_t> px((size_t)cols * kGlyphW * rows * kGlyphH, 0);
  for (int g = 0; g < kGlyphs; g++) {
    int gx = (g % cols) * kGlyphW, gy = (g / cols) * kGlyphH;
    for (int y = 0; y < kGlyphH; y++)
      for (int x = 0; x < kGlyphW; x++)
        if (kBitmap[g][y] & (0x80 >> x))
          px[(size_t)((gy + y) * cols * kGlyphW) + gx + x] = 0xFF;
  }
  glGenTextures(1, &tatlas);
  glBindTexture(GL_TEXTURE_2D, tatlas);
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, cols * kGlyphW, rows * kGlyphH, 0, GL_RED, GL_UNSIGNED_BYTE, px.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, ps_align);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, ps_rowlen);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, ps_skip_p);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, ps_skip_r);
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, ps_imgh);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool ensure_text_renderer() {
  if (tprog || !text_ok) return text_ok;
  build_atlas();
  const char* vs =
    "#version 150 core\n"
    "uniform vec2 u_res;\n"
    "in vec2 p;\n"
    "in vec3 c;\n"
    "in vec2 t;\n"
    "out vec3 v_c; out vec2 v_t;\n"
    "void main(){ vec2 ndc = p / u_res * 2.0 - 1.0; ndc.y = -ndc.y;"
    " gl_Position = vec4(ndc, 0.0, 1.0); v_c = c; v_t = t; }\n";
  const char* fs =
    "#version 150 core\n"
    "uniform sampler2D u_tex;\n"
    "in vec3 v_c; in vec2 v_t;\n"
    "out vec4 o;\n"
    "void main(){ float a = texture(u_tex, v_t).r; o = vec4(v_c, a * 0.95); }\n";
  auto shader = [&](GLenum type, const char* src) -> GLuint {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char buf[1024] = {0};
      glGetShaderInfoLog(s, sizeof(buf) - 1, nullptr, buf);
      VLOG("gl", "text shader compile failed: %s", buf);
    }
    return s;
  };
  GLuint v = shader(GL_VERTEX_SHADER, vs);
  GLuint f = shader(GL_FRAGMENT_SHADER, fs);
  tprog = glCreateProgram();
  glAttachShader(tprog, v);
  glAttachShader(tprog, f);
  glBindAttribLocation(tprog, 0, "p");
  glBindAttribLocation(tprog, 1, "c");
  glBindAttribLocation(tprog, 2, "t");
  glLinkProgram(tprog);
  GLint ok = 0;
  glGetProgramiv(tprog, GL_LINK_STATUS, &ok);
  if (!ok) { VLOG("gl", "text program link failed"); text_ok = false; return false; }
  tu_res = glGetUniformLocation(tprog, "u_res");
  tu_tex = glGetUniformLocation(tprog, "u_tex");
  glGenVertexArrays(1, &tvao);
  glGenBuffers(1, &tvbo);
  VLOG("gl", "text renderer ready (atlas=%u glyphs=%d)", (unsigned)tatlas, (int)font::kGlyphs);
  return true;
}

bool ensure_renderer() {
  if (prog || !draw_ok) return draw_ok;
  const char* vs =
    "#version 150 core\n"
    "uniform vec2 u_res;\n"
    "in vec2 p;\n"
    "in vec3 c;\n"
    "out vec3 v_c;\n"
    "void main(){ vec2 ndc = p / u_res * 2.0 - 1.0; ndc.y = -ndc.y;"
    " gl_Position = vec4(ndc, 0.0, 1.0); v_c = c; }\n";
  const char* fs =
    "#version 150 core\n"
    "in vec3 v_c;\n"
    "out vec4 o;\n"
    "void main(){ o = vec4(v_c, 0.9); }\n";
  auto shader = [&](GLenum type, const char* src) -> GLuint {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char buf[1024] = {0};
      glGetShaderInfoLog(s, sizeof(buf) - 1, nullptr, buf);
      VLOG("gl", "shader compile failed: %s", buf);
    }
    return s;
  };
  GLuint v = shader(GL_VERTEX_SHADER, vs);
  GLuint f = shader(GL_FRAGMENT_SHADER, fs);
  prog = glCreateProgram();
  glAttachShader(prog, v);
  glAttachShader(prog, f);
  glBindAttribLocation(prog, 0, "p");
  glBindAttribLocation(prog, 1, "c");
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    VLOG("gl", "program link failed");
    draw_ok = false;
    return false;
  }
  u_res = glGetUniformLocation(prog, "u_res");
  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  VLOG("gl", "line renderer ready (prog=%u)", (unsigned)prog);
  return true;
}

struct V { float x, y, r, g, b; };

void push_rect(std::vector<V>& out, const esp::Rect& rc, float r, float g, float b) {
  float x0 = rc.x0, y0 = rc.y0, x1 = rc.x1, y1 = rc.y1;
  // 4 edges as line list
  V q[] = {{x0,y0,r,g,b},{x1,y0,r,g,b}, {x1,y0,r,g,b},{x1,y1,r,g,b},
           {x1,y1,r,g,b},{x0,y1,r,g,b}, {x0,y1,r,g,b},{x0,y0,r,g,b}};
  for (auto& v : q) out.push_back(v);
}

struct TV { float x, y, r, g, b, u, v; };  // text vertex: pos2 + color3 + uv2

// UTF-8 -> font glyph indices (ASCII + Cyrillic; unknown -> '?')
void utf8_to_glyphs(const char* s, std::vector<int>& out) {
  for (const unsigned char* p = (const unsigned char*)s; *p; ) {
    unsigned c = 0;
    if (p[0] < 0x80) { c = p[0]; p += 1; }
    else if ((p[0] & 0xE0) == 0xC0 && p[1]) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
    else { c = '?'; p += 1; }
    int gi;
    if (c >= 0x20 && c <= 0x7E) gi = (int)c;
    else if (c >= 0x410 && c <= 0x42F) gi = 128 + (c - 0x410);
    else if (c >= 0x430 && c <= 0x44F) gi = 160 + (c - 0x430);
    else if (c == 0x401) gi = 192;
    else if (c == 0x451) gi = 193;
    else if (c == ' ') gi = ' ';
    else gi = '?';
    out.push_back(gi);
  }
}

// One text line, centered horizontally at cx, top edge at ty. Color (r,g,b).
void push_text(std::vector<TV>& out, const char* s, float cx, float ty,
               float r, float gg, float b, float scale) {
  std::vector<int> gs;
  utf8_to_glyphs(s, gs);
  if (gs.empty()) return;
  const int cols = 16;
  const float gw = 8.0f * scale, gh = 8.0f * scale;
  const float atlas_w = 128.0f, atlas_h = 104.0f;  // 16 cols x 13 rows (194 glyphs)
  float x0 = cx - (float)gs.size() * gw * 0.5f;
  for (size_t i = 0; i < gs.size(); i++) {
    int g = gs[i];
    float u0 = (float)(g % cols) * 8.0f / atlas_w;
    float v0 = (float)(g / cols) * 8.0f / atlas_h;
    float u1 = u0 + 8.0f / atlas_w, v1 = v0 + 8.0f / atlas_h;
    float x = x0 + (float)i * gw, y = ty;
    TV q[6] = {
      {x, y, r, gg, b, u0, v0}, {x + gw, y, r, gg, b, u1, v0}, {x + gw, y + gh, r, gg, b, u1, v1},
      {x, y, r, gg, b, u0, v0}, {x + gw, y + gh, r, gg, b, u1, v1}, {x, y + gh, r, gg, b, u0, v1},
    };
    for (auto& tv : q) out.push_back(tv);
  }
}

void draw_frame() {
  if (!cfg::get().esp_enabled) return;
  GLint vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;

  Snapshot& s = snapshot();
  std::vector<SnapEnt> ents;
  SnapCam cam;
  {
    std::lock_guard<std::mutex> l(s.m);
    if (s.tick == 0) return;
    ents = s.ents;
    cam = s.cam;
  }
  // Frame-truth camera + projection from the render thread (this thread).
  // Falls back to the producer's snapshot while JVM resolution is pending.
  bool fresh = jvm::refresh_camera(cam, w, h);
  static uint64_t diag_n = 0;
  if (++diag_n % 120 == 1)
    VLOG("gl", "draw diag: fresh=%d pos=%.1f %.1f %.1f yaw=%.1f pitch=%.1f proj_ok=%d ents=%d",
         (int)fresh, cam.x, cam.y, cam.z, cam.yaw, cam.pitch, (int)cam.proj_ok, (int)ents.size());
  if (ents.empty() || !cam.valid || !cam.proj_ok) return;

  const cfg::Config& cfg = cfg::get();
  esp::Cam c{cam.x, cam.y, cam.z, cam.yaw, cam.pitch};
  std::vector<V> verts;
  verts.reserve(ents.size() * 8);
  std::vector<TV> tverts;
  tverts.reserve(ents.size() * 64);
  char line[128];
  for (auto& e : ents) {
    if (!e.alive) continue;
    if (e.self) continue;  // no box on own player (first/third person)
    double ex = (e.minX + e.maxX) * 0.5, ey = (e.minY + e.maxY) * 0.5, ez = (e.minZ + e.maxZ) * 0.5;
    double ddx = ex - cam.x, ddy = ey - cam.y, ddz = ez - cam.z;
    double dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
    if (dist > (double)cfg.esp_max_dist) continue;  // menu max-distance slider
    esp::Rect rc;
    if (!esp::box_rect(c, cam.proj, e.minX, e.minY, e.minZ, e.maxX, e.maxY, e.maxZ, w, h, rc)) continue;
    float br = 1.0f, bg = 1.0f, bb = 1.0f;  // box color
    if (!e.player) { br = 0.25f; bg = 1.0f; bb = 0.25f; }
    if (cfg.esp_boxes) push_rect(verts, rc, br, bg, bb);
    // name + distance above the box
    if (cfg.esp_names) {
      std::snprintf(line, sizeof(line), "%s %.1fm", e.name, dist);
      push_text(tverts, line, (rc.x0 + rc.x1) * 0.5f, rc.y0 - 12.0f, br, bg, 0.3f, 1.0f);
    }
  }
  if (verts.empty() && tverts.empty()) return;
  if (!ensure_renderer()) return;

  // save state
  GLint prev_prog = 0, prev_vao = 0, prev_vbo = 0, prev_tex = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  GLboolean was_blend = glIsEnabled(GL_BLEND);
  GLboolean was_depth = glIsEnabled(GL_DEPTH_TEST);

  glUseProgram(prog);
  glUniform2f(u_res, (float)w, (float)h);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(V), verts.data(), GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(2 * sizeof(float)));
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDrawArrays(GL_LINES, 0, (GLsizei)verts.size());
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  // text pass
  if (!tverts.empty() && ensure_text_renderer() && text_ok) {
    glUseProgram(tprog);
    glUniform2f(tu_res, (float)w, (float)h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tatlas);
    glUniform1i(tu_tex, 0);
    glBindVertexArray(tvao);
    glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, tverts.size() * sizeof(TV), tverts.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TV), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TV), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(TV), (void*)(5 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)tverts.size());
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
  }

  // restore state
  glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
  glBindVertexArray(prev_vao);
  glUseProgram(prev_prog);
  if (!was_blend) glDisable(GL_BLEND);
  if (was_depth) glEnable(GL_DEPTH_TEST);
}

// ---- GLFW callback chaining (menu input) ----
// CRITICAL LWJGL 3 FACT: LWJGL does NOT register GLFW callbacks. It renders a
// tiny stub window and POLLS events itself, then dispatches them to Java via
// its own mechanisms (org.lwjgl.glfw.GLFW::glfwPollEvents + callback wrappers
// registered through JNI). If we return OUR interceptor from
// glfwSetKeyCallback-style installers, LWJGL never sees the game's callback
// (it doesn't call the installers for polling) AND, far worse, our trampolines
// violate LWJGL's assumption that the callback pointer is a Java-created JNI
// method handle -> InstantiationException/native crash on the first event.
//
// Therefore: only glXSwapBuffers is intercepted via dlsym. The menu gets its
// input through LWJGL's own already-registered callbacks by polling GLFW
// directly — specifically, we read the shared state that LWJGL leaves in GLFW
// via glfwGetKey/glfwGetCursorPos/glfwGetMouseButton inside the swap hook
// (cheap, lock-free, render-thread-safe). RShift/Insert toggle is polled with
// the same mechanism, so no dlsym callback fakery is needed.
// The installers below are deliberately NOT hooked anymore (kept as dead code
// reference in git history).
using KeyCbFn = void (*)(GLFWwindow*, int, int, int, int);
using CharCbFn = void (*)(GLFWwindow*, unsigned int);
using MouseCbFn = void (*)(GLFWwindow*, int, int, int);
using CursorCbFn = void (*)(GLFWwindow*, double, double);
using ScrollCbFn = void (*)(GLFWwindow*, double, double);

// GLFWwindow* is opaque here; we only ever pass stored pointers back to GLFW.
struct GLFWwindowFake; // never used

// Polled input state for the menu (render thread only).
struct PolledInput {
  bool rshift_prev = false, insert_prev = false;
  double mouse_x = 0, mouse_y = 0;
  bool mouse_prev[8] = {};
};
PolledInput g_input;

// Per-frame polled input -> ImGui. Called from draw path on the render thread
// with the real GLFW window handle cached by the swap hook.
void poll_menu_input(void* glfw_window);

// Edge-detect RShift/Insert via glfwGetKey and feed ImGui every frame.
// glfwGetKey works regardless of who installed callbacks (it reads GLFW's
// internal state), so this cannot clash with LWJGL.
namespace {
using GetKeyFn = int (*)(void*, int);
using GetCursorFn = void (*)(void*, double*, double*);
using GetMouseBtnFn = int (*)(void*, int);
GetKeyFn fn_get_key = nullptr;
GetCursorFn fn_get_cursor = nullptr;
GetMouseBtnFn fn_get_mouse = nullptr;

void ensure_glfw_input_fns() {
  if (fn_get_key) return;
  fn_get_key = reinterpret_cast<GetKeyFn>(resolve_real<GetKeyFn>("glfwGetKey"));
  fn_get_cursor = reinterpret_cast<GetCursorFn>(resolve_real<GetCursorFn>("glfwGetCursorPos"));
  fn_get_mouse = reinterpret_cast<GetMouseBtnFn>(resolve_real<GetMouseBtnFn>("glfwGetMouseButton"));
}
} // namespace

void poll_menu_input(void* win) {
  if (!win) return;
  ensure_glfw_input_fns();
  if (!fn_get_key || !fn_get_cursor || !fn_get_mouse) return;
  // GLFW_KEY_RIGHT_SHIFT=344, GLFW_KEY_INSERT=260
  bool rs = fn_get_key(win, 344) == 1;
  bool ins = fn_get_key(win, 260) == 1;
  if ((rs && !g_input.rshift_prev) || (ins && !g_input.insert_prev)) {
    cfg::get().menu_open = !cfg::get().menu_open;
    VLOG("menu", "menu %s (polled %s)", cfg::get().menu_open ? "OPEN" : "CLOSED", rs ? "RShift" : "Insert");
  }
  g_input.rshift_prev = rs;
  g_input.insert_prev = ins;
  // mouse position + buttons (hover state for ImGui even when closed)
  fn_get_cursor(win, &g_input.mouse_x, &g_input.mouse_y);
  voltage::menu::cursor_pos_event(g_input.mouse_x, g_input.mouse_y);
  for (int b = 0; b < 8; b++) {
    bool down = fn_get_mouse(win, b) == 1;
    if (down != g_input.mouse_prev[b]) voltage::menu::mouse_button_event(b, down ? 1 : 0, 0);
    g_input.mouse_prev[b] = down;
  }
  // While the menu is open the OS cursor must be visible & free so the user
  // can point at widgets. LWJGL normally keeps a disabled (captured) cursor
  // in-game. Toggle the input mode only when the state changes.
  static int last_mode = -1;
  const int want = cfg::get().menu_open ? 0 /*GLFW_CURSOR_NORMAL*/ : 1 /*GLFW_CURSOR_DISABLED*/;
  if (want != last_mode) {
    using SetInputModeFn = void (*)(void*, int, int);
    static SetInputModeFn fn_set_mode = nullptr;
    if (!fn_set_mode)
      fn_set_mode = reinterpret_cast<SetInputModeFn>(resolve_real<SetInputModeFn>("glfwSetInputMode"));
    if (fn_set_mode) {
      fn_set_mode(win, 0x00033000 /*GLFW_CURSOR*/, want);
      last_mode = want;
      VLOG("menu", "cursor mode -> %s", want == 0 ? "NORMAL" : "DISABLED");
    }
  }
  // NOTE: keyboard events while the menu is open are NOT consumed (LWJGL
  // polls them itself); the menu is mouse-driven by design, so the worst case
  // is the player walking while the window is open — same as any 2nd monitor.
}

// The GLFW window handle: captured from the first hooked glfwSwapBuffers
// call or via glfwGetCurrentContext() (LWJGL swaps through glXSwapBuffers
// with the window's drawable, but the window pointer itself is what
// glfwGetKey needs for menu polling). Namespace scope so both the swap hook
// and the direct glfwSwapBuffers override can publish it.
void* g_glfw_window = nullptr;

// True when the producer's last tick saw a non-null level (in world),
// false on the title screen / pause disconnect. This flag is independent of
// camera validity, which the producer rarely achieves (the render thread is
// the camera truth anyway).
bool in_world() {
  Snapshot& s = snapshot();
  std::lock_guard<std::mutex> l(s.m);
  return s.in_world && s.tick != 0;
}

void on_swap(const char* who) {
  if (!backend_logged) {
    backend_logged = true;
    VLOG("gl", "first swap via %s -> ESP hook live", who);
  }
  if (!g_glfw_window) {
    using GetCtxFn = void* (*)();
    static GetCtxFn fn_get_ctx = nullptr;
    if (!fn_get_ctx) fn_get_ctx = reinterpret_cast<GetCtxFn>(resolve_real<GetCtxFn>("glfwGetCurrentContext"));
    if (fn_get_ctx) g_glfw_window = fn_get_ctx();
  }
  bool world = in_world();
  if (cfg::get().menu_open && !world) {
    cfg::get().menu_open = false;  // leaving the world (menu screen) closes ClickGUI
  }
  voltage::jvm::triggerbot_tick();  // frame-synced TriggerBot (render thread)
  voltage::jvm::aimassist_tick();   // silent rotation pull (render thread)
  voltage::jvm::autosprint_tick();  // sprint hold + WTap reset
  if (world) poll_menu_input(g_glfw_window);  // RShift/Insert + mouse (in world only)
  if (world) draw_frame();
  if (cfg::get().menu_open) {
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 0 && vp[3] > 0) voltage::menu::render_frame(vp[2], vp[3]);
  }
}

} // namespace
} // namespace gl
} // namespace voltage

namespace voltage {
namespace gl {
// Defined below; returned to explicit-dlsym users (e.g. GLFW loading libGL itself).
void hooked_glXSwapBuffers(Display* d, GLXDrawable x);
int hooked_eglSwapBuffers(EGLDisplay d, EGLSurface s);
void hooked_glfwSwapBuffers(GLFWwindow* w);
// GLFW input-callback installers (menu input interception, defined below).
void* hooked_glfwSetKeyCallback(GLFWwindow*, KeyCbFn);
void* hooked_glfwSetCharCallback(GLFWwindow*, CharCbFn);
void* hooked_glfwSetMouseButtonCallback(GLFWwindow*, MouseCbFn);
void* hooked_glfwSetCursorPosCallback(GLFWwindow*, CursorCbFn);
void* hooked_glfwSetScrollCallback(GLFWwindow*, ScrollCbFn);
} // namespace gl
} // namespace voltage

// ---- dlsym interposition: catch GLFW's explicit dlopen+dlsym of swap fns ----
extern "C" __attribute__((visibility("default"))) void* dlsym(void* handle, const char* name) {
  if (!voltage::gl::real_dlsym)
    voltage::gl::ensure_real_dlsym();
  if (voltage::gl::gl_enabled() && voltage::gl::dlsym_hook_enabled() &&
      voltage::gl::is_swap_name(name)) {
    if (!std::strcmp(name, "glXSwapBuffers")) return (void*)voltage::gl::hooked_glXSwapBuffers;
    return (void*)voltage::gl::hooked_glfwSwapBuffers;
  }
  return voltage::gl::real_dlsym(handle, name);
}

namespace voltage {
namespace gl {

// Wrappers returned to explicit-dlsym users (e.g. GLFW loading libGL itself).
// Also used by the plain LD_PRELOAD overrides below.
void hooked_glXSwapBuffers(Display* d, GLXDrawable x) {
  if (!gl_enabled()) { if (!real_glx) real_glx = resolve_real<GlxSwapFn>("glXSwapBuffers"); if (real_glx) real_glx(d, x); return; }
  if (!real_glx) real_glx = resolve_real<GlxSwapFn>("glXSwapBuffers");
  on_swap("glXSwapBuffers/dlsym");
  if (real_glx) real_glx(d, x);
}

int hooked_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  if (!gl_enabled()) { if (!real_egl) real_egl = resolve_real<EglSwapFn>("eglSwapBuffers"); if (real_egl) return real_egl(d, s); return 0; }
  if (!real_egl) real_egl = resolve_real<EglSwapFn>("eglSwapBuffers");
  on_swap("eglSwapBuffers/dlsym");
  if (real_egl) return real_egl(d, s);
  return 0;
}

void hooked_glfwSwapBuffers(GLFWwindow* w) {
  // The direct override fires when LWJGL calls the global symbol: capture the
  // real window handle here too (glfwGetCurrentContext may be unresolvable
  // when libglfw is RTLD_LOCAL — menu input needs this pointer for polling).
  if (w && !g_glfw_window) {
    g_glfw_window = w;
    VLOG("gl", "captured GLFWwindow*=%p from glfwSwapBuffers", w);
  }
  if (!gl_enabled()) { if (!real_glfw) real_glfw = resolve_real<GlfwSwapFn>("glfwSwapBuffers"); if (real_glfw) real_glfw(w); return; }
  if (!real_glfw) real_glfw = resolve_real<GlfwSwapFn>("glfwSwapBuffers");
  on_swap("glfwSwapBuffers/dlsym");
  if (real_glfw) real_glfw(w);
}

// ---- GLFW callback installers: pass-through (deliberately NOT hooked) ----
// See the big comment above: faking these breaks LWJGL's callback model.
// The LD_PRELOAD overrides below forward to the real GLFW unchanged.
typedef void* (*SetKeyCbFn)(GLFWwindow*, KeyCbFn);
typedef void* (*SetCharCbFn)(GLFWwindow*, CharCbFn);
typedef void* (*SetMouseCbFn)(GLFWwindow*, MouseCbFn);
typedef void* (*SetCursorCbFn)(GLFWwindow*, CursorCbFn);
typedef void* (*SetScrollCbFn)(GLFWwindow*, ScrollCbFn);

void* hooked_glfwSetKeyCallback(GLFWwindow* w, KeyCbFn cb) {
  SetKeyCbFn real = resolve_real<SetKeyCbFn>("glfwSetKeyCallback");
  return real ? real(w, cb) : nullptr;
}
void* hooked_glfwSetCharCallback(GLFWwindow* w, CharCbFn cb) {
  SetCharCbFn real = resolve_real<SetCharCbFn>("glfwSetCharCallback");
  return real ? real(w, cb) : nullptr;
}
void* hooked_glfwSetMouseButtonCallback(GLFWwindow* w, MouseCbFn cb) {
  SetMouseCbFn real = resolve_real<SetMouseCbFn>("glfwSetMouseButtonCallback");
  return real ? real(w, cb) : nullptr;
}
void* hooked_glfwSetCursorPosCallback(GLFWwindow* w, CursorCbFn cb) {
  SetCursorCbFn real = resolve_real<SetCursorCbFn>("glfwSetCursorPosCallback");
  return real ? real(w, cb) : nullptr;
}
void* hooked_glfwSetScrollCallback(GLFWwindow* w, ScrollCbFn cb) {
  SetScrollCbFn real = resolve_real<SetScrollCbFn>("glfwSetScrollCallback");
  return real ? real(w, cb) : nullptr;
}

void on_eject() {
  // Restore vanilla cursor mode so the game is playable right after dlclose.
  if (g_glfw_window) {
    using SetInputModeFn = void (*)(void*, int, int);
    static SetInputModeFn fn = nullptr;
    if (!fn) fn = reinterpret_cast<SetInputModeFn>(resolve_real<SetInputModeFn>("glfwSetInputMode"));
    if (fn) fn(g_glfw_window, 0x00033000 /*GLFW_CURSOR*/, 1 /*DISABLED*/);
  }
  cfg::get().menu_open = false;
  cfg::get().esp_enabled = false;
  cfg::get().triggerbot_enabled = false;
  cfg::get().autosprint_enabled = false;
}

} // namespace gl
} // namespace voltage

// ---- LD_PRELOAD overrides (default visibility despite -fvisibility=hidden) ----
extern "C" __attribute__((visibility("default"))) void glfwSwapBuffers(GLFWwindow* w) {
  voltage::gl::hooked_glfwSwapBuffers(w);
}

extern "C" __attribute__((visibility("default"))) void glXSwapBuffers(Display* d, GLXDrawable x) {
  voltage::gl::hooked_glXSwapBuffers(d, x);
}

extern "C" __attribute__((visibility("default"))) int eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  return voltage::gl::hooked_eglSwapBuffers(d, s);
}

extern "C" __attribute__((visibility("default"))) void* glfwSetKeyCallback(GLFWwindow* w, void* cb) {
  if (voltage::gl::gl_enabled() && voltage::gl::dlsym_hook_enabled())
    return voltage::gl::hooked_glfwSetKeyCallback(w, (voltage::gl::KeyCbFn)cb);
  using Fn = void* (*)(GLFWwindow*, void*);
  Fn real = voltage::gl::resolve_real<Fn>("glfwSetKeyCallback");
  return real ? real(w, cb) : nullptr;
}

extern "C" __attribute__((visibility("default"))) void* glfwSetCharCallback(GLFWwindow* w, void* cb) {
  if (voltage::gl::gl_enabled() && voltage::gl::dlsym_hook_enabled())
    return voltage::gl::hooked_glfwSetCharCallback(w, (voltage::gl::CharCbFn)cb);
  using Fn = void* (*)(GLFWwindow*, void*);
  Fn real = voltage::gl::resolve_real<Fn>("glfwSetCharCallback");
  return real ? real(w, cb) : nullptr;
}

extern "C" __attribute__((visibility("default"))) void* glfwSetMouseButtonCallback(GLFWwindow* w, void* cb) {
  if (voltage::gl::gl_enabled() && voltage::gl::dlsym_hook_enabled())
    return voltage::gl::hooked_glfwSetMouseButtonCallback(w, (voltage::gl::MouseCbFn)cb);
  using Fn = void* (*)(GLFWwindow*, void*);
  Fn real = voltage::gl::resolve_real<Fn>("glfwSetMouseButtonCallback");
  return real ? real(w, cb) : nullptr;
}

extern "C" __attribute__((visibility("default"))) void* glfwSetCursorPosCallback(GLFWwindow* w, void* cb) {
  if (voltage::gl::gl_enabled() && voltage::gl::dlsym_hook_enabled())
    return voltage::gl::hooked_glfwSetCursorPosCallback(w, (voltage::gl::CursorCbFn)cb);
  using Fn = void* (*)(GLFWwindow*, void*);
  Fn real = voltage::gl::resolve_real<Fn>("glfwSetCursorPosCallback");
  return real ? real(w, cb) : nullptr;
}

extern "C" __attribute__((visibility("default"))) void* glfwSetScrollCallback(GLFWwindow* w, void* cb) {
  if (voltage::gl::gl_enabled() && voltage::gl::dlsym_hook_enabled())
    return voltage::gl::hooked_glfwSetScrollCallback(w, (voltage::gl::ScrollCbFn)cb);
  using Fn = void* (*)(GLFWwindow*, void*);
  Fn real = voltage::gl::resolve_real<Fn>("glfwSetScrollCallback");
  return real ? real(w, cb) : nullptr;
}
