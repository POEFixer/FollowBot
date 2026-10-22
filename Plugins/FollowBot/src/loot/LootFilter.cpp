// LootFilter.cpp - see LootFilter.h.
#include "LootFilter.h"

#include "../common/Strings.h"
#include "LootCategories.h"

namespace fb {

namespace {

enum class CatOutcome : uint8_t { Pass = 0, Off, RarityLow, RarityUnknown, ValueLow, Count };
enum class ValOutcome : uint8_t { Disabled = 0, Low, Unknown, Count };

// Why an item is left on the ground: [category outcome][value-rule outcome].
constexpr const char* kSkipReasons[static_cast<int>(CatOutcome::Count)][static_cast<int>(ValOutcome::Count)] = {
    /* Pass (unused) */ {"", "", ""},
    /* Off */
    {"its category is off", "category off, worth less than your value threshold", "category off, no price known"},
    /* RarityLow */
    {"rarity below the category minimum", "rarity too low, worth less than your value threshold",
     "rarity too low, no price known"},
    /* RarityUnknown */
    {"rarity unknown (the category needs a minimum rarity)", "rarity unknown, worth less than your value threshold",
     "rarity unknown, no price known"},
    /* ValueLow */
    {"worth less than the category minimum", "worth less than the category minimum and your value threshold",
     "worth less than the category minimum"},
};

CatOutcome EvaluateCategory(const LootItemInfo& item, const LootSettings& s) {
    const int idx = static_cast<int>(item.category);
    if (idx < 0 || idx >= kItemCategoryCount) return CatOutcome::Off;
    const LootCategoryRule& r = s.categories[static_cast<size_t>(idx)];
    if (!r.enabled) return CatOutcome::Off;
    if (CategoryUsesRarity(item.category) && r.minRarity > 0) {
        if (item.rarity < 0) return CatOutcome::RarityUnknown;
        if (item.rarity < r.minRarity) return CatOutcome::RarityLow;
    }
    // A category minimum value applies only when the price is known.
    if (r.minValue > 0.f && item.priceKnown && ComparedValue(item, s.valuePerCell) < r.minValue) return CatOutcome::ValueLow;
    return CatOutcome::Pass;
}

ValOutcome EvaluateValue(const LootItemInfo& item, const LootSettings& s, bool* pass) {
    *pass = false;
    if (!s.valueEnabled) return ValOutcome::Disabled;
    if (!item.priceKnown) return ValOutcome::Unknown;
    if (ComparedValue(item, s.valuePerCell) >= s.minValue) {
        *pass = true;
        return ValOutcome::Disabled;   // unused when it passes
    }
    return ValOutcome::Low;
}

LootDecision Pick(const char* why) { return LootDecision{true, why}; }
LootDecision Skip(const char* why) { return LootDecision{false, why}; }

}  // namespace

LootItemKeys LootItemKeys::From(const LootItemInfo& item) {
    LootItemKeys k;
    k.base = FoldCase(item.baseName);
    k.unique = FoldCase(item.uniqueName);
    k.path = ToLowerAscii(StripAtSuffix(item.path));
    return k;
}

void LootNameMatcher::Build(const std::vector<std::string>& entries) {
    m_entries.clear();
    m_entries.reserve(entries.size());
    for (const auto& raw : entries) {
        std::string t = Trim(raw);
        Entry e;
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            e.exact = true;
            t = Trim(std::string_view(t).substr(1, t.size() - 2));
        }
        if (t.empty()) continue;
        e.text = FoldCase(t);
        m_entries.push_back(std::move(e));
    }
}

bool LootNameMatcher::Matches(const LootItemKeys& k) const {
    for (const Entry& e : m_entries) {
        if (e.exact) {
            if ((!k.base.empty() && k.base == e.text) || (!k.unique.empty() && k.unique == e.text)) return true;
            continue;
        }
        if (k.base.find(e.text) != std::string::npos) return true;
        if (!k.unique.empty() && k.unique.find(e.text) != std::string::npos) return true;
        if (k.path.find(e.text) != std::string::npos) return true;
    }
    return false;
}

LootDecision EvaluateLoot(const LootItemInfo& item, const LootSettings& s, const LootItemKeys& keys,
                          const LootNameMatcher& include, const LootNameMatcher& exclude) {
    if (!exclude.Empty() && exclude.Matches(keys)) return Skip("on your exclude list");
    if (item.category == ItemCategory::Quest && s.alwaysQuestItems) return Pick("quest item");
    if (!s.enabled) return Skip("loot pickup is off");
    if (!include.Empty() && include.Matches(keys)) return Pick("on your include list");

    const CatOutcome cat = EvaluateCategory(item, s);
    if (cat == CatOutcome::Pass) return Pick(item.category == ItemCategory::Uniques ? "unique item" : "its category is on");
    bool valuePass = false;
    const ValOutcome val = EvaluateValue(item, s, &valuePass);
    if (valuePass) return Pick("worth at least your value threshold");
    return Skip(kSkipReasons[static_cast<int>(cat)][static_cast<int>(val)]);
}

LootDecision EvaluateLoot(const LootItemInfo& item, const LootSettings& s) {
    LootNameMatcher include, exclude;
    include.Build(s.includeNames);
    exclude.Build(s.excludeNames);
    return EvaluateLoot(item, s, LootItemKeys::From(item), include, exclude);
}

bool TrustPriceCategory(const std::string& priceCategory, bool unique) {
    // The host price categories (poe2scout vocabulary for both sources;
    // POEFixer/core/prices/CLAUDE.md), plus the user's own custom prices.
    static const char* const kStackable[] = {"currency", "fragments", "abyss",    "uncutgems", "essences",
                                             "ultimatum", "idol",     "runes",    "ritual",    "expedition",
                                             "delirium", "breach",    "lineagesupportgems",    "vaultkeys",
                                             "incursion", "verisium", "vaal",     "custom"};
    static const char* const kUnique[] = {"accessory", "armour", "flask", "jewel", "map", "weapon", "sanctum"};
    if (priceCategory.empty()) return false;
    if (unique) {
        for (const char* c : kUnique)
            if (priceCategory == c) return true;
        return false;
    }
    for (const char* c : kStackable)
        if (priceCategory == c) return true;
    return false;
}

}  // namespace fb
