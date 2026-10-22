// BotView.h - the immutable picture the worker publishes after every tick for
// the render thread: the debug overlay, the status window and the settings
// tabs' diagnostics read only this (shared_ptr<const BotView>).
#pragma once

#include "../world/World.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fb {

struct InteractableView {
    uint32_t     id = 0;
    InteractKind kind = InteractKind::Other;
    int          portalType = -1;
    Vec2         grid;
    Vec3         world;
    bool         targetable = true;
    bool         visibleNow = false;
};

struct MarkerView {
    Vec2        grid;
    uint32_t    color = 0xFFFFFFFFu;
    std::string label;
};

struct BlockView {
    Vec2 center;
    int  radius = 0;
};

struct BotView {
    uint64_t    nowMs = 0;
    uint64_t    tick = 0;
    bool        running = false;
    bool        paused = false;
    bool        debugWalk = false;
    std::string gateReason;          // empty = not holding
    std::string mode;                // one line: what the bot does
    std::string taskName;            // active exclusive task ("" = Follow)
    std::string taskDetail;
    std::string lastError;
    std::string debugResult;         // last debug-tool result line

    // Game.
    bool        attached = false;
    bool        inGame = false;
    bool        foreground = false;
    std::string areaId;
    uint32_t    areaCounter = 0;
    bool        isTown = false, isHideout = false, isMap = false;
    std::string characterName;
    int         profileIndex = 0;
    std::string profileName;
    bool        profileAuto = false;
    bool        hostHasControlNames = false, hostHasStashTabs = false, hostHasStashObjects = false;

    // Self / leader.
    Vec2        selfGrid;
    Vec3        selfWorld;
    float       hpPct = 0.f;
    bool        leaderConfigured = false;
    bool        leaderVisible = false;
    std::string leaderName;
    Vec2        leaderGrid;
    Vec3        leaderWorld;
    Vec2        leaderLastGrid;
    float       leaderDistance = -1.f;
    float       leaderSpeed = 0.f;
    VanishKind  vanish = VanishKind::None;
    std::vector<Vec2> leaderRoute;
    std::vector<std::string> nearbyPlayers;

    // Movement.
    std::vector<Vec2> path;
    std::string planStatus;
    bool        hasGoal = false;
    Vec2        goal;
    bool        hasLookahead = false;
    Vec2        lookahead;
    bool        hasCursor = false;
    Vec2        cursorClient;
    bool        moving = false;
    bool        sprinting = false;
    float       followDistance = 0.f, stopDistance = 0.f;
    std::string stuckStep;
    int         stuckEpisodes = 0;
    std::vector<Vec2> stuckMarks;
    std::vector<BlockView> blocks;

    // World objects.
    std::vector<InteractableView> interactables;
    std::vector<MarkerView> markers;         // task markers (loot targets, blacklisted, ...)
    int         monsters = 0;
    int         groundItems = 0;
    int         usedCells = 0;
    bool        inventoryValid = false;

    // Combat.
    float       combatRadius = 0.f, leashRadius = 0.f;
    std::string currentRule;
    std::vector<std::string> ruleWhyNot;

    // Session.
    int         deathsMap = 0, deathsSession = 0;
    uint64_t    runMs = 0;

    // Read on request for the Debug / Skills tabs.
    std::vector<SkillInfo> skills;
    std::vector<BuffInfo>  selfBuffs;
    std::vector<BuffInfo>  leaderBuffs;

    // Projection of this tick (the overlay re-projects with a fresh matrix
    // when it can; this is the fallback) and the terrain height.
    ProjectionData proj;
};

}  // namespace fb
