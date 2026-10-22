// Mover.cpp - see Mover.h.
#include "Mover.h"

#include "../nav/PathPlanner.h"

#include <algorithm>
#include <cmath>

namespace fb {

const char* MoveStatusName(MoveStatus s) {
    switch (s) {
        case MoveStatus::Idle:         return "idle";
        case MoveStatus::Moving:       return "moving";
        case MoveStatus::Activating:   return "activation click";
        case MoveStatus::Deferred:     return "cursor busy (other plugin)";
        case MoveStatus::NoProjection: return "cannot project";
        case MoveStatus::NoKey:        return "no move key";
        case MoveStatus::Aborted:      return "aborted";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

Vec2 Mover::PickLookahead(const std::vector<Vec2>& path, const Vec2& player, float lookahead,
                          const std::function<bool(const Vec2&, const Vec2&)>& los) {
    if (path.empty()) return player;
    if (path.size() == 1) return path[0];
    size_t seg = 0;
    PolylineDistance(player, path, &seg);
    float t = 0.f;
    PointSegmentDistance(player, path[seg], path[seg + 1], &t);
    const Vec2 onPath = Lerp(path[seg], path[seg + 1], t);
    // Walk forward from the projection of the player onto the path.
    std::vector<Vec2> candidates;
    float acc = 0.f;
    Vec2 prev = onPath;
    for (size_t i = seg + 1; i < path.size(); ++i) {
        const float d = Dist(prev, path[i]);
        if (acc + d >= lookahead) {
            const float need = lookahead - acc;
            candidates.push_back(d > 1e-4f ? Lerp(prev, path[i], need / d) : path[i]);
            break;
        }
        acc += d;
        candidates.push_back(path[i]);
        prev = path[i];
    }
    if (candidates.empty()) return path.back();
    if (!los) return candidates.back();
    for (size_t k = candidates.size(); k-- > 0;)
        if (los(player, candidates[k])) return candidates[k];
    return candidates.front();
}

Vec2 Mover::CursorToward(const Vec2& playerScreen, const Vec2& targetScreen, float distPx, const SafeRect& safe) {
    const Vec2 dir = Normalize(targetScreen - playerScreen);
    if (Length2(dir) < 1e-6f) return safe.Clamp(playerScreen);
    return ClampAlongRay(safe, playerScreen, playerScreen + dir * distPx);
}

bool Mover::StraightAhead(const std::vector<Vec2>& path, const Vec2& player, float cells, float maxTurnRad) {
    if (path.size() < 2) return true;
    size_t seg = 0;
    PolylineDistance(player, path, &seg);
    float acc = 0.f;
    Vec2 baseDir = Normalize(path[seg + 1] - path[seg]);
    for (size_t i = seg + 1; i + 1 < path.size() && acc < cells; ++i) {
        const Vec2 d = path[i + 1] - path[i];
        acc += Length(d);
        if (AngleBetween(baseDir, d) > maxTurnRad) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Ticking
// ---------------------------------------------------------------------------

void Mover::BeginTick(const World& w, uint64_t now) {
    const bool fg = w.gate.foregroundExact;
    if (fg && !m_wasForeground) m_needActivation = true;  // focus regained: PoE swallows the first click
    m_wasForeground = fg;
    if (!m_haveArea || w.gate.areaCounter != m_area) {
        if (m_haveArea) {
            m_needActivation = true;
            m_zoneInMs = now;
            m_zoneInPos = w.self.grid;
            m_ignited = false;
            m_moving = false;
            m_sprinting = false;
        }
        m_area = w.gate.areaCounter;
        m_haveArea = true;
    }
    if (!m_ignited) {
        if (Dist(w.self.grid, m_zoneInPos) > 1.5f) m_ignited = true;
        if (now - m_zoneInMs >= static_cast<uint64_t>(m_p.ignitionTimeoutMs)) m_ignited = true;
    }
}

bool Mover::IgnitionPending(uint64_t now) const {
    if (m_ignited) return false;
    return now - m_zoneInMs < static_cast<uint64_t>(m_p.ignitionTimeoutMs);
}

void Mover::Stop(Actuator& act) {
    if (m_moving || act.MoveKeyHeld()) act.SetMoveKey(MoveVk(), false);
    if (m_sprinting) {
        act.KeyUp(m_p.sprintKey);
        m_sprinting = false;
    }
    m_moving = false;
    m_haveLookahead = false;
    m_haveCursorTarget = false;
}

ActResult Mover::StopPrecise(const World& w, Actuator& act) {
    Stop(act);
    if (m_p.moveKey <= 0 || m_p.clickToMove) return ActResult::Ok;
    Vec2 feet;
    if (!ProjectWorld(w.proj, w.self.world, feet)) return ActResult::Refused;
    return act.PressAt(m_p.moveKey, feet, 20);
}

ActResult Mover::Dodge(const World& w, const Vec2& dirGrid, Actuator& act) {
    const int vk = m_p.dodgeKey != 0 ? m_p.dodgeKey : m_p.sprintKey;
    if (vk == 0) return ActResult::Refused;
    Stop(act);
    Vec2 dir = Normalize(dirGrid);
    if (Length2(dir) < 1e-6f) dir = {1.f, 0.f};
    Vec2 playerScreen, aimScreen;
    if (!ProjectWorld(w.proj, w.self.world, playerScreen)) return ActResult::Refused;
    const Vec2 aimGrid = w.self.grid + dir * 18.f;
    if (!ProjectGrid(w.proj, aimGrid, w.self.world.z, aimScreen)) return ActResult::Refused;
    const SafeRect safe = MakeSafeRect(w.proj.screenW, w.proj.screenH);
    const Vec2 cursor = CursorToward(playerScreen, aimScreen, 200.f, safe);
    const ActResult r = act.MoveCursor(cursor);
    if (r != ActResult::Ok) return r;
    return act.TapKey(vk, 40);
}

MoveStatus Mover::AimAndHold(const World& w, const Vec2& lookaheadGrid, bool isGoal, Actuator& act, uint64_t now) {
    if (MoveVk() <= 0) return MoveStatus::NoKey;
    if (act.CursorBusyByOther()) {
        // Pick My Loot owns the cursor: hold still rather than walk toward its clicks.
        Stop(act);
        return MoveStatus::Deferred;
    }
    // Nothing projects (screen size 0, a bad camera read): release the move key
    // held from an earlier tick, or the character runs on toward the stale cursor.
    Vec2 playerScreen, targetScreen;
    if (!ProjectWorld(w.proj, w.self.world, playerScreen)) {
        Stop(act);
        return MoveStatus::NoProjection;
    }
    if (!ProjectGrid(w.proj, lookaheadGrid, w.self.world.z, targetScreen)) {
        Stop(act);
        return MoveStatus::NoProjection;
    }
    m_haveLookahead = true;
    m_lookahead = lookaheadGrid;
    const SafeRect safe = MakeSafeRect(w.proj.screenW, w.proj.screenH);

    float distPx = static_cast<float>(m_p.cursorDistancePx);
    if (isGoal) {
        // Close to the final point: do not throw the cursor far past it.
        const float toTarget = Dist(playerScreen, targetScreen);
        distPx = std::clamp(toTarget, 70.f, distPx);
    }
    const Vec2 cursor = CursorToward(playerScreen, targetScreen, distPx, safe);

    if (m_needActivation && m_p.activationClick) {
        // One real click just below the feet before holding the key.
        const Vec2 below = safe.Clamp(Vec2{playerScreen.x, playerScreen.y + 40.f});
        const ActResult r = act.UiClick(below, VK_LBUTTON, false);
        if (r == ActResult::Aborted) return MoveStatus::Aborted;
        m_needActivation = false;
        m_lastAimMs = 0;
        return MoveStatus::Activating;
    }
    m_needActivation = false;

    const bool moved = !m_haveCursorTarget || Dist(cursor, m_cursorTarget) > static_cast<float>(m_p.reaimPx);
    const bool timeUp = now - m_lastAimMs >= static_cast<uint64_t>(m_p.reaimMs + m_aimJitterMs);
    if (moved || timeUp || !act.HasCursor()) {
        if (act.MoveCursor(cursor) != ActResult::Ok) {
            m_moving = false;
            return MoveStatus::Aborted;
        }
        m_cursorTarget = cursor;
        m_haveCursorTarget = true;
        m_lastAimMs = now;
        m_aimJitterMs = act.Random().UniformInt(0, m_p.reaimJitterMs);
    }
    if (act.SetMoveKey(MoveVk(), true) != ActResult::Ok) {
        m_moving = false;
        return MoveStatus::Aborted;
    }
    // The bot owns the cursor while it steers: Auto My Aim backs off.
    act.MarkCursorBusy(300);
    m_moving = true;
    return MoveStatus::Moving;
}

void Mover::ReleaseSprint(Actuator& act, uint64_t now) {
    if (!m_sprinting) return;
    act.KeyUp(m_p.sprintKey);
    m_sprinting = false;
    m_sprintStopMs = now;
}

void Mover::UpdateSprint(const World& w, const std::vector<Vec2>& path, const Vec2& goal, Actuator& act, uint64_t now) {
    if (!m_p.sprint || m_p.sprintKey == 0) return;
    const float remaining = path.size() >= 2 ? RemainingLength(w.self.grid, path) : Dist(w.self.grid, goal);
    const bool enemies = m_p.sprintNoEnemyRadius > 0.f && w.CountMonstersNear(w.self.grid, m_p.sprintNoEnemyRadius) > 0;
    if (m_sprinting) {
        const bool tooLong = now - m_sprintStartMs >= static_cast<uint64_t>(m_p.sprintMaxHoldMs);
        if (remaining < m_p.sprintDistance * 0.6f || enemies || tooLong) ReleaseSprint(act, now);
        return;
    }
    if (m_sprintStopMs != 0 && now - m_sprintStopMs < static_cast<uint64_t>(m_p.sprintCooldownMs)) return;
    if (remaining <= m_p.sprintDistance || enemies) return;
    if (!StraightAhead(path, w.self.grid, 20.f, 25.f * kPi / 180.f)) return;
    if (act.KeyDown(m_p.sprintKey) == ActResult::Ok) {
        m_sprinting = true;
        m_sprintStartMs = now;
    }
}

MoveStatus Mover::Steer(const World& w, const std::vector<Vec2>& path, const Vec2& goal, Actuator& act,
                        const PathPlanner* planner, uint64_t now) {
    if (path.size() < 2) return SteerToward(w, goal, act, now);
    std::function<bool(const Vec2&, const Vec2&)> los;
    if (planner && planner->Grid()) los = [planner](const Vec2& a, const Vec2& b) { return planner->LineOfSight(a, b, 1); };
    const Vec2 look = PickLookahead(path, w.self.grid, m_p.lookaheadCells, los);
    const bool isGoal = Dist(look, path.back()) < 0.5f;
    const MoveStatus st = AimAndHold(w, look, isGoal, act, now);
    if (st == MoveStatus::Moving) UpdateSprint(w, path, goal, act, now);
    else if (st != MoveStatus::Activating) ReleaseSprint(act, now);
    return st;
}

MoveStatus Mover::SteerToward(const World& w, const Vec2& target, Actuator& act, uint64_t now) {
    const MoveStatus st = AimAndHold(w, target, true, act, now);
    if (st != MoveStatus::Moving && st != MoveStatus::Activating) ReleaseSprint(act, now);
    return st;
}

}  // namespace fb
