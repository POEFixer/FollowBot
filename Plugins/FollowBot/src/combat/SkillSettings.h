// SkillSettings.h - the skill / flask rule engine (design §5.10): rule struct,
// engine settings and their JSON (per-field fallback + clamp).
//
// A rule fires a key the user binds (the game's key bindings are not readable);
// `skillName` optionally gates it on the skill being ready (CanBeUsed). Rules
// are evaluated top-down: list order is the priority.
//
// Flask rules name a belt slot: 0..1 = flask slots 1..2, 2..4 = charm slots
// 1..3, -1 = the belt is not checked (the key is pressed whenever the other
// conditions hold).
#pragma once

#include "../common/JsonUtil.h"
#include "../core/LocText.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fb {

enum class RuleKind : uint8_t { Attack = 0, Self, LeaderBuff, Movement, Flask, Count };
enum class TargetMode : uint8_t {
    NearestEnemy = 0,
    StrongestEnemy,   // rarity, then HP
    LowestHpEnemy,
    DensestCluster,
    Leader,
    Self,
    CursorUnchanged,
    Count
};
enum class CombatWhen : uint8_t { Always = 0, InCombat, OutOfCombat, Count };
enum class MoveState : uint8_t { Any = 0, Moving, Stationary, Count };
enum class CombatMode : uint8_t {
    Passive = 0,      // Attack rules off
    Assist,           // attack only hostiles within the leash around the leader
    Aggressive,       // any hostile within the combat radius
    Count
};

inline const char* RuleKindName(RuleKind k) {
    switch (k) {
        case RuleKind::Attack:     return "Attack";
        case RuleKind::Self:       return "Self buff";
        case RuleKind::LeaderBuff: return "Leader buff";
        case RuleKind::Movement:   return "Movement";
        case RuleKind::Flask:      return "Flask";
        case RuleKind::Count:      break;
    }
    return "?";
}

// The target mode's key (lang/en.json): the Skills tab's target list.
inline const char* TargetModeName(TargetMode t) {
    switch (t) {
        case TargetMode::NearestEnemy:    return Key("skill_target_nearest");
        case TargetMode::StrongestEnemy:  return Key("skill_target_strongest");
        case TargetMode::LowestHpEnemy:   return Key("skill_target_lowest_life");
        case TargetMode::DensestCluster:  return Key("skill_target_densest");
        case TargetMode::Leader:          return Key("skill_target_leader");
        case TargetMode::Self:            return Key("skill_target_self");
        case TargetMode::CursorUnchanged: return Key("skill_target_cursor");
        case TargetMode::Count:           break;
    }
    return Key("common_unknown");
}

inline const char* CombatWhenName(CombatWhen w) {
    switch (w) {
        case CombatWhen::Always:      return "Always";
        case CombatWhen::InCombat:    return "In combat";
        case CombatWhen::OutOfCombat: return "Out of combat";
        case CombatWhen::Count:       break;
    }
    return "?";
}

inline const char* MoveStateName(MoveState m) {
    switch (m) {
        case MoveState::Any:        return "Any";
        case MoveState::Moving:     return "Moving";
        case MoveState::Stationary: return "Stationary";
        case MoveState::Count:      break;
    }
    return "?";
}

inline const char* CombatModeName(CombatMode m) {
    switch (m) {
        case CombatMode::Passive:    return "Passive";
        case CombatMode::Assist:     return "Assist";
        case CombatMode::Aggressive: return "Aggressive";
        case CombatMode::Count:      break;
    }
    return "?";
}

struct SkillRule {
    bool        enabled = true;
    std::string label = "New rule";
    int         key = 0;                     // VK, mouse buttons included (sent as button events)
    RuleKind    kind = RuleKind::Attack;
    TargetMode  target = TargetMode::NearestEnemy;
    std::string skillName;                   // GrantedEffect id; optional readiness gate
    int         minEnemies = 1;
    float       enemyRadius = 40.f;          // cells around the target (cluster) / us
    int         minRarity = 0;               // 0..3
    float       maxTargetDistance = 50.f;    // cells from us
    bool        requireLos = true;
    float       selfHpBelow = -1.f;          // percent; < 0 = off
    float       selfEsBelow = -1.f;
    float       selfManaAbove = -1.f;
    float       selfManaBelow = -1.f;        // (mana flasks)
    float       leaderHpBelow = -1.f;
    std::string buffMissing;                 // fire only while this buff is NOT on us
    std::string buffPresent;                 // fire only while this buff IS on us ...
    int         buffMinStacks = 0;           // ... with at least this many stacks
    std::string leaderBuffMissing;
    int         minIntervalMs = 500;         // this rule's own cooldown
    CombatWhen  when = CombatWhen::InCombat;
    MoveState   moveState = MoveState::Any;
    float       leaderDistanceMin = -1.f;    // cells; < 0 = off
    float       leaderDistanceMax = -1.f;
    bool        stopToCast = false;          // release the move key before pressing
    int         holdMs = 0;                  // 0 = tap
    int         channelMs = 0;               // > 0 = hold up to this long, re-aiming at the target (channelled skills)
    int         flaskSlot = -1;              // Flask rules: belt slot 0..4 (see above), -1 = not checked
    bool        flaskSkipIfActive = true;
};

struct SkillSettings {
    bool       enabled = true;
    CombatMode mode = CombatMode::Assist;
    float      combatRadius = 50.f;          // a hostile this close (cells) = in combat
    float      leashRadius = 60.f;           // ... and this close to the leader
    float      maxCombatLeaderDistance = 60.f;  // Attack rules pause while the leader is farther
    int        globalCastGateMs = 150;       // min gap between any two casts (+ action gate jitter)
    bool       skipUntargetable = true;      // never aim at a monster the game marks untargetable / hidden
    std::vector<SkillRule> rules;
    std::vector<std::string> monsterBlacklist;  // metadata path substrings never targeted

    SkillSettings() { rules = DefaultRules(); }

    // Two flask examples, disabled: POEFixer's AutoPot may already drink.
    static std::vector<SkillRule> DefaultRules() {
        std::vector<SkillRule> v;
        SkillRule life;
        life.enabled = false;
        life.label = "Life flask";
        life.key = '1';
        life.kind = RuleKind::Flask;
        life.target = TargetMode::Self;
        life.selfHpBelow = 50.f;
        life.flaskSlot = 0;
        life.minEnemies = 0;
        life.when = CombatWhen::Always;
        life.minIntervalMs = 1000;
        v.push_back(life);
        SkillRule mana = life;
        mana.label = "Mana flask";
        mana.key = '2';
        mana.selfHpBelow = -1.f;
        mana.selfManaBelow = 35.f;
        mana.flaskSlot = 1;
        v.push_back(mana);
        return v;
    }
};

// Cuts a UTF-8 string to at most `maxBytes` without splitting a character.
inline void TruncateUtf8(std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
}

inline void Clamp(SkillRule& r) {
    TruncateUtf8(r.label, 64);
    TruncateUtf8(r.skillName, 128);
    TruncateUtf8(r.buffMissing, 128);
    TruncateUtf8(r.buffPresent, 128);
    TruncateUtf8(r.leaderBuffMissing, 128);
    r.key = std::clamp(r.key, 0, 254);
    r.minEnemies = std::clamp(r.minEnemies, 0, 50);
    r.enemyRadius = std::clamp(r.enemyRadius, 1.f, 150.f);
    r.minRarity = std::clamp(r.minRarity, 0, 3);
    r.maxTargetDistance = std::clamp(r.maxTargetDistance, 1.f, 150.f);
    auto pct = [](float& v) { if (!(v >= 0.f)) v = -1.f; else v = (std::min)(v, 100.f); };
    pct(r.selfHpBelow);
    pct(r.selfEsBelow);
    pct(r.selfManaAbove);
    pct(r.selfManaBelow);
    pct(r.leaderHpBelow);
    r.buffMinStacks = std::clamp(r.buffMinStacks, 0, 999);
    r.minIntervalMs = std::clamp(r.minIntervalMs, 0, 600000);
    auto dist = [](float& v) { if (!(v >= 0.f)) v = -1.f; else v = (std::min)(v, 300.f); };
    dist(r.leaderDistanceMin);
    dist(r.leaderDistanceMax);
    r.holdMs = std::clamp(r.holdMs, 0, 5000);
    r.channelMs = std::clamp(r.channelMs, 0, 10000);
    r.flaskSlot = std::clamp(r.flaskSlot, -1, 4);
}

inline void Clamp(SkillSettings& s) {
    s.combatRadius = std::clamp(s.combatRadius, 5.f, 150.f);
    s.leashRadius = std::clamp(s.leashRadius, 5.f, 200.f);
    s.maxCombatLeaderDistance = std::clamp(s.maxCombatLeaderDistance, 10.f, 300.f);
    s.globalCastGateMs = std::clamp(s.globalCastGateMs, 50, 2000);
    if (s.rules.size() > 64) s.rules.resize(64);
    for (auto& r : s.rules) Clamp(r);
    if (s.monsterBlacklist.size() > 200) s.monsterBlacklist.resize(200);
    for (auto& b : s.monsterBlacklist) TruncateUtf8(b, 200);
}

inline void ToJson(json::Json& j, const SkillRule& r) {
    j = json::Json::object();
    j["enabled"] = r.enabled;
    j["label"] = r.label;
    j["key"] = r.key;
    j["kind"] = static_cast<int>(r.kind);
    j["target"] = static_cast<int>(r.target);
    j["skill_name"] = r.skillName;
    j["min_enemies"] = r.minEnemies;
    j["enemy_radius"] = r.enemyRadius;
    j["min_rarity"] = r.minRarity;
    j["max_target_distance"] = r.maxTargetDistance;
    j["require_los"] = r.requireLos;
    j["self_hp_below"] = r.selfHpBelow;
    j["self_es_below"] = r.selfEsBelow;
    j["self_mana_above"] = r.selfManaAbove;
    j["self_mana_below"] = r.selfManaBelow;
    j["leader_hp_below"] = r.leaderHpBelow;
    j["buff_missing"] = r.buffMissing;
    j["buff_present"] = r.buffPresent;
    j["buff_min_stacks"] = r.buffMinStacks;
    j["leader_buff_missing"] = r.leaderBuffMissing;
    j["min_interval_ms"] = r.minIntervalMs;
    j["when"] = static_cast<int>(r.when);
    j["move_state"] = static_cast<int>(r.moveState);
    j["leader_distance_min"] = r.leaderDistanceMin;
    j["leader_distance_max"] = r.leaderDistanceMax;
    j["stop_to_cast"] = r.stopToCast;
    j["hold_ms"] = r.holdMs;
    j["channel_ms"] = r.channelMs;
    j["flask_slot"] = r.flaskSlot;
    j["flask_skip_if_active"] = r.flaskSkipIfActive;
}

inline void FromJson(const json::Json& j, SkillRule& r) {
    using json::Read;
    Read(j, "enabled", r.enabled);
    Read(j, "label", r.label);
    Read(j, "key", r.key);
    json::ReadEnum(j, "kind", r.kind, static_cast<int>(RuleKind::Count));
    json::ReadEnum(j, "target", r.target, static_cast<int>(TargetMode::Count));
    Read(j, "skill_name", r.skillName);
    Read(j, "min_enemies", r.minEnemies);
    Read(j, "enemy_radius", r.enemyRadius);
    Read(j, "min_rarity", r.minRarity);
    Read(j, "max_target_distance", r.maxTargetDistance);
    Read(j, "require_los", r.requireLos);
    Read(j, "self_hp_below", r.selfHpBelow);
    Read(j, "self_es_below", r.selfEsBelow);
    Read(j, "self_mana_above", r.selfManaAbove);
    Read(j, "self_mana_below", r.selfManaBelow);
    Read(j, "leader_hp_below", r.leaderHpBelow);
    Read(j, "buff_missing", r.buffMissing);
    Read(j, "buff_present", r.buffPresent);
    Read(j, "buff_min_stacks", r.buffMinStacks);
    Read(j, "leader_buff_missing", r.leaderBuffMissing);
    Read(j, "min_interval_ms", r.minIntervalMs);
    json::ReadEnum(j, "when", r.when, static_cast<int>(CombatWhen::Count));
    json::ReadEnum(j, "move_state", r.moveState, static_cast<int>(MoveState::Count));
    Read(j, "leader_distance_min", r.leaderDistanceMin);
    Read(j, "leader_distance_max", r.leaderDistanceMax);
    Read(j, "stop_to_cast", r.stopToCast);
    Read(j, "hold_ms", r.holdMs);
    Read(j, "channel_ms", r.channelMs);
    Read(j, "flask_slot", r.flaskSlot);
    Read(j, "flask_skip_if_active", r.flaskSkipIfActive);
    Clamp(r);
}

inline void ToJson(json::Json& j, const SkillSettings& s) {
    j = json::Json::object();
    j["enabled"] = s.enabled;
    j["mode"] = static_cast<int>(s.mode);
    j["combat_radius"] = s.combatRadius;
    j["leash_radius"] = s.leashRadius;
    j["max_combat_leader_distance"] = s.maxCombatLeaderDistance;
    j["global_cast_gate_ms"] = s.globalCastGateMs;
    j["skip_untargetable"] = s.skipUntargetable;
    json::Json rules = json::Json::array();
    for (const auto& r : s.rules) {
        json::Json jr;
        ToJson(jr, r);
        rules.push_back(std::move(jr));
    }
    j["rules"] = std::move(rules);
    j["monster_blacklist"] = s.monsterBlacklist;
}

inline void FromJson(const json::Json& j, SkillSettings& s) {
    using json::Read;
    Read(j, "enabled", s.enabled);
    json::ReadEnum(j, "mode", s.mode, static_cast<int>(CombatMode::Count));
    Read(j, "combat_radius", s.combatRadius);
    Read(j, "leash_radius", s.leashRadius);
    Read(j, "max_combat_leader_distance", s.maxCombatLeaderDistance);
    Read(j, "global_cast_gate_ms", s.globalCastGateMs);
    Read(j, "skip_untargetable", s.skipUntargetable);
    if (j.is_object() && j.contains("rules") && j["rules"].is_array()) {
        s.rules.clear();
        for (const auto& jr : j["rules"]) {
            if (!jr.is_object()) continue;
            SkillRule r;
            FromJson(jr, r);
            s.rules.push_back(std::move(r));
        }
    }
    json::ReadStringList(j, "monster_blacklist", s.monsterBlacklist);
    Clamp(s);
}

}  // namespace fb
