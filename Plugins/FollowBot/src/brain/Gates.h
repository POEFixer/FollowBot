// Gates.h - when the bot must hold still (design §5.2 step 1). Pure: no SDK.
//
// Every reason releases all keys and publishes the reason. Cursor-busy (the
// Pick My Loot channel) is not a gate: it only defers cursor work for a tick.
#pragma once

#include "../world/World.h"

#include <cstdint>

namespace fb {

enum class GateReason : uint8_t {
    None = 0,
    Stopped,         // not started
    MaxRuntime,      // the run-time limit is reached (the bot stops)
    Paused,          // pause / panic hotkey
    OverrideHeld,    // hold-to-override key held (or released < resume time ago)
    NotAttached,
    Loading,
    EscapeMenu,
    NotInGame,
    NotForeground,   // another window (or the POEFixer overlay itself) is in front
    MenuVisible,     // the POEFixer menu is open
    HostAutomation,  // POEFixer drives input (crafting, memory-mode trade / Manual Buy, auto-stash, recovery, auto-login; host 365+)
    UserActivity,    // you used the mouse / keyboard in the game a moment ago
    PanelUnknown,    // no fresh inventory listing yet (fail-safe: a panel may be open)
    ForeignPanel,    // an inventory / stash panel the bot did not open is visible
    Count
};

const char* GateReasonText(GateReason r);

struct GateInputs {
    uint64_t now = 0;
    bool     running = false;
    bool     paused = false;
    bool     overrideHeld = false;
    uint64_t overrideReleasedMs = 0;   // 0 = never held
    uint64_t lastUserActivityMs = 0;   // 0 = none
    bool     botOwnsPanel = false;     // a stash task runs (it opened the panel)
    uint64_t foreignPanelSinceMs = 0;  // tracked by the brain; 0 = none
    uint64_t startedMs = 0;
};

struct GateParams {
    bool userActivityPause = true;
    int  userActivityPauseMs = 1500;
    int  overrideResumeMs = 600;
    int  maxRuntimeMinutes = 0;
    bool closeForeignPanels = false;
    int  closeForeignPanelsAfterMs = 3000;
};

struct GateResult {
    GateReason reason = GateReason::None;
    bool       closePanel = false;   // ForeignPanel: send Esc (the option is on and it stayed open long enough)
    bool       stopBot = false;      // MaxRuntime: stop, not just hold
};

GateResult EvaluateGates(const GameGate& g, const InventoryState& inv, const GateInputs& in, const GateParams& p);

// Panel visible that is not the bot's: from a fresh listing only.
inline bool ForeignPanelVisible(const InventoryState& inv) { return inv.listingFresh && inv.anyPanelOpen; }

}  // namespace fb
