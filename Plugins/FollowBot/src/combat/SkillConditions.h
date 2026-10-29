// SkillConditions.h - the pure half of the skill / flask rule engine (design
// §5.10): every condition a rule can carry, one function per condition family,
// target resolution and the whole-rule evaluation that returns the FIRST
// failing condition as a short reason (the "why not firing" line). No SDK, no
// input: SkillEngine.cpp feeds it the tick's facts and sends the press.
//
// Implemented in Targeting.cpp (the combat module's pure translation unit; the
// project's file list is fixed) and tested by tests/test_combat.cpp.
//
// Units: distances in grid cells, vitals in percent (0..100), times in ms.
#pragma once

#include "../world/Projection.h"
#include "../world/World.h"
#include "SkillSettings.h"
#include "Targeting.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fb {

// Presses up to this long are sent at once (the actuator blocks for them);
// longer holds and channels are tracked across ticks instead.
inline constexpr int kBlockingPressMaxMs = 120;
// Cluster radius of the DensestCluster mode (design: 12 cells).
inline constexpr float kClusterRadius = 12.f;
// Movement rules aim this far ahead on the path (design: 25 cells).
inline constexpr float kMovementAheadCells = 25.f;
// Read-on-request data older than this does not count (ms).
inline constexpr uint64_t kSkillsMaxAgeMs = 1000;
inline constexpr uint64_t kSelfBuffsMaxAgeMs = 1000;
inline constexpr uint64_t kLeaderBuffsMaxAgeMs = 1500;
inline constexpr uint64_t kBeltMaxAgeMs = 1000;

inline bool IsMouseButtonVk(int vk) { return vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06; }

const char* RarityName(int rarity);   // a key: "normal", "magic", "rare", "unique" (lower case)

// Trimmed, ASCII case-insensitive name equality (buff / skill ids); empty never matches.
bool NameEquals(std::string_view a, std::string_view b);

// --- moving / standing still -------------------------------------------------
// From the character's recent positions (one sample per tick) and its action
// flags (0x1080 = running, Actor action flags).
class MotionTracker {
public:
    static constexpr uint32_t kRunFlags = 0x1080;

    void Reset() {
        m_count = 0;
        m_head = 0;
    }
    // A zone change or a jump of more than 40 cells starts a new history.
    void Add(uint64_t now, const Vec2& pos, uint32_t area);
    // Moving = the run flags are set, or the speed over the last `windowMs` is
    // at least `minSpeed` cells/s (needs >= windowMs/3 of history).
    bool Moving(uint64_t now, uint32_t actionFlags, int windowMs = 300, float minSpeed = 2.f) const;

private:
    struct Sample {
        uint64_t t = 0;
        Vec2     pos;
    };
    static constexpr int kSamples = 32;
    const Sample& At(int back) const {   // 0 = newest
        return m_samples[static_cast<size_t>((m_head - 1 - back + kSamples * 2) % kSamples)];
    }
    std::array<Sample, kSamples> m_samples{};
    int      m_head = 0;
    int      m_count = 0;
    uint32_t m_area = 0;
};

// --- belt ----------------------------------------------------------------------
// Flask rules address the belt as 0..1 = flask slots, 2..4 = charm slots 0..2.
inline constexpr int kBeltSlots = 5;
int BeltIndexOf(const FlaskInfo& f);
const FlaskInfo* FindBeltSlot(const std::vector<FlaskInfo>& belt, int beltIndex);
const char* BeltSlotName(int beltIndex);   // a key: "Flask 1" .. "Charm 3", "not checked" for -1

// --- what a rule aims at -----------------------------------------------------------
enum class AimKind : uint8_t { None = 0, Monster, Leader, Self, PathAhead };

// Flask: none. Movement: the path ahead. Self / LeaderBuff: the player / the
// leader (CursorUnchanged: none). Attack: the target mode.
AimKind AimOf(const SkillRule& r);
// The target mode a rule of this kind starts with (the UI resets it on a kind change).
TargetMode NaturalTarget(RuleKind k);
// channelMs when > 0, else holdMs (0 = a tap).
inline int PressMsOf(const SkillRule& r) { return r.channelMs > 0 ? r.channelMs : r.holdMs; }
inline bool IsLongPress(const SkillRule& r) { return PressMsOf(r) > kBlockingPressMaxMs; }

// A short reason: a key and its arguments (the UI translates it). The engine
// copies it into its per-rule list by assignment, which reuses the strings'
// storage tick after tick.
struct Reason {
    LocText text;
    void Set(LocText t) { text = std::move(t); }
    void Clear() { text.clear(); }
    bool Empty() const { return text.empty(); }
};

// Everything the conditions read, gathered once per tick.
struct CombatFacts {
    uint64_t now = 0;
    // Self.
    bool  selfValid = false, selfDead = false;
    Vec2  self;
    Vec3  selfWorld;
    float hpPct = 0.f, esPct = 0.f, mpPct = 0.f;
    int   hpMax = 0, esMax = 0, mpMax = 0;
    bool  moving = false;
    // Leader.
    bool  leaderVisible = false;
    bool  leaderKnown = false;        // visible, or last seen in this area
    Vec2  leader;                     // live while visible, else his last position in this area
    Vec3  leaderWorld;
    float leaderDistance = -1.f;      // cells; -1 = unknown
    float leaderHpPct = 100.f;
    // Combat.
    bool  inCombat = false;
    int   hostiles = 0;               // in-combat hostiles (combat radius and leash)
    CombatMode mode = CombatMode::Assist;
    float combatRadius = 50.f, leashRadius = 60.f, maxCombatLeaderDistance = 60.f;
    // Read on request (null or stale = not read yet).
    const std::vector<SkillInfo>* skills = nullptr;
    bool  skillsFresh = false;
    const std::vector<BuffInfo>* selfBuffs = nullptr;
    bool  selfBuffsFresh = false;
    const std::vector<BuffInfo>* leaderBuffs = nullptr;
    bool  leaderBuffsFresh = false;
    const std::vector<FlaskInfo>* belt = nullptr;
    bool  beltFresh = false;
    // Input.
    bool  cursorBusy = false;         // another plugin owns the cursor this tick
    bool  projectionValid = false;
    // Keys a rule may not press: the bot's hotkeys, the move key, the sprint key.
    std::array<int, 8> reserved{};
    int   reservedCount = 0;
    void Reserve(int vk) {
        if (vk > 0 && reservedCount < static_cast<int>(reserved.size()) && !IsReserved(vk))
            reserved[static_cast<size_t>(reservedCount++)] = vk;
    }
    bool IsReserved(int vk) const {
        for (int i = 0; i < reservedCount; ++i)
            if (reserved[static_cast<size_t>(i)] == vk) return true;
        return false;
    }
};

// Per monster, per tick: usable (targetable per the world and the game, not
// blacklisted) and its aim point when it lies inside the safe rect.
struct MonsterView {
    bool usable = false;
    bool onScreen = false;
    Vec2 screen;
};

// `targetableState` (optional, same size): 1 targetable, 0 not (the game says
// untargetable or hidden), -1 unknown. Fills `views`, `usable` (combat-relevant)
// and `aimable` (usable and on screen), reusing their storage.
void ClassifyMonsters(const std::vector<Monster>& monsters, const ProjectionData& proj, const SafeRect& safe,
                      const std::vector<std::string>* blacklist, const std::vector<int8_t>* targetableState,
                      bool skipUntargetable, std::vector<MonsterView>& views, std::vector<uint8_t>& usable,
                      std::vector<uint8_t>& aimable);

// In combat = a usable hostile within combatRadius of us and (while the leader
// is visible) within leashRadius of him.
struct CombatState {
    bool inCombat = false;
    int  hostiles = 0;
};
CombatState ComputeCombat(const std::vector<Monster>& monsters, const std::vector<uint8_t>& usable, const Vec2& self,
                          bool leaderVisible, const Vec2& leader, float combatRadius, float leashRadius);

// The point `cells` path-length ahead of the point of `path` nearest to `from`
// (the end when the path is shorter); false for fewer than two points.
bool PointAlongPath(const std::vector<Vec2>& path, const Vec2& from, float cells, Vec2& out);

// Per-tick inputs for target resolution.
struct TargetContext {
    const std::vector<Monster>*     monsters = nullptr;
    const std::vector<MonsterView>* views = nullptr;
    const std::vector<uint8_t>*     usable = nullptr;
    const std::vector<uint8_t>*     aimable = nullptr;
    LosFn                           los;            // empty = always clear
    const ProjectionData*           proj = nullptr;
    SafeRect                        safe;
    const std::vector<Vec2>*        path = nullptr; // the path being walked (null / empty: standing)
};

struct RuleTarget {
    AimKind aim = AimKind::None;
    bool    needsCursor = false;
    size_t  monster = 0;      // AimKind::Monster
    int     rarity = -1;      // AimKind::Monster
    Vec2    grid;             // monster / leader / self / path point
    Vec2    screen;           // aim point in client px (needsCursor)
};

// --- one function per condition family (true = holds; else `why` says why) ----
bool CheckKey(const SkillRule& r, const CombatFacts& f, Reason& why);
bool CheckCombatMode(const SkillRule& r, const CombatFacts& f, Reason& why);    // Passive, leader too far
bool CheckCombatState(const SkillRule& r, const CombatFacts& f, Reason& why);   // in / out of combat
bool CheckMoveState(const SkillRule& r, const CombatFacts& f, Reason& why);
bool CheckVitals(const SkillRule& r, const CombatFacts& f, Reason& why);        // own life / ES / mana, leader life
bool CheckLeaderDistance(const SkillRule& r, const CombatFacts& f, Reason& why);
bool CheckBuffs(const SkillRule& r, const CombatFacts& f, Reason& why);         // own buff missing / present + stacks
bool CheckLeaderBuff(const SkillRule& r, const CombatFacts& f, Reason& why);
bool CheckSkillReady(const SkillRule& r, const CombatFacts& f, Reason& why);
bool CheckFlask(const SkillRule& r, const CombatFacts& f, Reason& why);
bool CheckInterval(const SkillRule& r, uint64_t lastFireMs, uint64_t now, Reason& why);
bool ResolveTarget(const SkillRule& r, const CombatFacts& f, const TargetContext& t, RuleTarget& out, Reason& why);
bool CheckEnemies(const SkillRule& r, const CombatFacts& f, const TargetContext& t, const RuleTarget& target,
                  Reason& why);
bool CheckCursor(const SkillRule& r, const CombatFacts& f, const RuleTarget& target, Reason& why);

// Engine-side facts about one rule.
struct RuleInputs {
    uint64_t    lastFireMs = 0;
    bool        classAllowed = true;     // the running task allows this cast class
    bool        longPressAllowed = true; // holds / channels may start now (only while following)
    const char* taskName = "";            // the running task's key ("" = none)
};

// The whole rule, cheapest and most telling conditions first. True = it may
// fire now (`target` says where to aim); false = `why` holds the first failing
// condition.
bool EvaluateRule(const SkillRule& r, const CombatFacts& f, const TargetContext& t, const RuleInputs& in,
                  RuleTarget& target, Reason& why);

}  // namespace fb
