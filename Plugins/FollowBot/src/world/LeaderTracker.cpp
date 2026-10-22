// LeaderTracker.cpp - see LeaderTracker.h.
#include "LeaderTracker.h"

#include "../common/Strings.h"
#include "Interactables.h"

namespace fb {

int FindLeader(const std::vector<PlayerInfo>& others, const LeaderParams& p) {
    if (!p.name.empty()) {
        const std::string want = p.caseInsensitive ? FoldCase(p.name) : p.name;
        for (size_t i = 0; i < others.size(); ++i) {
            const std::string& n = others[i].name;
            if (n.empty()) continue;
            if ((p.caseInsensitive ? FoldCase(n) : n) == want) return static_cast<int>(i);
        }
        if (!p.autoPick) return -1;
    }
    if (p.autoPick && others.size() == 1) return 0;
    return -1;
}

namespace {

// Distance of object `it` to the leader's vanish evidence: his last position
// (within `lastRadius`) or his action destination / route end (within
// destNearObjectCells). Returns < 0 when the object does not qualify.
float EvidenceDistance(const Interactable& it, const VanishInputs& in, const LeaderParams& p, float lastRadius) {
    float best = -1.f;
    const float dLast = Dist(it.grid, in.lastGrid);
    if (dLast <= lastRadius) best = dLast;
    auto consider = [&](bool have, const Vec2& pt) {
        if (!have) return;
        const float d = Dist(it.grid, pt);
        if (d <= p.destNearObjectCells) {
            // Rank by how far he still was from it, so a clicked object far from
            // his last position still competes fairly.
            const float score = (std::min)(dLast, d);
            if (best < 0.f || score < best) best = score;
        }
    };
    consider(in.hasActionDest, in.actionDest);
    consider(in.hasRouteEnd, in.routeEnd);
    return best;
}

VanishKind KindFor(InteractKind k) {
    switch (k) {
        case InteractKind::AreaTransition: return VanishKind::Transition;
        case InteractKind::BossArena:      return VanishKind::BossArena;
        case InteractKind::Portal:         return VanishKind::Portal;
        case InteractKind::Checkpoint:     return VanishKind::Checkpoint;
        case InteractKind::Waypoint:       return VanishKind::Waypoint;
        default:                           return VanishKind::Unknown;
    }
}

}  // namespace

VanishInfo ClassifyVanish(const VanishInputs& in, const std::vector<Interactable>& interactables,
                          const LeaderParams& p, uint64_t nowMs) {
    VanishInfo v;
    v.active = true;
    v.atMs = nowMs;
    v.lastGrid = in.lastGrid;
    if (in.lastDead) {
        v.kind = VanishKind::Died;
        return v;
    }
    // Already at the edge of our entity range: he may just have walked out of it
    // next to an object he never used. Only stronger evidence counts then - his
    // destination / route end at the object, or his last position right on it.
    const bool farAway = in.lastDistance >= p.outOfRangeCells;
    const float lastRadius = farAway ? (std::min)(p.vanishRadius, p.farVanishRadius) : p.vanishRadius;
    const Interactable* bestTravel = nullptr;
    float bestTravelD = 1e9f;
    const Interactable* bestWarp = nullptr;
    float bestWarpD = 1e9f;
    for (const Interactable& it : interactables) {
        const bool travel = IsTravelKind(it.kind);
        const bool warp = IsWarpKind(it.kind);
        if (!travel && !warp) continue;
        if (it.kind == InteractKind::Portal && !(it.open && it.targetable)) continue;
        const float d = EvidenceDistance(it, in, p, lastRadius);
        if (d < 0.f) continue;
        if (travel && d < bestTravelD) { bestTravelD = d; bestTravel = &it; }
        if (warp && d < bestWarpD) { bestWarpD = d; bestWarp = &it; }
    }
    const Interactable* pick = bestTravel ? bestTravel : bestWarp;
    if (pick) {
        v.kind = KindFor(pick->kind);
        v.interactableId = pick->id;
        v.interactKind = pick->kind;
        v.interactGrid = pick->grid;
        return v;
    }
    v.kind = in.lastDistance >= p.outOfRangeCells ? VanishKind::OutOfRange : VanishKind::Unknown;
    return v;
}

void LeaderTracker::Reset() {
    m_state = LeaderState{};
    m_missingTicks = 0;
    m_missingSinceMs = 0;
    m_lastUpdateMs = 0;
    m_havePrev = false;
    m_haveArea = false;
}

void LeaderTracker::Update(uint64_t nowMs, uint32_t areaCounter, const Vec2& selfGrid, const LeaderObservation& obs,
                           const std::vector<Interactable>& interactables, const LeaderParams& p) {
    m_state.configured = !p.name.empty() || p.autoPick;
    const bool areaChanged = m_haveArea && areaCounter != m_area;
    m_area = areaCounter;
    m_haveArea = true;
    if (areaChanged) {
        // Our own area changed: nothing he did in the old area explains anything
        // here. Keep the vanish record for the UI until he reappears.
        m_missingTicks = 0;
        m_missingSinceMs = 0;
        m_havePrev = false;
        m_state.route.clear();
        m_state.hasActionDest = false;
        m_state.velocity = {};
        m_state.speed = 0.f;
    }

    if (obs.found) {
        const PlayerInfo& info = obs.info;
        // Velocity (cells/s), EMA; a teleport-sized jump resets it.
        if (m_havePrev && nowMs > m_prevMs) {
            const float dt = static_cast<float>(nowMs - m_prevMs) / 1000.f;
            const Vec2 delta = info.grid - m_prevGrid;
            if (Length(delta) > p.teleportCells) {
                m_state.velocity = {};
            } else if (dt > 0.f) {
                const Vec2 inst = delta / dt;
                m_state.velocity = m_state.velocity * (1.f - p.emaAlpha) + inst * p.emaAlpha;
            }
        }
        m_state.speed = Length(m_state.velocity);
        if (m_state.speed > 0.5f) m_state.heading = Normalize(m_state.velocity);
        m_havePrev = true;
        m_prevGrid = info.grid;
        m_prevMs = nowMs;

        m_state.visible = true;
        m_state.info = info;
        m_state.lastSeenMs = nowMs;
        m_state.lastGrid = info.grid;
        m_state.lastWorld = info.world;
        m_state.lastSeenArea = areaCounter;
        m_state.lastDistance = Dist(info.grid, selfGrid);
        m_state.lastHpPct = info.hpPct;
        m_state.lastDead = info.dead || (info.hpMax > 0 && info.hpCur <= 0);
        m_state.hasActionDest = obs.hasActionDest;
        if (obs.hasActionDest) m_state.actionDest = obs.actionDest;
        if (obs.routeUpdated) {
            m_state.route = obs.route;
            if (m_state.route.size() > p.maxRoute) m_state.route.resize(p.maxRoute);
            m_state.routeMs = nowMs;
        }
        m_missingTicks = 0;
        m_missingSinceMs = 0;
        m_state.vanish = VanishInfo{};
        m_lastUpdateMs = nowMs;
        return;
    }

    // Not visible this tick.
    m_state.visible = false;
    m_havePrev = false;
    if (m_state.lastSeenMs == 0 || m_state.vanish.active) {
        m_lastUpdateMs = nowMs;
        return;
    }
    if (m_state.lastSeenArea != areaCounter) {
        // Seen only in another area: this is a reacquire wait, not a vanish.
        m_lastUpdateMs = nowMs;
        return;
    }
    if (m_missingTicks == 0) m_missingSinceMs = nowMs;
    ++m_missingTicks;
    if (m_missingTicks >= p.vanishMinTicks && nowMs - m_missingSinceMs >= static_cast<uint64_t>(p.vanishMinMs)) {
        VanishInputs in;
        in.lastGrid = m_state.lastGrid;
        in.lastDistance = m_state.lastDistance;
        in.lastDead = m_state.lastDead;
        in.hasActionDest = m_state.hasActionDest;
        in.actionDest = m_state.actionDest;
        in.hasRouteEnd = !m_state.route.empty();
        if (in.hasRouteEnd) in.routeEnd = m_state.route.back();
        m_state.vanish = ClassifyVanish(in, interactables, p, nowMs);
    }
    m_lastUpdateMs = nowMs;
}

}  // namespace fb
