// LeaderTracker.h - finds the leader among the other players, keeps his
// last-known position, velocity, route and action destination, and classifies
// how he vanished (design §5.1). Pure: no SDK (the WorldBuilder feeds it).
//
// Identity = the configured character name vs Entity.PlayerName (exact UTF-8,
// optionally case-insensitive), or "auto" = the only other player when exactly
// one is present. Vanish = missing for >= 2 consecutive ticks and >= 120 ms
// while our own area counter is unchanged. Classification at the vanish
// moment, in this order:
//   Died        his last HP was 0;
//   Transition / BossArena / Portal
//               the nearest object of those kinds within vanishRadius of his
//               last position, or within 3 cells of his action destination /
//               route end (portals only when open / targetable); when he was
//               already >= outOfRangeCells from us (he may simply have left our
//               entity range), his last position must be within
//               farVanishRadius of the object (or the destination test hold);
//   Checkpoint / Waypoint
//               the same test for those kinds (only when no travel object
//               qualified: following through is cheap to retry, missing a
//               transition is not);
//   OutOfRange  his last distance from us was >= outOfRangeCells;
//   Unknown     otherwise.
// Reappearance clears the vanish.
#pragma once

#include "World.h"

#include <string>
#include <vector>

namespace fb {

struct LeaderParams {
    std::string name;                // UTF-8 character name
    bool  caseInsensitive = true;
    bool  autoPick = false;          // the only other player, when exactly one is present
    float vanishRadius = 10.f;
    float outOfRangeCells = 125.f;
    float farVanishRadius = 3.f;     // beyond outOfRangeCells: his last position this close to the object
    float destNearObjectCells = 3.f;
    float teleportCells = 40.f;      // a jump this large in one tick resets the velocity
    float emaAlpha = 0.3f;
    int   vanishMinTicks = 2;
    int   vanishMinMs = 120;
    size_t maxRoute = 64;
};

// What the WorldBuilder read about the leader this tick.
struct LeaderObservation {
    bool       found = false;
    PlayerInfo info;                 // grid/world are the LIVE position (ReadRender)
    bool       hasActionDest = false;
    Vec2       actionDest;
    bool       routeUpdated = false; // `route` is a fresh read (route cadence)
    std::vector<Vec2> route;
};

// Index into `others` of the leader, -1 when none.
int FindLeader(const std::vector<PlayerInfo>& others, const LeaderParams& p);

// Vanish classification (pure function of the moment).
struct VanishInputs {
    Vec2  lastGrid;
    float lastDistance = 0.f;
    bool  lastDead = false;
    bool  hasActionDest = false;
    Vec2  actionDest;
    bool  hasRouteEnd = false;
    Vec2  routeEnd;
};
VanishInfo ClassifyVanish(const VanishInputs& in, const std::vector<Interactable>& interactables,
                          const LeaderParams& p, uint64_t nowMs);

class LeaderTracker {
public:
    void Reset();
    // `interactables` = this tick's visible + remembered ones (World.interactables).
    void Update(uint64_t nowMs, uint32_t areaCounter, const Vec2& selfGrid, const LeaderObservation& obs,
                const std::vector<Interactable>& interactables, const LeaderParams& p);
    const LeaderState& State() const { return m_state; }

private:
    LeaderState m_state;
    int         m_missingTicks = 0;
    uint64_t    m_missingSinceMs = 0;
    uint64_t    m_lastUpdateMs = 0;
    bool        m_havePrev = false;
    Vec2        m_prevGrid;
    uint64_t    m_prevMs = 0;
    uint32_t    m_area = 0;
    bool        m_haveArea = false;
};

}  // namespace fb
