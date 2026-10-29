// SkillEngine.h - the skill / flask rule engine (design §5.10). Evaluated every
// tick by the Brain (step 6), interleaved with whatever task runs.
//
// Contract (phase 2 implements SkillEngine.cpp; this interface is final):
//  - TryCast: when skills are enabled, evaluate the rules top-down; the first
//    rule whose conditions all hold and whose own minIntervalMs elapsed fires,
//    provided `current` allows its cast class (ClassOf) and the global cast gate
//    (globalCastGateMs + the actuator's action gate) is open. Conditions: combat
//    state (a hostile within combatRadius and within leashRadius of the leader),
//    combat mode (Passive: no Attack rules; Assist: targets inside the leash;
//    Aggressive: any within combatRadius), Attack rules suppressed while the
//    leader is farther than maxCombatLeaderDistance, enemies / rarity / radius,
//    vitals (self HP / ES / mana, leader HP), buffs (missing / present + stacks,
//    leader buff missing), skill readiness (skillName found in w.skills and
//    CanBeUsed), moving / stationary, leader distance window. Flask rules: the
//    belt slot's charges >= per use, skip if active.
//    Execution: aim at the target's screen point (safe rect; off-screen
//    targets skipped), stopToCast releases the move key first, press (tap /
//    holdMs / channelMs) through the Actuator (mouse-button keys at the aimed
//    point). Self rules aim at the player, Leader rules at the leader,
//    Movement rules 25 cells ahead on the path. Sets req.skills / selfBuffs /
//    leaderBuffs / flasks while rules need them; mem.currentRule = the label.
//    Returns true when a cast was sent (the Brain skips steering this tick).
//  - WhyNot: per rule, the first failing condition on the last evaluation.
//
// Implementation notes (phase 2):
//  - The conditions are pure (SkillConditions.h, tested); this class gathers
//    the tick's facts, reads the game's Targetable flag of nearby monsters
//    (cached, a bounded number of reads per tick) and sends the press.
//  - Taps and presses up to kBlockingPressMaxMs go out at once. Longer holds
//    and channels are tracked across ticks (the worker never sleeps through
//    them): the key is held, TryCast keeps returning true (no steering), a
//    channel re-aims at its target every tick and ends early when the target
//    is gone; the key is released when the time is up, the rule changes, a
//    task takes over, or input is released anywhere (gate hold, stop). Holds
//    and channels start only while the bot follows (`current` is the Follow
//    task) and always stand still.
//  - A tap that aimed while the move key stays held puts the cursor back where
//    the Mover had it, so walking continues in the same direction.
#pragma once

#include "../brain/Task.h"
#include "SkillConditions.h"
#include "SkillSettings.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fb {

class SkillEngine {
public:
    SkillEngine();

    bool TryCast(BotContext& ctx, const Task* current);
    void Reset();

    const std::vector<LocText>& WhyNot() const { return m_whyNot; }
    const std::string& LastRule() const { return m_lastRule; }
    // A hold or channel is in progress (its key is down).
    bool HoldActive() const { return m_hold.active; }

    static CastClass ClassOf(RuleKind k) {
        switch (k) {
            case RuleKind::Attack:     return CastClass::Attack;
            case RuleKind::Movement:   return CastClass::Movement;
            case RuleKind::Flask:      return CastClass::Flask;
            case RuleKind::Self:
            case RuleKind::LeaderBuff:
            case RuleKind::Count:      break;
        }
        return CastClass::Self;
    }

private:
    // A press held across ticks (hold / channel).
    struct Hold {
        bool        active = false;
        size_t      rule = 0;
        int         key = 0;
        bool        channel = false;
        bool        needsCursor = false;
        AimKind     aim = AimKind::None;
        Vec2        aimScreen;
        Vec2        targetGrid;
        uint64_t    startMs = 0;
        uint64_t    untilMs = 0;
        uint64_t    minUntilMs = 0;   // a channel is not cut short before this
        std::string label;
    };

    // Line of sight per target cell, valid for one tick (the origin is always
    // the character). Fixed open-addressing table: no allocation.
    struct LosCache {
        static constexpr size_t kSize = 512;
        struct Slot {
            uint64_t key = 0;
            uint32_t gen = 0;
            bool     clear = false;
        };
        std::array<Slot, kSize> slots{};
        uint32_t gen = 1;
        void NewTick() {
            if (++gen == 0) {
                slots.fill(Slot{});
                gen = 1;
            }
        }
        bool Find(uint64_t key, bool& clear) const;
        void Put(uint64_t key, bool clear);
    };

    struct TargetableEntry {
        int8_t   state = -1;    // 1 targetable, 0 not, -1 unknown
        uint64_t readMs = 0;
        uint64_t seenMs = 0;
    };

    void SetRequests(WorldRequests& req, const SkillSettings& sk) const;
    void BuildFrame(BotContext& ctx);
    void UpdateTargetable(BotContext& ctx);
    bool ContinueHold(BotContext& ctx, const Task* current, bool followPath);
    void ReleaseHold(BotContext& ctx, const char* why);   // `why`: a key
    bool Fire(BotContext& ctx, size_t index, const RuleTarget& target);
    bool LineOfSight(const Vec2& from, const Vec2& to);
    void SetWhy(size_t index, const LocText& text);

    std::vector<LocText>     m_whyNot;
    std::vector<uint64_t>    m_lastFireMs;   // per rule index
    std::string              m_lastRule;
    uint64_t                 m_lastCastMs = 0;
    uint64_t                 m_nextCastMs = 0;   // global cast gate

    // Per-tick frame (storage reused).
    MotionTracker            m_motion;
    CombatFacts              m_facts;
    TargetContext            m_tctx;
    std::vector<MonsterView> m_views;
    std::vector<uint8_t>     m_usable, m_aimable;
    std::vector<int8_t>      m_targetableState;
    LosCache                 m_los;
    const PathPlanner*       m_losPlanner = nullptr;

    // Targetable flags of monsters by entity id (per area).
    std::unordered_map<uint32_t, TargetableEntry> m_targetable;
    uint32_t                 m_targetableArea = 0;
    uint64_t                 m_lastPruneMs = 0;

    Hold                     m_hold;
};

}  // namespace fb
