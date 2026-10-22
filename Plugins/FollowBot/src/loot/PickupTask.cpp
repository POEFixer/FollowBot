// PickupTask.cpp - see PickupTask.h. Pure (no SDK): the game is reached
// through the scanner's host and BotContext.
#include "PickupTask.h"

#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "LootScanCore.h"

#include <cstdio>

namespace fb {

namespace {

// Overlay marker colours (ImGui ABGR).
constexpr uint32_t kColTarget = 0xFF3CDCFFu;   // amber: the item being picked up
constexpr uint32_t kColWanted = 0xFF78E650u;   // green: wanted
constexpr uint32_t kColNoRoom = 0xFF2896FFu;   // orange: wanted, no room
constexpr uint32_t kColSkip   = 0xFF5050FFu;   // red: gave up on it for a while
constexpr size_t   kMaxMarkers = 24;
constexpr int      kPreemptCooldownMs = 1000;
constexpr uint64_t kLeaderLostGraceMs = 500;   // the leader missing from one read is not "gone"

const GroundItem* FindItem(const World& w, uint32_t id) {
    for (const auto& gi : w.items)
        if (gi.id == id) return &gi;
    return nullptr;
}

bool EnemiesNear(const World& w, const LootSettings& s, const Vec2& at) {
    return !s.lootDuringCombat && s.enemyRadius > 0.f && w.CountMonstersNear(at, s.enemyRadius) > 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// VanishWatch
// ---------------------------------------------------------------------------

void VanishWatch::Start(uint64_t clickMs, float distToItem, int waitMs, int movingWaitMs) {
    const uint64_t wait = static_cast<uint64_t>(waitMs > 0 ? waitMs : 0);
    const uint64_t moving = static_cast<uint64_t>(movingWaitMs > waitMs ? movingWaitMs : waitMs);
    m_clickMs = clickMs;
    m_deadlineMs = clickMs + wait;
    m_capMs = clickMs + moving;
    m_lastApproachMs = 0;
    m_absentSinceMs = 0;
    m_minDist = distToItem;
}

VanishWatch::Result VanishWatch::Update(uint64_t now, bool present, bool listEmpty, float distToItem) {
    if (!present) {
        if (!listEmpty) return Result::Vanished;
        if (m_absentSinceMs == 0) m_absentSinceMs = now;
        return now - m_absentSinceMs >= kEmptyListConfirmMs ? Result::Vanished : Result::Pending;
    }
    m_absentSinceMs = 0;
    if (distToItem < m_minDist - kApproachStepCells) {
        // Still walking to it (the click made the character go there): give the
        // pickup time to land after the last step, up to the moving cap.
        m_minDist = distToItem;
        m_lastApproachMs = now;
        m_deadlineMs = (std::min)(m_capMs, (std::max)(m_deadlineMs, now + kAfterApproachMs));
    }
    return now > m_deadlineMs ? Result::TimedOut : Result::Pending;
}

// ---------------------------------------------------------------------------
// PickupTask
// ---------------------------------------------------------------------------

void PickupTask::TrackArea(BotContext& ctx) {
    if (!m_haveArea || ctx.w.gate.areaCounter != m_area) {
        m_area = ctx.w.gate.areaCounter;
        m_haveArea = true;
        m_attempts.clear();   // entity ids are per area
        m_cooldownUntilMs = 0;
    }
    if (ctx.mem.startedMs != m_startedSession) {   // the bot (re)started: BotMemory was reset
        m_startedSession = ctx.mem.startedMs;
        m_pickedTotal.store(0, std::memory_order_relaxed);
        m_fullAtFreeCells = -1;
    }
}

void PickupTask::UpdateFullFlag(BotContext& ctx) {
    if (!ctx.mem.inventoryFull) {
        m_fullAtFreeCells = -1;
        return;
    }
    // Set by us: cleared once the backpack has more room than when it was set.
    if (m_fullAtFreeCells >= 0 && ctx.w.inv.valid && m_scanner.FreeCells() > m_fullAtFreeCells) {
        ctx.mem.inventoryFull = false;
        ctx.log.Add(LogCat::Loot, "Backpack has room again (%d free cells)", m_scanner.FreeCells());
        m_fullAtFreeCells = -1;
    }
}

const char* PickupTask::BlockReason(const BotContext& ctx) const {
    const World& w = ctx.w;
    const LootSettings& s = ctx.p.loot;
    if (!s.enabled && !s.alwaysQuestItems) return "loot pickup is off";
    if (!w.self.valid || w.self.dead) return "no character";
    if ((w.gate.isTown || w.gate.isHideout) && !s.lootInTownHideout) return "not in town and hideout (setting)";
    if (ctx.now < m_cooldownUntilMs) return "following first for a moment";
    if (w.leader.configured) {
        if (!w.leader.visible) return "leader not in sight";
        if (Dist(w.self.grid, w.leader.info.grid) > s.maxLeaderDistance) return "leader too far";
    }
    if (EnemiesNear(w, s, w.self.grid)) return "enemies nearby";
    return nullptr;
}

const char* PickupTask::PreemptReason(const BotContext& ctx) const {
    const World& w = ctx.w;
    const LootSettings& s = ctx.p.loot;
    if (!s.enabled && !s.alwaysQuestItems) return "loot pickup was turned off";
    if (!w.self.valid || w.self.dead) return "no character";
    if ((w.gate.isTown || w.gate.isHideout) && !s.lootInTownHideout) return "in town or hideout";
    if (w.leader.configured) {
        if (!w.leader.visible) {
            if (w.leader.lastSeenMs == 0 || ctx.now - w.leader.lastSeenMs > kLeaderLostGraceMs) return "the leader left sight";
        } else if (Dist(w.self.grid, w.leader.info.grid) > s.maxLeaderDistance + s.preemptLeaderExtra) {
            return "the leader is too far";
        }
    }
    if (EnemiesNear(w, s, w.self.grid)) return "enemies nearby";
    return nullptr;
}

const LootCandidate* PickupTask::ChooseTarget(const BotContext& ctx, bool* anyNoRoom) const {
    const World& w = ctx.w;
    const LootSettings& s = ctx.p.loot;
    for (const LootCandidate& c : m_scanner.Candidates()) {   // wanted, not blacklisted, nearest first
        if (ctx.mem.IsBlacklisted(c.item.id, ctx.now)) continue;
        const auto at = m_attempts.find(c.item.id);
        if (at != m_attempts.end() && at->second >= s.maxAttempts) continue;
        if (w.leader.configured &&
            (!w.leader.visible || Dist(c.item.grid, w.leader.info.grid) > s.maxLeaderDistance))
            continue;
        if (EnemiesNear(w, s, c.item.grid)) continue;
        if (!c.fits) {
            if (anyNoRoom) *anyNoRoom = true;
            continue;
        }
        return &c;
    }
    return nullptr;
}

bool PickupTask::StillWanted(const BotContext& ctx) const {
    const World& w = ctx.w;
    const LootSettings& s = ctx.p.loot;
    for (const LootCandidate& c : m_scanner.Candidates()) {
        if (c.item.id != m_targetId) continue;
        if (!c.fits || ctx.mem.IsBlacklisted(c.item.id, ctx.now)) return false;
        if (w.leader.configured && w.leader.visible &&
            Dist(c.item.grid, w.leader.info.grid) > s.maxLeaderDistance + s.preemptLeaderExtra)
            return false;
        return !EnemiesNear(w, s, c.item.grid);
    }
    return false;
}

bool PickupTask::WantsToStart(BotContext& ctx) {
    TrackArea(ctx);
    const LootSettings& s = ctx.p.loot;
    if (!s.enabled && !s.alwaysQuestItems) {
        SetStatus("loot pickup is off");
        return false;
    }
    ctx.req.groundItems = true;
    m_scanner.Update(ctx);
    UpdateFullFlag(ctx);
    if (!m_scanner.Candidates().empty()) ctx.req.inventoryFast = true;   // keep `fits` fresh while loot is near

    const char* blocked = BlockReason(ctx);
    if (blocked) {
        SetStatus(blocked);
        AddMarkers(ctx, 0);
        return false;
    }
    bool noRoom = false;
    const LootCandidate* c = ChooseTarget(ctx, &noRoom);
    if (noRoom && !ctx.mem.inventoryFull) {
        ctx.mem.inventoryFull = true;
        m_fullAtFreeCells = m_scanner.FreeCells();
        ctx.log.Add(LogCat::Loot, "Backpack full: a wanted item has no room (%d free cells)", m_fullAtFreeCells);
    }
    AddMarkers(ctx, c ? c->item.id : 0);
    if (!c) {
        SetStatus(noRoom ? "wanted items have no room" : "nothing wanted in range");
        return false;
    }
    m_pendingId = c->item.id;
    SetStatus("starting");
    return true;
}

void PickupTask::Start(BotContext& ctx) {
    TrackArea(ctx);
    m_startMs = ctx.now;
    m_phase = Phase::Choose;
    m_targetId = m_pendingId;
    m_pendingId = 0;
    m_pickedRun = 0;
    m_failReason.clear();
}

void PickupTask::BeginTarget(BotContext& ctx, const LootCandidate& c) {
    const LootSettings& s = ctx.p.loot;
    m_targetId = c.item.id;
    m_targetItem = c.item;
    m_targetInfo = c.info;
    m_targetName = lootcore::DisplayName(c.info);
    WorldClickTarget t;
    t.id = c.item.id;
    t.grid = c.item.grid;
    t.world = c.item.world;
    t.modelHeight = 0.f;
    t.targetableAddr = c.item.targetableAddr;
    t.screenOffsetY = static_cast<float>(s.labelOffsetPx);   // the label sits above the item
    WorldClickParams p;
    p.interactRange = s.pickupRange;
    p.dwellMs = s.hoverDwellMs;
    p.holdMs = s.pressHoldMs;
    p.reHovers = s.reHovers;
    p.heightFactor = 0.f;
    p.approachTimeoutMs = std::clamp(s.maxContinuousMs, 2000, 20000);
    m_click.Start(t, p, ctx.now);
    const auto at = m_attempts.find(m_targetId);
    ctx.log.Add(LogCat::Loot, "Picking up %s (%s, %.0f cells, attempt %d/%d)", m_targetName.c_str(),
                ItemCategoryName(c.info.category), c.item.dist, (at != m_attempts.end() ? at->second : 0) + 1,
                s.maxAttempts);
}

void PickupTask::GiveUp(BotContext& ctx, const char* why) {
    const LootSettings& s = ctx.p.loot;
    ctx.mem.Blacklist(m_targetId, ctx.now + static_cast<uint64_t>(s.blacklistMs));
    m_attempts.erase(m_targetId);
    ctx.log.Add(LogCat::Loot, "Giving up on %s for %d s: %s", m_targetName.c_str(), s.blacklistMs / 1000, why);
    m_scanner.RequestPass();
}

void PickupTask::AttemptFailed(BotContext& ctx, const char* why) {
    const LootSettings& s = ctx.p.loot;
    int& n = m_attempts[m_targetId];
    ++n;
    if (n >= s.maxAttempts) {
        GiveUp(ctx, why);
        return;
    }
    ctx.log.Add(LogCat::Loot, "%s: %s (attempt %d/%d)", m_targetName.c_str(), why, n, s.maxAttempts);
}

TaskStatus PickupTask::Finish(BotContext& ctx, TaskStatus st, const char* why, int cooldownMs) {
    m_click.Abort(ctx);
    m_phase = Phase::Choose;
    m_targetId = 0;
    if (cooldownMs > 0) m_cooldownUntilMs = ctx.now + static_cast<uint64_t>(cooldownMs);
    if (st == TaskStatus::Failed) m_failReason = why;
    else m_failReason.clear();
    ctx.log.Add(LogCat::Loot, "Pickup %s: %s (%d picked up)", st == TaskStatus::Failed ? "stopped" : "finished", why,
                m_pickedRun);
    SetStatus(why);
    return st;
}

TaskStatus PickupTask::Tick(BotContext& ctx) {
    TrackArea(ctx);
    const World& w = ctx.w;
    const LootSettings& s = ctx.p.loot;
    ctx.req.groundItems = true;
    ctx.req.inventoryFast = true;
    m_scanner.Update(ctx);
    UpdateFullFlag(ctx);
    AddMarkers(ctx, m_targetId);

    if (const char* why = PreemptReason(ctx)) return Finish(ctx, TaskStatus::Failed, why, kPreemptCooldownMs);
    if (ctx.now - m_startMs >= static_cast<uint64_t>(s.maxContinuousMs))
        return Finish(ctx, TaskStatus::Done, "time limit reached, following first", s.yieldCooldownMs);

    char line[200];
    for (int step = 0; step < 2; ++step) {   // Choose may hand over to Click in the same tick
        switch (m_phase) {
            case Phase::Choose: {
                bool noRoom = false;
                const LootCandidate* c = ChooseTarget(ctx, &noRoom);
                if (noRoom && !ctx.mem.inventoryFull) {
                    ctx.mem.inventoryFull = true;
                    m_fullAtFreeCells = m_scanner.FreeCells();
                    ctx.log.Add(LogCat::Loot, "Backpack full: a wanted item has no room (%d free cells)", m_fullAtFreeCells);
                }
                if (!c) {
                    const char* why = m_pickedRun > 0 ? "nothing left to pick up"
                                      : noRoom        ? "wanted items have no room"
                                                      : "nothing to pick up";
                    return Finish(ctx, TaskStatus::Done, why, 0);
                }
                BeginTarget(ctx, *c);
                m_phase = Phase::Click;
                continue;
            }
            case Phase::Click: {
                const GroundItem* gi = FindItem(w, m_targetId);
                if (!gi) {
                    // Gone before our click: someone else took it (or it despawned).
                    ctx.log.Add(LogCat::Loot, "%s is gone before the click", m_targetName.c_str());
                    m_click.Abort(ctx);
                    m_phase = Phase::Choose;
                    return TaskStatus::Running;
                }
                if (!StillWanted(ctx)) {
                    ctx.log.Add(LogCat::Loot, "%s is no longer wanted here (rules, room, leader or enemies)",
                                m_targetName.c_str());
                    m_click.Abort(ctx);
                    m_phase = Phase::Choose;
                    return TaskStatus::Running;
                }
                WorldClickTarget t = m_click.Target();
                t.grid = gi->grid;
                t.world = gi->world;
                t.targetableAddr = gi->targetableAddr;
                m_click.UpdateTarget(t);
                m_targetItem = *gi;
                const auto at = m_attempts.find(m_targetId);
                std::snprintf(line, sizeof(line), "Picking up %s (attempt %d/%d)", m_targetName.c_str(),
                              (at != m_attempts.end() ? at->second : 0) + 1, s.maxAttempts);
                ctx.report.detail.assign(line);
                SetStatus(m_click.CurrentPhase() == WorldClick::Phase::Approach ? "walking to an item" : "clicking an item");
                const WorldClickStatus st = m_click.Tick(ctx);
                if (st == WorldClickStatus::Clicked) {
                    m_vanish.Start(ctx.now, Dist(w.self.grid, gi->grid), s.vanishWaitMs, s.vanishWaitMovingMs);
                    m_phase = Phase::WaitVanish;
                    SetStatus("waiting for the pickup");
                    return TaskStatus::Running;
                }
                if (st == WorldClickStatus::Failed) {
                    const std::string& why = m_click.FailReason();
                    if (why == "no move key set") return Finish(ctx, TaskStatus::Failed, "no move key set (General tab)", 0);
                    if (why == "object unreachable") GiveUp(ctx, "no path to it");
                    else if (why == "stuck on the way") GiveUp(ctx, "stuck on the way to it");
                    else if (why == ActResultName(ActResult::Aborted))
                        ctx.log.Add(LogCat::Loot, "%s: the click was interrupted (input refused)", m_targetName.c_str());
                    else if (why == "object not on screen") AttemptFailed(ctx, "its label is not on screen");
                    else if (why == "approach timed out") AttemptFailed(ctx, "could not reach it in time");
                    else AttemptFailed(ctx, why.c_str());
                    m_phase = Phase::Choose;
                }
                return TaskStatus::Running;
            }
            case Phase::WaitVanish: {
                const GroundItem* gi = FindItem(w, m_targetId);
                const float dist = gi ? Dist(w.self.grid, gi->grid) : 0.f;
                ctx.report.hasTarget = true;
                ctx.report.target = gi ? gi->grid : m_targetItem.grid;
                const VanishWatch::Result vr = m_vanish.Update(ctx.now, gi != nullptr, w.items.empty(), dist);
                std::snprintf(line, sizeof(line), "Picking up %s (%s)", m_targetName.c_str(),
                              m_vanish.Approaching(ctx.now) ? "walking to it" : "waiting");
                ctx.report.detail.assign(line);
                switch (vr) {
                    case VanishWatch::Result::Vanished: {
                        ++m_pickedRun;
                        m_pickedTotal.fetch_add(1, std::memory_order_relaxed);
                        m_attempts.erase(m_targetId);
                        m_scanner.NotePickedUp(m_targetInfo, ctx.now);
                        if (m_targetInfo.priceKnown)
                            ctx.log.Add(LogCat::Loot, "Picked up %s (%.2f %s)", m_targetName.c_str(), m_targetInfo.value,
                                        RefCurrencyShort(s.currency));
                        else
                            ctx.log.Add(LogCat::Loot, "Picked up %s", m_targetName.c_str());
                        m_phase = Phase::Choose;
                        SetStatus("picked up an item");
                        return TaskStatus::Running;
                    }
                    case VanishWatch::Result::TimedOut:
                        AttemptFailed(ctx, "it did not vanish after the click");
                        m_phase = Phase::Choose;
                        return TaskStatus::Running;
                    case VanishWatch::Result::Pending:
                        break;
                }
                return TaskStatus::Running;
            }
        }
    }
    return TaskStatus::Running;
}

void PickupTask::Observe(BotContext& ctx) {
    if (!m_scanner.DiagnosticsWanted(ctx.now)) return;
    TrackArea(ctx);
    ctx.req.groundItems = true;
    m_scanner.Update(ctx);   // throttled: a second call in the same tick does nothing
}

void PickupTask::Abort(BotContext& ctx) {
    m_click.Abort(ctx);
    m_phase = Phase::Choose;
    m_targetId = 0;
    m_pendingId = 0;
    SetStatus("interrupted");
}

bool PickupTask::AllowsCasts(CastClass c) const {
    switch (c) {
        case CastClass::Flask:
            return true;
        case CastClass::Self:
        case CastClass::Attack:
            // Only while walking up to the item: a cast during the hover or the
            // pickup walk would move the cursor or stop the character.
            return m_phase == Phase::Click && m_click.CurrentPhase() == WorldClick::Phase::Approach;
        case CastClass::Movement:
            return false;
    }
    return false;
}

void PickupTask::AddMarkers(BotContext& ctx, uint32_t chosenId) const {
    const DebugSettings& d = ctx.s.global.debug;
    if (!d.overlay || !d.showLoot) return;
    size_t n = 0;
    for (const LootCandidate& c : m_scanner.All()) {
        if (n >= kMaxMarkers) break;
        TaskMarker m;
        m.grid = c.item.grid;
        if (c.item.id == chosenId && chosenId != 0) {
            m.color = kColTarget;
            m.label = "target";
        } else if (c.blacklisted) {
            m.color = kColSkip;
            m.label = "skip";
        } else if (!c.decision.pick) {
            continue;
        } else if (!c.fits) {
            m.color = kColNoRoom;
            m.label = "no room";
        } else {
            m.color = kColWanted;
            m.label = "loot";
        }
        ctx.report.markers.push_back(std::move(m));
        ++n;
    }
}

}  // namespace fb
