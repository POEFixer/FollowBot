// StashDiag.h - what the Stash tab's Diagnostics card shows: the host's stash
// objects and scan request (written by the WorldBuilder), the last enumerated
// tabs, the current plan, the run's phase and its last failure (written by
// StashTask). Worker writes, render thread reads; one mutex, copies only on a
// change (`seq`), never per frame. Pure: no SDK.
#pragma once

#include "../common/Geometry.h"
#include "../world/World.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fb {

struct StashObjectDiag {
    uint32_t    id = 0;
    bool        guild = false;
    int         rank = 0;
    Vec2        grid;
    std::string path;
};

struct StashTabDiag {
    int         index = -1;
    std::string name;
    int         active = -1;
    bool        onScreen = false;
};

struct StashPlanGroupDiag {
    std::string tab;                  // "" = the open tab
    int         items = 0;
};

// Host stash-object list status (EnumerateStashObjects).
enum : int {
    kStashObjUnsupported = -3,        // host older than 365
    kStashObjNotRequested = -2,       // the scan flag is off (stopped, or not in town / hideout)
    kStashObjNotReady = -1,           // requested; the host has not built the list yet
};

struct StashDiagData {
    uint64_t seq = 0;
    // Host stash objects (WorldBuilder).
    int      objectsStatus = kStashObjUnsupported;   // >= 0: number of objects
    uint32_t scanFlags = 0;                           // what this plugin requests right now
    std::vector<StashObjectDiag> objects;
    // Backpack occupancy of the latest listing (the protected-cells editor overlay).
    bool     backpackValid = false;
    std::array<uint8_t, kBackpackCells> occupied{};
    // Tabs (StashTask), from the last enumeration.
    int      tabsStatus = -3;         // -3 never read, -2 unsupported, -1 panel not open, >= 0 count
    bool     tabsGuild = false;
    bool     toggleOnScreen = false;
    uint64_t tabsMs = 0;
    std::vector<StashTabDiag> tabs;
    // The current / last plan.
    std::vector<StashPlanGroupDiag> plan;
    int      planKept = 0, planProtected = 0;
    uint64_t planMs = 0;
    // The run (keys and LocTexts: the render thread translates them).
    const char* trigger = "";         // the trigger's key; "" when no run
    const char* phase = "";           // the phase's key
    const char* whyNot = "";          // why no automatic run starts now (a key)
    int         moved = 0, skipped = 0;
    LocText     lastResult;
    LocText     lastFailure;
    uint64_t    lastFailureMs = 0;
    std::deque<LocText> warnings;     // the last few, newest last
};

class StashDiag {
public:
    template <typename F>
    void Update(F&& f) {
        std::lock_guard<std::mutex> lock(m_mu);
        f(m_data);
        m_data.seq = ++m_seq;
        m_seqAtomic.store(m_seq, std::memory_order_release);
    }
    // Copies when `seq` differs from the caller's; true when `out` changed.
    bool CopyIfNewer(StashDiagData& out) const {
        if (m_seqAtomic.load(std::memory_order_acquire) == out.seq && out.seq != 0) return false;
        std::lock_guard<std::mutex> lock(m_mu);
        out = m_data;
        return true;
    }
    uint64_t Seq() const { return m_seqAtomic.load(std::memory_order_acquire); }

    static void AddWarning(StashDiagData& d, LocText w) {
        if (!d.warnings.empty() && d.warnings.back() == w) return;
        d.warnings.push_back(std::move(w));
        while (d.warnings.size() > 8) d.warnings.pop_front();
    }

private:
    mutable std::mutex    m_mu;
    StashDiagData         m_data;
    uint64_t              m_seq = 0;
    std::atomic<uint64_t> m_seqAtomic{0};
};

// The plugin's one instance (one plugin object per DLL).
inline StashDiag& GlobalStashDiag() {
    static StashDiag d;
    return d;
}

}  // namespace fb
