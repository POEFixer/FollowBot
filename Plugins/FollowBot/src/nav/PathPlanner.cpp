// PathPlanner.cpp - see PathPlanner.h.
#include "PathPlanner.h"

#include "../core/LocText.h"

#include <algorithm>

namespace fb {

const char* ReplanReasonName(ReplanReason r) {
    switch (r) {
        case ReplanReason::None:        return "none";
        case ReplanReason::NoPath:      return "no path yet";
        case ReplanReason::Requested:   return "requested";
        case ReplanReason::NavChanged:  return "grid changed";
        case ReplanReason::GoalMoved:   return "goal moved";
        case ReplanReason::OffCorridor: return "left the path";
        case ReplanReason::Timer:       return "timer";
    }
    return "?";
}

const char* PlanStatusName(PlanStatus s) {
    switch (s) {
        case PlanStatus::Idle:         return Key("plan_idle");
        case PlanStatus::NoGrid:       return Key("plan_no_grid");
        case PlanStatus::AtGoal:       return Key("plan_at_goal");
        case PlanStatus::Path:         return Key("plan_path");
        case PlanStatus::Direct:       return Key("plan_direct");
        case PlanStatus::KeptPrevious: return Key("plan_kept_previous");
        case PlanStatus::Unreachable:  return Key("plan_unreachable");
    }
    return Key("common_unknown");
}

bool InUnreachableBackoff(const PlannerState& st, const Vec2& goal, uint64_t now, const PlannerParams& p) {
    if (st.lastFailMs == 0 || now < st.lastFailMs) return false;
    if (now - st.lastFailMs >= static_cast<uint64_t>(p.unreachableBackoffMs)) return false;
    return Dist(goal, st.failedGoal) <= p.sameFailedGoalCells;
}

ReplanReason DecideReplan(const PlannerState& st, const Vec2& player, const Vec2& goal, uint64_t now,
                          uint64_t navGeneration, uint64_t overlayGeneration, const PlannerParams& p) {
    if (InUnreachableBackoff(st, goal, now, p)) return ReplanReason::None;
    if (st.retryAfterMs != 0 && now < st.retryAfterMs) return ReplanReason::None;
    if (!st.hasPath || st.path.empty()) return ReplanReason::NoPath;
    if (st.requested) return ReplanReason::Requested;
    if (navGeneration != st.navGeneration || overlayGeneration != st.overlayGeneration) return ReplanReason::NavChanged;
    const float remaining = RemainingLength(player, st.path);
    const float moveLimit = (std::max)(p.goalMoveMinCells, p.goalMoveFraction * remaining);
    if (Dist(goal, st.planGoal) > moveLimit) return ReplanReason::GoalMoved;
    if (PolylineDistance(player, st.path) > p.corridorCells) return ReplanReason::OffCorridor;
    if (now >= st.planMs && now - st.planMs >= static_cast<uint64_t>(p.replanMs)) return ReplanReason::Timer;
    return ReplanReason::None;
}

void PathPlanner::SetGrid(std::shared_ptr<const NavGrid> grid) {
    if (grid == m_grid) return;
    m_grid = std::move(grid);
    m_overlay.Clear();
    Clear();
}

void PathPlanner::Clear() {
    m_state = PlannerState{};
    m_unreachableStreak = 0;
    m_status = PlanStatus::Idle;
    m_lastReason = ReplanReason::None;
    m_lastFail = AStarFail::None;
}

bool PathPlanner::LineOfSight(const Vec2& a, const Vec2& b, int minClearance) const {
    return m_grid && fb::LineOfSight(*m_grid, &m_overlay, a, b, minClearance);
}

bool PathPlanner::IsWalkable(const Vec2& p) const {
    return m_grid && IsPassable(*m_grid, &m_overlay, NavGrid::CellOf(p.x), NavGrid::CellOf(p.y));
}

bool PathPlanner::SnapToWalkable(const Vec2& in, Vec2& out, int radius) const {
    if (!m_grid) {
        out = in;
        return false;
    }
    const int cx = NavGrid::CellOf(in.x), cy = NavGrid::CellOf(in.y);
    if (IsPassable(*m_grid, &m_overlay, cx, cy)) {
        out = in;
        return true;
    }
    int ox = 0, oy = 0;
    if (!NearestPassable(*m_grid, &m_overlay, cx, cy, radius, ox, oy)) return false;
    out = NavGrid::CellCenter(ox, oy);
    return true;
}

PlanStatus PathPlanner::Update(const Vec2& player, const Vec2& goal, uint64_t now) {
    if (m_overlay.Expire(now)) m_state.requested = true;
    if (!m_grid) {
        m_state.hasPath = true;
        m_state.path = {player, goal};
        m_state.planGoal = goal;
        m_status = PlanStatus::NoGrid;
        return m_status;
    }
    if (Dist(player, goal) < 1.0f) {
        m_status = PlanStatus::AtGoal;
        return m_status;
    }
    const uint64_t navGen = m_grid->Generation();
    const uint64_t ovGen = m_overlay.Generation();
    const ReplanReason reason = DecideReplan(m_state, player, goal, now, navGen, ovGen, m_params);
    if (reason == ReplanReason::None) {
        if (InUnreachableBackoff(m_state, goal, now, m_params) && m_status != PlanStatus::KeptPrevious)
            m_status = PlanStatus::Unreachable;
        return m_status;
    }
    m_lastReason = reason;
    m_state.requested = false;
    m_state.navGeneration = navGen;
    m_state.overlayGeneration = ovGen;

    // Short, clear moves need no search.
    if (Dist(player, goal) <= m_params.directLosMaxCells && LineOfSight(player, goal, 1)) {
        m_state.hasPath = true;
        m_state.path = {player, goal};
        m_state.planGoal = goal;
        m_state.planMs = now;
        m_state.lastFailMs = 0;
        m_state.retryAfterMs = 0;
        m_lastSuccessMs = now;
        m_lastFail = AStarFail::None;
        m_unreachableStreak = 0;
        m_status = PlanStatus::Direct;
        return m_status;
    }

    AStarResult r = FindPath(*m_grid, &m_overlay, player, goal, m_params.astar);
    ++m_searches;
    m_lastIterations = r.iterations;
    if (r.found && !r.path.empty()) {
        m_state.hasPath = true;
        m_state.path = std::move(r.path);
        if (m_state.path.front() != player) m_state.path.insert(m_state.path.begin(), player);
        m_state.planGoal = goal;
        m_state.planMs = now;
        m_state.lastFailMs = 0;
        m_state.retryAfterMs = 0;
        m_lastSuccessMs = now;
        m_lastFail = AStarFail::None;
        m_unreachableStreak = 0;
        m_status = PlanStatus::Path;
        return m_status;
    }

    m_lastFail = r.fail;
    m_state.planMs = now;
    m_state.retryAfterMs = now + static_cast<uint64_t>(m_params.failRetryMs);
    // Keep the previous path for a moment: one failed search (a temporary block,
    // a goal standing on an unwalkable cell for a tick) must not stop the walk.
    if (m_state.hasPath && m_lastSuccessMs != 0 && now - m_lastSuccessMs < static_cast<uint64_t>(m_params.failKeepMs)) {
        m_status = PlanStatus::KeptPrevious;
        return m_status;
    }
    if (LineOfSight(player, goal, 1)) {
        m_state.hasPath = true;
        m_state.path = {player, goal};
        m_state.planGoal = goal;
        m_unreachableStreak = 0;
        m_status = PlanStatus::Direct;
        return m_status;
    }
    m_state.hasPath = false;
    m_state.path.clear();
    m_state.lastFailMs = now;
    m_state.failedGoal = goal;
    // The backoff holds every search off, not only one for the same goal: a goal
    // moving about on unreachable ground would otherwise re-run a full search
    // (up to astar.maxIterations) every failRetryMs.
    int backoff = (std::max)(m_params.unreachableBackoffMs, 0);
    for (int i = 0; i < m_unreachableStreak && backoff < m_params.unreachableBackoffMaxMs; ++i) backoff *= 2;
    backoff = (std::max)(m_params.unreachableBackoffMs, (std::min)(backoff, m_params.unreachableBackoffMaxMs));
    if (m_unreachableStreak < 16) ++m_unreachableStreak;
    m_state.retryAfterMs = now + static_cast<uint64_t>(backoff < 0 ? 0 : backoff);
    m_status = PlanStatus::Unreachable;
    return m_status;
}

}  // namespace fb
