// TravelSettings.h - following the leader through transitions and portals
// (design §5.7): the settings struct and its JSON (per-field fallback + clamp).
#pragma once

#include "../common/JsonUtil.h"

namespace fb {

struct TravelSettings {
    bool  followTransitions = true;   // area transitions the leader took
    bool  followPortals = true;       // portals (town, player, map device, map return) the leader took
    bool  enterBossArenas = true;     // *_ArenaPortal transitions
    bool  followOtherPortalKinds = false;  // also camp-teleport / parent-area / sub-area portals (kinds 5-7)
    float vanishRadius = 10.f;        // cells: an object this close to his last position explains a vanish
    float outOfRangeCells = 125.f;    // he was this far from us when he vanished: out of range, not travel
    float fallbackRadius = 25.f;      // cells: another object of the same kind this close to his last position
                                      // is used when the one he took is gone (e.g. another map-device portal)
    float interactRange = 6.f;        // cells: approach this close before clicking an object
    int   hoverDwellMs = 120;         // hover before the press (the game must register the hover)
    int   pressHoldMs = 50;
    int   reHovers = 2;               // extra hovers at small offsets when the object is not highlighted
    float tallObjectHeightFactor = 0.5f;  // aim at z + modelHeight * factor (tall portals)
    int   areaChangeTimeoutMs = 6000; // wait this long for our area to change after the click
    int   maxAttempts = 3;            // clicks (each after a fresh approach) before giving up
    int   findObjectMs = 4000;        // at his last position, wait this long for the object to show up
    bool  seekOnFailure = true;       // after a failed travel, walk to his last position and wait
};

inline void Clamp(TravelSettings& s) {
    s.vanishRadius = std::clamp(s.vanishRadius, 3.f, 30.f);
    s.outOfRangeCells = std::clamp(s.outOfRangeCells, 60.f, 200.f);
    s.fallbackRadius = std::clamp(s.fallbackRadius, 5.f, 80.f);
    s.interactRange = std::clamp(s.interactRange, 2.f, 20.f);
    s.hoverDwellMs = std::clamp(s.hoverDwellMs, 40, 400);
    s.pressHoldMs = std::clamp(s.pressHoldMs, 20, 200);
    s.reHovers = std::clamp(s.reHovers, 0, 4);
    s.tallObjectHeightFactor = std::clamp(s.tallObjectHeightFactor, 0.f, 1.f);
    s.areaChangeTimeoutMs = std::clamp(s.areaChangeTimeoutMs, 2000, 20000);
    s.maxAttempts = std::clamp(s.maxAttempts, 1, 6);
    s.findObjectMs = std::clamp(s.findObjectMs, 1000, 15000);
}

inline void ToJson(json::Json& j, const TravelSettings& s) {
    j = json::Json::object();
    j["follow_transitions"] = s.followTransitions;
    j["follow_portals"] = s.followPortals;
    j["enter_boss_arenas"] = s.enterBossArenas;
    j["follow_other_portal_kinds"] = s.followOtherPortalKinds;
    j["vanish_radius"] = s.vanishRadius;
    j["out_of_range_cells"] = s.outOfRangeCells;
    j["fallback_radius"] = s.fallbackRadius;
    j["interact_range"] = s.interactRange;
    j["hover_dwell_ms"] = s.hoverDwellMs;
    j["press_hold_ms"] = s.pressHoldMs;
    j["re_hovers"] = s.reHovers;
    j["tall_object_height_factor"] = s.tallObjectHeightFactor;
    j["area_change_timeout_ms"] = s.areaChangeTimeoutMs;
    j["max_attempts"] = s.maxAttempts;
    j["find_object_ms"] = s.findObjectMs;
    j["seek_on_failure"] = s.seekOnFailure;
}

inline void FromJson(const json::Json& j, TravelSettings& s) {
    using json::Read;
    Read(j, "follow_transitions", s.followTransitions);
    Read(j, "follow_portals", s.followPortals);
    Read(j, "enter_boss_arenas", s.enterBossArenas);
    Read(j, "follow_other_portal_kinds", s.followOtherPortalKinds);
    Read(j, "vanish_radius", s.vanishRadius);
    Read(j, "out_of_range_cells", s.outOfRangeCells);
    Read(j, "fallback_radius", s.fallbackRadius);
    Read(j, "interact_range", s.interactRange);
    Read(j, "hover_dwell_ms", s.hoverDwellMs);
    Read(j, "press_hold_ms", s.pressHoldMs);
    Read(j, "re_hovers", s.reHovers);
    Read(j, "tall_object_height_factor", s.tallObjectHeightFactor);
    Read(j, "area_change_timeout_ms", s.areaChangeTimeoutMs);
    Read(j, "max_attempts", s.maxAttempts);
    Read(j, "find_object_ms", s.findObjectMs);
    Read(j, "seek_on_failure", s.seekOnFailure);
    Clamp(s);
}

}  // namespace fb
