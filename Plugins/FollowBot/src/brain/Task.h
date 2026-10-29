// Task.h - the shared vocabulary of the brain and every task (design §5.2).
//
// A task is one exclusive activity (Travel, Stash trip, Pickup, Death, a debug
// walk) or the default Follow. The Brain ticks the active one; every task gets
// a BotContext with this tick's World and the movement / input machinery.
// Decision code reads only World; SDK reads happen in adapters (WorldBuilder,
// LootScanner, DeathTask / StashTask internals) through `sdk`.
#pragma once

#include "../config/Settings.h"
#include "../core/LocText.h"
#include "../world/World.h"
#include "Gates.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace PluginSDK {
struct Context;   // PluginSDK.h: only adapter .cpp files include it
}

namespace fb {

class Actuator;
class BotLog;
class Mover;
class PathPlanner;
class StuckRecovery;

enum class TaskStatus : uint8_t { Running = 0, Done, Failed };

// The status's key (lang/en.json).
inline const char* TaskStatusName(TaskStatus s) {
    switch (s) {
        case TaskStatus::Running: return Key("task_status_running");
        case TaskStatus::Done:    return Key("task_status_done");
        case TaskStatus::Failed:  return Key("task_status_failed");
    }
    return Key("common_unknown");
}

// What a skill / flask cast does to the cursor and the character: a task says
// which ones may interleave with it (a hover before a click allows none that
// move the cursor).
enum class CastClass : uint8_t { Self = 0, Flask, Attack, Movement };

// Reads the decision code asks the WorldBuilder for. Tasks set them every tick
// they need them; the Brain clears them before each tick.
struct WorldRequests {
    bool inventoryFast = false;     // scan the inventories every ~200 ms (stash, loot)
    bool skills = false;            // EnumerateActiveSkills every 250 ms
    bool selfBuffs = false;         // own buffs every 250 ms
    bool leaderBuffs = false;       // leader buffs every 500 ms
    bool flasks = false;            // belt flasks / charms
    bool interactablesNow = false;  // refresh interactables on the next tick
    bool groundItems = false;       // ground items every 150 ms (else every 500 ms)

    void Clear() { *this = WorldRequests{}; }
};

// Memory shared between tasks and kept across ticks (reset on Start).
struct BotMemory {
    uint64_t startedMs = 0;
    // Area tracking.
    uint32_t area = 0;
    bool     haveArea = false;
    uint64_t areaEnteredMs = 0;
    std::string areaId;
    // Deaths.
    int      deathsSession = 0;
    int      deathsMap = 0;           // in the current map (reset when a new map is entered)
    uint64_t lastDeathMs = 0;
    // Loot: entity id -> blacklisted until (ms). Ids are per area: cleared on area change.
    std::unordered_map<uint32_t, uint64_t> lootBlacklist;
    bool     inventoryFull = false;   // a wanted item found no room
    // Stash.
    bool     manualStashRequested = false;
    // A stash run left items behind (the tab looked full, or a routed tab could
    // not be used and the open tab is not used instead): automatic runs pause -
    // another one would only repeat it (a portal per trip, the leader left each
    // time) - until a manual run moves items with nothing left behind, the
    // backpack shrinks below stashBlockedUsedCells, or Start.
    bool     stashBlocked = false;
    int      stashBlockedUsedCells = 0;
    struct StashTrip {
        bool        active = false;
        uint32_t    originArea = 0;
        std::string originAreaId;
        Vec2        originGrid;
        uint64_t    startedMs = 0;
    } trip;
    // Follow.
    uint64_t leaderIdleSinceMs = 0;   // the leader has not moved since (0 = moving / unknown)
    // Requests from tasks to the plugin.
    bool        stopRequested = false;   // stop the bot (death limit, emergency exit, ...)
    LocText     stopReason;
    LocText     lastError;
    std::string currentRule;             // the last skill rule that fired (its user label)

    bool IsBlacklisted(uint32_t id, uint64_t now) const {
        const auto it = lootBlacklist.find(id);
        return it != lootBlacklist.end() && now < it->second;
    }
    void Blacklist(uint32_t id, uint64_t until) { lootBlacklist[id] = until; }
    void RequestStop(LocText why) {
        stopRequested = true;
        stopReason = std::move(why);
    }
};

// A point a task wants drawn on the debug overlay.
struct TaskMarker {
    Vec2        grid;
    uint32_t    color = 0xFF00FFFFu;   // ImGui ABGR
    const char* label = "";            // a key (lang/en.json), "" = none
};

// What the running task reports this tick (status window, overlay).
struct TaskReport {
    LocText     detail;               // one line: what it does / why it waits
    bool        hasTarget = false;
    Vec2        target;
    std::vector<TaskMarker> markers;

    void Clear() {
        detail.clear();
        hasTarget = false;
        markers.clear();
    }
};

// Small host reads pure helpers need, provided by the plugin (adapters).
struct HostHooks {
    // Targetable.IsTargetedByPlayer of a Targetable component address: the hover
    // has settled on that object. Unset = unknown (the click goes ahead).
    std::function<bool(uintptr_t targetableAddr)> isTargeted;
};

struct BotContext {
    const World&    w;
    Actuator&       act;
    Mover&          mover;
    PathPlanner&    planner;
    StuckRecovery&  stuck;
    const Settings& s;
    const Profile&  p;            // the profile in use (auto-selected by character)
    BotLog&         log;
    BotMemory&      mem;
    WorldRequests&  req;
    TaskReport&     report;
    const HostHooks& hooks;
    const PluginSDK::Context* sdk = nullptr;   // adapters only; null in tests
    uint64_t        now = 0;
    bool            cursorBusy = false;        // another plugin owns the cursor this tick
};

class Task {
public:
    virtual ~Task() = default;
    // The task's key (lang/en.json): logs and the UI translate it.
    virtual const char* Name() const = 0;
    virtual TaskStatus Tick(BotContext& ctx) = 0;
    // Stop now: release input, forget transient state. Called on preemption,
    // gate holds (unless KeepThroughHold) and stop.
    virtual void Abort(BotContext& ctx) = 0;
    virtual bool AllowsCasts(CastClass c) const = 0;
    // True to survive a hold of this kind and resume afterwards (travel waits
    // through the loading screen). Input is released during any hold.
    virtual bool KeepThroughHold(GateReason r) const {
        (void)r;
        return false;
    }
};

}  // namespace fb
