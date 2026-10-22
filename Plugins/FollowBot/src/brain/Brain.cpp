// Brain.cpp - see Brain.h.
// FB_TESTS_EXCLUDE: the glue over every task (some tasks are SDK adapters); not built into tests/.
#include "Brain.h"

#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "../move/StuckRecovery.h"
#include "../nav/PathPlanner.h"

namespace fb {

namespace {
// Emergency exit: ticks to retry /exit while the action gate defers it (a cast
// or click just went out) before stopping with the reason.
constexpr int kEmergencyExitTries = 5;
}  // namespace

void Brain::OnStart(BotContext& ctx) {
    AbortExclusive(ctx);
    m_follow.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    ctx.act.ReleaseAll();
    ctx.stuck.Reset(ctx.now);
    ctx.planner.Clear();
    m_skills.Reset();
    m_deadHold = false;
    m_foreignPanelSinceMs = 0;
    m_exitTries = 0;
    const uint32_t area = ctx.mem.area;
    const bool haveArea = ctx.mem.haveArea;
    const std::string areaId = ctx.mem.areaId;
    const uint64_t entered = ctx.mem.areaEnteredMs;
    ctx.mem = BotMemory{};
    ctx.mem.startedMs = ctx.now;
    ctx.mem.area = area;
    ctx.mem.haveArea = haveArea;
    ctx.mem.areaId = areaId;
    ctx.mem.areaEnteredMs = entered;
}

void Brain::OnStop(BotContext& ctx) {
    AbortExclusive(ctx);
    m_follow.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    ctx.act.ReleaseAll();
    ctx.planner.Clear();
    m_gate = GateReason::Stopped;
}

void Brain::AbortExclusive(BotContext& ctx) {
    if (!m_exclusive) return;
    ctx.log.Add(LogCat::Task, "%s aborted", m_exclusive->Name());
    m_exclusive->Abort(ctx);
    m_exclusive = nullptr;
    ctx.act.SetUiTaskActive(false);
}

void Brain::StartExclusive(BotContext& ctx, Task* t) {
    if (m_exclusive == t) return;
    if (m_exclusive) AbortExclusive(ctx);
    m_follow.Abort(ctx);
    ctx.stuck.Reset(ctx.now);
    m_exclusive = t;
    ctx.log.Add(LogCat::Task, "%s started", t->Name());
}

void Brain::EndExclusive(BotContext& ctx, TaskStatus st) {
    if (!m_exclusive) return;
    std::string extra;
    if (m_exclusive == &m_travel && !m_travel.FailReason().empty()) extra = m_travel.FailReason();
    else if (m_exclusive == &m_stash && !m_stash.FailReason().empty()) extra = m_stash.FailReason();
    else if (m_exclusive == &m_pickup && !m_pickup.FailReason().empty()) extra = m_pickup.FailReason();
    else if (m_exclusive == &m_walk) {
        extra = m_walk.Result();
        m_debugResult = "Debug walk: " + extra;
    }
    ctx.log.Add(LogCat::Task, "%s %s%s%s", m_exclusive->Name(), TaskStatusName(st), extra.empty() ? "" : ": ",
                extra.c_str());
    if (st == TaskStatus::Failed && !extra.empty()) ctx.mem.lastError = std::string(m_exclusive->Name()) + ": " + extra;
    ctx.mover.Stop(ctx.act);
    ctx.stuck.Reset(ctx.now);
    ctx.act.SetUiTaskActive(false);
    m_exclusive = nullptr;
}

void Brain::Hold(BotContext& ctx, GateReason why, bool closePanel) {
    if (m_exclusive && !m_exclusive->KeepThroughHold(why)) {
        if (m_exclusive == &m_walk) m_debugResult = std::string("Debug walk stopped: ") + GateReasonText(why);
        AbortExclusive(ctx);
    }
    if (m_pendingWalk >= 0 && !m_walk.KeepThroughHold(why)) m_pendingWalk = -1;
    m_follow.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    ctx.act.ReleaseAll();
    // Standing still is not "stuck": disarm the detector (the history is kept),
    // or the first walking tick after the hold compares against a pre-hold sample.
    ctx.stuck.NoteIdle(ctx.w, ctx.now);
    if (closePanel && ctx.now - m_lastEscMs > 1500) {
        m_lastEscMs = ctx.now;
        ctx.log.Add(LogCat::Gate, "Closing a panel the bot did not open (Esc)");
        ctx.act.TapKey(VK_ESCAPE, 30);
    }
    if (why != m_gate) ctx.log.Add(LogCat::Gate, "Holding: %s", GateReasonText(why));
    m_gate = why;
}

void Brain::TrackArea(BotContext& ctx) {
    const GameGate& g = ctx.w.gate;
    if (ctx.mem.haveArea && ctx.mem.area == g.areaCounter) return;
    const bool first = !ctx.mem.haveArea;
    ctx.mem.area = g.areaCounter;
    ctx.mem.haveArea = true;
    ctx.mem.areaEnteredMs = ctx.now;
    ctx.mem.areaId = g.areaId;
    ctx.mem.lootBlacklist.clear();   // entity ids are per area
    ctx.planner.Clear();
    if (g.isMap) {
        if (death::IsNewMapInstance(m_lastMapId, m_lastMapHash, g.areaId, g.areaHash)) {
            m_lastMapId = g.areaId;
            m_lastMapHash = g.areaHash;
            ctx.mem.deathsMap = 0;
        } else if (!death::KnownAreaHash(m_lastMapHash)) {
            m_lastMapHash = g.areaHash;   // the same instance, its hash known now
        }
    }
    if (!first) ctx.log.Add(LogCat::General, "Entered %s", g.areaId.empty() ? "a new area" : g.areaId.c_str());
}

void Brain::Tick(BotContext& ctx, const BrainInputs& in, BrainOutput& out) {
    const World& w = ctx.w;
    ctx.mover.BeginTick(w, ctx.now);
    TrackArea(ctx);
    m_pickup.Observe(ctx);   // live loot Diagnostics while the Loot tab shows them (holds and stop included)
    if (in.stashNow) ctx.mem.manualStashRequested = true;
    if (in.debugCancel) {
        const bool walking = m_exclusive == &m_walk;
        if (walking || m_pendingWalk >= 0) m_debugResult = "Debug walk stopped";
        if (walking) AbortExclusive(ctx);
        m_pendingWalk = -1;
    }
    if (in.debugWalkKind >= 0 && in.debugWalkKind < static_cast<int>(InteractKind::Count)) {
        // Started once the gates pass (the button is clicked with the POEFixer menu open).
        m_pendingWalk = in.debugWalkKind;
        m_pendingWalkClick = in.debugWalkClick;
        m_debugResult = "Debug walk: waiting (close the POEFixer menu)";
    }

    // Requests from tasks (death limit, an item on the cursor, ...): before the
    // gates and before any task, so no input follows the request (a hold such as
    // the panel the stash run left open must not keep it pending, and no task may
    // run on the tick that finally stops). No Esc: the panel stays as it is.
    if (ctx.mem.stopRequested) {
        out.stopBot = true;
        out.stopReason = ctx.mem.stopReason;
        ctx.mem.stopRequested = false;
        Hold(ctx, GateReason::Stopped, false);
        out.gate = GateReason::Stopped;
        return;
    }

    // A debug walk runs without Start (it only needs the safety gates).
    const bool debugWalking = DebugWalkActive();

    // 1. Gates.
    const bool botPanel = m_exclusive == &m_stash && m_stash.OwnsPanel();
    if (ForeignPanelVisible(w.inv) && !botPanel) {
        if (m_foreignPanelSinceMs == 0) m_foreignPanelSinceMs = ctx.now;
    } else {
        m_foreignPanelSinceMs = 0;
    }
    GateInputs gi;
    gi.now = ctx.now;
    gi.running = in.running || debugWalking;
    gi.paused = in.paused;
    gi.overrideHeld = in.overrideHeld;
    gi.overrideReleasedMs = in.overrideReleasedMs;
    gi.lastUserActivityMs = in.lastUserActivityMs;
    gi.botOwnsPanel = botPanel;
    gi.foreignPanelSinceMs = m_foreignPanelSinceMs;
    gi.startedMs = in.running ? ctx.mem.startedMs : 0;
    GateParams gp;
    const SafetySettings& sf = ctx.s.global.safety;
    gp.userActivityPause = sf.userActivityPause;
    gp.userActivityPauseMs = sf.userActivityPauseMs;
    gp.overrideResumeMs = sf.overrideResumeMs;
    gp.maxRuntimeMinutes = sf.maxRuntimeMinutes;
    gp.closeForeignPanels = sf.closeForeignPanels;
    gp.closeForeignPanelsAfterMs = sf.closeForeignPanelsAfterMs;
    const GateResult g = EvaluateGates(w.gate, w.inv, gi, gp);
    out.gate = g.reason;
    if (g.stopBot) {
        out.stopBot = true;
        out.stopReason = GateReasonText(g.reason);
    }
    if (g.reason != GateReason::None) {
        Hold(ctx, g.reason, g.closePanel);
        return;
    }
    if (m_gate != GateReason::None) ctx.log.Add(LogCat::Gate, "Resumed");
    m_gate = GateReason::None;

    // Emergency exit: HP below the threshold in a map -> /exit once, then stop.
    // The command can wait behind the action gate (a cast or click just went
    // out): hold still and retry for a few ticks; the stop reason says whether
    // /exit was sent.
    if (in.running && sf.emergencyExit && w.gate.isMap && w.self.valid && !w.self.dead && w.self.hpMax > 0 &&
        w.self.hpPct < static_cast<float>(sf.emergencyExitHpPct)) {
        if (m_exitTries == 0)
            ctx.log.Add(LogCat::General, "Emergency exit: HP %.0f%% < %d%%", w.self.hpPct, sf.emergencyExitHpPct);
        AbortExclusive(ctx);
        ctx.mover.Stop(ctx.act);
        ctx.act.ReleaseAll();
        ctx.stuck.NoteIdle(ctx.w, ctx.now);
        const ActResult r = ctx.act.TypeChatCommand("/exit");
        const bool deferred = r == ActResult::NotReady || r == ActResult::CursorBusy;
        if (r == ActResult::Ok) {
            m_exitTries = 0;
            out.stopBot = true;
            out.stopReason = "Emergency exit (low HP)";
        } else if (deferred && ++m_exitTries < kEmergencyExitTries) {
            ctx.log.Add(LogCat::General, "Emergency exit: /exit not sent yet (%s) - retrying", ActResultName(r));
            ctx.report.detail = "Emergency exit: retrying /exit";
        } else {
            m_exitTries = 0;
            out.stopBot = true;
            out.stopReason = deferred ? std::string("Emergency exit: /exit could not be sent")
                                      : std::string("Emergency exit: /exit was not sent (") + ActResultName(r) + ")";
        }
        return;
    }
    m_exitTries = 0;

    // 2. Death.
    if (m_exclusive != &m_death && m_death.WantsToStart(ctx)) {
        StartExclusive(ctx, &m_death);
        m_death.Start(ctx);
    }
    if (m_exclusive != &m_death && w.self.dead) {
        // With auto-respawn on, the death task starts once death is confirmed
        // (deadConfirmMs): until then this is just "dead", not a manual job.
        const bool autoRespawn = ctx.p.death.autoRespawn;
        if (!m_deadHold) ctx.log.Add(LogCat::Death, autoRespawn ? "Dead" : "Dead: holding (respawn manually)");
        m_deadHold = true;
        AbortExclusive(ctx);
        m_follow.Abort(ctx);
        ctx.mover.Stop(ctx.act);
        ctx.act.ReleaseAll();
        ctx.stuck.NoteIdle(ctx.w, ctx.now);
        ctx.report.detail = autoRespawn ? "Dead" : "Dead - respawn manually";
        return;
    }
    m_deadHold = false;

    // A pending debug walk starts now that the gates pass.
    if (m_pendingWalk >= 0) {
        m_walk.Configure(static_cast<InteractKind>(m_pendingWalk), m_pendingWalkClick);
        m_pendingWalk = -1;
        StartExclusive(ctx, &m_walk);
        m_walk.Start(ctx);
        if (!m_walk.Result().empty()) {   // nothing to walk to
            m_debugResult = "Debug walk: " + m_walk.Result();
            m_exclusive = nullptr;
        } else {
            m_debugResult = "Debug walk: running";
        }
    }
    if (!in.running && m_exclusive != &m_walk) {
        // Only a debug walk runs without Start.
        Hold(ctx, GateReason::Stopped, false);
        out.gate = GateReason::Stopped;
        return;
    }

    // 3. Exclusive task in progress (a vanish to follow through preempts Pickup).
    if (m_exclusive == &m_pickup && m_travel.WantsToStart(ctx)) {
        AbortExclusive(ctx);
        StartExclusive(ctx, &m_travel);
        m_travel.Start(ctx);
    }
    if (!m_exclusive) {
        // 4. Travel trigger, 5. Stash trigger.
        if (m_travel.WantsToStart(ctx)) {
            StartExclusive(ctx, &m_travel);
            m_travel.Start(ctx);
        } else if (m_stash.WantsToStart(ctx)) {
            StartExclusive(ctx, &m_stash);
            m_stash.Start(ctx);
        }
    }
    if (m_exclusive) {
        const TaskStatus st = m_exclusive->Tick(ctx);
        Task* running = m_exclusive;
        if (st != TaskStatus::Running) EndExclusive(ctx, st);
        else m_skills.TryCast(ctx, running);   // 6. interleaved casts
        return;
    }

    // 6. Skill cast (a cast owns the cursor this tick; movement resumes next tick).
    // A long press or channel stands still: the detector must not stay armed.
    if (m_skills.TryCast(ctx, &m_follow)) {
        if (!ctx.mover.IsMoving()) ctx.stuck.NoteIdle(ctx.w, ctx.now);
        return;
    }

    // 7. Loot trigger.
    if (m_pickup.WantsToStart(ctx)) {
        StartExclusive(ctx, &m_pickup);
        m_pickup.Start(ctx);
        const TaskStatus st = m_pickup.Tick(ctx);
        if (st != TaskStatus::Running) EndExclusive(ctx, st);
        return;
    }

    // 8. Follow / 9. idle near the leader.
    m_follow.Tick(ctx);
}

void Brain::FillView(BotView& v, const BotContext& ctx) const {
    const World& w = ctx.w;
    v.nowMs = ctx.now;
    v.tick = w.tick;
    v.gateReason = m_gate == GateReason::None ? std::string() : GateReasonText(m_gate);
    v.taskName = ActiveTaskName();
    v.taskDetail = ctx.report.detail;
    v.debugWalk = DebugWalkActive();
    v.debugResult = m_debugResult;
    if (!v.gateReason.empty()) v.mode = v.gateReason;
    else if (m_exclusive) v.mode = m_exclusive->Name();
    else v.mode = FollowTask::ModeName(m_follow.CurrentMode());
    v.lastError = ctx.mem.lastError;

    v.attached = w.gate.attached;
    v.inGame = w.gate.inGame;
    v.foreground = w.gate.foregroundExact;
    v.areaId = w.gate.areaId;
    v.areaCounter = w.gate.areaCounter;
    v.isTown = w.gate.isTown;
    v.isHideout = w.gate.isHideout;
    v.isMap = w.gate.isMap;
    v.characterName = w.self.name;
    v.hostHasControlNames = w.gate.hostHasControlNames;
    v.hostHasStashTabs = w.gate.hostHasStashTabs;
    v.hostHasStashObjects = w.gate.hostHasStashObjects;

    v.selfGrid = w.self.grid;
    v.selfWorld = w.self.world;
    v.hpPct = w.self.hpPct;
    const LeaderState& L = w.leader;
    v.leaderConfigured = L.configured;
    v.leaderVisible = L.visible;
    v.leaderName = L.visible ? L.info.name : ctx.p.follow.leaderName;
    v.leaderGrid = L.info.grid;
    v.leaderWorld = L.info.world;
    v.leaderLastGrid = L.lastGrid;
    v.leaderDistance = L.visible ? Dist(L.info.grid, w.self.grid) : (L.lastSeenMs ? Dist(L.lastGrid, w.self.grid) : -1.f);
    v.leaderSpeed = L.speed;
    v.vanish = L.vanish.active ? L.vanish.kind : VanishKind::None;
    v.leaderRoute = L.route;
    v.nearbyPlayers.clear();
    for (const auto& o : w.others)
        if (!o.name.empty()) v.nearbyPlayers.push_back(o.name);

    v.path = ctx.planner.Path();
    v.planStatus = PlanStatusName(ctx.planner.Status());
    v.hasGoal = m_follow.HasGoal() || ctx.report.hasTarget;
    v.goal = ctx.report.hasTarget ? ctx.report.target : m_follow.Goal();
    v.hasLookahead = ctx.mover.HasLookahead();
    v.lookahead = ctx.mover.Lookahead();
    v.hasCursor = ctx.mover.HasCursorTarget();
    v.cursorClient = ctx.mover.CursorTarget();
    v.moving = ctx.mover.IsMoving();
    v.sprinting = ctx.mover.Sprinting();
    v.followDistance = ctx.p.follow.followDistance;
    v.stopDistance = ctx.p.follow.stopDistance;
    v.stuckStep = RecoveryStepName(ctx.stuck.CurrentStep());
    v.stuckEpisodes = ctx.stuck.Episodes();
    v.stuckMarks = ctx.stuck.StuckMarks();
    v.blocks.clear();
    for (const auto& b : ctx.planner.Overlay().Blocks())
        v.blocks.push_back({NavGrid::CellCenter(b.cx, b.cy), b.r});

    v.interactables.clear();
    for (const auto& it : w.interactables) {
        InteractableView iv;
        iv.id = it.id;
        iv.kind = it.kind;
        iv.portalType = it.portalType;
        iv.grid = it.grid;
        iv.world = it.world;
        iv.targetable = it.targetable;
        iv.visibleNow = it.visibleNow;
        v.interactables.push_back(iv);
    }
    v.markers.clear();
    for (const auto& m : ctx.report.markers) v.markers.push_back({m.grid, m.color, m.label});
    v.monsters = static_cast<int>(w.monsters.size());
    v.groundItems = static_cast<int>(w.items.size());
    v.usedCells = w.inv.usedCells;
    v.inventoryValid = w.inv.valid;

    v.combatRadius = ctx.p.skills.combatRadius;
    v.leashRadius = ctx.p.skills.leashRadius;
    v.currentRule = ctx.mem.currentRule;
    v.ruleWhyNot = m_skills.WhyNot();

    v.deathsMap = ctx.mem.deathsMap;
    v.deathsSession = ctx.mem.deathsSession;
    v.runMs = ctx.mem.startedMs ? ctx.now - ctx.mem.startedMs : 0;

    v.skills = w.skills;
    v.selfBuffs = w.selfBuffs;
    v.leaderBuffs = w.leaderBuffs;
    v.proj = w.proj;
}

}  // namespace fb
