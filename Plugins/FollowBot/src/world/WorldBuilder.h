// WorldBuilder.h - fills the per-tick World from the PluginSDK (design §5.1).
// The ONLY place the core reads the game; worker thread only.
//
// Cadences: gate / self / leader every tick (the leader's position live via
// Components.ReadRender: far entities refresh only every 10th host tick);
// monsters every tick (type-filtered snapshot); ground items every 150 ms when
// requested, else 500 ms; interactables every 250 ms and at once after an area
// change (AreaTransition, Renderable, NPC snapshots); the leader's route every
// 100 ms; inventories every 200 ms when requested, else 400 ms (Inventory.Scan,
// read >= 80 ms later); the walkable grid when its Data() pointer changes
// (polled every 250 ms; the host parses a new area's grid after the area
// event); skills / own buffs every 250 ms and leader buffs every 500 ms, only
// while requested.
#pragma once

#include "Interactables.h"
#include "LeaderTracker.h"
#include "World.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace PluginSDK {
struct Context;
}

namespace fb {

struct WorldRequests;   // brain/Task.h

struct BuildParams {
    LeaderParams leader;
    float monsterRange = 150.f;   // cells
    float itemRange = 120.f;
    // The bot runs (or a debug walk): in town / hideout the host's wider entity
    // scan and its stash-object list are requested (host 365+), never otherwise.
    bool  botActive = false;
};

class WorldBuilder {
public:
    explicit WorldBuilder(const PluginSDK::Context* sdk) : m_sdk(sdk) {}

    void Build(World& w, const WorldRequests& req, const BuildParams& p, uint64_t now);
    void Reset();   // forget every cache (start, disable, game detached)

    const LeaderTracker& Leader() const { return m_leader; }
    uint64_t NavGeneration() const { return m_navGen; }
    // How long the last grid build took (ms) and its size (Diagnostics).
    double LastNavBuildMs() const { return m_navBuildMs; }

private:
    void BuildInteractables(World& w, uint64_t now);
    void BuildItems(World& w);
    void BuildInventory(World& w, bool fast, uint64_t now);
    void BuildNav(World& w, uint64_t now);
    void BuildRequested(World& w, const WorldRequests& req, uint64_t now);
    void UpdateScanFlags(const GameGate& g, bool botActive);
    void MergeStashObjects(std::vector<Interactable>& visible);

    const PluginSDK::Context* m_sdk = nullptr;
    LeaderTracker      m_leader;
    InteractableMemory m_interactables;
    uint64_t m_lastInteractMs = 0;
    uint32_t m_area = 0;
    bool     m_haveArea = false;

    std::vector<GroundItem> m_items;
    uint64_t m_lastItemsMs = 0;

    // Host entity scan flags this plugin requests (EntityScan::*), changed only
    // on transitions; m_scanFlagsSent false = unknown (sent on the next Build).
    uint32_t m_scanFlags = 0;
    bool     m_scanFlagsSent = false;
    uint64_t m_stashDiagSig = 0;   // what the Stash tab's diagnostics last got
    uint64_t m_occMask = ~0ull;    // backpack occupancy the diagnostics last got

    std::vector<Vec2> m_route;
    uint64_t m_lastRouteMs = 0;
    uint32_t m_routeLeaderId = 0;

    // Inventory: scan requested at m_scanMs, read once m_scanMs + 80 passed.
    uint64_t m_scanMs = 0;
    bool     m_scanPending = false;
    InventoryState m_inv;

    // Walkable grid.
    const uint8_t* m_gridPtr = nullptr;
    int      m_gridW = 0, m_gridH = 0;
    uint64_t m_lastGridPollMs = 0;
    uint64_t m_navGen = 0;
    double   m_navBuildMs = 0.0;
    std::shared_ptr<const NavGrid> m_nav;
    std::shared_ptr<const HeightField> m_height;

    std::string m_selfName;
    uint32_t m_selfNameArea = 0;

    std::vector<SkillInfo> m_skills;   uint64_t m_skillsMs = 0;
    std::vector<BuffInfo>  m_selfBuffs; uint64_t m_selfBuffsMs = 0;
    std::vector<BuffInfo>  m_leaderBuffs; uint64_t m_leaderBuffsMs = 0;
    std::vector<FlaskInfo> m_flasks;   uint64_t m_flasksMs = 0;
    uint64_t m_tick = 0;
};

}  // namespace fb
