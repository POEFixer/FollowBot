// UiSafety.cpp - the Death & Safety tab: respawn, death limits, safety stops,
// humanisation and the worker rate.
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

void FollowBotPlugin::DrawTabSafety() {
    Profile& p = EditProfile();
    DeathSettings& d = p.death;
    SafetySettings& sf = m_settings.global.safety;
    HumanSettings& hu = m_settings.global.human;
    const BotView* v = m_uiView.get();
    bool changed = false;
    const char* ms = I18n::Key("unit_ms");
    const char* px = I18n::Key("unit_px");

    // --- Death -----------------------------------------------------------
    PluginUi::BeginCard("##death");
    Heading(ICON_FA_SKULL, I18n::Tr("safety_death"), I18n::Tr("safety_death_help"));
    changed |= Check(I18n::Tr("safety_auto_respawn"), "autoRespawn", &d.autoRespawn, I18n::Tr("safety_auto_respawn_tip"));
    ImGui::BeginDisabled(!d.autoRespawn);
    changed |= Check(I18n::Tr("safety_prefer_cp"), "preferCp", &d.preferCheckpoint, I18n::Tr("safety_prefer_cp_tip"));
    changed |= Check(I18n::Tr("safety_generic"), "genericResp", &d.allowGenericRespawn, I18n::Tr("safety_generic_tip"));
    {
        const LabelColumn col({I18n::Tr("safety_resp_delay"), I18n::Tr("safety_random_extra"), I18n::Tr("safety_retry_every"),
                               I18n::Tr("safety_tries")});
        changed |= ui::IntRow(col, I18n::Tr("safety_resp_delay"), "##respDelay", &d.respawnDelayMs, 500, 15000, ms,
                              I18n::Tr("safety_resp_delay_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_random_extra"), "##respJitter", &d.respawnJitterMs, 0, 5000, ms,
                              I18n::Tr("safety_resp_jitter_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_retry_every"), "##respRetry", &d.retryIntervalMs, 500, 10000, ms,
                              I18n::Tr("safety_retry_every_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_tries"), "##respTries", &d.maxRetries, 1, 20, nullptr,
                              I18n::Tr("safety_tries_tip"));
    }
    ImGui::EndDisabled();
    {
        const LabelColumn col({I18n::Tr("safety_max_deaths_map"), I18n::Tr("safety_max_deaths_session"),
                               I18n::Tr("safety_dead_after")});
        changed |= ui::IntRow(col, I18n::Tr("safety_max_deaths_map"), "##maxDeathMap", &d.maxDeathsPerMap, 0, 50, nullptr,
                              I18n::Tr("safety_max_deaths_map_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_max_deaths_session"), "##maxDeathSess", &d.maxDeathsPerSession, 0, 500,
                              nullptr, I18n::Tr("safety_max_deaths_session_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_dead_after"), "##deadConfirm", &d.deadConfirmMs, 100, 3000, ms,
                              I18n::Tr("safety_dead_after_tip"));
    }
    if (!ctx()->Ui.HasFindElements()) ui::HostNote(I18n::Tr("safety_respawn_old_host"));
    if (v)
        Hint(I18n::TrF("safety_deaths", {{"map", Num("%d", v->deathsMap)}, {"session", Num("%d", v->deathsSession)}}).c_str());
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Safety ----------------------------------------------------------
    PluginUi::BeginCard("##safety");
    Heading(ICON_FA_SHIELD_HALVED, I18n::Tr("safety_safety"), I18n::Tr("safety_safety_help"));
    changed |= Check(I18n::Tr("safety_user_pause"), "userPause", &sf.userActivityPause, I18n::Tr("safety_user_pause_tip"));
    {
        const LabelColumn col({I18n::Tr("safety_pause_for"), I18n::Tr("safety_override_resume"), I18n::Tr("safety_stop_after"),
                               I18n::Tr("safety_close_after")});
        ImGui::BeginDisabled(!sf.userActivityPause);
        changed |= ui::IntRow(col, I18n::Tr("safety_pause_for"), "##userPauseMs", &sf.userActivityPauseMs, 200, 30000, ms,
                              I18n::Tr("safety_pause_for_tip"));
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, I18n::Tr("safety_override_resume"), "##ovResume", &sf.overrideResumeMs, 0, 5000, ms,
                              I18n::Tr("safety_override_resume_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_stop_after"), "##maxRun", &sf.maxRuntimeMinutes, 0, 1440,
                              I18n::Key("unit_min"), I18n::Tr("safety_stop_after_tip"));
        ImGui::BeginDisabled(!sf.closeForeignPanels);
        changed |= ui::IntRow(col, I18n::Tr("safety_close_after"), "##closePanelMs", &sf.closeForeignPanelsAfterMs, 1000, 30000,
                              ms, I18n::Tr("safety_close_after_tip"));
        ImGui::EndDisabled();
    }
    changed |= Check(I18n::Tr("safety_close_panels"), "closePanels", &sf.closeForeignPanels, I18n::Tr("safety_close_panels_tip"));
    changed |= Check(I18n::Tr("safety_emergency"), "emergency", &sf.emergencyExit, I18n::Tr("safety_emergency_tip"));
    if (sf.emergencyExit) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        const std::string fmt = ui::Units(I18n::Key("safety_emergency_below"), "%d");
        changed |= ImGui::SliderInt("##emergencyHp", &sf.emergencyExitHpPct, 1, 90, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp);
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Humanisation ----------------------------------------------------
    PluginUi::BeginCard("##human");
    Heading(ICON_FA_HAND, I18n::Tr("safety_input_timing"), I18n::Tr("safety_input_timing_help"));
    {
        const LabelColumn col({I18n::Tr("safety_min_gap"), I18n::Tr("safety_random_extra"), I18n::Tr("safety_click_spread"),
                               I18n::Tr("safety_cursor_speed"), I18n::Tr("safety_cursor_curve"),
                               I18n::Tr("safety_cursor_tremor"), I18n::Tr("safety_reaim_jitter"), I18n::Tr("safety_bot_tick")});
        changed |= ui::IntRow(col, I18n::Tr("safety_min_gap"), "##gateMin", &hu.actionMinIntervalMs, 30, 1000, ms,
                              I18n::Tr("safety_min_gap_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_random_extra"), "##gateJit", &hu.actionJitterMs, 0, 500, ms,
                              I18n::Tr("safety_gate_jitter_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_click_spread"), "##clickJit", &hu.clickJitterPx, 0, 20, px,
                              I18n::Tr("safety_click_spread_tip"));
        ImGui::BeginDisabled(!hu.humanMouse);
        changed |= ui::FloatRow(col, I18n::Tr("safety_cursor_speed"), "##hmSpeed", &hu.mouseSpeed, 0.2f, 3.f, nullptr, "%.2f",
                                I18n::Tr("safety_cursor_speed_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("safety_cursor_curve"), "##hmCurve", &hu.mouseCurvature, 0.f, 3.f, nullptr, "%.2f",
                                I18n::Tr("safety_cursor_curve_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("safety_cursor_tremor"), "##hmTremor", &hu.mouseTremor, 0.f, 3.f, nullptr, "%.2f",
                                I18n::Tr("safety_cursor_tremor_tip"));
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, I18n::Tr("safety_reaim_jitter"), "##reaimJit", &hu.reaimJitterMs, 0, 200, ms,
                              I18n::Tr("safety_reaim_jitter_tip"));
        changed |= ui::IntRow(col, I18n::Tr("safety_bot_tick"), "##tickMs", &m_settings.global.tickMs, 10, 50, ms,
                              I18n::Tr("safety_bot_tick_tip"));
    }
    changed |= Check(I18n::Tr("safety_human_mouse"), "humanMouse", &hu.humanMouse, I18n::Tr("safety_human_mouse_tip"));
    PluginUi::EndCard();
    if (changed) MarkDirty();
}
