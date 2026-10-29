// TravelTask.h - follow the leader through the transition / portal he used
// (design §5.7). Exclusive task, started by the Brain at step 4.
//
// Contract (the Brain relies on it):
//  - WantsToStart: the leader vanished (w.leader.vanish.active) with a kind the
//    settings allow following through (Transition: followTransitions,
//    BossArena: enterBossArenas, Portal: followPortals and a plausible portal
//    kind, see TravelLogic.h), our area counter has not changed since, and this
//    vanish (its atMs) has not already ended in Done / Failed. Never pre-clicks:
//    only after a vanish. A travel aborted by a hold or a preemption resumes
//    for the same vanish with the attempts it has left (bounded).
//  - Start: takes w.leader.vanish as the target (interactableId, re-resolved by
//    id every tick; falls back to the nearest usable interactable of the same
//    kind to the leader's last position within fallbackRadius, e.g. another
//    map-device portal). An object not in our entity list yet (remembered or
//    not seen) is walked toward until it shows up.
//  - Tick: WorldClick (approach to interactRange, dwell hover, press) -> wait
//    up to areaChangeTimeoutMs for OUR area counter to change -> Done. No change
//    -> re-approach with a slightly different aim and retry (maxAttempts).
//    Failure -> Failed with FailReason(); the Brain then lets FollowTask seek
//    the last position. With seekOnFailure off the task instead stays Running,
//    holding still where it is with the reason, until the leader shows up again,
//    a new vanish happens or our area changes.
//  - Done also when our area changed for any reason or the leader is in sight
//    again (FollowTask takes over: reacquire after a zone change).
//  - KeepThroughHold: Loading (the loading screen after the click is the goal)
//    and PanelUnknown (the inventory listing restarts after a zone change).
//  - AllowsCasts: Flask always; Self only while walking (never around the click
//    or while the character walks into the object after it).
#pragma once

#include "../brain/Task.h"
#include "WorldClick.h"

#include <string>

namespace fb {

// What the Travel tab's Diagnostics shows: the last vanish the task looked at
// and how it went. Written by the worker on changes, read by the UI (copy).
struct TravelDiag {
    bool         haveVanish = false;
    uint64_t     vanishAtMs = 0;
    VanishKind   kind = VanishKind::None;
    const char*  state = "";          // a key: "travel_state_following", "travel_state_arrived", ...
    LocText      detail;              // the latest step
    bool         haveObject = false;
    uint32_t     objectId = 0;
    InteractKind objectKind = InteractKind::Other;
    int          portalType = -1;
    std::string  objectName;          // metadata path without the common prefix
    float        objectDistance = -1.f;   // cells from us when it was chosen
    bool         fallbackObject = false;  // not the one he used (another of the same kind)
    int          attempts = 0;
    int          maxAttempts = 0;
    LocText      lastFailure;         // persists across travels
    uint64_t     lastFailureMs = 0;
    int          followed = 0;        // travels that arrived since the plugin loaded
    int          failed = 0;
};

class TravelTask : public Task {
public:
    const char* Name() const override { return Key("task_travel"); }

    bool WantsToStart(BotContext& ctx);
    void Start(BotContext& ctx);
    TaskStatus Tick(BotContext& ctx) override;
    void Abort(BotContext& ctx) override;
    bool AllowsCasts(CastClass c) const override;
    bool KeepThroughHold(GateReason r) const override {
        return r == GateReason::Loading || r == GateReason::PanelUnknown;
    }

    const LocText& FailReason() const { return m_fail; }

    // Thread-safe copy of the diagnostics (render thread).
    static TravelDiag Diagnostics();

private:
    enum class Phase : uint8_t { Idle = 0, Seek, Click, WaitArea, Stranded };
    enum class Walk : uint8_t { Moving = 0, Arrived, Unreachable, Stuck, NoKey, Waiting };

    TaskStatus Finish(BotContext& ctx, TaskStatus st, const LocText& why);
    TaskStatus AttemptFailed(BotContext& ctx, const LocText& why, uint64_t pauseMs = 350);
    void BeginClick(BotContext& ctx, const Interactable& it);
    void EnterSeek(BotContext& ctx);
    void NewAttemptDeadline(const BotContext& ctx);
    WorldClickTarget MakeTarget(const Interactable& it) const;
    Walk WalkToward(BotContext& ctx, const Vec2& goal, float arrival);
    void SetDetail(BotContext& ctx, LocText text);
    void PublishObject(const BotContext& ctx, const Interactable& it);

    WorldClick  m_click;
    LocText     m_fail;
    Phase       m_phase = Phase::Idle;
    // The vanish being followed.
    uint64_t     m_vanishAtMs = 0;          // the vanish this state belongs to
    uint64_t     m_finishedVanishAtMs = 0;  // ended in Done / Failed: never started again
    uint64_t     m_skipLoggedAtMs = 0;      // "not followed" logged for this vanish
    VanishKind   m_kind = VanishKind::None;
    InteractKind m_targetKind = InteractKind::Count;
    uint32_t     m_usedId = 0;              // the object he used (0 = none known)
    uint32_t     m_targetId = 0;            // the object being clicked now
    uint32_t     m_excludeId = 0;           // an object that failed: not the fallback again
    Vec2         m_anchor;                  // where his object stood / his last position
    Vec2         m_leaderLast;
    uint32_t     m_startArea = 0;
    // Progress.
    int      m_attempts = 0;                // failed cycles (approach + click + wait)
    int      m_resumes = 0;                 // restarts after an abort, same vanish
    uint64_t m_firstStartMs = 0;
    uint64_t m_phaseMs = 0;                 // when the current phase began
    uint64_t m_clickMs = 0;                 // the press that should take us through
    uint64_t m_arrivedAnchorMs = 0;         // reached his last position without seeing the object
    uint64_t m_unreachableSinceMs = 0;
    uint64_t m_retryAfterMs = 0;            // short pause between attempts
    uint64_t m_attemptDeadlineMs = 0;       // walking + finding + clicking of this attempt ends by then
    Vec2     m_aimOffset;                   // this attempt's nudge (cells)
};

}  // namespace fb
