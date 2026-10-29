// StuckRecovery.cpp - see StuckRecovery.h.
#include "StuckRecovery.h"

#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../nav/PathPlanner.h"
#include "../world/Projection.h"
#include "Mover.h"

#include <algorithm>
#include <cmath>

namespace fb {

const char* StuckCauseName(StuckCause c) {
    switch (c) {
        case StuckCause::None:         return Key("stuck_cause_none");
        case StuckCause::Displacement: return Key("stuck_cause_not_moving");
        case StuckCause::NoProgress:   return Key("stuck_cause_no_progress");
    }
    return Key("common_unknown");
}

const char* RecoveryStepName(RecoveryStep s) {
    switch (s) {
        case RecoveryStep::None:        return Key("stuck_step_none");
        case RecoveryStep::RepathBlock: return Key("stuck_step_repath");
        case RecoveryStep::SideStep:    return Key("stuck_step_side_step");
        case RecoveryStep::Door:        return Key("stuck_step_door");
        case RecoveryStep::Backtrack:   return Key("stuck_step_backtrack");
        case RecoveryStep::Dodge:       return Key("stuck_step_dodge");
        case RecoveryStep::EscapeProbe: return Key("stuck_step_escape");
        case RecoveryStep::GiveUp:      return Key("stuck_step_give_up");
    }
    return Key("common_unknown");
}

// ---------------------------------------------------------------------------
// Detector
// ---------------------------------------------------------------------------

void StuckDetector::Reset(uint64_t now) {
    m_active = false;
    m_activeSinceMs = now;
    m_anchorMs = now;
    m_cause = StuckCause::None;
}

void StuckDetector::Push(uint64_t now, const Vec2& pos, int historyMs) {
    if (m_history.empty() || now - m_history.back().ms >= 100) m_history.push_back({now, pos});
    else m_history.back().pos = pos;   // keep the newest position in the last slot
    while (!m_history.empty() && now - m_history.front().ms > static_cast<uint64_t>(historyMs)) m_history.pop_front();
}

bool StuckDetector::SampleAt(uint64_t t, Sample& out) const {
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (it->ms <= t) {
            out = *it;
            return true;
        }
    }
    return false;
}

float StuckDetector::MovedWithin(uint64_t now, int ms) const {
    if (m_history.empty() || now < static_cast<uint64_t>(ms)) return 0.f;
    Sample old;
    if (!SampleAt(now - static_cast<uint64_t>(ms), old)) return 0.f;
    return Dist(old.pos, m_history.back().pos);
}

Vec2 StuckDetector::PathDirection(const std::vector<Vec2>& path, const Vec2& pos, const Vec2& goal) {
    if (path.size() >= 2) {
        size_t seg = 0;
        PolylineDistance(pos, path, &seg);
        const Vec2 d = Normalize(path[seg + 1] - path[seg]);
        if (Length2(d) > 0.f) return d;
    }
    return Normalize(goal - pos);
}

bool StuckDetector::Update(uint64_t now, const Vec2& pos, const Vec2& goal, const std::vector<Vec2>& path,
                           float arrivalRadius, bool active, bool ignitionPending, const StuckParams& p) {
    Push(now, pos, p.historyMs);
    const float dist = Dist(pos, goal);
    const bool wants = active && !ignitionPending && dist > arrivalRadius;
    if (!wants) {
        m_active = false;
        return false;
    }
    if (!m_active) {
        m_active = true;
        m_activeSinceMs = now;
        m_anchorMs = now;
        m_lastPos = pos;
        m_along = 0.f;
        m_anchorAlong = 0.f;
        m_cause = StuckCause::None;
        return false;
    }
    // Progress along the planned path: this tick's step projected on the path's
    // direction where the bot stands (walking a detour that leads away from the
    // goal is progress; a straight-line measure to the goal would call it stuck).
    m_along += Dot(pos - m_lastPos, PathDirection(path, pos, goal));
    m_lastPos = pos;
    // Displacement window: only once the bot has been trying for a whole window.
    if (now - m_activeSinceMs >= static_cast<uint64_t>(p.windowMs)) {
        Sample old;
        if (SampleAt(now - static_cast<uint64_t>(p.windowMs), old) && old.ms >= m_activeSinceMs &&
            Dist(old.pos, pos) < p.minDisplacement) {
            m_cause = StuckCause::Displacement;
            m_activeSinceMs = now;
            m_anchorMs = now;
            m_anchorAlong = m_along;
            return true;
        }
    }
    if (m_along >= m_anchorAlong + p.progressAlongCells) {
        m_anchorMs = now;
        m_anchorAlong = m_along;
    } else if (now - m_anchorMs >= static_cast<uint64_t>(p.noProgressMs)) {
        m_cause = StuckCause::NoProgress;
        m_activeSinceMs = now;
        m_anchorMs = now;
        m_anchorAlong = m_along;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Ladder
// ---------------------------------------------------------------------------

RecoveryStep RecoveryLadder::Next(const LadderOptions& o) {
    static constexpr RecoveryStep kOrder[] = {RecoveryStep::RepathBlock, RecoveryStep::SideStep,
                                              RecoveryStep::Door,        RecoveryStep::Backtrack,
                                              RecoveryStep::Dodge,       RecoveryStep::EscapeProbe};
    constexpr int kCount = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
    while (m_index < kCount) {
        const RecoveryStep s = kOrder[m_index++];
        const bool applies = (s == RecoveryStep::Door && o.door) || (s == RecoveryStep::Backtrack && o.backtrack) ||
                             (s == RecoveryStep::Dodge && o.dodge) || (s == RecoveryStep::EscapeProbe && o.escape) ||
                             s == RecoveryStep::RepathBlock || s == RecoveryStep::SideStep;
        if (applies) {
            m_current = s;
            return s;
        }
    }
    m_current = RecoveryStep::GiveUp;
    return m_current;
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

void StuckRecovery::Reset(uint64_t now) {
    m_detector.Reset(now);
    m_ladder.Reset();
    m_step = RecoveryStep::None;
    m_stepUntilMs = 0;
    m_stepSteers = false;
    m_goodSinceMs = now;
    m_gaveUp = false;
}

void StuckRecovery::NoteIdle(const World& w, uint64_t now) {
    static const std::vector<Vec2> kNoPath;
    m_detector.Update(now, w.self.grid, w.self.grid, kNoPath, 1.f, false, false, m_cfg.detect);
    if (m_step != RecoveryStep::None && m_step != RecoveryStep::GiveUp) {
        m_step = RecoveryStep::None;
        m_stepSteers = false;
    }
    m_goodSinceMs = now;
}

Vec2 StuckRecovery::PathDirection(const World& w, const PathPlanner& planner, const Vec2& goal) const {
    const auto& path = planner.Path();
    if (path.size() >= 2) {
        size_t seg = 0;
        PolylineDistance(w.self.grid, path, &seg);
        const Vec2 d = Normalize(path[(std::min)(seg + 1, path.size() - 1)] - w.self.grid);
        if (Length2(d) > 0.f) return d;
    }
    const Vec2 d = Normalize(goal - w.self.grid);
    return Length2(d) > 0.f ? d : Vec2{1.f, 0.f};
}

const Interactable* StuckRecovery::FindDoor(const World& w, const PathPlanner& planner) const {
    const Interactable* best = nullptr;
    float bestD = m_cfg.doorSearchCells;
    for (const auto& it : w.interactables) {
        if (it.kind != InteractKind::Door || it.open || !it.visibleNow) continue;
        const float d = Dist(it.grid, w.self.grid);
        if (d > bestD) continue;
        const auto& path = planner.Path();
        if (path.size() >= 2 && PolylineDistance(it.grid, path) > 6.f) continue;
        best = &it;
        bestD = d;
    }
    return best;
}

bool StuckRecovery::FindBacktrack(const World& w, const PathPlanner& planner, Vec2& out) const {
    const auto& h = m_detector.History();
    for (auto it = h.rbegin(); it != h.rend(); ++it) {
        if (Dist(it->pos, w.self.grid) < m_cfg.backtrackMinCells) continue;
        if (planner.Grid() && !planner.LineOfSight(w.self.grid, it->pos, 1)) continue;
        out = it->pos;
        return true;
    }
    return false;
}

bool StuckRecovery::FindEscape(const World& w, const PathPlanner& planner, const Vec2& goal, Vec2& out) const {
    if (!planner.Grid()) return false;
    const float base = Dist(w.self.grid, goal);
    float bestScore = -1e9f;
    bool found = false;
    for (int r : {10, 20, 30}) {
        for (int k = 0; k < 8; ++k) {
            const float a = static_cast<float>(k) * kPi / 4.f;
            const Vec2 c = w.self.grid + Vec2{std::cos(a), std::sin(a)} * static_cast<float>(r);
            if (!planner.IsWalkable(c) || !planner.LineOfSight(w.self.grid, c, 1)) continue;
            const float score = base - Dist(c, goal) - static_cast<float>(r) * 0.05f;
            if (score > bestScore) {
                bestScore = score;
                out = c;
                found = true;
            }
        }
    }
    return found;
}

bool StuckRecovery::StartStep(const World& w, Mover& mover, PathPlanner& planner, Actuator& act, BotLog& log,
                              const Vec2& goal, uint64_t now) {
    LadderOptions o;
    const Interactable* door = FindDoor(w, planner);
    Vec2 back, esc;
    o.door = door != nullptr;
    o.backtrack = FindBacktrack(w, planner, back);
    o.dodge = mover.HasDodgeKey();
    o.escape = FindEscape(w, planner, goal, esc);
    m_step = m_ladder.Next(o);
    m_stepSteers = false;
    const Vec2 dir = PathDirection(w, planner, goal);
    log.Add(LogCat::Stuck, TrF("log_stuck", {{"cause", Tr(StuckCauseName(m_detector.Cause()))},
                                             {"x", Num("%.0f", w.self.grid.x)},
                                             {"y", Num("%.0f", w.self.grid.y)},
                                             {"step", Tr(RecoveryStepName(m_step))}}));
    switch (m_step) {
        case RecoveryStep::RepathBlock: {
            const Vec2 ahead = w.self.grid + dir * 3.f;
            planner.AddTemporaryBlock(ahead, m_cfg.blockRadius, now + static_cast<uint64_t>(m_cfg.blockMs));
            planner.RequestReplan();
            m_stepUntilMs = now;   // instantaneous
            return true;
        }
        case RecoveryStep::SideStep: {
            m_stepTarget = w.self.grid + Perp(dir) * (6.f * static_cast<float>(m_sideSign));
            m_sideSign = -m_sideSign;
            m_stepUntilMs = now + static_cast<uint64_t>(m_cfg.sideStepMs);
            m_stepSteers = true;
            return true;
        }
        case RecoveryStep::Door: {
            mover.Stop(act);
            Vec2 screen;
            if (door && ProjectLifted(w.proj, door->world, door->modelHeight * 0.5f, screen) && OnScreen(w.proj, screen, 4.f)) {
                act.WorldClick(screen, 120, 50, VK_LBUTTON, nullptr, 0);
                log.Add(LogCat::Stuck, TrF("log_stuck_door", {{"id", Num("%u", door->id)},
                                                              {"x", Num("%.0f", door->grid.x)},
                                                              {"y", Num("%.0f", door->grid.y)}}));
            }
            planner.RequestReplan();
            m_stepUntilMs = now + 400;   // let it open
            return true;
        }
        case RecoveryStep::Backtrack:
            m_stepTarget = back;
            m_stepUntilMs = now + 1500;
            m_stepSteers = true;
            return true;
        case RecoveryStep::Dodge: {
            m_dodgeAnimAtPress = w.self.animationId;
            const ActResult r = mover.Dodge(w, dir, act);
            m_dodgeCheckUntil = now + 400;
            if (r != ActResult::Ok) log.Add(LogCat::Stuck, TrF("log_stuck_dodge_not_sent", {{"why", Tr(ActResultName(r))}}));
            m_stepUntilMs = now + 450;
            return true;
        }
        case RecoveryStep::EscapeProbe:
            m_stepTarget = esc;
            m_stepUntilMs = now + 2000;
            m_stepSteers = true;
            return true;
        case RecoveryStep::GiveUp:
        case RecoveryStep::None:
            break;
    }
    return false;
}

RecoveryState StuckRecovery::Tick(const World& w, Mover& mover, PathPlanner& planner, Actuator& act, BotLog& log,
                                  const Vec2& goal, float arrivalRadius, bool active, uint64_t now) {
    if (m_gaveUp) return RecoveryState::GaveUp;
    // A running step.
    if (m_step != RecoveryStep::None && m_step != RecoveryStep::GiveUp) {
        if (m_step == RecoveryStep::Dodge && m_dodgeCheckUntil != 0) {
            if (w.self.animationId != m_dodgeAnimAtPress) {
                log.Add(LogCat::Stuck, TrF("log_stuck_dodge_started", {{"animation", Num("%d", w.self.animationId)}}));
                m_dodgeCheckUntil = 0;
            } else if (now >= m_dodgeCheckUntil) {
                log.Add(LogCat::Stuck, Tr("log_stuck_dodge_swallowed"));
                m_dodgeCheckUntil = 0;
            }
        }
        if (now < m_stepUntilMs) {
            if (m_stepSteers) {
                if (Dist(w.self.grid, m_stepTarget) < 1.5f) {
                    m_stepUntilMs = now;
                } else {
                    mover.SteerToward(w, m_stepTarget, act, now);
                }
            }
            return RecoveryState::Recovering;
        }
        // Step finished.
        if (m_stepSteers) mover.Stop(act);
        if (m_step != RecoveryStep::RepathBlock) planner.RequestReplan();
        m_step = RecoveryStep::None;
        m_stepSteers = false;
        m_detector.Reset(now);
    }

    const bool stuck = m_detector.Update(now, w.self.grid, goal, planner.Path(), arrivalRadius, active,
                                         mover.IgnitionPending(now), m_cfg.detect);
    if (!stuck) {
        // Good progress for a while resets the ladder.
        const bool moving = active && m_detector.MovedWithin(now, m_cfg.detect.windowMs) >= m_cfg.detect.minDisplacement * 2.f;
        if (!moving) m_goodSinceMs = now;
        else if (m_ladder.Index() > 0 && now - m_goodSinceMs >= static_cast<uint64_t>(m_cfg.ladderResetMs)) {
            log.Add(LogCat::Stuck, Tr("log_stuck_ladder_reset"));
            m_ladder.Reset();
        }
        return RecoveryState::Normal;
    }
    ++m_episodes;
    m_marks.push_back(w.self.grid);
    if (m_marks.size() > 32) m_marks.erase(m_marks.begin());
    m_goodSinceMs = now;
    if (!StartStep(w, mover, planner, act, log, goal, now)) {
        m_gaveUp = true;
        mover.Stop(act);
        log.Add(LogCat::Stuck, Tr("log_stuck_gave_up"));
        return RecoveryState::GaveUp;
    }
    return m_step == RecoveryStep::RepathBlock ? RecoveryState::Normal : RecoveryState::Recovering;
}

}  // namespace fb
