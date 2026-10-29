// PickupTask.h - pick up wanted ground items (design §5.9). Exclusive task,
// started by the Brain at step 7.
//
// Contract (this interface is final):
//  - WantsToStart: loot enabled (or a quest item), not in town / hideout unless
//    lootInTownHideout, the leader within maxLeaderDistance and the item within
//    that distance of the leader, no hostile within enemyRadius unless
//    lootDuringCombat, a candidate that `fits` (else mem.inventoryFull = true,
//    which feeds the stash trigger). Updates the scanner (requests
//    req.groundItems / req.inventoryFast as needed).
//  - Tick: WorldClick on the item (pickupRange, label offset labelOffsetPx up),
//    wait for the entity to vanish (vanishWaitMs, extended to
//    vanishWaitMovingMs while still walking toward it); success -> next item
//    or Done; failure -> attempts++ (maxAttempts) -> mem.Blacklist(id,
//    blacklistMs). Max continuous time maxContinuousMs, then Done (yield to
//    Follow). Abort itself (Failed) when the leader is farther than
//    maxLeaderDistance + preemptLeaderExtra, or enemies appear (unless allowed).
//  - AllowsCasts: Flask always; Self / Attack only while approaching.
//
// Details: without a configured leader the leader rules do not apply; with one
// he must be in sight. "No hostile within enemyRadius" is checked around us and
// around the item. An unreachable item (or one we got stuck on the way to) is
// blacklisted at once; a refused input (the game lost focus, a stop is pending)
// costs no attempt. After yielding (max continuous time) the task does not
// start again for yieldCooldownMs, after a preemption for 1 s. The
// inventory-full flag is set when a wanted item finds no room and cleared once
// the backpack has more free cells than at that moment (stashed or used up).
#pragma once

#include "../brain/Task.h"
#include "../travel/WorldClick.h"
#include "LootScanner.h"

#include <atomic>
#include <string>
#include <unordered_map>

namespace fb {

// The vanish check after a pickup click. Pure (tested).
class VanishWatch {
public:
    enum class Result : uint8_t { Pending = 0, Vanished, TimedOut };

    // An empty item list may be a failed read rather than "everything picked
    // up": an item missing from an empty list must stay missing this long.
    static constexpr uint64_t kEmptyListConfirmMs = 300;
    // After the last step toward the item, the pickup gets this long to land.
    static constexpr uint64_t kAfterApproachMs = 400;
    // A step toward the item: the distance shrank by at least this much.
    static constexpr float kApproachStepCells = 0.2f;

    void Start(uint64_t clickMs, float distToItem, int waitMs, int movingWaitMs);
    Result Update(uint64_t now, bool present, bool listEmpty, float distToItem);
    uint64_t Deadline() const { return m_deadlineMs; }
    bool Approaching(uint64_t now) const { return m_lastApproachMs != 0 && now - m_lastApproachMs <= kAfterApproachMs; }

private:
    uint64_t m_clickMs = 0;
    uint64_t m_deadlineMs = 0;
    uint64_t m_capMs = 0;
    uint64_t m_lastApproachMs = 0;
    uint64_t m_absentSinceMs = 0;
    float    m_minDist = 0.f;
};

class PickupTask : public Task {
public:
    const char* Name() const override { return Key("task_pickup"); }

    bool WantsToStart(BotContext& ctx);
    void Start(BotContext& ctx);
    TaskStatus Tick(BotContext& ctx) override;
    void Abort(BotContext& ctx) override;
    bool AllowsCasts(CastClass c) const override;

    // Keeps the scanner (and its Diagnostics) live while the bot holds or is
    // stopped; does nothing unless the settings tab asked for Diagnostics
    // within the last second. The Brain calls it every tick before the gates.
    void Observe(BotContext& ctx);

    LootScanner& Scanner() { return m_scanner; }
    const LootScanner& Scanner() const { return m_scanner; }

    // Why the last run ended (empty while running / after a normal Done).
    const LocText& FailReason() const { return m_failReason; }
    // What the loot logic does or why it does not loot right now: a key (a
    // static string; safe to read from the render thread).
    const char* Status() const { return m_status.load(std::memory_order_relaxed); }
    // Items picked up since the bot started (render thread: atomic).
    int PickedTotal() const { return m_pickedTotal.load(std::memory_order_relaxed); }

private:
    enum class Phase : uint8_t { Choose = 0, Click, WaitVanish };

    void TrackArea(BotContext& ctx);
    void UpdateFullFlag(BotContext& ctx);
    const char* BlockReason(const BotContext& ctx) const;
    const char* PreemptReason(const BotContext& ctx) const;
    const LootCandidate* ChooseTarget(const BotContext& ctx, bool* anyNoRoom) const;
    bool StillWanted(const BotContext& ctx) const;
    void BeginTarget(BotContext& ctx, const LootCandidate& c);
    // `why`: a key (lang/en.json).
    void AttemptFailed(BotContext& ctx, const char* why);
    void GiveUp(BotContext& ctx, const char* why);
    TaskStatus Finish(BotContext& ctx, TaskStatus st, const char* why, int cooldownMs);
    void AddMarkers(BotContext& ctx, uint32_t chosenId) const;
    void SetStatus(const char* s) { m_status.store(s, std::memory_order_relaxed); }

    LootScanner m_scanner;
    WorldClick  m_click;
    uint32_t    m_targetId = 0;
    uint64_t    m_startMs = 0;
    std::unordered_map<uint32_t, int> m_attempts;

    Phase        m_phase = Phase::Choose;
    uint32_t     m_pendingId = 0;      // chosen by WantsToStart, taken by Start
    GroundItem   m_targetItem;
    LootItemInfo m_targetInfo;
    std::string  m_targetName;
    VanishWatch  m_vanish;
    uint64_t     m_cooldownUntilMs = 0;
    uint32_t     m_area = 0;
    bool         m_haveArea = false;
    int          m_fullAtFreeCells = -1;   // free cells when the inventory-full flag was set
    int          m_pickedRun = 0;
    LocText      m_failReason;
    std::atomic<const char*> m_status{Key("loot_status_idle")};
    std::atomic<int>         m_pickedTotal{0};
    uint64_t     m_startedSession = 0;     // BotMemory::startedMs the total belongs to
};

}  // namespace fb
