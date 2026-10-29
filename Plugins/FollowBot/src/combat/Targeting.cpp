// Targeting.cpp - target selection (Targeting.h) and the pure rule conditions
// (SkillConditions.h). No SDK, no input; built into tests/.
#include "Targeting.h"

#include "SkillConditions.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace fb {

// ===========================================================================
// Strings
// ===========================================================================

namespace {

inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool IsMonsterMode(TargetMode m) {
    return m == TargetMode::NearestEnemy || m == TargetMode::StrongestEnemy || m == TargetMode::LowestHpEnemy ||
           m == TargetMode::DensestCluster;
}

}  // namespace

std::string_view TrimView(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool EqualsAsciiCI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}

bool ContainsAsciiCI(std::string_view hay, std::string_view needle) {
    if (needle.empty() || needle.size() > hay.size()) return false;
    const size_t last = hay.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t k = 0;
        while (k < needle.size() && LowerAscii(hay[i + k]) == LowerAscii(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

bool IsBlacklistedPath(std::string_view path, const std::vector<std::string>* blacklist) {
    if (!blacklist) return false;
    for (const auto& entry : *blacklist) {
        const std::string_view e = TrimView(entry);
        if (!e.empty() && ContainsAsciiCI(path, e)) return true;
    }
    return false;
}

bool NameEquals(std::string_view a, std::string_view b) {
    const std::string_view ta = TrimView(a), tb = TrimView(b);
    return !ta.empty() && EqualsAsciiCI(ta, tb);
}

const char* RarityName(int rarity) {
    switch (rarity) {
        case 0: return Key("rarity_lower_normal");
        case 1: return Key("rarity_lower_magic");
        case 2: return Key("rarity_lower_rare");
        case 3: return Key("rarity_lower_unique");
        default: break;
    }
    return Key("rarity_lower_unknown");
}

// ===========================================================================
// Targeting
// ===========================================================================

int CountMonstersNear(const std::vector<Monster>& monsters, const Vec2& center, float radius) {
    return CountMonstersNear(monsters, center, radius, 0, nullptr);
}

int CountMonstersNear(const std::vector<Monster>& monsters, const Vec2& center, float radius, int minRarity,
                      const std::vector<uint8_t>* mask) {
    const bool useMask = mask && mask->size() == monsters.size();
    const float r2 = radius * radius;
    int n = 0;
    for (size_t i = 0; i < monsters.size(); ++i) {
        const Monster& m = monsters[i];
        if (useMask && !(*mask)[i]) continue;
        if (m.rarity < minRarity) continue;
        if (Dist2(m.grid, center) <= r2) ++n;
    }
    return n;
}

namespace {

struct Best {
    bool   have = false;
    size_t index = 0;
    float  dist = 0.f;
    int    rarity = 0;
    float  hp = 0.f;
    int    cluster = 0;
};

// Whether the candidate beats the current best for the mode (ties -> nearer).
bool Beats(TargetMode mode, const Best& b, float d, int rarity, float hp, int cluster) {
    if (!b.have) return true;
    constexpr float kHpEps = 0.01f;
    switch (mode) {
        case TargetMode::StrongestEnemy:
            if (rarity != b.rarity) return rarity > b.rarity;
            if (std::fabs(hp - b.hp) > kHpEps) return hp > b.hp;
            return d < b.dist;
        case TargetMode::LowestHpEnemy:
            if (std::fabs(hp - b.hp) > kHpEps) return hp < b.hp;
            return d < b.dist;
        case TargetMode::DensestCluster:
            if (cluster != b.cluster) return cluster > b.cluster;
            return d < b.dist;
        case TargetMode::NearestEnemy:
        default:
            return d < b.dist;
    }
}

}  // namespace

TargetPick SelectTarget(const std::vector<Monster>& monsters, const TargetQuery& q, const LosFn& los) {
    TargetPick pick;
    if (!IsMonsterMode(q.mode)) return pick;
    const bool useMask = q.eligible && q.eligible->size() == monsters.size();
    const float max2 = q.maxDistance * q.maxDistance;
    const float leash2 = q.leashRadius * q.leashRadius;
    const bool leash = q.restrictToLeash && q.leaderVisible;
    Best best;
    for (size_t i = 0; i < monsters.size(); ++i) {
        const Monster& m = monsters[i];
        if (!m.targetable) { ++pick.rejects.untargetable; continue; }
        if (IsBlacklistedPath(m.path, q.blacklist)) { ++pick.rejects.blacklisted; continue; }
        const float d2 = Dist2(m.grid, q.self);
        if (d2 > max2) { ++pick.rejects.tooFar; continue; }
        if (m.rarity < q.minRarity) { ++pick.rejects.lowRarity; continue; }
        if (leash && Dist2(m.grid, q.leader) > leash2) { ++pick.rejects.outsideLeash; continue; }
        if (useMask && !(*q.eligible)[i]) { ++pick.rejects.notEligible; continue; }
        ++pick.candidates;
        const float d = std::sqrt(d2);
        const int cluster = q.mode == TargetMode::DensestCluster ? CountMonstersNear(monsters, m.grid, q.clusterRadius) : 0;
        if (!Beats(q.mode, best, d, m.rarity, m.hpPct, cluster)) continue;
        // Line of sight only for a candidate that would become the best.
        if (q.requireLos && los && !los(q.self, m.grid)) { ++pick.rejects.noLos; continue; }
        best.have = true;
        best.index = i;
        best.dist = d;
        best.rarity = m.rarity;
        best.hp = m.hpPct;
        best.cluster = cluster;
    }
    if (!best.have) return pick;
    pick.found = true;
    pick.index = best.index;
    pick.grid = monsters[best.index].grid;
    pick.clusterCount = q.mode == TargetMode::DensestCluster ? best.cluster
                                                             : CountMonstersNear(monsters, pick.grid, q.clusterRadius);
    return pick;
}

// ===========================================================================
// Rule conditions (SkillConditions.h)
// ===========================================================================


// --- motion ------------------------------------------------------------------

void MotionTracker::Add(uint64_t now, const Vec2& pos, uint32_t area) {
    if (!IsFinite(pos)) return;
    if (m_count > 0) {
        const Sample& last = At(0);
        if (area != m_area || Dist(last.pos, pos) > 40.f || now < last.t) {
            Reset();
        } else if (now - last.t < 15) {
            return;   // one sample per tick is plenty
        }
    }
    m_area = area;
    m_samples[static_cast<size_t>(m_head)] = Sample{now, pos};
    m_head = (m_head + 1) % kSamples;
    if (m_count < kSamples) ++m_count;
}

bool MotionTracker::Moving(uint64_t now, uint32_t actionFlags, int windowMs, float minSpeed) const {
    if ((actionFlags & kRunFlags) == kRunFlags) return true;
    if (m_count < 2 || windowMs <= 0) return false;
    const Sample& newest = At(0);
    if (now > newest.t + 500) return false;   // no fresh position: unknown, treat as standing
    // The oldest sample still inside the window.
    const Sample* ref = nullptr;
    for (int k = 1; k < m_count; ++k) {
        const Sample& s = At(k);
        if (newest.t - s.t > static_cast<uint64_t>(windowMs)) break;
        ref = &s;
    }
    if (!ref) return false;
    const uint64_t span = newest.t - ref->t;
    if (span < static_cast<uint64_t>(windowMs / 3) || span == 0) return false;
    const float speed = Dist(newest.pos, ref->pos) / (static_cast<float>(span) / 1000.f);
    return speed >= minSpeed;
}

// --- belt ----------------------------------------------------------------------

int BeltIndexOf(const FlaskInfo& f) {
    if (f.slot < 0) return -1;
    return f.isCharm ? 2 + f.slot : f.slot;
}

const FlaskInfo* FindBeltSlot(const std::vector<FlaskInfo>& belt, int beltIndex) {
    if (beltIndex < 0) return nullptr;
    for (const auto& f : belt)
        if (f.valid && BeltIndexOf(f) == beltIndex) return &f;
    return nullptr;
}

const char* BeltSlotName(int beltIndex) {
    switch (beltIndex) {
        case 0: return Key("belt_flask_1");
        case 1: return Key("belt_flask_2");
        case 2: return Key("belt_charm_1");
        case 3: return Key("belt_charm_2");
        case 4: return Key("belt_charm_3");
        default: break;
    }
    return Key("belt_not_checked");
}

// --- aim -----------------------------------------------------------------------

AimKind AimOf(const SkillRule& r) {
    switch (r.kind) {
        case RuleKind::Flask:      return AimKind::None;
        case RuleKind::Movement:   return AimKind::PathAhead;
        case RuleKind::Self:       return r.target == TargetMode::CursorUnchanged ? AimKind::None : AimKind::Self;
        case RuleKind::LeaderBuff: return r.target == TargetMode::CursorUnchanged ? AimKind::None : AimKind::Leader;
        case RuleKind::Attack:
        case RuleKind::Count:      break;
    }
    if (IsMonsterMode(r.target)) return AimKind::Monster;
    switch (r.target) {
        case TargetMode::Leader: return AimKind::Leader;
        case TargetMode::Self:   return AimKind::Self;
        default:                 break;
    }
    return AimKind::None;
}

TargetMode NaturalTarget(RuleKind k) {
    switch (k) {
        case RuleKind::Attack:     return TargetMode::NearestEnemy;
        case RuleKind::Self:       return TargetMode::Self;
        case RuleKind::LeaderBuff: return TargetMode::Leader;
        case RuleKind::Movement:   return TargetMode::CursorUnchanged;
        case RuleKind::Flask:      return TargetMode::Self;
        case RuleKind::Count:      break;
    }
    return TargetMode::NearestEnemy;
}

// --- per-tick classification ----------------------------------------------------

void ClassifyMonsters(const std::vector<Monster>& monsters, const ProjectionData& proj, const SafeRect& safe,
                      const std::vector<std::string>* blacklist, const std::vector<int8_t>* targetableState,
                      bool skipUntargetable, std::vector<MonsterView>& views, std::vector<uint8_t>& usable,
                      std::vector<uint8_t>& aimable) {
    const size_t n = monsters.size();
    views.resize(n);
    usable.assign(n, 0);
    aimable.assign(n, 0);
    const bool haveState = targetableState && targetableState->size() == n;
    for (size_t i = 0; i < n; ++i) {
        const Monster& m = monsters[i];
        MonsterView v;
        bool ok = m.targetable && !IsBlacklistedPath(m.path, blacklist);
        if (ok && skipUntargetable && haveState && (*targetableState)[i] == 0) ok = false;
        v.usable = ok;
        if (ok) {
            // Aim at the body, not the feet: half the model height, capped (bogus bounds).
            const float lift = std::clamp(m.modelHeight * 0.5f, 0.f, 120.f);
            Vec2 s;
            if (ProjectLifted(proj, m.world, lift, s) && safe.Contains(s)) {
                v.onScreen = true;
                v.screen = s;
            }
        }
        views[i] = v;
        usable[i] = ok ? 1 : 0;
        aimable[i] = (ok && v.onScreen) ? 1 : 0;
    }
}

CombatState ComputeCombat(const std::vector<Monster>& monsters, const std::vector<uint8_t>& usable, const Vec2& self,
                          bool leaderVisible, const Vec2& leader, float combatRadius, float leashRadius) {
    CombatState st;
    const bool useMask = usable.size() == monsters.size();
    const float c2 = combatRadius * combatRadius, l2 = leashRadius * leashRadius;
    for (size_t i = 0; i < monsters.size(); ++i) {
        if (useMask && !usable[i]) continue;
        const Vec2& g = monsters[i].grid;
        if (Dist2(g, self) > c2) continue;
        if (leaderVisible && Dist2(g, leader) > l2) continue;
        ++st.hostiles;
    }
    st.inCombat = st.hostiles > 0;
    return st;
}

bool PointAlongPath(const std::vector<Vec2>& path, const Vec2& from, float cells, Vec2& out) {
    if (path.size() < 2) return false;
    size_t seg = 0;
    PolylineDistance(from, path, &seg);
    float t = 0.f;
    PointSegmentDistance(from, path[seg], path[seg + 1], &t);
    Vec2 at = Lerp(path[seg], path[seg + 1], t);
    float left = cells < 0.f ? 0.f : cells;
    for (size_t i = seg + 1; i < path.size(); ++i) {
        const float d = Dist(at, path[i]);
        if (d >= left) {
            out = d > 1e-6f ? Lerp(at, path[i], left / d) : path[i];
            return true;
        }
        left -= d;
        at = path[i];
    }
    out = path.back();
    return true;
}

// --- condition families ---------------------------------------------------------

bool CheckKey(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.key <= 0) {
        why.Set(Tr("skill_why_no_key"));
        return false;
    }
    if (f.IsReserved(r.key)) {
        why.Set(Tr("skill_why_reserved_key"));
        return false;
    }
    return true;
}

bool CheckCombatMode(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.kind != RuleKind::Attack) return true;
    if (f.mode == CombatMode::Passive) {
        why.Set(Tr("skill_why_passive"));
        return false;
    }
    if (f.leaderDistance >= 0.f && f.leaderDistance > f.maxCombatLeaderDistance) {
        why.Set(TrF("skill_why_leader_far", {{"distance", Num("%.0f", f.leaderDistance)},
                                             {"max", Num("%.0f", f.maxCombatLeaderDistance)}}));
        return false;
    }
    return true;
}

bool CheckCombatState(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.when == CombatWhen::InCombat && !f.inCombat) {
        why.Set(Tr("skill_why_not_in_combat"));
        return false;
    }
    if (r.when == CombatWhen::OutOfCombat && f.inCombat) {
        why.Set(TrF("skill_why_in_combat", {{"hostiles", Num("%d", f.hostiles)}}));
        return false;
    }
    return true;
}

bool CheckMoveState(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.moveState == MoveState::Moving && !f.moving) {
        why.Set(Tr("skill_why_standing"));
        return false;
    }
    if (r.moveState == MoveState::Stationary && f.moving) {
        why.Set(Tr("skill_why_moving"));
        return false;
    }
    return true;
}

bool CheckVitals(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.selfHpBelow >= 0.f) {
        if (f.hpMax <= 0) { why.Set(Tr("skill_why_life_unreadable")); return false; }
        if (!(f.hpPct < r.selfHpBelow)) {
            why.Set(TrF("skill_why_life", {{"value", Num("%.0f", f.hpPct)}, {"limit", Num("%.0f", r.selfHpBelow)}}));
            return false;
        }
    }
    if (r.selfEsBelow >= 0.f) {
        if (f.esMax <= 0) { why.Set(Tr("skill_why_no_es")); return false; }
        if (!(f.esPct < r.selfEsBelow)) {
            why.Set(TrF("skill_why_es", {{"value", Num("%.0f", f.esPct)}, {"limit", Num("%.0f", r.selfEsBelow)}}));
            return false;
        }
    }
    if (r.selfManaAbove >= 0.f || r.selfManaBelow >= 0.f) {
        if (f.mpMax <= 0) { why.Set(Tr("skill_why_mana_unreadable")); return false; }
        if (r.selfManaAbove >= 0.f && !(f.mpPct > r.selfManaAbove)) {
            why.Set(TrF("skill_why_mana_above", {{"value", Num("%.0f", f.mpPct)}, {"limit", Num("%.0f", r.selfManaAbove)}}));
            return false;
        }
        if (r.selfManaBelow >= 0.f && !(f.mpPct < r.selfManaBelow)) {
            why.Set(TrF("skill_why_mana_below", {{"value", Num("%.0f", f.mpPct)}, {"limit", Num("%.0f", r.selfManaBelow)}}));
            return false;
        }
    }
    if (r.leaderHpBelow >= 0.f) {
        if (!f.leaderVisible) { why.Set(Tr("skill_why_leader_not_visible")); return false; }
        if (!(f.leaderHpPct < r.leaderHpBelow)) {
            why.Set(TrF("skill_why_leader_life",
                        {{"value", Num("%.0f", f.leaderHpPct)}, {"limit", Num("%.0f", r.leaderHpBelow)}}));
            return false;
        }
    }
    return true;
}

bool CheckLeaderDistance(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.leaderDistanceMin < 0.f && r.leaderDistanceMax < 0.f) return true;
    if (f.leaderDistance < 0.f) {
        why.Set(Tr("skill_why_leader_distance_unknown"));
        return false;
    }
    if (r.leaderDistanceMin >= 0.f && f.leaderDistance < r.leaderDistanceMin) {
        why.Set(TrF("skill_why_leader_too_close",
                    {{"distance", Num("%.0f", f.leaderDistance)}, {"limit", Num("%.0f", r.leaderDistanceMin)}}));
        return false;
    }
    if (r.leaderDistanceMax >= 0.f && f.leaderDistance > r.leaderDistanceMax) {
        why.Set(TrF("skill_why_leader_too_far",
                    {{"distance", Num("%.0f", f.leaderDistance)}, {"limit", Num("%.0f", r.leaderDistanceMax)}}));
        return false;
    }
    return true;
}

namespace {

// Stack count of a buff (0 = not present). Aggregated lists carry one entry with
// the stacks in `charges`; the raw list may repeat the buff: the larger wins.
int BuffStacks(const std::vector<BuffInfo>& buffs, std::string_view name) {
    int stacks = 0, entries = 0;
    for (const auto& b : buffs) {
        if (!NameEquals(b.name, name)) continue;
        ++entries;
        stacks = (std::max)(stacks, (std::max)(b.charges, 1));
    }
    return (std::max)(stacks, entries);
}

// At most this many bytes of a name go into a reason (cut on a UTF-8 boundary).
constexpr size_t kNameMax = 48;
std::string ShortName(std::string_view v) {
    if (v.size() <= kNameMax) return std::string(v);
    size_t n = kNameMax;
    while (n > 0 && (static_cast<unsigned char>(v[n]) & 0xC0) == 0x80) --n;
    return std::string(v.substr(0, n));
}

}  // namespace

bool CheckBuffs(const SkillRule& r, const CombatFacts& f, Reason& why) {
    const std::string_view missing = TrimView(r.buffMissing);
    const std::string_view present = TrimView(r.buffPresent);
    if (missing.empty() && present.empty()) return true;
    if (!f.selfBuffs || !f.selfBuffsFresh) {
        why.Set(Tr("skill_why_buffs_not_read"));
        return false;
    }
    if (!missing.empty() && BuffStacks(*f.selfBuffs, missing) > 0) {
        why.Set(TrF("skill_why_buff_active", {{"buff", ShortName(missing)}}));
        return false;
    }
    if (!present.empty()) {
        const int stacks = BuffStacks(*f.selfBuffs, present);
        if (stacks == 0) {
            why.Set(TrF("skill_why_buff_not_active", {{"buff", ShortName(present)}}));
            return false;
        }
        if (r.buffMinStacks > 0 && stacks < r.buffMinStacks) {
            why.Set(TrF("skill_why_buff_stacks", {{"buff", ShortName(present)},
                                                  {"stacks", Num("%d", stacks)},
                                                  {"min", Num("%d", r.buffMinStacks)}}));
            return false;
        }
    }
    return true;
}

bool CheckLeaderBuff(const SkillRule& r, const CombatFacts& f, Reason& why) {
    const std::string_view missing = TrimView(r.leaderBuffMissing);
    if (missing.empty()) return true;
    if (!f.leaderVisible) {
        why.Set(Tr("skill_why_leader_not_visible"));
        return false;
    }
    if (!f.leaderBuffs || !f.leaderBuffsFresh) {
        why.Set(Tr("skill_why_leader_buffs_not_read"));
        return false;
    }
    if (BuffStacks(*f.leaderBuffs, missing) > 0) {
        why.Set(TrF("skill_why_leader_has_buff", {{"buff", ShortName(missing)}}));
        return false;
    }
    return true;
}

bool CheckSkillReady(const SkillRule& r, const CombatFacts& f, Reason& why) {
    const std::string_view name = TrimView(r.skillName);
    if (name.empty()) return true;
    if (!f.skills || !f.skillsFresh) {
        why.Set(Tr("skill_why_skills_not_read"));
        return false;
    }
    bool found = false;
    for (const auto& s : *f.skills) {
        if (!NameEquals(s.name, name)) continue;
        found = true;
        if (s.canBeUsed) return true;
    }
    if (!found) why.Set(TrF("skill_why_skill_not_found", {{"skill", ShortName(name)}}));
    else why.Set(TrF("skill_why_skill_not_ready", {{"skill", ShortName(name)}}));
    return false;
}

bool CheckFlask(const SkillRule& r, const CombatFacts& f, Reason& why) {
    if (r.kind != RuleKind::Flask || r.flaskSlot < 0) return true;
    if (!f.belt || !f.beltFresh) {
        why.Set(Tr("skill_why_belt_not_read"));
        return false;
    }
    const FlaskInfo* fl = FindBeltSlot(*f.belt, r.flaskSlot);
    const LocText slot = Tr(BeltSlotName(r.flaskSlot));
    if (!fl) {
        why.Set(TrF("skill_why_slot_empty", {{"slot", slot}}));
        return false;
    }
    const int perUse = (std::max)(fl->perUse, 1);
    if (fl->charges < perUse) {
        why.Set(TrF("skill_why_slot_charges", {{"slot", slot}, {"charges", Num("%d", fl->charges)}, {"needed", Num("%d", perUse)}}));
        return false;
    }
    if (r.flaskSkipIfActive && fl->active) {
        why.Set(TrF("skill_why_slot_active", {{"slot", slot}}));
        return false;
    }
    return true;
}

bool CheckInterval(const SkillRule& r, uint64_t lastFireMs, uint64_t now, Reason& why) {
    if (lastFireMs == 0 || r.minIntervalMs <= 0) return true;
    const uint64_t next = lastFireMs + static_cast<uint64_t>(r.minIntervalMs);
    if (now >= next) return true;
    why.Set(TrF("skill_why_interval", {{"ms", Num("%llu", static_cast<unsigned long long>(next - now))}}));
    return false;
}

namespace {

// The running task for a reason: its key, or "this task".
LocText TaskText(const RuleInputs& in) {
    return in.taskName && in.taskName[0] ? Tr(in.taskName) : Tr("task_this");
}

void DescribeRejects(const TargetPick& p, Reason& why) {
    LocText list;   // "<n> too far, <n> out of sight, ..." as nested text_join entries
    auto add = [&](int n, const char* key) {
        if (n <= 0) return;
        LocText part = TrF(key, {{"count", Num("%d", n)}});
        list = list.empty() ? std::move(part) : TrF("text_join", {{"a", list}, {"b", part}});
    };
    add(p.rejects.tooFar, Key("skill_reject_too_far"));
    add(p.rejects.notEligible, Key("skill_reject_not_eligible"));
    add(p.rejects.noLos, Key("skill_reject_no_los"));
    add(p.rejects.lowRarity, Key("skill_reject_low_rarity"));
    add(p.rejects.outsideLeash, Key("skill_reject_outside_leash"));
    add(p.rejects.blacklisted, Key("skill_reject_blacklisted"));
    add(p.rejects.untargetable, Key("skill_reject_untargetable"));
    if (list.empty()) why.Set(Tr("skill_why_no_target"));
    else why.Set(TrF("skill_why_no_target_rejects", {{"rejects", list}}));
}

}  // namespace

bool ResolveTarget(const SkillRule& r, const CombatFacts& f, const TargetContext& t, RuleTarget& out, Reason& why) {
    out = RuleTarget{};
    out.aim = AimOf(r);
    out.needsCursor = out.aim != AimKind::None;
    if (!out.needsCursor) return true;
    if (!f.projectionValid || !t.proj || !t.proj->valid) {
        why.Set(Tr("skill_why_no_projection"));
        return false;
    }
    const ProjectionData& proj = *t.proj;
    switch (out.aim) {
        case AimKind::Monster: {
            if (!t.monsters || t.monsters->empty()) {
                why.Set(Tr("skill_why_no_enemies"));
                return false;
            }
            TargetQuery q;
            q.mode = r.target;
            q.self = f.self;
            q.leader = f.leader;
            q.leaderVisible = f.leaderVisible;
            q.maxDistance = (std::min)(r.maxTargetDistance, f.combatRadius);
            q.minRarity = r.minRarity;
            q.clusterRadius = kClusterRadius;
            q.requireLos = r.requireLos;
            q.restrictToLeash = f.mode == CombatMode::Assist;
            q.leashRadius = f.leashRadius;
            q.eligible = t.aimable;
            const TargetPick pick = SelectTarget(*t.monsters, q, t.los);
            if (!pick.found) {
                DescribeRejects(pick, why);
                return false;
            }
            const Monster& m = (*t.monsters)[pick.index];
            out.monster = pick.index;
            out.rarity = m.rarity;
            out.grid = m.grid;
            if (t.views && pick.index < t.views->size() && (*t.views)[pick.index].onScreen) {
                out.screen = (*t.views)[pick.index].screen;
            } else if (!ProjectWorld(proj, m.world, out.screen) || !t.safe.Contains(out.screen)) {
                why.Set(Tr("skill_why_target_off_screen"));
                return false;
            }
            return true;
        }
        case AimKind::Leader: {
            if (!f.leaderVisible) {
                why.Set(Tr("skill_why_leader_not_visible"));
                return false;
            }
            out.grid = f.leader;
            if (!ProjectWorld(proj, f.leaderWorld, out.screen) || !t.safe.Contains(out.screen)) {
                why.Set(Tr("skill_why_leader_off_screen"));
                return false;
            }
            return true;
        }
        case AimKind::Self: {
            out.grid = f.self;
            if (!ProjectWorld(proj, f.selfWorld, out.screen)) {
                why.Set(Tr("skill_why_self_off_screen"));
                return false;
            }
            out.screen = t.safe.Clamp(out.screen);
            return true;
        }
        case AimKind::PathAhead: {
            Vec2 point;
            bool have = false;
            if (t.path && PointAlongPath(*t.path, f.self, kMovementAheadCells, point) && Dist(point, f.self) >= 3.f)
                have = true;
            if (!have && f.leaderKnown) {
                const float d = Dist(f.leader, f.self);
                if (d >= 3.f) {
                    point = f.self + Normalize(f.leader - f.self) * (std::min)(kMovementAheadCells, d);
                    have = true;
                }
            }
            if (!have) {
                why.Set(Tr("skill_why_nothing_to_move_to"));
                return false;
            }
            Vec2 playerScreen, pointScreen;
            if (!ProjectWorld(proj, f.selfWorld, playerScreen) ||
                !ProjectGrid(proj, point, f.selfWorld.z, pointScreen)) {
                why.Set(Tr("skill_why_no_path_projection"));
                return false;
            }
            out.grid = point;
            out.screen = ClampAlongRay(t.safe, playerScreen, pointScreen);
            return true;
        }
        case AimKind::None:
            break;
    }
    return true;
}

bool CheckEnemies(const SkillRule& r, const CombatFacts& f, const TargetContext& t, const RuleTarget& target,
                  Reason& why) {
    if (r.minEnemies <= 0) return true;
    static const std::vector<Monster> kNone;
    const std::vector<Monster>& ms = t.monsters ? *t.monsters : kNone;
    if (target.aim == AimKind::Monster) {
        const int n = CountMonstersNear(ms, target.grid, r.enemyRadius, 0, t.usable);
        if (n < r.minEnemies) {
            why.Set(TrF("skill_why_enemies_target", {{"radius", Num("%.0f", r.enemyRadius)},
                                                     {"count", Num("%d", n)},
                                                     {"min", Num("%d", r.minEnemies)}}));
            return false;
        }
        return true;
    }
    const int n = CountMonstersNear(ms, f.self, r.enemyRadius, r.minRarity, t.usable);
    if (n < r.minEnemies) {
        if (r.minRarity > 0)
            why.Set(TrF("skill_why_enemies_rarity", {{"rarity", Tr(RarityName(r.minRarity))},
                                                     {"radius", Num("%.0f", r.enemyRadius)},
                                                     {"count", Num("%d", n)},
                                                     {"min", Num("%d", r.minEnemies)}}));
        else
            why.Set(TrF("skill_why_enemies", {{"radius", Num("%.0f", r.enemyRadius)},
                                              {"count", Num("%d", n)},
                                              {"min", Num("%d", r.minEnemies)}}));
        return false;
    }
    return true;
}

bool CheckCursor(const SkillRule& r, const CombatFacts& f, const RuleTarget& target, Reason& why) {
    if (!f.cursorBusy) return true;
    if (target.needsCursor || IsMouseButtonVk(r.key)) {
        why.Set(Tr("skill_why_cursor_busy"));
        return false;
    }
    return true;
}

bool EvaluateRule(const SkillRule& r, const CombatFacts& f, const TargetContext& t, const RuleInputs& in,
                  RuleTarget& target, Reason& why) {
    why.Clear();
    target = RuleTarget{};
    if (!r.enabled) {
        why.Set(Tr("skill_why_off"));
        return false;
    }
    if (!CheckKey(r, f, why)) return false;
    if (!f.selfValid) {
        why.Set(Tr("skill_why_char_unreadable"));
        return false;
    }
    if (f.selfDead) {
        why.Set(Tr("skill_why_dead"));
        return false;
    }
    if (!in.classAllowed) {
        why.Set(TrF("skill_why_not_while", {{"task", TaskText(in)}}));
        return false;
    }
    if (!CheckCombatMode(r, f, why)) return false;
    if (!CheckCombatState(r, f, why)) return false;
    if (!CheckMoveState(r, f, why)) return false;
    if (!CheckVitals(r, f, why)) return false;
    if (!CheckLeaderDistance(r, f, why)) return false;
    if (!CheckBuffs(r, f, why)) return false;
    if (!CheckLeaderBuff(r, f, why)) return false;
    if (!CheckSkillReady(r, f, why)) return false;
    if (!CheckFlask(r, f, why)) return false;
    if (!in.longPressAllowed && IsLongPress(r)) {
        why.Set(TrF("skill_why_long_press_wait", {{"task", TaskText(in)}}));
        return false;
    }
    if (!CheckInterval(r, in.lastFireMs, f.now, why)) return false;
    if (!ResolveTarget(r, f, t, target, why)) return false;
    if (!CheckEnemies(r, f, t, target, why)) return false;
    if (!CheckCursor(r, f, target, why)) return false;
    return true;
}

}  // namespace fb
