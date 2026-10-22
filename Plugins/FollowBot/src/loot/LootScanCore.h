// LootScanCore.h - the pure core of the loot scanner (see LootScanner.h):
// resolution cache, price memo, classification, filter decisions, backpack
// fit and the Diagnostics snapshot. No SDK: the game is reached through
// ILootHost. Included by LootScanner.cpp, PickupTask.cpp and the unit tests.
#pragma once

#include "../common/Strings.h"
#include "../world/InventoryModel.h"
#include "LootCategories.h"
#include "LootScanner.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace fb {

namespace lootcore {

inline constexpr uint64_t kPassIntervalMs = 150;      // one pass at most this often
inline constexpr uint64_t kRetryMs = 1000;            // an unreadable item is retried at most once a second
inline constexpr int      kResolveBudget = 24;        // new items read per pass (a fresh area may hold 100+)
inline constexpr int      kNameTries = 3;             // reads before an item without a base name is accepted
inline constexpr size_t   kCacheCap = 4096;
inline constexpr uint64_t kPriceStaleMs = 600000;     // hosts without a price revision: re-read every 10 min
inline constexpr uint64_t kPendingReadMarginMs = 250; // an inventory read this long after a pickup contains it
inline constexpr uint64_t kPendingMaxMs = 3000;       // a pickup not seen in any read by then is dropped
inline constexpr size_t   kPendingCap = 16;

// FNV-1a over everything the filter decision depends on.
class Fingerprint {
public:
    void Bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            m_h ^= b[i];
            m_h *= 1099511628211ull;
        }
    }
    template <typename T>
    void Pod(const T& v) { Bytes(&v, sizeof(v)); }
    void Str(const std::string& s) {
        Bytes(s.data(), s.size());
        Pod(s.size());
    }
    uint64_t Value() const { return m_h == 0 ? 1 : m_h; }   // 0 means "never decided"

private:
    uint64_t m_h = 1469598103934665603ull;
};

inline uint64_t FilterFingerprint(const LootSettings& s) {
    Fingerprint f;
    f.Pod(s.enabled);
    f.Pod(s.alwaysQuestItems);
    for (const auto& r : s.categories) {
        f.Pod(r.enabled);
        f.Pod(r.minRarity);
        f.Pod(r.minValue);
    }
    for (const auto& n : s.includeNames) f.Str(n);
    f.Pod(s.includeNames.size());
    for (const auto& n : s.excludeNames) f.Str(n);
    f.Pod(s.excludeNames.size());
    f.Pod(s.valueEnabled);
    f.Pod(s.minValue);
    f.Pod(s.valuePerCell);
    f.Pod(s.currency);
    return f.Value();
}

inline float UnitIn(RefCurrency c, float chaos, float divine, float exalt) {
    switch (c) {
        case RefCurrency::Chaos:   return chaos;
        case RefCurrency::Divine:  return divine;
        case RefCurrency::Exalted: return exalt;
        case RefCurrency::Count:   break;
    }
    return exalt;
}

inline bool FiniteNonNegative(float v) { return std::isfinite(v) && v >= 0.f; }

// Unique name, else base name, else the last part of the path.
inline std::string DisplayName(const LootItemInfo& i) {
    if (!i.uniqueName.empty()) return i.uniqueName;
    if (!i.baseName.empty()) return i.baseName;
    const std::string_view p = StripAtSuffix(i.path);
    const size_t slash = p.rfind('/');
    return std::string(slash == std::string_view::npos ? p : p.substr(slash + 1));
}

inline bool SamePathCI(std::string_view a, std::string_view b) {
    a = StripAtSuffix(a);
    b = StripAtSuffix(b);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (AsciiLower(a[i]) != AsciiLower(b[i])) return false;
    return true;
}

// A backpack stack of the same item with room left.
inline bool HasStackRoom(const std::vector<BagItem>& bag, const std::string& path, int maxStack) {
    if (maxStack <= 1 || path.empty()) return false;
    for (const BagItem& b : bag)
        if (b.stack > 0 && b.stack < maxStack && SamePathCI(b.path, path)) return true;
    return false;
}

// Whether the item can go into the backpack: gold needs no room, quest items
// are always tried (the game may keep them apart), an unread backpack is
// tried; else a stack with room or a free w x h rectangle that still leaves
// minFreeCells free.
inline bool FitsBackpack(const LootItemInfo& info, const Occupancy& occ, int freeCells, const InventoryState& inv,
                         int minFreeCells) {
    if (!NeedsBackpackSpace(info.category) || info.category == ItemCategory::Quest) return true;
    if (!inv.valid) return true;
    if (HasStackRoom(inv.items, info.path, info.maxStack)) return true;
    const int w = std::clamp(info.w, 1, kBackpackCols), h = std::clamp(info.h, 1, kBackpackRows);
    int x = 0, y = 0;
    if (!FindFreeRect(occ, w, h, x, y)) return false;
    return minFreeCells <= 0 || freeCells - w * h >= minFreeCells;
}

inline void MarkRect(Occupancy& occ, int x, int y, int w, int h) {
    for (int yy = y; yy < y + h && yy < kBackpackRows; ++yy)
        for (int xx = x; xx < x + w && xx < kBackpackCols; ++xx) occ[static_cast<size_t>(yy * kBackpackCols + xx)] = 1;
}

inline LootCandidate& Slot(std::vector<LootCandidate>& v, size_t i) {
    if (i < v.size()) return v[i];
    v.emplace_back();
    return v.back();
}

}  // namespace lootcore

inline void LootScanner::Clear() {
    m_candidates.clear();
    m_all.clear();
    m_cache.clear();
    m_prices.clear();
    m_pending.clear();
    m_lastUpdateMs = 0;
    m_forcePass = false;
    m_priceRevision = 0;
    m_pricesClearedMs = 0;
    m_settingsHash = 0;
    m_unresolved = 0;
    m_freeCells = kBackpackCells;
    m_priceLookups = 0;
    m_resolves = 0;
}

inline void LootScanner::NotePickedUp(const LootItemInfo& info, uint64_t now) {
    if (!NeedsBackpackSpace(info.category) || info.category == ItemCategory::Quest) return;
    if (m_pending.size() >= lootcore::kPendingCap) m_pending.erase(m_pending.begin());
    PendingPickup p;
    p.path = info.path;
    p.w = std::clamp(info.w, 1, kBackpackCols);
    p.h = std::clamp(info.h, 1, kBackpackRows);
    p.maxStack = info.maxStack;
    p.atMs = now;
    m_pending.push_back(std::move(p));
    m_forcePass = true;
}

inline void LootScanner::UpdateWith(BotContext& ctx, ILootHost& host) {
    using namespace lootcore;
    const World& w = ctx.w;
    const LootSettings& s = ctx.p.loot;
    const uint64_t now = ctx.now;

    if (!m_haveArea || w.gate.areaCounter != m_area) {
        Clear();   // entity ids and prices are per area
        m_area = w.gate.areaCounter;
        m_haveArea = true;
    }
    if (!m_forcePass && m_lastUpdateMs != 0 && now >= m_lastUpdateMs && now - m_lastUpdateMs < kPassIntervalMs) return;
    m_forcePass = false;
    m_lastUpdateMs = now;
    ++m_passes;

    // New prices (or a host that cannot tell, every 10 minutes): ask again.
    const uint64_t rev = host.PriceRevision();
    if (rev != m_priceRevision || (rev == 0 && (m_pricesClearedMs == 0 || now - m_pricesClearedMs > kPriceStaleMs)) ||
        m_prices.size() > kCacheCap) {
        m_priceRevision = rev;
        m_pricesClearedMs = now;
        m_prices.clear();
        for (auto& kv : m_cache) kv.second.priceDone = false;
    }
    // Settings changed: the name lists are folded again and every item is decided again.
    const uint64_t fp = FilterFingerprint(s);
    if (fp != m_settingsHash) {
        m_settingsHash = fp;
        m_include.Build(s.includeNames);
        m_exclude.Build(s.excludeNames);
    }
    if (m_cache.size() > kCacheCap) m_cache.clear();

    // Backpack occupancy plus the picks the last read does not contain yet.
    const InventoryState& inv = w.inv;
    Occupancy occ{};
    if (inv.valid) occ = inv.occupied;
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [&](const PendingPickup& p) {
                                       return (inv.readMs != 0 && inv.readMs >= p.atMs + kPendingReadMarginMs) ||
                                              now - p.atMs > kPendingMaxMs;
                                   }),
                    m_pending.end());
    if (inv.valid) {
        for (const PendingPickup& p : m_pending) {
            if (HasStackRoom(inv.items, p.path, p.maxStack)) continue;
            int x = 0, y = 0;
            if (FindFreeRect(occ, p.w, p.h, x, y)) MarkRect(occ, x, y, p.w, p.h);
        }
    }
    int used = 0;
    for (uint8_t c : occ) used += c;
    m_freeCells = kBackpackCells - used;

    int resolveBudget = kResolveBudget;
    int priceBudget = s.priceLookupBudget;
    // Prices are asked for only when a rule (or the Diagnostics list) uses
    // them: a host lookup that misses scans the whole price list.
    const bool diag = DiagnosticsWanted(now);
    m_unresolved = 0;
    size_t nAll = 0;
    for (const GroundItem& gi : w.items) {
        if (!(gi.dist <= s.scanRadius) || gi.id == 0) continue;
        auto ins = m_cache.try_emplace(gi.id);
        Entry& e = ins.first->second;
        if (!ins.second && e.address != gi.address) e = Entry{};   // the id now names another entity
        e.address = gi.address;

        // 1. Resolve the inner item once (retried at most once a second).
        if (!e.resolved) {
            if (now < e.retryAtMs || resolveBudget <= 0) {
                ++m_unresolved;
                continue;
            }
            --resolveBudget;
            ++m_resolves;
            ++e.tries;
            ResolvedItem r;
            const bool ok = gi.address != 0 && host.ResolveItem(gi.address, r) && !r.path.empty();
            if (!ok || (r.baseName.empty() && e.tries < kNameTries)) {
                e.retryAtMs = now + kRetryMs;
                ++m_unresolved;
                continue;
            }
            LootItemInfo& i = e.info;
            i.id = gi.id;
            i.path = std::move(r.path);
            i.baseName = std::move(r.baseName);
            i.uniqueName = r.rarity == 3 ? std::move(r.uniqueName) : std::string();
            i.rarity = (r.rarity >= 0 && r.rarity <= 3) ? r.rarity : -1;
            i.w = r.w > 0 ? std::clamp(r.w, 1, kBackpackCols) : 1;
            i.h = r.h > 0 ? std::clamp(r.h, 1, kBackpackRows) : 1;
            i.stack = r.stack > 0 ? r.stack : 1;
            i.maxStack = r.maxStack > 0 ? r.maxStack : 0;
            i.category = ClassifyItem(i.path, i.rarity);
            e.keys = LootItemKeys::From(i);
            e.resolved = true;
            e.priceDone = false;
            e.decidedFor = 0;
        }

        // 2. Price (memoised per name for the area; rationed per pass).
        const LootCategoryRule& rule = s.categories[static_cast<size_t>(e.info.category)];
        if (!e.priceDone && (diag || s.valueEnabled || rule.minValue > 0.f)) {
            const bool unique = e.info.category == ItemCategory::Uniques;
            const std::string* name = nullptr;
            if (unique) name = &e.info.uniqueName;   // unidentified uniques: the host names them by their art
            else if (PricedByBaseName(e.info.category)) name = &e.info.baseName;
            if (!name || name->empty()) {
                e.priceDone = true;
                e.priceTrusted = false;
                e.decidedFor = 0;
            } else {
                std::string key;
                key.reserve(name->size() + 2);
                key += unique ? "u:" : "b:";
                key += *name;
                auto it = m_prices.find(key);
                if (it == m_prices.end() && priceBudget > 0) {
                    --priceBudget;
                    ++m_priceLookups;
                    PriceQuote q;
                    PriceMemo m;
                    if (host.LookupPrice(*name, q) && q.found && TrustPriceCategory(q.category, unique) &&
                        FiniteNonNegative(q.chaos) && FiniteNonNegative(q.divine) && FiniteNonNegative(q.exalt)) {
                        m.trusted = true;
                        m.chaos = q.chaos;
                        m.divine = q.divine;
                        m.exalt = q.exalt;
                    }
                    it = m_prices.emplace(std::move(key), m).first;
                }
                if (it != m_prices.end()) {
                    e.priceDone = true;
                    e.priceTrusted = it->second.trusted;
                    e.unitChaos = it->second.chaos;
                    e.unitDivine = it->second.divine;
                    e.unitExalt = it->second.exalt;
                    e.decidedFor = 0;
                }
            }
        }
        e.info.priceKnown = e.priceDone && e.priceTrusted;
        e.info.value = e.info.priceKnown
                           ? UnitIn(s.currency, e.unitChaos, e.unitDivine, e.unitExalt) * static_cast<float>(e.info.stack)
                           : 0.f;

        // 3. Decide (again only when the settings or the price changed).
        if (e.decidedFor != fp) {
            e.decision = EvaluateLoot(e.info, s, e.keys, m_include, m_exclude);
            e.decidedFor = fp;
        }

        LootCandidate& c = Slot(m_all, nAll++);
        c.item = gi;
        c.info = e.info;
        c.decision = e.decision;
        c.fits = FitsBackpack(e.info, occ, m_freeCells, inv, s.minFreeCells);
        c.blacklisted = ctx.mem.IsBlacklisted(gi.id, now);
    }
    m_all.resize(nAll);
    std::sort(m_all.begin(), m_all.end(), [](const LootCandidate& a, const LootCandidate& b) {
        if (a.item.dist != b.item.dist) return a.item.dist < b.item.dist;
        return a.item.id < b.item.id;
    });
    size_t nCand = 0;
    for (const LootCandidate& c : m_all)
        if (c.decision.pick && !c.blacklisted) Slot(m_candidates, nCand++) = c;
    m_candidates.resize(nCand);

    // 4. Diagnostics snapshot, only while the settings tab shows it.
    if (DiagnosticsWanted(now)) {
        auto d = std::make_shared<LootDiagnostics>();
        d->atMs = now;
        d->unresolved = m_unresolved;
        d->inventoryValid = inv.valid;
        d->freeCells = m_freeCells;
        d->inventoryFull = ctx.mem.inventoryFull;
        d->priceMemo = m_prices.size();
        d->priceRevision = m_priceRevision;
        d->currency = s.currency;
        for (const auto& kv : ctx.mem.lootBlacklist)
            if (now < kv.second) ++d->blacklisted;
        d->rows.reserve(m_all.size());
        for (const LootCandidate& c : m_all) {
            LootDiagRow r;
            r.id = c.item.id;
            r.name = DisplayName(c.info);
            r.category = c.info.category;
            r.rarity = c.info.rarity;
            r.stack = c.info.stack;
            r.dist = c.item.dist;
            r.priceKnown = c.info.priceKnown;
            r.value = c.info.value;
            r.pick = c.decision.pick;
            r.reason = c.decision.reason;
            r.fits = c.fits;
            r.blacklisted = c.blacklisted;
            if (r.pick && !r.blacklisted) ++d->wanted;
            d->rows.push_back(std::move(r));
        }
        std::lock_guard<std::mutex> lock(m_diagMu);
        m_diag = std::move(d);
    }
}

}  // namespace fb
