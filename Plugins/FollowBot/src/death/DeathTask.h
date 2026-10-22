// DeathTask.h - detect death and respawn through the death screen
// (design §5.8). Exclusive task; preempts everything (Brain step 2).
//
// Contract (the Brain relies on it):
//  - WantsToStart: w.self.dead (HP 0 with max HP > 0) for >= deadConfirmMs.
//    Every call also feeds the death tracker: a confirmed death that ends
//    while the task is not running (you or a party member respawned it, or a
//    hold aborted the task) is still counted, once.
//  - Start: release input; schedule the first click after respawnDelayMs +
//    0..respawnJitterMs (a resumed death retries after retryIntervalMs).
//  - Tick: find the visible "death_screen" (host 365+:
//    Ui.FindElements(0, "death_screen", ControlName, 12, visible)), inside it
//    the first VISIBLE of "respawn_at_checkpoint_button",
//    "revive_at_checkpoint_button" and - only with allowGenericRespawn -
//    "respawn_button" (first instead of last when preferCheckpoint is off);
//    UI-click the centre of its rect; retry every retryIntervalMs, at most
//    maxRetries tries. Alive again -> count the death (mem.deathsSession; deaths
//    in a map also mem.deathsMap), clear position-dependent blacklists, Done.
//    maxDeathsPerMap / maxDeathsPerSession reached -> mem.RequestStop(reason).
//    Without the host functions, with autoRespawn off, or when the names are not
//    found: hold with "Dead - respawn manually" (Running) and log the reason once
//    (with the dump hint when a lookup failed).
//  - KeepThroughHold: Loading and PanelUnknown (a respawn in town / hideout
//    loads a new area and restarts the inventory listing).
//  - AllowsCasts: none.
#pragma once

#include "../brain/Task.h"
#include "DeathLogic.h"

#include <string>

namespace fb {

class DeathTask : public Task {
public:
    const char* Name() const override { return "Death"; }

    bool WantsToStart(BotContext& ctx);
    void Start(BotContext& ctx);
    TaskStatus Tick(BotContext& ctx) override;
    void Abort(BotContext& ctx) override;
    bool AllowsCasts(CastClass) const override { return false; }
    bool KeepThroughHold(GateReason r) const override {
        return r == GateReason::Loading || r == GateReason::PanelUnknown;
    }

    // Debug tool ("Test death screen lookup"): what the lookup finds right now,
    // as one line for the Debug tab. Runs on the worker.
    static std::string ProbeDeathScreen(BotContext& ctx);

private:
    void BeginDeath(BotContext& ctx);
    void OnRevived(BotContext& ctx, bool byTask);
    TaskStatus HoldManual(BotContext& ctx, const std::string& why, bool dumpHint);

    death::DeathTracker m_tracker;
    // The death being handled (from confirmed to alive again).
    bool        m_active = false;
    uint64_t    m_session = 0;          // mem.startedMs when it was confirmed (a death before Start is not counted)
    bool        m_inMap = false;
    std::string m_areaId;
    uint64_t    m_nextTryMs = 0;
    int         m_tries = 0;            // death-screen lookups (each may click)
    int         m_clicks = 0;
    const char* m_lastButton = nullptr;
    bool        m_manualLogged = false;
};

}  // namespace fb
