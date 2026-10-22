// LootScanner.h - turns this tick's ground items (World::items) into resolved,
// filtered pickup candidates (design §5.9). Adapter: the inner item resolution
// (Entities.GetWorldItemInner, Inventory.ReadItem*, Components.ReadBase for the
// size, Prices.LookupPrice) happens in LootScanner.cpp through BotContext::sdk.
//
// Contract (this interface is final):
//  - Update: at most every 150 ms; resolves new items (cache per entity id until
//    the area changes, unresolved retried at most once a second), prices
//    memoised per base + rarity for the area with priceLookupBudget lookups
//    per pass, classifies (LootCategories), filters (LootFilter), and keeps the
//    wanted ones within scanRadius sorted nearest first, with `fits` = a free
//    backpack rectangle exists for the item's size (InventoryModel).
//  - Without an SDK (tests) Update does nothing.
//
// Details: prices are looked up only while a rule uses them (the value rule
// or a category minimum value) or the Diagnostics list is open: a host lookup
// that misses scans the whole price list. A unique is priced by its unique
// name, stackable loot by its base name; magic / rare equipment has no price.
// New items are read at most 24 per pass (a fresh area can hold 100+).
//
// Layout: Update (LootScanner.cpp, plugin build only) wraps the PluginSDK in an
// ILootHost and calls UpdateWith, the pure core. UpdateWith, Clear and
// NotePickedUp are defined in LootScanCore.h: a file that calls them includes
// it. The unit tests link their own Update that drives UpdateWith with a fake
// host (tests/test_loot.cpp).
//
// Threads: everything runs on the worker except RequestDiagnostics /
// Diagnostics, which the settings tab calls from the render thread.
#pragma once

#include "../brain/Task.h"
#include "LootFilter.h"
#include "LootHost.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fb {

struct LootCandidate {
    GroundItem   item;
    LootItemInfo info;
    LootDecision decision;
    bool         fits = true;         // a free backpack rectangle exists (or no room is needed)
    bool         blacklisted = false; // gave up on it for a while (All() only)
};

// One row of the Diagnostics list (published for the render thread).
struct LootDiagRow {
    uint32_t     id = 0;
    std::string  name;                // unique name, else base name, else the path's last part
    ItemCategory category = ItemCategory::Other;
    int          rarity = -1;
    int          stack = 1;
    float        dist = 0.f;
    bool         priceKnown = false;
    float        value = 0.f;         // LootSettings::currency, per stack
    bool         pick = false;
    const char*  reason = "";         // static string
    bool         fits = true;
    bool         blacklisted = false;
};

struct LootDiagnostics {
    uint64_t     atMs = 0;
    std::vector<LootDiagRow> rows;    // resolved items within the scan radius, nearest first
    int          unresolved = 0;      // within the radius, not readable yet
    int          wanted = 0;          // rows with pick && !blacklisted
    int          blacklisted = 0;     // entries in the blacklist still running
    bool         inventoryValid = false;
    int          freeCells = 0;       // backpack (incl. items picked since the last read)
    bool         inventoryFull = false;
    size_t       priceMemo = 0;       // memoised price answers
    uint64_t     priceRevision = 0;
    RefCurrency  currency = RefCurrency::Exalted;
};

class LootScanner {
public:
    void Update(BotContext& ctx);
    void UpdateWith(BotContext& ctx, ILootHost& host);
    void Clear();

    // Wanted items, nearest first (blacklisted ones excluded).
    const std::vector<LootCandidate>& Candidates() const { return m_candidates; }
    // Every resolved item of the last pass with its decision (Diagnostics).
    const std::vector<LootCandidate>& All() const { return m_all; }

    // The next Update runs a pass at once (after a pickup, the list changed).
    void RequestPass() { m_forcePass = true; }
    // A picked item is in the backpack before the next inventory read: `fits`
    // accounts for it until a read newer than the pickup arrives.
    void NotePickedUp(const LootItemInfo& info, uint64_t now);

    // Within the scan radius but not readable yet (last pass).
    int UnresolvedCount() const { return m_unresolved; }
    // Free backpack cells of the last pass, including picks not read back yet.
    int FreeCells() const { return m_freeCells; }
    uint64_t Passes() const { return m_passes; }
    int PriceLookups() const { return m_priceLookups; }   // host lookups since the last Clear
    int Resolves() const { return m_resolves; }           // host resolutions since the last Clear

    // Render thread: ask for a Diagnostics snapshot (published while asked for
    // within the last second) and read the latest one.
    void RequestDiagnostics(uint64_t nowMs) const { m_diagWantedMs.store(nowMs, std::memory_order_relaxed); }
    bool DiagnosticsWanted(uint64_t nowMs) const {
        const uint64_t t = m_diagWantedMs.load(std::memory_order_relaxed);
        return t != 0 && (t >= nowMs || nowMs - t <= 1000);
    }
    std::shared_ptr<const LootDiagnostics> Diagnostics() const {
        std::lock_guard<std::mutex> lock(m_diagMu);
        return m_diag;
    }

private:
    struct Entry {
        uintptr_t    address = 0;
        bool         resolved = false;
        int          tries = 0;
        uint64_t     retryAtMs = 0;
        LootItemInfo info;
        LootItemKeys keys;
        bool         priceDone = false;   // looked up (or nothing to look up)
        bool         priceTrusted = false;
        float        unitChaos = 0.f, unitDivine = 0.f, unitExalt = 0.f;
        uint64_t     decidedFor = 0;      // settings fingerprint the decision belongs to
        LootDecision decision;
    };
    struct PriceMemo {
        bool  trusted = false;
        float chaos = 0.f, divine = 0.f, exalt = 0.f;
    };
    struct PendingPickup {
        std::string path;
        int         w = 1, h = 1, maxStack = 0;
        uint64_t    atMs = 0;
    };

    std::vector<LootCandidate> m_candidates;
    std::vector<LootCandidate> m_all;
    uint64_t m_lastUpdateMs = 0;
    uint32_t m_area = 0;
    bool     m_haveArea = false;
    bool     m_forcePass = false;

    std::unordered_map<uint32_t, Entry>         m_cache;
    std::unordered_map<std::string, PriceMemo>  m_prices;
    uint64_t m_priceRevision = 0;
    uint64_t m_pricesClearedMs = 0;
    uint64_t m_settingsHash = 0;
    LootNameMatcher m_include, m_exclude;
    std::vector<PendingPickup> m_pending;
    int      m_unresolved = 0;
    int      m_freeCells = kBackpackCells;
    uint64_t m_passes = 0;
    int      m_priceLookups = 0;
    int      m_resolves = 0;

    mutable std::mutex m_diagMu;
    std::shared_ptr<const LootDiagnostics> m_diag;
    mutable std::atomic<uint64_t> m_diagWantedMs{0};
};

}  // namespace fb
