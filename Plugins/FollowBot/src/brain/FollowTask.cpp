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
        case Mode::NoLeader:      return Key("mode_no_leader");
        case Mode::Disabled:      return Key("mode_disabled");
        case Mode::Reacquire:     return Key("mode_reacquire");
        case Mode::NotHere:       return Key("mode_not_here");
        case Mode::Holding:       return Key("mode_holding");
        case Mode::Following:     return Key("mode_following");
        case Mode::LostHold:      return Key("mode_lost_hold");
        case Mode::Seeking:       return Key("mode_seeking");
        case Mode::WaitingAtLast: return Key("mode_waiting_at_last");
        case Mode::GaveUp:        return Key("mode_gave_up");
    }
    return Key("common_unknown");
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
        ctx.report.detail = Tr("follow_cursor_busy");
        return;
    }
    const PlanStatus ps = ctx.planner.Update(w.self.grid, goal, ctx.now);
    if (ps == PlanStatus::Unreachable) {
        // No path on the walkable grid (and no line of sight): walking straight
        // into the wall would only trip the stuck recovery. Wait and re-plan.
        StopMoving(ctx);
        ctx.report.detail = Tr("follow_unreachable");
        return;
    }
    m_steered = true;
    const RecoveryState rs =
        ctx.stuck.Tick(w, ctx.mover, ctx.planner, ctx.act, ctx.log, goal, arrival, true, ctx.now);
    if (rs == RecoveryState::GaveUp) {
        m_giveUpUntilMs = ctx.now + static_cast<uint64_t>(ctx.p.follow.stuck.followRetryMs);
        StopMoving(ctx);
        m_mode = Mode::GaveUp;
        ctx.log.Add(LogCat::Move, TrF("log_follow_gave_up", {{"ms", Num("%d", ctx.p.follow.stuck.followRetryMs)}}));
        return;
    }
    if (rs == RecoveryState::Recovering) {
        m_moving = true;
        ctx.report.detail = TrF("follow_unsticking", {{"step", Tr(RecoveryStepName(ctx.stuck.CurrentStep()))}});
        return;
    }
    const MoveStatus ms = ctx.mover.Steer(w, ctx.planner.Path(), goal, ctx.act, &ctx.planner, ctx.now);
    m_moving = ms == MoveStatus::Moving || ms == MoveStatus::Activating;
    if (ms == MoveStatus::NoKey) ctx.report.detail = Tr("follow_no_move_key");
    else if (ms == MoveStatus::NoProjection) ctx.report.detail = Tr("follow_no_projection");
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
        ctx.report.detail = Tr("follow_set_leader");
        return TaskStatus::Running;
    }
    if ((w.gate.isTown || w.gate.isHideout) && !f.followInTownHideout) {
        m_mode = Mode::Disabled;
        m_wantMove = false;
        StopMoving(ctx);
        ctx.report.detail = Tr("follow_town_off");
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
            ctx.report.detail = TrF("follow_near_leader", {{"cells", Num("%.0f", Dist(w.self.grid, L.info.grid))}});
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
                ctx.report.detail = Tr("mode_reacquire");
            } else {
                m_mode = Mode::NotHere;
                ctx.report.detail = L.lastSeenMs == 0 ? Tr("follow_not_seen_yet") : Tr("mode_not_here");
            }
            return TaskStatus::Running;
        }
        if (!L.vanish.active && ctx.now - L.lastSeenMs < static_cast<uint64_t>(f.lostHoldMs) && m_haveGoal) {
            m_mode = Mode::LostHold;
            ctx.report.detail = Tr("mode_lost_hold");
        } else {
            // Seek his last position, then wait there with the reason.
            m_goal = L.vanish.active ? L.vanish.lastGrid : L.lastGrid;
            m_haveGoal = true;
            arrival = (std::max)(3.f, f.stopDistance * 0.5f);
            const float dist = Dist(w.self.grid, m_goal);
            if (dist <= arrival) {
                StopMoving(ctx);
                m_mode = Mode::WaitingAtLast;
                ctx.report.detail = L.vanish.active
                                        ? TrF("follow_left_waiting", {{"kind", Tr(VanishKindName(L.vanish.kind))}})
                                        : Tr("follow_out_of_sight_waiting");
                return TaskStatus::Running;
            }
            m_mode = Mode::Seeking;
            ctx.report.detail = L.vanish.active
                                    ? TrF("follow_left_walking", {{"kind", Tr(VanishKindName(L.vanish.kind))}})
                                    : Tr("mode_seeking");
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
        m_result = TrF("walk_none_known", {{"kind", Tr(InteractKindName(m_kind))}});
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
    ctx.log.Add(LogCat::Task, TrF("log_walk_to", {{"kind", Tr(InteractKindName(m_kind))},
                                                  {"id", Num("%u", it->id)},
                                                  {"x", Num("%.0f", it->grid.x)},
                                                  {"y", Num("%.0f", it->grid.y)}}));
}

void WalkToTask::Abort(BotContext& ctx) {
    m_wc.Abort(ctx);
    if (m_result.empty()) m_result = Tr("walk_stopped");
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
        m_result = Tr("walk_arrived");
        return TaskStatus::Done;
    }
    ctx.report.detail = TrF("walk_walking_to", {{"kind", Tr(InteractKindName(m_kind))}});
    const WorldClickStatus st = m_wc.Tick(ctx);
    if (st == WorldClickStatus::Clicked) {
        m_result = Tr("walk_clicked");
        return TaskStatus::Done;
    }
    if (st == WorldClickStatus::Failed) {
        m_result = TrF("walk_failed", {{"why", Tr(m_wc.FailReason())}});
        return TaskStatus::Failed;
    }
    return TaskStatus::Running;
}

}  // namespace fb
