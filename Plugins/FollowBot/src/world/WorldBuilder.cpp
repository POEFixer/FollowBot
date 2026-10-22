// WorldBuilder.cpp - see WorldBuilder.h.
// FB_TESTS_EXCLUDE: SDK adapter; not built into tests/.
#include "WorldBuilder.h"

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../brain/Task.h"
#include "../common/Clock.h"
#include "../common/Strings.h"
#include "../nav/NavGrid.h"
#include "../stash/StashDiag.h"
#include "InventoryModel.h"

#include <chrono>
#include <optional>

namespace fb {

namespace {

using PluginSDK::Entity;
using PluginSDK::EntityType;

float GridScale(const PluginSDK::Snapshot& s) {
    return s.WorldToGridConvertor > 0.1f ? s.WorldToGridConvertor : kGridToWorld;
}

bool IsServerModEntity(const std::wstring& path) {
    static constexpr wchar_t kPrefix[] = L"Metadata/Monsters/MonsterMods/";
    constexpr size_t n = sizeof(kPrefix) / sizeof(kPrefix[0]) - 1;
    return path.size() >= n && path.compare(0, n, kPrefix) == 0;
}

// A living hostile monster that can be fought (the Pick My Loot / Auto My Aim test).
bool IsHostileMonster(const Entity& e) {
    if (!e.IsValid || e.EntityType != EntityType::Monster || e.CurrentHP <= 0) return false;
    if (e.EntityState == PluginSDK::EntityState::MonsterFriendly) return false;
    if (e.EntityState == PluginSDK::EntityState::PinnacleBossHidden) return false;
    if (e.EntityState == PluginSDK::EntityState::Useless) return false;
    return !IsServerModEntity(e.Path);
}

float Pct(int cur, int max) { return max > 0 ? 100.f * static_cast<float>(cur) / static_cast<float>(max) : 0.f; }

std::string Narrow(const std::wstring& w) { return WideToUtf8(w); }

}  // namespace

void WorldBuilder::Reset() {
    m_scanFlagsSent = false;   // unknown: the next Build sends what it wants (the host cleared ours on disable)
    m_scanFlags = 0;
    m_stashDiagSig = 0;
    m_occMask = ~0ull;
    m_leader.Reset();
    m_interactables.Clear();
    m_lastInteractMs = 0;
    m_haveArea = false;
    m_items.clear();
    m_lastItemsMs = 0;
    m_route.clear();
    m_lastRouteMs = 0;
    m_scanPending = false;
    m_scanMs = 0;
    m_inv = InventoryState{};
    m_gridPtr = nullptr;
    m_gridW = m_gridH = 0;
    m_lastGridPollMs = 0;
    m_nav.reset();
    m_height.reset();
    m_selfName.clear();
    m_skills.clear();
    m_selfBuffs.clear();
    m_leaderBuffs.clear();
    m_flasks.clear();
    m_skillsMs = m_selfBuffsMs = m_leaderBuffsMs = m_flasksMs = 0;
}

void WorldBuilder::Build(World& w, const WorldRequests& req, const BuildParams& p, uint64_t now) {
    w = World{};
    w.nowMs = now;
    w.tick = ++m_tick;
    if (!m_sdk) return;
    const PluginSDK::Context& sdk = *m_sdk;

    // --- gate + players (one snapshot, players only) -------------------
    const PluginSDK::Snapshot snap = sdk.Game.GetSnapshot(EntityType::Player);
    GameGate& g = w.gate;
    g.attached = snap.IsAttached;
    g.gameState = static_cast<int>(snap.State);
    g.inGame = snap.State == PluginSDK::GameState::InGame;
    g.loading = snap.State == PluginSDK::GameState::AreaLoading || snap.State == PluginSDK::GameState::Loading;
    g.escapeMenu = snap.State == PluginSDK::GameState::Escape;
    HWND game = snap.GameWindow ? snap.GameWindow : sdk.Game.GetGameWindow();
    g.gameWindow = reinterpret_cast<uintptr_t>(game);
    g.foregroundExact = game != nullptr && ::GetForegroundWindow() == game;
    g.menuVisible = sdk.Game.IsMenuVisible();
    g.screenW = snap.ScreenWidth;
    g.screenH = snap.ScreenHeight;
    g.areaCounter = static_cast<uint32_t>(snap.AreaChangeCounter);
    g.areaId = snap.CurrentAreaName;
    g.areaHash = snap.CurrentAreaHash;
    g.areaLevel = snap.CurrentAreaLevel;
    g.isTown = snap.IsTown;
    g.isHideout = snap.IsHideout;
    g.isMap = !g.isTown && !g.isHideout && (StartsWith(g.areaId, "Map") || g.areaLevel >= 65);
    // Host 365+ capabilities (SDK tail functions; each false on an older host).
    g.hostAutomation = sdk.Game.IsHostAutomationActive();
    g.hostHasControlNames = sdk.Ui.HasControlNames() && sdk.Ui.HasFindElements();
    g.hostHasStashTabs = sdk.Game.HasStashTabs();
    g.hostHasStashObjects = sdk.Game.HasStashObjects() && sdk.Overlay.HasEntityScanFlags();
    UpdateScanFlags(g, p.botActive);

    const bool areaChanged = !m_haveArea || g.areaCounter != m_area;
    if (areaChanged) {
        m_area = g.areaCounter;
        m_haveArea = true;
        m_lastInteractMs = 0;
        m_lastItemsMs = 0;
        m_items.clear();
        m_route.clear();
        m_scanPending = false;
        m_inv = InventoryState{};
        m_lastGridPollMs = 0;   // poll the grid at once
    }

    // --- projection -----------------------------------------------------
    w.proj.valid = snap.ScreenWidth > 0 && snap.ScreenHeight > 0;
    for (int i = 0; i < 16; ++i) w.proj.m[i] = snap.WorldToScreenMatrix[i];
    w.proj.screenW = snap.ScreenWidth;
    w.proj.screenH = snap.ScreenHeight;

    if (!g.inGame) {
        w.proj.height = m_height;
        return;
    }
    const float scale = GridScale(snap);

    // --- self -------------------------------------------------------------
    SelfState& self = w.self;
    const Entity& pe = snap.Player;
    self.valid = pe.IsValid || snap.Vitals.IsValid;
    self.id = pe.Id;
    self.address = pe.Address;
    self.actorAddr = pe.Components.Actor;
    self.buffsAddr = pe.Components.Buffs;
    self.lifeAddr = pe.Components.Life;
    self.renderAddr = pe.Components.Render;
    self.playerAddr = pe.Components.Player;
    self.grid = {pe.GridPositionX, pe.GridPositionY};
    self.world = {pe.WorldX, pe.WorldY, pe.WorldZ};
    const PluginSDK::Vitals& v = snap.Vitals;
    self.hpCur = v.CurrentHP;
    self.hpMax = v.MaxHP;
    self.esCur = v.CurrentES;
    self.esMax = v.MaxES;
    self.mpCur = v.CurrentMP;
    self.mpMax = v.MaxMP;
    self.hpPct = static_cast<float>(v.HPPercent);
    self.esPct = static_cast<float>(v.ESPercent);
    self.mpPct = static_cast<float>(v.MPPercent);
    self.dead = (pe.IsValid && pe.MaxHP > 0 && pe.CurrentHP <= 0) || (v.IsValid && v.MaxHP > 0 && v.CurrentHP <= 0);
    if (self.actorAddr) {
        const PluginSDK::ActorAction aa = sdk.Components.ReadActorAction(self.actorAddr);
        if (aa.Valid) self.actionFlags = static_cast<uint32_t>(aa.Flags);
        const PluginSDK::Actor ac = sdk.Components.ReadActor(self.actorAddr);
        if (ac.Valid) self.animationId = ac.AnimationId;
    }
    if (m_selfName.empty() || m_selfNameArea != g.areaCounter) {
        std::string name = Narrow(pe.PlayerName);
        if (name.empty() && self.playerAddr) name = sdk.Components.ReadPlayer(self.playerAddr).Name;
        if (!name.empty()) {
            m_selfName = std::move(name);
            m_selfNameArea = g.areaCounter;
        }
    }
    self.name = m_selfName;

    // --- other players ----------------------------------------------------
    for (const Entity& e : snap.Entities) {
        if (!e.IsValid || e.EntityType != EntityType::Player) continue;
        if (e.EntitySubtype != PluginSDK::EntitySubtype::PlayerOther || e.Id == self.id) continue;
        PlayerInfo pi;
        pi.id = e.Id;
        pi.address = e.Address;
        pi.actorAddr = e.Components.Actor;
        pi.buffsAddr = e.Components.Buffs;
        pi.renderAddr = e.Components.Render;
        pi.lifeAddr = e.Components.Life;
        pi.name = Narrow(e.PlayerName);
        pi.grid = {e.GridPositionX, e.GridPositionY};
        pi.world = {e.WorldX, e.WorldY, e.WorldZ};
        pi.hpCur = e.CurrentHP;
        pi.hpMax = e.MaxHP;
        pi.hpPct = e.MaxHP > 0 ? Pct(e.CurrentHP, e.MaxHP) : 100.f;
        pi.dead = e.MaxHP > 0 && e.CurrentHP <= 0;
        w.others.push_back(std::move(pi));
    }

    // --- walkable grid, terrain height ------------------------------------
    BuildNav(w, now);
    w.proj.height = m_height;

    // --- interactables (before the leader: vanish classification uses them)
    BuildInteractables(w, now);
    if (req.interactablesNow) m_lastInteractMs = 0;

    // --- leader -----------------------------------------------------------
    LeaderObservation obs;
    const int li = FindLeader(w.others, p.leader);
    if (li >= 0) {
        obs.found = true;
        obs.info = w.others[static_cast<size_t>(li)];
        if (obs.info.renderAddr) {
            const PluginSDK::Render r = sdk.Components.ReadRender(obs.info.renderAddr);
            if (r.Valid) {
                obs.info.world = {r.WorldX, r.WorldY, r.WorldZ};
                obs.info.grid = {r.WorldX / scale, r.WorldY / scale};
                w.others[static_cast<size_t>(li)].grid = obs.info.grid;
                w.others[static_cast<size_t>(li)].world = obs.info.world;
            }
        }
        if (obs.info.actorAddr) {
            const PluginSDK::ActorAction aa = sdk.Components.ReadActorAction(obs.info.actorAddr);
            if (aa.Valid && (aa.DestX != 0 || aa.DestY != 0)) {
                obs.hasActionDest = true;
                obs.actionDest = {static_cast<float>(aa.DestX), static_cast<float>(aa.DestY)};
            }
        }
        if (obs.info.id != m_routeLeaderId || now - m_lastRouteMs >= 100) {
            m_routeLeaderId = obs.info.id;
            m_lastRouteMs = now;
            m_route.clear();
            for (const auto& n : sdk.Components.ReadPathfinding(obs.info.address))
                m_route.push_back({static_cast<float>(n.X), static_cast<float>(n.Y)});
            obs.routeUpdated = true;
            obs.route = m_route;
        }
    }
    m_leader.Update(now, g.areaCounter, self.grid, obs, w.interactables, p.leader);
    w.leader = m_leader.State();

    // --- monsters (every tick) ---------------------------------------------
    {
        const PluginSDK::Snapshot ms = sdk.Game.GetSnapshot(EntityType::Monster);
        for (const Entity& e : ms.Entities) {
            if (!IsHostileMonster(e)) continue;
            const Vec2 grid{e.GridPositionX, e.GridPositionY};
            const float d = Dist(grid, self.grid);
            if (d > p.monsterRange) continue;
            Monster m;
            m.id = e.Id;
            m.address = e.Address;
            m.buffsAddr = e.Components.Buffs;
            m.targetableAddr = e.Components.Targetable;
            m.renderAddr = e.Components.Render;
            m.grid = grid;
            m.world = {e.WorldX, e.WorldY, e.WorldZ};
            m.rarity = e.Rarity;
            const int hp = e.CurrentHP + e.CurrentES, maxHp = e.MaxHP + e.MaxES;
            m.hpPct = maxHp > 0 ? Pct(hp, maxHp) : 100.f;
            m.dist = d;
            m.modelHeight = e.ModelBoundsZ;
            m.path = Narrow(e.Path);
            w.monsters.push_back(std::move(m));
        }
    }

    // --- ground items (cadence) ---------------------------------------------
    const uint64_t itemEvery = req.groundItems ? 150 : 500;
    if (m_lastItemsMs == 0 || now - m_lastItemsMs >= itemEvery) {
        m_lastItemsMs = now;
        BuildItems(w);
    }
    w.items = m_items;
    for (auto& it : w.items) it.dist = Dist(it.grid, self.grid);

    // --- inventories ----------------------------------------------------------
    BuildInventory(w, req.inventoryFast, now);

    // --- requested reads --------------------------------------------------------
    BuildRequested(w, req, now);
}

void WorldBuilder::UpdateScanFlags(const GameGate& g, bool botActive) {
    const PluginSDK::Context& sdk = *m_sdk;
    if (!sdk.Overlay.HasEntityScanFlags()) {
        m_scanFlags = 0;
        m_scanFlagsSent = true;
        return;
    }
    // Only while the bot runs and we stand in a town / hideout: the sleeping walk
    // is the most expensive part of the host's entity scan. A loading screen (or
    // any not-in-game state) keeps what was asked; stopping always drops it.
    uint32_t want = m_scanFlags;
    if (!botActive || !g.attached) {
        want = 0;
    } else if (g.inGame && !g.loading) {
        want = (g.isTown || g.isHideout)
                   ? (PluginSDK::EntityScan::IncludeSleeping | PluginSDK::EntityScan::StashObjects)
                   : 0u;
    }
    if (m_scanFlagsSent && want == m_scanFlags) return;   // transitions only
    sdk.Overlay.SetEntityScanFlags(want);
    m_scanFlags = want;
    m_scanFlagsSent = true;
}

void WorldBuilder::MergeStashObjects(std::vector<Interactable>& visible) {
    const PluginSDK::Context& sdk = *m_sdk;
    int status = kStashObjUnsupported;
    std::optional<std::vector<PluginSDK::StashObject>> objs;
    if (sdk.Game.HasStashObjects() && sdk.Overlay.HasEntityScanFlags()) {
        if ((m_scanFlags & PluginSDK::EntityScan::StashObjects) == 0) {
            status = kStashObjNotRequested;
        } else {
            objs = sdk.Game.EnumerateStashObjects();
            status = objs ? static_cast<int>(objs->size()) : kStashObjNotReady;
        }
    }
    // Diagnostics signature: status, flags and every object's id and cell.
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&sig](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    mix(static_cast<uint64_t>(static_cast<int64_t>(status)));
    mix(m_scanFlags);
    if (objs) {
        for (const PluginSDK::StashObject& o : *objs) {
            const bool guild = o.Domain == PluginSDK::StashDomain::Guild;
            Vec2 grid{o.GridX, o.GridY};
            if (grid.x == 0.f && grid.y == 0.f && (o.WorldX != 0.f || o.WorldY != 0.f))
                grid = {o.WorldX / kGridToWorld, o.WorldY / kGridToWorld};
            mix(o.EntityId);
            mix(static_cast<uint64_t>(static_cast<int64_t>(grid.x)) << 32 ^ static_cast<uint64_t>(static_cast<int64_t>(grid.y)));
            const InteractKind kind = guild ? InteractKind::GuildStash : InteractKind::Stash;
            bool merged = false;
            for (auto& v : visible) {
                if (v.id != o.EntityId) continue;
                v.kind = kind;   // the host's list decides the domain
                if (v.path.empty()) v.path = o.Path;
                merged = true;
                break;
            }
            if (merged) continue;
            Interactable it;
            it.id = o.EntityId;
            it.address = o.EntityAddress;
            it.kind = kind;
            it.grid = grid;
            it.world = {o.WorldX, o.WorldY, o.WorldZ};
            it.path = o.Path;
            it.targetable = true;
            it.open = true;
            visible.push_back(std::move(it));
        }
    }
    if (sig == m_stashDiagSig) return;
    m_stashDiagSig = sig;
    const uint32_t flags = m_scanFlags;
    GlobalStashDiag().Update([&](StashDiagData& d) {
        d.objectsStatus = status;
        d.scanFlags = flags;
        d.objects.clear();
        if (!objs) return;
        for (const PluginSDK::StashObject& o : *objs) {
            StashObjectDiag od;
            od.id = o.EntityId;
            od.guild = o.Domain == PluginSDK::StashDomain::Guild;
            od.rank = o.Rank;
            od.grid = {o.GridX, o.GridY};
            od.path = o.Path;
            d.objects.push_back(std::move(od));
        }
    });
}

void WorldBuilder::BuildNav(World& w, uint64_t now) {
    if (m_lastGridPollMs != 0 && now - m_lastGridPollMs < 250) {
        w.nav = m_nav;
        w.navGeneration = m_navGen;
        return;
    }
    m_lastGridPollMs = now;
    PluginSDK::WalkableGridHandle h = m_sdk->Terrain.GetWalkableGrid();
    if (h.Valid() && (h.Data() != m_gridPtr || h.Width() != m_gridW || h.Height() != m_gridH)) {
        const auto t0 = std::chrono::steady_clock::now();
        auto grid = NavGrid::FromPacked(h.Data(), h.SizeBytes(), h.Width(), h.Height(), m_navGen + 1);
        if (grid) {
            m_nav = std::move(grid);
            ++m_navGen;
            m_gridPtr = h.Data();
            m_gridW = h.Width();
            m_gridH = h.Height();
            m_navBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            // The height grid belongs to the same area: pin the host buffer.
            auto hh = std::make_shared<PluginSDK::HeightGridHandle>(m_sdk->Terrain.GetHeightGrid());
            if (hh->Valid()) {
                auto hf = std::make_shared<HeightField>();
                hf->data = hh->Data();
                hf->w = hh->Width();
                hf->h = hh->Height();
                hf->keep = hh;
                m_height = std::move(hf);
            } else {
                m_height.reset();
            }
        }
    } else if (!h.Valid()) {
        m_nav.reset();
        m_gridPtr = nullptr;
    }
    w.nav = m_nav;
    w.navGeneration = m_navGen;
}

void WorldBuilder::BuildInteractables(World& w, uint64_t now) {
    const bool due = m_lastInteractMs == 0 || now - m_lastInteractMs >= 250;
    if (due) {
        m_lastInteractMs = now;
        std::vector<Interactable> visible;
        const PluginSDK::Context& sdk = *m_sdk;
        for (EntityType type : {EntityType::AreaTransition, EntityType::Renderable, EntityType::NPC}) {
            const PluginSDK::Snapshot s = sdk.Game.GetSnapshot(type);
            for (const Entity& e : s.Entities) {
                if (!e.IsValid || e.Path.empty()) continue;
                EntityTraits tr;
                tr.isAreaTransitionType = e.EntityType == EntityType::AreaTransition;
                tr.hasTriggerableBlockage = e.Components.TriggerableBlockage != 0;
                const std::string path = Narrow(e.Path);
                bool closed = false;
                if (tr.hasTriggerableBlockage) {
                    const auto tb = sdk.Components.ReadTriggerableBlockage(e.Components.TriggerableBlockage);
                    closed = tb.Valid && tb.IsClosed;
                    tr.blockageClosed = closed;
                }
                const InteractKind kind = ClassifyInteractable(path, tr);
                if (kind == InteractKind::Other) continue;
                Interactable it;
                it.id = e.Id;
                it.address = e.Address;
                it.targetableAddr = e.Components.Targetable;
                it.stateMachineAddr = e.Components.StateMachine;
                it.transitionableAddr = e.Components.Transitionable;
                it.kind = kind;
                it.grid = {e.GridPositionX, e.GridPositionY};
                it.world = {e.WorldX, e.WorldY, e.WorldZ};
                it.modelHeight = e.ModelBoundsZ;
                it.path = path;
                if (it.targetableAddr) {
                    const auto t = sdk.Components.ReadTargetable(it.targetableAddr);
                    if (t.Valid) it.targetable = t.IsTargetable && !t.HiddenFromPlayer;
                }
                if (it.transitionableAddr) {
                    const auto tr2 = sdk.Components.ReadTransitionable(it.transitionableAddr);
                    if (tr2.Valid) it.transitionState = tr2.CurrentState;
                }
                if (kind == InteractKind::Portal && it.stateMachineAddr) {
                    // MultiplexPortal shared state "portal" (index 1, define order): the portal kind.
                    uint64_t value = 0;
                    if (sdk.Sekhema.GetStateMachineValue(it.stateMachineAddr, 1, value) && value < 32)
                        it.portalType = static_cast<int>(value);
                }
                it.open = kind == InteractKind::Door ? !closed : it.targetable;
                visible.push_back(std::move(it));
            }
        }
        // Host 365+: stash objects the entity list drops (invalid flag), in town / hideout.
        MergeStashObjects(visible);
        m_interactables.Update(w.gate.areaCounter, visible, now);
    }
    w.interactables = m_interactables.All();
}

void WorldBuilder::BuildItems(World& w) {
    m_items.clear();
    const PluginSDK::Snapshot s = m_sdk->Game.GetSnapshot(EntityType::Item);
    for (const Entity& e : s.Entities) {
        if (!e.IsValid || e.EntityType != EntityType::Item || !e.Components.HasWorldItem() || e.Address == 0) continue;
        GroundItem gi;
        gi.id = e.Id;
        gi.address = e.Address;
        gi.targetableAddr = e.Components.Targetable;
        gi.grid = {e.GridPositionX, e.GridPositionY};
        gi.world = {e.WorldX, e.WorldY, e.WorldZ};
        gi.dist = Dist(gi.grid, w.self.grid);
        if (gi.dist > 150.f) continue;
        m_items.push_back(std::move(gi));
    }
}

void WorldBuilder::BuildInventory(World& w, bool fast, uint64_t now) {
    const uint64_t every = fast ? 200 : 400;
    const PluginSDK::Context& sdk = *m_sdk;
    if (!m_scanPending && (m_scanMs == 0 || now - m_scanMs >= every)) {
        sdk.Inventory.Scan(-1);
        m_scanMs = now;
        m_scanPending = true;
    } else if (m_scanPending && now - m_scanMs >= 80) {
        m_scanPending = false;
        std::vector<InvListingEntry> listing;
        bool haveBackpack = false;
        std::vector<BagItem> bag;
        bool cursorItem = false;
        for (const auto& inv : sdk.Inventory.GetAll()) {
            InvListingEntry e;
            e.id = inv.InventoryId;
            const char* name = sdk.Inventory.GetName(inv.InventoryId);
            e.name = name ? name : "";
            e.cols = inv.TotalBoxesX;
            e.rows = inv.TotalBoxesY;
            e.grid.valid = inv.Grid.Valid;
            e.grid.x = inv.Grid.GridScreenX;
            e.grid.y = inv.Grid.GridScreenY;
            e.grid.cell = inv.Grid.CellSize;
            e.grid.cols = inv.TotalBoxesX;
            e.grid.rows = inv.TotalBoxesY;
            e.grid.inventoryId = inv.InventoryId;
            for (const auto& it : inv.Items) {
                BagItem b;
                b.address = it.Address;
                b.x = it.SlotX;
                b.y = it.SlotY;
                b.w = it.Width > 0 ? it.Width : 1;
                b.h = it.Height > 0 ? it.Height : 1;
                b.stack = it.StackCount;
                b.isCurrency = it.IsCurrency;
                b.path = it.Path;
                b.baseName = it.BaseTypeName;
                b.uniqueName = it.UniqueName;
                e.items.push_back(std::move(b));
            }
            if (inv.InventoryId == kBackpackInventoryId) {
                haveBackpack = true;
                bag = e.items;
            } else if (inv.InventoryId == kCursorInventoryId) {
                cursorItem = !inv.Items.empty();
            }
            listing.push_back(std::move(e));
        }
        const PanelState ps = DetectPanels(listing);
        InventoryState st;
        st.readMs = now;
        st.stamp = m_scanMs;   // when this listing's scan was requested: "scanned after X" tests compare it
        st.valid = haveBackpack;
        st.items = std::move(bag);
        st.usedCells = ComputeOccupancy(st.items, st.occupied);
        st.freeCells = kBackpackCells - st.usedCells;
        st.cursorKnown = true;
        st.cursorHasItem = cursorItem;
        st.anyPanelOpen = ps.anyPanelOpen;
        st.backpackOpen = ps.backpackOpen;
        st.stashOpen = ps.stashOpen;
        st.guildOpen = ps.guildOpen;
        st.openStashInventoryId = ps.openStashId;
        st.stashItemCount = ps.stashItemCount;
        st.backpack = ps.backpack;
        st.stash = ps.stash;
        if (haveBackpack) {   // the Stash tab's protected-cells editor shows what is in the backpack
            uint64_t mask = 0;
            for (int i = 0; i < kBackpackCells; ++i)
                if (st.occupied[static_cast<size_t>(i)]) mask |= 1ull << i;
            if (mask != m_occMask) {
                m_occMask = mask;
                const auto occ = st.occupied;
                GlobalStashDiag().Update([&](StashDiagData& d) {
                    d.backpackValid = true;
                    d.occupied = occ;
                });
            }
        }
        m_inv = std::move(st);
    }
    w.inv = m_inv;
    w.inv.listingFresh = m_inv.readMs != 0 && now - m_inv.readMs < 1500;
}

void WorldBuilder::BuildRequested(World& w, const WorldRequests& req, uint64_t now) {
    const PluginSDK::Context& sdk = *m_sdk;
    if (req.skills && w.self.actorAddr && (m_skillsMs == 0 || now - m_skillsMs >= 250)) {
        m_skillsMs = now;
        m_skills.clear();
        for (const auto& s : sdk.Components.EnumerateActiveSkills(w.self.actorAddr)) {
            SkillInfo si;
            si.name = s.Name;
            si.barSlot = s.SkillSlotIndex;
            si.onBar = s.IsOnSkillBar;
            si.canBeUsed = s.CanBeUsed;
            si.totalCooldownMs = s.TotalCooldownMs;
            si.maxUses = s.MaxUses;
            si.activeCooldowns = s.TotalActiveCooldowns;
            si.skillDetailsAddr = s.SkillDetailsAddr;
            m_skills.push_back(std::move(si));
        }
    }
    auto readBuffs = [&](uintptr_t buffsAddr, std::vector<BuffInfo>& out) {
        out.clear();
        if (!buffsAddr) return;
        std::vector<PluginSDK::Buff> raw = sdk.Components.EnumerateAggregatedBuffs(buffsAddr);
        if (raw.empty()) raw = sdk.Components.EnumerateBuffs(buffsAddr);
        for (const auto& b : raw) {
            BuffInfo bi;
            bi.name = b.Name;
            bi.timeLeft = b.TimeLeft;
            bi.totalTime = b.TotalTime;
            bi.charges = b.Charges;
            bi.flaskSlot = b.FlaskSlot;
            bi.sourceId = b.SourceEntityId;
            out.push_back(std::move(bi));
        }
    };
    if (req.selfBuffs && (m_selfBuffsMs == 0 || now - m_selfBuffsMs >= 250)) {
        m_selfBuffsMs = now;
        readBuffs(w.self.buffsAddr, m_selfBuffs);
    }
    if (req.leaderBuffs && (m_leaderBuffsMs == 0 || now - m_leaderBuffsMs >= 500)) {
        m_leaderBuffsMs = now;
        uintptr_t addr = 0;
        if (w.leader.visible) addr = w.leader.info.buffsAddr;
        readBuffs(addr, m_leaderBuffs);
    }
    if (req.flasks && (m_flasksMs == 0 || now - m_flasksMs >= 200)) {
        m_flasksMs = now;
        m_flasks.clear();
        for (const auto& f : sdk.Flasks.AllFlasks()) {
            FlaskInfo fi;
            fi.valid = f.Valid;
            fi.slot = f.SlotIndex;
            fi.charges = f.ChargesCurrent;
            fi.perUse = f.PerUseEffective > 0 ? f.PerUseEffective : f.PerUseBase;
            fi.usable = f.Usable;
            fi.active = f.Active;
            fi.isLife = f.IsLife;
            fi.isMana = f.IsMana;
            fi.name = f.Name;
            fi.baseType = f.BaseType;
            m_flasks.push_back(std::move(fi));
        }
        for (const auto& c : sdk.Flasks.AllCharms()) {
            FlaskInfo fi;
            fi.valid = c.Valid;
            fi.isCharm = true;
            fi.slot = c.SlotIndex;
            fi.charges = c.ChargesCurrent;
            fi.perUse = c.PerUseBase;
            fi.active = c.Active;
            fi.usable = c.ChargesCurrent >= c.PerUseBase;
            fi.name = c.Name;
            fi.baseType = c.BaseType;
            m_flasks.push_back(std::move(fi));
        }
    }
    w.skills = m_skills;
    w.skillsMs = m_skillsMs;
    w.selfBuffs = m_selfBuffs;
    w.selfBuffsMs = m_selfBuffsMs;
    w.leaderBuffs = m_leaderBuffs;
    w.leaderBuffsMs = m_leaderBuffsMs;
    w.flasks = m_flasks;
    w.flasksMs = m_flasksMs;
}

}  // namespace fb
