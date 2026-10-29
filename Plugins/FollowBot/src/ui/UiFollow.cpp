// UiFollow.cpp - the Follow & Movement tab: following distances and goal,
// steering, sprint, navigation and stuck recovery.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "Tabs.h"

#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

void FollowBotPlugin::DrawTabFollow() {
    Profile& p = EditProfile();
    FollowSettings& f = p.follow;
    bool changed = false;
    const char* cells = I18n::Key("unit_cells");
    const char* ms = I18n::Key("unit_ms");
    const char* px = I18n::Key("unit_px");

    // --- Following ---------------------------------------------------------
    PluginUi::BeginCard("##following");
    Heading(ICON_FA_PERSON_WALKING, I18n::Tr("follow_following"), I18n::Tr("follow_following_help"));
    {
        const LabelColumn col({I18n::Tr("follow_distance"), I18n::Tr("follow_stop_distance"), I18n::Tr("follow_predict_ahead"),
                               I18n::Tr("follow_formation_slot"), I18n::Tr("follow_slot_spacing"),
                               I18n::Tr("follow_reacquire"), I18n::Tr("follow_lost_hold")});
        changed |= ui::FloatRow(col, I18n::Tr("follow_distance"), "##followDist", &f.followDistance, 6.f, 150.f, cells, "%.0f",
                                I18n::Tr("follow_distance_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("follow_stop_distance"), "##stopDist", &f.stopDistance, 2.f, 140.f, cells, "%.0f",
                                I18n::Tr("follow_stop_distance_tip"));
        ImGui::BeginDisabled(!f.predict);
        changed |= ui::IntRow(col, I18n::Tr("follow_predict_ahead"), "##predictMs", &f.predictMs, 0, 2000, ms,
                              I18n::Tr("follow_predict_ahead_tip"));
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, I18n::Tr("follow_formation_slot"), "##formSlot", &f.formationSlot, 0, 5, nullptr,
                              I18n::Tr("follow_formation_slot_tip"));
        ImGui::BeginDisabled(f.formationSlot == 0);
        changed |= ui::FloatRow(col, I18n::Tr("follow_slot_spacing"), "##formSpacing", &f.formationSpacing, 2.f, 30.f, cells,
                                "%.0f", I18n::Tr("follow_slot_spacing_tip"));
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, I18n::Tr("follow_reacquire"), "##reacquire", &f.reacquireMs, 1000, 60000, ms,
                              I18n::Tr("follow_reacquire_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_lost_hold"), "##lostHold", &f.lostHoldMs, 0, 10000, ms,
                              I18n::Tr("follow_lost_hold_tip"));
    }
    changed |= Check(I18n::Tr("follow_predict"), "predict", &f.predict, I18n::Tr("follow_predict_tip"));
    changed |= Check(I18n::Tr("follow_destination"), "followDest", &f.followDestination, I18n::Tr("follow_destination_tip"));
    changed |= Check(I18n::Tr("follow_in_town"), "followTown", &f.followInTownHideout, I18n::Tr("follow_in_town_tip"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Movement ------------------------------------------------------------
    PluginUi::BeginCard("##movement");
    Heading(ICON_FA_ROUTE, I18n::Tr("follow_steering"), I18n::Tr("follow_steering_help"));
    {
        const LabelColumn col({I18n::Tr("follow_look_ahead"), I18n::Tr("follow_cursor_distance"), I18n::Tr("follow_reaim_every"),
                               I18n::Tr("follow_reaim_moved")});
        changed |= ui::FloatRow(col, I18n::Tr("follow_look_ahead"), "##lookahead", &f.lookaheadCells, 4.f, 40.f, cells, "%.0f",
                                I18n::Tr("follow_look_ahead_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_cursor_distance"), "##cursorDist", &f.cursorDistancePx, 60, 600, px,
                              I18n::Tr("follow_cursor_distance_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_reaim_every"), "##reaimMs", &f.reaimMs, 16, 500, ms,
                              I18n::Tr("follow_reaim_every_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_reaim_moved"), "##reaimPx", &f.reaimPx, 1, 60, px,
                              I18n::Tr("follow_reaim_moved_tip"));
    }
    changed |= Check(I18n::Tr("follow_activation"), "activation", &f.activationClick, I18n::Tr("follow_activation_tip"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Sprint ----------------------------------------------------------------
    PluginUi::BeginCard("##sprint");
    Heading(ICON_FA_PERSON_RUNNING, I18n::Tr("follow_sprint"), I18n::Tr("follow_sprint_help"));
    changed |= Check(I18n::Tr("follow_sprint_on"), "sprint", &f.sprint, I18n::Tr("follow_sprint_on_tip"));
    ImGui::BeginDisabled(!f.sprint || f.sprintKey == 0);
    {
        const LabelColumn col({I18n::Tr("follow_sprint_farther"), I18n::Tr("follow_sprint_no_enemy"),
                               I18n::Tr("follow_sprint_hold"), I18n::Tr("follow_sprint_pause")});
        changed |= ui::FloatRow(col, I18n::Tr("follow_sprint_farther"), "##sprintDist", &f.sprintDistance, 10.f, 200.f, cells,
                                "%.0f", I18n::Tr("follow_sprint_farther_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("follow_sprint_no_enemy"), "##sprintEnemy", &f.sprintNoEnemyRadius, 0.f, 120.f,
                                cells, "%.0f", I18n::Tr("follow_sprint_no_enemy_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_sprint_hold"), "##sprintHold", &f.sprintMaxHoldMs, 500, 60000, ms,
                              I18n::Tr("follow_sprint_hold_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_sprint_pause"), "##sprintCd", &f.sprintCooldownMs, 0, 30000, ms,
                              I18n::Tr("follow_sprint_pause_tip"));
    }
    ImGui::EndDisabled();
    if (f.sprint && f.sprintKey == 0) Hint(I18n::Tr("follow_sprint_no_key"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Navigation ------------------------------------------------------------
    PluginUi::BeginCard("##nav");
    Heading(ICON_FA_DIAGRAM_PROJECT, I18n::Tr("follow_path_finding"), I18n::Tr("follow_path_finding_help"));
    {
        const LabelColumn col({I18n::Tr("follow_replan"), I18n::Tr("follow_wall_penalty"), I18n::Tr("follow_search_limit"),
                               I18n::Tr("follow_snap_radius")});
        changed |= ui::IntRow(col, I18n::Tr("follow_replan"), "##replan", &f.replanMs, 200, 10000, ms,
                              I18n::Tr("follow_replan_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("follow_wall_penalty"), "##wallPen", &f.wallPenalty, 0.f, 5.f, nullptr, "%.1f",
                                I18n::Tr("follow_wall_penalty_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_search_limit"), "##astarIter", &f.astarMaxIterations, 10000, 4000000,
                              nullptr, I18n::Tr("follow_search_limit_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_snap_radius"), "##snap", &f.snapRadius, 1, 100, cells,
                              I18n::Tr("follow_snap_radius_tip"));
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Stuck ------------------------------------------------------------------
    PluginUi::BeginCard("##stuck");
    Heading(ICON_FA_ARROWS_ROTATE, I18n::Tr("follow_unstuck"), I18n::Tr("follow_unstuck_help"));
    {
        StuckSettings& k = f.stuck;
        const LabelColumn col({I18n::Tr("follow_stuck_moved_less"), I18n::Tr("follow_stuck_within"),
                               I18n::Tr("follow_stuck_no_progress"), I18n::Tr("follow_stuck_calm"),
                               I18n::Tr("follow_stuck_block_radius"), I18n::Tr("follow_stuck_block_for"),
                               I18n::Tr("follow_stuck_side_step"), I18n::Tr("follow_stuck_door"),
                               I18n::Tr("follow_stuck_back"), I18n::Tr("follow_stuck_ignition"),
                               I18n::Tr("follow_stuck_retry")});
        changed |= ui::FloatRow(col, I18n::Tr("follow_stuck_moved_less"), "##stMin", &k.minDisplacement, 0.2f, 10.f, cells,
                                "%.1f", I18n::Tr("follow_stuck_moved_less_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_within"), "##stWin", &k.windowMs, 300, 10000, ms,
                              I18n::Tr("follow_stuck_within_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_no_progress"), "##stNoProg", &k.noProgressMs, 500, 20000, ms,
                              I18n::Tr("follow_stuck_no_progress_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_calm"), "##stReset", &k.ladderResetMs, 500, 30000, ms,
                              I18n::Tr("follow_stuck_calm_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_block_radius"), "##stBlockR", &k.blockRadius, 0, 8, cells,
                              I18n::Tr("follow_stuck_block_radius_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_block_for"), "##stBlockMs", &k.blockMs, 1000, 120000, ms,
                              I18n::Tr("follow_stuck_block_for_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_side_step"), "##stSide", &k.sideStepMs, 100, 3000, ms,
                              I18n::Tr("follow_stuck_side_step_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("follow_stuck_door"), "##stDoor", &k.doorSearchCells, 2.f, 40.f, cells, "%.0f",
                                I18n::Tr("follow_stuck_door_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("follow_stuck_back"), "##stBack", &k.backtrackMinCells, 2.f, 60.f, cells, "%.0f",
                                I18n::Tr("follow_stuck_back_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_ignition"), "##stIgn", &k.ignitionTimeoutMs, 0, 30000, ms,
                              I18n::Tr("follow_stuck_ignition_tip"));
        changed |= ui::IntRow(col, I18n::Tr("follow_stuck_retry"), "##stRetry", &k.followRetryMs, 500, 30000, ms,
                              I18n::Tr("follow_stuck_retry_tip"));
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    if (PluginUi::BeginDiagnostics(I18n::Tr("follow_diag_tip"), "followDiag")) {
        const BotView* v = m_uiView.get();
        if (!v) {
            Hint(I18n::Tr("common_no_data"));
        } else {
            const std::string moving =
                v->sprinting ? I18n::TrF("follow_diag_moving_sprint", {{"moving", ui::YesNo(v->moving)}})
                             : I18n::TrF("follow_diag_moving", {{"moving", ui::YesNo(v->moving)}});
            Hint(ui::Join({I18n::TrF("follow_diag_mode", {{"mode", ui::KeyText(v->mode)}}), moving,
                           I18n::TrF("follow_diag_plan", {{"plan", ui::KeyText(v->planStatus)},
                                                          {"points", Num("%d", static_cast<int>(v->path.size()))}})})
                     .c_str());
            Hint(I18n::TrF("follow_diag_leader",
                           {{"name", v->leaderName.empty() ? std::string(I18n::Tr("common_none")) : v->leaderName},
                            {"state", v->leaderVisible ? I18n::Tr("follow_diag_visible") : I18n::Tr("follow_diag_not_visible")},
                            {"cells", Num("%.0f", v->leaderDistance)},
                            {"speed", Num("%.1f", v->leaderSpeed)},
                            {"nodes", Num("%d", static_cast<int>(v->leaderRoute.size()))}})
                     .c_str());
            Hint(ui::Join({I18n::TrF("follow_diag_stuck", {{"count", Num("%d", v->stuckEpisodes)}}),
                           I18n::TrF("follow_diag_step", {{"step", ui::KeyText(v->stuckStep)}}),
                           I18n::TrF("follow_diag_avoided", {{"count", Num("%d", static_cast<int>(v->blocks.size()))}})})
                     .c_str());
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
