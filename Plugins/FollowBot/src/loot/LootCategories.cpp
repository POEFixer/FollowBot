// LootCategories.cpp - see LootCategories.h. The path table is the only place
// that knows the game's item folders.
#include "LootCategories.h"

#include "../common/Strings.h"

namespace fb {

namespace {

using C = ItemCategory;

// Ordered: the first matching row wins, so the specific rows (quest, gold,
// splinters, omens / essences / catalysts / emotions) come before the broad
// folder rows. Sources: data/balance/baseitemtypes Id column (2026-10-20).
constexpr ItemPathRule kRules[] = {
    // Quest items: picked even with loot off (alwaysQuestItems).
    {"Metadata/Items/QuestItems/", nullptr, C::Quest},
    {"Metadata/Items/Quest/", nullptr, C::Quest},
    {"Metadata/Items/Heist/QuestItems/", nullptr, C::Quest},

    // Gold piles.
    {"Metadata/Items/Currency/GoldCoin", nullptr, C::Gold},

    // Splinters combine into fragments (Breach Splinter, Splinter of Xoph..., but
    // not the Blessings that share the CurrencyBreach prefix).
    {"Metadata/Items/Currency/CurrencyBreach", "Shard", C::Fragments},
    {"Metadata/Items/Currency/CurrencyAfflictionShard", nullptr, C::Fragments},   // Simulacrum Splinter
    {"Metadata/Items/Currency/CurrencyRitualShard", nullptr, C::Fragments},       // Petition Splinter
    {"Metadata/Items/Currency/CurrencyRitualSplinter", nullptr, C::Fragments},
    {"Metadata/Items/Currency/CurrencyLegion", "Shard", C::Fragments},            // Timeless splinters
    {"Metadata/Items/Currency/ExpeditionPinnacleKeyShard", nullptr, C::Fragments},
    // League pinnacle keys live in Currency subfolders (Kulemak's Invitation, Breachlord Sac,
    // Raven's Reflection, The Triskelion Reforged, Head of the King, Idol of Estazunti).
    {"Metadata/Items/Currency/Abyss/", nullptr, C::Fragments},
    {"Metadata/Items/Currency/Atlas/", nullptr, C::Fragments},
    {"Metadata/Items/Currency/Breach/", nullptr, C::Fragments},
    {"Metadata/Items/Currency/Delirium/", nullptr, C::Fragments},
    {"Metadata/Items/Currency/Expedition/", nullptr, C::Fragments},
    {"Metadata/Items/Currency/Ritual/", nullptr, C::Fragments},

    // Omens, essences, catalysts, distilled emotions.
    {"Metadata/Items/Currency/Omen", nullptr, C::Omens},
    {"Metadata/Items/Omens/", nullptr, C::Omens},
    {"Metadata/Items/Currency/CurrencyEssence", nullptr, C::Omens},
    {"Metadata/Items/Currency/CurrencyLesserEssence", nullptr, C::Omens},
    {"Metadata/Items/Currency/CurrencyGreaterEssence", nullptr, C::Omens},
    {"Metadata/Items/Currency/CurrencyPerfectEssence", nullptr, C::Omens},
    {"Metadata/Items/Currency/CurrencyCorruptedEssence", nullptr, C::Omens},
    {"Metadata/Items/Currency/CurrencyJewelleryQuality", nullptr, C::Omens},      // catalysts
    {"Metadata/Items/Currency/CurrencyJewelQuality", nullptr, C::Omens},          // refined catalysts
    {"Metadata/Items/Currency/DistilledEmotion", nullptr, C::Omens},
    {"Metadata/Items/Currency/EndgameDistilledEmotion", nullptr, C::Omens},

    // Everything else in Currency: orbs, shards, abyssal bones, verisium, fossils...
    {"Metadata/Items/Currency/", nullptr, C::Currency},
    {"Metadata/Items/AtlasExiles/", nullptr, C::Currency},
    {"Metadata/Items/Brequel/", nullptr, C::Currency},                             // Wombgifts
    {"Metadata/Items/Expedition/ExpeditionVendorCurrency", nullptr, C::Currency},  // artifacts

    // Runes, soul cores, idols (one game folder).
    {"Metadata/Items/SoulCores/", nullptr, C::Runes},
    {"Metadata/Items/Runes/", nullptr, C::Runes},
    {"Metadata/Items/Socketables/", nullptr, C::Runes},

    // Gems: skill, support, meta, uncut, lineage.
    {"Metadata/Items/Gems/", nullptr, C::Gems},
    {"Metadata/Items/Gem/", nullptr, C::Gems},

    // Waystones and expedition logbooks.
    {"Metadata/Items/Maps/", nullptr, C::Waystones},
    {"Metadata/Items/Expedition/", nullptr, C::Waystones},

    // Precursor tablets.
    {"Metadata/Items/TowerAugment/", nullptr, C::Tablets},

    // Fragments: map fragments, breachstones, reliquary keys, pinnacle keys,
    // ultimatum, trial keys (Barya).
    {"Metadata/Items/MapFragments/", nullptr, C::Fragments},
    {"Metadata/Items/Pinnacle/", nullptr, C::Fragments},
    {"Metadata/Items/Ultimatum", nullptr, C::Fragments},
    {"Metadata/Items/Sanctum/", nullptr, C::Fragments},
    {"Metadata/Items/Scarabs/", nullptr, C::Fragments},

    {"Metadata/Items/Jewels/", nullptr, C::Jewels},

    // Flasks and charms (charms are Flasks/FourCharm*).
    {"Metadata/Items/Flasks/", nullptr, C::Flasks},
    {"Metadata/Items/Charms/", nullptr, C::Flasks},

    // Equipment by class.
    {"Metadata/Items/Weapons/", nullptr, C::Weapons},
    {"Metadata/Items/Wands/", nullptr, C::Weapons},
    {"Metadata/Items/Staves/", nullptr, C::Weapons},
    {"Metadata/Items/Sceptres/", nullptr, C::Weapons},
    {"Metadata/Items/Armours/", nullptr, C::Armour},      // body, helmets, gloves, boots, shields, bucklers, foci
    {"Metadata/Items/Quivers/", nullptr, C::Armour},
    {"Metadata/Items/Amulets/", nullptr, C::Accessories},
    {"Metadata/Items/Rings/", nullptr, C::Accessories},
    {"Metadata/Items/Belts/", nullptr, C::Accessories},
};

constexpr ItemCategory kDisplayOrder[] = {
    C::Quest,  C::Gold,    C::Currency, C::Omens,   C::Runes,   C::Fragments,   C::Gems,  C::Waystones,
    C::Tablets, C::Jewels, C::Flasks,   C::Uniques, C::Weapons, C::Armour, C::Accessories, C::Other,
};
static_assert(sizeof(kDisplayOrder) / sizeof(kDisplayOrder[0]) == static_cast<size_t>(kItemCategoryCount),
              "every ItemCategory is listed once in the display order");

// ASCII case-insensitive prefix test (paths are ASCII; no allocation).
bool StartsWithCI(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (AsciiLower(s[i]) != AsciiLower(prefix[i])) return false;
    return true;
}

bool ContainsCIAscii(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (StartsWithCI(hay.substr(i), needle)) return true;
    return false;
}

ItemCategory ClassifyPath(std::string_view path) {
    const std::string_view p = StripAtSuffix(path);
    for (const ItemPathRule& r : kRules) {
        const std::string_view prefix(r.prefix);
        if (!StartsWithCI(p, prefix)) continue;
        if (r.contains && !ContainsCIAscii(p.substr(prefix.size()), r.contains)) continue;
        return r.category;
    }
    return C::Other;
}

// Classes that can drop as a unique (rarity 3 turns them into Uniques).
bool CanBeUnique(ItemCategory c) {
    return CategoryUsesRarity(c) || c == C::Other;
}

}  // namespace

ItemCategory ClassifyItem(std::string_view path, int rarity) {
    const ItemCategory c = ClassifyPath(path);
    if (rarity == 3 && CanBeUnique(c)) return C::Uniques;
    return c;
}

bool IsQuestItemPath(std::string_view path) { return ClassifyPath(path) == C::Quest; }

const ItemPathRule* ItemPathRules(size_t& count) {
    count = sizeof(kRules) / sizeof(kRules[0]);
    return kRules;
}

const ItemCategory* LootCategoryDisplayOrder(size_t& count) {
    count = sizeof(kDisplayOrder) / sizeof(kDisplayOrder[0]);
    return kDisplayOrder;
}

const char* LootCategoryHint(ItemCategory c) {
    switch (c) {
        case C::Currency:    return Key("loot_hint_currency");
        case C::Quest:       return Key("loot_hint_quest");
        case C::Gems:        return Key("loot_hint_gems");
        case C::Waystones:   return Key("loot_hint_waystones");
        case C::Tablets:     return Key("loot_hint_tablets");
        case C::Runes:       return Key("loot_hint_runes");
        case C::Omens:       return Key("loot_hint_omens");
        case C::Fragments:   return Key("loot_hint_fragments");
        case C::Jewels:      return Key("loot_hint_jewels");
        case C::Flasks:      return Key("loot_hint_flasks");
        case C::Uniques:     return Key("loot_hint_uniques");
        case C::Weapons:     return Key("loot_hint_weapons");
        case C::Armour:      return Key("loot_hint_armour");
        case C::Accessories: return Key("loot_hint_accessories");
        case C::Other:       return Key("loot_hint_other");
        case C::Gold:        return Key("loot_hint_gold");
        case C::Count:       break;
    }
    return "";
}

}  // namespace fb
