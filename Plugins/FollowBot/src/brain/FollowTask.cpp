// FollowTask.cpp - see FollowTask.h.
#include "FollowTask.h"

#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "../move/StuckRecovery.h"
#include "../nav/PathPlanner.h"

#include <cstdio>

namespace fb {

const char* FollowTask::ModeName(Mode m) {
    switch (m) {
        case Mode::NoLeader:      return "No leader set";
        case Mode::Disabled:      return "Following off here";
        case Mode::Reacquire:     return "Waiting for the leader to arrive";
        case Mode::NotHere:       return "Leader not in this area";
        case Mode::Holding:       return "Near the leader";
        case Mode::Following:     return "Following";
        case Mode::LostHold:      return "Leader just left sight";
        case Mode::Seeking:       return "Walking to the leader's last position";
        case Mode::WaitingAtLast: return "Waiting at the leader's last position";
        case Mode::GaveUp:        return "Stuck: retrying soon";
    }
    return "?";
}

Vec2 FollowTask::FormationOffset(int slot, float spacing, const Vec2& heading) {
    if (slot <= 0) return {0.f, 0.f};
    // Leader frame: x = forward, y = right. Slots fan out behind him.
    static const Vec2 kSlots[5] = {{-1.f, -1.f}, {-1.f, 1.f}, {-2.f, 0.f}, {-2.f, -2.f}, {-2.f, 2.f}};
    const Vec2 local = kSlots[(slot - 1) % 5] * spacing;
    Vec2 fwd = Normalize(heading);
    if (Length2(fwd) < 1e-6f) fwd = {1.f, 0.f};
    const Vec2 right = Perp(fwd);
    return fwd * local.x + right * local.y;
}

Vec2 FollowTask::ComputeGoal(BotContext& ctx) const {
    const LeaderState& L = ctx.w.leader;
    const FollowSettings& f = ctx.p.follow;
    Vec2 goal = L.info.grid;
    if (f.followDestination && L.speed > 2.f && !L.route.empty() && Dist(L.route.back(), goal) < 80.f) {
        goal = L.route.back();
    } else if (f.predict && f.predictMs > 0 && L.speed > 0.5f) {
        const Vec2 predicted = goal + L.velocity * (static_cast<float>(f.predictMs) / 1000.f);
        // Keep the prediction on walkable ground: step back toward him until it is.
        Vec2 p = predicted;
        for (int i = 0; i < 8 && ctx.planner.Grid() && !ctx.planner.IsWalkable(p); ++i) p = Lerp(p, goal, 0.35f);
        if (!ctx.planner.Grid() || ctx.planner.IsWalkable(p)) goal = p;
    }
    if (f.formationSlot > 0) {
        Vec2 p = goal + FormationOffset(f.formationSlot, f.formationSpacing, L.heading);
        Vec2 snapped;
        if (ctx.planner.SnapToWalkable(p, snapped, 6)) goal = snapped;
        else if (!ctx.planner.Grid()) goal = p;
    }
    return goal;
}

void FollowTask::StopMoving(BotContext& ctx) {
    if (m_moving || ctx.mover.IsMoving()) ctx.mover.Stop(ctx.act);
    m_moving = false;
}

void FollowTask::Abort(BotContext& ctx) {
    StopMoving(ctx);
    m_wantMove = false;
    m_haveGoal = false;
}

void FollowTask::MoveToward(BotContext& ctx, const Vec2& goal, float arrival) {
    const World& w = ctx.w;
    if (m_giveUpUntilMs != 0 && ctx.now < m_giveUpUntilMs) {
        StopMoving(ctx);
        m_mode = Mode::GaveUp;
        return;
    }
    if (m_giveUpUntilMs != 0) {
        m_giveUpUntilMs = 0;
        ctx.stuck.Reset(ctx.now);
    }
    if (ctx.cursorBusy) {
        StopMoving(ctx);
        ctx.report.detail = "Waiting: another plugin is using the cursor";
        return;
    }
    const PlanStatus ps = ctx.planner.Update(w.self.grid, goal, ctx.now);
    if (ps == PlanStatus::Unreachable) {
        // No path on the walkable grid (and no line of sight): walking straight
        // into the wall would only trip the stuck recovery. Wait and re-plan.
        StopMoving(ctx);
        ctx.report.detail = "Leader unreachable from here: waiting";
        return;
    }
    m_steered = true;
    const RecoveryState rs =
        ctx.stuck.Tick(w, ctx.mover, ctx.planner, ctx.act, ctx.log, goal, arrival, true, ctx.now);
    if (rs == RecoveryState::GaveUp) {
        m_giveUpUntilMs = ctx.now + static_cast<uint64_t>(ctx.p.follow.stuck.followRetryMs);
        StopMoving(ctx);
        m_mode = Mode::GaveUp;
        ctx.log.Add(LogCat::Move, "Follow: stuck recovery exhausted, retrying in %d ms", ctx.p.follow.stuck.followRetryMs);
        return;
    }
    if (rs == RecoveryState::Recovering) {
        m_moving = true;
        ctx.report.detail = std::string("Unsticking: ") + RecoveryStepName(ctx.stuck.CurrentStep());
        return;
    }
    const MoveStatus ms = ctx.mover.Steer(w, ctx.planner.Path(), goal, ctx.act, &ctx.planner, ctx.now);
    m_moving = ms == MoveStatus::Moving || ms == MoveStatus::Activating;
    if (ms == MoveStatus::NoKey) ctx.report.detail = "No move key set (General tab)";
    else if (ms == MoveStatus::NoProjection) ctx.report.detail = "Cannot project the path on screen";
}

TaskStatus FollowTask::Tick(BotContext& ctx) {
    m_steered = false;
    const TaskStatus st = TickInner(ctx);
    // Not walking this tick: keep the stuck history, disarm the detector.
    if (!m_steered) ctx.stuck.NoteIdle(ctx.w, ctx.now);
    return st;
}

TaskStatus FollowTask::TickInner(BotContext& ctx) {
    const World& w = ctx.w;
    const LeaderState& L = w.leader;
    const FollowSettings& f = ctx.p.follow;

    if (!L.configured) {
        m_mode = Mode::NoLeader;
        m_haveGoal = false;
        m_wantMove = false;
        StopMoving(ctx);
        ctx.report.detail = "Set the leader's character name (General tab)";
        return TaskStatus::Running;
    }
    if ((w.gate.isTown || w.gate.isHideout) && !f.followInTownHideout) {
        m_mode = Mode::Disabled;
        m_wantMove = false;
        StopMoving(ctx);
        ctx.report.detail = "Following in town / hideout is off";
        return TaskStatus::Running;
    }

    float arrival = f.stopDistance;
    if (L.visible) {
        m_goal = ComputeGoal(ctx);
        m_haveGoal = true;
        const float dist = Dist(w.self.grid, m_goal);
        // The hysteresis keeps its own state: one tick whose steering was
        // deferred (Pick My Loot's cursor) or failed must not turn "walking to
        // stopDistance" into "standing until followDistance".
        m_wantMove = NextMoving(m_wantMove, dist, f.followDistance, f.stopDistance);
        if (!m_wantMove) {
            StopMoving(ctx);
            m_mode = Mode::Holding;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Near the leader (%.0f cells)", Dist(w.self.grid, L.info.grid));
            ctx.report.detail = buf;
            return TaskStatus::Running;
        }
        m_mode = Mode::Following;
        ctx.report.hasTarget = true;
        ctx.report.target = m_goal;
    } else {
        const bool seenHere = L.lastSeenMs != 0 && L.lastSeenArea == w.gate.areaCounter;
        if (!seenHere) {
            // Not seen in this area yet: after our own area change, give him time.
            const uint64_t since = ctx.mem.areaEnteredMs;
            StopMoving(ctx);
            m_haveGoal = false;
            m_wantMove = false;
            if (since != 0 && ctx.now - since < static_cast<uint64_t>(f.reacquireMs)) {
                m_mode = Mode::Reacquire;
                ctx.report.detail = "Waiting for the leader to arrive";
            } else {
                m_mode = Mode::NotHere;
                ctx.report.detail = L.lastSeenMs == 0 ? "Leader not seen yet" : "Leader not in this area";
            }
            return TaskStatus::Running;
        }
        if (!L.vanish.active && ctx.now - L.lastSeenMs < static_cast<uint64_t>(f.lostHoldMs) && m_haveGoal) {
            m_mode = Mode::LostHold;
            ctx.report.detail = "Leader just left sight";
        } else {
            // Seek his last position, then wait there with the reason.
            m_goal = L.vanish.active ? L.vanish.lastGrid : L.lastGrid;
            m_haveGoal = true;
            arrival = (std::max)(3.f, f.stopDistance * 0.5f);
            const float dist = Dist(w.self.grid, m_goal);
            if (dist <= arrival) {
                StopMoving(ctx);
                m_mode = Mode::WaitingAtLast;
                char buf[160];
                if (L.vanish.active)
                    std::snprintf(buf, sizeof(buf), "Leader left (%s): waiting at his last position",
                                  VanishKindName(L.vanish.kind));
                else
                    std::snprintf(buf, sizeof(buf), "Leader out of sight: waiting at his last position");
                ctx.report.detail = buf;
                return TaskStatus::Running;
            }
            m_mode = Mode::Seeking;
            ctx.report.detail = L.vanish.active ? std::string("Leader left (") + VanishKindName(L.vanish.kind) +
                                                      "): walking to his last position"
                                                : std::string("Walking to the leader's last position");
        }
        ctx.report.hasTarget = true;
        ctx.report.target = m_goal;
    }
    MoveToward(ctx, m_goal, arrival);
    return TaskStatus::Running;
}

// ---------------------------------------------------------------------------
// Debug walk
// ---------------------------------------------------------------------------

void WalkToTask::Start(BotContext& ctx) {
    m_startMs = ctx.now;
    m_targetId = 0;
    m_result.clear();
    const Interactable* it = ctx.w.NearestInteractable(m_kind, ctx.w.self.grid);
    if (!it) {
        m_result = std::string("No ") + InteractKindName(m_kind) + " known in this area";
        return;
    }
    m_targetId = it->id;
    WorldClickTarget t;
    t.id = it->id;
    t.grid = it->grid;
    t.world = it->world;
    t.modelHeight = it->modelHeight;
    t.targetableAddr = it->targetableAddr;
    WorldClickParams p;
    p.interactRange = ctx.p.travel.interactRange;
    p.dwellMs = ctx.p.travel.hoverDwellMs;
    p.holdMs = ctx.p.travel.pressHoldMs;
    p.reHovers = ctx.p.travel.reHovers;
    p.heightFactor = ctx.p.travel.tallObjectHeightFactor;
    if (m_kind == InteractKind::Stash || m_kind == InteractKind::GuildStash) p.interactRange = ctx.p.stash.approachRingCells;
    m_wc.Start(t, p, ctx.now);
    ctx.log.Add(LogCat::Task, "Debug walk to %s %u at (%.0f, %.0f)", InteractKindName(m_kind), it->id, it->grid.x,
                it->grid.y);
}

void WalkToTask::Abort(BotContext& ctx) {
    m_wc.Abort(ctx);
    if (m_result.empty()) m_result = "Stopped";
}

TaskStatus WalkToTask::Tick(BotContext& ctx) {
    if (m_targetId == 0) return TaskStatus::Failed;
    if (const Interactable* it = ctx.w.FindInteractable(m_targetId)) {
        WorldClickTarget t = m_wc.Target();
        t.grid = it->grid;
        t.world = it->world;
        t.modelHeight = it->modelHeight;
        t.targetableAddr = it->targetableAddr;
        m_wc.UpdateTarget(t);
    }
    if (!m_click && Dist(ctx.w.self.grid, m_wc.Target().grid) <= ctx.p.travel.interactRange) {
        ctx.mover.Stop(ctx.act);
        m_result = "Arrived";
        return TaskStatus::Done;
    }
    ctx.report.detail = std::string("Walking to the nearest ") + InteractKindName(m_kind);
    const WorldClickStatus st = m_wc.Tick(ctx);
    if (st == WorldClickStatus::Clicked) {
        m_result = "Arrived and clicked";
        return TaskStatus::Done;
    }
    if (st == WorldClickStatus::Failed) {
        m_result = "Failed: " + m_wc.FailReason();
        return TaskStatus::Failed;
    }
    return TaskStatus::Running;
}

}  // namespace fb
