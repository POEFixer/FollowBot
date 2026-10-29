// Gates.cpp - see Gates.h.
#include "Gates.h"

namespace fb {

const char* GateReasonText(GateReason r) {
    switch (r) {
        case GateReason::None:           return "";
        case GateReason::Stopped:        return Key("gate_stopped");
        case GateReason::MaxRuntime:     return Key("gate_max_runtime");
        case GateReason::Paused:         return Key("gate_paused");
        case GateReason::OverrideHeld:   return Key("gate_override_held");
        case GateReason::NotAttached:    return Key("gate_not_attached");
        case GateReason::Loading:        return Key("gate_loading");
        case GateReason::EscapeMenu:     return Key("gate_escape_menu");
        case GateReason::NotInGame:      return Key("gate_not_in_game");
        case GateReason::NotForeground:  return Key("gate_not_foreground");
        case GateReason::MenuVisible:    return Key("gate_menu_visible");
        case GateReason::HostAutomation: return Key("gate_host_automation");
        case GateReason::UserActivity:   return Key("gate_user_activity");
        case GateReason::PanelUnknown:   return Key("gate_panel_unknown");
        case GateReason::ForeignPanel:   return Key("gate_foreign_panel");
        case GateReason::Count:          break;
    }
    return Key("common_unknown");
}

GateResult EvaluateGates(const GameGate& g, const InventoryState& inv, const GateInputs& in, const GateParams& p) {
    GateResult r;
    auto hold = [&](GateReason why) {
        r.reason = why;
        return r;
    };
    if (!in.running) return hold(GateReason::Stopped);
    if (p.maxRuntimeMinutes > 0 && in.startedMs != 0 && in.now >= in.startedMs &&
        in.now - in.startedMs >= static_cast<uint64_t>(p.maxRuntimeMinutes) * 60000ull) {
        r.stopBot = true;
        return hold(GateReason::MaxRuntime);
    }
    if (in.paused) return hold(GateReason::Paused);
    if (in.overrideHeld) return hold(GateReason::OverrideHeld);
    if (in.overrideReleasedMs != 0 && in.now >= in.overrideReleasedMs &&
        in.now - in.overrideReleasedMs < static_cast<uint64_t>(p.overrideResumeMs))
        return hold(GateReason::OverrideHeld);
    if (!g.attached) return hold(GateReason::NotAttached);
    if (g.loading) return hold(GateReason::Loading);
    if (g.escapeMenu) return hold(GateReason::EscapeMenu);
    if (!g.inGame) return hold(GateReason::NotInGame);
    if (!g.foregroundExact) return hold(GateReason::NotForeground);
    if (g.menuVisible) return hold(GateReason::MenuVisible);
    if (g.hostAutomation) return hold(GateReason::HostAutomation);
    if (p.userActivityPause && in.lastUserActivityMs != 0 && in.now >= in.lastUserActivityMs &&
        in.now - in.lastUserActivityMs < static_cast<uint64_t>(p.userActivityPauseMs))
        return hold(GateReason::UserActivity);
    if (!in.botOwnsPanel) {
        if (!inv.listingFresh) return hold(GateReason::PanelUnknown);
        if (inv.anyPanelOpen) {
            r.closePanel = p.closeForeignPanels && in.foreignPanelSinceMs != 0 && in.now >= in.foreignPanelSinceMs &&
                           in.now - in.foreignPanelSinceMs >= static_cast<uint64_t>(p.closeForeignPanelsAfterMs);
            return hold(GateReason::ForeignPanel);
        }
    }
    return r;
}

}  // namespace fb
