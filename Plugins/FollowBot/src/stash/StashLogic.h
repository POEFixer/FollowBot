// StashLogic.h - the stash run's pure decisions (design §5.11): when to start,
// which stash object, which tab action, and the per-item deposit confirmation.
// Pure: no SDK, header-only; StashTask (the adapter) feeds it what it read and
// sends the input it decides. Unit-tested in tests/test_stash.cpp.
#pragma once

#include "../common/Geometry.h"
#include "../common/Strings.h"
#include "../world/Interactables.h"
#include "../world/World.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

// ---------------------------------------------------------------------------
// Trigger (same values as StashTask::Trigger)
// ---------------------------------------------------------------------------

enum class StashTrigger : uint8_t { None = 0, Manual, Opportunistic, LeaveMap };

struct TriggerInputs {
    bool enabled = true;
    bool manual = false;              // the "Stash now" hotkey / button
    bool opportunistic = true;        // setting
    bool leaveMapTrip = false;        // setting
    bool inTownOrHideout = false;
    bool inMap = false;
    bool stashKnown = false;          // a stash object of the target domain in this area
    int  usedCells = 0;
    bool inventoryFull = false;       // a wanted item found no room
    int  stashWhenUsedCells = 40;
    int  tripWhenUsedCells = 52;
    bool leaderIdleOrAbsent = true;
    int  hostilesNear = 0;            // within tripNoEnemyRadius
    bool portalKeyBound = false;
    bool coolingDown = false;         // an automatic run failed a moment ago
    bool somethingToStash = true;     // the plan has at least one item to deposit
    bool grewSinceLastRun = true;     // more items than after the last run (or the full flag is new)
    bool tripPending = false;         // a leave-map trip is unfinished: we are in town / hideout away from its map
    bool stashBlocked = false;        // a run left items behind (tab full / missing): automatic runs wait
};

struct TriggerDecision {
    StashTrigger trigger = StashTrigger::None;
    bool         needsTrip = false;   // leave the map through an own portal around the run
    const char*  why = "";            // why nothing starts (or what starts): a key (lang/en.json)
};

inline TriggerDecision DecideStashTrigger(const TriggerInputs& in) {
    TriggerDecision d;
    auto none = [&d](const char* why) {
        d.trigger = StashTrigger::None;
        d.why = why;
        return d;
    };
    if (!in.enabled) return none(Key("stash_why_off"));
    if (in.tripPending && in.inTownOrHideout) {
        d.trigger = StashTrigger::LeaveMap;
        d.needsTrip = true;
        d.why = Key("stash_why_finishing_trip");
        return d;
    }
    if (in.manual) {
        if (in.inTownOrHideout) {
            d.trigger = StashTrigger::Manual;
            d.why = Key("stash_why_now");
            return d;
        }
        if (in.inMap && in.portalKeyBound) {
            d.trigger = StashTrigger::Manual;
            d.needsTrip = true;
            d.why = Key("stash_why_now_portal");
            return d;
        }
        return none(in.inMap ? Key("stash_why_now_no_portal_key") : Key("stash_why_now_wrong_area"));
    }
    // A full or missing tab does not empty itself: an automatic run (a portal
    // trip each time) would only leave the same items behind again.
    if (in.stashBlocked) return none(Key("stash_why_blocked"));
    if (in.coolingDown) return none(Key("stash_why_cooling_down"));
    const bool full = in.inventoryFull;
    if (in.inTownOrHideout) {
        if (!in.opportunistic) return none(Key("stash_why_town_off"));
        if (!in.stashKnown) return none(Key("stash_why_no_stash"));
        if (in.usedCells < in.stashWhenUsedCells && !full) return none(Key("stash_why_below_threshold"));
        if (!in.somethingToStash) return none(Key("stash_why_nothing"));
        if (!in.grewSinceLastRun) return none(Key("stash_why_nothing_new"));
        if (!in.leaderIdleOrAbsent) return none(Key("stash_why_leader_moving"));
        d.trigger = StashTrigger::Opportunistic;
        d.why = Key("stash_why_full_enough");
        return d;
    }
    if (in.inMap) {
        if (!in.leaveMapTrip) return none(Key("stash_why_trip_off"));
        if (!in.portalKeyBound) return none(Key("stash_why_no_portal_key"));
        if (in.usedCells < in.tripWhenUsedCells && !full) return none(Key("stash_why_below_trip_threshold"));
        if (!in.somethingToStash) return none(Key("stash_why_nothing"));
        if (!in.grewSinceLastRun) return none(Key("stash_why_nothing_new"));
        if (in.hostilesNear > 0) return none(Key("stash_why_enemies"));
        d.trigger = StashTrigger::LeaveMap;
        d.needsTrip = true;
        d.why = Key("stash_why_trip");
        return d;
    }
    return none(Key("stash_why_wrong_area"));
}

// ---------------------------------------------------------------------------
// The stash object
// ---------------------------------------------------------------------------

// The stash object of the domain to use: the unskinned object (rank 2) before a
// skin / affinity / pet stash (rank 1), each nearest first; nullptr when none.
inline const Interactable* PickStashObject(const std::vector<Interactable>& list, bool guild, const Vec2& self) {
    const InteractKind want = guild ? InteractKind::GuildStash : InteractKind::Stash;
    const Interactable* best = nullptr;
    int bestRank = -1;
    float bestD = 0.f;
    for (const auto& it : list) {
        if (it.kind != want) continue;
        int rank = it.path.empty() ? 1 : StashPathRank(it.path, guild);
        if (rank <= 0) rank = 1;   // classified as this domain by the host list
        const float d = Dist(it.grid, self);
        if (!best || rank > bestRank || (rank == bestRank && d < bestD)) {
            best = &it;
            bestRank = rank;
            bestD = d;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// The leader stands still
// ---------------------------------------------------------------------------

// "The leader has not moved for N ms": position-based (the speed EMA lags), and
// reset whenever it was not fed for a while (no evidence in between).
class LeaderIdleTracker {
public:
    void Update(bool visible, const Vec2& pos, uint64_t now) {
        const bool gap = m_lastMs == 0 || now < m_lastMs || now - m_lastMs > kGapMs;
        m_lastMs = now;
        if (!visible) {
            m_since = 0;
            m_have = false;
            return;
        }
        if (gap || !m_have || Dist(pos, m_anchor) > kMoveCells) {
            m_anchor = pos;
            m_have = true;
            m_since = now;
        }
    }
    bool IdleFor(uint64_t now, int ms) const {
        return m_since != 0 && now >= m_since && now - m_since >= static_cast<uint64_t>(ms < 0 ? 0 : ms);
    }
    uint64_t Since() const { return m_since; }
    void Reset() { *this = LeaderIdleTracker{}; }

private:
    static constexpr uint64_t kGapMs = 400;
    static constexpr float kMoveCells = 1.5f;
    uint64_t m_lastMs = 0;
    uint64_t m_since = 0;
    Vec2     m_anchor;
    bool     m_have = false;
};

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

struct TabInfo {
    int         index = -1;
    int         active = -1;          // 1 selected, 0 not, -1 unknown
    bool        onScreen = false;
    float       x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    std::string name;

    Vec2 Center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    bool Clickable() const { return onScreen && w > 1.f && h > 1.f; }
};

// Index of the tab named `wanted` (trimmed, case-insensitive), -1 when none.
inline int FindTab(const std::vector<TabInfo>& tabs, std::string_view wanted) {
    const std::string key = FoldCase(Trim(wanted));
    if (key.empty()) return -1;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (FoldCase(Trim(tabs[i].name)) == key) return static_cast<int>(i);
    return -1;
}

enum class TabAction : uint8_t {
    UseOpen = 0,     // no tab wanted: deposit into whatever is open
    AlreadyActive,   // the wanted tab is selected
    Click,           // click its rect
    OpenList,        // its list entry is hidden: click the list toggle first
    NotFound,        // no tab of that name
    NotVisible       // found, but not on screen and no toggle to open its list
};

inline const char* TabActionName(TabAction a) {
    switch (a) {
        case TabAction::UseOpen:       return "use the open tab";
        case TabAction::AlreadyActive: return "already selected";
        case TabAction::Click:         return "click it";
        case TabAction::OpenList:      return "open the tab list";
        case TabAction::NotFound:      return "not found";
        case TabAction::NotVisible:    return "not visible";
    }
    return "?";
}

inline TabAction DecideTab(const std::vector<TabInfo>& tabs, bool toggleClickable, std::string_view wanted,
                           int& outIndex) {
    outIndex = -1;
    if (Trim(wanted).empty()) return TabAction::UseOpen;
    const int i = FindTab(tabs, wanted);
    if (i < 0) return TabAction::NotFound;
    outIndex = i;
    const TabInfo& t = tabs[static_cast<size_t>(i)];
    if (t.active == 1) return TabAction::AlreadyActive;
    if (t.Clickable()) return TabAction::Click;
    if (toggleClickable) return TabAction::OpenList;
    return TabAction::NotVisible;
}

enum class SwitchCheck : uint8_t { Switched = 0, NotYet, Unknown };

// After a tab click: did the wanted tab become the selected one? The host's
// IsActive decides when it knows it; otherwise (own stash only) a change of the
// open storage inventory id is taken as the switch; else Unknown.
inline SwitchCheck CheckTabSwitched(const std::vector<TabInfo>& tabs, std::string_view wanted, int openIdBefore,
                                    int openIdNow, bool guild) {
    const int i = FindTab(tabs, wanted);
    if (i >= 0) {
        const int a = tabs[static_cast<size_t>(i)].active;
        if (a == 1) return SwitchCheck::Switched;
        if (a == 0) return SwitchCheck::NotYet;
    }
    if (!guild && openIdBefore != 0 && openIdNow != 0 && openIdNow != openIdBefore) return SwitchCheck::Switched;
    return SwitchCheck::Unknown;
}

// ---------------------------------------------------------------------------
// Deposit: per-item confirmation (port of the host's StashTransfer::Tracker
// rule, time injected)
// ---------------------------------------------------------------------------
//
// A dispatched Ctrl+click puts the item IN FLIGHT for confirmWindowMs; an
// in-flight item is never re-clicked and never given up on, the queue moves on
// to the next item meanwhile (the server queues moves; a guild stash confirms
// only after a round trip). Only the item VANISHING from a valid backpack
// listing scanned AFTER the click confirms the move. A window that ended with
// the item still present (seen in a listing scanned after the window) is one
// failed attempt; maxConfirmWindows of them = skipped. tabFullSkips skips in a
// row (no confirmed move between them) = the tab is full. A hold needs no
// special care: windows that ended during it are judged by the first listing
// scanned after it.

struct DepositParams {
    int clickDelayMs = 70;
    int confirmWindowMs = 1500;
    int maxConfirmWindows = 3;
    int tabFullSkips = 3;
};

class DepositQueue {
public:
    enum class State : uint8_t { Running = 0, Done, TabFull };
    enum class ItemState : uint8_t { Pending = 0, InFlight, Moved, Skipped, Gone };

    struct Entry {
        uintptr_t item = 0;
        ItemState state = ItemState::Pending;
        int       attempts = 0;
        uint64_t  clickMs = 0;
        uint64_t  deadline = 0;
        bool      present = true;     // in the latest listing
    };

    // `items` in click order (largest first); duplicates and 0 are ignored.
    void Begin(const std::vector<uintptr_t>& items, const DepositParams& p, uint64_t now) {
        m_entries.clear();
        m_entries.reserve(items.size());
        for (uintptr_t it : items) {
            if (it == 0) continue;
            bool dup = false;
            for (const auto& e : m_entries)
                if (e.item == it) { dup = true; break; }
            if (dup) continue;
            Entry e;
            e.item = it;
            m_entries.push_back(e);
        }
        m_p = p;
        m_p.confirmWindowMs = std::max(1, m_p.confirmWindowMs);
        m_p.maxConfirmWindows = std::max(1, m_p.maxConfirmWindows);
        m_p.tabFullSkips = std::max(1, m_p.tabFullSkips);
        m_nextClickMs = now;
        m_lastStamp = 0;
        m_consecutiveSkips = 0;
        m_tabFull = false;
    }

    // A valid backpack listing whose scan was requested at `scanStampMs`.
    void OnListing(const std::vector<uintptr_t>& present, uint64_t scanStampMs) {
        if (scanStampMs > m_lastStamp) m_lastStamp = scanStampMs;
        for (auto& e : m_entries) {
            e.present = std::find(present.begin(), present.end(), e.item) != present.end();
            if (e.present) continue;
            const bool clicked = e.state == ItemState::InFlight || (e.state == ItemState::Pending && e.attempts > 0);
            if (clicked && scanStampMs > e.clickMs) {
                e.state = ItemState::Moved;   // a late confirmation of an expired window counts too
                m_consecutiveSkips = 0;
            } else if (e.state == ItemState::Pending && e.attempts == 0) {
                e.state = ItemState::Gone;    // removed by something else (you, the game)
            }
        }
    }

    // Expired windows and completion. Call every tick.
    State Update(uint64_t now) {
        for (auto& e : m_entries) {
            if (e.state != ItemState::InFlight || now < e.deadline) continue;
            if (m_lastStamp <= e.deadline || !e.present) continue;   // no evidence after the window yet
            if (e.attempts >= m_p.maxConfirmWindows) {
                e.state = ItemState::Skipped;
                ++m_consecutiveSkips;
            } else {
                e.state = ItemState::Pending;   // still here: click it again
            }
        }
        if (!m_tabFull && m_consecutiveSkips >= m_p.tabFullSkips) {
            m_tabFull = true;
            for (auto& e : m_entries)
                if (e.state == ItemState::Pending) e.state = ItemState::Skipped;
        }
        for (const auto& e : m_entries)
            if (e.state == ItemState::InFlight || e.state == ItemState::Pending) return State::Running;
        return m_tabFull ? State::TabFull : State::Done;
    }

    // The item to click now; 0 = none (the click delay runs, everything is in
    // flight, or the group is finished).
    uintptr_t Next(uint64_t now) const {
        if (m_tabFull || now < m_nextClickMs) return 0;
        for (const auto& e : m_entries)
            if (e.state == ItemState::Pending && e.present) return e.item;
        return 0;
    }

    void OnClicked(uintptr_t item, uint64_t now) {
        for (auto& e : m_entries) {
            if (e.item != item) continue;
            e.state = ItemState::InFlight;
            ++e.attempts;
            e.clickMs = now;
            e.deadline = now + static_cast<uint64_t>(m_p.confirmWindowMs);
            break;
        }
        m_nextClickMs = now + static_cast<uint64_t>(std::max(0, m_p.clickDelayMs));
    }

    int Count(ItemState s) const {
        int n = 0;
        for (const auto& e : m_entries)
            if (e.state == s) ++n;
        return n;
    }
    int  Moved() const { return Count(ItemState::Moved); }
    int  Skipped() const { return Count(ItemState::Skipped); }
    int  Remaining() const { return Count(ItemState::Pending) + Count(ItemState::InFlight); }
    int  Total() const { return static_cast<int>(m_entries.size()); }
    bool TabFull() const { return m_tabFull; }
    int  ConsecutiveSkips() const { return m_consecutiveSkips; }
    bool AnyInFlight() const { return Count(ItemState::InFlight) > 0; }
    const std::vector<Entry>& Entries() const { return m_entries; }

private:
    std::vector<Entry> m_entries;
    DepositParams      m_p;
    uint64_t           m_nextClickMs = 0;
    uint64_t           m_lastStamp = 0;
    int                m_consecutiveSkips = 0;
    bool               m_tabFull = false;
};

}  // namespace fb
