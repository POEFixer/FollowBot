// LootCategories.h - which ItemCategory an item is, by its metadata path and
// rarity (design §5.9). Locale-independent: never by display text. Pure.
//
// One ordered data table of path rules (LootCategories.cpp, first match wins),
// built from the game's BaseItemTypes ids (data/balance/baseitemtypes, patch
// 2026-10-20): Metadata/Items/QuestItems/* -> Quest; Currency/GoldCoin -> Gold;
// breach / simulacrum / ritual / timeless splinters and the league pinnacle keys
// in Currency subfolders -> Fragments; Currency/Omen*, the essence and catalyst
// families and DistilledEmotion* -> Omens; the rest of Currency/* -> Currency;
// SoulCores/* (runes, soul cores, idols) -> Runes; Gems/* and Gem/* -> Gems;
// Maps/* (waystones) and Expedition logbooks -> Waystones; TowerAugment/* ->
// Tablets; MapFragments/*, Pinnacle/*, Ultimatum*, Sanctum/* -> Fragments;
// Jewels/*; Flasks/* (flasks and charms); Weapons/*; Armours/* and Quivers/*;
// Amulets / Rings / Belts -> Accessories; anything else -> Other. Rarity 3
// turns any class that can be unique into Uniques. A patch that moves items
// needs one table edit (tests/test_loot.cpp pins the table on real ids).
#pragma once

#include "../world/World.h"

#include <cstddef>
#include <string_view>

namespace fb {

// Category of an item from its "Metadata/Items/..." path; rarity 0..3 (-1 unknown).
// Matching is ASCII case-insensitive on the path without its "@n" suffix.
ItemCategory ClassifyItem(std::string_view path, int rarity);

// Weapons, Armour, Accessories (their rules use minRarity).
inline bool IsEquipmentCategory(ItemCategory c) {
    return c == ItemCategory::Weapons || c == ItemCategory::Armour || c == ItemCategory::Accessories;
}

bool IsQuestItemPath(std::string_view path);

// Categories whose items carry a rarity, so a "min rarity" rule applies:
// equipment plus jewels, flasks / charms, waystones and tablets.
inline bool CategoryUsesRarity(ItemCategory c) {
    return IsEquipmentCategory(c) || c == ItemCategory::Jewels || c == ItemCategory::Flasks ||
           c == ItemCategory::Waystones || c == ItemCategory::Tablets;
}

// Gold piles go to the gold counter, never into the backpack.
inline bool NeedsBackpackSpace(ItemCategory c) { return c != ItemCategory::Gold; }

// Categories the host price service can value by base name (stackable loot);
// equipment, jewels, flasks, waystones and tablets are priced only as uniques
// (the price list files a base name under its most expensive unique).
inline bool PricedByBaseName(ItemCategory c) {
    return c == ItemCategory::Currency || c == ItemCategory::Omens || c == ItemCategory::Runes ||
           c == ItemCategory::Fragments || c == ItemCategory::Gems || c == ItemCategory::Other;
}

// One row of the classification table (exposed for the tests and the UI help).
struct ItemPathRule {
    const char*  prefix;     // path prefix (ASCII, case-insensitive)
    const char*  contains;   // nullptr, or a text that must follow the prefix
    ItemCategory category;
};

const ItemPathRule* ItemPathRules(size_t& count);

// The order the settings tab lists the categories in (every category once).
const ItemCategory* LootCategoryDisplayOrder(size_t& count);

// One line of what a category holds (settings tab tooltips).
const char* LootCategoryHint(ItemCategory c);   // the category's tooltip key (lang/en.json)

}  // namespace fb
