// InventoryModel.cpp - see InventoryModel.h.
#include "InventoryModel.h"

#include "../common/Strings.h"

#include <algorithm>

namespace fb {

bool IsPlayerInventoryName(const std::string& name) {
    static const char* const kPrefixes[] = {"MainInventory", "BodyArmour", "Weapon", "Offhand", "Helm",
                                            "Amulet",        "Ring",       "Gloves", "Boots",   "Belt",
                                            "Flask",         "Cursor",     "Trinket", "Charm",  "Ritual"};
    for (const char* p : kPrefixes)
        if (StartsWith(name, p)) return true;
    return false;
}

PanelState DetectPanels(const std::vector<InvListingEntry>& listing) {
    PanelState st;
    const InvListingEntry* bestStorage = nullptr;
    for (const auto& inv : listing) {
        if (!inv.grid.valid || inv.cols <= 0 || inv.rows <= 0) continue;
        if (inv.cols * inv.rows >= 40) st.anyPanelOpen = true;
        if (inv.id == kBackpackInventoryId) {
            st.backpackOpen = true;
            st.backpack = inv.grid;
            st.backpack.cols = inv.cols;
            st.backpack.rows = inv.rows;
            st.backpack.inventoryId = inv.id;
            continue;
        }
        if (inv.id == kGuildStashInventoryId) {
            st.guildOpen = true;
            bestStorage = &inv;   // the guild tab wins: only one stash panel is open at a time
            continue;
        }
        if (IsPlayerInventoryName(inv.name)) continue;
        if (inv.cols * inv.rows < 12) continue;   // tiny special grids are not a storage tab
        if (!bestStorage || (bestStorage->id != kGuildStashInventoryId && inv.items.size() > bestStorage->items.size()))
            bestStorage = &inv;
    }
    if (bestStorage) {
        st.stashOpen = true;
        st.openStashId = bestStorage->id;
        st.stashItemCount = static_cast<int>(bestStorage->items.size());
        st.stash = bestStorage->grid;
        st.stash.cols = bestStorage->cols;
        st.stash.rows = bestStorage->rows;
        st.stash.inventoryId = bestStorage->id;
    }
    return st;
}

int ComputeOccupancy(const std::vector<BagItem>& items, Occupancy& occ) {
    occ.fill(0);
    for (const auto& it : items) {
        const int w = std::clamp(it.w, 1, kBackpackCols), h = std::clamp(it.h, 1, kBackpackRows);
        for (int y = it.y; y < it.y + h; ++y)
            for (int x = it.x; x < it.x + w; ++x)
                if (x >= 0 && y >= 0 && x < kBackpackCols && y < kBackpackRows) occ[static_cast<size_t>(y * kBackpackCols + x)] = 1;
    }
    int used = 0;
    for (uint8_t c : occ) used += c;
    return used;
}

bool FindFreeRect(const Occupancy& occ, int w, int h, int& outX, int& outY) {
    w = std::clamp(w, 1, kBackpackCols);
    h = std::clamp(h, 1, kBackpackRows);
    for (int x = 0; x + w <= kBackpackCols; ++x) {
        for (int y = 0; y + h <= kBackpackRows; ++y) {
            bool fits = true;
            for (int yy = y; yy < y + h && fits; ++yy)
                for (int xx = x; xx < x + w; ++xx)
                    if (occ[static_cast<size_t>(yy * kBackpackCols + xx)]) { fits = false; break; }
            if (fits) {
                outX = x;
                outY = y;
                return true;
            }
        }
    }
    return false;
}

bool TouchesMask(const BagItem& item, uint64_t mask) {
    if (mask == 0) return false;
    const int w = std::clamp(item.w, 1, kBackpackCols), h = std::clamp(item.h, 1, kBackpackRows);
    for (int y = item.y; y < item.y + h; ++y)
        for (int x = item.x; x < item.x + w; ++x) {
            if (x < 0 || y < 0 || x >= kBackpackCols || y >= kBackpackRows) continue;
            if ((mask >> (y * kBackpackCols + x)) & 1ull) return true;
        }
    return false;
}

}  // namespace fb
