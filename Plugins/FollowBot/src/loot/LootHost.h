// LootHost.h - what the loot scanner needs from the game, as an interface.
// Pure: the PluginSDK implementation lives in LootScanner.cpp (plugin build);
// the unit tests drive the scanner with a fake (tests/test_loot.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace fb {

// The inner item of a ground WorldItem.
struct ResolvedItem {
    std::string path;              // "Metadata/Items/..." (empty = not readable yet)
    std::string baseName;          // localized base type name, UTF-8 ("" = not readable yet)
    std::string uniqueName;        // localized unique name (rarity 3 only), UTF-8
    int         rarity = -1;       // 0..3, -1 unknown
    int         w = 0, h = 0;      // backpack size, 0 = unknown
    int         stack = 1;
    int         maxStack = 0;      // 0 unknown
};

// One answer of the host price list.
struct PriceQuote {
    bool        found = false;
    float       chaos = 0.f, divine = 0.f, exalt = 0.f;   // per unit
    std::string category;          // host price category ("currency", "weapon", ...)
};

class ILootHost {
public:
    virtual ~ILootHost() = default;
    // Reads the inner item of the WorldItem entity at `worldItemAddr`; false
    // when it cannot be read yet (mid-spawn, stale address).
    virtual bool ResolveItem(uintptr_t worldItemAddr, ResolvedItem& out) = 0;
    // Price list lookup by display name; false = no answer. The price list is English:
    // with another game language only a host that prices by item (below) finds prices.
    virtual bool LookupPrice(const std::string& name, PriceQuote& out) = 0;
    // The host prices an item by what it is (base item, rarity, unique), never by the
    // localized name the game shows (POEFixer 371+): right in every game language.
    virtual bool CanPriceByItem() = 0;
    // The price of the WorldItem at `worldItemAddr` by what it is. true = answered
    // (out.found tells whether the item has a price); false = the host could not read
    // the item (or cannot price by item): ask LookupPrice by name instead.
    virtual bool LookupPriceByItem(uintptr_t worldItemAddr, PriceQuote& out) = 0;
    // Changes whenever the host publishes new prices (0 = the host cannot tell).
    virtual uint64_t PriceRevision() = 0;
};

}  // namespace fb
