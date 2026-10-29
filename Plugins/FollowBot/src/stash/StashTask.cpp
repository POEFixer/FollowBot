// StashTask.cpp - see StashTask.h.
// FB_TESTS_EXCLUDE: adapter (stash tabs, item rarity and the game state through BotContext::sdk); not built into tests/.
// The decisions it applies are pure and tested: StashLogic.h, StashPlanner, PortalTrip.
#include "StashTask.h"

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../loot/LootCategories.h"
#include "../move/Mover.h"
#include "StashDiag.h"

#include <algorithm>
#include <cstdio>

namespace fb {

static_assert(static_cast<int>(StashTask::Trigger::None) == static_cast<int>(StashTrigger::None) &&
                  static_cast<int>(StashTask::Trigger::Manual) == static_cast<int>(StashTrigger::Manual) &&
                  static_cast<int>(StashTask::Trigger::Opportunistic) == static_cast<int>(StashTrigger::Opportunistic) &&
                  static_cast<int>(StashTask::Trigger::LeaveMap) == static_cast<int>(StashTrigger::LeaveMap),
              "StashTask::Trigger and StashTrigger must keep the same values");

namespace {

constexpr uint64_t kRunWatchdogMs = 240000;    // one run, portal trips excluded
constexpr uint64_t kLocateTimeoutMs = 5000;    // the host builds its stash list a tick after the request
constexpr uint64_t kGridWaitMs = 2000;         // for a readable backpack grid after the panel opened
constexpr uint64_t kCtrlSettleMs = 50;         // Ctrl down -> first click
constexpr uint64_t kCursorSettleMs = 40;       // cursor on the item -> click
constexpr int      kTabTries = 3;
constexpr uint64_t kCloseVerifyMs = 1500;      // wait for a listing scanned after an Esc
constexpr int      kCloseTries = 3;
constexpr int      kEscMenuCheckMs = 200;      // after an Esc: did the game menu open instead?
constexpr uint64_t kHoldGapMs = 400;           // a longer gap between ticks = a hold
constexpr uint64_t kListingStallMs = 5000;     // no new backpack listing this long during a deposit = fail
constexpr uint64_t kStrandRetryMs = 5000;
constexpr int      kStrandRetries = 3;
constexpr int      kFailsPerArea = 3;
constexpr int      kApproachTimeoutMs = 25000;
constexpr uint32_t kStashMarkerColor = 0xFF40C0FFu;   // ImGui ABGR (amber)

// The stash kind for the detail and log lines (keys).
const char* DomainName(bool guild) { return guild ? Key("stash_domain_guild") : Key("stash_domain_own"); }

WorldClickTarget StashClickTarget(const Interactable& it) {
    WorldClickTarget t;
    t.id = it.id;
    t.grid = it.grid;
    t.world = it.world;
    t.modelHeight = 0.f;   // clicked at its position, like the host's own stash walk
    t.targetableAddr = it.targetableAddr;
    return t;
}

}  // namespace

const char* StashTask::TriggerName(Trigger t) {
    switch (t) {
        case Trigger::None:          return Key("stash_trigger_none");
        case Trigger::Manual:        return Key("stash_trigger_manual");
        case Trigger::Opportunistic: return Key("stash_trigger_town");
        case Trigger::LeaveMap:      return Key("stash_trigger_leave_map");
    }
    return Key("common_unknown");
}

const char* StashTask::PhaseText() const {
    switch (m_phase) {
        case Phase::Idle:       return "";
        case Phase::TripLeave:  return Key("stash_phase_trip_leave");
        case Phase::Locate:     return Key("stash_phase_locate");
        case Phase::Approach:   return Key("stash_phase_approach");
        case Phase::WaitOpen:   return Key("stash_phase_wait_open");
        case Phase::Prepare:    return Key("stash_phase_prepare");
        case Phase::SelectTab:  return Key("stash_phase_select_tab");
        case Phase::Deposit:    return Key("stash_phase_deposit");
        case Phase::Closing:    return Key("stash_phase_closing");
        case Phase::TripReturn: return Key("stash_phase_trip_return");
        case Phase::Stranded:   return Key("stash_phase_stranded");
    }
    return "";
}

bool StashTask::AllowsCasts(CastClass c) const {
    return c == CastClass::Flask && !m_ownsPanel && !m_click.CursorCritical() && !m_trip.CursorCritical();
}

bool StashTask::KeepThroughHold(GateReason r) const {
    switch (r) {
        case GateReason::Loading:
        case GateReason::PanelUnknown:
        case GateReason::UserActivity:
        case GateReason::OverrideHeld:
        case GateReason::EscapeMenu:
        case GateReason::NotForeground:
        case GateReason::MenuVisible:
            return true;
        case GateReason::NotInGame:
            return m_phase == Phase::TripLeave || m_phase == Phase::TripReturn;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void StashTask::SetPhase(BotContext& ctx, Phase p) {
    m_phase = p;
    m_phaseMs = ctx.now;
    switch (p) {
        case Phase::Locate:
            m_runStartMs = ctx.now;   // the watchdog counts the stash part only
            ctx.req.interactablesNow = true;
            break;
        case Phase::SelectTab:
            m_tabStep = 0;
            m_tabTries = 0;
            break;
        case Phase::Closing:
            m_escMs = 0;
            m_escTries = 0;
            // After any deposit click (success or failure) the cursor must be empty.
            m_cursorCheckPending = m_lastDepositClickMs != 0;
            break;
        default:
            break;
    }
    const char* text = PhaseText();
    GlobalStashDiag().Update([&](StashDiagData& d) { d.phase = text; });
}

void StashTask::ReleaseCtrl(BotContext& ctx) {
    if (ctx.act.IsHeld(VK_CONTROL)) ctx.act.KeyUp(VK_CONTROL);
}

bool StashTask::PanelOpen(const World& w) const {
    const InventoryState& inv = w.inv;
    if (!inv.backpackOpen) return false;
    return m_guild ? inv.guildOpen : (inv.stashOpen && !inv.guildOpen);
}

// Once open, the panel counts as gone only when the backpack grid is gone too:
// switching own-stash tabs replaces the storage grid for a moment, the
// backpack stays.
bool StashTask::PanelGone(const World& w) const {
    const InventoryState& inv = w.inv;
    return inv.valid && !inv.backpackOpen && !inv.stashOpen && !inv.guildOpen;
}

void StashTask::Warn(BotContext& ctx, const LocText& text) {
    if (std::find(m_warned.begin(), m_warned.end(), text) != m_warned.end()) return;
    m_warned.push_back(text);
    ctx.log.Add(LogCat::Stash, TrF("log_stash_warning", {{"text", text}}));
    GlobalStashDiag().Update([&](StashDiagData& d) { StashDiag::AddWarning(d, text); });
}

int StashTask::RarityOf(BotContext& ctx, const BagItem& it) {
    if (it.rarity >= 0) return it.rarity;
    for (const auto& e : m_rarity)
        if (e.addr == it.address && e.path == it.path) return e.rarity;
    int r = -1;
    if (ctx.sdk && it.address) r = std::clamp(ctx.sdk->Inventory.ReadItemRarity(it.address), 0, 3);
    if (m_rarity.size() >= 160) m_rarity.clear();   // bounded; addresses are reused by new items
    m_rarity.push_back({it.address, it.path, r});
    return r;
}

void StashTask::BuildPlan(BotContext& ctx, const std::vector<BagItem>& items, DepositPlan& out) {
    std::vector<ItemCategory> cats;
    cats.reserve(items.size());
    for (const auto& it : items) cats.push_back(ClassifyItem(it.path, RarityOf(ctx, it)));
    out = PlanDeposit(items, cats, ctx.p.stash);
}

void StashTask::PublishPlan(const DepositPlan& plan, uint64_t now) {
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.plan.clear();
        for (const auto& g : plan.groups) d.plan.push_back({g.tab, static_cast<int>(g.items.size())});
        d.planKept = plan.kept;
        d.planProtected = plan.protectedSkipped;
        d.planMs = now;
    });
}

bool StashTask::SomethingToStash(BotContext& ctx) {
    const InventoryState& inv = ctx.w.inv;
    if (!inv.valid) return false;
    if (m_trigStamp != 0 && inv.stamp == m_trigStamp) return m_trigSomething;
    DepositPlan p;
    BuildPlan(ctx, inv.items, p);
    m_trigStamp = inv.stamp;
    m_trigSomething = PlanItemCount(p) > 0;
    PublishPlan(p, ctx.now);
    return m_trigSomething;
}

bool StashTask::ReadTabs(BotContext& ctx) {
    m_tabs.clear();
    m_toggle = TabInfo{};
    if (!ctx.sdk) {
        m_tabsStatus = -2;
        return false;
    }
    const PluginSDK::StashTabList list =
        ctx.sdk->Game.EnumerateStashTabs(m_guild ? PluginSDK::StashDomain::Guild : PluginSDK::StashDomain::Own);
    m_tabsStatus = list.Supported ? list.Status : -2;
    for (const auto& t : list.Tabs) {
        TabInfo ti;
        ti.index = t.Index;
        ti.active = t.IsActive;
        ti.onScreen = t.OnScreen;
        ti.x = t.X;
        ti.y = t.Y;
        ti.w = t.W;
        ti.h = t.H;
        ti.name = t.Name;
        m_tabs.push_back(std::move(ti));
    }
    m_toggle.onScreen = list.Toggle.OnScreen;
    m_toggle.x = list.Toggle.X;
    m_toggle.y = list.Toggle.Y;
    m_toggle.w = list.Toggle.W;
    m_toggle.h = list.Toggle.H;
    const uint64_t now = ctx.now;
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.tabsStatus = m_tabsStatus;
        d.tabsGuild = m_guild;
        d.toggleOnScreen = m_toggle.Clickable();
        d.tabsMs = now;
        d.tabs.clear();
        for (const auto& t : m_tabs) d.tabs.push_back({t.index, t.name, t.active, t.Clickable()});
    });
    return list.Supported;
}

void StashTask::UpdateDetail() {
    const LocText trip = Tr(m_trip.PhaseText());
    const LocText domain = Tr(DomainName(m_guild));
    switch (m_phase) {
        case Phase::Idle:
            m_detail.clear();
            return;
        case Phase::TripLeave:
            m_detail = TrF("stash_detail_trip_leave", {{"step", trip}});
            return;
        case Phase::Approach:
            m_detail = TrF(m_click.CurrentPhase() == WorldClick::Phase::Approach ? Key("stash_detail_walking")
                                                                                 : Key("stash_detail_clicking"),
                           {{"stash", domain}, {"try", Num("%d", m_attempt + 1)}});
            return;
        case Phase::WaitOpen:
            m_detail = TrF("stash_detail_wait_open", {{"stash", domain}, {"try", Num("%d", m_attempt)}});
            return;
        case Phase::SelectTab:
            m_detail = TrF("stash_detail_select_tab",
                           {{"tab", m_group < m_plan.groups.size() ? m_plan.groups[m_group].tab : std::string()}});
            return;
        case Phase::Deposit:
            m_detail = TrF("stash_detail_deposit", {{"tab", m_groupTabLabel},
                                                    {"moved", Num("%d", m_queue.Moved())},
                                                    {"total", Num("%d", m_queue.Total())},
                                                    {"group", Num("%d", static_cast<int>(m_group) + 1)},
                                                    {"groups", Num("%d", static_cast<int>(m_plan.groups.size()))}});
            return;
        case Phase::TripReturn:
            m_detail = TrF("stash_detail_trip_return", {{"step", trip}});
            return;
        case Phase::Stranded:
            m_detail = TrF("stash_detail_stranded", {{"why", m_trip.FailReason()}});
            return;
        default:
            m_detail = TrF("stash_detail_phase", {{"phase", Tr(PhaseText())}});
            return;
    }
}

// ---------------------------------------------------------------------------
// Trigger
// ---------------------------------------------------------------------------

bool StashTask::WantsToStart(BotContext& ctx) {
    const World& w = ctx.w;
    const StashSettings& s = ctx.p.stash;
    m_pending = Trigger::None;
    m_pendingTrip = false;

    m_leaderIdle.Update(w.leader.visible, w.leader.info.grid, ctx.now);
    ctx.mem.leaderIdleSinceMs = m_leaderIdle.Since();

    const bool town = w.gate.isTown || w.gate.isHideout;
    BotMemory::StashTrip& trip = ctx.mem.trip;
    if (trip.active && !town && !w.gate.areaId.empty() && w.gate.areaId == trip.originAreaId) {
        ctx.log.Add(LogCat::Stash, TrF("log_stash_back_in", {{"area", w.gate.areaId}}));
        trip = BotMemory::StashTrip{};
    }
    const bool tripPending = trip.active && town && w.gate.areaCounter != trip.originArea;
    if (w.inv.valid && m_baselineUsed >= 0 && w.inv.usedCells < m_baselineUsed) m_baselineUsed = w.inv.usedCells;
    if (m_failsInArea > 0 && m_failArea != w.gate.areaCounter) m_failsInArea = 0;
    if (ctx.mem.stashBlocked && w.inv.valid && w.inv.usedCells < ctx.mem.stashBlockedUsedCells) {
        ctx.mem.stashBlocked = false;
        ctx.log.Add(LogCat::Stash, Tr("log_stash_room_again"));
    }

    const bool manual = ctx.mem.manualStashRequested;
    TriggerInputs in;
    in.enabled = s.enabled;
    in.manual = manual;
    in.opportunistic = s.opportunistic;
    in.leaveMapTrip = s.leaveMapTrip;
    in.inTownOrHideout = town;
    in.inMap = w.gate.isMap;
    in.stashKnown = PickStashObject(w.interactables, s.target == StashTarget::Guild, w.self.grid) != nullptr;
    in.usedCells = w.inv.valid ? w.inv.usedCells : 0;
    in.inventoryFull = ctx.mem.inventoryFull || w.inv.fullFlag;
    in.stashWhenUsedCells = s.stashWhenUsedCells;
    in.tripWhenUsedCells = s.tripWhenUsedCells;
    in.leaderIdleOrAbsent = !w.leader.visible || m_leaderIdle.IdleFor(ctx.now, s.leaderIdleMs);
    in.hostilesNear = s.tripNoEnemyRadius > 0.f ? w.CountMonstersNear(w.self.grid, s.tripNoEnemyRadius) : 0;
    in.portalKeyBound = s.portalKey != 0;
    in.coolingDown = ctx.now < m_retryAfterMs || m_failsInArea >= kFailsPerArea;
    in.grewSinceLastRun = m_baselineUsed < 0 || in.usedCells > m_baselineUsed || in.inventoryFull;
    in.tripPending = tripPending;
    in.stashBlocked = ctx.mem.stashBlocked;
    in.somethingToStash = true;   // classified below, only when everything else passes
    TriggerDecision d = DecideStashTrigger(in);
    if (d.trigger != StashTrigger::None && !manual && !tripPending) {
        in.somethingToStash = SomethingToStash(ctx);
        d = DecideStashTrigger(in);
    }
    if (manual && d.trigger == StashTrigger::None) {
        ctx.mem.manualStashRequested = false;   // refused: consumed, never left pending
        ctx.log.Add(LogCat::Stash, TrF("log_stash_now_refused", {{"why", Tr(d.why)}}));
    }
    if (d.why != m_lastWhy) {
        m_lastWhy = d.why;
        const char* why = d.trigger == StashTrigger::None ? d.why : "";
        GlobalStashDiag().Update([&](StashDiagData& dd) { dd.whyNot = why; });
    }
    m_pending = static_cast<Trigger>(d.trigger);
    m_pendingTrip = d.needsTrip;
    return m_pending != Trigger::None;
}

void StashTask::Start(BotContext& ctx) {
    const World& w = ctx.w;
    const StashSettings& s = ctx.p.stash;
    m_active = m_pending == Trigger::None ? Trigger::Manual : m_pending;
    ctx.mem.manualStashRequested = false;   // consumed (a press meanwhile is served by this run)
    m_fail.clear();
    m_pendingFail.clear();
    m_stopAfter = false;
    m_guild = s.target == StashTarget::Guild;
    m_runStartMs = m_phaseMs = m_lastTickMs = ctx.now;
    m_attempt = 0;
    m_ownsPanel = false;
    m_moved = m_skipped = 0;
    m_runLeftItems = false;
    m_warned.clear();
    m_plan = DepositPlan{};
    m_group = 0;
    m_groupActive = false;
    m_cursorPlaced = false;
    m_stashId = 0;
    m_clickMs = m_openMs = m_lastDepositClickMs = 0;
    m_cursorCheckPending = false;
    m_strandRetries = 0;
    m_lastListingStamp = 0;
    m_tripRun = m_pendingTrip;
    const bool town = w.gate.isTown || w.gate.isHideout;
    ctx.log.Add(LogCat::Stash, TrF("log_stash_run_started", {{"trigger", Tr(TriggerName(m_active))},
                                                             {"stash", Tr(DomainName(m_guild))},
                                                             {"used", Num("%d", w.inv.usedCells)},
                                                             {"cells", Num("%d", kBackpackCells)}}));
    const char* trig = TriggerName(m_active);
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.trigger = trig;
        d.moved = 0;
        d.skipped = 0;
    });
    if (m_tripRun && !town) {
        m_trip.BeginLeave(ctx);
        SetPhase(ctx, Phase::TripLeave);
    } else {
        if (m_tripRun) ctx.log.Add(LogCat::Stash, Tr("log_stash_resuming_trip"));
        SetPhase(ctx, Phase::Locate);
    }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

TaskStatus StashTask::Tick(BotContext& ctx) {
    const uint64_t now = ctx.now;
    if (m_phase == Phase::Idle) {
        m_fail = Tr("stash_fail_not_started");
        return TaskStatus::Failed;
    }
    if (m_lastTickMs != 0 && now > m_lastTickMs + kHoldGapMs) {
        // A hold kept the task (input was released): timers move with it, the
        // cursor may have moved, Ctrl is re-pressed before the next click.
        const uint64_t gap = now - m_lastTickMs;
        m_phaseMs += gap;
        m_runStartMs += gap;
        if (m_strandedMs) m_strandedMs += gap;
        if (m_escMs) m_escMs += gap;
        if (m_lastListingSeenMs) m_lastListingSeenMs += gap;
        m_cursorPlaced = false;
        if (gap >= 1000)
            ctx.log.Add(LogCat::Stash, TrF("log_stash_resumed", {{"seconds", Num("%.1f", static_cast<double>(gap) / 1000.0)}}));
    }
    m_lastTickMs = now;
    ctx.req.inventoryFast = true;

    const bool tripPhase = m_phase == Phase::TripLeave || m_phase == Phase::TripReturn || m_phase == Phase::Stranded;
    if (!tripPhase && m_phase != Phase::Closing && now - m_runStartMs > kRunWatchdogMs)
        return FailRun(ctx, Tr("stash_fail_too_long"));

    TaskStatus st = TaskStatus::Running;
    switch (m_phase) {
        case Phase::Idle:
            break;
        case Phase::TripLeave: {
            const TripStatus ts = m_trip.TickLeave(ctx);
            if (ts == TripStatus::Arrived) {
                SetPhase(ctx, Phase::Locate);
            } else if (ts != TripStatus::Running) {
                m_pendingFail = TrF("stash_fail_trip", {{"why", m_trip.FailReason()}});
                return Finish(ctx, TaskStatus::Failed);
            }
            break;
        }
        case Phase::Locate:     st = TickLocate(ctx); break;
        case Phase::Approach:   st = TickApproach(ctx); break;
        case Phase::WaitOpen:   st = TickWaitOpen(ctx); break;
        case Phase::Prepare:    st = TickPrepare(ctx); break;
        case Phase::SelectTab:  st = TickSelectTab(ctx); break;
        case Phase::Deposit:    st = TickDeposit(ctx); break;
        case Phase::Closing:    st = TickClosing(ctx); break;
        case Phase::TripReturn: st = TickTripReturn(ctx); break;
        case Phase::Stranded:   st = TickStranded(ctx); break;
    }
    if (st != TaskStatus::Running) return st;

    UpdateDetail();
    ctx.report.detail = m_detail;
    if (m_stashId != 0 && (m_phase == Phase::Approach || m_phase == Phase::WaitOpen)) {
        if (const Interactable* it = ctx.w.FindInteractable(m_stashId)) {
            TaskMarker mk;
            mk.grid = it->grid;
            mk.color = kStashMarkerColor;
            mk.label = Key("marker_stash");
            ctx.report.markers.push_back(std::move(mk));
        }
    }
    return TaskStatus::Running;
}

void StashTask::StartStashClick(BotContext& ctx, const Interactable& stash) {
    const StashSettings& s = ctx.p.stash;
    const TravelSettings& t = ctx.p.travel;
    WorldClickParams wp;
    wp.interactRange = s.approachRingCells;   // the object blocks: stand on a ring around it
    wp.dwellMs = s.clickDwellMs;
    wp.holdMs = (std::max)(t.pressHoldMs, 60);
    wp.reHovers = t.reHovers;
    wp.heightFactor = 0.f;
    wp.approachTimeoutMs = kApproachTimeoutMs;
    m_stashId = stash.id;
    m_click.Start(StashClickTarget(stash), wp, ctx.now);
    SetPhase(ctx, Phase::Approach);
}

TaskStatus StashTask::TickLocate(BotContext& ctx) {
    const World& w = ctx.w;
    if (!w.gate.isTown && !w.gate.isHideout) return FailRun(ctx, Tr("stash_fail_not_town"));
    const Interactable* st = PickStashObject(w.interactables, m_guild, w.self.grid);
    if (!st) {
        if (ctx.now - m_phaseMs < kLocateTimeoutMs) return TaskStatus::Running;
        return FailRun(ctx, TrF(w.gate.hostHasStashObjects ? Key("stash_fail_none_found") : Key("stash_fail_none_found_old_host"),
                                {{"stash", Tr(DomainName(m_guild))}}));
    }
    ctx.log.Add(LogCat::Stash, TrF("log_stash_using", {{"stash", Tr(DomainName(m_guild))},
                                                       {"id", Num("%u", st->id)},
                                                       {"x", Num("%.0f", st->grid.x)},
                                                       {"y", Num("%.0f", st->grid.y)},
                                                       {"cells", Num("%.0f", Dist(st->grid, w.self.grid))}}));
    StartStashClick(ctx, *st);
    return TaskStatus::Running;
}

TaskStatus StashTask::TickApproach(BotContext& ctx) {
    const World& w = ctx.w;
    const StashSettings& s = ctx.p.stash;
    const Interactable* it = w.FindInteractable(m_stashId);
    if (!it) {
        it = PickStashObject(w.interactables, m_guild, w.self.grid);
        if (!it) return FailRun(ctx, TrF("stash_fail_unknown", {{"stash", Tr(DomainName(m_guild))}}));
        m_stashId = it->id;
    }
    // Opportunistic runs give way while nothing is open yet: the leader leaving
    // through something the bot follows comes first.
    if (m_active == Trigger::Opportunistic && !m_ownsPanel) {
        const VanishInfo& v = w.leader.vanish;
        const TravelSettings& t = ctx.p.travel;
        const bool follows = (v.kind == VanishKind::Transition && t.followTransitions) ||
                             (v.kind == VanishKind::Portal && t.followPortals) ||
                             (v.kind == VanishKind::BossArena && t.enterBossArenas);
        if (v.active && follows && v.atMs != m_yieldedVanishMs) {
            m_yieldedVanishMs = v.atMs;
            ctx.log.Add(LogCat::Stash, TrF("log_stash_yield", {{"kind", Tr(VanishKindName(v.kind))}}));
            return Finish(ctx, TaskStatus::Done, false);
        }
    }
    m_click.UpdateTarget(StashClickTarget(*it));
    // The panel of an earlier click opened late.
    if (m_ownsPanel && m_clickMs != 0 && w.inv.valid && w.inv.stamp > m_clickMs && PanelOpen(w)) {
        m_click.Abort(ctx);
        m_openMs = w.inv.stamp;
        SetPhase(ctx, Phase::Prepare);
        return TaskStatus::Running;
    }
    const WorldClickStatus cs = m_click.Tick(ctx);
    if (cs == WorldClickStatus::Running) return TaskStatus::Running;
    ++m_attempt;
    if (cs == WorldClickStatus::Clicked) {
        // From the first click until the run ends the panel is the bot's own:
        // a late opening is never a "foreign panel".
        m_ownsPanel = true;
        ctx.act.SetUiTaskActive(true);
        m_clickMs = ctx.now;
        SetPhase(ctx, Phase::WaitOpen);
        return TaskStatus::Running;
    }
    if (m_attempt >= s.openRetries)
        return FailRun(ctx, TrF("stash_fail_unreachable", {{"stash", Tr(DomainName(m_guild))}, {"why", Tr(m_click.FailReason())}}));
    ctx.log.Add(LogCat::Stash, TrF("log_stash_click_retry", {{"why", Tr(m_click.FailReason())},
                                                             {"try", Num("%d", m_attempt + 1)},
                                                             {"max", Num("%d", s.openRetries)}}));
    StartStashClick(ctx, *it);
    return TaskStatus::Running;
}

TaskStatus StashTask::TickWaitOpen(BotContext& ctx) {
    const World& w = ctx.w;
    const StashSettings& s = ctx.p.stash;
    if (w.inv.valid && w.inv.stamp > m_clickMs) {
        if (PanelOpen(w)) {
            m_openMs = w.inv.stamp;
            ctx.log.Add(LogCat::Stash, TrF("log_stash_panel_open", {{"stash", Tr(DomainName(m_guild))},
                                                                    {"inventory", Num("%d", w.inv.openStashInventoryId)}}));
            SetPhase(ctx, Phase::Prepare);
            return TaskStatus::Running;
        }
        if (w.inv.backpackOpen && !m_guild && w.inv.guildOpen)
            return FailRun(ctx, Tr("stash_fail_guild_opened"));
        if (w.inv.backpackOpen && m_guild && w.inv.stashOpen && !w.inv.guildOpen)
            return FailRun(ctx, Tr("stash_fail_own_opened"));
    }
    if (ctx.now - m_phaseMs < static_cast<uint64_t>(s.openTimeoutMs)) return TaskStatus::Running;
    if (m_attempt >= s.openRetries)
        return FailRun(ctx, TrF("stash_fail_not_opened", {{"stash", Tr(DomainName(m_guild))}, {"clicks", Num("%d", m_attempt)}}));
    const Interactable* it = w.FindInteractable(m_stashId);
    if (!it) it = PickStashObject(w.interactables, m_guild, w.self.grid);
    if (!it) return FailRun(ctx, TrF("stash_fail_unknown", {{"stash", Tr(DomainName(m_guild))}}));
    ctx.log.Add(LogCat::Stash, TrF("log_stash_not_opened_retry", {{"try", Num("%d", m_attempt + 1)},
                                                                  {"max", Num("%d", s.openRetries)}}));
    StartStashClick(ctx, *it);
    return TaskStatus::Running;
}

TaskStatus StashTask::TickPrepare(BotContext& ctx) {
    const World& w = ctx.w;
    const InventoryState& inv = w.inv;
    const bool ready = inv.valid && inv.stamp >= m_openMs && inv.backpack.valid && inv.backpack.cell > 1.f &&
                       inv.cursorKnown;
    if (!ready) {
        if (ctx.now - m_phaseMs > kGridWaitMs) return FailRun(ctx, Tr("stash_fail_grid_unreadable"));
        return TaskStatus::Running;
    }
    if (PanelGone(w)) return FailRun(ctx, TrF("stash_fail_panel_closed", {{"stash", Tr(DomainName(m_guild))}}));
    if (inv.cursorHasItem) return StopForCursor(ctx, Key("stash_when_before"));
    m_planItems = inv.items;
    BuildPlan(ctx, m_planItems, m_plan);
    PublishPlan(m_plan, ctx.now);
    ctx.log.Add(LogCat::Stash, TrF("log_stash_plan", {{"items", Num("%d", PlanItemCount(m_plan))},
                                                      {"groups", Num("%d", static_cast<int>(m_plan.groups.size()))},
                                                      {"kept", Num("%d", m_plan.kept)},
                                                      {"protected", Num("%d", m_plan.protectedSkipped)}}));
    m_group = 0;
    if (m_plan.groups.empty()) {
        SetPhase(ctx, Phase::Closing);
        return TaskStatus::Running;
    }
    SetPhase(ctx, Phase::SelectTab);
    return TaskStatus::Running;
}

void StashTask::FallbackToOpenTab(BotContext& ctx, const LocText& why) {
    const DepositGroup& g = m_plan.groups[m_group];
    if (ctx.p.stash.missingTabUsesOpenTab) {
        Warn(ctx, TrF("stash_warn_open_tab", {{"why", why}}));
        m_groupTabLabel = Tr("stash_open_tab");
        BeginGroup(ctx);
        return;
    }
    Warn(ctx, TrF("stash_warn_items_stay", {{"why", why}, {"items", Num("%d", static_cast<int>(g.items.size()))}}));
    m_runLeftItems = true;
    m_skipped += static_cast<int>(g.items.size());
    ++m_group;
    m_tabStep = 0;
    m_tabTries = 0;
    if (m_group >= m_plan.groups.size()) SetPhase(ctx, Phase::Closing);
}

TaskStatus StashTask::TickSelectTab(BotContext& ctx) {
    const World& w = ctx.w;
    if (m_group >= m_plan.groups.size()) {
        SetPhase(ctx, Phase::Closing);
        return TaskStatus::Running;
    }
    if (w.inv.stamp > m_openMs && PanelGone(w))
        return FailRun(ctx, TrF("stash_fail_panel_closed", {{"stash", Tr(DomainName(m_guild))}}));
    const DepositGroup& g = m_plan.groups[m_group];
    if (Trim(g.tab).empty()) {
        m_groupTabLabel = Tr("stash_open_tab");
        BeginGroup(ctx);
        return TaskStatus::Running;
    }
    if (!w.gate.hostHasStashTabs) {
        FallbackToOpenTab(ctx, Tr("stash_why_tabs_old_host"));
        return TaskStatus::Running;
    }
    switch (m_tabStep) {
        case 0: {   // decide
            const bool ok = ReadTabs(ctx);
            if (!ok || m_tabsStatus < 0) {
                FallbackToOpenTab(ctx, TrF("stash_why_tabs_unreadable", {{"stash", Tr(DomainName(m_guild))},
                                                                          {"status", Num("%d", m_tabsStatus)}}));
                return TaskStatus::Running;
            }
            const TabAction a = DecideTab(m_tabs, m_toggle.Clickable(), g.tab, m_tabIndex);
            switch (a) {
                case TabAction::UseOpen:
                case TabAction::AlreadyActive:
                    m_groupTabLabel = TrF("stash_tab_named", {{"tab", g.tab}});
                    BeginGroup(ctx);
                    return TaskStatus::Running;
                case TabAction::Click:
                case TabAction::OpenList: {
                    if (ctx.cursorBusy) return TaskStatus::Running;
                    m_cursorAt = a == TabAction::Click ? m_tabs[static_cast<size_t>(m_tabIndex)].Center() : m_toggle.Center();
                    if (ctx.act.MoveCursor(m_cursorAt) != ActResult::Ok) return TaskStatus::Running;
                    m_cursorMs = ctx.now;
                    m_tabStep = a == TabAction::Click ? 1 : 3;
                    return TaskStatus::Running;
                }
                case TabAction::NotFound:
                    FallbackToOpenTab(ctx, TrF("stash_why_tab_not_found", {{"tab", g.tab}}));
                    return TaskStatus::Running;
                case TabAction::NotVisible:
                    FallbackToOpenTab(ctx, TrF("stash_why_tab_not_visible", {{"tab", g.tab}}));
                    return TaskStatus::Running;
            }
            return TaskStatus::Running;
        }
        case 1:
        case 3: {   // click the tab (1) or the list toggle (3) once the cursor settled
            if (ctx.now - m_cursorMs < kCursorSettleMs || ctx.cursorBusy || !ctx.act.GateReady()) return TaskStatus::Running;
            m_openIdBefore = w.inv.openStashInventoryId;
            const ActResult r = ctx.act.UiClick(m_cursorAt, VK_LBUTTON, false);
            if (r == ActResult::NotReady || r == ActResult::CursorBusy) return TaskStatus::Running;
            if (r != ActResult::Ok) {
                // Aborted (input released) or refused: decide again, a bounded number of times.
                if (++m_tabTries >= kTabTries) {
                    FallbackToOpenTab(ctx, TrF("stash_why_tab_click_failed", {{"tab", g.tab}, {"why", Tr(ActResultName(r))}}));
                    return TaskStatus::Running;
                }
                m_tabStep = 0;
                return TaskStatus::Running;
            }
            m_tabClickMs = ctx.now;
            m_tabStep = m_tabStep == 1 ? 2 : 4;
            return TaskStatus::Running;
        }
        case 2: {   // did the tab switch?
            if (ctx.now - m_tabClickMs < static_cast<uint64_t>(ctx.p.stash.tabSwitchWaitMs)) return TaskStatus::Running;
            ReadTabs(ctx);
            const SwitchCheck c = CheckTabSwitched(m_tabs, g.tab, m_openIdBefore, w.inv.openStashInventoryId, m_guild);
            if (c == SwitchCheck::NotYet) {
                if (++m_tabTries >= kTabTries) {
                    FallbackToOpenTab(ctx, TrF("stash_why_tab_not_switched", {{"tab", g.tab}}));
                    return TaskStatus::Running;
                }
                m_tabStep = 0;
                return TaskStatus::Running;
            }
            if (c == SwitchCheck::Unknown) Warn(ctx, Tr("stash_warn_tab_unverified"));
            ctx.log.Add(LogCat::Stash, TrF("log_stash_switched", {{"tab", g.tab}}));
            m_groupTabLabel = TrF("stash_tab_named", {{"tab", g.tab}});
            BeginGroup(ctx);
            return TaskStatus::Running;
        }
        case 4: {   // the list toggle was clicked: decide again on a fresh read
            if (ctx.now - m_tabClickMs < static_cast<uint64_t>(ctx.p.stash.tabSwitchWaitMs)) return TaskStatus::Running;
            if (++m_tabTries >= kTabTries) {
                FallbackToOpenTab(ctx, TrF("stash_why_tab_list_closed", {{"tab", g.tab}}));
                return TaskStatus::Running;
            }
            m_tabStep = 0;
            return TaskStatus::Running;
        }
        default:
            m_tabStep = 0;
            return TaskStatus::Running;
    }
}

void StashTask::BeginGroup(BotContext& ctx) {
    const StashSettings& s = ctx.p.stash;
    const DepositGroup& g = m_plan.groups[m_group];
    std::vector<uintptr_t> addrs;
    addrs.reserve(g.items.size());
    for (const auto& d : g.items)
        if (d.bagIndex < m_planItems.size()) addrs.push_back(m_planItems[d.bagIndex].address);
    DepositParams dp;
    dp.clickDelayMs = m_guild ? s.guildClickDelayMs : s.ownClickDelayMs;
    dp.confirmWindowMs = s.confirmWindowMs;
    dp.maxConfirmWindows = s.maxConfirmWindows;
    dp.tabFullSkips = s.tabFullSkips;
    m_queue.Begin(addrs, dp, ctx.now);
    m_lastListingSeenMs = ctx.now;
    m_groupActive = true;
    m_cursorPlaced = false;
    m_ctrlMs = 0;
    m_tabStep = 0;
    m_tabTries = 0;
    ctx.log.Add(LogCat::Stash, TrF("log_stash_depositing", {{"items", Num("%d", m_queue.Total())}, {"tab", m_groupTabLabel}}));
    SetPhase(ctx, Phase::Deposit);
}

void StashTask::EndGroup(BotContext& ctx, bool tabFull) {
    ReleaseCtrl(ctx);
    m_cursorPlaced = false;
    m_groupActive = false;
    const int moved = m_queue.Moved(), skipped = m_queue.Skipped();
    m_moved += moved;
    m_skipped += skipped;
    ctx.log.Add(LogCat::Stash, TrF(tabFull ? Key("log_stash_group_done_full") : Key("log_stash_group_done"),
                                   {{"tab", m_groupTabLabel}, {"moved", Num("%d", moved)}, {"skipped", Num("%d", skipped)}}));
    if (tabFull) {
        Warn(ctx, TrF("stash_warn_tab_full", {{"tab", m_groupTabLabel}, {"items", Num("%d", skipped)}}));
        m_runLeftItems = true;
    }
    const int tm = m_moved, ts = m_skipped;
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.moved = tm;
        d.skipped = ts;
    });
    ++m_group;
    SetPhase(ctx, m_group < m_plan.groups.size() ? Phase::SelectTab : Phase::Closing);
}

TaskStatus StashTask::TickDeposit(BotContext& ctx) {
    const World& w = ctx.w;
    const InventoryState& inv = w.inv;
    // Evidence from a new listing.
    if (inv.valid && inv.stamp != m_lastListingStamp && inv.stamp > m_openMs) {
        m_lastListingStamp = inv.stamp;
        m_lastListingSeenMs = ctx.now;
        if (PanelGone(w)) {
            ReleaseCtrl(ctx);
            return FailRun(ctx, TrF("stash_fail_panel_closed_deposit", {{"stash", Tr(DomainName(m_guild))}}));
        }
        if (inv.cursorKnown && inv.cursorHasItem) return StopForCursor(ctx, Key("stash_when_during"));
        m_present.clear();
        for (const auto& it : inv.items) m_present.push_back(it.address);
        m_queue.OnListing(m_present, inv.stamp);
    }
    if (ctx.now - m_lastListingSeenMs > kListingStallMs) {
        ReleaseCtrl(ctx);
        return FailRun(ctx, Tr("stash_fail_listing_stalled"));
    }
    const DepositQueue::State qs = m_queue.Update(ctx.now);
    if (qs != DepositQueue::State::Running) {
        EndGroup(ctx, qs == DepositQueue::State::TabFull);
        return TaskStatus::Running;
    }
    if (ctx.cursorBusy) return TaskStatus::Running;
    // Ctrl held for the group (re-pressed after a hold released it).
    if (!ctx.act.IsHeld(VK_CONTROL)) {
        m_cursorPlaced = false;
        if (ctx.act.KeyDown(VK_CONTROL) == ActResult::Ok) m_ctrlMs = ctx.now;
        return TaskStatus::Running;
    }
    if (ctx.now - m_ctrlMs < kCtrlSettleMs) return TaskStatus::Running;
    if (!m_cursorPlaced) {
        const uintptr_t item = m_queue.Next(ctx.now);
        if (item == 0) return TaskStatus::Running;
        const BagItem* bi = nullptr;
        for (const auto& it : inv.items)
            if (it.address == item) { bi = &it; break; }
        if (!bi) return TaskStatus::Running;   // the next listing decides about it
        const GridInfo& g = inv.backpack;
        Vec2 at = g.ItemCenter(bi->x, bi->y, (std::max)(1, bi->w), (std::max)(1, bi->h));
        const float spread = (std::min)(g.cell * 0.25f, static_cast<float>(ctx.act.Config().clickJitterPx));
        at += ctx.act.Random().CenterBiased(spread);
        if (w.gate.screenW > 0 && (at.x < 0.f || at.y < 0.f || at.x >= static_cast<float>(w.gate.screenW) ||
                                   at.y >= static_cast<float>(w.gate.screenH)))
            return FailRun(ctx, Tr("stash_fail_grid_outside"));
        if (ctx.act.MoveCursor(at) != ActResult::Ok) return TaskStatus::Running;
        m_cursorPlaced = true;
        m_cursorItem = item;
        m_cursorAt = at;
        m_cursorMs = ctx.now;
        return TaskStatus::Running;
    }
    if (ctx.now - m_cursorMs < kCursorSettleMs || !ctx.act.GateReady()) return TaskStatus::Running;
    const ActResult r = ctx.act.UiClick(m_cursorAt, VK_LBUTTON, false);
    if (r == ActResult::NotReady || r == ActResult::CursorBusy) return TaskStatus::Running;
    m_cursorPlaced = false;
    if (r == ActResult::Ok) {
        m_queue.OnClicked(m_cursorItem, ctx.now);
        m_lastDepositClickMs = ctx.now;
    }
    return TaskStatus::Running;
}

TaskStatus StashTask::TickClosing(BotContext& ctx) {
    const World& w = ctx.w;
    const InventoryState& inv = w.inv;
    ReleaseCtrl(ctx);
    // After a deposit the cursor must be empty (judged on a listing scanned after the last click).
    if (m_cursorCheckPending) {
        if (!(inv.valid && inv.cursorKnown && inv.stamp > m_lastDepositClickMs)) {
            if (ctx.now - m_phaseMs < kCloseVerifyMs) return TaskStatus::Running;
            Warn(ctx, Tr("stash_warn_cursor_unchecked"));
        } else if (inv.cursorHasItem) {
            return StopForCursor(ctx, Key("stash_when_after"));
        }
        m_cursorCheckPending = false;
    }
    if (!m_ownsPanel) return AfterPanelClosed(ctx);
    if (m_escMs != 0 && !(inv.valid && inv.stamp > m_escMs)) {
        if (ctx.now - m_escMs < kCloseVerifyMs) return TaskStatus::Running;
        if (m_escTries >= kCloseTries) {
            Warn(ctx, Tr("stash_warn_close_unverified"));
            return AfterPanelClosed(ctx);
        }
        // No listing after the Esc: judge by the latest one.
    }
    if (!inv.anyPanelOpen) {
        ctx.log.Add(LogCat::Stash, Tr("log_stash_panel_closed"));
        return AfterPanelClosed(ctx);
    }
    if (m_escTries >= kCloseTries) {
        if (m_pendingFail.empty()) m_pendingFail = Tr("stash_fail_not_closed");
        ctx.log.Add(LogCat::Stash, TrF("log_stash_panel_stayed_open", {{"presses", Num("%d", m_escTries)}}));
        return Finish(ctx, TaskStatus::Failed);
    }
    const ActResult r = ctx.act.TapKey(VK_ESCAPE, 30);
    if (r != ActResult::Ok) return TaskStatus::Running;
    ++m_escTries;
    m_escMs = ctx.now;
    // The game menu opens when the panel was already gone: Esc again closes it.
    // Checked here at once (bounded, cancellable): once the menu is up the gates
    // hold every tick and this task would not run to press it.
    if (ctx.sdk) {
        for (int waited = 0; waited < kEscMenuCheckMs; waited += 25) {
            if (!ctx.act.SleepCancellable(25)) break;
            if (ctx.sdk->Game.GetState() == PluginSDK::GameState::Escape) {
                ctx.log.Add(LogCat::Stash, Tr("log_stash_esc_menu"));
                ctx.act.TapKey(VK_ESCAPE, 30);
                break;
            }
        }
    }
    return TaskStatus::Running;
}

TaskStatus StashTask::AfterPanelClosed(BotContext& ctx) {
    m_ownsPanel = false;
    ctx.act.SetUiTaskActive(false);
    const bool town = ctx.w.gate.isTown || ctx.w.gate.isHideout;
    if (m_tripRun && ctx.mem.trip.active && town) {
        m_trip.BeginReturn(ctx);
        SetPhase(ctx, Phase::TripReturn);
        return TaskStatus::Running;
    }
    return Finish(ctx, m_pendingFail.empty() ? TaskStatus::Done : TaskStatus::Failed);
}

TaskStatus StashTask::TickTripReturn(BotContext& ctx) {
    const TripStatus ts = m_trip.TickReturn(ctx);
    if (ts == TripStatus::Running) return TaskStatus::Running;
    if (ts == TripStatus::Arrived) return Finish(ctx, m_pendingFail.empty() ? TaskStatus::Done : TaskStatus::Failed);
    m_strandedMs = ctx.now;
    m_strandedArea = ctx.w.gate.areaCounter;
    ctx.log.Add(LogCat::Stash, TrF("stash_detail_stranded", {{"why", m_trip.FailReason()}}));
    SetPhase(ctx, Phase::Stranded);
    return TaskStatus::Running;
}

TaskStatus StashTask::TickStranded(BotContext& ctx) {
    const World& w = ctx.w;
    ctx.mover.Stop(ctx.act);
    if (w.gate.areaCounter != m_strandedArea) {
        ctx.log.Add(LogCat::Stash, Tr("log_stash_left_town"));
        ctx.mem.trip = BotMemory::StashTrip{};
        return Finish(ctx, m_pendingFail.empty() ? TaskStatus::Done : TaskStatus::Failed);
    }
    if (w.leader.visible) {
        ctx.log.Add(LogCat::Stash, Tr("log_stash_leader_here"));
        ctx.mem.trip = BotMemory::StashTrip{};
        return Finish(ctx, m_pendingFail.empty() ? TaskStatus::Done : TaskStatus::Failed);
    }
    if (m_strandRetries < kStrandRetries && ctx.now - m_strandedMs >= kStrandRetryMs) {
        ++m_strandRetries;
        ctx.log.Add(LogCat::Stash, TrF("log_stash_strand_retry", {{"try", Num("%d", m_strandRetries)},
                                                                  {"max", Num("%d", kStrandRetries)}}));
        m_trip.BeginReturn(ctx);
        SetPhase(ctx, Phase::TripReturn);
    }
    return TaskStatus::Running;
}

// ---------------------------------------------------------------------------
// Endings
// ---------------------------------------------------------------------------

TaskStatus StashTask::FailRun(BotContext& ctx, const LocText& why) {
    if (m_pendingFail.empty()) m_pendingFail = why;
    ReleaseCtrl(ctx);
    m_click.Abort(ctx);
    m_cursorPlaced = false;
    ctx.log.Add(LogCat::Stash, TrF("log_stash_run_failed", {{"why", why}}));
    if (m_ownsPanel && m_phase != Phase::Closing) {
        SetPhase(ctx, Phase::Closing);
        return TaskStatus::Running;
    }
    const bool town = ctx.w.gate.isTown || ctx.w.gate.isHideout;
    if (m_tripRun && ctx.mem.trip.active && town && m_phase != Phase::TripReturn && m_phase != Phase::Stranded &&
        m_phase != Phase::TripLeave) {
        m_trip.BeginReturn(ctx);
        SetPhase(ctx, Phase::TripReturn);
        return TaskStatus::Running;
    }
    return Finish(ctx, TaskStatus::Failed);
}

TaskStatus StashTask::StopForCursor(BotContext& ctx, const char* when) {
    ReleaseCtrl(ctx);
    m_click.Abort(ctx);
    m_cursorPlaced = false;
    m_pendingFail = TrF("stash_fail_cursor_item", {{"when", Tr(when)}});
    m_stopAfter = true;
    return Finish(ctx, TaskStatus::Failed);   // the panel stays as it is
}

TaskStatus StashTask::Finish(BotContext& ctx, TaskStatus st, bool countAsRun) {
    const World& w = ctx.w;
    ReleaseCtrl(ctx);
    m_click.Abort(ctx);
    m_trip.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    m_cursorPlaced = false;
    m_groupActive = false;
    m_ownsPanel = false;
    ctx.act.SetUiTaskActive(false);
    if (!m_pendingFail.empty()) st = TaskStatus::Failed;
    m_fail = st == TaskStatus::Failed ? (m_pendingFail.empty() ? Tr("task_status_failed") : m_pendingFail) : LocText();
    if (countAsRun) {
        if (st == TaskStatus::Done) {
            ctx.mem.inventoryFull = false;
            m_failsInArea = 0;
            m_retryAfterMs = 0;
            if (w.inv.valid) m_baselineUsed = w.inv.usedCells;
        } else {
            m_retryAfterMs = ctx.now + static_cast<uint64_t>(ctx.p.stash.failCooldownMs);
            if (m_failArea == w.gate.areaCounter) ++m_failsInArea;
            else {
                m_failArea = w.gate.areaCounter;
                m_failsInArea = 1;
            }
        }
        // Items left behind pause the automatic runs; a manual run that moved
        // items and left nothing behind lifts the pause.
        if (m_runLeftItems) {
            if (!ctx.mem.stashBlocked) ctx.log.Add(LogCat::Stash, Tr("log_stash_paused"));
            ctx.mem.stashBlocked = true;
            ctx.mem.stashBlockedUsedCells = w.inv.valid ? w.inv.usedCells : kBackpackCells;
        } else if (ctx.mem.stashBlocked && m_active == Trigger::Manual && st == TaskStatus::Done && m_moved > 0) {
            ctx.mem.stashBlocked = false;
            ctx.log.Add(LogCat::Stash, Tr("log_stash_resumed_runs"));
        }
    }
    m_trigStamp = 0;   // re-plan the trigger on the next listing
    if (m_stopAfter) ctx.mem.RequestStop(TrF("error_stash", {{"why", m_fail}}));

    const LocText trigger = Tr(TriggerName(m_active));
    const LocText result =
        !countAsRun ? TrF("stash_result_gave_way", {{"trigger", trigger}})
                    : TrF(st == TaskStatus::Done ? Key("stash_result_done") : Key("stash_result_failed"),
                          {{"trigger", trigger}, {"moved", Num("%d", m_moved)}, {"skipped", Num("%d", m_skipped)}});
    if (m_fail.empty()) ctx.log.Add(LogCat::Stash, TrF("log_stash_result", {{"result", result}}));
    else ctx.log.Add(LogCat::Stash, TrF("log_stash_result_why", {{"result", result}, {"why", m_fail}}));
    const LocText fail = m_fail;
    const uint64_t now = ctx.now;
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.trigger = "";
        d.phase = "";
        d.lastResult = result;
        if (!fail.empty()) {
            d.lastFailure = fail;
            d.lastFailureMs = now;
        }
    });
    m_active = Trigger::None;
    m_phase = Phase::Idle;
    m_tripRun = false;
    m_detail.clear();
    return st;
}

void StashTask::Abort(BotContext& ctx) {
    if (m_phase != Phase::Idle) ctx.log.Add(LogCat::Stash, TrF("log_stash_interrupted", {{"phase", Tr(PhaseText())}}));
    ReleaseCtrl(ctx);
    m_click.Abort(ctx);
    m_trip.Abort(ctx);
    m_ownsPanel = false;
    m_cursorPlaced = false;
    m_groupActive = false;
    m_active = Trigger::None;
    m_phase = Phase::Idle;
    m_detail.clear();
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.trigger = "";
        d.phase = "";
    });
}

}  // namespace fb
