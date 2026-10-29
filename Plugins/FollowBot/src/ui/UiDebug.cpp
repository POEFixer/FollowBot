// UiDebug.cpp - the Debug tab: overlay toggles, status window, the decision
// log, the self-service RE tools and live skills / buffs.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../common/Clock.h"
#include "../common/Strings.h"
#include "Tabs.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

namespace {

// The status window's corners (DebugSettings::statusAnchor order).
constexpr const char* kAnchorKeys[] = {I18n::Key("debug_anchor_top_left"),     I18n::Key("debug_anchor_top_centre"),
                                       I18n::Key("debug_anchor_top_right"),    I18n::Key("debug_anchor_bottom_left"),
                                       I18n::Key("debug_anchor_bottom_centre"), I18n::Key("debug_anchor_bottom_right")};

// "Find UI by control name" match kinds (FollowBotPlugin::m_findUiMatch order).
constexpr const char* kFindMatchKeys[] = {I18n::Key("debug_find_exact"), I18n::Key("debug_find_prefix"),
                                          I18n::Key("debug_find_text")};

// The debug walks' targets.
constexpr InteractKind kWalks[] = {InteractKind::Stash,          InteractKind::GuildStash, InteractKind::Portal,
                                   InteractKind::AreaTransition, InteractKind::Checkpoint, InteractKind::Waypoint};

// A log line as shown: "<seconds> s <category>: <text>".
std::string LogLineText(double agoSeconds, const char* category, const std::string& text) {
    return I18n::TrF("debug_log_line", {{"seconds", Num("%6.1f", agoSeconds)}, {"category", category}, {"text", text}});
}

}  // namespace

void FollowBotPlugin::DrawTabDebug() {
    DebugSettings& d = m_settings.global.debug;
    const BotView* v = m_uiView.get();
    bool changed = false;

    // --- Overlay -----------------------------------------------------------
    PluginUi::BeginCard("##overlay");
    Heading(ICON_FA_EYE, I18n::Tr("debug_overlay"), I18n::Tr("debug_overlay_help"));
    changed |= Check(I18n::Tr("debug_show_overlay"), "overlay", &d.overlay, I18n::Tr("debug_show_overlay_tip"));
    ImGui::BeginDisabled(!d.overlay);
    if (PluginUi::WideEnoughForTwoColumns() && ImGui::BeginTable("##ovcols", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        changed |= Check(I18n::Tr("debug_ov_path"), "ovPath", &d.showPath, I18n::Tr("debug_ov_path_tip"));
        changed |= Check(I18n::Tr("debug_ov_lookahead"), "ovLook", &d.showLookahead, I18n::Tr("debug_ov_lookahead_tip"));
        changed |= Check(I18n::Tr("debug_ov_cursor"), "ovCursor", &d.showCursor, I18n::Tr("debug_ov_cursor_tip"));
        changed |= Check(I18n::Tr("debug_ov_leader_route"), "ovLeader", &d.showLeader, I18n::Tr("debug_ov_leader_tip"));
        changed |= Check(I18n::Tr("debug_ov_circles"), "ovCircles", &d.showCircles, I18n::Tr("debug_ov_circles_tip"));
        ImGui::TableNextColumn();
        changed |= Check(I18n::Tr("debug_ov_route"), "ovRoute", &d.showRoute, I18n::Tr("debug_ov_route_tip"));
        changed |= Check(I18n::Tr("debug_ov_objects"), "ovObjects", &d.showInteractables, I18n::Tr("debug_ov_objects_tip"));
        changed |= Check(I18n::Tr("debug_ov_loot"), "ovLoot", &d.showLoot, I18n::Tr("debug_ov_loot_tip"));
        changed |= Check(I18n::Tr("debug_ov_combat"), "ovCombat", &d.showCombat, I18n::Tr("debug_ov_combat_tip"));
        changed |= Check(I18n::Tr("debug_ov_stuck"), "ovStuck", &d.showStuck, I18n::Tr("debug_ov_stuck_tip"));
        ImGui::EndTable();
    } else {
        changed |= Check(I18n::Tr("debug_ov_path"), "ovPath", &d.showPath, nullptr);
        changed |= Check(I18n::Tr("debug_ov_lookahead"), "ovLook", &d.showLookahead, nullptr);
        changed |= Check(I18n::Tr("debug_ov_cursor"), "ovCursor", &d.showCursor, nullptr);
        changed |= Check(I18n::Tr("debug_ov_leader"), "ovLeader", &d.showLeader, nullptr);
        changed |= Check(I18n::Tr("debug_ov_route"), "ovRoute", &d.showRoute, nullptr);
        changed |= Check(I18n::Tr("debug_ov_circles"), "ovCircles", &d.showCircles, nullptr);
        changed |= Check(I18n::Tr("debug_ov_objects"), "ovObjects", &d.showInteractables, nullptr);
        changed |= Check(I18n::Tr("debug_ov_loot"), "ovLoot", &d.showLoot, nullptr);
        changed |= Check(I18n::Tr("debug_ov_combat"), "ovCombat", &d.showCombat, nullptr);
        changed |= Check(I18n::Tr("debug_ov_stuck"), "ovStuck", &d.showStuck, nullptr);
    }
    ImGui::EndDisabled();
    changed |= Check(I18n::Tr("debug_status_window"), "statusWin", &d.statusWindow, I18n::Tr("debug_status_window_tip"));
    ImGui::BeginDisabled(!d.statusWindow);
    {
        const LabelColumn col({I18n::Tr("debug_status_position"), I18n::Tr("debug_status_size")});
        col.Row(I18n::Tr("debug_status_position"));
        int anchor = d.statusAnchor;
        const std::vector<const char*> anchors = ui::TrAll(kAnchorKeys);
        if (PluginUi::Combo("##anchor", anchor, anchors.data(), static_cast<int>(anchors.size()))) {
            d.statusAnchor = anchor;
            changed = true;
        }
        changed |= ui::FloatRow(col, I18n::Tr("debug_status_size"), "##statusScale", &d.statusScale, 0.7f, 1.6f,
                                I18n::Key("unit_times"), "%.1f", I18n::Tr("debug_status_size_tip"));
    }
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Tools ---------------------------------------------------------------
    PluginUi::BeginCard("##tools");
    Heading(ICON_FA_TOOLBOX, I18n::Tr("debug_tools"), I18n::Tr("debug_tools_help"));
    const bool uiNames = ctx()->Ui.HasControlNames();
    const bool uiSearch = ctx()->Ui.HasFindElements();
    if (ImGui::Button(I18n::TrId("debug_dump_entities"))) RequestJob(Job::DumpEntities);
    Tip(I18n::Tr("debug_dump_entities_tip"));
    ImGui::SameLine();
    if (ImGui::Button(I18n::TrId("debug_dump_ui"))) RequestJob(Job::DumpUi);
    Tip(uiNames ? I18n::Tr("debug_dump_ui_tip") : I18n::Tr("debug_dump_ui_tip_old"));
    ImGui::SameLine();
    if (PluginUi::SubtleButton(I18n::TrId("debug_probe_death"))) RequestJob(Job::ProbeDeath);
    Tip(I18n::Tr("debug_probe_death_tip"));
    ImGui::Spacing();

    // Find UI elements by their locale-independent control name.
    ImGui::TextUnformatted(I18n::Tr("debug_find_ui"));
    ImGui::SameLine();
    ImGui::TextDisabled("?");
    Tip(I18n::Tr("debug_find_ui_tip"));
    ImGui::BeginDisabled(!uiSearch);
    {
        const float em = ImGui::GetFontSize();
        const ImGuiStyle& st = ImGui::GetStyle();
        const char* findLabel = I18n::TrId("debug_find");
        const float findW = PluginUi::ButtonWidth(findLabel);
        // The match combo fits its widest translated item, plus the arrow button.
        const std::vector<const char*> matches = ui::TrAll(kFindMatchKeys);
        float widest = 0.0f;
        for (const char* m : matches) widest = (std::max)(widest, ImGui::CalcTextSize(m).x);
        const float comboW = widest + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float inputW = (std::max)(em * 8.0f, avail - comboW - findW - st.ItemSpacing.x * 2.0f);
        ImGui::SetNextItemWidth(inputW);
        const bool enter = PluginUi::InputString("##findUiText", I18n::Tr("debug_find_hint"), m_findUiText,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        PluginUi::Combo("##findUiMatch", m_findUiMatch, matches.data(), static_cast<int>(matches.size()));
        Tip(I18n::Tr("debug_find_match_tip"));
        ImGui::SameLine();
        const bool find = ImGui::Button(findLabel);
        Check(I18n::Tr("debug_find_visible"), "findUiVisible", &m_findUiVisibleOnly, I18n::Tr("debug_find_visible_tip"));
        if ((find || enter) && uiSearch) {
            {
                std::lock_guard<std::mutex> lock(m_findUiMu);
                m_findUiQuery = m_findUiText;
                m_findUiQueryMatch = m_findUiMatch;
                m_findUiQueryVisibleOnly = m_findUiVisibleOnly;
            }
            RequestJob(Job::FindUi);
        }
    }
    ImGui::EndDisabled();
    if (!uiSearch) ui::HostNote(I18n::Tr("debug_find_needs_365"));
    ImGui::Spacing();
    ImGui::TextUnformatted(I18n::Tr("debug_walk_to"));
    for (size_t i = 0; i < sizeof(kWalks) / sizeof(kWalks[0]); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        if (PluginUi::SubtleButton(I18n::TrId(InteractKindName(kWalks[i])))) RequestDebugWalk(kWalks[i], m_debugWalkClick);
    }
    Check(I18n::Tr("debug_walk_click"), "walkClick", &m_debugWalkClick, I18n::Tr("debug_walk_click_tip"));
    if (m_debugActive.load()) {
        ImGui::SameLine();
        if (ImGui::Button(I18n::TrId("debug_walk_stop"))) m_debugCancel = true;
    }
    if (v && !v->debugResult.empty()) {
        std::string result;
        for (const LocText& line : v->debugResult) {
            if (!result.empty()) result += '\n';
            result += ui::Text(line);
        }
        Hint(result.c_str());
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Decision log ----------------------------------------------------------
    PluginUi::BeginCard("##log");
    Heading(ICON_FA_SCROLL, I18n::Tr("debug_log"), I18n::Tr("debug_log_help"));
    {
        const LabelColumn col({I18n::Tr("debug_log_filter"), I18n::Tr("debug_log_category")});
        col.Row(I18n::Tr("debug_log_filter"));
        PluginUi::InputString("##logFilter", I18n::Tr("debug_log_filter_hint"), m_logFilter);
        col.Row(I18n::Tr("debug_log_category"));
        std::vector<const char*> cats;
        cats.push_back(I18n::Tr("debug_log_all"));
        for (int i = 0; i < static_cast<int>(LogCat::Count); ++i) cats.push_back(I18n::Tr(LogCatName(static_cast<LogCat>(i))));
        int sel = m_logCat + 1;
        if (PluginUi::Combo("##logCat", sel, cats.data(), static_cast<int>(cats.size()))) m_logCat = sel - 1;
    }
    const std::vector<LogLine> lines = m_log.Snapshot();
    const uint64_t now = NowMs();
    std::string copy;   // the lines shown, in English (for bug reports, like the log file)
    const float h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
    if (ImGui::BeginChild("##logLines", ImVec2(0.0f, h), ImGuiChildFlags_Borders)) {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (m_logCat >= 0 && static_cast<int>(it->cat) != m_logCat) continue;
            const std::string text = ui::Text(it->text);
            const std::string english = Render(it->text, &EnglishText);
            // The filter finds the line in the program language or in its English words.
            if (!MatchesLogFilter(text, english, m_logFilter)) continue;
            const double ago = static_cast<double>(now >= it->timeMs ? now - it->timeMs : 0) / 1000.0;
            const char* catKey = LogCatName(it->cat);
            ImGui::TextUnformatted(LogLineText(ago, I18n::Tr(catKey), text).c_str());
            const char* catEnglish = EnglishText(catKey);
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), "%7.1fs  %-7s ", ago, catEnglish ? catEnglish : catKey);
            copy += prefix;
            copy += english;
            copy += '\n';
        }
    }
    ImGui::EndChild();
    if (PluginUi::SubtleButton(I18n::TrId("debug_log_copy"))) ImGui::SetClipboardText(copy.c_str());
    Tip(I18n::Tr("debug_log_copy_tip"));
    ImGui::SameLine();
    if (PluginUi::SubtleButton(I18n::TrId("debug_log_clear"))) m_log.Clear();
    ImGui::SameLine();
    changed |= Check(I18n::Tr("debug_log_file"), "logFile", &d.logToFile, I18n::Tr("debug_log_file_tip"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Live skills / buffs --------------------------------------------------
    PluginUi::BeginCard("##live");
    Heading(ICON_FA_WAND_MAGIC_SPARKLES, I18n::Tr("debug_live"), I18n::Tr("debug_live_help"));
    Check(I18n::Tr("debug_live_read"), "liveSkills", &m_showSkillsLive, I18n::Tr("debug_live_read_tip"));
    if (m_showSkillsLive && v) {
        if (ImGui::BeginTable("##skills", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn(I18n::TrId("skills_skill"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn(I18n::TrId("skills_col_bar_slot"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(I18n::TrId("skills_col_ready"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (const auto& s : v->skills) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableNextColumn();
                if (s.onBar) ImGui::Text("%d", s.barSlot + 1);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ui::YesNo(s.canBeUsed));
            }
            ImGui::EndTable();
        }
        std::string buffs;
        for (const auto& b : v->selfBuffs) {
            if (!buffs.empty()) buffs += ", ";
            buffs += b.name;
            if (b.charges > 1) buffs += " x" + std::to_string(b.charges);
        }
        ImGui::Spacing();
        Hint(buffs.empty() ? I18n::Tr("debug_live_buffs_none") : I18n::TrF("debug_live_buffs", {{"buffs", buffs}}).c_str());
        std::string lbuffs;
        for (const auto& b : v->leaderBuffs) {
            if (!lbuffs.empty()) lbuffs += ", ";
            lbuffs += b.name;
        }
        Hint(lbuffs.empty() ? I18n::Tr("debug_live_leader_buffs_none")
                            : I18n::TrF("debug_live_leader_buffs", {{"buffs", lbuffs}}).c_str());
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    if (PluginUi::BeginDiagnostics(I18n::Tr("debug_diag_tip"), "debugDiag")) {
        if (!v) {
            Hint(I18n::Tr("common_no_data_enabled"));
        } else {
            std::string area = I18n::TrF("debug_diag_area", {{"area", v->areaId.empty() ? std::string("-") : v->areaId},
                                                              {"counter", Num("%u", v->areaCounter)}});
            if (v->isTown) area += std::string(" ") + I18n::Tr("debug_area_town");
            if (v->isHideout) area += std::string(" ") + I18n::Tr("debug_area_hideout");
            if (v->isMap) area += std::string(" ") + I18n::Tr("debug_area_map");
            Hint(ui::Join({I18n::TrF("debug_diag_attached", {{"value", ui::YesNo(v->attached)}}),
                           I18n::TrF("debug_diag_in_game", {{"value", ui::YesNo(v->inGame)}}),
                           I18n::TrF("debug_diag_front", {{"value", ui::YesNo(v->foreground)}}), area})
                     .c_str());
            const std::string profile = ui::ProfileName(v->profileName);
            Hint(ui::Join({I18n::TrF("debug_diag_character",
                                     {{"name", v->characterName.empty() ? std::string("-") : v->characterName}}),
                           v->profileAuto ? I18n::TrF("debug_diag_profile_auto", {{"name", profile}})
                                          : I18n::TrF("debug_diag_profile", {{"name", profile}}),
                           I18n::TrF("debug_diag_life", {{"value", Num("%.0f", v->hpPct)}}),
                           I18n::TrF("debug_diag_grid", {{"x", Num("%.0f", v->selfGrid.x)}, {"y", Num("%.0f", v->selfGrid.y)}})})
                     .c_str());
            Hint(ui::Join({I18n::TrF("debug_diag_monsters", {{"count", Num("%d", v->monsters)}}),
                           I18n::TrF("debug_diag_ground", {{"count", Num("%d", v->groundItems)}}),
                           I18n::TrF("debug_diag_objects", {{"count", Num("%d", static_cast<int>(v->interactables.size()))}}),
                           I18n::TrF("debug_diag_backpack", {{"state", v->inventoryValid ? I18n::Tr("debug_backpack_read")
                                                                                         : I18n::Tr("debug_backpack_not_read")},
                                                             {"used", Num("%d", v->usedCells)}})})
                     .c_str());
            // Asked of the host directly (cheap capability checks), not of the world snapshot.
            const auto has = [](bool yes) { return yes ? I18n::Tr("common_yes") : I18n::Tr("debug_no_365"); };
            Hint(I18n::TrF("debug_diag_host", {{"names", has(ctx()->Ui.HasControlNames())},
                                               {"search", has(ctx()->Ui.HasFindElements())},
                                               {"tabs", has(ctx()->Game.HasStashTabs())},
                                               {"objects", has(ctx()->Game.HasStashObjects())}})
                     .c_str());
            if (!v->interactables.empty() &&
                ImGui::BeginTable("##objs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn(I18n::TrId("skills_col_kind"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(I18n::TrId("debug_col_distance"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(I18n::TrId("debug_col_portal_type"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(I18n::TrId("debug_col_state"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableHeadersRow();
                int shown = 0;
                for (const auto& it : v->interactables) {
                    if (++shown > 24) break;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(I18n::Tr(InteractKindName(it.kind)));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", Dist(it.grid, v->selfGrid));
                    ImGui::TableNextColumn();
                    if (it.portalType >= 0) ImGui::Text("%d", it.portalType);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(it.visibleNow ? (it.targetable ? I18n::Tr("debug_state_visible")
                                                                          : I18n::Tr("debug_state_inactive"))
                                                         : I18n::Tr("debug_state_remembered"));
                }
                ImGui::EndTable();
            }
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
