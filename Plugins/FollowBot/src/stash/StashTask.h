// StashTask.h - empty the backpack into the own or the guild stash
// (design §5.11). Exclusive task, started by the Brain at step 5.
//
// Contract (this interface is final):
//  - WantsToStart: stash enabled and one of
//      Manual        mem.manualStashRequested (hotkey) - consumed by Start;
//      Opportunistic in town / hideout, a stash of the target domain known
//                    (w.interactables Stash / GuildStash, host stash objects
//                    when available), used cells >= stashWhenUsedCells or
//                    mem.inventoryFull, the leader idle for leaderIdleMs (or absent);
//      LeaveMap      leaveMapTrip on, in a map, used cells >= tripWhenUsedCells
//                    or mem.inventoryFull, no hostile within tripNoEnemyRadius,
//                    portalKey bound (runs a PortalTrip around the stash run).
//  - Tick: walk to a ring approachRingCells around the stash object,
//    WorldClick, wait <= openTimeoutMs for the panel (own: a non-player
//    storage grid; guild: inventory 10002), openRetries tries; tab routing
//    (host stash tabs when available, else the open tab with one warning);
//    deposit per StashPlanner with Ctrl held per group, click delays
//    ownClickDelayMs / guildClickDelayMs, an item counts as moved only when it
//    vanished from a fresh backpack listing (confirmWindowMs, maxConfirmWindows,
//    tabFullSkips); the cursor item (inventory 13) empty before and after;
//    close with Esc (Esc again if the game menu opened; verify, 3 tries).
//    Sets act.SetUiTaskActive(true) while the panel is open (no chat input) and
//    req.inventoryFast throughout; the Gates treat the panel as the bot's own.
//  - AllowsCasts: Flask only while walking, none while the panel is open.
//
// Details (implementation):
//  - A refused manual request (not in town / hideout and no portal key) is
//    consumed with a log line. Automatic triggers also need something to stash
//    (the plan is not empty), more items than right after the last run (or a
//    new full flag), and no failure cooldown (failCooldownMs; three failures in
//    one area stop automatic runs there until the area changes).
//  - A run that left items behind (a tab looked full, a routed tab unusable)
//    pauses the automatic triggers (mem.stashBlocked) until a manual run moves
//    items with nothing left behind, the backpack shrinks below its size after
//    that run, or Start.
//  - Manual in a map with a portal key set runs the portal trip too.
//  - An unfinished leave-map trip (the task was interrupted in town / hideout)
//    resumes: stash, then back through the portal; a trip whose map we are
//    back in is forgotten.
//  - Opportunistic runs yield (before the panel opens) when the leader leaves
//    through something the bot follows.
//  - An item on the cursor before or after the deposit stops the bot with the
//    reason and leaves the panel open (a click could drop the item).
//  - Failures close the panel first, and during a trip go back through the
//    portal before reporting; a failed way back waits "stranded" (re-trying
//    the portal every 5 s, 3 times) until the area changes or the leader is here.
//  - KeepThroughHold: loading, panel check, user activity, override, game menu,
//    another window in front, the POEFixer menu (the run resumes and
//    re-validates; input is released meanwhile); NotInGame only during a trip.
#pragma once

#include "../brain/Task.h"
#include "../travel/WorldClick.h"
#include "PortalTrip.h"
#include "StashLogic.h"
#include "StashPlanner.h"

#include <string>
#include <vector>

namespace fb {

class StashTask : public Task {
public:
    enum class Trigger : uint8_t { None = 0, Manual, Opportunistic, LeaveMap };

    const char* Name() const override { return Key("task_stash"); }

    bool WantsToStart(BotContext& ctx);
    void Start(BotContext& ctx);
    TaskStatus Tick(BotContext& ctx) override;
    void Abort(BotContext& ctx) override;
    bool AllowsCasts(CastClass c) const override;
    bool KeepThroughHold(GateReason r) const override;

    Trigger PendingTrigger() const { return m_pending; }
    Trigger ActiveTrigger() const { return m_active; }
    // True while the stash panel is (or is being) opened by this task: the
    // Gates do not count it as a foreign panel.
    bool OwnsPanel() const { return m_ownsPanel; }
    const LocText& FailReason() const { return m_fail; }
    static const char* TriggerName(Trigger t);   // the trigger's key (lang/en.json)

    // What the run does right now: a key ("" when idle).
    const char* PhaseText() const;

private:
    enum class Phase : uint8_t {
        Idle = 0,
        TripLeave,    // leaving the map through an own portal
        Locate,       // finding the stash object
        Approach,     // walking to it and clicking it
        WaitOpen,     // waiting for the panel
        Prepare,      // cursor check + the plan
        SelectTab,    // switching to the group's tab
        Deposit,      // Ctrl+clicking the group's items
        Closing,      // Esc until the panel is gone
        TripReturn,   // back through the portal
        Stranded      // the way back failed: waiting in town / hideout
    };

    void SetPhase(BotContext& ctx, Phase p);
    TaskStatus TickLocate(BotContext& ctx);
    TaskStatus TickApproach(BotContext& ctx);
    TaskStatus TickWaitOpen(BotContext& ctx);
    TaskStatus TickPrepare(BotContext& ctx);
    TaskStatus TickSelectTab(BotContext& ctx);
    TaskStatus TickDeposit(BotContext& ctx);
    TaskStatus TickClosing(BotContext& ctx);
    TaskStatus TickTripReturn(BotContext& ctx);
    TaskStatus TickStranded(BotContext& ctx);

    // Failure: close the panel / go back through the portal first, then report.
    TaskStatus FailRun(BotContext& ctx, const LocText& why);
    // An item on the cursor: stop the bot, leave everything as it is. `when`: a key.
    TaskStatus StopForCursor(BotContext& ctx, const char* when);
    // `countAsRun` false: the run gave way (no cooldown, no baseline).
    TaskStatus Finish(BotContext& ctx, TaskStatus st, bool countAsRun = true);
    TaskStatus AfterPanelClosed(BotContext& ctx);
    void StartStashClick(BotContext& ctx, const Interactable& stash);
    void BeginGroup(BotContext& ctx);
    void EndGroup(BotContext& ctx, bool tabFull);
    void FallbackToOpenTab(BotContext& ctx, const LocText& why);
    bool ReadTabs(BotContext& ctx);
    bool PanelOpen(const World& w) const;
    bool PanelGone(const World& w) const;
    void ReleaseCtrl(BotContext& ctx);
    void Warn(BotContext& ctx, const LocText& text);
    int  RarityOf(BotContext& ctx, const BagItem& it);
    bool SomethingToStash(BotContext& ctx);
    void BuildPlan(BotContext& ctx, const std::vector<BagItem>& items, DepositPlan& out);
    void PublishPlan(const DepositPlan& plan, uint64_t now);
    void UpdateDetail();

    Trigger     m_pending = Trigger::None;
    Trigger     m_active = Trigger::None;
    bool        m_ownsPanel = false;
    WorldClick  m_click;
    PortalTrip  m_trip;
    DepositPlan m_plan;
    LocText     m_fail;

    // --- additions -----------------------------------------------------------
    Phase       m_phase = Phase::Idle;
    bool        m_pendingTrip = false;    // the pending decision runs a portal trip
    bool        m_tripRun = false;        // this run goes through a portal
    bool        m_guild = false;
    uint64_t    m_runStartMs = 0;
    uint64_t    m_phaseMs = 0;
    uint64_t    m_lastTickMs = 0;
    int         m_attempt = 0;            // stash clicks (open attempts) this run
    uint32_t    m_stashId = 0;
    uint64_t    m_clickMs = 0;            // the last stash click
    uint64_t    m_openMs = 0;             // the panel was seen open (listing scan time)
    uint64_t    m_escMs = 0;
    int         m_escTries = 0;
    LocText     m_pendingFail;            // reported after closing / going back
    bool        m_stopAfter = false;      // stop the bot when the run ends
    // Plan + deposit.
    std::vector<BagItem> m_planItems;
    size_t      m_group = 0;
    DepositQueue m_queue;
    bool        m_groupActive = false;
    uint64_t    m_lastListingStamp = 0;
    uint64_t    m_lastListingSeenMs = 0;      // when the deposit last got a new listing
    std::vector<uintptr_t> m_present;     // scratch
    uint64_t    m_ctrlMs = 0;
    uint64_t    m_lastDepositClickMs = 0;
    bool        m_cursorCheckPending = false;  // the cursor is checked after the deposit
    bool        m_cursorPlaced = false;
    uintptr_t   m_cursorItem = 0;
    Vec2        m_cursorAt;
    uint64_t    m_cursorMs = 0;
    int         m_moved = 0, m_skipped = 0;
    bool        m_runLeftItems = false;   // a group stayed behind (tab full, or a routed tab unusable)
    // Tabs.
    std::vector<TabInfo> m_tabs;
    TabInfo     m_toggle;
    int         m_tabsStatus = -3;
    int         m_tabStep = 0;
    int         m_tabTries = 0;
    int         m_tabIndex = -1;
    int         m_openIdBefore = 0;
    uint64_t    m_tabClickMs = 0;
    LocText     m_groupTabLabel;          // what the detail line calls the group's tab
    std::vector<LocText> m_warned;        // warnings already given this run
    // Stranded.
    uint64_t    m_strandedMs = 0;
    uint32_t    m_strandedArea = 0;
    int         m_strandRetries = 0;
    // Trigger state.
    uint64_t    m_retryAfterMs = 0;
    uint32_t    m_failArea = 0;
    int         m_failsInArea = 0;
    int         m_baselineUsed = -1;      // used cells right after the last run
    uint64_t    m_yieldedVanishMs = 0;
    LeaderIdleTracker m_leaderIdle;
    uint64_t    m_trigStamp = 0;
    bool        m_trigSomething = false;
    const char* m_lastWhy = nullptr;
    struct RarityEntry { uintptr_t addr; std::string path; int rarity; };
    std::vector<RarityEntry> m_rarity;
    LocText     m_detail;
};

}  // namespace fb
