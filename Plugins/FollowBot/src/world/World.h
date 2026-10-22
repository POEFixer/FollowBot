// World.h - the bot's immutable per-tick view of the game (design §5.1).
//
// Plain structs, no SDK types: WorldBuilder fills them from the PluginSDK, every
// decision module reads only these, so the decision logic compiles (and is
// tested) without the host. Coordinates are grid cells (float) unless a field
// is named world*; screen points are game-window client pixels.
//
// This header is also the shared vocabulary between modules (interactable and
// vanish kinds, item categories): a module never includes another module's
// private headers.
#pragma once

#include "../common/Geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fb {

class NavGrid;  // nav/NavGrid.h

// ---------------------------------------------------------------------------
// Shared enums
// ---------------------------------------------------------------------------

enum class InteractKind : uint8_t {
    AreaTransition = 0,
    BossArena,
    Portal,
    Checkpoint,
    Waypoint,
    Stash,
    GuildStash,
    MapDevice,
    Door,
    Other,
    Count
};

inline const char* InteractKindName(InteractKind k) {
    switch (k) {
        case InteractKind::AreaTransition: return "Transition";
        case InteractKind::BossArena:      return "Boss arena";
        case InteractKind::Portal:         return "Portal";
        case InteractKind::Checkpoint:     return "Checkpoint";
        case InteractKind::Waypoint:       return "Waypoint";
        case InteractKind::Stash:          return "Stash";
        case InteractKind::GuildStash:     return "Guild stash";
        case InteractKind::MapDevice:      return "Map device";
        case InteractKind::Door:           return "Door";
        case InteractKind::Other:          return "Other";
        case InteractKind::Count:          break;
    }
    return "?";
}

// How the leader left our sight (LeaderTracker, design §5.1). BossArena is kept
// apart from Transition so the UI can say which one he took.
enum class VanishKind : uint8_t {
    None = 0,
    Transition,
    BossArena,
    Portal,
    Checkpoint,
    Waypoint,
    OutOfRange,
    Died,
    Unknown,
    Count
};

inline const char* VanishKindName(VanishKind k) {
    switch (k) {
        case VanishKind::None:       return "None";
        case VanishKind::Transition: return "Area transition";
        case VanishKind::BossArena:  return "Boss arena";
        case VanishKind::Portal:     return "Portal";
        case VanishKind::Checkpoint: return "Checkpoint";
        case VanishKind::Waypoint:   return "Waypoint";
        case VanishKind::OutOfRange: return "Out of range";
        case VanishKind::Died:       return "Died";
        case VanishKind::Unknown:    return "Unknown";
        case VanishKind::Count:      break;
    }
    return "?";
}

// True for the kinds the bot can follow through by clicking an object.
inline bool IsFollowThroughVanish(VanishKind k) {
    return k == VanishKind::Transition || k == VanishKind::BossArena || k == VanishKind::Portal;
}

// Item categories shared by loot (pickup rules) and stash (tab routing, keep
// rules). Decided by metadata path (loot/LootCategories), never by display text.
// Append only: the order is stored in settings files.
enum class ItemCategory : uint8_t {
    Currency = 0,
    Quest,
    Gems,
    Waystones,
    Tablets,
    Runes,        // runes, soul cores, idols
    Omens,        // omens, essences, catalysts, distilled emotions (Currency subfolders)
    Fragments,
    Jewels,
    Flasks,       // flasks and charms
    Uniques,      // any class, rarity 3
    Weapons,      // equipment by class, min rarity per rule
    Armour,
    Accessories,
    Other,
    Gold,         // gold piles (no backpack space); appended by the loot module
    Count
};

inline constexpr int kItemCategoryCount = static_cast<int>(ItemCategory::Count);

inline const char* ItemCategoryName(ItemCategory c) {
    switch (c) {
        case ItemCategory::Currency:    return "Currency";
        case ItemCategory::Quest:       return "Quest items";
        case ItemCategory::Gems:        return "Gems";
        case ItemCategory::Waystones:   return "Waystones";
        case ItemCategory::Tablets:     return "Tablets";
        case ItemCategory::Runes:       return "Runes, soul cores, idols";
        case ItemCategory::Omens:       return "Omens, essences, catalysts";
        case ItemCategory::Fragments:   return "Fragments";
        case ItemCategory::Jewels:      return "Jewels";
        case ItemCategory::Flasks:      return "Flasks and charms";
        case ItemCategory::Uniques:     return "Unique items";
        case ItemCategory::Weapons:     return "Weapons";
        case ItemCategory::Armour:      return "Armour";
        case ItemCategory::Accessories: return "Accessories";
        case ItemCategory::Other:       return "Other";
        case ItemCategory::Gold:        return "Gold";
        case ItemCategory::Count:       break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Per-tick state
// ---------------------------------------------------------------------------

struct GameGate {
    bool     attached = false;
    bool     inGame = false;          // game state InGame
    bool     loading = false;         // area loading / loading screen
    bool     escapeMenu = false;      // the game's Escape menu (paused)
    bool     foregroundExact = false; // GetForegroundWindow() == the game window (not the overlay)
    bool     menuVisible = false;     // POEFixer's own menu
    int      gameState = 0;           // PluginSDK::GameState value
    int      screenW = 0, screenH = 0;
    uintptr_t gameWindow = 0;         // HWND as an integer (World stays Win32-free)
    uint32_t areaCounter = 0;         // bumps on every area load
    std::string areaId;               // raw WorldArea id
    std::string areaHash;
    int      areaLevel = 0;
    bool     isTown = false, isHideout = false, isMap = false;
    bool     hostAutomation = false;  // host automation drives input (crafting, memory trade, auto-stash, recovery; host 365+)
    bool     hostHasStashTabs = false, hostHasControlNames = false, hostHasStashObjects = false;
};

struct SelfState {
    bool      valid = false;
    uint32_t  id = 0;
    uintptr_t address = 0, actorAddr = 0, buffsAddr = 0, lifeAddr = 0, renderAddr = 0, playerAddr = 0;
    Vec2      grid;
    Vec3      world;
    float     hpPct = 0.f, esPct = 0.f, mpPct = 0.f;  // 0..100
    int       hpCur = 0, hpMax = 0, esCur = 0, esMax = 0, mpCur = 0, mpMax = 0;
    bool      dead = false;
    uint32_t  actionFlags = 0;   // Actor action flags (0x400 using ability, 0x1080 running)
    int       animationId = 0;
    std::string name;            // character name, UTF-8
};

struct PlayerInfo {
    uint32_t  id = 0;
    uintptr_t address = 0, actorAddr = 0, buffsAddr = 0, renderAddr = 0, lifeAddr = 0;
    std::string name;            // UTF-8
    Vec2      grid;
    Vec3      world;
    float     hpPct = 100.f;
    int       hpCur = 0, hpMax = 0;
    bool      dead = false;
};

struct VanishInfo {
    bool       active = false;
    uint64_t   atMs = 0;
    VanishKind kind = VanishKind::None;
    uint32_t   interactableId = 0;   // the object he most likely used (0 = none)
    InteractKind interactKind = InteractKind::Other;
    Vec2       interactGrid;         // where that object stood
    Vec2       lastGrid;             // his last known position
};

struct LeaderState {
    bool       configured = false;   // a name is set (or auto-pick is on)
    bool       visible = false;      // in this tick's entity list
    PlayerInfo info;                 // valid while visible (live position)
    uint64_t   lastSeenMs = 0;
    Vec2       lastGrid;
    Vec3       lastWorld;
    uint32_t   lastSeenArea = 0;     // our area counter when he was last seen
    float      lastDistance = 0.f;   // cells from us when last seen
    float      lastHpPct = 100.f;
    bool       lastDead = false;
    Vec2       velocity;             // cells/s, EMA
    float      speed = 0.f;          // |velocity|
    Vec2       heading{1.f, 0.f};    // last non-zero movement direction
    std::vector<Vec2> route;         // his server route (<= 64 nodes)
    uint64_t   routeMs = 0;
    Vec2       actionDest;
    bool       hasActionDest = false;
    VanishInfo vanish;
};

struct Monster {
    uint32_t  id = 0;
    uintptr_t address = 0, buffsAddr = 0, targetableAddr = 0, renderAddr = 0;
    Vec2      grid;
    Vec3      world;
    int       rarity = 0;            // 0 normal .. 3 unique
    float     hpPct = 100.f;
    float     dist = 0.f;            // cells from us
    float     modelHeight = 0.f;
    std::string path;                // metadata path, UTF-8
    bool      targetable = true;     // unknown = true; Targeting reads Targetable for candidates
};

struct GroundItem {
    uint32_t  id = 0;
    uintptr_t address = 0;           // the WorldItem entity (resolved by LootScanner)
    uintptr_t targetableAddr = 0;
    Vec2      grid;
    Vec3      world;
    float     dist = 0.f;
};

struct Interactable {
    uint32_t     id = 0;
    uintptr_t    address = 0, targetableAddr = 0, stateMachineAddr = 0, transitionableAddr = 0;
    InteractKind kind = InteractKind::Other;
    int          portalType = -1;     // MultiplexPortal "portal" state (1 Player, 2 Town, 3 Map Device,
                                      // 4 Map Return, ...), -1 unknown
    int          transitionState = -1;
    Vec2         grid;
    Vec3         world;
    float        modelHeight = 0.f;
    bool         targetable = true;
    bool         open = true;         // usable: targetable (and open for doors means "not closed")
    bool         visibleNow = false;  // in this tick's entity list (false = remembered for this area)
    std::string  path;
    uint64_t     firstSeenMs = 0;
    uint64_t     lastSeenMs = 0;
};

struct BagItem {
    uintptr_t   address = 0;
    int         x = 0, y = 0, w = 1, h = 1;
    int         stack = 0;
    bool        isCurrency = false;
    int         rarity = -1;          // -1 unknown (listings carry 0; resolved on demand)
    std::string path, baseName, uniqueName;
};

struct GridInfo {
    bool  valid = false;              // the grid is on screen
    float x = 0.f, y = 0.f, cell = 0.f;  // client pixels of cell (0,0) top-left, cell size
    int   cols = 0, rows = 0;
    int   inventoryId = 0;

    Vec2 CellCenter(int cx, int cy) const { return {x + (cx + 0.5f) * cell, y + (cy + 0.5f) * cell}; }
    Vec2 ItemCenter(int cx, int cy, int w, int h) const {
        return {x + (cx + w * 0.5f) * cell, y + (cy + h * 0.5f) * cell};
    }
};

inline constexpr int kBackpackCols = 12;
inline constexpr int kBackpackRows = 5;
inline constexpr int kBackpackCells = kBackpackCols * kBackpackRows;

struct InventoryState {
    bool     valid = false;           // the backpack was read in this area
    uint64_t stamp = 0;               // host scan stamp of the backpack read
    uint64_t readMs = 0;              // when the listing was read
    bool     listingFresh = false;    // the panel listing is recent enough to trust
    int      usedCells = 0, freeCells = kBackpackCells;
    std::array<uint8_t, kBackpackCells> occupied{};  // y*12+x, 1 = taken
    std::vector<BagItem> items;
    bool     cursorKnown = false;
    bool     cursorHasItem = false;
    bool     anyPanelOpen = false;    // any item grid (>= 40 cells) on screen
    bool     backpackOpen = false;
    bool     stashOpen = false;       // a non-player storage grid on screen
    bool     guildOpen = false;       // the guild stash grid (id 10002) on screen
    int      openStashInventoryId = 0;
    int      stashItemCount = 0;
    GridInfo backpack;
    GridInfo stash;
    bool     fullFlag = false;        // a wanted item found no room (loot feeds the stash trigger)
};

// Terrain height per grid cell (world Z), pinned by an opaque keep-alive.
struct HeightField {
    const float*          data = nullptr;
    int                   w = 0, h = 0;
    std::shared_ptr<void> keep;

    float At(float gx, float gy, float fallback) const {
        if (!data || w <= 0 || h <= 0) return fallback;
        const int xi = static_cast<int>(gx + 0.5f), yi = static_cast<int>(gy + 0.5f);
        if (xi < 0 || yi < 0 || xi >= w || yi >= h) return fallback;
        return data[static_cast<size_t>(yi) * static_cast<size_t>(w) + static_cast<size_t>(xi)];
    }
};

struct ProjectionData {
    bool  valid = false;
    float m[16] = {};                 // row-major world -> clip (Snapshot.WorldToScreenMatrix)
    int   screenW = 0, screenH = 0;
    std::shared_ptr<const HeightField> height;
};

struct SkillInfo {
    std::string name;                 // GrantedEffect id
    int         barSlot = -1;
    bool        onBar = false;
    bool        canBeUsed = false;
    int         totalCooldownMs = 0;
    int         maxUses = 0;
    int         activeCooldowns = 0;
    uintptr_t   skillDetailsAddr = 0;
};

struct BuffInfo {
    std::string name;                 // BuffDefinitions name
    float       timeLeft = 0.f, totalTime = 0.f;
    int         charges = 0;          // aggregated stack count
    int         flaskSlot = -1;
    uint32_t    sourceId = 0;
};

struct FlaskInfo {
    bool        valid = false;
    bool        isCharm = false;
    int         slot = -1;
    int         charges = 0, perUse = 0;
    bool        usable = false, active = false, isLife = false, isMana = false;
    std::string name, baseType;
};

struct World {
    uint64_t       nowMs = 0;
    uint64_t       tick = 0;
    GameGate       gate;
    SelfState      self;
    LeaderState    leader;
    std::vector<PlayerInfo>   others;         // other players in the area (leader included)
    std::vector<Monster>      monsters;       // hostile, alive
    std::vector<GroundItem>   items;
    std::vector<Interactable> interactables;  // visible now + remembered in this area
    InventoryState inv;
    ProjectionData proj;
    std::shared_ptr<const NavGrid> nav;
    uint64_t       navGeneration = 0;

    // Read on request (WorldRequests); stamps say how fresh they are.
    std::vector<SkillInfo> skills;      uint64_t skillsMs = 0;
    std::vector<BuffInfo>  selfBuffs;   uint64_t selfBuffsMs = 0;
    std::vector<BuffInfo>  leaderBuffs; uint64_t leaderBuffsMs = 0;
    std::vector<FlaskInfo> flasks;      uint64_t flasksMs = 0;

    // Nearest interactable of a kind (any when kind == Count), or nullptr.
    const Interactable* NearestInteractable(InteractKind kind, const Vec2& from, float maxDist = 1e9f) const {
        const Interactable* best = nullptr;
        float bestD = maxDist;
        for (const auto& it : interactables) {
            if (kind != InteractKind::Count && it.kind != kind) continue;
            const float d = Dist(it.grid, from);
            if (d <= bestD) { bestD = d; best = &it; }
        }
        return best;
    }

    const Interactable* FindInteractable(uint32_t id) const {
        for (const auto& it : interactables)
            if (it.id == id) return &it;
        return nullptr;
    }

    // Hostile monsters within `radius` cells of `from`.
    int CountMonstersNear(const Vec2& from, float radius) const {
        int n = 0;
        for (const auto& m : monsters)
            if (Dist(m.grid, from) <= radius) ++n;
        return n;
    }
};

}  // namespace fb
