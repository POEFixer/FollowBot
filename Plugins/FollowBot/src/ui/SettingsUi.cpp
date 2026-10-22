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
        if (ImGui::BeginTabItem("General###tabGeneral")) {
            DrawTabGeneral();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Follow & Movement###tabFollow")) {
            DrawTabFollow();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Travel###tabTravel")) {
            DrawTabTravel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loot###tabLoot")) {
            DrawTabLoot();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Skills###tabSkills")) {
            wantsSkills = true;
            DrawTabSkills();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stash###tabStash")) {
            DrawTabStash();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Death & Safety###tabSafety")) {
            DrawTabSafety();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug###tabDebug")) {
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
        pill = "Plugin disabled";
    } else if (!running) {
        pill = m_debugActive.load() ? "Debug walk" : "Stopped";
        if (m_debugActive.load()) col = pal.Accent;
    } else if (paused) {
        pill = "Paused";
        col = pal.Warning;
    } else if (v && !v->gateReason.empty()) {
        pill = "Holding: " + v->gateReason;
        col = pal.Warning;
    } else {
        pill = v ? v->mode : std::string("Running");
        col = pal.Success;
    }
    PluginUi::StatusPill(pill.c_str(), ui::U32(col));
    ImGui::SameLine();
    if (!running) {
        if (ImGui::Button(ICON_FA_PLAY "  Start###start")) StartBot();
        Tip("Starts following. The bot only acts while the game window is in front and the POEFixer menu is closed.");
    } else {
        if (ImGui::Button(ICON_FA_STOP "  Stop###stop")) StopBot();
        ImGui::SameLine();
        if (PluginUi::SubtleButton(paused ? ICON_FA_PLAY "  Resume###pause" : ICON_FA_PAUSE "  Pause###pause")) TogglePause();
    }
    char keys[160];
    std::snprintf(keys, sizeof(keys), "Start / stop: %s   |   Pause: %s",
                  PluginUi::KeyName(m_settings.global.hotkeys.startStop), PluginUi::KeyName(m_settings.global.hotkeys.pause));
    Hint(keys);
    if (v) {
        char line[256];
        if (v->leaderConfigured) {
            if (v->leaderVisible)
                std::snprintf(line, sizeof(line), "Leader %s: %.0f cells away   |   %s", v->leaderName.c_str(),
                              v->leaderDistance, v->taskDetail.c_str());
            else
                std::snprintf(line, sizeof(line), "Leader %s: not in sight%s%s   |   %s", v->leaderName.c_str(),
                              v->vanish != VanishKind::None ? " (left: " : "",
                              v->vanish != VanishKind::None ? (std::string(VanishKindName(v->vanish)) + ")").c_str() : "",
                              v->taskDetail.c_str());
        } else {
            std::snprintf(line, sizeof(line), "No leader set: enter his character name below.");
        }
        Hint(line);
        if (!v->lastError.empty()) ui::Warning(v->lastError.c_str());
    }
    if (m_saveGuard.Holding()) {
        ui::Warning(m_saveGuard.Reason() == io::LoadStatus::Corrupt
                        ? "Your settings file could not be read (not valid JSON). A copy was kept as "
                          "config/settings.json.corrupt. The defaults are in use and nothing is saved until you "
                          "change a setting."
                        : "Your settings file exists but could not be opened (another program may be using it). "
                          "The defaults are in use; the file is read again shortly.");
    }
    PluginUi::EndCard();
}

void FollowBotPlugin::DrawTabGeneral() {
    const BotView* v = m_uiView.get();
    Profile& p = EditProfile();
    FollowSettings& f = p.follow;

    // --- Leader ---------------------------------------------------------
    PluginUi::BeginCard("##leader");
    Heading(ICON_FA_USER, "Leader",
            "The character the bot follows. It is found by its character name among the players in your area\n"
            "(the game has no readable party list).");
    {
        const LabelColumn col({"Character name", "Pick a nearby player"});
        col.Row("Character name");
        if (PluginUi::InputString("##leaderName", "e.g. MyLeader", f.leaderName)) MarkDirty();
        Tip("The leader's character name, exactly as the game shows it.\nUpper / lower case does not matter (unless you turn that off below).");
        col.Row("Pick a nearby player");
        const char* preview = "Choose...";
        if (ImGui::BeginCombo("##pickLeader", preview)) {
            if (!v || v->nearbyPlayers.empty()) ImGui::TextDisabled("No other player in the area");
            else
                for (size_t i = 0; i < v->nearbyPlayers.size(); ++i) {
                    char lbl[160];
                    std::snprintf(lbl, sizeof(lbl), "%s###pl%d", v->nearbyPlayers[i].c_str(), static_cast<int>(i));
                    if (ImGui::Selectable(lbl)) {
                        f.leaderName = v->nearbyPlayers[i];
                        MarkDirty();
                    }
                }
            ImGui::EndCombo();
        }
        Tip("Fills the name from the players the bot sees around you right now.");
    }
    if (Check("Auto: follow the only other player", "leaderAuto", &f.leaderAuto,
              "When no name is set (or the named player is not here) and exactly one other player is in the\n"
              "area, follow that player."))
        MarkDirty();
    if (Check("Ignore upper / lower case", "leaderCase", &f.leaderCaseInsensitive, "Match the name without regard to case."))
        MarkDirty();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Keys -----------------------------------------------------------
    PluginUi::BeginCard("##keys");
    Heading(ICON_FA_KEYBOARD, "Game keys",
            "The keys the bot presses in the game. The game's key bindings cannot be read: set them here exactly as\n"
            "they are bound in the game's Options -> Input.");
    {
        const LabelColumn col({"Move", "Dodge / Sprint", "Dodge only", "Portal"});
        col.RowNoWidth("Move");
        KeyRow("##moveKey", &f.moveKey,
               "Bind \"Move Only\" to this key in the game (Options -> Input), then set the same key here.\n"
               "The bot holds it while the cursor points where to go.", false);
        col.RowNoWidth("Dodge / Sprint");
        KeyRow("##sprintKey", &f.sprintKey, "Your Dodge Roll key. Held, the character sprints after the roll (long straight stretches).", false);
        col.RowNoWidth("Dodge only");
        KeyRow("##dodgeKey", &f.dodgeKey, "Optional: a key that only dodges (stuck recovery). Without it the Dodge / Sprint key is tapped.", false);
        col.RowNoWidth("Portal");
        KeyRow("##portalKey", &p.stash.portalKey, "The key that opens a portal (a skill or scroll), for leaving the map to stash.", false);
    }
    if (Check("Move with the left mouse button instead", "clickToMove", &f.clickToMove,
              "Holds the left mouse button instead of a Move key.\nNot recommended: it also attacks or picks up whatever is under the cursor."))
        MarkDirty();
    if (f.moveKey == 0 && !f.clickToMove) ui::Warning("No Move key set: the bot cannot walk.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Hotkeys --------------------------------------------------------
    PluginUi::BeginCard("##hotkeys");
    Heading(ICON_FA_GAMEPAD, "Hotkeys", "Work while the game (or the POEFixer overlay) is in front. The left and right mouse buttons cannot be hotkeys.");
    {
        HotkeySettings& hk = m_settings.global.hotkeys;
        const LabelColumn col({"Start / stop", "Pause (panic)", "Hold to take over", "Stash now"});
        col.RowNoWidth("Start / stop");
        KeyRow("##hkStart", &hk.startStop, "Starts or stops the bot.", true);
        col.RowNoWidth("Pause (panic)");
        KeyRow("##hkPause", &hk.pause, "Pauses at once (all keys released); press again to resume.", true);
        col.RowNoWidth("Hold to take over");
        KeyRow("##hkOverride", &hk.holdOverride,
               "While you hold this key the bot holds still; it resumes shortly after you release it.", true);
        col.RowNoWidth("Stash now");
        KeyRow("##hkStash", &hk.stashNow, "Asks for a stash run now.", true);
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Profiles -------------------------------------------------------
    PluginUi::BeginCard("##profiles");
    Heading(ICON_FA_ID_CARD, "Profiles",
            "Several sets of settings. With auto-select on, the profile bound to the character you play is used;\n"
            "otherwise the selected one.");
    const int editIdx = EditProfileIndex();
    {
        const LabelColumn col({"Profile in use", "For character"});
        col.Row("Profile in use");
        if (ImGui::BeginCombo("##profile", m_settings.profiles[static_cast<size_t>(editIdx)].name.c_str())) {
            for (size_t i = 0; i < m_settings.profiles.size(); ++i) {
                char lbl[128];
                std::snprintf(lbl, sizeof(lbl), "%s###prof%d", m_settings.profiles[i].name.c_str(), static_cast<int>(i));
                if (ImGui::Selectable(lbl, static_cast<int>(i) == editIdx)) {
                    m_settings.activeProfile = m_settings.profiles[i].name;
                    MarkDirty();
                }
            }
            ImGui::EndCombo();
        }
        Tip("The profile the bot uses (and these tabs edit).");
        col.Row("For character");
        if (PluginUi::InputString("##profChar", "character name (optional)", p.character)) MarkDirty();
        Tip("With auto-select on, this profile is used whenever you play this character.");
    }
    if (v && !v->characterName.empty()) {
        if (PluginUi::SubtleButton("Use my current character###useChar")) {
            p.character = v->characterName;
            MarkDirty();
        }
        Tip("Binds this profile to the character you are playing now.");
    }
    if (Check("Auto-select by character", "autoSelect", &m_settings.autoSelectByCharacter,
              "Picks the profile whose character matches the one you play."))
        MarkDirty();
    if (v && v->profileAuto) Hint("Selected automatically for this character.");
    if (ImGui::Button("New###profNew")) {
        const int idx = AddProfile(m_settings, "Profile");
        if (idx >= 0) {
            m_settings.activeProfile = m_settings.profiles[static_cast<size_t>(idx)].name;
            MarkDirty();
        }
    }
    ImGui::SameLine();
    if (PluginUi::SubtleButton("Duplicate###profDup")) {
        const int idx = DuplicateProfile(m_settings, editIdx);
        if (idx >= 0) {
            m_settings.activeProfile = m_settings.profiles[static_cast<size_t>(idx)].name;
            MarkDirty();
        }
    }
    ImGui::SameLine();
    if (PluginUi::SubtleButton("Rename###profRename")) {
        m_renameBuf = m_settings.profiles[static_cast<size_t>(editIdx)].name;
        ImGui::OpenPopup("##renameProfile");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_settings.profiles.size() <= 1);
    if (PluginUi::SubtleButton("Delete###profDel")) ImGui::OpenPopup("##deleteProfile");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##renameProfile")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        PluginUi::InputString("##renameInput", "new name", m_renameBuf);
        if (ImGui::Button("Rename###doRename")) {
            if (RenameProfile(m_settings, editIdx, m_renameBuf)) MarkDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PluginUi::SubtleButton("Cancel###cancelRename")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (PluginUi::ConfirmPopup("##deleteProfile", "Delete this profile? Its settings are lost.", "Delete")) {
        if (DeleteProfile(m_settings, editIdx)) MarkDirty();
    }
    PluginUi::EndCard();
    ImGui::Spacing();
    Hint("Follow Bot sends real keyboard and mouse input to the game. It only acts while the game window itself is in "
         "front, the POEFixer menu is closed and no panel it did not open is visible; your own mouse or keyboard input "
         "pauses it for a moment. Use it within the game's rules.");
}
