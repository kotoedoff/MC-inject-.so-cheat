#pragma once
// Feature toggles shared by all modules. Flipped by the ImGui menu (menu.cpp),
// persisted to config.json.
namespace voltage {
namespace cfg {

struct Config {
  // ESP
  bool esp_enabled = true;    // master switch
  bool esp_boxes = true;      // step 4: draw boxes
  bool esp_names = true;      // name + distance text above boxes
  bool esp_health = false;    // later: draw hp
  float esp_max_dist = 64.0f;
  // TriggerBot (roadmap phase 2)
  bool triggerbot_enabled = false;
  float triggerbot_cooldown = 0.93f;  // getAttackCooldownProgress(0.5f) >= this
  float triggerbot_range = 3.0f;      // meters, entity pos -> player pos
  bool triggerbot_only_crits = false; // vanilla crit rule: falling + airborne + dry + no sprint
  // AimAssist: silent rotation pull, movement keys untouched (free strafe)
  bool aimassist_enabled = false;
  float aimassist_range = 6.0f;       // meters to target
  float aimassist_speed = 5.0f;       // 1..20, base pull factor (ref: speed/10*0.3)
  float aimassist_smooth = 7.0f;      // 1..20, damping divider (ref: 1+smooth/10*2)
  float aimassist_fov = 90.0f;      // engage cone, degrees (angle <= fov/2)
  bool aimassist_players = true;
  bool aimassist_mobs = false;
  // AutoSprint: always sprint while moving forward; legit sprint-reset on hit
  bool autosprint_enabled = false;
  bool autosprint_wtap = true;        // release sprint on attack (+ optional hop)
  bool autosprint_jump = false;       // small jump on the attack (WTap-lite)
  int  autosprint_resume_ticks = 4;   // ticks before sprint re-engages
  // ClickGUI (roadmap phase 3)
  bool menu_open = false;             // toggled by RShift via hooked key callback
  bool menu_loaded_config = false;    // internal: config.json applied once
};

inline Config& get() { static Config c; return c; }

} // namespace cfg
} // namespace voltage
