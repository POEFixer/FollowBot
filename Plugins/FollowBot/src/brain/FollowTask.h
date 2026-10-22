// FollowTask.h - the default behaviour: keep up with the leader (design §5.6),
// plus the debug "walk to the nearest X" task. Pure: no SDK.
//
// Hysteresis: start moving when the goal is farther than followDistance, stop
// when closer than stopDistance. Goal = the leader's position, optionally
// predicted (+ velocity x predictMs, kept on walkable ground), optionally the
// end of his route while he moves ("follow destination"), plus an optional
// formation offset (slot 1..5 x spacing, rotated by his heading).
// Leader just lost (no vanish yet): keep the last goal for lostHoldMs. Vanish
// of a kind the bot does not follow through (checkpoint, waypoint, unknown,
// out of range, or travel off / failed): walk to his last position and wait
// there, saying why. After our own area change: wait up to reacquireMs for him
// to appear before reporting "leader not in this area".
#pragma once

#include "../travel/WorldClick.h"
#include "Task.h"

#include <string>

namespace fb {

class FollowTask : public Task {
public:
    enum class Mode : uint8_t {
        NoLeader = 0,     // no name set (and auto-pick off)
        Disabled,         // following off here (town / hideout option)
        Reacquire,        // we just changed area: waiting for him
        NotHere,          // he is not in this area (reacquire timed out)
        Holding,          // close enough: standing
        Following,        // walking toward him
        LostHold,         // he just left sight: keeping the last goal
        Seeking,          // walking to his last known position
        WaitingAtLast,    // at his last position, waiting (reason shown)
        GaveUp            // stuck: retrying after followRetryMs
    };

    const char* Name() const override { return "Follow"; }
    TaskStatus Tick(BotContext& ctx) override;   // always Running
    void Abort(BotContext& ctx) override;
    bool AllowsCasts(CastClass) const override { return true; }

    Mode CurrentMode() const { return m_mode; }
    static const char* ModeName(Mode m);
    bool HasGoal() const { return m_haveGoal; }
    Vec2 Goal() const { return m_goal; }
    bool Moving() const { return m_moving; }

    // --- pure helpers (tested) -------------------------------------------
    // Offset of formation slot 1..5 in the leader's frame (behind him), rotated
    // by his heading; zero for slot 0.
    static Vec2 FormationOffset(int slot, float spacing, const Vec2& heading);
    // Hysteresis: the next "moving" state for a goal distance.
    static bool NextMoving(bool moving, float dist, float followDistance, float stopDistance) {
        if (!moving && dist > followDistance) return true;
        if (moving && dist < stopDistance) return false;
        return moving;
    }

private:
    TaskStatus TickInner(BotContext& ctx);
    Vec2 ComputeGoal(BotContext& ctx) const;
    void MoveToward(BotContext& ctx, const Vec2& goal, float arrival);
    void StopMoving(BotContext& ctx);

    Mode     m_mode = Mode::NoLeader;
    bool     m_wantMove = false;   // the follow / stop hysteresis (NextMoving); a deferred or failed steer keeps it
    bool     m_moving = false;     // steered this tick
    bool     m_haveGoal = false;
    Vec2     m_goal;
    uint64_t m_giveUpUntilMs = 0;
    bool     m_steered = false;   // the stuck recovery was ticked this tick
};

// Debug tool: walk to the nearest interactable of a kind (no leader needed) and
// optionally click it - tests the walk, the projection and the click.
class WalkToTask : public Task {
public:
    void Configure(InteractKind kind, bool click) {
        m_kind = kind;
        m_click = click;
    }
    const char* Name() const override { return "Debug walk"; }
    void Start(BotContext& ctx);
    TaskStatus Tick(BotContext& ctx) override;
    void Abort(BotContext& ctx) override;
    bool AllowsCasts(CastClass) const override { return false; }
    // A test started from the POEFixer menu waits while the menu is open, the
    // game is not in front or you just clicked; stop / pause end it.
    bool KeepThroughHold(GateReason r) const override {
        return r != GateReason::Stopped && r != GateReason::Paused && r != GateReason::OverrideHeld &&
               r != GateReason::MaxRuntime;
    }
    const std::string& Result() const { return m_result; }

private:
    InteractKind m_kind = InteractKind::Stash;
    bool         m_click = true;
    uint32_t     m_targetId = 0;
    WorldClick   m_wc;
    uint64_t     m_startMs = 0;
    std::string  m_result;
};

}  // namespace fb
