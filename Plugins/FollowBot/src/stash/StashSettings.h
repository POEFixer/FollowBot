// StashSettings.h - stashing, and leaving the map to stash (design §5.11): the
// settings struct and its JSON (per-field fallback + clamp).
#pragma once

#include "../common/JsonUtil.h"
#include "../world/World.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fb {

enum class StashTarget : uint8_t { Own = 0, Guild, Count };

inline const char* StashTargetName(StashTarget t) {
    switch (t) {
        case StashTarget::Own:   return "Own stash";
        case StashTarget::Guild: return "Guild stash";
        case StashTarget::Count: break;
    }
    return "?";
}

// Items of `category` go to the tab named `tab` (the user's tab name).
struct StashRoute {
    ItemCategory category = ItemCategory::Currency;
    std::string  tab;
};

struct StashSettings {
    bool        enabled = true;
    StashTarget target = StashTarget::Own;
    // Trigger (b): opportunistic in town / hideout.
    bool        opportunistic = true;
    int         stashWhenUsedCells = 40;    // of 60 backpack cells (or the inventory-full flag)
    int         leaderIdleMs = 2000;        // the leader stood still this long (or is absent)
    // Trigger (c): leave the map through an own portal, stash, come back.
    bool        leaveMapTrip = false;
    int         tripWhenUsedCells = 52;
    float       tripNoEnemyRadius = 30.f;   // cells
    int         portalKey = 0;              // the key that opens a portal (skill / scroll); 0 = none
    // Routing.
    std::vector<StashRoute> routes;
    std::string defaultTab;                 // empty = whatever tab is open
    bool        missingTabUsesOpenTab = true;  // a routed tab not found / not reachable: use the open tab (else keep the items)
    // What stays in the backpack.
    uint64_t    protectedCells = 0;         // bit y*12+x of the 12x5 backpack
    uint32_t    keepCategories = 0;         // bit per ItemCategory
    std::vector<std::string> keepNames;     // base / unique name or path substring (case-insensitive)
    // Deposit timing.
    int         ownClickDelayMs = 70;
    int         guildClickDelayMs = 320;
    int         confirmWindowMs = 1500;     // an item counts as moved only once it vanished from a fresh listing
    int         maxConfirmWindows = 3;      // then skip it
    int         tabFullSkips = 3;           // consecutive skips in one tab = tab full
    // Opening.
    int         openTimeoutMs = 3000;
    int         openRetries = 3;
    float       approachRingCells = 7.f;    // stand this far from the stash object
    int         tabSwitchWaitMs = 350;
    int         clickDwellMs = 150;         // hover on the stash object before the press
    int         failCooldownMs = 30000;     // after a failed automatic run, wait this long before the next one
    // Portal trip.
    int         portalWaitMs = 3000;        // wait for our new portal to appear after the key
    float       portalSearchCells = 15.f;

    bool IsProtected(int x, int y) const {
        if (x < 0 || y < 0 || x >= kBackpackCols || y >= kBackpackRows) return false;
        return (protectedCells >> (y * kBackpackCols + x)) & 1ull;
    }
    void SetProtected(int x, int y, bool on) {
        if (x < 0 || y < 0 || x >= kBackpackCols || y >= kBackpackRows) return;
        const uint64_t bit = 1ull << (y * kBackpackCols + x);
        protectedCells = on ? (protectedCells | bit) : (protectedCells & ~bit);
    }
    bool Keeps(ItemCategory c) const { return (keepCategories >> static_cast<int>(c)) & 1u; }
    void SetKeep(ItemCategory c, bool on) {
        const uint32_t bit = 1u << static_cast<int>(c);
        keepCategories = on ? (keepCategories | bit) : (keepCategories & ~bit);
    }
};

inline void Clamp(StashSettings& s) {
    s.stashWhenUsedCells = std::clamp(s.stashWhenUsedCells, 1, kBackpackCells);
    s.leaderIdleMs = std::clamp(s.leaderIdleMs, 0, 30000);
    s.tripWhenUsedCells = std::clamp(s.tripWhenUsedCells, 1, kBackpackCells);
    s.tripNoEnemyRadius = std::clamp(s.tripNoEnemyRadius, 0.f, 120.f);
    s.portalKey = std::clamp(s.portalKey, 0, 254);
    if (s.routes.size() > 64) s.routes.resize(64);
    for (auto& r : s.routes)
        if (r.tab.size() > 64) r.tab.resize(64);
    if (s.defaultTab.size() > 64) s.defaultTab.resize(64);
    s.protectedCells &= (1ull << kBackpackCells) - 1ull;
    s.keepCategories &= (1u << kItemCategoryCount) - 1u;
    s.ownClickDelayMs = std::clamp(s.ownClickDelayMs, 30, 1000);
    s.guildClickDelayMs = std::clamp(s.guildClickDelayMs, 100, 2000);
    s.confirmWindowMs = std::clamp(s.confirmWindowMs, 300, 5000);
    s.maxConfirmWindows = std::clamp(s.maxConfirmWindows, 1, 10);
    s.tabFullSkips = std::clamp(s.tabFullSkips, 1, 10);
    s.openTimeoutMs = std::clamp(s.openTimeoutMs, 1000, 10000);
    s.openRetries = std::clamp(s.openRetries, 1, 10);
    s.approachRingCells = std::clamp(s.approachRingCells, 3.f, 20.f);
    s.tabSwitchWaitMs = std::clamp(s.tabSwitchWaitMs, 100, 2000);
    s.clickDwellMs = std::clamp(s.clickDwellMs, 40, 600);
    s.failCooldownMs = std::clamp(s.failCooldownMs, 0, 600000);
    s.portalWaitMs = std::clamp(s.portalWaitMs, 1000, 10000);
    s.portalSearchCells = std::clamp(s.portalSearchCells, 5.f, 40.f);
}

inline void ToJson(json::Json& j, const StashSettings& s) {
    j = json::Json::object();
    j["enabled"] = s.enabled;
    j["target"] = static_cast<int>(s.target);
    j["opportunistic"] = s.opportunistic;
    j["stash_when_used_cells"] = s.stashWhenUsedCells;
    j["leader_idle_ms"] = s.leaderIdleMs;
    j["leave_map_trip"] = s.leaveMapTrip;
    j["trip_when_used_cells"] = s.tripWhenUsedCells;
    j["trip_no_enemy_radius"] = s.tripNoEnemyRadius;
    j["portal_key"] = s.portalKey;
    json::Json routes = json::Json::array();
    for (const auto& r : s.routes) routes.push_back({{"category", static_cast<int>(r.category)}, {"tab", r.tab}});
    j["routes"] = std::move(routes);
    j["default_tab"] = s.defaultTab;
    j["missing_tab_uses_open_tab"] = s.missingTabUsesOpenTab;
    j["protected_cells"] = json::MaskToString(s.protectedCells);
    j["keep_categories"] = s.keepCategories;
    j["keep_names"] = s.keepNames;
    j["own_click_delay_ms"] = s.ownClickDelayMs;
    j["guild_click_delay_ms"] = s.guildClickDelayMs;
    j["confirm_window_ms"] = s.confirmWindowMs;
    j["max_confirm_windows"] = s.maxConfirmWindows;
    j["tab_full_skips"] = s.tabFullSkips;
    j["open_timeout_ms"] = s.openTimeoutMs;
    j["open_retries"] = s.openRetries;
    j["approach_ring_cells"] = s.approachRingCells;
    j["tab_switch_wait_ms"] = s.tabSwitchWaitMs;
    j["click_dwell_ms"] = s.clickDwellMs;
    j["fail_cooldown_ms"] = s.failCooldownMs;
    j["portal_wait_ms"] = s.portalWaitMs;
    j["portal_search_cells"] = s.portalSearchCells;
}

inline void FromJson(const json::Json& j, StashSettings& s) {
    using json::Read;
    Read(j, "enabled", s.enabled);
    json::ReadEnum(j, "target", s.target, static_cast<int>(StashTarget::Count));
    Read(j, "opportunistic", s.opportunistic);
    Read(j, "stash_when_used_cells", s.stashWhenUsedCells);
    Read(j, "leader_idle_ms", s.leaderIdleMs);
    Read(j, "leave_map_trip", s.leaveMapTrip);
    Read(j, "trip_when_used_cells", s.tripWhenUsedCells);
    Read(j, "trip_no_enemy_radius", s.tripNoEnemyRadius);
    Read(j, "portal_key", s.portalKey);
    if (j.is_object() && j.contains("routes") && j["routes"].is_array()) {
        s.routes.clear();
        for (const auto& jr : j["routes"]) {
            const int c = json::Get<int>(jr, "category", -1);
            if (c < 0 || c >= kItemCategoryCount) continue;
            StashRoute r;
            r.category = static_cast<ItemCategory>(c);
            r.tab = json::Get<std::string>(jr, "tab", std::string());
            s.routes.push_back(std::move(r));
        }
    }
    Read(j, "default_tab", s.defaultTab);
    Read(j, "missing_tab_uses_open_tab", s.missingTabUsesOpenTab);
    s.protectedCells = json::GetMask(j, "protected_cells", s.protectedCells);
    Read(j, "keep_categories", s.keepCategories);
    json::ReadStringList(j, "keep_names", s.keepNames);
    Read(j, "own_click_delay_ms", s.ownClickDelayMs);
    Read(j, "guild_click_delay_ms", s.guildClickDelayMs);
    Read(j, "confirm_window_ms", s.confirmWindowMs);
    Read(j, "max_confirm_windows", s.maxConfirmWindows);
    Read(j, "tab_full_skips", s.tabFullSkips);
    Read(j, "open_timeout_ms", s.openTimeoutMs);
    Read(j, "open_retries", s.openRetries);
    Read(j, "approach_ring_cells", s.approachRingCells);
    Read(j, "tab_switch_wait_ms", s.tabSwitchWaitMs);
    Read(j, "click_dwell_ms", s.clickDwellMs);
    Read(j, "fail_cooldown_ms", s.failCooldownMs);
    Read(j, "portal_wait_ms", s.portalWaitMs);
    Read(j, "portal_search_cells", s.portalSearchCells);
    Clamp(s);
}

}  // namespace fb
