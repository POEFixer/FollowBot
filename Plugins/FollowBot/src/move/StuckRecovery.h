// StuckRecovery.h - stuck detection and the layered recovery ladder
// (design §5.5). The detector and the ladder are pure (tested); the executor
// drives Mover / PathPlanner / Actuator for one step at a time.
//
// Stuck = displacement < minDisplacement over windowMs while moving toward a
// goal farther than the arrival radius, or no progress along the planned path
// for noProgressMs (progress = progressAlongCells walked along the path's own
// direction where the bot stands, so a detour that leads away from the goal
// still counts; without a path, along the direction to the goal). The ladder
// (each step logged; reset after ladderResetMs of good progress):
//   1 RepathBlock  temporary block (blockRadius, blockMs) on the cell ahead + repath
//   2 SideStep     sideStepMs perpendicular to the path (alternating sides)
//   3 Door         click a closed door / blockage within doorSearchCells near the path
//   4 Backtrack    walk to a position of the last 10 s >= backtrackMinCells away with LOS
//   5 Dodge        dodge roll along the path (dodge key, else sprint key)
//   6 EscapeProbe  walkable cells at 10 / 20 / 30 cells in 8 directions: best progress
//   7 GiveUp       the caller applies its own policy (loot blacklists the item,
//                  follow retries after followRetryMs, travel fails after 3 cycles)
// Steps that do not apply (no door, no backtrack point, no dodge key, no escape
// cell) are skipped.
#pragma once

#include "../world/World.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace fb {

class Actuator;
class BotLog;
class Mover;
class PathPlanner;

struct StuckParams {
    float minDisplacement = 1.5f;
    int   windowMs = 1200;
    int   noProgressMs = 2500;
    float progressAlongCells = 2.f;   // walked along the planned path since the last progress
    int   historyMs = 10000;
};

enum class StuckCause : uint8_t { None = 0, Displacement, NoProgress };
const char* StuckCauseName(StuckCause c);

class StuckDetector {
public:
    struct Sample {
        uint64_t ms = 0;
        Vec2     pos;
    };

    void Reset(uint64_t now);
    // Call every tick. `active` = the bot is trying to move toward `goal` along
    // `path` (the planner's polyline; empty = straight at the goal);
    // `ignitionPending` = a zone-in has not let the character move yet.
    // Returns true once per stuck episode (the windows restart afterwards).
    // Progress is each tick's step projected on the path's direction where the
    // bot stands, summed: a re-plan or a moving goal changes the direction, not
    // the progress made so far, and circling or sliding along a wall sums to
    // (almost) nothing.
    bool Update(uint64_t now, const Vec2& pos, const Vec2& goal, const std::vector<Vec2>& path, float arrivalRadius,
                bool active, bool ignitionPending, const StuckParams& p);
    // The direction of travel at `pos`: along the nearest segment of `path`,
    // else toward `goal` (zero when both are degenerate).
    static Vec2 PathDirection(const std::vector<Vec2>& path, const Vec2& pos, const Vec2& goal);
    StuckCause Cause() const { return m_cause; }

    // Position history (thinned to ~100 ms), newest last; kept while inactive too.
    const std::deque<Sample>& History() const { return m_history; }
    // Displacement over the last `ms` (0 when the history is shorter).
    float MovedWithin(uint64_t now, int ms) const;

private:
    void Push(uint64_t now, const Vec2& pos, int historyMs);
    bool SampleAt(uint64_t t, Sample& out) const;   // newest sample at or before t

    std::deque<Sample> m_history;
    bool       m_active = false;
    uint64_t   m_activeSinceMs = 0;
    uint64_t   m_anchorMs = 0;       // the last progress (or episode)
    Vec2       m_lastPos;            // the previous active tick's position
    float      m_along = 0.f;        // walked along the path since activation (cells, signed)
    float      m_anchorAlong = 0.f;  // m_along at the last progress
    StuckCause m_cause = StuckCause::None;
};

enum class RecoveryStep : uint8_t { None = 0, RepathBlock, SideStep, Door, Backtrack, Dodge, EscapeProbe, GiveUp };
const char* RecoveryStepName(RecoveryStep s);

// What can be tried right now (the executor fills it in).
struct LadderOptions {
    bool door = false;
    bool backtrack = false;
    bool dodge = false;
    bool escape = true;
};

class RecoveryLadder {
public:
    // The next applicable step after the current one; GiveUp once exhausted
    // (and again until Reset).
    RecoveryStep Next(const LadderOptions& o);
    void Reset() { m_index = 0; }
    RecoveryStep Current() const { return m_current; }
    int Index() const { return m_index; }

private:
    int          m_index = 0;   // next step to consider (0 = RepathBlock)
    RecoveryStep m_current = RecoveryStep::None;
};

struct StuckConfig {
    StuckParams detect;
    int   ladderResetMs = 4000;
    int   blockRadius = 2;
    int   blockMs = 20000;
    int   sideStepMs = 450;
    float doorSearchCells = 12.f;
    float backtrackMinCells = 8.f;
};

enum class RecoveryState : uint8_t {
    Normal = 0,   // no recovery running: the caller steers normally
    Recovering,   // a step owns the movement this tick: the caller must not steer
    GaveUp        // the ladder is exhausted: apply the caller's give-up policy, then Reset()
};

class StuckRecovery {
public:
    void Configure(const StuckConfig& c) { m_cfg = c; }
    void Reset(uint64_t now);

    // Call every tick while the caller wants to move toward `goal` (`active`)
    // and also while idle (active = false keeps the history and cools down).
    RecoveryState Tick(const World& w, Mover& mover, PathPlanner& planner, Actuator& act, BotLog& log,
                       const Vec2& goal, float arrivalRadius, bool active, uint64_t now);

    // The caller is not trying to move this tick (arrived, waiting, holding):
    // keeps the position history, disarms the detector (no false "stuck" when
    // movement resumes after standing) and drops a running step.
    void NoteIdle(const World& w, uint64_t now);

    RecoveryStep CurrentStep() const { return m_step; }
    int  Episodes() const { return m_episodes; }
    const std::vector<Vec2>& StuckMarks() const { return m_marks; }
    const StuckDetector& Detector() const { return m_detector; }

private:
    bool StartStep(const World& w, Mover& mover, PathPlanner& planner, Actuator& act, BotLog& log,
                   const Vec2& goal, uint64_t now);
    bool FindBacktrack(const World& w, const PathPlanner& planner, Vec2& out) const;
    bool FindEscape(const World& w, const PathPlanner& planner, const Vec2& goal, Vec2& out) const;
    const Interactable* FindDoor(const World& w, const PathPlanner& planner) const;
    Vec2 PathDirection(const World& w, const PathPlanner& planner, const Vec2& goal) const;

    StuckConfig    m_cfg;
    StuckDetector  m_detector;
    RecoveryLadder m_ladder;
    RecoveryStep   m_step = RecoveryStep::None;
    uint64_t       m_stepUntilMs = 0;
    Vec2           m_stepTarget;
    bool           m_stepSteers = false;
    int            m_sideSign = 1;
    int            m_dodgeAnimAtPress = 0;
    uint64_t       m_dodgeCheckUntil = 0;
    uint64_t       m_goodSinceMs = 0;
    int            m_episodes = 0;
    bool           m_gaveUp = false;
    std::vector<Vec2> m_marks;
};

}  // namespace fb
