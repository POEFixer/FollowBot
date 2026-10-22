// StashPlanner.cpp - see StashPlanner.h. Pure: no SDK.
#include "StashPlanner.h"

#include "../common/Strings.h"

#include <algorithm>

namespace fb {

namespace {

// The cells an item covers, clamped into the 12 x 5 backpack, touch a protected bit.
bool TouchesProtected(const BagItem& it, const StashSettings& s) {
    if (s.protectedCells == 0) return false;
    const int w = std::clamp(it.w, 1, kBackpackCols), h = std::clamp(it.h, 1, kBackpackRows);
    for (int y = it.y; y < it.y + h; ++y)
        for (int x = it.x; x < it.x + w; ++x)
            if (s.IsProtected(x, y)) return true;
    return false;
}

// Group key of a tab name: trimmed and case-folded ("Dump " and "dump" are one tab).
std::string TabKey(std::string_view tab) { return FoldCase(Trim(tab)); }

}  // namespace

std::vector<std::string> FoldKeepNames(const std::vector<std::string>& names) {
    std::vector<std::string> out;
    out.reserve(names.size());
    for (const auto& n : names) {
        std::string t = Trim(n);
        if (!t.empty()) out.push_back(FoldCase(t));
    }
    return out;
}

bool MatchesKeepName(const BagItem& it, const std::vector<std::string>& foldedNames) {
    if (foldedNames.empty()) return false;
    const std::string base = FoldCase(it.baseName);
    const std::string uniq = FoldCase(it.uniqueName);
    const std::string path = FoldCase(it.path);
    for (const auto& n : foldedNames) {
        if (n.empty()) continue;
        if (base.find(n) != std::string::npos || uniq.find(n) != std::string::npos || path.find(n) != std::string::npos)
            return true;
    }
    return false;
}

const std::string& RouteTabFor(ItemCategory c, const StashSettings& s) {
    for (const auto& r : s.routes)
        if (r.category == c && !Trim(r.tab).empty()) return r.tab;
    return s.defaultTab;
}

DepositPlan PlanDeposit(const std::vector<BagItem>& items, const std::vector<ItemCategory>& categories,
                        const StashSettings& s) {
    DepositPlan plan;
    const std::vector<std::string> keep = FoldKeepNames(s.keepNames);

    // Group order: the open tab ("" - whatever is selected when the panel opens)
    // first, before any tab switch changes what "open" means; then named tabs in
    // the order they first appear in the routes; then the default tab.
    std::vector<std::string> order;   // tab keys
    order.push_back(std::string());
    auto addOrder = [&order](const std::string& tab) {
        const std::string key = TabKey(tab);
        if (key.empty()) return;
        if (std::find(order.begin(), order.end(), key) == order.end()) order.push_back(key);
    };
    for (const auto& r : s.routes) addOrder(r.tab);
    addOrder(s.defaultTab);

    std::vector<DepositGroup> groups(order.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const BagItem& it = items[i];
        const ItemCategory cat = i < categories.size() ? categories[i] : ItemCategory::Other;
        if (TouchesProtected(it, s)) {
            ++plan.protectedSkipped;
            continue;
        }
        // Quest items never go to a stash (the game refuses them; a refused click
        // would also count toward "tab full").
        if (cat == ItemCategory::Quest || s.Keeps(cat) || MatchesKeepName(it, keep)) {
            ++plan.kept;
            continue;
        }
        const std::string& tab = RouteTabFor(cat, s);
        const std::string key = TabKey(tab);
        size_t g = 0;
        for (size_t k = 0; k < order.size(); ++k)
            if (order[k] == key) { g = k; break; }
        if (groups[g].items.empty() && !key.empty()) groups[g].tab = Trim(tab);
        DepositItem d;
        d.bagIndex = i;
        d.x = it.x;
        d.y = it.y;
        d.w = std::max(1, it.w);
        d.h = std::max(1, it.h);
        d.category = cat;
        groups[g].items.push_back(d);
    }

    // Largest first; ties in the game's column-major order (x, then y).
    for (auto& g : groups) {
        std::stable_sort(g.items.begin(), g.items.end(), [](const DepositItem& a, const DepositItem& b) {
            const int aa = a.w * a.h, bb = b.w * b.h;
            if (aa != bb) return aa > bb;
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });
    }
    for (auto& g : groups)
        if (!g.items.empty()) plan.groups.push_back(std::move(g));
    return plan;
}

}  // namespace fb
