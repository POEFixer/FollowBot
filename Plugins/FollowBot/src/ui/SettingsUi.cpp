// SettingsUi.cpp - the settings tab bar, the status header and the General tab.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../config/Profiles.h"
#include "Tabs.h"

#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

int FollowBotPlugin::EditProfileIndex() const {
    int idx = -1;
    if (m_uiView) idx = m_uiView->profileIndex;
    if (idx < 0 || idx >= static_cast<int>(m_settings.profiles.size()))
        idx = ResolveProfileIndex(m_settings, m_uiView ? m_uiView->characterName : std::string());
    if (idx < 0) idx = 0;
    return idx;
}

Profile& FollowBotPlugin::EditProfile() {
    if (m_settings.profiles.empty()) m_settings.profiles.push_back(Profile{});
    return m_settings.profiles[static_cast<size_t>(EditProfileIndex())];
}

bool FollowBotPlugin::KeyRow(const char* id, int* vk, const char* tip, bool hotkey) {
    const int before = *vk;
    if (!PluginUi::HotkeyCapture(id, vk, tip)) return false;
    if (hotkey && !IsAllowedHotkey(*vk)) {
        // The bot clicks with the mouse itself: its own clicks must not trigger a hotkey.
        *vk = before;
        return false;
    }
    if (hotkey) OnKeyBound(*vk);
    else MarkDirty();
    return true;
}

void FollowBotPlugin::DrawSettings() {
    if (!ctx()->ImGuiContext) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
    EnsureLoaded();
    m_uiView = LatestView();
    bool wantsSkills = false;

    ImGui::PushID("FollowBotSettings");
    DrawHeader();
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##fbTabs")) {
        if (ImGui::BeginTabItem(I18n::TrId("tab_general"))) {
            DrawTabGeneral();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_follow"))) {
            DrawTabFollow();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_travel"))) {
            DrawTabTravel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_loot"))) {
            DrawTabLoot();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_skills"))) {
            wantsSkills = true;
            DrawTabSkills();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_stash"))) {
            DrawTabStash();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_safety"))) {
            DrawTabSafety();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(I18n::TrId("tab_debug"))) {
            wantsSkills = wantsSkills || m_showSkillsLive;
            DrawTabDebug();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    ImGui::PopID();
    m_uiWantsSkills = wantsSkills;

    if (m_dirty && !ImGui::IsAnyItemActive()) SaveSettings();
}

void FollowBotPlugin::DrawHeader() {
    const PluginUi::Palette pal = PluginUi::Colors();
    const BotView* v = m_uiView.get();
    PluginUi::BeginCard("##header");
    const bool running = m_running.load();
    const bool paused = m_paused.load();
    ImVec4 col = pal.TextSecondary;
    std::string pill;
    if (!m_enabled) {
        pill = I18n::Tr("header_plugin_disabled");
    } else if (!running) {
        pill = m_debugActive.load() ? I18n::Tr("task_debug_walk") : I18n::Tr("gate_stopped");
        if (m_debugActive.load()) col = pal.Accent;
    } else if (paused) {
        pill = I18n::Tr("header_paused");
        col = pal.Warning;
    } else if (v && !v->gateReason.empty()) {
        pill = I18n::TrF("header_holding", {{"why", ui::KeyText(v->gateReason)}});
        col = pal.Warning;
    } else {
        pill = v ? ui::KeyText(v->mode) : I18n::Tr("header_running");
        col = pal.Success;
    }
    PluginUi::StatusPill(pill.c_str(), ui::U32(col));
    ImGui::SameLine();
    if (!running) {
        if (ImGui::Button(ui::IconLabel(ICON_FA_PLAY, I18n::Tr("header_start"), "start").c_str())) StartBot();
        Tip(I18n::Tr("header_start_tip"));
    } else {
        if (ImGui::Button(ui::IconLabel(ICON_FA_STOP, I18n::Tr("header_stop"), "stop").c_str())) StopBot();
        ImGui::SameLine();
        if (PluginUi::SubtleButton(paused ? ui::IconLabel(ICON_FA_PLAY, I18n::Tr("header_resume"), "pause").c_str()
                                          : ui::IconLabel(ICON_FA_PAUSE, I18n::Tr("header_pause"), "pause").c_str()))
            TogglePause();
    }
    Hint(ui::Join({I18n::TrF("header_key_start_stop", {{"key", PluginUi::KeyName(m_settings.global.hotkeys.startStop)}}),
                   I18n::TrF("header_key_pause", {{"key", PluginUi::KeyName(m_settings.global.hotkeys.pause)}})})
             .c_str());
    if (v) {
        const std::string detail = ui::Text(v->taskDetail);
        if (v->leaderConfigured) {
            std::string leader;
            if (v->leaderVisible)
                leader = I18n::TrF("header_leader_cells", {{"name", v->leaderName}, {"cells", Num("%.0f", v->leaderDistance)}});
            else if (v->vanish != VanishKind::None)
                leader = I18n::TrF("header_leader_left", {{"name", v->leaderName}, {"kind", I18n::Tr(VanishKindName(v->vanish))}});
            else
                leader = I18n::TrF("header_leader_not_in_sight", {{"name", v->leaderName}});
            Hint(ui::Join({leader, detail}).c_str());
        } else {
            Hint(I18n::Tr("header_no_leader"));
        }
        if (!v->lastError.empty()) ui::Warning(ui::Text(v->lastError).c_str());
    }
    if (m_saveGuard.Holding()) {
        ui::Warning(m_saveGuard.Reason() == io::LoadStatus::Corrupt ? I18n::Tr("header_settings_corrupt")
                                                                    : I18n::Tr("header_settings_unreadable"));
    }
    PluginUi::EndCard();
}

void FollowBotPlugin::DrawTabGeneral() {
    const BotView* v = m_uiView.get();
    Profile& p = EditProfile();
    FollowSettings& f = p.follow;

    // --- Leader ---------------------------------------------------------
    PluginUi::BeginCard("##leader");
    Heading(ICON_FA_USER, I18n::Tr("general_leader"), I18n::Tr("general_leader_help"));
    {
        const LabelColumn col({I18n::Tr("general_character_name"), I18n::Tr("general_pick_player")});
        col.Row(I18n::Tr("general_character_name"));
        if (PluginUi::InputString("##leaderName", I18n::Tr("general_leader_hint"), f.leaderName)) MarkDirty();
        Tip(I18n::Tr("general_leader_name_tip"));
        col.Row(I18n::Tr("general_pick_player"));
        if (ImGui::BeginCombo("##pickLeader", I18n::Tr("general_choose"))) {
            if (!v || v->nearbyPlayers.empty()) ImGui::TextDisabled("%s", I18n::Tr("general_no_other_player"));
            else
                for (size_t i = 0; i < v->nearbyPlayers.size(); ++i) {
                    const std::string lbl = ui::WithId(v->nearbyPlayers[i], "pl" + std::to_string(i));
                    if (ImGui::Selectable(lbl.c_str())) {
                        f.leaderName = v->nearbyPlayers[i];
                        MarkDirty();
                    }
                }
            ImGui::EndCombo();
        }
        Tip(I18n::Tr("general_pick_player_tip"));
    }
    if (Check(I18n::Tr("general_leader_auto"), "leaderAuto", &f.leaderAuto, I18n::Tr("general_leader_auto_tip")))
        MarkDirty();
    if (Check(I18n::Tr("general_leader_case"), "leaderCase", &f.leaderCaseInsensitive, I18n::Tr("general_leader_case_tip")))
        MarkDirty();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Keys -----------------------------------------------------------
    PluginUi::BeginCard("##keys");
    Heading(ICON_FA_KEYBOARD, I18n::Tr("general_game_keys"), I18n::Tr("general_game_keys_help"));
    {
        const LabelColumn col({I18n::Tr("general_key_move"), I18n::Tr("general_key_sprint"), I18n::Tr("general_key_dodge"),
                               I18n::Tr("general_key_portal")});
        col.RowNoWidth(I18n::Tr("general_key_move"));
        KeyRow("##moveKey", &f.moveKey, I18n::Tr("general_key_move_tip"), false);
        col.RowNoWidth(I18n::Tr("general_key_sprint"));
        KeyRow("##sprintKey", &f.sprintKey, I18n::Tr("general_key_sprint_tip"), false);
        col.RowNoWidth(I18n::Tr("general_key_dodge"));
        KeyRow("##dodgeKey", &f.dodgeKey, I18n::Tr("general_key_dodge_tip"), false);
        col.RowNoWidth(I18n::Tr("general_key_portal"));
        KeyRow("##portalKey", &p.stash.portalKey, I18n::Tr("general_key_portal_tip"), false);
    }
    if (Check(I18n::Tr("general_click_to_move"), "clickToMove", &f.clickToMove, I18n::Tr("general_click_to_move_tip")))
        MarkDirty();
    if (f.moveKey == 0 && !f.clickToMove) ui::Warning(I18n::Tr("general_no_move_key"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Hotkeys --------------------------------------------------------
    PluginUi::BeginCard("##hotkeys");
    Heading(ICON_FA_GAMEPAD, I18n::Tr("general_hotkeys"), I18n::Tr("general_hotkeys_help"));
    {
        HotkeySettings& hk = m_settings.global.hotkeys;
        const LabelColumn col({I18n::Tr("general_hk_start_stop"), I18n::Tr("general_hk_pause"),
                               I18n::Tr("general_hk_override"), I18n::Tr("general_hk_stash")});
        col.RowNoWidth(I18n::Tr("general_hk_start_stop"));
        KeyRow("##hkStart", &hk.startStop, I18n::Tr("general_hk_start_stop_tip"), true);
        col.RowNoWidth(I18n::Tr("general_hk_pause"));
        KeyRow("##hkPause", &hk.pause, I18n::Tr("general_hk_pause_tip"), true);
        col.RowNoWidth(I18n::Tr("general_hk_override"));
        KeyRow("##hkOverride", &hk.holdOverride, I18n::Tr("general_hk_override_tip"), true);
        col.RowNoWidth(I18n::Tr("general_hk_stash"));
        KeyRow("##hkStash", &hk.stashNow, I18n::Tr("general_hk_stash_tip"), true);
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Profiles -------------------------------------------------------
    PluginUi::BeginCard("##profiles");
    Heading(ICON_FA_ID_CARD, I18n::Tr("profile_title"), I18n::Tr("profile_help"));
    const int editIdx = EditProfileIndex();
    {
        const LabelColumn col({I18n::Tr("profile_in_use"), I18n::Tr("profile_for_character")});
        col.Row(I18n::Tr("profile_in_use"));
        if (ImGui::BeginCombo("##profile", ui::ProfileName(m_settings.profiles[static_cast<size_t>(editIdx)].name).c_str())) {
            for (size_t i = 0; i < m_settings.profiles.size(); ++i) {
                const std::string lbl =
                    ui::WithId(ui::ProfileName(m_settings.profiles[i].name), "prof" + std::to_string(i));
                if (ImGui::Selectable(lbl.c_str(), static_cast<int>(i) == editIdx)) {
                    m_settings.activeProfile = m_settings.profiles[i].name;
                    MarkDirty();
                }
            }
            ImGui::EndCombo();
        }
        Tip(I18n::Tr("profile_in_use_tip"));
        col.Row(I18n::Tr("profile_for_character"));
        if (PluginUi::InputString("##profChar", I18n::Tr("profile_character_hint"), p.character)) MarkDirty();
        Tip(I18n::Tr("profile_for_character_tip"));
    }
    if (v && !v->characterName.empty()) {
        if (PluginUi::SubtleButton(I18n::TrId("profile_use_current"))) {
            p.character = v->characterName;
            MarkDirty();
        }
        Tip(I18n::Tr("profile_use_current_tip"));
    }
    if (Check(I18n::Tr("profile_auto_select"), "autoSelect", &m_settings.autoSelectByCharacter,
              I18n::Tr("profile_auto_select_tip")))
        MarkDirty();
    if (v && v->profileAuto) Hint(I18n::Tr("profile_auto_selected"));
    if (ImGui::Button(I18n::TrId("profile_new"))) {
        const int idx = AddProfile(m_settings, "Profile");
        if (idx >= 0) {
            m_settings.activeProfile = m_settings.profiles[static_cast<size_t>(idx)].name;
            MarkDirty();
        }
    }
    ImGui::SameLine();
    if (PluginUi::SubtleButton(I18n::TrId("profile_duplicate"))) {
        const int idx = DuplicateProfile(m_settings, editIdx);
        if (idx >= 0) {
            m_settings.activeProfile = m_settings.profiles[static_cast<size_t>(idx)].name;
            MarkDirty();
        }
    }
    ImGui::SameLine();
    if (PluginUi::SubtleButton(I18n::TrId("profile_rename"))) {
        m_renameBuf = m_settings.profiles[static_cast<size_t>(editIdx)].name;
        ImGui::OpenPopup("##renameProfile");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_settings.profiles.size() <= 1);
    if (PluginUi::SubtleButton(I18n::TrId("profile_delete"))) ImGui::OpenPopup("##deleteProfile");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##renameProfile")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        PluginUi::InputString("##renameInput", I18n::Tr("profile_new_name_hint"), m_renameBuf);
        if (ImGui::Button(ui::WithId(I18n::Tr("profile_rename"), "doRename").c_str())) {
            if (RenameProfile(m_settings, editIdx, m_renameBuf)) MarkDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PluginUi::SubtleButton(I18n::TrId("common_cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (PluginUi::ConfirmPopup("##deleteProfile", I18n::Tr("profile_delete_confirm"), I18n::Tr("profile_delete"))) {
        if (DeleteProfile(m_settings, editIdx)) MarkDirty();
    }
    PluginUi::EndCard();
    ImGui::Spacing();
    Hint(I18n::Tr("general_disclaimer"));
}
