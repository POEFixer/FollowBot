// SkillEngine.cpp - see SkillEngine.h. The decisions live in the pure
// SkillConditions / Targeting code; this file gathers the tick's facts, reads
// the Targetable flag of nearby monsters and sends the presses.
// FB_TESTS_EXCLUDE: SDK adapter (Targetable reads); its pure half is Targeting.cpp.
#include "SkillEngine.h"

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "../nav/PathPlanner.h"

#include <algorithm>
#include <cstring>

namespace fb {

namespace {

// Monsters farther than the widest radius a rule can use get no Targetable read.
constexpr float kTargetableMarginCells = 5.f;
constexpr int   kTargetableReadsPerTick = 24;
constexpr uint64_t kTargetableTtlMs = 400;
constexpr uint64_t kTargetableForgetMs = 5000;
// A channel runs at least this long before a lost target ends it.
constexpr int   kChannelMinMs = 150;
// After a failed press nothing is tried for this long.
constexpr int   kFailBackoffMs = 250;
constexpr uint32_t kMarkerColor = 0xFF3C3CFFu;   // ImGui ABGR: red

// The Brain passes its Follow task on the follow path (steering is skipped
// after a cast there); any other task steers on its own before the cast.
bool IsFollowPath(const Task* current) { return !current || std::strcmp(current->Name(), "Follow") == 0; }

bool Fresh(uint64_t stampMs, uint64_t now, uint64_t maxAge) {
    return stampMs != 0 && now >= stampMs && now - stampMs <= maxAge;
}

uint64_t CellKey(const Vec2& p) {
    const auto x = static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.x)));
    const auto y = static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.y)));
    return (static_cast<uint64_t>(x) << 32) | y;
}

const char* AimText(const RuleTarget& t) {
    switch (t.aim) {
        case AimKind::Monster:   return "at a monster";
        case AimKind::Leader:    return "on the leader";
        case AimKind::Self:      return "on self";
        case AimKind::PathAhead: return "ahead on the path";
        case AimKind::None:      break;
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// LOS cache
// ---------------------------------------------------------------------------

bool SkillEngine::LosCache::Find(uint64_t key, bool& clear) const {
    size_t i = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 55) % kSize;
    for (size_t probe = 0; probe < 8; ++probe, i = (i + 1) % kSize) {
        const Slot& s = slots[i];
        if (s.gen != gen) return false;
        if (s.key == key) {
            clear = s.clear;
            return true;
        }
    }
    return false;
}

void SkillEngine::LosCache::Put(uint64_t key, bool clear) {
    size_t i = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 55) % kSize;
    for (size_t probe = 0; probe < 8; ++probe, i = (i + 1) % kSize) {
        Slot& s = slots[i];
        if (s.gen != gen || s.key == key) {
            s = Slot{key, gen, clear};
            return;
        }
    }
    // Neighbourhood full: overwrite the home slot (it is only a cache).
    slots[static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 55) % kSize] = Slot{key, gen, clear};
}

// ---------------------------------------------------------------------------

SkillEngine::SkillEngine() { m_targetable.reserve(256); }

void SkillEngine::Reset() {
    m_whyNot.clear();
    m_lastFireMs.clear();
    m_lastRule.clear();
    m_lastCastMs = 0;
    m_nextCastMs = 0;
    m_hold = Hold{};
    m_motion.Reset();
    m_targetable.clear();
    m_lastPruneMs = 0;
}

void SkillEngine::SetWhy(size_t index, const char* text) {
    if (index < m_whyNot.size()) m_whyNot[index].assign(text ? text : "");
}

bool SkillEngine::LineOfSight(const Vec2& from, const Vec2& to) {
    // No walkable grid yet: nothing to test against, do not block casts.
    if (!m_losPlanner || !m_losPlanner->Grid()) return true;
    const uint64_t key = CellKey(to);
    bool clear = false;
    if (m_los.Find(key, clear)) return clear;
    clear = m_losPlanner->LineOfSight(from, to, 1);
    m_los.Put(key, clear);
    return clear;
}

void SkillEngine::SetRequests(WorldRequests& req, const SkillSettings& sk) const {
    for (const auto& r : sk.rules) {
        if (!r.enabled) continue;
        if (!TrimView(r.skillName).empty()) req.skills = true;
        if (!TrimView(r.buffMissing).empty() || !TrimView(r.buffPresent).empty()) req.selfBuffs = true;
        if (!TrimView(r.leaderBuffMissing).empty()) req.leaderBuffs = true;
        if (r.kind == RuleKind::Flask && r.flaskSlot >= 0) req.flasks = true;
    }
}

void SkillEngine::UpdateTargetable(BotContext& ctx) {
    const World& w = ctx.w;
    const SkillSettings& sk = ctx.p.skills;
    const size_t n = w.monsters.size();
    m_targetableState.assign(n, static_cast<int8_t>(-1));
    if (!sk.skipUntargetable || !ctx.sdk) return;
    if (w.gate.areaCounter != m_targetableArea) {
        m_targetable.clear();
        m_targetableArea = w.gate.areaCounter;
    }
    // Widest radius any rule looks at.
    float radius = sk.combatRadius;
    for (const auto& r : sk.rules)
        if (r.enabled) radius = (std::max)(radius, (std::max)(r.maxTargetDistance, r.enemyRadius));
    radius += kTargetableMarginCells;
    const float r2 = radius * radius;
    int reads = 0;
    for (size_t i = 0; i < n; ++i) {
        const Monster& m = w.monsters[i];
        if (Dist2(m.grid, w.self.grid) > r2) continue;
        TargetableEntry& e = m_targetable[m.id];
        e.seenMs = ctx.now;
        if (m.targetableAddr != 0 && reads < kTargetableReadsPerTick &&
            (e.readMs == 0 || ctx.now - e.readMs >= kTargetableTtlMs)) {
            ++reads;
            e.readMs = ctx.now;
            const PluginSDK::Targetable t = ctx.sdk->Components.ReadTargetable(m.targetableAddr);
            e.state = t.Valid ? static_cast<int8_t>(t.IsTargetable && !t.HiddenFromPlayer ? 1 : 0) : static_cast<int8_t>(-1);
        }
        m_targetableState[i] = e.state;
    }
    if (ctx.now - m_lastPruneMs >= 2000) {
        m_lastPruneMs = ctx.now;
        for (auto it = m_targetable.begin(); it != m_targetable.end();) {
            if (ctx.now - it->second.seenMs > kTargetableForgetMs) it = m_targetable.erase(it);
            else ++it;
        }
    }
}

void SkillEngine::BuildFrame(BotContext& ctx) {
    const World& w = ctx.w;
    const SkillSettings& sk = ctx.p.skills;
    CombatFacts& f = m_facts;
    f = CombatFacts{};
    f.now = ctx.now;

    const SelfState& s = w.self;
    f.selfValid = s.valid;
    f.selfDead = s.dead;
    f.self = s.grid;
    f.selfWorld = s.world;
    f.hpPct = s.hpPct;
    f.esPct = s.esPct;
    f.mpPct = s.mpPct;
    f.hpMax = s.hpMax;
    f.esMax = s.esMax;
    f.mpMax = s.mpMax;
    f.moving = m_motion.Moving(ctx.now, s.actionFlags);

    const LeaderState& L = w.leader;
    if (L.visible) {
        f.leaderVisible = true;
        f.leaderKnown = true;
        f.leader = L.info.grid;
        f.leaderWorld = L.info.world;
        f.leaderDistance = Dist(L.info.grid, s.grid);
        f.leaderHpPct = L.info.hpMax > 0 ? L.info.hpPct : 100.f;
    } else if (L.lastSeenMs != 0 && L.lastSeenArea == w.gate.areaCounter) {
        f.leaderKnown = true;
        f.leader = L.lastGrid;
        f.leaderWorld = L.lastWorld;
        f.leaderDistance = Dist(L.lastGrid, s.grid);
    }

    f.mode = sk.mode;
    f.combatRadius = sk.combatRadius;
    f.leashRadius = sk.leashRadius;
    f.maxCombatLeaderDistance = sk.maxCombatLeaderDistance;

    f.skills = &w.skills;
    f.skillsFresh = Fresh(w.skillsMs, ctx.now, kSkillsMaxAgeMs);
    f.selfBuffs = &w.selfBuffs;
    f.selfBuffsFresh = Fresh(w.selfBuffsMs, ctx.now, kSelfBuffsMaxAgeMs);
    f.leaderBuffs = &w.leaderBuffs;
    f.leaderBuffsFresh = Fresh(w.leaderBuffsMs, ctx.now, kLeaderBuffsMaxAgeMs);
    f.belt = &w.flasks;
    f.beltFresh = Fresh(w.flasksMs, ctx.now, kBeltMaxAgeMs);

    f.cursorBusy = ctx.cursorBusy;
    f.projectionValid = w.proj.valid && w.proj.screenW > 0 && w.proj.screenH > 0;

    const HotkeySettings& hk = ctx.s.global.hotkeys;
    f.Reserve(hk.startStop);
    f.Reserve(hk.pause);
    f.Reserve(hk.holdOverride);
    f.Reserve(hk.stashNow);
    const FollowSettings& fs = ctx.p.follow;
    f.Reserve(fs.clickToMove ? VK_LBUTTON : fs.moveKey);
    f.Reserve(fs.sprintKey);

    UpdateTargetable(ctx);
    const SafeRect safe = MakeSafeRect(w.proj.screenW, w.proj.screenH);
    ClassifyMonsters(w.monsters, w.proj, safe, &sk.monsterBlacklist, &m_targetableState, sk.skipUntargetable, m_views,
                     m_usable, m_aimable);
    const CombatState cs =
        ComputeCombat(w.monsters, m_usable, f.self, f.leaderVisible, f.leader, sk.combatRadius, sk.leashRadius);
    f.inCombat = cs.inCombat;
    f.hostiles = cs.hostiles;

    m_tctx.monsters = &w.monsters;
    m_tctx.views = &m_views;
    m_tctx.usable = &m_usable;
    m_tctx.aimable = &m_aimable;
    m_tctx.proj = &w.proj;
    m_tctx.safe = safe;
    // The planner's path is only "the path ahead" while the character walks it.
    m_tctx.path = ctx.mover.IsMoving() ? &ctx.planner.Path() : nullptr;
    m_los.NewTick();
    m_losPlanner = &ctx.planner;
    // A one-pointer capture fits std::function's small buffer: no allocation.
    m_tctx.los = [this](const Vec2& from, const Vec2& to) { return LineOfSight(from, to); };
}

void SkillEngine::ReleaseHold(BotContext& ctx, const char* why) {
    if (!m_hold.active) return;
    ctx.act.KeyUp(m_hold.key);
    ctx.log.Add(LogCat::Combat, "Released \"%s\" (%s)", m_hold.label.c_str(), why ? why : "done");
    const uint64_t gate = ctx.now + static_cast<uint64_t>(ctx.p.skills.globalCastGateMs);
    if (gate > m_nextCastMs) m_nextCastMs = gate;
    m_hold = Hold{};
}

bool SkillEngine::ContinueHold(BotContext& ctx, const Task* current, bool followPath) {
    Hold& h = m_hold;
    const auto& rules = ctx.p.skills.rules;
    if (!ctx.act.IsHeld(h.key)) {
        // Input was released elsewhere (gate hold, focus loss, stop): just forget it.
        ctx.log.Add(LogCat::Combat, "Hold of \"%s\" ended: input was released", h.label.c_str());
        m_hold = Hold{};
        return false;
    }
    const SkillRule* r = h.rule < rules.size() ? &rules[h.rule] : nullptr;
    if (r && (r->key != h.key || r->label != h.label)) r = nullptr;   // the rule list was edited
    const char* stop = nullptr;
    if (!r || !r->enabled) stop = "the rule changed";
    else if (!followPath) stop = "a task took over";
    else if (current && !current->AllowsCasts(ClassOf(r->kind))) stop = "not allowed now";
    else if (!ctx.w.self.valid || ctx.w.self.dead) stop = "character dead or not readable";
    else if (ctx.now >= h.untilMs) stop = "done";
    else if (h.needsCursor && ctx.cursorBusy) stop = "another plugin took the cursor";
    else if (h.channel && h.needsCursor && ctx.now >= h.minUntilMs) {
        RuleTarget t;
        Reason why;
        if (!ResolveTarget(*r, m_facts, m_tctx, t, why)) {
            stop = "target gone";
        } else {
            if (Dist(t.screen, h.aimScreen) > 6.f && ctx.act.MoveCursor(t.screen) == ActResult::Ok) h.aimScreen = t.screen;
            h.targetGrid = t.grid;
        }
    }
    if (stop) {
        if (r) {
            Reason why;
            why.Set("released: %s", stop);
            SetWhy(h.rule, why.text);
        }
        ReleaseHold(ctx, stop);
        return false;
    }
    // Still holding: stand still, keep the cursor ours.
    if (ctx.mover.IsMoving() || ctx.act.MoveKeyHeld()) ctx.mover.Stop(ctx.act);
    if (h.needsCursor) ctx.act.MarkCursorBusy(150);
    Reason why;
    why.Set("holding (%llu ms left)", static_cast<unsigned long long>(h.untilMs - ctx.now));
    SetWhy(h.rule, why.text);
    for (size_t i = 0; i < m_whyNot.size(); ++i)
        if (i != h.rule) m_whyNot[i].assign("waiting: another rule holds its key");
    if (h.aim == AimKind::Monster) ctx.report.markers.push_back({h.targetGrid, kMarkerColor, "cast"});
    return true;
}

bool SkillEngine::Fire(BotContext& ctx, size_t index, const RuleTarget& t) {
    const SkillSettings& sk = ctx.p.skills;
    const SkillRule& r = sk.rules[index];
    Actuator& act = ctx.act;
    const int pressMs = PressMsOf(r);
    const bool longPress = pressMs > kBlockingPressMaxMs;
    const bool stop = r.stopToCast || longPress;
    if (stop && (ctx.mover.IsMoving() || act.MoveKeyHeld())) ctx.mover.Stop(act);
    // Walking on with the move key: put the cursor back afterwards (keeps the direction).
    const bool restore = !stop && t.needsCursor && act.MoveKeyHeld() && act.HasCursor();
    const Vec2 previous = act.LastCursor();
    Vec2 aim = t.screen;
    if (t.needsCursor && t.aim != AimKind::PathAhead) aim = m_tctx.safe.Clamp(aim + act.ClickJitter());

    ActResult res = ActResult::Ok;
    if (!longPress) {
        const int tapMs = pressMs > 0 ? pressMs : 30 + act.Random().UniformInt(0, 25);
        if (t.needsCursor) {
            act.MarkCursorBusy(static_cast<uint32_t>(tapMs + 250));
            res = act.PressAt(r.key, aim, tapMs);
        } else {
            res = act.TapKey(r.key, tapMs);
        }
        if (restore && res == ActResult::Ok) act.MoveCursor(previous);
    } else {
        if (t.needsCursor) {
            act.MarkCursorBusy(300);
            res = act.MoveCursor(aim);
        }
        if (res == ActResult::Ok) res = act.KeyDown(r.key);
        if (res == ActResult::Ok) {
            act.MarkGate();   // KeyDown itself is not gated
            m_hold = Hold{};
            m_hold.active = true;
            m_hold.rule = index;
            m_hold.key = r.key;
            m_hold.channel = r.channelMs > 0;
            m_hold.needsCursor = t.needsCursor;
            m_hold.aim = t.aim;
            m_hold.aimScreen = aim;
            m_hold.targetGrid = t.grid;
            m_hold.startMs = ctx.now;
            m_hold.untilMs = ctx.now + static_cast<uint64_t>(pressMs);
            m_hold.minUntilMs = ctx.now + static_cast<uint64_t>(kChannelMinMs);
            m_hold.label = r.label;
        }
    }

    if (res != ActResult::Ok) {
        Reason why;
        why.Set("ready, but the press failed (%s)", ActResultName(res));
        SetWhy(index, why.text);
        ctx.log.Add(LogCat::Combat, "Cast \"%s\" failed: %s", r.label.c_str(), ActResultName(res));
        m_nextCastMs = ctx.now + static_cast<uint64_t>(kFailBackoffMs);
        return false;
    }
    m_lastFireMs[index] = ctx.now;
    m_lastCastMs = ctx.now;
    m_nextCastMs = ctx.now + static_cast<uint64_t>(sk.globalCastGateMs) +
                   static_cast<uint64_t>(act.Random().UniformInt(0, act.Gate().JitterMs()));
    m_lastRule = r.label;
    ctx.mem.currentRule = r.label;
    SetWhy(index, longPress ? "fired (holding)" : "fired");
    if (t.aim == AimKind::Monster) {
        ctx.report.markers.push_back({t.grid, kMarkerColor, "cast"});
        ctx.log.Add(LogCat::Combat, "Cast \"%s\" at a %s monster", r.label.c_str(), RarityName(t.rarity));
    } else {
        const char* where = AimText(t);
        ctx.log.Add(LogCat::Combat, "Cast \"%s\"%s%s", r.label.c_str(), where[0] ? " " : "", where);
    }
    return true;
}

bool SkillEngine::TryCast(BotContext& ctx, const Task* current) {
    const SkillSettings& sk = ctx.p.skills;
    const std::vector<SkillRule>& rules = sk.rules;
    const size_t n = rules.size();
    if (m_whyNot.size() != n) m_whyNot.resize(n);
    if (m_lastFireMs.size() != n) m_lastFireMs.resize(n, 0);
    const World& w = ctx.w;
    if (w.self.valid) m_motion.Add(ctx.now, w.self.grid, w.gate.areaCounter);

    if (!sk.enabled) {
        if (m_hold.active) ReleaseHold(ctx, "skill rules turned off");
        for (auto& s : m_whyNot) s.assign("skill rules are off");
        return false;
    }
    SetRequests(ctx.req, sk);
    BuildFrame(ctx);
    const bool followPath = IsFollowPath(current);

    if (m_hold.active && ContinueHold(ctx, current, followPath)) return true;
    if (n == 0) return false;

    const char* taskName = current ? current->Name() : "";
    int chosen = -1;
    RuleTarget chosenTarget;
    Reason why;
    RuleTarget target;
    for (size_t i = 0; i < n; ++i) {
        const SkillRule& r = rules[i];
        RuleInputs in;
        in.lastFireMs = m_lastFireMs[i];
        in.classAllowed = !current || current->AllowsCasts(ClassOf(r.kind));
        in.longPressAllowed = followPath;
        in.taskName = taskName;
        if (EvaluateRule(r, m_facts, m_tctx, in, target, why)) {
            if (chosen < 0) {
                chosen = static_cast<int>(i);
                chosenTarget = target;
                SetWhy(i, "ready");
            } else {
                why.Set("ready (rule %d comes first)", chosen + 1);
                SetWhy(i, why.text);
            }
        } else {
            SetWhy(i, why.text);
        }
    }
    if (chosen < 0) return false;
    const size_t c = static_cast<size_t>(chosen);
    if (ctx.now < m_nextCastMs) {
        why.Set("ready: global cast gate (%llu ms)", static_cast<unsigned long long>(m_nextCastMs - ctx.now));
        SetWhy(c, why.text);
        return false;
    }
    if (!ctx.act.GateReady()) {
        SetWhy(c, "ready: waiting for the input gate");
        return false;
    }
    if (!ctx.act.CanAct()) {
        SetWhy(c, "ready: the game window is not in front");
        return false;
    }
    return Fire(ctx, c, chosenTarget);
}

}  // namespace fb
