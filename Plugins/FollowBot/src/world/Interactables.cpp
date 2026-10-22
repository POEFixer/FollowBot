// Interactables.cpp - see Interactables.h.
#include "Interactables.h"

#include "../common/Strings.h"

namespace fb {

namespace {

enum class Match : uint8_t { Prefix, Exact };

struct Rule {
    const char*  pattern;
    Match        match;
    InteractKind kind;
};

// THE classification table (game files: metadata/miscellaneousobjects/*.ot,
// 2026-10-20 unpack). First match wins; order matters where prefixes overlap.
// AreaTransition* objects whose name contains "_ArenaPortal" are boss arena
// entrances (handled in ClassifyInteractable).
constexpr Rule kRules[] = {
    {"Metadata/MiscellaneousObjects/AreaTransition", Match::Prefix, InteractKind::AreaTransition},
    {"Metadata/MiscellaneousObjects/MultiplexPortal", Match::Prefix, InteractKind::Portal},
    {"Metadata/MiscellaneousObjects/PlayerPortal", Match::Prefix, InteractKind::Portal},
    {"Metadata/MiscellaneousObjects/MapPortal", Match::Prefix, InteractKind::Portal},
    {"Metadata/MiscellaneousObjects/ReturnToLastTownPortal", Match::Prefix, InteractKind::Portal},
    {"Metadata/MiscellaneousObjects/Hideout/TeleportPortal", Match::Prefix, InteractKind::Portal},
    {"Metadata/MiscellaneousObjects/PortalTransition", Match::Prefix, InteractKind::Portal},
    {"Metadata/MiscellaneousObjects/Checkpoint", Match::Prefix, InteractKind::Checkpoint},
    {"Metadata/MiscellaneousObjects/Waypoints/", Match::Prefix, InteractKind::Waypoint},
    {"Metadata/MiscellaneousObjects/Waypoint", Match::Exact, InteractKind::Waypoint},
    {"Metadata/MiscellaneousObjects/MapDevice", Match::Prefix, InteractKind::MapDevice},
    {"Metadata/MapDevices/", Match::Prefix, InteractKind::MapDevice},
};

constexpr std::string_view kArenaMarker = "_ArenaPortal";

}  // namespace

int StashPathRank(std::string_view path, bool guild) {
    constexpr std::string_view kObjects = "Metadata/MiscellaneousObjects/";
    const size_t at = path.find(kObjects);
    if (at == std::string_view::npos) return 0;
    const std::string_view name = path.substr(at + kObjects.size());
    if (!StartsWith(name, "Stash") && !StartsWith(name, "GuildStash")) return 0;
    // "…/Stash" is also the prefix of every guild skin: the domain is read off
    // "GuildStash" in the object's own name.
    if ((name.find("GuildStash") != std::string_view::npos) != guild) return 0;
    const std::string_view base = guild ? "Metadata/MiscellaneousObjects/GuildStash" : "Metadata/MiscellaneousObjects/Stash";
    const bool exact = path == base || (StartsWith(path, base) && path.size() > base.size() && path[base.size()] == '@');
    return exact ? 2 : 1;
}

InteractKind ClassifyInteractable(std::string_view rawPath, const EntityTraits& traits) {
    const std::string_view path = StripAtSuffix(rawPath);
    for (const Rule& r : kRules) {
        const bool hit = r.match == Match::Exact ? path == r.pattern : StartsWith(path, r.pattern);
        if (!hit) continue;
        if (r.kind == InteractKind::AreaTransition && path.find(kArenaMarker) != std::string_view::npos)
            return InteractKind::BossArena;
        return r.kind;
    }
    if (StashPathRank(rawPath, false) > 0) return InteractKind::Stash;
    if (StashPathRank(rawPath, true) > 0) return InteractKind::GuildStash;
    if (traits.hasTriggerableBlockage) return InteractKind::Door;
    if (traits.isAreaTransitionType) {
        return path.find(kArenaMarker) != std::string_view::npos ? InteractKind::BossArena
                                                                   : InteractKind::AreaTransition;
    }
    return InteractKind::Other;
}

void InteractableMemory::Clear() {
    m_list.clear();
    m_index.clear();
    m_haveArea = false;
}

void InteractableMemory::Update(uint32_t areaCounter, const std::vector<Interactable>& visible, uint64_t nowMs) {
    if (!m_haveArea || areaCounter != m_area) {
        m_list.clear();
        m_index.clear();
        m_area = areaCounter;
        m_haveArea = true;
    }
    for (auto& it : m_list) it.visibleNow = false;
    for (const Interactable& v : visible) {
        const auto found = m_index.find(v.id);
        if (found != m_index.end()) {
            Interactable& slot = m_list[found->second];
            const uint64_t first = slot.firstSeenMs;
            slot = v;
            slot.firstSeenMs = first;
            slot.lastSeenMs = nowMs;
            slot.visibleNow = true;
        } else {
            Interactable copy = v;
            copy.firstSeenMs = nowMs;
            copy.lastSeenMs = nowMs;
            copy.visibleNow = true;
            m_index[v.id] = m_list.size();
            m_list.push_back(std::move(copy));
        }
    }
    // Bounded: drop the oldest remembered (not visible) entries past 512.
    if (m_list.size() > 512) {
        std::vector<Interactable> kept;
        kept.reserve(m_list.size());
        size_t drop = m_list.size() - 512;
        for (auto& it : m_list) {
            if (drop > 0 && !it.visibleNow) { --drop; continue; }
            kept.push_back(std::move(it));
        }
        m_list = std::move(kept);
        m_index.clear();
        for (size_t i = 0; i < m_list.size(); ++i) m_index[m_list[i].id] = i;
    }
}

}  // namespace fb
