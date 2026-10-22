// LootSettings.h - ground loot pickup (design §5.9): the settings struct and
// its JSON (per-field fallback + clamp). Categories are the shared
// fb::ItemCategory (world/World.h), decided by metadata path
// (loot/LootCategories). Gold is a category of its own (on by default).
#pragma once

#include "../common/JsonUtil.h"
#include "../world/World.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace fb {

struct LootCategoryRule {
    bool  enabled = false;
    int   minRarity = 0;      // equipment categories: 0 normal, 1 magic, 2 rare, 3 unique
    float minValue = 0.f;     // in the reference currency; 0 = no value floor
};

enum class RefCurrency : uint8_t { Chaos = 0, Divine, Exalted, Count };

inline const char* RefCurrencyShort(RefCurrency c) {
    switch (c) {
        case RefCurrency::Chaos:   return "c";
        case RefCurrency::Divine:  return "div";
        case RefCurrency::Exalted: return "ex";
        case RefCurrency::Count:   break;
    }
    return "?";
}

inline const char* RefCurrencyName(RefCurrency c) {
    switch (c) {
        case RefCurrency::Chaos:   return "Chaos Orb";
        case RefCurrency::Divine:  return "Divine Orb";
        case RefCurrency::Exalted: return "Exalted Orb";
        case RefCurrency::Count:   break;
    }
    return "?";
}

using LootRules = std::array<LootCategoryRule, kItemCategoryCount>;

// Defaults: currency-like and rare-by-nature items on; plain equipment off.
inline LootRules DefaultLootRules() {
    LootRules r{};
    auto on = [&](ItemCategory c, int minRarity = 0) {
        r[static_cast<size_t>(c)].enabled = true;
        r[static_cast<size_t>(c)].minRarity = minRarity;
    };
    on(ItemCategory::Currency);
    on(ItemCategory::Quest);
    on(ItemCategory::Gems);
    on(ItemCategory::Waystones);
    on(ItemCategory::Tablets);
    on(ItemCategory::Runes);
    on(ItemCategory::Omens);
    on(ItemCategory::Fragments);
    on(ItemCategory::Jewels);
    on(ItemCategory::Uniques);
    on(ItemCategory::Gold);
    r[static_cast<size_t>(ItemCategory::Weapons)].minRarity = 2;
    r[static_cast<size_t>(ItemCategory::Armour)].minRarity = 2;
    r[static_cast<size_t>(ItemCategory::Accessories)].minRarity = 2;
    return r;
}

struct LootSettings {
    bool  enabled = true;
    bool  alwaysQuestItems = true;            // quest items even with loot off
    LootRules categories = DefaultLootRules();
    std::vector<std::string> includeNames;    // base name, unique name or path substring (case-insensitive)
    std::vector<std::string> excludeNames;    // exclude wins over everything
    bool  valueEnabled = false;               // pick anything worth at least minValue
    float minValue = 1.f;
    bool  valuePerCell = false;               // compare the value per backpack cell (value / (w x h))
    RefCurrency currency = RefCurrency::Exalted;
    float scanRadius = 40.f;                  // cells from us
    float maxLeaderDistance = 45.f;           // only while the leader is this close (and the item too)
    float enemyRadius = 25.f;                 // no hostile this close ...
    bool  lootDuringCombat = false;           // ... unless this is on
    bool  lootInTownHideout = false;
    int   minFreeCells = 0;                   // keep this many backpack cells free (quest items, gold ignore it)
    float pickupRange = 6.f;                  // approach this close before clicking
    int   labelOffsetPx = 16;                 // click this many px above the item's ground point
    int   hoverDwellMs = 120;                 // hover before the press (the game must register the hover)
    int   pressHoldMs = 50;
    int   reHovers = 1;                       // extra hovers at small offsets when the label is not highlighted
    int   vanishWaitMs = 900;                 // the item must vanish within this after the click
    int   vanishWaitMovingMs = 2500;          // ... extended while we still walk toward it
    int   maxAttempts = 3;                    // clicks per item before it is blacklisted
    int   blacklistMs = 60000;
    int   maxContinuousMs = 8000;             // then yield to Follow ...
    int   yieldCooldownMs = 2000;             // ... for at least this long before looting again
    float preemptLeaderExtra = 10.f;          // abort when the leader is farther than maxLeaderDistance + this
    int   priceLookupBudget = 32;             // price lookups per scan pass
};

inline constexpr size_t kLootNameListMax = 512;     // entries per name list
inline constexpr size_t kLootNameMaxLen = 256;       // bytes per entry

inline void Clamp(LootSettings& s) {
    for (auto* list : {&s.includeNames, &s.excludeNames}) {
        if (list->size() > kLootNameListMax) list->resize(kLootNameListMax);
        for (auto& n : *list)
            if (n.size() > kLootNameMaxLen) n.resize(kLootNameMaxLen);
    }
    for (auto& r : s.categories) {
        r.minRarity = std::clamp(r.minRarity, 0, 3);
        if (!(r.minValue >= 0.f)) r.minValue = 0.f;
        r.minValue = (std::min)(r.minValue, 100000.f);
    }
    if (!(s.minValue >= 0.f)) s.minValue = 0.f;
    s.minValue = (std::min)(s.minValue, 100000.f);
    s.scanRadius = std::clamp(s.scanRadius, 5.f, 150.f);
    s.maxLeaderDistance = std::clamp(s.maxLeaderDistance, 10.f, 200.f);
    s.enemyRadius = std::clamp(s.enemyRadius, 0.f, 120.f);
    s.pickupRange = std::clamp(s.pickupRange, 2.f, 20.f);
    s.labelOffsetPx = std::clamp(s.labelOffsetPx, 0, 80);
    s.hoverDwellMs = std::clamp(s.hoverDwellMs, 40, 400);
    s.pressHoldMs = std::clamp(s.pressHoldMs, 20, 200);
    s.reHovers = std::clamp(s.reHovers, 0, 4);
    s.minFreeCells = std::clamp(s.minFreeCells, 0, kBackpackCells - 1);
    s.vanishWaitMs = std::clamp(s.vanishWaitMs, 200, 5000);
    s.vanishWaitMovingMs = std::clamp(s.vanishWaitMovingMs, s.vanishWaitMs, 10000);
    s.maxAttempts = std::clamp(s.maxAttempts, 1, 10);
    s.blacklistMs = std::clamp(s.blacklistMs, 5000, 600000);
    s.maxContinuousMs = std::clamp(s.maxContinuousMs, 1000, 60000);
    s.yieldCooldownMs = std::clamp(s.yieldCooldownMs, 0, 30000);
    s.preemptLeaderExtra = std::clamp(s.preemptLeaderExtra, 0.f, 60.f);
    s.priceLookupBudget = std::clamp(s.priceLookupBudget, 1, 256);
}

inline void ToJson(json::Json& j, const LootSettings& s) {
    j = json::Json::object();
    j["enabled"] = s.enabled;
    j["always_quest_items"] = s.alwaysQuestItems;
    json::Json cats = json::Json::array();
    for (int i = 0; i < kItemCategoryCount; ++i) {
        const auto& r = s.categories[static_cast<size_t>(i)];
        cats.push_back({{"category", i}, {"enabled", r.enabled}, {"min_rarity", r.minRarity}, {"min_value", r.minValue}});
    }
    j["categories"] = std::move(cats);
    j["include_names"] = s.includeNames;
    j["exclude_names"] = s.excludeNames;
    j["value_enabled"] = s.valueEnabled;
    j["min_value"] = s.minValue;
    j["value_per_cell"] = s.valuePerCell;
    j["currency"] = static_cast<int>(s.currency);
    j["scan_radius"] = s.scanRadius;
    j["max_leader_distance"] = s.maxLeaderDistance;
    j["enemy_radius"] = s.enemyRadius;
    j["loot_during_combat"] = s.lootDuringCombat;
    j["loot_in_town_hideout"] = s.lootInTownHideout;
    j["min_free_cells"] = s.minFreeCells;
    j["pickup_range"] = s.pickupRange;
    j["label_offset_px"] = s.labelOffsetPx;
    j["hover_dwell_ms"] = s.hoverDwellMs;
    j["press_hold_ms"] = s.pressHoldMs;
    j["re_hovers"] = s.reHovers;
    j["vanish_wait_ms"] = s.vanishWaitMs;
    j["vanish_wait_moving_ms"] = s.vanishWaitMovingMs;
    j["max_attempts"] = s.maxAttempts;
    j["blacklist_ms"] = s.blacklistMs;
    j["max_continuous_ms"] = s.maxContinuousMs;
    j["yield_cooldown_ms"] = s.yieldCooldownMs;
    j["preempt_leader_extra"] = s.preemptLeaderExtra;
    j["price_lookup_budget"] = s.priceLookupBudget;
}

inline void FromJson(const json::Json& j, LootSettings& s) {
    using json::Read;
    Read(j, "enabled", s.enabled);
    Read(j, "always_quest_items", s.alwaysQuestItems);
    for (const auto& c : json::ChildArray(j, "categories")) {
        const int idx = json::Get<int>(c, "category", -1);
        if (idx < 0 || idx >= kItemCategoryCount) continue;
        auto& r = s.categories[static_cast<size_t>(idx)];
        Read(c, "enabled", r.enabled);
        Read(c, "min_rarity", r.minRarity);
        Read(c, "min_value", r.minValue);
    }
    json::ReadStringList(j, "include_names", s.includeNames);
    json::ReadStringList(j, "exclude_names", s.excludeNames);
    Read(j, "value_enabled", s.valueEnabled);
    Read(j, "min_value", s.minValue);
    Read(j, "value_per_cell", s.valuePerCell);
    json::ReadEnum(j, "currency", s.currency, static_cast<int>(RefCurrency::Count));
    Read(j, "scan_radius", s.scanRadius);
    Read(j, "max_leader_distance", s.maxLeaderDistance);
    Read(j, "enemy_radius", s.enemyRadius);
    Read(j, "loot_during_combat", s.lootDuringCombat);
    Read(j, "loot_in_town_hideout", s.lootInTownHideout);
    Read(j, "min_free_cells", s.minFreeCells);
    Read(j, "pickup_range", s.pickupRange);
    Read(j, "label_offset_px", s.labelOffsetPx);
    Read(j, "hover_dwell_ms", s.hoverDwellMs);
    Read(j, "press_hold_ms", s.pressHoldMs);
    Read(j, "re_hovers", s.reHovers);
    Read(j, "vanish_wait_ms", s.vanishWaitMs);
    Read(j, "vanish_wait_moving_ms", s.vanishWaitMovingMs);
    Read(j, "max_attempts", s.maxAttempts);
    Read(j, "blacklist_ms", s.blacklistMs);
    Read(j, "max_continuous_ms", s.maxContinuousMs);
    Read(j, "yield_cooldown_ms", s.yieldCooldownMs);
    Read(j, "preempt_leader_extra", s.preemptLeaderExtra);
    Read(j, "price_lookup_budget", s.priceLookupBudget);
    Clamp(s);
}

}  // namespace fb
