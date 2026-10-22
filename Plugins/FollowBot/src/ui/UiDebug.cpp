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

void FollowBotPlugin::DrawTabDebug() {
    DebugSettings& d = m_settings.global.debug;
    const BotView* v = m_uiView.get();
    bool changed = false;

    // --- Overlay -----------------------------------------------------------
    PluginUi::BeginCard("##overlay");
    Heading(ICON_FA_EYE, "World overlay", "Draws what the bot plans on top of the game (only while the game is in front).");
    changed |= Check("Show the debug overlay", "overlay", &d.overlay, "Path, targets, leader markers, objects and stuck marks.");
    ImGui::BeginDisabled(!d.overlay);
    if (PluginUi::WideEnoughForTwoColumns() && ImGui::BeginTable("##ovcols", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        changed |= Check("Path", "ovPath", &d.showPath, "The planned path on the ground.");
        changed |= Check("Look-ahead point", "ovLook", &d.showLookahead, "The point the steering aims at.");
        changed |= Check("Cursor target", "ovCursor", &d.showCursor, "Where the bot puts the cursor.");
        changed |= Check("Leader and his route", "ovLeader", &d.showLeader, "His position, last known position and route.");
        changed |= Check("Follow / stop circles", "ovCircles", &d.showCircles, "Around the leader.");
        ImGui::TableNextColumn();
        changed |= Check("Leader route", "ovRoute", &d.showRoute, "The route the server gave him.");
        changed |= Check("Objects", "ovObjects", &d.showInteractables, "Transitions, portals, waypoints, stashes with their kind.");
        changed |= Check("Loot targets", "ovLoot", &d.showLoot, "Items the bot wants and items it gave up on.");
        changed |= Check("Combat radius", "ovCombat", &d.showCombat, "Combat radius around you and the leash around the leader.");
        changed |= Check("Stuck marks", "ovStuck", &d.showStuck, "Where it got stuck and the spots it avoids.");
        ImGui::EndTable();
    } else {
        changed |= Check("Path", "ovPath", &d.showPath, nullptr);
        changed |= Check("Look-ahead point", "ovLook", &d.showLookahead, nullptr);
        changed |= Check("Cursor target", "ovCursor", &d.showCursor, nullptr);
        changed |= Check("Leader", "ovLeader", &d.showLeader, nullptr);
        changed |= Check("Leader route", "ovRoute", &d.showRoute, nullptr);
        changed |= Check("Follow / stop circles", "ovCircles", &d.showCircles, nullptr);
        changed |= Check("Objects", "ovObjects", &d.showInteractables, nullptr);
        changed |= Check("Loot targets", "ovLoot", &d.showLoot, nullptr);
        changed |= Check("Combat radius", "ovCombat", &d.showCombat, nullptr);
        changed |= Check("Stuck marks", "ovStuck", &d.showStuck, nullptr);
    }
    ImGui::EndDisabled();
    changed |= Check("Status window while running", "statusWin", &d.statusWindow,
                     "A small panel in the game: mode, task, hold reason, leader distance, deaths.");
    ImGui::BeginDisabled(!d.statusWindow);
    {
        const LabelColumn col({"Position", "Size"});
        col.Row("Position");
        int anchor = d.statusAnchor;
        if (PluginUi::Combo("##anchor", anchor, {"Top left", "Top centre", "Top right", "Bottom left", "Bottom centre", "Bottom right"})) {
            d.statusAnchor = anchor;
            changed = true;
        }
        changed |= ui::FloatRow(col, "Size", "##statusScale", &d.statusScale, 0.7f, 1.6f, "%.1f x", "Scale of the status window.");
    }
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Tools ---------------------------------------------------------------
    PluginUi::BeginCard("##tools");
    Heading(ICON_FA_TOOLBOX, "Tools",
            "Dumps write text files to the plugin's dumps folder; they help to check object names and UI\n"
            "elements after a game patch. The walk tests run without a leader and stop at the target.");
    const bool uiNames = ctx()->Ui.HasControlNames();
    const bool uiSearch = ctx()->Ui.HasFindElements();
    if (ImGui::Button("Dump entities near me###dumpEnt")) RequestJob(Job::DumpEntities);
    Tip("Every entity within 60 cells: path, type, components, state values, flags.");
    ImGui::SameLine();
    if (ImGui::Button("Dump UI tree###dumpUi")) RequestJob(Job::DumpUi);
    Tip(uiNames ? "Every visible UI element with its control name, text and screen rectangle.\n"
                  "Control names are the same in every game language."
                : "Every visible UI element with its text and screen rectangle.\n"
                  "Control names need POEFixer 365 or newer.");
    ImGui::SameLine();
    if (PluginUi::SubtleButton("Test death screen lookup###probeDeath")) RequestJob(Job::ProbeDeath);
    Tip("Reports whether the death screen and its respawn buttons can be found right now,\n"
        "which are visible, where they are and which one the bot would click.\n"
        "Most useful while the character is dead.");
    ImGui::Spacing();

    // Find UI elements by their locale-independent control name.
    ImGui::TextUnformatted("Find UI by control name:");
    ImGui::SameLine();
    ImGui::TextDisabled("?");
    Tip("Searches the game UI (up to 32 levels, 4000 elements) and lists the first 12 matches with\n"
        "their control-name path, visibility, screen rectangle and text, here and in the decision log.\n"
        "Example: death_screen, respawn_at_checkpoint_button.");
    ImGui::BeginDisabled(!uiSearch);
    {
        const float em = ImGui::GetFontSize();
        const ImGuiStyle& st = ImGui::GetStyle();
        const float findW = PluginUi::ButtonWidth("Find");
        const float comboW = em * 11.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float inputW = (std::max)(em * 8.0f, avail - comboW - findW - st.ItemSpacing.x * 2.0f);
        ImGui::SetNextItemWidth(inputW);
        const bool enter = PluginUi::InputString("##findUiText", "control name", m_findUiText,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        PluginUi::Combo("##findUiMatch", m_findUiMatch, {"Exact name", "Name starts with", "Localized text"});
        Tip("Exact name: the control name equals the text.\n"
            "Name starts with: the control name begins with the text.\n"
            "Localized text: the text shown in your game language equals it.");
        ImGui::SameLine();
        const bool find = ImGui::Button("Find###findUiGo");
        Check("Visible elements only", "findUiVisible", &m_findUiVisibleOnly,
              "On: the element and every parent must be visible.\nOff: hidden elements are listed too (e.g. a button the game shows later).");
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
    if (!uiSearch) ui::HostNote("Searching the UI by name needs POEFixer 365 or newer.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Walk to the nearest:");
    const struct { const char* label; InteractKind kind; } kWalks[] = {
        {"Stash###wStash", InteractKind::Stash},
        {"Guild stash###wGuild", InteractKind::GuildStash},
        {"Portal###wPortal", InteractKind::Portal},
        {"Transition###wTrans", InteractKind::AreaTransition},
        {"Checkpoint###wCp", InteractKind::Checkpoint},
        {"Waypoint###wWp", InteractKind::Waypoint},
    };
    for (size_t i = 0; i < sizeof(kWalks) / sizeof(kWalks[0]); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        if (PluginUi::SubtleButton(kWalks[i].label)) RequestDebugWalk(kWalks[i].kind, m_debugWalkClick);
    }
    Check("Click it on arrival", "walkClick", &m_debugWalkClick, "Tests the hover and the click too (a portal or transition takes you through).");
    if (m_debugActive.load()) {
        ImGui::SameLine();
        if (ImGui::Button("Stop walk###stopWalk")) m_debugCancel = true;
    }
    if (v && !v->debugResult.empty()) Hint(v->debugResult.c_str());
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Decision log ----------------------------------------------------------
    PluginUi::BeginCard("##log");
    Heading(ICON_FA_SCROLL, "Decision log", "What the bot decided and why (the newest 400 lines).");
    {
        const LabelColumn col({"Filter", "Category"});
        col.Row("Filter");
        PluginUi::InputString("##logFilter", "text to find", m_logFilter);
        col.Row("Category");
        std::vector<const char*> cats;
        cats.push_back("All");
        for (int i = 0; i < static_cast<int>(LogCat::Count); ++i) cats.push_back(LogCatName(static_cast<LogCat>(i)));
        int sel = m_logCat + 1;
        if (PluginUi::Combo("##logCat", sel, cats.data(), static_cast<int>(cats.size()))) m_logCat = sel - 1;
    }
    const std::vector<LogLine> lines = m_log.Snapshot();
    const uint64_t now = NowMs();
    std::string copy;
    const float h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
    if (ImGui::BeginChild("##logLines", ImVec2(0.0f, h), ImGuiChildFlags_Borders)) {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (m_logCat >= 0 && static_cast<int>(it->cat) != m_logCat) continue;
            if (!m_logFilter.empty() && !ContainsCI(it->text, m_logFilter)) continue;
            const double ago = static_cast<double>(now >= it->timeMs ? now - it->timeMs : 0) / 1000.0;
            char buf[1200];
            std::snprintf(buf, sizeof(buf), "%7.1fs  %-7s %s", ago, LogCatName(it->cat), it->text.c_str());
            ImGui::TextUnformatted(buf);
            copy += buf;
            copy += '\n';
        }
    }
    ImGui::EndChild();
    if (PluginUi::SubtleButton("Copy###logCopy")) ImGui::SetClipboardText(copy.c_str());
    Tip("Copies the lines shown (newest first).");
    ImGui::SameLine();
    if (PluginUi::SubtleButton("Clear###logClear")) m_log.Clear();
    ImGui::SameLine();
    changed |= Check("Also write to a file", "logFile", &d.logToFile, "logs/followbot-<date>.log in the plugin folder.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Live skills / buffs --------------------------------------------------
    PluginUi::BeginCard("##live");
    Heading(ICON_FA_WAND_MAGIC_SPARKLES, "Live skills and buffs",
            "The character's skills (name, bar slot, ready) and current buffs: copy names into skill rules.");
    Check("Read them now", "liveSkills", &m_showSkillsLive, "Reads skills and buffs a few times a second while this is on.");
    if (m_showSkillsLive && v) {
        if (ImGui::BeginTable("##skills", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Bar slot", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Ready", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (const auto& s : v->skills) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableNextColumn();
                if (s.onBar) ImGui::Text("%d", s.barSlot + 1);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.canBeUsed ? "yes" : "no");
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
        Hint(buffs.empty() ? "Your buffs: none" : ("Your buffs: " + buffs).c_str());
        std::string lbuffs;
        for (const auto& b : v->leaderBuffs) {
            if (!lbuffs.empty()) lbuffs += ", ";
            lbuffs += b.name;
        }
        Hint(lbuffs.empty() ? "Leader buffs: none (or not visible)" : ("Leader buffs: " + lbuffs).c_str());
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    if (PluginUi::BeginDiagnostics("What the bot reads from the game right now.", "debugDiag")) {
        if (!v) {
            Hint("No data yet (the bot reads the game while the plugin is enabled).");
        } else {
            char line[256];
            std::snprintf(line, sizeof(line), "Attached: %s   |   in game: %s   |   game in front: %s   |   area: %s (#%u)%s%s%s",
                          v->attached ? "yes" : "no", v->inGame ? "yes" : "no", v->foreground ? "yes" : "no",
                          v->areaId.empty() ? "-" : v->areaId.c_str(), v->areaCounter, v->isTown ? " town" : "",
                          v->isHideout ? " hideout" : "", v->isMap ? " map" : "");
            Hint(line);
            std::snprintf(line, sizeof(line), "Character: %s   |   profile: %s%s   |   life %.0f %%   |   grid (%.0f, %.0f)",
                          v->characterName.empty() ? "-" : v->characterName.c_str(), v->profileName.c_str(),
                          v->profileAuto ? " (auto)" : "", v->hpPct, v->selfGrid.x, v->selfGrid.y);
            Hint(line);
            std::snprintf(line, sizeof(line), "Monsters: %d   |   ground items: %d   |   objects: %d   |   backpack: %s, %d / 60 cells",
                          v->monsters, v->groundItems, static_cast<int>(v->interactables.size()),
                          v->inventoryValid ? "read" : "not read", v->usedCells);
            Hint(line);
            // Asked of the host directly (cheap capability checks), not of the world snapshot.
            std::snprintf(line, sizeof(line), "Host features: UI names %s, UI search %s, stash tabs %s, stash objects %s",
                          ctx()->Ui.HasControlNames() ? "yes" : "no (365+)", ctx()->Ui.HasFindElements() ? "yes" : "no (365+)",
                          ctx()->Game.HasStashTabs() ? "yes" : "no (365+)", ctx()->Game.HasStashObjects() ? "yes" : "no (365+)");
            Hint(line);
            if (!v->interactables.empty() &&
                ImGui::BeginTable("##objs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Portal type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableHeadersRow();
                int shown = 0;
                for (const auto& it : v->interactables) {
                    if (++shown > 24) break;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(InteractKindName(it.kind));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", Dist(it.grid, v->selfGrid));
                    ImGui::TableNextColumn();
                    if (it.portalType >= 0) ImGui::Text("%d", it.portalType);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(it.visibleNow ? (it.targetable ? "visible" : "visible, inactive") : "remembered");
                }
                ImGui::EndTable();
            }
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
