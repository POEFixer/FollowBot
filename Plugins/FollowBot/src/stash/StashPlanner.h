// StashPlanner.h - what to deposit, in which order, into which tab
// (design §5.11). Pure.
//
// Contract (this interface is final):
// backpack items that touch no protected cell and match no keep rule
// (keepCategories, keepNames) are deposited, largest first, grouped by target
// tab: the first route whose category matches, else defaultTab ("" = the open
// tab). `categories[i]` is the ItemCategory of items[i] (LootCategories).
#pragma once

#include "../world/World.h"
#include "StashSettings.h"

#include <string>
#include <vector>

namespace fb {

struct DepositItem {
    size_t       bagIndex = 0;   // index into the backpack item list
    int          x = 0, y = 0, w = 1, h = 1;
    ItemCategory category = ItemCategory::Other;
};

struct DepositGroup {
    std::string tab;              // "" = whatever tab is open
    std::vector<DepositItem> items;
};

struct DepositPlan {
    std::vector<DepositGroup> groups;
    int kept = 0;                 // matched a keep rule
    int protectedSkipped = 0;     // touched a protected cell
};

// Group order: the open tab ("" - whatever is selected when the panel opens) first,
// before any tab switch changes what "open" means; then named tabs in the order
// they first appear in the routes; the default tab last. Tab names group
// trimmed and case-insensitively. Quest items are always kept (the game refuses
// them in a stash).
DepositPlan PlanDeposit(const std::vector<BagItem>& items, const std::vector<ItemCategory>& categories,
                        const StashSettings& s);

// --- helpers (also used by the trigger) -----------------------------------
// Keep names trimmed and case-folded once (empty entries dropped).
std::vector<std::string> FoldKeepNames(const std::vector<std::string>& names);
// The item's base name, unique name or path contains one of the folded names.
bool MatchesKeepName(const BagItem& item, const std::vector<std::string>& foldedNames);
// The tab items of `c` go to: the first route of that category with a tab
// name, else defaultTab ("" = the open tab).
const std::string& RouteTabFor(ItemCategory c, const StashSettings& s);
inline int PlanItemCount(const DepositPlan& p) {
    int n = 0;
    for (const auto& g : p.groups) n += static_cast<int>(g.items.size());
    return n;
}

}  // namespace fb
