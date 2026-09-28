#pragma once
// ESP math: world-to-screen. Entity polling via JNI lands here in step 3-4.
// Pure math + unit-testable, no GL/JNI dependency.
#include <cmath>

namespace voltage {
namespace esp {

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

// Column-major 4x4 MVP * world -> NDC -> screen. Returns false when behind camera.
inline bool world_to_screen(const float mvp[16], const Vec3& w,
                            int width, int height, Vec2& out) {
  float x = mvp[0]*w.x + mvp[4]*w.y + mvp[8]*w.z  + mvp[12];
  float y = mvp[1]*w.x + mvp[5]*w.y + mvp[9]*w.z  + mvp[13];
  float z = mvp[2]*w.x + mvp[6]*w.y + mvp[10]*w.z + mvp[14];
  float w_ = mvp[3]*w.x + mvp[7]*w.y + mvp[11]*w.z + mvp[15];
  if (w_ <= 0.001f) return false;
  float ndc_x = x / w_, ndc_y = y / w_;
  (void)z;
  out.x = (ndc_x * 0.5f + 0.5f) * (float)width;
  out.y = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)height;
  return true;
}

// Height of a 2D box for an entity of world-height h at screen scale s.
inline float box_height(float world_h, float scale) { return world_h * scale; }

#ifndef V_PI
#define V_PI 3.14159265358979323846
#endif

struct Cam { double x, y, z; float yaw, pitch; };  // yaw=YRot, pitch=XRot(+=down)

// Project world point using the GAME's own projection matrix (column-major, from
// RenderSystem.getProjectionMatrix) + view basis built from the MC camera.
// Returns false when behind camera.
inline bool project(const Cam& c, const float P[16], double wx, double wy, double wz,
                    int w, int h, float& sx, float& sy) {
  double yr = (double)c.yaw * V_PI / 180.0;
  double pr = (double)c.pitch * V_PI / 180.0;
  double syaw = std::sin(yr), cyaw = std::cos(yr);
  double spit = std::sin(pr), cpit = std::cos(pr);
  // forward: yaw=0 -> +Z(south), yaw=90 -> -X(west); pitch>0 -> down
  double fx = -syaw * cpit, fy = -spit, fz = cyaw * cpit;
  // right = fwd x (0,1,0) (pitch cancels -> horizontal); up = right x fwd
  double rx = -cyaw, ry = 0.0, rz = -syaw;
  double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
  double dx = wx - c.x, dy = wy - c.y, dz = wz - c.z;
  // view space: +x right, +y up, -z forward
  double vx = dx * rx + dy * ry + dz * rz;
  double vy = dx * ux + dy * uy + dz * uz;
  double vz = -(dx * fx + dy * fy + dz * fz);
  // clip = P * v (P column-major)
  double cx = P[0] * vx + P[4] * vy + P[8] * vz + P[12];
  double cy = P[1] * vx + P[5] * vy + P[9] * vz + P[13];
  double cw = P[3] * vx + P[7] * vy + P[11] * vz + P[15];
  if (cw < 0.001) return false;
  float ndc_x = (float)(cx / cw), ndc_y = (float)(cy / cw);
  sx = (ndc_x * 0.5f + 0.5f) * (float)w;
  sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)h;
  return true;
}

struct Rect { float x0, y0, x1, y1; bool ok = false; };

// 8 AABB corners -> screen-space bounding rect. False when fully behind.
inline bool box_rect(const Cam& c, const float P[16],
                     double minX, double minY, double minZ,
                     double maxX, double maxY, double maxZ,
                     int w, int h, Rect& out) {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  int n = 0;
  for (int i = 0; i < 8; i++) {
    double px = (i & 1) ? maxX : minX;
    double py = (i & 2) ? maxY : minY;
    double pz = (i & 4) ? maxZ : minZ;
    float sx, sy;
    if (!project(c, P, px, py, pz, w, h, sx, sy)) continue;
    if (sx < x0) x0 = sx;
    if (sy < y0) y0 = sy;
    if (sx > x1) x1 = sx;
    if (sy > y1) y1 = sy;
    n++;
  }
  if (n == 0) return false;
  out = {x0, y0, x1, y1, true};
  return true;
}

} // namespace esp
} // namespace voltage

