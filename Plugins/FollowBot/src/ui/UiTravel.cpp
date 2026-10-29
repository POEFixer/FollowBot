// UiTravel.cpp - the Travel tab: following the leader through area
// transitions, portals and boss arena entrances (design §5.7), with the last
// vanish and the last failure in its Diagnostics.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../common/Clock.h"
#include "../travel/TravelLogic.h"
#include "Tabs.h"

#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;

void FollowBotPlugin::DrawTabTravel() {
    TravelSettings& t = EditProfile().travel;
    const BotView* v = m_uiView.get();
    bool changed = false;
    const char* cells = I18n::Key("unit_cells");
    const char* ms = I18n::Key("unit_ms");

    // --- What to follow ------------------------------------------------------
    PluginUi::BeginCard("##travelWhat");
    Heading(ICON_FA_DOOR_OPEN, I18n::Tr("travel_follow_through"), I18n::Tr("travel_follow_through_help"));
    changed |= Check(I18n::Tr("travel_follow_transitions"), "followTrans", &t.followTransitions,
                     I18n::Tr("travel_follow_transitions_tip"));
    changed |= Check(I18n::Tr("travel_follow_portals"), "followPortals", &t.followPortals, I18n::Tr("travel_follow_portals_tip"));
    ImGui::Indent();
    ImGui::BeginDisabled(!t.followPortals);
    changed |= Check(I18n::Tr("travel_follow_other_portals"), "followOtherPortals", &t.followOtherPortalKinds,
                     I18n::Tr("travel_follow_other_portals_tip"));
    ImGui::EndDisabled();
    ImGui::Unindent();
    changed |= Check(I18n::Tr("travel_follow_arenas"), "followArenas", &t.enterBossArenas, I18n::Tr("travel_follow_arenas_tip"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Recognising where he went ----------------------------------------------
    PluginUi::BeginCard("##travelWhere");
    Heading(ICON_FA_MAGNIFYING_GLASS_LOCATION, I18n::Tr("travel_where"), I18n::Tr("travel_where_help"));
    {
        const LabelColumn col({I18n::Tr("travel_vanish_radius"), I18n::Tr("travel_out_of_range"), I18n::Tr("travel_fallback_radius")});
        changed |= ui::FloatRow(col, I18n::Tr("travel_vanish_radius"), "##vanishR", &t.vanishRadius, 3.f, 30.f, cells, "%.0f",
                                I18n::Tr("travel_vanish_radius_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("travel_out_of_range"), "##oor", &t.outOfRangeCells, 60.f, 200.f, cells, "%.0f",
                                I18n::Tr("travel_out_of_range_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("travel_fallback_radius"), "##fallbackR", &t.fallbackRadius, 5.f, 80.f, cells,
                                "%.0f", I18n::Tr("travel_fallback_radius_tip"));
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Clicking ------------------------------------------------------------------
    PluginUi::BeginCard("##travelClick");
    Heading(ICON_FA_HAND_POINTER, I18n::Tr("travel_clicking"), I18n::Tr("travel_clicking_help"));
    {
        const LabelColumn col({I18n::Tr("travel_click_from"), I18n::Tr("travel_hover"), I18n::Tr("travel_hold"),
                               I18n::Tr("travel_rehovers"), I18n::Tr("travel_aim_height")});
        changed |= ui::FloatRow(col, I18n::Tr("travel_click_from"), "##interactR", &t.interactRange, 2.f, 20.f, cells, "%.0f",
                                I18n::Tr("travel_click_from_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_hover"), "##dwell", &t.hoverDwellMs, 40, 400, ms, I18n::Tr("travel_hover_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_hold"), "##hold", &t.pressHoldMs, 20, 200, ms, I18n::Tr("travel_hold_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_rehovers"), "##rehover", &t.reHovers, 0, 4, nullptr,
                              I18n::Tr("travel_rehovers_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("travel_aim_height"), "##aimH", &t.tallObjectHeightFactor, 0.f, 1.f, nullptr,
                                "%.2f", I18n::Tr("travel_aim_height_tip"));
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Retries -------------------------------------------------------------------
    PluginUi::BeginCard("##travelRetry");
    Heading(ICON_FA_ROTATE, I18n::Tr("travel_retries"), I18n::Tr("travel_retries_help"));
    {
        const LabelColumn col({I18n::Tr("travel_area_wait"), I18n::Tr("travel_tries"), I18n::Tr("travel_find_object")});
        changed |= ui::IntRow(col, I18n::Tr("travel_area_wait"), "##areaWait", &t.areaChangeTimeoutMs, 2000, 20000, ms,
                              I18n::Tr("travel_area_wait_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_tries"), "##tries", &t.maxAttempts, 1, 6, nullptr,
                              I18n::Tr("travel_tries_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_find_object"), "##findObj", &t.findObjectMs, 1000, 15000, ms,
                              I18n::Tr("travel_find_object_tip"));
    }
    changed |= Check(I18n::Tr("travel_seek_on_failure"), "seekFail", &t.seekOnFailure, I18n::Tr("travel_seek_on_failure_tip"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Diagnostics -------------------------------------------------------------
    if (PluginUi::BeginDiagnostics(I18n::Tr("travel_diag_tip"), "travelDiag")) {
        const TravelDiag d = TravelTask::Diagnostics();
        if (v) {
            const char* leader = v->leaderVisible      ? I18n::Tr("travel_diag_in_sight")
                                 : v->leaderConfigured ? I18n::Tr("travel_diag_not_in_sight")
                                                       : I18n::Tr("travel_diag_not_set");
            const char* now = v->vanish != VanishKind::None ? I18n::Tr(VanishKindName(v->vanish)) : I18n::Tr("vanish_none");
            Hint(ui::Join({I18n::TrF("travel_diag_leader", {{"state", leader}}),
                           I18n::TrF("travel_diag_vanished", {{"kind", now}}),
                           I18n::TrF("travel_diag_task", {{"task", v->taskName.empty() ? I18n::Tr("common_none")
                                                                                       : ui::KeyText(v->taskName)}})})
                     .c_str());
            if (v->taskName == I18n::Key("task_travel") && !v->taskDetail.empty()) Hint(ui::Text(v->taskDetail).c_str());
        }
        if (!d.haveVanish) {
            Hint(I18n::Tr("travel_diag_no_vanish"));
        } else {
            const uint64_t nowMs = NowMs();
            const double ago = d.vanishAtMs && nowMs >= d.vanishAtMs ? static_cast<double>(nowMs - d.vanishAtMs) / 1000.0 : 0.0;
            std::string state = I18n::Tr(d.state && d.state[0] ? d.state : "common_none");
            if (!d.detail.empty()) state = I18n::TrF("travel_diag_state_detail", {{"state", state}, {"detail", ui::Text(d.detail)}});
            Hint(ui::Join({I18n::TrF("travel_diag_last_vanish", {{"kind", I18n::Tr(VanishKindName(d.kind))},
                                                                 {"seconds", Num("%.0f", ago)}}),
                           state})
                     .c_str());
            if (d.haveObject) {
                std::string kind = I18n::Tr(InteractKindName(d.objectKind));
                if (d.objectKind == InteractKind::Portal)
                    kind = I18n::TrF("travel_diag_portal_kind", {{"kind", kind}, {"portal", I18n::Tr(travel::PortalKindName(d.portalType))}});
                Hint(I18n::TrF(d.fallbackObject ? I18n::Key("travel_diag_object_other") : I18n::Key("travel_diag_object_his"),
                               {{"kind", kind},
                                {"id", Num("%u", d.objectId)},
                                {"name", d.objectName.empty() ? std::string("?") : d.objectName},
                                {"cells", Num("%.0f", d.objectDistance)}})
                         .c_str());
            }
            Hint(ui::Join({I18n::TrF("travel_diag_attempts", {{"attempts", Num("%d", d.attempts)}, {"max", Num("%d", d.maxAttempts)}}),
                           I18n::TrF("travel_diag_totals", {{"followed", Num("%d", d.followed)}, {"failed", Num("%d", d.failed)}})})
                     .c_str());
        }
        if (!d.lastFailure.empty()) {
            const uint64_t nowMs = NowMs();
            const double ago = nowMs >= d.lastFailureMs ? static_cast<double>(nowMs - d.lastFailureMs) / 1000.0 : 0.0;
            ui::Warning(I18n::TrF("travel_diag_last_failure", {{"seconds", Num("%.0f", ago)}, {"why", ui::Text(d.lastFailure)}})
                            .c_str());
        }
        if (v) {
            int transitions = 0, portals = 0, arenas = 0;
            for (const auto& it : v->interactables) {
                if (it.kind == InteractKind::AreaTransition) ++transitions;
                else if (it.kind == InteractKind::Portal) ++portals;
                else if (it.kind == InteractKind::BossArena) ++arenas;
            }
            Hint(I18n::TrF("travel_diag_known", {{"transitions", Num("%d", transitions)},
                                                 {"portals", Num("%d", portals)},
                                                 {"arenas", Num("%d", arenas)}})
                     .c_str());
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
