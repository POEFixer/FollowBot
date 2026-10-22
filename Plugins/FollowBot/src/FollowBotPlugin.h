// FollowBotPlugin.h - the Follow Bot plugin: a follower character that keeps up
// with a named leader (design: docs/superpowers/specs/2026-10-22-followbot-
// plugin-design.md).
//
// Threads:
//  - render thread: OnFrame polls the hotkeys (edge-detected, a just-bound key
//    and a key held at enable never toggle), writes atomics and the worker
//    heartbeat; DrawUI draws the overlay and the status window from the latest
//    published BotView; DrawSettings draws the tabs. Nothing else.
//  - bot worker: WorldBuilder::Build -> Brain::Tick -> publish BotView, every
//    tickMs; three-layer guard (SEH -> try/catch -> work); a fault releases all
//    input and backs off 1 s; without a heartbeat newer than 500 ms it releases
//    input and idles.
//  - input hook thread (user-activity pause).
#pragma once

#include <imgui.h>

#include "../../../POEFixer/plugin_sdk/PluginSDK.h"

#include "brain/Brain.h"
#include "brain/BotView.h"
#include "common/BotLog.h"
#include "common/FileUtil.h"
#include "config/Settings.h"
#include "input/Actuator.h"
#include "input/InputHook.h"
#include "move/Mover.h"
#include "move/StuckRecovery.h"
#include "nav/PathPlanner.h"
#include "world/WorldBuilder.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class FollowBotPlugin : public PluginSDK::Plugin {
public:
    FollowBotPlugin();
    ~FollowBotPlugin() override;

    const char* GetName() const override;
    bool WantsOverlay() const override;
    void OnEnable(bool gameAttached) override;
    void OnDisable() override;
    void SaveSettings() override;
    void DrawUI() override;
    void DrawSettings() override;

private:
    // Debug jobs the worker runs between ticks (they read the game).
    enum class Job : int { None = 0, DumpEntities, DumpUi, ProbeDeath, FindUi };

    // --- lifecycle / settings (render thread) ---------------------------
    void EnsureLoaded();
    void RetrySettingsLoad();
    void MarkDirty();
    void PushSettings();
    void OnKeyBound(int vk);
    void FrameTick();
    void UpdateHotkeys(uint64_t now);
    void StartWorker();
    void StopWorker();

    // Commands (render thread -> worker).
    void StartBot();
    void StopBot();
    void TogglePause();
    // Stops a running or requested debug walk (the bot itself stays stopped).
    void CancelDebugWalk() {
        m_debugWalkKind = -1;
        m_debugCancel = true;
    }
    void RequestStashNow() { m_stashNowCount.fetch_add(1); }
    void RequestDebugWalk(fb::InteractKind kind, bool click);
    void RequestJob(Job j) { m_job.store(static_cast<int>(j)); }

    // --- worker ------------------------------------------------------------
    void WorkerLoop();
    static int TickSEH(FollowBotPlugin* self);
    int TickGuarded();
    void Tick();
    void RunJob(Job j, fb::BotContext& ctx);
    bool InputAllowed() const;   // the actuator's abort test (any thread)
    std::shared_ptr<const fb::Settings> WorkerSettings();
    void Publish(std::shared_ptr<const fb::BotView> v);

    // --- UI (ui/*.cpp) ------------------------------------------------------
    std::shared_ptr<const fb::BotView> LatestView() const;
    fb::Profile& EditProfile();
    int EditProfileIndex() const;
    void DrawHeader();
    void DrawTabGeneral();
    void DrawTabFollow();
    void DrawTabTravel();
    void DrawTabLoot();
    void DrawTabSkills();
    void DrawTabStash();
    void DrawTabSafety();
    void DrawTabDebug();
    void DrawOverlay();
    void DrawStatusWindow();
    // A key binding row (move / sprint / skill keys may be mouse buttons).
    bool KeyRow(const char* id, int* vk, const char* tip, bool hotkey);

    // --- render-thread state -----------------------------------------------
    fb::Settings        m_settings;
    fb::io::SaveGuard   m_saveGuard;
    bool                m_loaded = false;
    bool                m_enabled = false;
    bool                m_dirty = false;
    PluginSDK::EventsService::Token m_frameTok;
    int                 m_waitReleaseVk = 0;
    std::array<bool, 4> m_keyPrev{};       // startStop, pause, override, stashNow
    fb::UserActivityMonitor m_activity;
    std::shared_ptr<const fb::BotView> m_uiView;   // the view this frame's UI draws
    // Settings UI state.
    std::string         m_renameBuf;
    std::string         m_logFilter;
    int                 m_logCat = -1;
    bool                m_debugWalkClick = true;
    bool                m_showSkillsLive = false;
    std::string         m_findUiText;              // Debug tab: "Find UI by control name" input
    int                 m_findUiMatch = 0;         // 0 control name, 1 name starts with, 2 localized text
    bool                m_findUiVisibleOnly = false;

    // --- shared (atomics / mutex) ------------------------------------------
    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_paused{false};
    std::atomic<bool>     m_overrideHeld{false};
    std::atomic<uint64_t> m_overrideReleasedMs{0};
    std::atomic<uint64_t> m_heartbeatMs{0};
    // For InputAllowed, which the actuator polls DURING blocking actions (world
    // click trajectories, dwells, gate waits) on the worker: written every frame.
    std::atomic<bool>     m_menuVisible{false};    // the POEFixer menu is open
    std::atomic<int>      m_activityPauseMs{0};    // the user-activity pause window, 0 = off
    std::atomic<int>      m_stashNowCount{0};
    std::atomic<int>      m_debugWalkKind{-1};
    std::atomic<bool>     m_debugWalkClickReq{true};
    std::atomic<bool>     m_debugCancel{false};
    std::atomic<bool>     m_debugActive{false};
    std::atomic<int>      m_job{0};
    std::mutex            m_findUiMu;              // the Job::FindUi query (render thread -> worker)
    std::string           m_findUiQuery;
    int                   m_findUiQueryMatch = 0;
    bool                  m_findUiQueryVisibleOnly = false;
    std::atomic<bool>     m_uiWantsSkills{false};
    std::atomic<bool>     m_workerStop{false};
    std::mutex            m_settingsMu;
    std::shared_ptr<const fb::Settings> m_sharedSettings;
    mutable std::mutex    m_viewMu;
    std::shared_ptr<const fb::BotView> m_view;
    std::mutex            m_msgMu;
    std::string           m_stopMessage;   // why the worker stopped the bot (shown once)
    fb::BotLog            m_log;

    // --- worker-only state ---------------------------------------------------
    fb::WorldBuilder   m_builder{ctx()};
    fb::World          m_world;
    fb::Actuator       m_act;
    fb::Mover          m_mover;
    fb::PathPlanner    m_planner;
    fb::StuckRecovery  m_stuck;
    fb::BotMemory      m_mem;
    fb::WorldRequests  m_req;
    fb::TaskReport     m_report;
    fb::HostHooks      m_hooks;
    fb::Brain          m_brain;
    bool               m_wasRunning = false;
    int                m_stashSeen = 0;
    int                m_profileIndex = 0;
    const fb::Settings* m_configuredFor = nullptr;
    int                m_configuredProfile = -1;
    uint64_t           m_lastFaultMs = 0;
    int                m_faults = 0;
    bool               m_idleReleased = false;
    std::string        m_lastHostArea;

    // Last member: destroyed (joined) first, before anything it could touch.
    std::thread        m_worker;
};
