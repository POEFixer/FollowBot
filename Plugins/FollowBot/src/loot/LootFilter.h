// LootFilter.h - should this ground item be picked up? (design §5.9). Pure.
//
// Contract (this interface is final): exclude names win; quest items always
// (alwaysQuestItems, even with loot off); include names pick; then the
// category rule (enabled, minRarity for categories whose items have a rarity,
// minValue when the price is known); then the value rule (valueEnabled and
// value >= minValue, optionally per backpack cell); an unknown price leaves it
// to the category rule. Name lists match the base name, the unique name or a
// path substring, case-insensitive (any script); an entry in double quotes
// ("Chaos Orb") must equal the whole base or unique name. Distance / leader /
// enemy / inventory-space constraints are NOT decided here (PickupTask
// applies them).
#pragma once

#include "LootSettings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fb {

struct LootItemInfo {
    uint32_t     id = 0;
    std::string  path;          // inner item metadata path
    std::string  baseName;      // localized base type name (UTF-8)
    std::string  uniqueName;    // localized unique name or ""
    int          rarity = -1;   // 0..3, -1 unknown
    int          w = 1, h = 1;  // inventory size (unknown = 1x1)
    int          stack = 1;
    int          maxStack = 0;  // stack limit (0 unknown, 1 not stackable)
    ItemCategory category = ItemCategory::Other;
    bool         priceKnown = false;
    float        value = 0.f;   // in LootSettings::currency, per stack
};

struct LootDecision {
    bool        pick = false;
    const char* reason = "";    // a key (lang/en.json) for Diagnostics; always a static string
};

LootDecision EvaluateLoot(const LootItemInfo& item, const LootSettings& s);

// --- prepared matching (the scanner evaluates many items per pass) -----------

// The item's names folded once for the name lists.
struct LootItemKeys {
    std::string base, unique, path;   // case-folded
    static LootItemKeys From(const LootItemInfo& item);
};

// One name list folded once.
class LootNameMatcher {
public:
    void Build(const std::vector<std::string>& entries);
    bool Matches(const LootItemKeys& k) const;
    bool Empty() const { return m_entries.empty(); }

private:
    struct Entry {
        std::string text;   // folded, quotes removed
        bool        exact = false;
    };
    std::vector<Entry> m_entries;
};

LootDecision EvaluateLoot(const LootItemInfo& item, const LootSettings& s, const LootItemKeys& keys,
                          const LootNameMatcher& include, const LootNameMatcher& exclude);

// --- prices ------------------------------------------------------------------

// Whether a host price-list answer can be trusted for this item: stackable
// loot only under the stackable price categories; uniques only under the
// unique ones (a base name resolves to its most expensive unique, never right
// for a magic or rare item).
bool TrustPriceCategory(const std::string& priceCategory, bool unique);

// The value the rules compare: per stack, or per backpack cell.
inline float ComparedValue(const LootItemInfo& item, bool perCell) {
    if (!perCell) return item.value;
    const int cells = (item.w > 0 ? item.w : 1) * (item.h > 0 ? item.h : 1);
    return item.value / static_cast<float>(cells);
}

}  // namespace fb
