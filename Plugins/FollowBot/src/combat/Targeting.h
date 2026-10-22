// Targeting.h - which hostile a skill rule aims at (design §5.10). Pure.
//
// Contract (phase 2 implements Targeting.cpp; this interface is final):
// candidates = World::monsters (already hostile + alive) that are targetable,
// not blacklisted by metadata path substring, within maxDistance of us, of at
// least minRarity, inside the leash around the leader when restrictToLeash,
// with line of sight when requireLos (the `los` callback, walkable grid).
// Modes: NearestEnemy, StrongestEnemy (rarity, then HP), LowestHpEnemy,
// DensestCluster (the candidate with most hostiles within clusterRadius).
// Leader / Self / CursorUnchanged are not monster modes (found = false).
//
// Additions (non-breaking): `eligible` lets the caller drop monsters it already
// judged unusable (off screen, untargetable per the game); TargetPick counts
// why candidates were dropped (the rule's "why not firing" line). Line of sight
// is only asked for a candidate that would beat the current best, so a mode
// costs at most one LOS test per improvement, not one per monster.
#pragma once

#include "../world/World.h"
#include "SkillSettings.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

struct TargetQuery {
    TargetMode mode = TargetMode::NearestEnemy;
    Vec2  self;
    Vec2  leader;
    bool  leaderVisible = false;
    float maxDistance = 50.f;
    int   minRarity = 0;
    float clusterRadius = 12.f;
    bool  requireLos = true;
    bool  restrictToLeash = false;
    float leashRadius = 60.f;
    const std::vector<std::string>* blacklist = nullptr;
    // Optional per-monster mask (same size as the monster list): 0 = skip.
    const std::vector<uint8_t>* eligible = nullptr;
};

// Why candidates were dropped (each monster counted once, first reason wins).
struct TargetRejects {
    int notEligible = 0;    // the caller's mask (off screen / untargetable)
    int untargetable = 0;   // Monster::targetable false
    int blacklisted = 0;
    int tooFar = 0;
    int lowRarity = 0;
    int outsideLeash = 0;
    int noLos = 0;
};

struct TargetPick {
    bool   found = false;
    size_t index = 0;            // into the monster list
    Vec2   grid;
    int    clusterCount = 0;     // hostiles within clusterRadius of the pick
    int    candidates = 0;       // monsters that passed every cheap filter (before LOS)
    TargetRejects rejects;
};

using LosFn = std::function<bool(const Vec2& from, const Vec2& to)>;

TargetPick SelectTarget(const std::vector<Monster>& monsters, const TargetQuery& q, const LosFn& los);

// Hostiles within `radius` of `center`.
int CountMonstersNear(const std::vector<Monster>& monsters, const Vec2& center, float radius);

// Same, counting only monsters of at least `minRarity` and (with a mask) whose
// mask entry is non-zero.
int CountMonstersNear(const std::vector<Monster>& monsters, const Vec2& center, float radius, int minRarity,
                      const std::vector<uint8_t>* mask);

// Spaces / tabs / line breaks cut from both ends (no allocation).
std::string_view TrimView(std::string_view s);

// ASCII case-insensitive equality / substring test without allocating
// (metadata paths, buff and skill ids are ASCII). An empty needle never matches.
bool EqualsAsciiCI(std::string_view a, std::string_view b);
bool ContainsAsciiCI(std::string_view hay, std::string_view needle);

// True when `path` contains one of the (trimmed, non-empty) blacklist entries.
bool IsBlacklistedPath(std::string_view path, const std::vector<std::string>* blacklist);

}  // namespace fb
