// WorldClick.h - walk up to a world object and click it (design §5.7). Shared
// by travel, loot, stash and the debug walk. Pure: no SDK (the hover check
// comes through BotContext::hooks).
//
// Phases, one per tick or so:
//   Approach  plan + steer until within interactRange (stuck recovery on);
//   Settle    move key released, wait until the character stands (<= 250 ms);
//   Click     project the object (z + modelHeight x heightFactor for tall
//             objects, or `screenOffsetY` px up for ground items), hover with a
//             dwell, read Targetable.IsTargetedByPlayer (up to reHovers small
//             re-hovers when false, then press anyway), press-hold. The click
//             itself blocks the worker for dwell + hold (~200 ms, cancellable).
// The caller re-resolves the target every tick (UpdateTarget) and decides what
// "success" means afterwards (area change, item gone, panel open).
#pragma once

#include "../brain/Task.h"

#include <Windows.h>

#include <string>

namespace fb {

struct WorldClickTarget {
    uint32_t  id = 0;
    Vec2      grid;
    Vec3      world;
    float     modelHeight = 0.f;
    uintptr_t targetableAddr = 0;
    float     screenOffsetY = 0.f;   // > 0: click this many px above the ground point (item labels)
};

struct WorldClickParams {
    float interactRange = 6.f;
    int   dwellMs = 120;
    int   holdMs = 50;
    int   reHovers = 2;
    float heightFactor = 0.5f;
    int   approachTimeoutMs = 20000;
    int   button = VK_LBUTTON;
};

enum class WorldClickStatus : uint8_t { Running = 0, Clicked, Failed };

class WorldClick {
public:
    enum class Phase : uint8_t { Idle = 0, Approach, Settle, Click, Done, Failed };

    void Start(const WorldClickTarget& target, const WorldClickParams& params, uint64_t now);
    void UpdateTarget(const WorldClickTarget& target) { m_target = target; }
    WorldClickStatus Tick(BotContext& ctx);
    void Abort(BotContext& ctx);

    Phase CurrentPhase() const { return m_phase; }
    bool Active() const { return m_phase == Phase::Approach || m_phase == Phase::Settle || m_phase == Phase::Click; }
    // True while the cursor must not be touched by anything else (hover/click).
    bool CursorCritical() const { return m_phase == Phase::Settle || m_phase == Phase::Click; }
    // Why the click failed: a key (lang/en.json; click_fail_* or an act_* result), "" before any failure.
    const char* FailReason() const { return m_fail; }
    const WorldClickTarget& Target() const { return m_target; }

private:
    WorldClickStatus Fail(BotContext& ctx, const char* why);

    WorldClickTarget m_target;
    WorldClickParams m_params;
    Phase    m_phase = Phase::Idle;
    uint64_t m_startMs = 0;
    uint64_t m_settleMs = 0;
    Vec2     m_settlePos;
    const char* m_fail = "";
};

}  // namespace fb
