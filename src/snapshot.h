#pragma once
// Shared snapshot: jvm thread (producer) -> render thread (consumer in swap hook).
#include <cstdint>
#include <mutex>
#include <vector>

namespace voltage {

struct SnapEnt {
  double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
  bool player = false;
  bool alive = false;
  bool self = false;  // the local player (skipped in first person)
  bool living = false;  // LivingEntity (aim/trigger targets, not items/projectiles)
  double vx = 0, vy = 0, vz = 0;  // velocity blocks/sec (aim prediction)
  char name[64] = "-";
};

struct SnapCam {
  double x = 0, y = 0, z = 0;
  float yaw = 0, pitch = 0, fov = 70.0f;  // yaw=YRot, pitch=XRot, fov=vertical degrees (log only)
  // True game projection matrix (RenderSystem.getProjectionMatrix()), column-major.
  float proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  bool proj_ok = false;
  bool valid = false;
};

struct Snapshot {
  std::mutex m;
  std::vector<SnapEnt> ents;
  SnapCam cam;
  uint64_t tick = 0;      // increments each producer update
  bool in_world = false;  // producer saw level != null on the last tick
  uint64_t snap_ms = 0;   // steady_clock ms of last publish (aim extrapolation)
};

inline Snapshot& snapshot() { static Snapshot s; return s; }

} // namespace voltage
