// Mover.h - steering: the held move key + a cursor placed in the walking
// direction (design §5.5). The old bot's movement model, fixed: client ->
// desktop conversion, cursor a fixed pixel distance from the player instead of
// on a waypoint next to the feet, terrain height for every projected point,
// re-aim only when the target moved, no blocking trajectories while walking.
//
//  - Lookahead = the farthest path point within lookaheadCells (along the path)
//    that is in line of sight from the player.
//  - Cursor = player screen point + direction x cursorDistancePx, clamped into
//    the safe rect (never onto the HUD / flask bar).
//  - Activation click: after the game regains focus or after a zone-in, one
//    click 40 px below the feet before holding the move key (PoE swallows held
//    keys until a real click lands).
//  - Ignition gate: after a zone-in, stuck checks wait until the character moved
//    once or ignitionTimeoutMs passed (another player's hideout streams in).
//  - Sprint: hold the Dodge/Sprint key on long, straight, enemy-free stretches.
#pragma once

#include "../input/Actuator.h"
#include "../world/Projection.h"
#include "../world/World.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace fb {

class PathPlanner;

struct MoverParams {
    int   moveKey = 'T';
    bool  clickToMove = false;
    int   sprintKey = 0;
    int   dodgeKey = 0;
    float lookaheadCells = 14.f;
    int   cursorDistancePx = 180;
    int   reaimMs = 50;
    int   reaimPx = 6;
    int   reaimJitterMs = 20;
    bool  activationClick = true;
    bool  sprint = true;
    float sprintDistance = 55.f;
    float sprintNoEnemyRadius = 35.f;
    int   sprintMaxHoldMs = 8000;
    int   sprintCooldownMs = 3000;
    int   ignitionTimeoutMs = 7000;
};

enum class MoveStatus : uint8_t {
    Idle = 0,     // not moving (move key released)
    Moving,       // move key held, cursor aimed
    Activating,   // the activation click was sent this tick
    Deferred,     // another plugin owns the cursor: held still this tick
    NoProjection, // the player or the target cannot be projected
    NoKey,        // no move key configured
    Aborted       // the actuator refused (game not in front / stop pending)
};

const char* MoveStatusName(MoveStatus s);

class Mover {
public:
    void Configure(const MoverParams& p) { m_p = p; }
    const MoverParams& Params() const { return m_p; }

    // Call once per tick before any steering: tracks focus and zone changes
    // (activation click, ignition gate) and sprint timers.
    void BeginTick(const World& w, uint64_t now);

    // Steer along `path` (planner polyline, first point ~ the player) toward
    // `goal`. `planner` (optional) supplies line of sight for the lookahead.
    MoveStatus Steer(const World& w, const std::vector<Vec2>& path, const Vec2& goal, Actuator& act,
                     const PathPlanner* planner, uint64_t now);
    // Straight at `target` (recovery moves, short approaches).
    MoveStatus SteerToward(const World& w, const Vec2& target, Actuator& act, uint64_t now);

    void Stop(Actuator& act);                                  // releases move + sprint keys
    ActResult StopPrecise(const World& w, Actuator& act);      // + taps the move key on the feet
    // Dodge roll along `dirGrid` (dodge key, else a tap of the sprint key).
    ActResult Dodge(const World& w, const Vec2& dirGrid, Actuator& act);
    bool HasDodgeKey() const { return m_p.dodgeKey != 0 || m_p.sprintKey != 0; }

    bool IsMoving() const { return m_moving; }
    bool Sprinting() const { return m_sprinting; }
    bool IgnitionPending(uint64_t now) const;
    void RequestActivationClick() { m_needActivation = true; }

    bool HasLookahead() const { return m_haveLookahead; }
    Vec2 Lookahead() const { return m_lookahead; }
    bool HasCursorTarget() const { return m_haveCursorTarget; }
    Vec2 CursorTarget() const { return m_cursorTarget; }

    // --- pure helpers (tested) -----------------------------------------
    // Farthest point within `lookahead` path-length of the point nearest to the
    // player that `los(player, point)` accepts (the next point when none does).
    static Vec2 PickLookahead(const std::vector<Vec2>& path, const Vec2& player, float lookahead,
                              const std::function<bool(const Vec2&, const Vec2&)>& los);
    // Cursor point `distPx` from the player toward the target, clamped into `safe`.
    static Vec2 CursorToward(const Vec2& playerScreen, const Vec2& targetScreen, float distPx, const SafeRect& safe);
    // Whether a stretch of path ahead is near-straight (all turns < maxTurnRad over `cells`).
    static bool StraightAhead(const std::vector<Vec2>& path, const Vec2& player, float cells, float maxTurnRad);

private:
    MoveStatus AimAndHold(const World& w, const Vec2& lookaheadGrid, bool isGoal, Actuator& act, uint64_t now);
    void UpdateSprint(const World& w, const std::vector<Vec2>& path, const Vec2& goal, Actuator& act, uint64_t now);
    void ReleaseSprint(Actuator& act, uint64_t now);
    int  MoveVk() const { return m_p.clickToMove ? VK_LBUTTON : m_p.moveKey; }

    MoverParams m_p;
    bool     m_moving = false;
    bool     m_needActivation = true;
    bool     m_wasForeground = false;
    uint32_t m_area = 0;
    bool     m_haveArea = false;
    uint64_t m_zoneInMs = 0;
    Vec2     m_zoneInPos;
    bool     m_ignited = true;
    uint64_t m_lastAimMs = 0;
    int      m_aimJitterMs = 0;
    bool     m_haveLookahead = false;
    Vec2     m_lookahead;
    bool     m_haveCursorTarget = false;
    Vec2     m_cursorTarget;
    bool     m_sprinting = false;
    uint64_t m_sprintStartMs = 0;
    uint64_t m_sprintStopMs = 0;
};

}  // namespace fb
