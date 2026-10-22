// SettingsIo.cpp - JSON load/save and clamping of the settings aggregate.
#include "Settings.h"

#include "Profiles.h"

namespace fb {

using json::Json;
using json::Read;

// ---------------------------------------------------------------------------
// Clamping
// ---------------------------------------------------------------------------

namespace {
int ClampKey(int vk) { return std::clamp(vk, 0, 254); }
int ClampHotkey(int vk, int fallback) {
    vk = ClampKey(vk);
    return IsAllowedHotkey(vk) ? vk : fallback;
}
}  // namespace

void Clamp(HotkeySettings& s) {
    const HotkeySettings d;
    s.startStop = ClampHotkey(s.startStop, d.startStop);
    s.pause = ClampHotkey(s.pause, d.pause);
    s.holdOverride = ClampHotkey(s.holdOverride, 0);
    s.stashNow = ClampHotkey(s.stashNow, 0);
}

void Clamp(GlobalSettings& s) {
    Clamp(s.hotkeys);
    s.safety.userActivityPauseMs = std::clamp(s.safety.userActivityPauseMs, 200, 30000);
    s.safety.overrideResumeMs = std::clamp(s.safety.overrideResumeMs, 0, 5000);
    s.safety.maxRuntimeMinutes = std::clamp(s.safety.maxRuntimeMinutes, 0, 24 * 60);
    s.safety.emergencyExitHpPct = std::clamp(s.safety.emergencyExitHpPct, 1, 90);
    s.safety.closeForeignPanelsAfterMs = std::clamp(s.safety.closeForeignPanelsAfterMs, 1000, 30000);
    s.human.actionMinIntervalMs = std::clamp(s.human.actionMinIntervalMs, 30, 1000);
    s.human.actionJitterMs = std::clamp(s.human.actionJitterMs, 0, 500);
    s.human.clickJitterPx = std::clamp(s.human.clickJitterPx, 0, 20);
    s.human.mouseSpeed = std::clamp(s.human.mouseSpeed, 0.2f, 3.0f);
    s.human.mouseCurvature = std::clamp(s.human.mouseCurvature, 0.0f, 3.0f);
    s.human.mouseTremor = std::clamp(s.human.mouseTremor, 0.0f, 3.0f);
    s.human.reaimJitterMs = std::clamp(s.human.reaimJitterMs, 0, 200);
    s.debug.statusAnchor = std::clamp(s.debug.statusAnchor, 0, 5);
    s.debug.statusScale = std::clamp(s.debug.statusScale, 0.7f, 1.6f);
    s.tickMs = std::clamp(s.tickMs, 10, 50);
}

void Clamp(FollowSettings& s) {
    if (s.leaderName.size() > 64) s.leaderName.resize(64);
    s.moveKey = ClampKey(s.moveKey);
    s.sprintKey = ClampKey(s.sprintKey);
    s.dodgeKey = ClampKey(s.dodgeKey);
    s.followDistance = std::clamp(s.followDistance, 6.f, 150.f);
    s.stopDistance = std::clamp(s.stopDistance, 2.f, 140.f);
    if (s.stopDistance > s.followDistance - 2.f) s.stopDistance = (std::max)(2.f, s.followDistance - 2.f);
    s.predictMs = std::clamp(s.predictMs, 0, 2000);
    s.formationSlot = std::clamp(s.formationSlot, 0, 5);
    s.formationSpacing = std::clamp(s.formationSpacing, 2.f, 30.f);
    s.reacquireMs = std::clamp(s.reacquireMs, 1000, 60000);
    s.lostHoldMs = std::clamp(s.lostHoldMs, 0, 10000);
    s.lookaheadCells = std::clamp(s.lookaheadCells, 4.f, 40.f);
    s.cursorDistancePx = std::clamp(s.cursorDistancePx, 60, 600);
    s.reaimMs = std::clamp(s.reaimMs, 16, 500);
    s.reaimPx = std::clamp(s.reaimPx, 1, 60);
    s.sprintDistance = std::clamp(s.sprintDistance, 10.f, 200.f);
    s.sprintNoEnemyRadius = std::clamp(s.sprintNoEnemyRadius, 0.f, 120.f);
    s.sprintMaxHoldMs = std::clamp(s.sprintMaxHoldMs, 500, 60000);
    s.sprintCooldownMs = std::clamp(s.sprintCooldownMs, 0, 30000);
    s.replanMs = std::clamp(s.replanMs, 200, 10000);
    s.astarMaxIterations = std::clamp(s.astarMaxIterations, 10000, 4000000);
    s.wallPenalty = std::clamp(s.wallPenalty, 0.f, 5.f);
    s.snapRadius = std::clamp(s.snapRadius, 1, 100);
    StuckSettings& k = s.stuck;
    k.minDisplacement = std::clamp(k.minDisplacement, 0.2f, 10.f);
    k.windowMs = std::clamp(k.windowMs, 300, 10000);
    k.noProgressMs = std::clamp(k.noProgressMs, 500, 20000);
    k.ladderResetMs = std::clamp(k.ladderResetMs, 500, 30000);
    k.blockRadius = std::clamp(k.blockRadius, 0, 8);
    k.blockMs = std::clamp(k.blockMs, 1000, 120000);
    k.sideStepMs = std::clamp(k.sideStepMs, 100, 3000);
    k.doorSearchCells = std::clamp(k.doorSearchCells, 2.f, 40.f);
    k.backtrackMinCells = std::clamp(k.backtrackMinCells, 2.f, 60.f);
    k.ignitionTimeoutMs = std::clamp(k.ignitionTimeoutMs, 0, 30000);
    k.followRetryMs = std::clamp(k.followRetryMs, 500, 30000);
}

void Clamp(Profile& p) {
    if (p.name.empty()) p.name = "Default";
    if (p.name.size() > 48) p.name.resize(48);
    if (p.character.size() > 64) p.character.resize(64);
    Clamp(p.follow);
    Clamp(p.travel);
    Clamp(p.loot);
    Clamp(p.skills);
    Clamp(p.stash);
    Clamp(p.death);
}

void Clamp(Settings& s) {
    s.version = kSettingsVersion;
    Clamp(s.global);
    if (s.profiles.size() > 32) s.profiles.resize(32);
    for (auto& p : s.profiles) Clamp(p);
    EnsureProfiles(s);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

void ToJson(Json& j, const GlobalSettings& s) {
    j = Json::object();
    j["tick_ms"] = s.tickMs;
    j["hotkeys"] = {{"start_stop", s.hotkeys.startStop},
                    {"pause", s.hotkeys.pause},
                    {"hold_override", s.hotkeys.holdOverride},
                    {"stash_now", s.hotkeys.stashNow}};
    j["safety"] = {{"user_activity_pause", s.safety.userActivityPause},
                   {"user_activity_pause_ms", s.safety.userActivityPauseMs},
                   {"override_resume_ms", s.safety.overrideResumeMs},
                   {"max_runtime_minutes", s.safety.maxRuntimeMinutes},
                   {"emergency_exit", s.safety.emergencyExit},
                   {"emergency_exit_hp_pct", s.safety.emergencyExitHpPct},
                   {"close_foreign_panels", s.safety.closeForeignPanels},
                   {"close_foreign_panels_after_ms", s.safety.closeForeignPanelsAfterMs}};
    j["human"] = {{"action_min_interval_ms", s.human.actionMinIntervalMs},
                  {"action_jitter_ms", s.human.actionJitterMs},
                  {"click_jitter_px", s.human.clickJitterPx},
                  {"human_mouse", s.human.humanMouse},
                  {"mouse_speed", s.human.mouseSpeed},
                  {"mouse_curvature", s.human.mouseCurvature},
                  {"mouse_tremor", s.human.mouseTremor},
                  {"reaim_jitter_ms", s.human.reaimJitterMs}};
    j["debug"] = {{"overlay", s.debug.overlay},
                  {"show_path", s.debug.showPath},
                  {"show_lookahead", s.debug.showLookahead},
                  {"show_cursor", s.debug.showCursor},
                  {"show_leader", s.debug.showLeader},
                  {"show_route", s.debug.showRoute},
                  {"show_circles", s.debug.showCircles},
                  {"show_interactables", s.debug.showInteractables},
                  {"show_loot", s.debug.showLoot},
                  {"show_combat", s.debug.showCombat},
                  {"show_stuck", s.debug.showStuck},
                  {"status_window", s.debug.statusWindow},
                  {"status_anchor", s.debug.statusAnchor},
                  {"status_scale", s.debug.statusScale},
                  {"log_to_file", s.debug.logToFile}};
}

void FromJson(const Json& j, GlobalSettings& s) {
    Read(j, "tick_ms", s.tickMs);
    const Json& h = json::Child(j, "hotkeys");
    Read(h, "start_stop", s.hotkeys.startStop);
    Read(h, "pause", s.hotkeys.pause);
    Read(h, "hold_override", s.hotkeys.holdOverride);
    Read(h, "stash_now", s.hotkeys.stashNow);
    const Json& sf = json::Child(j, "safety");
    Read(sf, "user_activity_pause", s.safety.userActivityPause);
    Read(sf, "user_activity_pause_ms", s.safety.userActivityPauseMs);
    Read(sf, "override_resume_ms", s.safety.overrideResumeMs);
    Read(sf, "max_runtime_minutes", s.safety.maxRuntimeMinutes);
    Read(sf, "emergency_exit", s.safety.emergencyExit);
    Read(sf, "emergency_exit_hp_pct", s.safety.emergencyExitHpPct);
    Read(sf, "close_foreign_panels", s.safety.closeForeignPanels);
    Read(sf, "close_foreign_panels_after_ms", s.safety.closeForeignPanelsAfterMs);
    const Json& hu = json::Child(j, "human");
    Read(hu, "action_min_interval_ms", s.human.actionMinIntervalMs);
    Read(hu, "action_jitter_ms", s.human.actionJitterMs);
    Read(hu, "click_jitter_px", s.human.clickJitterPx);
    Read(hu, "human_mouse", s.human.humanMouse);
    Read(hu, "mouse_speed", s.human.mouseSpeed);
    Read(hu, "mouse_curvature", s.human.mouseCurvature);
    Read(hu, "mouse_tremor", s.human.mouseTremor);
    Read(hu, "reaim_jitter_ms", s.human.reaimJitterMs);
    const Json& d = json::Child(j, "debug");
    Read(d, "overlay", s.debug.overlay);
    Read(d, "show_path", s.debug.showPath);
    Read(d, "show_lookahead", s.debug.showLookahead);
    Read(d, "show_cursor", s.debug.showCursor);
    Read(d, "show_leader", s.debug.showLeader);
    Read(d, "show_route", s.debug.showRoute);
    Read(d, "show_circles", s.debug.showCircles);
    Read(d, "show_interactables", s.debug.showInteractables);
    Read(d, "show_loot", s.debug.showLoot);
    Read(d, "show_combat", s.debug.showCombat);
    Read(d, "show_stuck", s.debug.showStuck);
    Read(d, "status_window", s.debug.statusWindow);
    Read(d, "status_anchor", s.debug.statusAnchor);
    Read(d, "status_scale", s.debug.statusScale);
    Read(d, "log_to_file", s.debug.logToFile);
    Clamp(s);
}

void ToJson(Json& j, const FollowSettings& s) {
    j = Json::object();
    j["leader_name"] = s.leaderName;
    j["leader_auto"] = s.leaderAuto;
    j["leader_case_insensitive"] = s.leaderCaseInsensitive;
    j["move_key"] = s.moveKey;
    j["click_to_move"] = s.clickToMove;
    j["sprint_key"] = s.sprintKey;
    j["dodge_key"] = s.dodgeKey;
    j["follow_distance"] = s.followDistance;
    j["stop_distance"] = s.stopDistance;
    j["predict"] = s.predict;
    j["predict_ms"] = s.predictMs;
    j["follow_destination"] = s.followDestination;
    j["formation_slot"] = s.formationSlot;
    j["formation_spacing"] = s.formationSpacing;
    j["follow_in_town_hideout"] = s.followInTownHideout;
    j["reacquire_ms"] = s.reacquireMs;
    j["lost_hold_ms"] = s.lostHoldMs;
    j["lookahead_cells"] = s.lookaheadCells;
    j["cursor_distance_px"] = s.cursorDistancePx;
    j["reaim_ms"] = s.reaimMs;
    j["reaim_px"] = s.reaimPx;
    j["activation_click"] = s.activationClick;
    j["sprint"] = s.sprint;
    j["sprint_distance"] = s.sprintDistance;
    j["sprint_no_enemy_radius"] = s.sprintNoEnemyRadius;
    j["sprint_max_hold_ms"] = s.sprintMaxHoldMs;
    j["sprint_cooldown_ms"] = s.sprintCooldownMs;
    j["replan_ms"] = s.replanMs;
    j["astar_max_iterations"] = s.astarMaxIterations;
    j["wall_penalty"] = s.wallPenalty;
    j["snap_radius"] = s.snapRadius;
    const StuckSettings& k = s.stuck;
    j["stuck"] = {{"min_displacement", k.minDisplacement},
                  {"window_ms", k.windowMs},
                  {"no_progress_ms", k.noProgressMs},
                  {"ladder_reset_ms", k.ladderResetMs},
                  {"block_radius", k.blockRadius},
                  {"block_ms", k.blockMs},
                  {"side_step_ms", k.sideStepMs},
                  {"door_search_cells", k.doorSearchCells},
                  {"backtrack_min_cells", k.backtrackMinCells},
                  {"ignition_timeout_ms", k.ignitionTimeoutMs},
                  {"follow_retry_ms", k.followRetryMs}};
}

void FromJson(const Json& j, FollowSettings& s) {
    Read(j, "leader_name", s.leaderName);
    Read(j, "leader_auto", s.leaderAuto);
    Read(j, "leader_case_insensitive", s.leaderCaseInsensitive);
    Read(j, "move_key", s.moveKey);
    Read(j, "click_to_move", s.clickToMove);
    Read(j, "sprint_key", s.sprintKey);
    Read(j, "dodge_key", s.dodgeKey);
    Read(j, "follow_distance", s.followDistance);
    Read(j, "stop_distance", s.stopDistance);
    Read(j, "predict", s.predict);
    Read(j, "predict_ms", s.predictMs);
    Read(j, "follow_destination", s.followDestination);
    Read(j, "formation_slot", s.formationSlot);
    Read(j, "formation_spacing", s.formationSpacing);
    Read(j, "follow_in_town_hideout", s.followInTownHideout);
    Read(j, "reacquire_ms", s.reacquireMs);
    Read(j, "lost_hold_ms", s.lostHoldMs);
    Read(j, "lookahead_cells", s.lookaheadCells);
    Read(j, "cursor_distance_px", s.cursorDistancePx);
    Read(j, "reaim_ms", s.reaimMs);
    Read(j, "reaim_px", s.reaimPx);
    Read(j, "activation_click", s.activationClick);
    Read(j, "sprint", s.sprint);
    Read(j, "sprint_distance", s.sprintDistance);
    Read(j, "sprint_no_enemy_radius", s.sprintNoEnemyRadius);
    Read(j, "sprint_max_hold_ms", s.sprintMaxHoldMs);
    Read(j, "sprint_cooldown_ms", s.sprintCooldownMs);
    Read(j, "replan_ms", s.replanMs);
    Read(j, "astar_max_iterations", s.astarMaxIterations);
    Read(j, "wall_penalty", s.wallPenalty);
    Read(j, "snap_radius", s.snapRadius);
    const Json& k = json::Child(j, "stuck");
    Read(k, "min_displacement", s.stuck.minDisplacement);
    Read(k, "window_ms", s.stuck.windowMs);
    Read(k, "no_progress_ms", s.stuck.noProgressMs);
    Read(k, "ladder_reset_ms", s.stuck.ladderResetMs);
    Read(k, "block_radius", s.stuck.blockRadius);
    Read(k, "block_ms", s.stuck.blockMs);
    Read(k, "side_step_ms", s.stuck.sideStepMs);
    Read(k, "door_search_cells", s.stuck.doorSearchCells);
    Read(k, "backtrack_min_cells", s.stuck.backtrackMinCells);
    Read(k, "ignition_timeout_ms", s.stuck.ignitionTimeoutMs);
    Read(k, "follow_retry_ms", s.stuck.followRetryMs);
    Clamp(s);
}

void ToJson(Json& j, const Profile& p) {
    j = Json::object();
    j["name"] = p.name;
    j["character"] = p.character;
    Json f, t, l, sk, st, d;
    ToJson(f, p.follow);
    ToJson(t, p.travel);
    ToJson(l, p.loot);
    ToJson(sk, p.skills);
    ToJson(st, p.stash);
    ToJson(d, p.death);
    j["follow"] = std::move(f);
    j["travel"] = std::move(t);
    j["loot"] = std::move(l);
    j["skills"] = std::move(sk);
    j["stash"] = std::move(st);
    j["death"] = std::move(d);
}

void FromJson(const Json& j, Profile& p) {
    Read(j, "name", p.name);
    Read(j, "character", p.character);
    FromJson(json::Child(j, "follow"), p.follow);
    FromJson(json::Child(j, "travel"), p.travel);
    FromJson(json::Child(j, "loot"), p.loot);
    FromJson(json::Child(j, "skills"), p.skills);
    FromJson(json::Child(j, "stash"), p.stash);
    FromJson(json::Child(j, "death"), p.death);
    Clamp(p);
}

Json SettingsToJson(const Settings& s) {
    Json j = Json::object();
    j["version"] = kSettingsVersion;
    Json g;
    ToJson(g, s.global);
    j["global"] = std::move(g);
    j["activeProfile"] = s.activeProfile;
    j["autoSelectByCharacter"] = s.autoSelectByCharacter;
    Json profiles = Json::array();
    for (const auto& p : s.profiles) {
        Json jp;
        ToJson(jp, p);
        profiles.push_back(std::move(jp));
    }
    j["profiles"] = std::move(profiles);
    return j;
}

void SettingsFromJson(const Json& j, Settings& s) {
    FromJson(json::Child(j, "global"), s.global);
    Read(j, "activeProfile", s.activeProfile);
    Read(j, "autoSelectByCharacter", s.autoSelectByCharacter);
    const Json& arr = json::ChildArray(j, "profiles");
    if (!arr.empty()) {
        std::vector<Profile> loaded;
        for (const auto& jp : arr) {
            if (!jp.is_object()) continue;
            Profile p;
            FromJson(jp, p);
            loaded.push_back(std::move(p));
        }
        if (!loaded.empty()) s.profiles = std::move(loaded);
    }
    Clamp(s);
}

std::filesystem::path SettingsFilePath(const std::filesystem::path& pluginDir) {
    return pluginDir / "config" / "settings.json";
}

io::LoadStatus LoadSettingsFile(const std::filesystem::path& file, Settings& out) {
    std::string text;
    const io::ReadStatus rs = io::ReadTextFileChecked(file, text);
    if (rs == io::ReadStatus::Missing) return io::LoadStatus::Missing;
    if (rs == io::ReadStatus::Unreadable) return io::LoadStatus::Unreadable;
    const Json j = json::Parse(text);
    if (!j.is_object()) {
        io::KeepCorruptCopy(file);
        return io::LoadStatus::Corrupt;
    }
    Settings fresh;
    SettingsFromJson(j, fresh);
    out = std::move(fresh);
    return io::LoadStatus::Loaded;
}

bool SaveSettingsFile(const std::filesystem::path& file, const Settings& s) {
    if (file.empty()) return false;
    return io::WriteTextFileAtomic(file, json::Dump(SettingsToJson(s)));
}

}  // namespace fb
