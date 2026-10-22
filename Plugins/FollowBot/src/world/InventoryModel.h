// InventoryModel.h - backpack occupancy, free-space search and item-panel
// detection over a plain inventory listing (design §5.1). Pure: no SDK.
//
// The WorldBuilder converts the host's inventories (Inventory.GetAll /
// ReadSnapshot) into InvListingEntry values; everything here is plain math.
#pragma once

#include "World.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fb {

inline constexpr int kBackpackInventoryId = 1;      // MainInventory1, 12 x 5
inline constexpr int kCursorInventoryId = 13;       // Cursor1: the item held on the cursor
inline constexpr int kGuildStashInventoryId = 10002;  // the open guild stash tab (host synthetic id)

struct InvListingEntry {
    int         id = 0;
    std::string name;            // host inventory name ("MainInventory1", "GuildStash1", ...)
    int         cols = 0, rows = 0;
    GridInfo    grid;            // grid.valid = on screen
    std::vector<BagItem> items;
};

struct PanelState {
    bool     anyPanelOpen = false;   // any item grid of >= 40 cells on screen
    bool     backpackOpen = false;
    bool     stashOpen = false;      // a non-player storage grid on screen
    bool     guildOpen = false;
    int      openStashId = 0;
    int      stashItemCount = 0;
    GridInfo backpack;
    GridInfo stash;
};

// A player-equipment / special inventory name (never "storage").
bool IsPlayerInventoryName(const std::string& name);

PanelState DetectPanels(const std::vector<InvListingEntry>& listing);

using Occupancy = std::array<uint8_t, kBackpackCells>;

// Marks every cell the items cover (clamped into the 12 x 5 grid); returns the
// number of occupied cells.
int ComputeOccupancy(const std::vector<BagItem>& items, Occupancy& occ);

// First free w x h rectangle, column-major like the game fills (x outer, y inner).
bool FindFreeRect(const Occupancy& occ, int w, int h, int& outX, int& outY);
inline bool HasFreeRect(const Occupancy& occ, int w, int h) {
    int x = 0, y = 0;
    return FindFreeRect(occ, w, h, x, y);
}

// True when any cell of the item lies on a set bit of the 12 x 5 mask (y*12+x).
bool TouchesMask(const BagItem& item, uint64_t mask);

}  // namespace fb
