// Interactables.h - what a world object is (transition, portal, stash, ...) by
// its metadata path, and the per-area memory of the ones seen (design §5.1).
// Pure: no SDK.
//
// Classification is ONE data table (Interactables.cpp, kRules): a patch that
// renames or adds an object kind needs one edit there. Matching is a
// case-sensitive prefix (or substring) test on the "@"-stripped path.
#pragma once

#include "World.h"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace fb {

// What the WorldBuilder knows about an entity before classification.
struct EntityTraits {
    bool isAreaTransitionType = false;  // EntityType::AreaTransition (has the AreaTransition component)
    bool hasTriggerableBlockage = false;
    bool blockageClosed = false;        // TriggerableBlockage.IsClosed (valid only with the component)
};

// InteractKind for a path; Other when nothing matches (not an interactable the
// bot uses). Doors are decided by the blockage component, not the path.
InteractKind ClassifyInteractable(std::string_view path, const EntityTraits& traits);

// Own / guild stash by path (copy of core/StashPathMatch.h rules):
// 2 = the unskinned object of that domain, 1 = another stash of the domain
// (skin, affinity, pet stash), 0 = not one.
int StashPathRank(std::string_view path, bool guild);

// The kinds a leader can leave the area through by clicking.
inline bool IsTravelKind(InteractKind k) {
    return k == InteractKind::AreaTransition || k == InteractKind::BossArena || k == InteractKind::Portal;
}
inline bool IsWarpKind(InteractKind k) { return k == InteractKind::Checkpoint || k == InteractKind::Waypoint; }

// Interactables seen in the current area persist (position, kind) until the
// area changes, so a transition that left the network bubble can still be
// chosen when the leader vanished near it.
class InteractableMemory {
public:
    // Merges this tick's visible interactables; clears everything when the area
    // counter changed. Entries not seen now keep their last state with
    // visibleNow = false.
    void Update(uint32_t areaCounter, const std::vector<Interactable>& visible, uint64_t nowMs);
    void Clear();

    // Visible now + remembered, in insertion order (stable for the UI).
    const std::vector<Interactable>& All() const { return m_list; }
    uint32_t AreaCounter() const { return m_area; }

private:
    uint32_t m_area = 0;
    bool     m_haveArea = false;
    std::vector<Interactable> m_list;
    std::unordered_map<uint32_t, size_t> m_index;   // entity id -> m_list index
};

}  // namespace fb
