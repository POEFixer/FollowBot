// PathPlanner.h - owns the current goal and path and decides when to plan
// again (design §5.4). Pure: no SDK.
//
// Replans when the goal moved more than max(3, 10 % of the remaining length)
// cells, the player left the path corridor (> 6 cells from the polyline), the
// grid or the overlay (temporary blocks, doors) changed, every replanMs, or on
// request (stuck recovery). A failed plan keeps the previous path for 500 ms,
// then falls back to a direct move when the line of sight is clear. Short
// line-of-sight-clear moves skip A*. An unreachable result is reported and
// holds every further search off for unreachableBackoffMs, doubling on each
// consecutive unreachable result up to unreachableBackoffMaxMs (a success
// resets it) - whatever the goal does meanwhile: a leader moving about on
// ground we cannot reach must not cost a full search every few hundred ms.
#pragma once

#include "AStar.h"
#include "NavGrid.h"

#include <memory>
#include <vector>

namespace fb {

struct PlannerParams {
    int   replanMs = 1500;
    float corridorCells = 6.f;
    float goalMoveMinCells = 3.f;
    float goalMoveFraction = 0.10f;
    int   failKeepMs = 500;
    int   unreachableBackoffMs = 1000;
    int   unreachableBackoffMaxMs = 4000;   // consecutive unreachable results double the backoff up to this
    float directLosMaxCells = 40.f;   // shorter LOS-clear moves skip A*
    float sameFailedGoalCells = 3.f;  // a goal this close to the failed one waits out the backoff
    int   failRetryMs = 150;          // after a failed search, no new search for this long
    AStarParams astar;
};

enum class ReplanReason : uint8_t { None = 0, NoPath, Requested, NavChanged, GoalMoved, OffCorridor, Timer };

const char* ReplanReasonName(ReplanReason r);

enum class PlanStatus : uint8_t {
    Idle = 0,       // nothing planned yet
    NoGrid,         // no walkable grid: callers steer straight at the goal
    AtGoal,         // already there
    Path,           // an A* path
    Direct,         // a straight, line-of-sight-clear segment
    KeptPrevious,   // the last plan failed; the previous path is still used (<= failKeepMs)
    Unreachable     // no path; not retried until the backoff ends
};

const char* PlanStatusName(PlanStatus s);

// The planner's memory the replan decision looks at.
struct PlannerState {
    bool              hasPath = false;
    std::vector<Vec2> path;
    Vec2              planGoal;        // the goal the path was planned for
    uint64_t          planMs = 0;
    uint64_t          navGeneration = 0;
    uint64_t          overlayGeneration = 0;
    uint64_t          lastFailMs = 0;  // unreachable backoff
    Vec2              failedGoal;
    uint64_t          retryAfterMs = 0;  // a search just failed: wait before the next one
    bool              requested = false;
};

// Pure policy: whether (and why) to plan again now.
ReplanReason DecideReplan(const PlannerState& st, const Vec2& player, const Vec2& goal, uint64_t now,
                          uint64_t navGeneration, uint64_t overlayGeneration, const PlannerParams& p);

// True while an unreachable goal near `goal` is backing off.
bool InUnreachableBackoff(const PlannerState& st, const Vec2& goal, uint64_t now, const PlannerParams& p);

class PathPlanner {
public:
    void SetGrid(std::shared_ptr<const NavGrid> grid);
    void SetParams(const PlannerParams& p) { m_params = p; }
    const PlannerParams& Params() const { return m_params; }

    // Plans (or not) toward `goal` and returns the status; Path() is the polyline
    // to steer along (first point = the player's position when planned).
    PlanStatus Update(const Vec2& player, const Vec2& goal, uint64_t now);

    void RequestReplan() { m_state.requested = true; }
    void Clear();   // forget the goal and path (keeps the grid and overlay)

    const std::vector<Vec2>& Path() const { return m_state.path; }
    bool HasPath() const { return m_state.hasPath; }
    PlanStatus Status() const { return m_status; }
    ReplanReason LastReason() const { return m_lastReason; }
    AStarFail LastFail() const { return m_lastFail; }
    int LastIterations() const { return m_lastIterations; }
    int SearchCount() const { return m_searches; }   // A* searches run (diagnostics, tests)
    Vec2 PlannedGoal() const { return m_state.planGoal; }

    const NavGrid* Grid() const { return m_grid.get(); }
    std::shared_ptr<const NavGrid> GridPtr() const { return m_grid; }
    NavOverlay& Overlay() { return m_overlay; }
    const NavOverlay& Overlay() const { return m_overlay; }
    void AddTemporaryBlock(const Vec2& center, int radius, uint64_t untilMs) { m_overlay.AddBlock(center, radius, untilMs); }

    // Helpers over the current grid + overlay (false / unchanged without a grid).
    bool LineOfSight(const Vec2& a, const Vec2& b, int minClearance = 1) const;
    bool IsWalkable(const Vec2& p) const;
    bool SnapToWalkable(const Vec2& in, Vec2& out, int radius) const;

private:
    std::shared_ptr<const NavGrid> m_grid;
    NavOverlay    m_overlay;
    PlannerParams m_params;
    PlannerState  m_state;
    PlanStatus    m_status = PlanStatus::Idle;
    ReplanReason  m_lastReason = ReplanReason::None;
    AStarFail     m_lastFail = AStarFail::None;
    int           m_lastIterations = 0;
    int           m_searches = 0;
    int           m_unreachableStreak = 0;   // consecutive unreachable results (backoff doubling)
    uint64_t      m_lastSuccessMs = 0;
};

}  // namespace fb
