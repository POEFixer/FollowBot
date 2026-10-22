// Settings.h - the settings aggregate (design §5.14).
//
// <plugin dir>/config/settings.json:
//   { "version": 1, "global": {hotkeys, safety, humanisation, debug},
//     "activeProfile": "Default", "autoSelectByCharacter": true,
//     "profiles": [ { "name", "character", follow, travel, loot, skills, stash, death } ] }
//
// The core (follow / movement / navigation / stuck) settings live here; each
// feature module owns its struct and JSON in its own directory and this file
// only aggregates them. The worker gets an immutable shared_ptr<const Settings>
// on every change.
#pragma once

#include "../combat/SkillSettings.h"
#include "../common/FileUtil.h"
#include "../common/JsonUtil.h"
#include "../death/DeathSettings.h"
#include "../loot/LootSettings.h"
#include "../stash/StashSettings.h"
#include "../travel/TravelSettings.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fb {

inline constexpr int kSettingsVersion = 1;

struct HotkeySettings {
    int startStop = 0x77;      // F8
    int pause = 0x13;          // Pause (panic pause)
    int holdOverride = 0;      // while held the bot holds still
    int stashNow = 0;          // manual stash run
};

struct SafetySettings {
    bool userActivityPause = true;    // your own mouse / keyboard input in the game pauses the bot
    int  userActivityPauseMs = 1500;
    int  overrideResumeMs = 600;      // resume this long after the override key is released
    int  maxRuntimeMinutes = 0;       // stop after this long (0 = off)
    bool emergencyExit = false;       // type /exit once when HP falls below the threshold in a map
    int  emergencyExitHpPct = 20;
    bool closeForeignPanels = false;  // close a panel the bot did not open with Esc ...
    int  closeForeignPanelsAfterMs = 3000;  // ... after it stayed open this long
};

struct HumanSettings {
    int   actionMinIntervalMs = 100;  // global action gate: min gap between presses / clicks
    int   actionJitterMs = 60;        // + uniform 0..jitter
    int   clickJitterPx = 4;          // centre-biased click offset radius
    bool  humanMouse = true;          // human-like cursor path for clicks (never for move steering)
    float mouseSpeed = 0.45f;         // trajectory speed factor (lower = faster)
    float mouseCurvature = 0.25f;     // 0..3
    float mouseTremor = 0.2f;         // 0..3
    int   reaimJitterMs = 20;         // random extra to the move re-aim interval
};

struct DebugSettings {
    bool  overlay = false;            // world debug overlay
    bool  showPath = true;
    bool  showLookahead = true;
    bool  showCursor = true;
    bool  showLeader = true;
    bool  showRoute = true;
    bool  showCircles = true;
    bool  showInteractables = true;
    bool  showLoot = true;
    bool  showCombat = false;
    bool  showStuck = true;
    bool  statusWindow = true;        // small in-game status panel
    int   statusAnchor = 0;           // 0 top-left, 1 top-centre, 2 top-right, 3 bottom-left, 4 bottom-centre, 5 bottom-right
    float statusScale = 1.0f;
    bool  logToFile = false;          // decision log to <plugin>/logs/followbot-<date>.log
};

struct GlobalSettings {
    HotkeySettings hotkeys;
    SafetySettings safety;
    HumanSettings  human;
    DebugSettings  debug;
    int            tickMs = 16;       // worker tick (10..50)
};

struct StuckSettings {
    float minDisplacement = 1.5f;     // cells moved within windowMs below this = stuck
    int   windowMs = 1200;
    int   noProgressMs = 2500;        // no progress toward the goal for this long = stuck
    int   ladderResetMs = 4000;       // good progress this long resets the recovery ladder
    int   blockRadius = 2;            // temporary block on the cell ahead (cells)
    int   blockMs = 20000;
    int   sideStepMs = 450;
    float doorSearchCells = 12.f;
    float backtrackMinCells = 8.f;
    int   ignitionTimeoutMs = 7000;   // after a zone-in, stuck checks wait for the first move or this long
    int   followRetryMs = 3000;       // Follow gives up a goal for this long, then tries again
};

struct FollowSettings {
    // Leader.
    std::string leaderName;           // character name (UTF-8)
    bool  leaderAuto = false;         // the only other player in the area, when exactly one is present
    bool  leaderCaseInsensitive = true;
    // Keys.
    int   moveKey = 'T';              // bind "Move Only" to it in the game's options
    bool  clickToMove = false;        // hold the left mouse button instead of the move key
    int   sprintKey = 0;              // Dodge / Sprint (held = sprint)
    int   dodgeKey = 0;               // dodge-only key (stuck recovery); 0 = tap the sprint key
    // Following.
    float followDistance = 28.f;      // start moving when farther than this (cells)
    float stopDistance = 16.f;        // stop when closer than this
    bool  predict = true;             // aim where he will be
    int   predictMs = 350;
    bool  followDestination = false;  // aim at the end of his route while he moves
    int   formationSlot = 0;          // 0 off, 1..5: place among several followers
    float formationSpacing = 6.f;
    bool  followInTownHideout = true;
    int   reacquireMs = 8000;         // after our own area change, wait this long for him to appear
    int   lostHoldMs = 1500;          // keep the last goal this long when he just left sight
    // Movement.
    float lookaheadCells = 14.f;
    int   cursorDistancePx = 180;
    int   reaimMs = 50;
    int   reaimPx = 6;
    bool  activationClick = true;     // one click below the feet after focus / zone-in
    bool  sprint = true;
    float sprintDistance = 55.f;
    float sprintNoEnemyRadius = 35.f;
    int   sprintMaxHoldMs = 8000;
    int   sprintCooldownMs = 3000;
    // Navigation.
    int   replanMs = 1500;
    int   astarMaxIterations = 400000;
    float wallPenalty = 1.0f;         // scales the clearance cost (0 = shortest path, hugs walls)
    int   snapRadius = 30;
    StuckSettings stuck;
};

struct Profile {
    std::string    name = "Default";
    std::string    character;         // auto-selected for this character name (empty = none)
    FollowSettings follow;
    TravelSettings travel;
    LootSettings   loot;
    SkillSettings  skills;
    StashSettings  stash;
    DeathSettings  death;
};

struct Settings {
    int                  version = kSettingsVersion;
    GlobalSettings       global;
    std::string          activeProfile = "Default";
    bool                 autoSelectByCharacter = true;
    std::vector<Profile> profiles{Profile{}};
};

// Clamping (also applied after every load).
void Clamp(HotkeySettings& s);
void Clamp(GlobalSettings& s);
void Clamp(FollowSettings& s);
void Clamp(Profile& p);
void Clamp(Settings& s);

// JSON: per-field fallbacks (a missing or malformed field keeps its default).
void ToJson(json::Json& j, const GlobalSettings& s);
void FromJson(const json::Json& j, GlobalSettings& s);
void ToJson(json::Json& j, const FollowSettings& s);
void FromJson(const json::Json& j, FollowSettings& s);
void ToJson(json::Json& j, const Profile& p);
void FromJson(const json::Json& j, Profile& p);
json::Json SettingsToJson(const Settings& s);
void SettingsFromJson(const json::Json& j, Settings& s);

std::filesystem::path SettingsFilePath(const std::filesystem::path& pluginDir);

// Missing -> defaults (may be written); Unreadable / Corrupt -> defaults and the
// file is kept (Corrupt also copied to settings.json.corrupt); hold writes with
// io::SaveGuard.
io::LoadStatus LoadSettingsFile(const std::filesystem::path& file, Settings& out);
bool SaveSettingsFile(const std::filesystem::path& file, const Settings& s);

// A hotkey that may start / pause the bot: never the left or right mouse button
// (the bot's own clicks would trigger it).
inline bool IsAllowedHotkey(int vk) { return vk != 0x01 && vk != 0x02; }

}  // namespace fb
