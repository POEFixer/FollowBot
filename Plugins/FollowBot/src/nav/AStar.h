// AStar.h - exact walkable-grid A* (design §5.4). Pure: no SDK.
//
// A port of POEFixer/core/expedition/ExpeditionPathfinder (ours, pure STL):
// 8-directional, corner-cut prevention, Euclidean heuristic, endpoint snap,
// iteration cap, open-addressing node table. Additions:
//  - clearance cost: a step onto a cell with clearance 1 / 2 / 3 costs
//    + 3 / 1.5 / 0.5 x wallPenalty, so paths keep off walls when they can;
//  - dynamic blocks and door cells through NavOverlay;
//  - line-of-sight smoothing that never cuts closer to a wall than the raw path
//    went (a segment must keep min(2, lowest raw clearance on that stretch)) and
//    never squeezes diagonally between two blocked cells.
// Path points are cell centres (x + 0.5, y + 0.5); the first point is the exact
// start and, when the goal needed no snap, the last is the exact goal.
#pragma once

#include "NavGrid.h"

#include <vector>

namespace fb {

struct AStarParams {
    int   maxIterations = 400000;
    float wallPenalty = 1.0f;
    int   snapRadius = 30;          // endpoint snap-to-passable radius (cells)
    float maxStraightDist = 2500.f; // pre-reject beyond this straight-line distance
    bool  smooth = true;
};

enum class AStarFail : uint8_t { None = 0, NoGrid, StartBlocked, GoalBlocked, TooFar, NoPath, IterationCap };

const char* AStarFailName(AStarFail f);

struct AStarResult {
    bool              found = false;
    AStarFail         fail = AStarFail::None;
    std::vector<Vec2> path;          // smoothed (or raw when smooth = false)
    std::vector<Vec2> raw;           // every cell of the raw path (centres)
    int               iterations = 0;
    bool              startSnapped = false, goalSnapped = false;
    Vec2              start, goal;   // the snapped endpoints (cell centres)
    float             cost = 0.f;    // raw path cost including clearance penalties
};

// Extra cost of stepping onto a cell with this clearance.
inline float ClearancePenalty(uint8_t clearance, float wallPenalty) {
    if (wallPenalty <= 0.f) return 0.f;
    switch (clearance) {
        case 1: return 3.0f * wallPenalty;
        case 2: return 1.5f * wallPenalty;
        case 3: return 0.5f * wallPenalty;
        default: return 0.f;
    }
}

AStarResult FindPath(const NavGrid& grid, const NavOverlay* overlay, const Vec2& start, const Vec2& goal,
                     const AStarParams& params);

// Bresenham line from a to b over passable cells whose clearance is at least
// `minClearance`; a diagonal step also needs both orthogonal neighbours passable.
bool LineOfSight(const NavGrid& grid, const NavOverlay* overlay, const Vec2& a, const Vec2& b, int minClearance = 1);

// Greedy farthest-visible smoothing of a raw cell path (see the header comment).
std::vector<Vec2> SmoothPath(const NavGrid& grid, const NavOverlay* overlay, const std::vector<Vec2>& raw);

}  // namespace fb
