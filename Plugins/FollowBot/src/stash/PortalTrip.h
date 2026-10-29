// PortalTrip.h - leave the map through an own portal and come back
// (design §5.11). A helper StashTask drives for the leave-map trip. Pure: no
// SDK (the portal key press goes through the Actuator, the click through
// WorldClick).
//
// Contract (this interface is final):
//  - BeginLeave: remember the origin (area counter, id, position) in
//    mem.trip; press portalKey.
//  - TickLeave: wait <= portalWaitMs for a NEW Portal interactable within
//    portalSearchCells (portalType 1 / 2 or unknown, not seen before the press)
//    -> WorldClick -> our area changes -> Arrived (in town / hideout).
//  - BeginReturn / TickReturn: the portal nearest to our arrival point
//    (re-resolved; portalType 1 / 2 / 4 or unknown) -> WorldClick -> area
//    change -> Arrived (back in the map; mem.trip cleared).
//  - Any failure -> Failed with FailReason(); stranded in town / hideout ->
//    Stranded ("take the portal back").
//
// Details: the key is pressed on the first TickLeave (inside a tick, where the
// actuator's checks apply), at most twice when no portal shows up; enemies
// within tripNoEnemyRadius before the portal is clicked cancel the trip; each
// portal is clicked at most travel.maxAttempts times (a fresh approach before
// each), and the area must change within travel.areaChangeTimeoutMs of a
// click. A portal that led somewhere other than a town / hideout fails the
// trip (mem.trip cleared). The return portal ranks targetable before not,
// a known type (1 / 2 / 4) before unknown, then the nearest to the arrival
// point; a trip resumed without an arrival point (the task was interrupted)
// measures from where we stand.
#pragma once

#include "../brain/Task.h"
#include "../travel/WorldClick.h"

#include <string>
#include <vector>

namespace fb {

enum class TripStatus : uint8_t { Running = 0, Arrived, Failed, Stranded };

class PortalTrip {
public:
    void BeginLeave(BotContext& ctx);
    TripStatus TickLeave(BotContext& ctx);
    void BeginReturn(BotContext& ctx);
    TripStatus TickReturn(BotContext& ctx);
    void Abort(BotContext& ctx);

    bool CursorCritical() const { return m_click.CursorCritical(); }
    const LocText& FailReason() const { return m_fail; }

    // --- additions: status for the UI / report, and the pure selectors ---
    // What the trip does right now: a key ("" when idle).
    const char* PhaseText() const;
    bool Returning() const { return m_returning; }
    uint32_t PortalId() const { return m_portalId; }

    // A portal that appeared after the key press: kind Portal, id not in
    // `before`, within `radius` cells of `self`, portalType 1 / 2 or unknown.
    // Nearest first; nullptr when none.
    static const Interactable* FindNewPortal(const std::vector<Interactable>& list, const std::vector<uint32_t>& before,
                                             const Vec2& self, float radius);
    // The portal back: kind Portal with portalType 1 / 2 / 4 or unknown, within
    // `radius` cells of `from`; targetable first, then a known type, then the
    // nearest. nullptr when none.
    static const Interactable* FindReturnPortal(const std::vector<Interactable>& list, const Vec2& from, float radius);

private:
    enum class Phase : uint8_t { Idle = 0, Press, WaitPortal, FindReturn, Click, WaitArea, Done, Failed };

    TripStatus Fail(BotContext& ctx, const LocText& why, TripStatus st);
    TripStatus TickClickAndArea(BotContext& ctx);
    void StartClick(BotContext& ctx, const Interactable& portal);

    WorldClick  m_click;
    LocText     m_fail;
    uint64_t    m_phaseMs = 0;
    uint32_t    m_startArea = 0;
    // Additions.
    Phase       m_phase = Phase::Idle;
    bool        m_returning = false;
    std::vector<uint32_t> m_before;     // portal ids known when the key was pressed
    uint32_t    m_portalId = 0;
    int         m_presses = 0;
    int         m_clicks = 0;
    uint64_t    m_clickMs = 0;
    Vec2        m_arrival;              // where we stood right after arriving in town / hideout
    bool        m_haveArrival = false;
    uint32_t    m_arrivalArea = 0;      // the area counter m_arrival belongs to
    TripStatus  m_failStatus = TripStatus::Failed;
};

}  // namespace fb
