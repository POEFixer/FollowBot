// TravelTask.cpp - see TravelTask.h. Pure decision code over World (no SDK):
// built into tests/ (test_travel.cpp drives it with a fake world).
#include "TravelTask.h"

#include "../common/BotLog.h"
#include "../common/Strings.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "../move/StuckRecovery.h"
#include "../nav/PathPlanner.h"
#include "TravelLogic.h"

#include <cstdio>
#include <mutex>
#include <string_view>

namespace fb {

namespace {

constexpr int      kMaxResumes = 8;          // restarts after an abort for one vanish
constexpr uint64_t kRetryPauseMs = 350;      // pause between two attempts
constexpr uint64_t kUnreachableRetryPauseMs = 1500;
constexpr float    kGoneNearCells = 40.f;    // a remembered object this close and unseen ...
constexpr uint64_t kGoneAfterMs = 2000;      // ... this long is gone
constexpr uint64_t kUnreachableFailMs = 4000;

std::mutex g_diagMu;
TravelDiag g_diag;

template <class F>
void EditDiag(F&& f) {
    std::lock_guard<std::mutex> lock(g_diagMu);
    f(g_diag);
}

std::string ShortName(const std::string& path) {
    static constexpr std::string_view kObjects = "Metadata/MiscellaneousObjects/";
    static constexpr std::string_view kMeta = "Metadata/";
    std::string_view p = StripAtSuffix(path);
    if (StartsWith(p, kObjects)) p.remove_prefix(kObjects.size());
    else if (StartsWith(p, kMeta)) p.remove_prefix(kMeta.size());
    return std::string(p);
}

// The object word for the detail and log lines: "transition", "boss arena
// entrance", "portal" (keys).
const char* ObjectWord(InteractKind k) {
    switch (k) {
        case InteractKind::AreaTransition: return Key("travel_word_transition");
        case InteractKind::BossArena:      return Key("travel_word_arena");
        case InteractKind::Portal:         return Key("travel_word_portal");
        default:                           return Key("travel_word_object");
    }
}

}  // namespace

TravelDiag TravelTask::Diagnostics() {
    std::lock_guard<std::mutex> lock(g_diagMu);
    return g_diag;
}

// ---------------------------------------------------------------------------
// Start conditions
// ---------------------------------------------------------------------------

bool TravelTask::WantsToStart(BotContext& ctx) {
    const World& w = ctx.w;
    const LeaderState& L = w.leader;
    const VanishInfo& v = L.vanish;
    if (!v.active || !IsFollowThroughVanish(v.kind)) return false;
    // He vanished in THIS area (our own area change since then = nothing to follow here).
    if (L.visible || L.lastSeenArea != w.gate.areaCounter) return false;
    const TravelSettings& s = ctx.p.travel;
    if (v.atMs == m_finishedVanishAtMs) {
        // Ended already; only a stranded wait (seekOnFailure off) resumes after a hold.
        return m_phase == Phase::Stranded && m_vanishAtMs == v.atMs && !s.seekOnFailure;
    }

    auto notFollowed = [&](const LocText& why) {
        if (m_skipLoggedAtMs == v.atMs) return;
        m_skipLoggedAtMs = v.atMs;
        ctx.log.Add(LogCat::Travel, TrF("log_travel_not_followed", {{"kind", Tr(VanishKindName(v.kind))}, {"why", why}}));
        EditDiag([&](TravelDiag& d) {
            d.haveVanish = true;
            d.vanishAtMs = v.atMs;
            d.kind = v.kind;
            d.state = Key("travel_state_not_followed");
            d.detail = why;
            d.haveObject = false;
            d.attempts = 0;
            d.maxAttempts = s.maxAttempts;
        });
    };

    if (!travel::KindEnabled(v.kind, s)) {
        notFollowed(TrF("travel_why_setting_off", {{"setting", Tr(travel::KindSettingName(v.kind))}}));
        return false;
    }
    if (v.kind == VanishKind::Portal) {
        // Only a portal of a plausible kind: a wrong kind read or an unset portal is not his way out.
        if (const Interactable* used = w.FindInteractable(v.interactableId)) {
            const travel::PortalVerdict pv = travel::ClassifyPortalKind(used->portalType, s.followOtherPortalKinds);
            if (pv == travel::PortalVerdict::OtherKindOff) {
                notFollowed(TrF("travel_why_other_portal", {{"kind", Tr(travel::PortalKindName(used->portalType))},
                                                            {"setting", Tr("travel_follow_other_portals")}}));
                return false;
            }
            if (pv != travel::PortalVerdict::Follow) {
                notFollowed(TrF("travel_why_portal_kind", {{"number", Num("%d", used->portalType)},
                                                           {"kind", Tr(travel::PortalKindName(used->portalType))}}));
                return false;
            }
        }
    }
    const MoverParams& mp = ctx.mover.Params();
    if (mp.moveKey <= 0 && !mp.clickToMove) {
        notFollowed(Tr("travel_why_no_move_key"));
        return false;
    }
    if (v.atMs == m_vanishAtMs && m_resumes >= kMaxResumes) {
        // Interrupted again and again (holds, preemption): stop retrying this one.
        m_finishedVanishAtMs = v.atMs;
        m_phase = Phase::Idle;
        const LocText why = Tr("travel_why_interrupted");
        ctx.log.Add(LogCat::Travel, TrF("log_travel_given_up", {{"kind", Tr(VanishKindName(v.kind))}, {"why", why}}));
        EditDiag([&](TravelDiag& d) {
            d.state = Key("travel_state_failed");
            d.lastFailure = why;
            d.lastFailureMs = ctx.now;
            ++d.failed;
        });
        return false;
    }
    return true;
}

void TravelTask::Start(BotContext& ctx) {
    const World& w = ctx.w;
    const VanishInfo& v = w.leader.vanish;
    const TravelSettings& s = ctx.p.travel;
    const bool resume = v.atMs == m_vanishAtMs && m_phase != Phase::Idle;
    m_startArea = w.gate.areaCounter;
    m_click.Abort(ctx);
    if (resume) {
        ++m_resumes;
        if (m_phase == Phase::WaitArea) {
            // The click happened: keep waiting (a timeout counts as a failed attempt in Tick).
            ctx.log.Add(LogCat::Travel, Tr("log_travel_resumed_waiting"));
        } else if (m_phase != Phase::Stranded) {
            EnterSeek(ctx);
            NewAttemptDeadline(ctx);
            ctx.log.Add(LogCat::Travel, TrF("log_travel_resumed", {{"attempt", Num("%d", m_attempts + 1)},
                                                                   {"max", Num("%d", s.maxAttempts)}}));
        }
        return;
    }

    m_vanishAtMs = v.atMs;
    m_kind = v.kind;
    m_targetKind = travel::TargetKindFor(v.kind);
    m_usedId = v.interactableId;
    m_targetId = v.interactableId;
    m_excludeId = 0;
    m_leaderLast = v.lastGrid;
    m_anchor = v.interactableId != 0 ? v.interactGrid : v.lastGrid;
    m_attempts = 0;
    m_resumes = 0;
    m_firstStartMs = ctx.now;
    m_clickMs = 0;
    m_retryAfterMs = 0;
    m_aimOffset = {};
    m_fail.clear();
    EnterSeek(ctx);
    NewAttemptDeadline(ctx);

    const float dist = Dist(w.self.grid, m_anchor);
    ctx.log.Add(LogCat::Travel, TrF("log_travel_following", {{"kind", Tr(VanishKindName(v.kind))},
                                                             {"id", Num("%u", v.interactableId)},
                                                             {"cells", Num("%.0f", dist)}}));
    EditDiag([&](TravelDiag& d) {
        d.haveVanish = true;
        d.vanishAtMs = v.atMs;
        d.kind = v.kind;
        d.state = Key("travel_state_following");
        d.detail = Tr("travel_diag_walking");
        d.haveObject = false;
        d.attempts = 0;
        d.maxAttempts = s.maxAttempts;
    });
    if (const Interactable* used = w.FindInteractable(v.interactableId)) PublishObject(ctx, *used);
}

void TravelTask::Abort(BotContext& ctx) {
    // Keeps the phase and the attempts: Start resumes the same vanish.
    m_click.Abort(ctx);
}

bool TravelTask::AllowsCasts(CastClass c) const {
    switch (c) {
        case CastClass::Flask:
            return true;
        case CastClass::Self:
            return m_phase == Phase::Seek || m_phase == Phase::Stranded ||
                   (m_phase == Phase::Click && m_click.CurrentPhase() == WorldClick::Phase::Approach);
        case CastClass::Attack:
            return m_phase == Phase::Stranded;
        case CastClass::Movement:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void TravelTask::SetDetail(BotContext& ctx, LocText text) {
    ctx.report.detail = std::move(text);
}

void TravelTask::PublishObject(const BotContext& ctx, const Interactable& it) {
    const float dist = Dist(ctx.w.self.grid, it.grid);
    const bool fallback = m_usedId != 0 ? it.id != m_usedId : true;
    EditDiag([&](TravelDiag& d) {
        d.haveObject = true;
        d.objectId = it.id;
        d.objectKind = it.kind;
        d.portalType = it.portalType;
        d.objectName = ShortName(it.path);
        d.objectDistance = dist;
        d.fallbackObject = fallback;
    });
}

void TravelTask::EnterSeek(BotContext& ctx) {
    m_phase = Phase::Seek;
    m_phaseMs = ctx.now;
    m_arrivedAnchorMs = 0;
    m_unreachableSinceMs = 0;
}

void TravelTask::NewAttemptDeadline(const BotContext& ctx) {
    const float dist = Dist(ctx.w.self.grid, m_anchor);
    m_attemptDeadlineMs = ctx.now + static_cast<uint64_t>(travel::ApproachTimeoutMs(dist)) +
                          static_cast<uint64_t>(ctx.p.travel.findObjectMs);
}

WorldClickTarget TravelTask::MakeTarget(const Interactable& it) const {
    WorldClickTarget t;
    t.id = it.id;
    t.grid = it.grid;
    t.world = Vec3{it.world.x + m_aimOffset.x * kGridToWorld, it.world.y + m_aimOffset.y * kGridToWorld, it.world.z};
    t.modelHeight = it.modelHeight;
    // A remembered object's component address may be stale: no hover check then.
    t.targetableAddr = it.visibleNow ? it.targetableAddr : 0;
    return t;
}

void TravelTask::BeginClick(BotContext& ctx, const Interactable& it) {
    const World& w = ctx.w;
    const TravelSettings& s = ctx.p.travel;
    const travel::AimVariant aim = travel::AimForAttempt(m_attempts, s.tallObjectHeightFactor, it.grid, w.self.grid);
    m_aimOffset = aim.gridOffset;
    WorldClickParams p;
    p.interactRange = s.interactRange;
    p.dwellMs = s.hoverDwellMs;
    p.holdMs = s.pressHoldMs;
    p.reHovers = s.reHovers;
    p.heightFactor = aim.heightFactor;
    p.approachTimeoutMs = travel::ApproachTimeoutMs(Dist(w.self.grid, it.grid));
    p.button = VK_LBUTTON;
    m_click.Start(MakeTarget(it), p, ctx.now);
    m_phase = Phase::Click;
    m_phaseMs = ctx.now;
    EditDiag([&](TravelDiag& d) { d.detail = Tr("travel_diag_clicking"); });
}

TravelTask::Walk TravelTask::WalkToward(BotContext& ctx, const Vec2& goal, float arrival) {
    const World& w = ctx.w;
    ctx.report.hasTarget = true;
    ctx.report.target = goal;
    if (Dist(w.self.grid, goal) <= arrival) {
        ctx.mover.Stop(ctx.act);
        ctx.stuck.NoteIdle(w, ctx.now);
        return Walk::Arrived;
    }
    if (ctx.cursorBusy) {
        ctx.mover.Stop(ctx.act);
        ctx.stuck.NoteIdle(w, ctx.now);
        return Walk::Waiting;
    }
    const PlanStatus ps = ctx.planner.Update(w.self.grid, goal, ctx.now);
    if (ps == PlanStatus::Unreachable) {
        ctx.mover.Stop(ctx.act);
        ctx.stuck.NoteIdle(w, ctx.now);
        return Walk::Unreachable;
    }
    m_unreachableSinceMs = 0;
    const RecoveryState rs =
        ctx.stuck.Tick(w, ctx.mover, ctx.planner, ctx.act, ctx.log, goal, arrival, true, ctx.now);
    if (rs == RecoveryState::GaveUp) {
        ctx.stuck.Reset(ctx.now);
        ctx.mover.Stop(ctx.act);
        return Walk::Stuck;
    }
    if (rs == RecoveryState::Recovering) return Walk::Moving;
    const MoveStatus ms = ctx.mover.Steer(w, ctx.planner.Path(), goal, ctx.act, &ctx.planner, ctx.now);
    if (ms == MoveStatus::NoKey) return Walk::NoKey;
    return Walk::Moving;
}

TaskStatus TravelTask::Finish(BotContext& ctx, TaskStatus st, const LocText& why) {
    m_click.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    m_finishedVanishAtMs = m_vanishAtMs;
    const double secs = static_cast<double>(ctx.now - m_firstStartMs) / 1000.0;
    if (st == TaskStatus::Done) {
        m_fail.clear();
        m_phase = Phase::Idle;
        const bool arrived = ctx.w.gate.areaCounter != m_startArea;
        ctx.log.Add(LogCat::Travel, TrF(arrived ? Key("log_travel_arrived") : Key("log_travel_ended"),
                                        {{"why", why}, {"seconds", Num("%.1f", secs)}}));
        EditDiag([&](TravelDiag& d) {
            d.state = arrived ? Key("travel_state_arrived") : Key("travel_state_ended");
            d.detail = why;
            if (arrived) ++d.followed;
        });
        return st;
    }
    m_fail = why;
    ctx.log.Add(LogCat::Travel, TrF("log_travel_failed", {{"seconds", Num("%.1f", secs)}, {"why", why}}));
    const bool strand = !ctx.p.travel.seekOnFailure;
    EditDiag([&](TravelDiag& d) {
        d.state = strand ? Key("travel_state_failed_staying") : Key("travel_state_failed");
        d.detail = why;
        d.lastFailure = why;
        d.lastFailureMs = ctx.now;
        ++d.failed;
    });
    if (strand) {
        // "Walk to his last position after a failed travel" is off: stay here (Running) until he shows up.
        m_phase = Phase::Stranded;
        m_phaseMs = ctx.now;
        ctx.mem.lastError = TrF("error_travel", {{"why", why}});
        ctx.log.Add(LogCat::Travel, Tr("log_travel_staying"));
        return TaskStatus::Running;
    }
    m_phase = Phase::Idle;
    return TaskStatus::Failed;
}

TaskStatus TravelTask::AttemptFailed(BotContext& ctx, const LocText& why, uint64_t pauseMs) {
    const TravelSettings& s = ctx.p.travel;
    m_click.Abort(ctx);
    ctx.mover.Stop(ctx.act);
    ++m_attempts;
    EditDiag([&](TravelDiag& d) {
        d.attempts = m_attempts;
        d.lastFailure = why;
        d.lastFailureMs = ctx.now;
    });
    if (m_attempts >= s.maxAttempts)
        return Finish(ctx, TaskStatus::Failed, TrF("travel_why_tries", {{"why", why}, {"tries", Num("%d", m_attempts)}}));
    ctx.log.Add(LogCat::Travel, TrF("log_travel_attempt_failed", {{"attempt", Num("%d", m_attempts)},
                                                                  {"max", Num("%d", s.maxAttempts)},
                                                                  {"why", why}}));
    m_retryAfterMs = ctx.now + pauseMs;
    EnterSeek(ctx);
    NewAttemptDeadline(ctx);
    m_attemptDeadlineMs += pauseMs;
    ctx.planner.Clear();
    return TaskStatus::Running;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

TaskStatus TravelTask::Tick(BotContext& ctx) {
    const World& w = ctx.w;
    const LeaderState& L = w.leader;
    const TravelSettings& s = ctx.p.travel;

    // Our area changed (the click worked, or anything else moved us): FollowTask
    // reacquires the leader there.
    if (w.gate.areaCounter != m_startArea) {
        return Finish(ctx, TaskStatus::Done,
                      w.gate.areaId.empty() ? Tr("travel_now_in_new_area")
                                            : TrF("travel_now_in", {{"area", w.gate.areaId}}));
    }
    // He is in sight again: nothing to follow (also ends a stranded wait).
    if (L.visible) return Finish(ctx, TaskStatus::Done, Tr("travel_leader_in_sight"));

    if (m_phase == Phase::Stranded) {
        ctx.stuck.NoteIdle(w, ctx.now);
        SetDetail(ctx, TrF("travel_detail_stranded", {{"why", m_fail}}));
        return TaskStatus::Running;
    }
    if (!w.self.valid) {
        ctx.stuck.NoteIdle(w, ctx.now);
        SetDetail(ctx, Tr("travel_detail_waiting_game"));
        return TaskStatus::Running;
    }
    if (ctx.now < m_retryAfterMs) {
        ctx.stuck.NoteIdle(w, ctx.now);
        SetDetail(ctx, TrF("travel_detail_retry", {{"attempt", Num("%d", m_attempts + 1)}, {"max", Num("%d", s.maxAttempts)}}));
        return TaskStatus::Running;
    }

    const LocText word = Tr(ObjectWord(m_targetKind));
    ctx.report.markers.push_back({m_leaderLast, 0xFF40C0FFu, Key("marker_his_last")});

    if (m_phase == Phase::WaitArea) {
        ctx.stuck.NoteIdle(w, ctx.now);
        const uint64_t waited = ctx.now - m_clickMs;
        if (waited >= static_cast<uint64_t>(s.areaChangeTimeoutMs))
            return AttemptFailed(ctx, Tr("travel_why_no_area_change"));
        SetDetail(ctx, TrF("travel_detail_clicked_waiting", {{"object", word},
                                                             {"seconds", Num("%.1f", static_cast<double>(waited) / 1000.0)}}));
        return TaskStatus::Running;
    }

    // Walking, finding and clicking of one attempt are bounded together (the
    // object may drop in and out of the entity list while we walk).
    if (ctx.now > m_attemptDeadlineMs && !m_click.CursorCritical())
        return AttemptFailed(ctx, TrF("travel_why_not_in_time", {{"object", word}}));

    // The object to click, re-resolved every tick.
    const Interactable* t =
        travel::ResolveTarget(w.interactables, m_targetId, m_targetKind, m_leaderLast, s.fallbackRadius, s, m_excludeId);
    if (t && t->id != m_targetId) {
        m_targetId = t->id;
        ctx.log.Add(LogCat::Travel, TrF(t->id == m_usedId ? Key("log_travel_using_his") : Key("log_travel_using_other"),
                                        {{"object", word},
                                         {"id", Num("%u", t->id)},
                                         {"cells", Num("%.0f", Dist(t->grid, m_leaderLast))}}));
        PublishObject(ctx, *t);
        if (m_phase == Phase::Click) {
            m_click.Abort(ctx);
            EnterSeek(ctx);
        }
    } else if (!t && m_targetId != 0) {
        // The chosen one is no longer usable (closed, implausible) and no other is near.
        ctx.log.Add(LogCat::Travel, TrF("log_travel_unusable", {{"object", word}, {"id", Num("%u", m_targetId)}}));
        m_excludeId = m_targetId;
        m_targetId = 0;
        if (m_phase == Phase::Click) {
            m_click.Abort(ctx);
            EnterSeek(ctx);
        }
    }

    if (t) ctx.report.markers.push_back({t->grid, 0xFF00FFFFu, Key("task_travel")});

    // A clickable object in our entity list: approach and click it.
    if (m_phase != Phase::Click && t && t->visibleNow) BeginClick(ctx, *t);
    if (m_phase == Phase::Click && t) {
        if (!t->visibleNow && !m_click.CursorCritical()) {
            // Left our entity list while we walked: walk to where it was.
            m_click.Abort(ctx);
            EnterSeek(ctx);
        } else {
            m_click.UpdateTarget(MakeTarget(*t));
            const WorldClickStatus st = m_click.Tick(ctx);
            if (st == WorldClickStatus::Clicked) {
                m_clickMs = ctx.now;
                m_phase = Phase::WaitArea;
                ctx.log.Add(LogCat::Travel, TrF("log_travel_clicked", {{"object", word},
                                                                       {"id", Num("%u", t->id)},
                                                                       {"attempt", Num("%d", m_attempts + 1)},
                                                                       {"max", Num("%d", s.maxAttempts)}}));
                EditDiag([&](TravelDiag& d) { d.detail = Tr("travel_diag_clicked"); });
                SetDetail(ctx, TrF("travel_detail_clicked", {{"object", word}}));
                return TaskStatus::Running;
            }
            if (st == WorldClickStatus::Failed) {
                const std::string why = m_click.FailReason();
                if (why == Key("click_fail_no_move_key")) return Finish(ctx, TaskStatus::Failed, Tr("travel_why_no_move_key"));
                // No path right now may be a passing block: give the planner time before the next try.
                const bool noPath = why == Key("click_fail_unreachable");
                return AttemptFailed(ctx, why.empty() ? Tr("travel_why_click_failed") : Tr(why.c_str()),
                                     noPath ? kUnreachableRetryPauseMs : kRetryPauseMs);
            }
            if (m_click.CursorCritical()) SetDetail(ctx, TrF("travel_detail_clicking", {{"object", word}}));
            else
                SetDetail(ctx, TrF("travel_detail_walking", {{"object", word},
                                                             {"cells", Num("%.0f", Dist(w.self.grid, t->grid))}}));
            return TaskStatus::Running;
        }
    }
    if (m_phase != Phase::Seek) EnterSeek(ctx);

    // --- Seek: no clickable object in our entity list ---------------------------
    Vec2 goal = m_anchor;
    float arrival = (std::max)(3.f, s.interactRange);
    if (t) {
        // Remembered from earlier: walk to where it stood until it shows up again.
        if (travel::LooksGone(*t, w.self.grid, ctx.now, kGoneNearCells, kGoneAfterMs)) {
            ctx.log.Add(LogCat::Travel, TrF("log_travel_gone", {{"object", word}, {"id", Num("%u", t->id)}}));
            m_excludeId = t->id;
            m_targetId = 0;
            SetDetail(ctx, TrF("travel_detail_gone", {{"object", word}}));
            return TaskStatus::Running;
        }
        goal = t->grid;
        arrival = s.interactRange;
    }
    const Walk wr = WalkToward(ctx, goal, arrival);
    switch (wr) {
        case Walk::NoKey:
            return Finish(ctx, TaskStatus::Failed, Tr("travel_why_no_move_key"));
        case Walk::Stuck:
            return AttemptFailed(ctx, Tr("click_fail_stuck"));
        case Walk::Unreachable:
            if (m_unreachableSinceMs == 0) m_unreachableSinceMs = ctx.now;
            if (ctx.now - m_unreachableSinceMs >= kUnreachableFailMs)
                return AttemptFailed(ctx, TrF("travel_why_no_path", {{"object", word}}));
            SetDetail(ctx, TrF("travel_detail_no_path_yet", {{"object", word}}));
            return TaskStatus::Running;
        case Walk::Waiting:
            SetDetail(ctx, Tr("travel_detail_cursor_busy"));
            return TaskStatus::Running;
        case Walk::Arrived:
            if (m_arrivedAnchorMs == 0) m_arrivedAnchorMs = ctx.now;
            if (ctx.now - m_arrivedAnchorMs >= static_cast<uint64_t>(s.findObjectMs))
                return Finish(ctx, TaskStatus::Failed, TrF("travel_why_not_found", {{"object", word}}));
            SetDetail(ctx, TrF("travel_detail_looking", {{"object", word}}));
            return TaskStatus::Running;
        case Walk::Moving:
            break;
    }
    m_arrivedAnchorMs = 0;
    SetDetail(ctx, TrF("travel_detail_walking_his", {{"object", word}, {"cells", Num("%.0f", Dist(w.self.grid, goal))}}));
    return TaskStatus::Running;
}

}  // namespace fb
