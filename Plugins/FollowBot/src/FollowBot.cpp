// FollowBot.cpp - lifecycle, hotkeys, the bot worker and the plugin info.
// FB_TESTS_EXCLUDE: plugin entry (PluginSDK + ImGui); not built into tests/.
#include "FollowBotPlugin.h"

#include "../../../POEFixer/plugin_sdk/PluginUi.h"
#include "common/Clock.h"
#include "config/Profiles.h"
#include "input/Win32Input.h"

#include <cstdio>

using namespace fb;

namespace {
constexpr uint64_t kHeartbeatTimeoutMs = 500;
constexpr uint64_t kFaultBackoffMs = 1000;
}  // namespace

FollowBotPlugin::FollowBotPlugin() = default;

FollowBotPlugin::~FollowBotPlugin() {
    StopWorker();
    m_activity.Stop();
    m_act.ReleaseAll();
}

const char* FollowBotPlugin::GetName() const { return PluginSDK_GetInfo()->name; }

bool FollowBotPlugin::WantsOverlay() const {
    const DebugSettings& d = m_settings.global.debug;
    return d.overlay || (d.statusWindow && (m_running.load() || m_debugActive.load()));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void FollowBotPlugin::EnsureLoaded() {
    if (m_loaded) return;
    const std::filesystem::path dir = DirectoryPath();
    if (dir.empty()) return;
    const io::LoadStatus st = LoadSettingsFile(SettingsFilePath(dir), m_settings);
    m_saveGuard.OnLoad(st);
    if (st == io::LoadStatus::Corrupt)
        ctx()->Log.Warn("Follow Bot: config/settings.json is not valid JSON - kept as settings.json.corrupt; running on "
                        "defaults, nothing is saved until a setting is changed");
    else if (st == io::LoadStatus::Unreadable)
        ctx()->Log.Warn("Follow Bot: config/settings.json could not be read - running on defaults without "
                        "overwriting it; it is read again later");
    m_log.SetFileSink(dir / "logs", m_settings.global.debug.logToFile);
    m_loaded = true;
}

void FollowBotPlugin::RetrySettingsLoad() {
    Settings fresh;
    const io::LoadStatus st = LoadSettingsFile(SettingsFilePath(DirectoryPath()), fresh);
    if (st == io::LoadStatus::Unreadable) return;
    if (st == io::LoadStatus::Loaded) {
        m_settings = std::move(fresh);
        PushSettings();
        ctx()->Log.Info("Follow Bot: config/settings.json could be read now - its settings are in use");
    }
    m_saveGuard.OnLoad(st);
}

void FollowBotPlugin::SaveSettings() {
    if (!m_loaded) return;
    if (m_saveGuard.ShouldRetryLoad()) RetrySettingsLoad();
    if (m_saveGuard.MayWrite() && SaveSettingsFile(SettingsFilePath(DirectoryPath()), m_settings)) m_dirty = false;
}

void FollowBotPlugin::MarkDirty() {
    m_dirty = true;
    m_saveGuard.OnUserChange();
    Clamp(m_settings);
    m_log.SetFileSink(DirectoryPath() / "logs", m_settings.global.debug.logToFile);
    PushSettings();
}

void FollowBotPlugin::PushSettings() {
    auto copy = std::make_shared<const Settings>(m_settings);
    std::lock_guard<std::mutex> lock(m_settingsMu);
    m_sharedSettings = std::move(copy);
}

std::shared_ptr<const Settings> FollowBotPlugin::WorkerSettings() {
    std::lock_guard<std::mutex> lock(m_settingsMu);
    return m_sharedSettings;
}

void FollowBotPlugin::OnKeyBound(int vk) {
    m_waitReleaseVk = vk;
    MarkDirty();
}

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------

void FollowBotPlugin::OnEnable(bool) {
    if (!HostCompatible()) {
        ctx()->Log.Error("Follow Bot: this POEFixer version is not compatible with the plugin - disabled");
        return;
    }
    if (ctx()->ImGuiContext) ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
    EnsureLoaded();
    m_running = false;
    m_paused = false;
    m_overrideHeld = false;
    m_keyPrev.fill(true);   // a key still held at enable time must not toggle anything
    m_heartbeatMs = NowMs();
    m_hooks.isTargeted = [sdk = ctx()](uintptr_t addr) {
        return addr != 0 && sdk->Components.ReadTargetable(addr).IsTargetedByPlayer;
    };
    m_act.SetLog(&m_log);
    m_act.SetAbortCheck([this] { return !InputAllowed(); });
    PushSettings();
    if (m_settings.global.safety.userActivityPause) m_activity.Start();
    StartWorker();

    auto& events = const_cast<PluginSDK::EventsService&>(ctx()->Events);
    if (!m_frameTok.Valid()) m_frameTok = events.OnFrame([this] {
        try {
            FrameTick();
        } catch (...) {
            m_running = false;
        }
    });
    m_enabled = true;
    m_log.Add(LogCat::General, "Follow Bot enabled");
    ctx()->Log.Info("Follow Bot enabled");
}

void FollowBotPlugin::OnDisable() {
    auto& events = const_cast<PluginSDK::EventsService&>(ctx()->Events);
    if (m_frameTok.Valid()) events.Unsubscribe(m_frameTok);
    m_frameTok = {};
    m_running = false;
    StopWorker();          // joined: no host call and no input from the worker after this
    m_act.ReleaseAll();
    m_activity.Stop();
    ctx()->Overlay.SetIncludeSleepingEntities(false);
    ctx()->Overlay.SetWantsOverlayInput(false);
    ctx()->Overlay.SetEntityScanFlags(0);   // host 365+ (no-op on older hosts); the worker is joined
    m_builder.Reset();                      // its cached scan flags must not outlive the request
    m_enabled = false;
    SaveSettings();
    ctx()->Log.Info("Follow Bot disabled");
}

void FollowBotPlugin::StartWorker() {
    if (m_worker.joinable()) return;
    m_workerStop = false;
    m_worker = std::thread(&FollowBotPlugin::WorkerLoop, this);
}

void FollowBotPlugin::StopWorker() {
    m_workerStop = true;
    if (m_worker.joinable()) m_worker.join();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void FollowBotPlugin::StartBot() {
    if (m_running.load()) return;
    m_paused = false;
    {
        std::lock_guard<std::mutex> lock(m_msgMu);
        m_stopMessage.clear();
    }
    m_running = true;
    ctx()->Log.Info("Follow Bot started");
}

void FollowBotPlugin::StopBot() {
    if (!m_running.exchange(false)) return;
    ctx()->Log.Info("Follow Bot stopped");
}

void FollowBotPlugin::TogglePause() { m_paused = !m_paused.load(); }

void FollowBotPlugin::RequestDebugWalk(InteractKind kind, bool click) {
    m_debugWalkClickReq = click;
    m_debugWalkKind = static_cast<int>(kind);
}

// ---------------------------------------------------------------------------
// Render thread, every frame: hotkeys + heartbeat
// ---------------------------------------------------------------------------

void FollowBotPlugin::FrameTick() {
    if (!HostCompatible()) return;
    const uint64_t now = NowMs();
    m_menuVisible = ctx()->Game.IsMenuVisible();
    {
        const SafetySettings& sf = m_settings.global.safety;
        m_activityPauseMs = sf.userActivityPause ? sf.userActivityPauseMs : 0;
    }
    m_heartbeatMs = now;
    // The user-activity hook runs only while its option is on.
    const bool wantHook = m_settings.global.safety.userActivityPause;
    if (wantHook != m_activity.Running()) {
        if (wantHook) m_activity.Start();
        else m_activity.Stop();
    }
    m_activity.SetGameWindow(ctx()->Game.GetGameWindow());
    UpdateHotkeys(now);
}

void FollowBotPlugin::UpdateHotkeys(uint64_t now) {
    if (m_waitReleaseVk != 0 && !win::IsKeyDown(m_waitReleaseVk)) m_waitReleaseVk = 0;
    const HotkeySettings& hk = m_settings.global.hotkeys;
    const bool front = ctx()->Game.IsForeground();   // the game or the overlay
    const bool capturing = PluginUi::IsCapturingHotkey();
    auto down = [&](int vk) {
        return vk != 0 && vk != m_waitReleaseVk && front && !capturing && win::IsKeyDown(vk);
    };
    const int keys[4] = {hk.startStop, hk.pause, hk.holdOverride, hk.stashNow};
    bool now4[4];
    for (int i = 0; i < 4; ++i) now4[i] = down(keys[i]);
    // A debug walk runs while the bot is stopped: Start / Stop and Pause end the
    // walk (never start the full bot over it).
    const bool debugWalk = m_debugActive.load() || m_debugWalkKind.load() >= 0;
    if (now4[0] && !m_keyPrev[0]) {
        if (m_running.load()) StopBot();
        else if (debugWalk) CancelDebugWalk();
        else StartBot();
    }
    if (now4[1] && !m_keyPrev[1]) {
        if (m_running.load()) TogglePause();
        else if (debugWalk) CancelDebugWalk();
    }
    // Hold-to-override: held = hold still; resume overrideResumeMs after release.
    const bool overrideNow = keys[2] != 0 && front && !capturing && win::IsKeyDown(keys[2]);
    if (!overrideNow && m_overrideHeld.load()) m_overrideReleasedMs = now;
    m_overrideHeld = overrideNow;
    if (now4[3] && !m_keyPrev[3] && m_running.load()) RequestStashNow();
    for (int i = 0; i < 4; ++i) m_keyPrev[i] = now4[i];
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

bool FollowBotPlugin::InputAllowed() const {
    if (m_workerStop.load()) return false;
    if (!m_running.load() && !m_debugActive.load()) return false;
    if (m_paused.load() || m_overrideHeld.load()) return false;
    // The POEFixer menu opened (INSERT leaves the game in front): a click already
    // under way must not land on an ImGui control under the cursor (the overlay
    // forwards the button it sees, injected ones included). Every menu-open tick
    // is a MenuVisible hold anyway.
    if (m_menuVisible.load()) return false;
    // You just used the mouse or keyboard: a running trajectory or dwell stops now
    // (the UserActivity gate holds from the next tick).
    const int pauseMs = m_activityPauseMs.load();
    const uint64_t activity = m_activity.LastActivityMs();
    if (pauseMs > 0 && activity != 0 && ElapsedMs(NowMs(), activity) < static_cast<uint64_t>(pauseMs)) return false;
    const uint64_t beat = m_heartbeatMs.load();
    return beat != 0 && NowMs() - beat <= kHeartbeatTimeoutMs;
}

void FollowBotPlugin::Publish(std::shared_ptr<const BotView> v) {
    std::lock_guard<std::mutex> lock(m_viewMu);
    m_view = std::move(v);
}

std::shared_ptr<const BotView> FollowBotPlugin::LatestView() const {
    std::lock_guard<std::mutex> lock(m_viewMu);
    return m_view;
}

void FollowBotPlugin::WorkerLoop() {
    while (!m_workerStop.load()) {
        auto settings = WorkerSettings();
        // Stopped (and no debug walk): read the game at 10 Hz for the settings tabs only.
        const bool active = m_running.load() || m_debugActive.load() || m_debugWalkKind.load() >= 0;
        const int tickMs = !active ? 100 : (settings ? settings->global.tickMs : 16);
        // The beat before the clock: a beat the render thread stores between the
        // two reads must not lie in the future (`now - beat` would wrap around and
        // read as a frozen host, releasing every held key mid-action).
        const uint64_t beat = m_heartbeatMs.load();
        const uint64_t now = NowMs();
        if (beat == 0 || ElapsedMs(now, beat) > kHeartbeatTimeoutMs) {
            // The overlay host froze or the plugin is being unloaded: hands off.
            if (!m_idleReleased) {
                m_act.ReleaseAll();
                m_idleReleased = true;
            }
            ::Sleep(50);
            continue;
        }
        m_idleReleased = false;
        if (m_lastFaultMs != 0 && now - m_lastFaultMs < kFaultBackoffMs) {
            ::Sleep(20);
            continue;
        }
        if (TickSEH(this) == 0) {
            ++m_faults;
            m_lastFaultMs = NowMs();
            m_act.ReleaseAll();
            m_log.Add(LogCat::Error, "Worker fault #%d: input released, retrying in 1 s", m_faults);
        }
        const uint64_t spent = NowMs() - now;
        if (spent < static_cast<uint64_t>(tickMs)) ::Sleep(static_cast<DWORD>(static_cast<uint64_t>(tickMs) - spent));
    }
    m_act.ReleaseAll();
}

// Layer 1: SEH (no C++ objects with destructors in this frame).
int FollowBotPlugin::TickSEH(FollowBotPlugin* self) {
    __try {
        return self->TickGuarded();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Layer 2: C++ exceptions.
int FollowBotPlugin::TickGuarded() {
    try {
        Tick();
        return 1;
    } catch (...) {
        return 0;
    }
}

// Layer 3: the work.
void FollowBotPlugin::Tick() {
    const std::shared_ptr<const Settings> sp = WorkerSettings();
    if (!sp) return;
    const Settings& s = *sp;
    const uint64_t now = NowMs();

    // Profile in use: auto-selected by the character name of the last tick.
    m_profileIndex = ResolveProfileIndex(s, m_world.self.name);
    if (m_profileIndex < 0 || m_profileIndex >= static_cast<int>(s.profiles.size())) m_profileIndex = 0;
    const Profile& p = s.profiles[static_cast<size_t>(m_profileIndex)];

    // Configure the machinery when the settings or the profile changed.
    if (m_configuredFor != sp.get() || m_configuredProfile != m_profileIndex) {
        m_configuredFor = sp.get();
        m_configuredProfile = m_profileIndex;
        ActuatorConfig ac;
        ac.gateMinMs = s.global.human.actionMinIntervalMs;
        ac.gateJitterMs = s.global.human.actionJitterMs;
        ac.clickJitterPx = s.global.human.clickJitterPx;
        ac.humanMouse = s.global.human.humanMouse;
        ac.humanSpeed = s.global.human.mouseSpeed;
        ac.humanCurvature = s.global.human.mouseCurvature;
        ac.humanTremor = s.global.human.mouseTremor;
        m_act.Configure(ac);
        const FollowSettings& f = p.follow;
        MoverParams mp;
        mp.moveKey = f.moveKey;
        mp.clickToMove = f.clickToMove;
        mp.sprintKey = f.sprintKey;
        mp.dodgeKey = f.dodgeKey;
        mp.lookaheadCells = f.lookaheadCells;
        mp.cursorDistancePx = f.cursorDistancePx;
        mp.reaimMs = f.reaimMs;
        mp.reaimPx = f.reaimPx;
        mp.reaimJitterMs = s.global.human.reaimJitterMs;
        mp.activationClick = f.activationClick;
        mp.sprint = f.sprint;
        mp.sprintDistance = f.sprintDistance;
        mp.sprintNoEnemyRadius = f.sprintNoEnemyRadius;
        mp.sprintMaxHoldMs = f.sprintMaxHoldMs;
        mp.sprintCooldownMs = f.sprintCooldownMs;
        mp.ignitionTimeoutMs = f.stuck.ignitionTimeoutMs;
        m_mover.Configure(mp);
        PlannerParams pp;
        pp.replanMs = f.replanMs;
        pp.astar.maxIterations = f.astarMaxIterations;
        pp.astar.wallPenalty = f.wallPenalty;
        pp.astar.snapRadius = f.snapRadius;
        m_planner.SetParams(pp);
        StuckConfig sc;
        sc.detect.minDisplacement = f.stuck.minDisplacement;
        sc.detect.windowMs = f.stuck.windowMs;
        sc.detect.noProgressMs = f.stuck.noProgressMs;
        sc.ladderResetMs = f.stuck.ladderResetMs;
        sc.blockRadius = f.stuck.blockRadius;
        sc.blockMs = f.stuck.blockMs;
        sc.sideStepMs = f.stuck.sideStepMs;
        sc.doorSearchCells = f.stuck.doorSearchCells;
        sc.backtrackMinCells = f.stuck.backtrackMinCells;
        m_stuck.Configure(sc);
    }

    // World.
    BuildParams bp;
    bp.leader.name = p.follow.leaderName;
    bp.leader.caseInsensitive = p.follow.leaderCaseInsensitive;
    bp.leader.autoPick = p.follow.leaderAuto;
    bp.leader.vanishRadius = p.travel.vanishRadius;
    bp.leader.outOfRangeCells = p.travel.outOfRangeCells;
    bp.botActive = m_running.load() || m_debugActive.load() || m_debugWalkKind.load() >= 0;
    WorldRequests req = m_req;
    if (m_uiWantsSkills.load()) {
        req.skills = true;
        req.selfBuffs = true;
        req.leaderBuffs = true;
    }
    m_builder.Build(m_world, req, bp, now);
    m_planner.SetGrid(m_world.nav);
    m_act.SetGameWindow(reinterpret_cast<HWND>(m_world.gate.gameWindow));
    if (m_world.gate.areaId != m_lastHostArea && m_world.gate.inGame) {
        m_lastHostArea = m_world.gate.areaId;
        if (m_running.load()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Follow Bot: entered %s", m_lastHostArea.c_str());
            ctx()->Log.Info(buf);
        }
    }

    m_req.Clear();
    m_report.Clear();
    BotContext bc{m_world, m_act, m_mover, m_planner, m_stuck, s, p, m_log, m_mem, m_req, m_report, m_hooks};
    bc.sdk = ctx();
    bc.now = now;
    bc.cursorBusy = m_act.CursorBusyByOther();

    // Start / stop transitions.
    const bool running = m_running.load();
    if (running != m_wasRunning) {
        m_wasRunning = running;
        if (running) {
            m_brain.OnStart(bc);
            m_stashSeen = m_stashNowCount.load();
            m_log.Add(LogCat::General, "Started (profile \"%s\", leader \"%s\")", p.name.c_str(),
                      p.follow.leaderName.empty() ? (p.follow.leaderAuto ? "auto" : "-") : p.follow.leaderName.c_str());
        } else {
            m_brain.OnStop(bc);
            m_log.Add(LogCat::General, "Stopped");
        }
    }

    BrainInputs in;
    in.running = running;
    in.paused = m_paused.load();
    in.overrideHeld = m_overrideHeld.load();
    in.overrideReleasedMs = m_overrideReleasedMs.load();
    in.lastUserActivityMs = s.global.safety.userActivityPause ? m_activity.LastActivityMs() : 0;
    const int stashCount = m_stashNowCount.load();
    in.stashNow = stashCount != m_stashSeen;
    m_stashSeen = stashCount;
    in.debugWalkKind = m_debugWalkKind.exchange(-1);
    in.debugWalkClick = m_debugWalkClickReq.load();
    in.debugCancel = m_debugCancel.exchange(false);

    BrainOutput out;
    // Input is allowed for a debug walk even while the bot is stopped.
    if (in.debugWalkKind >= 0) m_debugActive = true;
    m_brain.Tick(bc, in, out);
    m_debugActive = m_brain.DebugWalkActive();
    if (out.stopBot && m_running.exchange(false)) {
        const std::string why = out.stopReason.empty() ? std::string("stopped") : out.stopReason;
        m_log.Add(LogCat::General, "Bot stopped: %s", why.c_str());
        {
            std::lock_guard<std::mutex> lock(m_msgMu);
            m_stopMessage = why;
        }
        const std::string line = "Follow Bot stopped: " + why;
        ctx()->Log.Info(line.c_str());
        m_brain.OnStop(bc);
        m_wasRunning = false;
    }

    // Debug jobs (read the game; run here, never on the render thread).
    const Job job = static_cast<Job>(m_job.exchange(0));
    if (job != Job::None) RunJob(job, bc);

    // Publish.
    auto v = std::make_shared<BotView>();
    m_brain.FillView(*v, bc);
    v->running = m_running.load();
    v->paused = m_paused.load();
    v->profileIndex = m_profileIndex;
    v->profileName = p.name;
    v->profileAuto = IsAutoSelected(s, m_world.self.name, m_profileIndex);
    {
        std::lock_guard<std::mutex> lock(m_msgMu);
        if (!m_stopMessage.empty() && !v->running) v->lastError = "Stopped: " + m_stopMessage;
    }
    Publish(std::move(v));
}

// ---------------------------------------------------------------------------
// Factory exports + plugin info
// ---------------------------------------------------------------------------

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() { return new FollowBotPlugin(); }
extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) { delete p; }

// Plugin info: the host checks it before CreatePlugin; the publish tool reads it.
POEFIXER_PLUGIN_INFO(
    "FollowBot",
    "Follow Bot",
    "1.0.1",
    "POEFixer",
    "A follower that keeps up with your leader through areas and portals, fights, loots and stashes by your rules, and recovers from getting stuck.")
