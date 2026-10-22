// TravelLogic.h - the pure decisions behind following the leader through a
// transition or portal (design §5.7): which vanish to follow, which object to
// click (re-resolved by id, else another usable one of the same kind near his
// last position), which portal kinds are plausible, when a remembered object
// is gone, the aim variation per attempt. No SDK: tests/test_travel.cpp.
#pragma once

#include "../world/World.h"
#include "TravelSettings.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace fb::travel {

// MultiplexPortal shared state "portal" (metadata/miscellaneousobjects/
// multiplexportal.ot, 2026-10-20 unpack): 0 uninitialised, 1 Player, 2 Town,
// 3 Map Device, 4 Map Return, 5 Camp Teleport, 6 Return To Parent Area,
// 7 Return To Child Area. The WorldBuilder reads it as StateMachine value 1
// (define order, INFERRED) and publishes -1 when it cannot.
enum PortalKind : int {
    kPortalUnknown = -1,
    kPortalUninitialised = 0,
    kPortalPlayer = 1,
    kPortalTown = 2,
    kPortalMapDevice = 3,
    kPortalMapReturn = 4,
    kPortalCampTeleport = 5,
    kPortalToParentArea = 6,
    kPortalToChildArea = 7,
};

inline const char* PortalKindName(int v) {
    switch (v) {
        case kPortalUnknown:       return "unknown kind";
        case kPortalUninitialised: return "not set up";
        case kPortalPlayer:        return "player portal";
        case kPortalTown:          return "town portal";
        case kPortalMapDevice:     return "map device portal";
        case kPortalMapReturn:     return "map return portal";
        case kPortalCampTeleport:  return "camp teleport";
        case kPortalToParentArea:  return "portal to the parent area";
        case kPortalToChildArea:   return "portal to a sub-area";
        default:                   return "implausible kind";
    }
}

enum class PortalVerdict : uint8_t {
    Follow = 0,      // player / town / map device / map return, or unknown (unreadable)
    OtherKindOff,    // camp teleport / parent / child area portal while that option is off
    NotSetUp,        // kind 0: the portal is not initialised
    Implausible      // a value outside the known kinds (wrong read)
};

inline PortalVerdict ClassifyPortalKind(int portalType, bool allowOtherKinds) {
    if (portalType == kPortalUnknown) return PortalVerdict::Follow;
    if (portalType >= kPortalPlayer && portalType <= kPortalMapReturn) return PortalVerdict::Follow;
    if (portalType >= kPortalCampTeleport && portalType <= kPortalToChildArea)
        return allowOtherKinds ? PortalVerdict::Follow : PortalVerdict::OtherKindOff;
    if (portalType == kPortalUninitialised) return PortalVerdict::NotSetUp;
    return PortalVerdict::Implausible;
}

// The object kind a follow-through vanish points at; Count for the others.
inline InteractKind TargetKindFor(VanishKind k) {
    switch (k) {
        case VanishKind::Transition: return InteractKind::AreaTransition;
        case VanishKind::BossArena:  return InteractKind::BossArena;
        case VanishKind::Portal:     return InteractKind::Portal;
        default:                     return InteractKind::Count;
    }
}

// Whether the settings allow following this kind of vanish through.
inline bool KindEnabled(VanishKind k, const TravelSettings& s) {
    switch (k) {
        case VanishKind::Transition: return s.followTransitions;
        case VanishKind::BossArena:  return s.enterBossArenas;
        case VanishKind::Portal:     return s.followPortals;
        default:                     return false;
    }
}

// The setting that switches a vanish kind off (for "not followed" reasons).
inline const char* KindSettingName(VanishKind k) {
    switch (k) {
        case VanishKind::Transition: return "Area transitions";
        case VanishKind::BossArena:  return "Boss arenas";
        case VanishKind::Portal:     return "Portals";
        default:                     return "-";
    }
}

// An object the bot may click to follow: a portal must have a plausible kind
// and must not be known closed (targetable == false means "read and closed";
// an unreadable Targetable stays true). Transitions are not gated on
// Targetable (some report it late); clicking a dead one fails the attempt.
inline bool UsableTarget(const Interactable& it, const TravelSettings& s) {
    if (it.kind != InteractKind::Portal) return true;
    if (ClassifyPortalKind(it.portalType, s.followOtherPortalKinds) != PortalVerdict::Follow) return false;
    return it.targetable && it.open;
}

// Re-resolves the target every tick: the object with `id` when it is still
// usable; otherwise the usable object of `kind` nearest to `anchor` (his last
// position) within `maxDist` cells, objects seen this tick first. `excludeId`
// (when not 0) is never picked as the fallback (an object that just failed).
// nullptr when nothing qualifies. Allocation-free.
inline const Interactable* ResolveTarget(const std::vector<Interactable>& list, uint32_t id, InteractKind kind,
                                         const Vec2& anchor, float maxDist, const TravelSettings& s,
                                         uint32_t excludeId = 0) {
    if (id != 0) {
        for (const Interactable& it : list)
            if (it.id == id && it.kind == kind && UsableTarget(it, s)) return &it;
    }
    const Interactable* best = nullptr;
    float bestD = 0.f;
    bool bestVisible = false;
    for (const Interactable& it : list) {
        if (it.kind != kind || (excludeId != 0 && it.id == excludeId) || !UsableTarget(it, s)) continue;
        const float d = Dist(it.grid, anchor);
        if (d > maxDist) continue;
        const bool better = !best || (it.visibleNow && !bestVisible) || (it.visibleNow == bestVisible && d < bestD);
        if (better) {
            best = &it;
            bestD = d;
            bestVisible = it.visibleNow;
        }
    }
    return best;
}

// A remembered object (not in this tick's entity list) that has stayed unseen
// for `goneAfterMs` while we stand within `nearCells` of it - well inside the
// network bubble, where it would be listed if it still existed - is gone
// (a closed portal, a despawned transition).
inline bool LooksGone(const Interactable& it, const Vec2& self, uint64_t now, float nearCells, uint64_t goneAfterMs) {
    if (it.visibleNow) return false;
    if (Dist(it.grid, self) > nearCells) return false;
    return it.lastSeenMs != 0 && now >= it.lastSeenMs && now - it.lastSeenMs >= goneAfterMs;
}

// How each attempt aims: the first uses the setting; later ones vary the
// height on the object and nudge the point a little toward the character, so
// a click that kept landing on a decoration or a label edge moves.
struct AimVariant {
    float heightFactor = 0.5f;
    Vec2  gridOffset;          // added to the object's grid / world position (cells)
};

inline AimVariant AimForAttempt(int attempt, float baseHeightFactor, const Vec2& objectGrid, const Vec2& from) {
    static constexpr float kHeightScale[] = {1.0f, 0.55f, 1.4f, 0.25f, 0.8f, 1.15f};
    static constexpr float kNudge[] = {0.0f, 0.5f, -0.4f, 0.8f, 0.3f, -0.7f};   // cells, toward / across
    const int i = attempt < 0 ? 0 : attempt % 6;
    AimVariant a;
    a.heightFactor = std::clamp(baseHeightFactor * kHeightScale[i], 0.f, 1.f);
    if (i == 0) return a;
    Vec2 toward = Normalize(from - objectGrid);
    if (Length2(toward) < 1e-6f) toward = {0.f, 1.f};
    const Vec2 across = Perp(toward);
    a.gridOffset = toward * kNudge[i] + across * (kNudge[(i + 2) % 6] * 0.5f);
    return a;
}

// Time allowed to walk `cells` to an object (generous: paths bend, doors, stuck recovery).
inline int ApproachTimeoutMs(float cells) {
    const float ms = 8000.f + (cells > 0.f ? cells : 0.f) * 350.f;
    return static_cast<int>(std::clamp(ms, 12000.f, 60000.f));
}

}  // namespace fb::travel
