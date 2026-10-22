// Brain.h - decides, every tick, what the bot does (design §5.2). First match
// wins; an exclusive task keeps running until Done / Failed or a higher layer
// preempts it:
//   1 Gates          -> hold (release keys, publish the reason)
//   2 Death          preempts everything (and "Dead" holds while no death task runs)
//   3 Exclusive task in progress (Travel, Stash trip, Pickup, Death, debug walk);
//                    a leader vanish to follow through preempts Pickup
//   4 Travel trigger
//   5 Stash trigger
//   6 Skill cast     (evaluated every tick, interleaved; a cast skips steering this tick)
//   7 Loot trigger
//   8 Follow
//   9 Idle near the leader (Follow releases the move key)
#pragma once

#include "../combat/SkillEngine.h"
#include "../death/DeathTask.h"
#include "../loot/PickupTask.h"
#include "../stash/StashTask.h"
#include "../travel/TravelTask.h"
#include "BotView.h"
#include "FollowTask.h"
#include "Gates.h"
#include "Task.h"

#include <string>

namespace fb {

// What the render thread asked for this tick.
struct BrainInputs {
    bool     running = false;
    bool     paused = false;
    bool     overrideHeld = false;
    uint64_t overrideReleasedMs = 0;
    uint64_t lastUserActivityMs = 0;
    bool     stashNow = false;           // edge: the stash hotkey / button
    int      debugWalkKind = -1;         // edge: InteractKind to walk to, -1 = none
    bool     debugWalkClick = true;
    bool     debugCancel = false;        // edge: stop a debug walk
};

struct BrainOutput {
    GateReason  gate = GateReason::None;
    bool        stopBot = false;         // the plugin must stop (limit reached, death limit, ...)
    std::string stopReason;
};

class Brain {
public:
    void Tick(BotContext& ctx, const BrainInputs& in, BrainOutput& out);
    // Start / stop of the bot: abort every task, release input, reset memory.
    void OnStart(BotContext& ctx);
    void OnStop(BotContext& ctx);

    void FillView(BotView& v, const BotContext& ctx) const;

    GateReason LastGate() const { return m_gate; }
    const char* ActiveTaskName() const { return m_exclusive ? m_exclusive->Name() : ""; }
    // A debug walk runs or waits to start (input is allowed for it while stopped).
    bool DebugWalkActive() const { return m_exclusive == &m_walk || m_pendingWalk >= 0; }
    const std::string& DebugResult() const { return m_debugResult; }
    void SetDebugResult(std::string s) { m_debugResult = std::move(s); }

    const FollowTask&  Follow() const { return m_follow; }
    const SkillEngine& Skills() const { return m_skills; }
    const PickupTask&  Pickup() const { return m_pickup; }

private:
    void Hold(BotContext& ctx, GateReason why, bool closePanel);
    void StartExclusive(BotContext& ctx, Task* t);
    void EndExclusive(BotContext& ctx, TaskStatus st);
    void AbortExclusive(BotContext& ctx);
    void TrackArea(BotContext& ctx);

    DeathTask   m_death;
    TravelTask  m_travel;
    StashTask   m_stash;
    PickupTask  m_pickup;
    FollowTask  m_follow;
    WalkToTask  m_walk;
    SkillEngine m_skills;

    Task*       m_exclusive = nullptr;
    int         m_pendingWalk = -1;        // InteractKind of a requested debug walk not started yet
    bool        m_pendingWalkClick = true;
    GateReason  m_gate = GateReason::Stopped;
    bool        m_deadHold = false;
    uint64_t    m_foreignPanelSinceMs = 0;
    uint64_t    m_lastEscMs = 0;
    int         m_exitTries = 0;           // emergency exit: /exit attempts the action gate deferred
    std::string m_lastMapId;               // the map instance the per-map death count belongs to
    std::string m_lastMapHash;
    std::string m_debugResult;
};

}  // namespace fb
