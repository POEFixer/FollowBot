// PortalTrip.cpp - see PortalTrip.h. Pure: no SDK.
#include "PortalTrip.h"

#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"

#include <algorithm>
#include <cstdio>

namespace fb {

namespace {

constexpr int      kMaxPresses = 2;            // the portal key, when no portal shows up
constexpr float    kReturnRadius = 40.f;       // cells around the arrival point
constexpr float    kReturnRadiusResume = 80.f; // cells around us (no arrival point)
constexpr uint64_t kArrivalSettleMs = 2000;    // wait this long for a valid own position after arriving
constexpr int      kPressRetryMs = 3000;       // keep retrying a refused key press this long
constexpr int      kApproachTimeoutMs = 15000;

// MultiplexPortal "portal" state: 0 not set yet, -1 unread (unknown).
bool UnknownType(int t) { return t <= 0; }
bool LeaveType(int t) { return UnknownType(t) || t == 1 || t == 2; }            // Player, Town
bool ReturnType(int t) { return UnknownType(t) || t == 1 || t == 2 || t == 4; } // + Map Return

WorldClickParams ClickParams(const BotContext& ctx) {
    const TravelSettings& t = ctx.p.travel;
    WorldClickParams wp;
    wp.interactRange = t.interactRange;
    wp.dwellMs = t.hoverDwellMs;
    wp.holdMs = t.pressHoldMs;
    wp.reHovers = t.reHovers;
    wp.heightFactor = t.tallObjectHeightFactor;
    wp.approachTimeoutMs = kApproachTimeoutMs;
    return wp;
}

WorldClickTarget TargetOf(const Interactable& it) {
    WorldClickTarget t;
    t.id = it.id;
    t.grid = it.grid;
    t.world = it.world;
    t.modelHeight = it.modelHeight;
    t.targetableAddr = it.targetableAddr;
    return t;
}

}  // namespace

const Interactable* PortalTrip::FindNewPortal(const std::vector<Interactable>& list, const std::vector<uint32_t>& before,
                                              const Vec2& self, float radius) {
    const Interactable* best = nullptr;
    float bestD = radius;
    for (const auto& it : list) {
        if (it.kind != InteractKind::Portal || !LeaveType(it.portalType)) continue;
        if (std::find(before.begin(), before.end(), it.id) != before.end()) continue;
        const float d = Dist(it.grid, self);
        if (d <= bestD) {
            bestD = d;
            best = &it;
        }
    }
    return best;
}

const Interactable* PortalTrip::FindReturnPortal(const std::vector<Interactable>& list, const Vec2& from, float radius) {
    const Interactable* best = nullptr;
    int bestScore = -1;
    float bestD = 0.f;
    for (const auto& it : list) {
        if (it.kind != InteractKind::Portal || !ReturnType(it.portalType)) continue;
        const float d = Dist(it.grid, from);
        if (d > radius) continue;
        const int score = (it.targetable ? 2 : 0) + (UnknownType(it.portalType) ? 0 : 1);
        if (!best || score > bestScore || (score == bestScore && d < bestD)) {
            best = &it;
            bestScore = score;
            bestD = d;
        }
    }
    return best;
}

const char* PortalTrip::PhaseText() const {
    switch (m_phase) {
        case Phase::Idle:       return "";
        case Phase::Press:      return "opening a portal";
        case Phase::WaitPortal: return "waiting for the portal to appear";
        case Phase::FindReturn: return "looking for the portal back";
        case Phase::Click:
            return m_click.CurrentPhase() == WorldClick::Phase::Approach ? "walking to the portal" : "clicking the portal";
        case Phase::WaitArea:   return "waiting for the area to change";
        case Phase::Done:       return m_returning ? "back in the map" : "arrived";
        case Phase::Failed:     return "failed";
    }
    return "";
}

TripStatus PortalTrip::Fail(BotContext& ctx, const std::string& why, TripStatus st) {
    m_click.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    m_fail = why;
    m_phase = Phase::Failed;
    m_failStatus = st;
    ctx.log.Add(LogCat::Stash, "Portal trip %s: %s", st == TripStatus::Stranded ? "stranded" : "failed", why.c_str());
    return st;
}

void PortalTrip::Abort(BotContext& ctx) {
    m_click.Abort(ctx);
    if (m_phase != Phase::Done && m_phase != Phase::Failed) m_phase = Phase::Idle;
}

void PortalTrip::BeginLeave(BotContext& ctx) {
    const World& w = ctx.w;
    m_fail.clear();
    m_phaseMs = ctx.now;
    m_startArea = w.gate.areaCounter;
    m_phase = Phase::Press;
    m_returning = false;
    m_before.clear();
    m_portalId = 0;
    m_presses = 0;
    m_clicks = 0;
    m_clickMs = 0;
    m_haveArrival = false;
    m_failStatus = TripStatus::Failed;
    BotMemory::StashTrip& t = ctx.mem.trip;
    t.active = true;
    t.originArea = w.gate.areaCounter;
    t.originAreaId = w.gate.areaId;
    t.originGrid = w.self.grid;
    t.startedMs = ctx.now;
    ctx.log.Add(LogCat::Stash, "Portal trip: leaving %s to stash", w.gate.areaId.empty() ? "the map" : w.gate.areaId.c_str());
}

void PortalTrip::BeginReturn(BotContext& ctx) {
    const World& w = ctx.w;
    m_click.Abort(ctx);
    m_fail.clear();
    m_phaseMs = ctx.now;
    m_startArea = w.gate.areaCounter;
    m_phase = Phase::FindReturn;
    m_returning = true;
    m_portalId = 0;
    m_clicks = 0;
    m_clickMs = 0;
    m_failStatus = TripStatus::Stranded;
    if (m_haveArrival && m_arrivalArea != w.gate.areaCounter) m_haveArrival = false;   // from another visit
    if (!m_haveArrival) ctx.log.Add(LogCat::Stash, "Portal trip: returning through the portal nearest to us");
}

void PortalTrip::StartClick(BotContext& ctx, const Interactable& portal) {
    m_portalId = portal.id;
    m_clicks = 0;
    m_phase = Phase::Click;
    m_phaseMs = ctx.now;
    m_click.Start(TargetOf(portal), ClickParams(ctx), ctx.now);
    ctx.log.Add(LogCat::Stash, "Portal trip: portal %u at (%.0f, %.0f), type %d%s", portal.id, portal.grid.x, portal.grid.y,
                portal.portalType, m_returning ? " (the way back)" : "");
}

TripStatus PortalTrip::TickLeave(BotContext& ctx) {
    const World& w = ctx.w;
    const StashSettings& s = ctx.p.stash;
    ctx.req.interactablesNow = true;
    const bool enemies = s.tripNoEnemyRadius > 0.f && w.CountMonstersNear(w.self.grid, s.tripNoEnemyRadius) > 0;
    switch (m_phase) {
        case Phase::Press: {
            if (s.portalKey == 0) {
                ctx.mem.trip = BotMemory::StashTrip{};
                return Fail(ctx, "no portal key set", TripStatus::Failed);
            }
            if (enemies) {
                ctx.mem.trip = BotMemory::StashTrip{};
                return Fail(ctx, "enemies came close before the portal opened", TripStatus::Failed);
            }
            ctx.mover.Stop(ctx.act);
            m_before.clear();
            for (const auto& it : w.interactables)
                if (it.kind == InteractKind::Portal) m_before.push_back(it.id);
            const ActResult r = ctx.act.TapKey(s.portalKey, 40);
            if (r != ActResult::Ok) {
                if (ctx.now - m_phaseMs > static_cast<uint64_t>(kPressRetryMs)) {
                    ctx.mem.trip = BotMemory::StashTrip{};
                    return Fail(ctx, std::string("could not press the portal key (") + ActResultName(r) + ")",
                                TripStatus::Failed);
                }
                return TripStatus::Running;
            }
            ++m_presses;
            m_phase = Phase::WaitPortal;
            m_phaseMs = ctx.now;
            ctx.log.Add(LogCat::Stash, "Portal trip: pressed the portal key (%d/%d)", m_presses, kMaxPresses);
            return TripStatus::Running;
        }
        case Phase::WaitPortal: {
            if (enemies) {
                ctx.mem.trip = BotMemory::StashTrip{};
                return Fail(ctx, "enemies came close before the portal opened", TripStatus::Failed);
            }
            if (const Interactable* p = FindNewPortal(w.interactables, m_before, w.self.grid, s.portalSearchCells)) {
                StartClick(ctx, *p);
                return TripStatus::Running;
            }
            if (ctx.now - m_phaseMs >= static_cast<uint64_t>(s.portalWaitMs)) {
                if (m_presses < kMaxPresses) {
                    ctx.log.Add(LogCat::Stash, "Portal trip: no portal after %d ms, pressing the key again", s.portalWaitMs);
                    m_phase = Phase::Press;
                    m_phaseMs = ctx.now;
                    return TripStatus::Running;
                }
                ctx.mem.trip = BotMemory::StashTrip{};
                return Fail(ctx,
                            "no portal appeared after pressing the portal key twice (is the key bound to the portal "
                            "skill in the game, and can a portal be opened here?)",
                            TripStatus::Failed);
            }
            return TripStatus::Running;
        }
        case Phase::Click:
        case Phase::WaitArea:
            return TickClickAndArea(ctx);
        case Phase::Done:
            return TripStatus::Arrived;
        case Phase::Failed:
            return m_failStatus;
        case Phase::Idle:
        case Phase::FindReturn:
            break;
    }
    return Fail(ctx, "the trip was not started", TripStatus::Failed);
}

TripStatus PortalTrip::TickReturn(BotContext& ctx) {
    const World& w = ctx.w;
    switch (m_phase) {
        case Phase::FindReturn: {
            ctx.req.interactablesNow = true;
            const Vec2 from = m_haveArrival ? m_arrival : w.self.grid;
            const float radius = m_haveArrival ? kReturnRadius : kReturnRadiusResume;
            if (const Interactable* p = FindReturnPortal(w.interactables, from, radius)) {
                StartClick(ctx, *p);
                return TripStatus::Running;
            }
            const int waitMs = (std::max)(ctx.p.stash.portalWaitMs, 3000);
            if (ctx.now - m_phaseMs >= static_cast<uint64_t>(waitMs))
                return Fail(ctx, "no portal back found near the arrival point", TripStatus::Stranded);
            return TripStatus::Running;
        }
        case Phase::Click:
        case Phase::WaitArea:
            return TickClickAndArea(ctx);
        case Phase::Done:
            return TripStatus::Arrived;
        case Phase::Failed:
            return m_failStatus;
        case Phase::Idle:
        case Phase::Press:
        case Phase::WaitPortal:
            break;
    }
    return Fail(ctx, "the way back was not started", TripStatus::Stranded);
}

TripStatus PortalTrip::TickClickAndArea(BotContext& ctx) {
    const World& w = ctx.w;
    const TravelSettings& t = ctx.p.travel;
    const TripStatus failAs = m_returning ? TripStatus::Stranded : TripStatus::Failed;
    char buf[200];

    if (m_phase == Phase::Click && w.gate.areaCounter == m_startArea) {
        const Interactable* p = w.FindInteractable(m_portalId);
        if (!p) {
            if (!m_returning) ctx.mem.trip = BotMemory::StashTrip{};
            return Fail(ctx, "the portal disappeared", failAs);
        }
        m_click.UpdateTarget(TargetOf(*p));
        const WorldClickStatus st = m_click.Tick(ctx);
        if (st == WorldClickStatus::Running) return TripStatus::Running;
        ++m_clicks;
        if (st == WorldClickStatus::Failed) {
            if (m_clicks >= t.maxAttempts) {
                if (!m_returning) ctx.mem.trip = BotMemory::StashTrip{};
                return Fail(ctx, "could not click the portal: " + m_click.FailReason(), failAs);
            }
            m_click.Start(TargetOf(*p), ClickParams(ctx), ctx.now);   // a fresh approach
            return TripStatus::Running;
        }
        m_clickMs = ctx.now;
        m_phase = Phase::WaitArea;
        return TripStatus::Running;
    }
    if (m_phase == Phase::Click) m_phase = Phase::WaitArea;   // a late click landed: the area already changed

    // Wait for our area to change (the loading screen holds the bot; the
    // counter is checked before any timeout when ticks resume).
    if (w.gate.areaCounter != m_startArea) {
        if (!m_returning) {
            if (!w.gate.isTown && !w.gate.isHideout) {
                ctx.mem.trip = BotMemory::StashTrip{};
                std::snprintf(buf, sizeof(buf), "the portal led to %s, not to a town or hideout",
                              w.gate.areaId.empty() ? "another area" : w.gate.areaId.c_str());
                return Fail(ctx, buf, TripStatus::Failed);
            }
            if (!w.self.valid && ctx.now - m_clickMs < kArrivalSettleMs) return TripStatus::Running;
            m_arrival = w.self.grid;
            m_haveArrival = w.self.valid;
            m_arrivalArea = w.gate.areaCounter;
            m_phase = Phase::Done;
            ctx.log.Add(LogCat::Stash, "Portal trip: arrived in %s", w.gate.areaId.c_str());
            return TripStatus::Arrived;
        }
        const BotMemory::StashTrip& trip = ctx.mem.trip;
        if (!trip.originAreaId.empty() && trip.originAreaId != w.gate.areaId)
            ctx.log.Add(LogCat::Stash, "Portal trip: returned to %s (the trip started in %s)", w.gate.areaId.c_str(),
                        trip.originAreaId.c_str());
        else
            ctx.log.Add(LogCat::Stash, "Portal trip: back in %s", w.gate.areaId.c_str());
        ctx.mem.trip = BotMemory::StashTrip{};
        m_phase = Phase::Done;
        return TripStatus::Arrived;
    }
    if (ctx.now - m_clickMs >= static_cast<uint64_t>(t.areaChangeTimeoutMs)) {
        const Interactable* p = w.FindInteractable(m_portalId);
        if (m_clicks >= t.maxAttempts || !p) {
            if (!m_returning) ctx.mem.trip = BotMemory::StashTrip{};
            std::snprintf(buf, sizeof(buf), "the area did not change after clicking the portal %d time%s", m_clicks,
                          m_clicks == 1 ? "" : "s");
            return Fail(ctx, buf, failAs);
        }
        ctx.log.Add(LogCat::Stash, "Portal trip: the area did not change, clicking the portal again");
        m_click.Start(TargetOf(*p), ClickParams(ctx), ctx.now);
        m_phase = Phase::Click;
    }
    return TripStatus::Running;
}

}  // namespace fb
