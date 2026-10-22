// UiStash.cpp - the Stash tab: target stash, when to stash (town / hideout and
// the leave-map portal trip), tab routing, what stays in the backpack
// (protected cells, keep rules), timing, and the Diagnostics card.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../common/Strings.h"
#include "../stash/StashDiag.h"
#include "Tabs.h"

#include <algorithm>
#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

namespace {

// Render-thread state of this tab (one plugin instance per DLL).
struct StashUiState {
    StashDiagData diag;                 // refreshed only when the worker published something new
    int           paint = -1;           // protected-cells drag: -1 none, 0 clear, 1 protect
    std::string   keepBuf;              // the keep-names field while it is edited
    std::vector<std::string> keepApplied;
    const void*   keepProfile = nullptr;
};

StashUiState& UiState() {
    static StashUiState s;
    return s;
}

const char* const* CategoryNames() {
    static const char* names[kItemCategoryCount] = {};
    if (!names[0])
        for (int i = 0; i < kItemCategoryCount; ++i) names[i] = ItemCategoryName(static_cast<ItemCategory>(i));
    return names;
}

const char* YesNo365(bool v) { return v ? "yes" : "no (needs POEFixer 365+)"; }

ImU32 Col(const ImVec4& c, float alphaMul = 1.0f) {
    ImVec4 v = c;
    v.w *= alphaMul;
    return ImGui::GetColorU32(v);   // style alpha applies: dimmed inside BeginDisabled
}

// The 12 x 5 backpack: click or drag to protect / free cells; a dot marks a cell
// that holds an item right now (latest listing).
bool ProtectedCellsGrid(StashSettings& st, const StashDiagData& diag, StashUiState& ui) {
    const PluginUi::Palette pal = PluginUi::Colors();
    const float em = ImGui::GetFontSize();
    const float gap = (std::max)(2.0f, std::floor(em * 0.15f));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cell = std::floor(std::clamp((avail - gap * (kBackpackCols - 1)) / kBackpackCols, em * 1.1f, em * 2.0f));
    const ImVec2 size(cell * kBackpackCols + gap * (kBackpackCols - 1), cell * kBackpackRows + gap * (kBackpackRows - 1));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##protGrid", size);
    Tip("Click or drag: items touching a protected cell are never stashed.\n"
        "A dot marks a cell that holds an item right now.");
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    auto cellAt = [&](const ImVec2& m, int& cx, int& cy) {
        const float fx = (m.x - p0.x) / (cell + gap), fy = (m.y - p0.y) / (cell + gap);
        cx = static_cast<int>(std::floor(fx));
        cy = static_cast<int>(std::floor(fy));
        return cx >= 0 && cy >= 0 && cx < kBackpackCols && cy < kBackpackRows;
    };
    bool changed = false;
    int hx = -1, hy = -1;
    const bool overCell = (hovered || active) && cellAt(ImGui::GetIO().MousePos, hx, hy);
    if (ImGui::IsItemActivated() && overCell) ui.paint = st.IsProtected(hx, hy) ? 0 : 1;
    if (active && ui.paint >= 0 && overCell && st.IsProtected(hx, hy) != (ui.paint == 1)) {
        st.SetProtected(hx, hy, ui.paint == 1);
        changed = true;
    }
    if (!active) ui.paint = -1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float round = (std::min)(ImGui::GetStyle().FrameRounding, cell * 0.25f);
    for (int y = 0; y < kBackpackRows; ++y) {
        for (int x = 0; x < kBackpackCols; ++x) {
            const ImVec2 a(p0.x + x * (cell + gap), p0.y + y * (cell + gap));
            const ImVec2 b(a.x + cell, a.y + cell);
            const bool prot = st.IsProtected(x, y);
            dl->AddRectFilled(a, b, prot ? Col(pal.Accent, 0.75f) : Col(pal.SurfaceVariant), round);
            dl->AddRect(a, b, Col(x == hx && y == hy ? pal.Text : pal.BorderSubtle, x == hx && y == hy ? 0.6f : 1.0f), round);
            if (diag.backpackValid && diag.occupied[static_cast<size_t>(y * kBackpackCols + x)])
                dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), (std::max)(2.0f, cell * 0.14f),
                                    prot ? Col(pal.Surface) : Col(pal.TextSecondary));
        }
    }
    return changed;
}

}  // namespace

void FollowBotPlugin::DrawTabStash() {
    Profile& prof = EditProfile();
    StashSettings& st = prof.stash;
    StashUiState& ui = UiState();
    GlobalStashDiag().CopyIfNewer(ui.diag);
    const StashDiagData& dg = ui.diag;
    const BotView* v = m_uiView.get();
    const PluginUi::Palette pal = PluginUi::Colors();
    bool changed = false;
    char buf[256];

    // --- Stash ----------------------------------------------------------------
    PluginUi::BeginCard("##stash");
    Heading(ICON_FA_BOX_ARCHIVE, "Stash",
            "Empties the backpack into your own or the guild stash: walks to the stash, opens it, switches to the\n"
            "right tab and Ctrl+clicks every item that is not protected or kept. An item counts as stashed only\n"
            "once it is gone from the backpack. The panel is closed again afterwards.");
    changed |= Check("Stash items", "stashOn", &st.enabled,
                     "Off: the bot never opens a stash (neither automatically nor with \"Stash now\").");
    ImGui::BeginDisabled(!st.enabled);
    {
        const LabelColumn col({"Stash"});
        col.Row("Stash");
        int target = static_cast<int>(st.target);
        if (PluginUi::Combo("##target", target, {"Own stash", "Guild stash"})) {
            st.target = static_cast<StashTarget>(target);
            changed = true;
        }
        Tip("Own stash: your personal stash.\nGuild stash: deposits are slower (each one waits for the server);\n"
            "use a slower click gap for it (Timing).");
    }
    ImGui::Spacing();
    const bool running = m_running.load();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stash now###stashNow")) RequestStashNow();
    ImGui::EndDisabled();
    Tip(running ? "Starts a stash run now: in a town or hideout directly, in a map through a portal\n(needs the portal key)."
                : "Works while the bot runs (Start it first). The \"Stash now\" hotkey is on the General tab.");
    ImGui::SameLine();
    if (!dg.trigger.empty()) {
        std::snprintf(buf, sizeof(buf), "%s: %s", dg.trigger.c_str(), dg.phase.empty() ? "starting" : dg.phase.c_str());
        PluginUi::StatusPill(buf, ui::U32(pal.Success));
    } else {
        PluginUi::StatusPill(running ? "Idle" : "Bot stopped", ui::U32(pal.TextSecondary));
    }
    if (v && !v->hostHasStashObjects)
        ui::HostNote("Stashes the game hides from the entity list are found with POEFixer 365 or newer; on this version the "
                     "bot uses only the stashes it can see.");
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    ImGui::BeginDisabled(!st.enabled);

    // --- When ---------------------------------------------------------------------
    PluginUi::BeginCard("##stashWhen");
    Heading(ICON_FA_CLOCK, "When to stash",
            "In a town or hideout the bot stashes on its own once the backpack is full enough and the leader stands\n"
            "still (or is not around). In a map it can open a portal, stash in the hideout and come back through\n"
            "the same portal (off by default). A run that failed waits before the next automatic one; after three\n"
            "failures in one area it stops trying there until the area changes.");
    changed |= Check("In town and hideout when the backpack fills", "opportunistic", &st.opportunistic,
                     "Needs a stash of the chosen kind in the area.");
    {
        ImGui::BeginDisabled(!st.opportunistic);
        const LabelColumn col({"Stash at", "Leader stands still", "Trip at", "No enemy within", "Portal key",
                               "Wait for the portal", "Look for it within", "After a failure wait"});
        changed |= ui::IntRow(col, "Stash at", "##stashAt", &st.stashWhenUsedCells, 1, kBackpackCells, "%d of 60 cells",
                              "Backpack cells in use (or a loot item that found no room).");
        changed |= ui::IntRow(col, "Leader stands still", "##leaderIdle", &st.leaderIdleMs, 0, 30000, "%d ms",
                              "The leader has not moved for this long (or is not in sight) before the bot leaves him.");
        ImGui::EndDisabled();
        ImGui::Spacing();
        changed |= Check("Leave the map through a portal to stash", "leaveTrip", &st.leaveMapTrip,
                         "In a map: opens a portal with the portal key, stashes in the hideout / town and returns\n"
                         "through the portal nearest to where it arrived. Off by default.");
        ImGui::BeginDisabled(!st.leaveMapTrip);
        changed |= ui::IntRow(col, "Trip at", "##tripAt", &st.tripWhenUsedCells, 1, kBackpackCells, "%d of 60 cells",
                              "Backpack cells in use (or a loot item that found no room).");
        changed |= ui::FloatRow(col, "No enemy within", "##tripEnemy", &st.tripNoEnemyRadius, 0.f, 120.f, "%.0f cells",
                                "Leaves only with no enemy this close; enemies arriving before the portal is clicked\n"
                                "call the trip off.");
        col.RowNoWidth("Portal key");
        KeyRow("##stashPortalKey", &st.portalKey,
               "The key that opens a portal (bind the portal skill to it in the game). The same key as on the General tab.",
               false);
        changed |= ui::IntRow(col, "Wait for the portal", "##portalWait", &st.portalWaitMs, 1000, 10000, "%d ms",
                              "After the key press; without a portal the key is pressed once more, then the trip fails.");
        changed |= ui::FloatRow(col, "Look for it within", "##portalSearch", &st.portalSearchCells, 5.f, 40.f, "%.0f cells",
                                "Only a portal that appeared this close after the key press is taken.");
        ImGui::EndDisabled();
        int coolS = st.failCooldownMs / 1000;
        if (ui::IntRow(col, "After a failure wait", "##failCool", &coolS, 0, 600, "%d s",
                       "Before the next automatic run (Stash now always works).")) {
            st.failCooldownMs = coolS * 1000;
            changed = true;
        }
    }
    if (st.leaveMapTrip && st.portalKey == 0) ui::Warning("Set the portal key: without it the bot cannot leave the map to stash.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Tabs ---------------------------------------------------------------------
    PluginUi::BeginCard("##stashTabs");
    Heading(ICON_FA_TABLE_LIST, "Tabs",
            "Items of a category go to the tab with that name (your own tab names; case does not matter). Everything\n"
            "else goes to the default tab, or into whatever tab is open when the default is empty. Tabs whose list is\n"
            "collapsed and cannot be opened, or names that do not exist, use the open tab instead (or keep the\n"
            "items, see below).");
    int removeAt = -1;
    if (!st.routes.empty() &&
        ImGui::BeginTable("##routes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        const float delW = PluginUi::ButtonWidth(ICON_FA_TRASH_CAN);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Tab name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, delW);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < st.routes.size(); ++i) {
            StashRoute& r = st.routes[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            int c = static_cast<int>(r.category);
            if (PluginUi::Combo("##cat", c, CategoryNames(), kItemCategoryCount)) {
                r.category = static_cast<ItemCategory>(c);
                changed = true;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (PluginUi::InputString("##tab", "tab name", r.tab)) {
                if (r.tab.size() > 64) r.tab.resize(64);
                changed = true;
            }
            ImGui::TableNextColumn();
            if (PluginUi::SubtleButton(ICON_FA_TRASH_CAN)) removeAt = static_cast<int>(i);
            Tip("Remove this route");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeAt >= 0) {
        st.routes.erase(st.routes.begin() + removeAt);
        changed = true;
    }
    ImGui::BeginDisabled(st.routes.size() >= 64);
    if (PluginUi::SubtleButton(ICON_FA_PLUS "  Add route###addRoute")) {
        StashRoute r;
        r.category = ItemCategory::Currency;
        for (int c = 0; c < kItemCategoryCount; ++c) {   // the first category without a route
            const auto cat = static_cast<ItemCategory>(c);
            if (cat == ItemCategory::Quest) continue;
            if (std::none_of(st.routes.begin(), st.routes.end(), [cat](const StashRoute& x) { return x.category == cat; })) {
                r.category = cat;
                break;
            }
        }
        st.routes.push_back(r);
        changed = true;
    }
    ImGui::EndDisabled();
    if (st.routes.empty()) Hint("No routes: everything goes to the default tab.");
    {
        const LabelColumn col({"Default tab"});
        col.Row("Default tab");
        if (PluginUi::InputString("##defaultTab", "the open tab", st.defaultTab)) {
            if (st.defaultTab.size() > 64) st.defaultTab.resize(64);
            changed = true;
        }
        Tip("Items without a route go here. Empty: whatever tab is open when the stash opens.");
    }
    changed |= Check("Use the open tab when a tab cannot be used", "missingTabOpen", &st.missingTabUsesOpenTab,
                     "On: items for a missing or unreachable tab go into the tab that is open (a warning is shown).\n"
                     "Off: those items stay in the backpack.");
    if (v && !v->hostHasStashTabs)
        ui::HostNote("Tab switching needs POEFixer 365 or newer. On this version every item goes into the open tab "
                     "(or stays, with the option above off).");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- What stays -------------------------------------------------------------
    PluginUi::BeginCard("##stashKeep");
    Heading(ICON_FA_LOCK, "What stays in the backpack",
            "Items on a protected cell (any of the cells they cover), items of a kept category and items matching a\n"
            "kept name are never stashed. Quest items always stay.");
    ImGui::TextUnformatted("Protected cells");
    changed |= ProtectedCellsGrid(st, dg, ui);
    if (PluginUi::SubtleButton("Clear###protClear")) {
        st.protectedCells = 0;
        changed = true;
    }
    Tip("Unprotect every cell.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dg.backpackValid);
    if (PluginUi::SubtleButton("Protect what is there now###protNow")) {
        for (int i = 0; i < kBackpackCells; ++i)
            if (dg.occupied[static_cast<size_t>(i)]) st.SetProtected(i % kBackpackCols, i / kBackpackCols, true);
        changed = true;
    }
    ImGui::EndDisabled();
    Tip("Protects every cell that holds an item right now (your build's flasks, scrolls, a portal stack ...).");
    ImGui::Spacing();
    ImGui::TextUnformatted("Kept categories");
    // As many columns as the longest category name allows (1..3).
    float longest = 0.f;
    for (int i = 0; i < kItemCategoryCount; ++i)
        longest = (std::max)(longest, ImGui::CalcTextSize(ItemCategoryName(static_cast<ItemCategory>(i))).x);
    const float colW = longest + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                       ImGui::GetStyle().ItemSpacing.x * 3.0f;
    const int keepCols = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / (std::max)(1.0f, colW)), 1, 3);
    if (ImGui::BeginTable("##keepCats", keepCols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
        for (int i = 0; i < kItemCategoryCount; ++i) {
            const auto cat = static_cast<ItemCategory>(i);
            ImGui::TableNextColumn();
            char id[32];
            std::snprintf(id, sizeof(id), "keep%d", i);
            if (cat == ItemCategory::Quest) {
                bool on = true;
                ImGui::BeginDisabled(true);
                Check(ItemCategoryName(cat), id, &on, "Quest items always stay (the stash refuses them).");
                ImGui::EndDisabled();
                continue;
            }
            bool on = st.Keeps(cat);
            if (Check(ItemCategoryName(cat), id, &on, nullptr)) {
                st.SetKeep(cat, on);
                changed = true;
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Kept names");
    PluginUi::HelpMarker("One per line (or comma-separated), any case. An item stays when its base name, unique name\n"
                         "or metadata path contains one of them, e.g. \"Wisdom\", \"Headhunter\", \"Waystone\".");
    {
        const bool external = ui.keepProfile != static_cast<const void*>(&prof) || ui.keepApplied != st.keepNames;
        if (external && !ImGui::IsAnyItemActive()) {
            ui.keepBuf = JoinLines(st.keepNames);
            ui.keepApplied = st.keepNames;
            ui.keepProfile = &prof;
        }
        const float h = ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
        if (PluginUi::InputStringMultiline("##keepNames", ui.keepBuf, ImVec2(-1.0f, h))) {
            std::vector<std::string> names = SplitList(ui.keepBuf);
            if (names.size() > 512) names.resize(512);
            for (auto& n : names)
                if (n.size() > 256) n.resize(256);
            st.keepNames = names;
            ui.keepApplied = std::move(names);
            ui.keepProfile = &prof;
            changed = true;
        }
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Timing ---------------------------------------------------------------------
    PluginUi::BeginCard("##stashTiming");
    Heading(ICON_FA_STOPWATCH, "Timing",
            "How fast the bot clicks and how long it waits. An item counts as stashed only once it is gone from the\n"
            "backpack; until then it is not clicked again. An item still there after the wait is clicked again, and\n"
            "skipped after the set number of waits; several skips in a row mean the tab is full.");
    {
        const LabelColumn col({"Own stash click gap", "Guild stash click gap", "Wait for an item to move", "Waits per item",
                               "Skips in a row = full", "Wait for the panel", "Clicks on the stash", "Stand this far away",
                               "After a tab click wait", "Hover the stash"});
        changed |= ui::IntRow(col, "Own stash click gap", "##ownGap", &st.ownClickDelayMs, 30, 1000, "%d ms",
                              "Between two Ctrl+clicks into your own stash (the input timing's minimum gap still applies).");
        changed |= ui::IntRow(col, "Guild stash click gap", "##guildGap", &st.guildClickDelayMs, 100, 2000, "%d ms",
                              "Between two Ctrl+clicks into the guild stash (slower: each deposit waits for the server).");
        changed |= ui::IntRow(col, "Wait for an item to move", "##confirmWin", &st.confirmWindowMs, 300, 5000, "%d ms",
                              "How long a clicked item may take to leave the backpack before it is clicked again.");
        changed |= ui::IntRow(col, "Waits per item", "##maxWin", &st.maxConfirmWindows, 1, 10, "%d",
                              "After this many waits with the item still there it is skipped.");
        changed |= ui::IntRow(col, "Skips in a row = full", "##tabFull", &st.tabFullSkips, 1, 10, "%d",
                              "This many skipped items in a row: the tab is treated as full and the rest of its items stay.");
        changed |= ui::IntRow(col, "Wait for the panel", "##openWait", &st.openTimeoutMs, 1000, 10000, "%d ms",
                              "After clicking the stash; then it is clicked again.");
        changed |= ui::IntRow(col, "Clicks on the stash", "##openTries", &st.openRetries, 1, 10, "%d",
                              "Tries to reach and open the stash before the run fails.");
        changed |= ui::FloatRow(col, "Stand this far away", "##ring", &st.approachRingCells, 3.f, 20.f, "%.0f cells",
                                "The stash blocks the way: the bot stops this far from it and clicks.");
        changed |= ui::IntRow(col, "After a tab click wait", "##tabWait", &st.tabSwitchWaitMs, 100, 2000, "%d ms",
                              "Before checking that the tab switched.");
        changed |= ui::IntRow(col, "Hover the stash", "##dwell", &st.clickDwellMs, 40, 600, "%d ms",
                              "The cursor rests on the stash this long before the click (the game must notice the hover).");
    }
    PluginUi::EndCard();
    ImGui::EndDisabled();   // !st.enabled
    ImGui::Spacing();

    // --- Diagnostics ----------------------------------------------------------------
    if (PluginUi::BeginDiagnostics("What the stash logic sees: host support, stash objects, the last tab list, the plan\n"
                                   "and the last run.",
                                   "stashDiag")) {
        std::snprintf(buf, sizeof(buf), "Host support - UI names: %s, stash tabs: %s, stash objects: %s",
                      YesNo365(v && v->hostHasControlNames), YesNo365(v && v->hostHasStashTabs),
                      YesNo365(v && v->hostHasStashObjects));
        Hint(buf);
        const char* objStatus = "";
        switch (dg.objectsStatus) {
            case kStashObjUnsupported:  objStatus = "not available on this POEFixer version"; break;
            case kStashObjNotRequested: objStatus = "not requested (only while the bot runs in a town / hideout)"; break;
            case kStashObjNotReady:     objStatus = "requested, not built yet"; break;
            default:                    break;
        }
        if (dg.objectsStatus >= 0)
            std::snprintf(buf, sizeof(buf), "Host stash list: %d object(s); wider entity scan %s", dg.objectsStatus,
                          dg.scanFlags ? "on" : "off");
        else
            std::snprintf(buf, sizeof(buf), "Host stash list: %s; wider entity scan %s", objStatus, dg.scanFlags ? "on" : "off");
        Hint(buf);

        // Stash objects the bot knows in this area.
        int known = 0;
        if (v)
            for (const auto& it : v->interactables)
                if (it.kind == InteractKind::Stash || it.kind == InteractKind::GuildStash) ++known;
        if (known > 0 &&
            ImGui::BeginTable("##stashObjs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Stash", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Cell", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableHeadersRow();
            for (const auto& it : v->interactables) {
                if (it.kind != InteractKind::Stash && it.kind != InteractKind::GuildStash) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s%s", InteractKindName(it.kind), it.visibleNow ? "" : " (remembered)");
                ImGui::TableNextColumn();
                ImGui::Text("%u", it.id);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f, %.0f", it.grid.x, it.grid.y);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f cells", Dist(it.grid, v->selfGrid));
            }
            ImGui::EndTable();
        } else {
            Hint("No stash object known in this area.");
        }
        for (const auto& o : dg.objects) {
            std::snprintf(buf, sizeof(buf), "Host: %s %u, rank %d, cell %.0f, %.0f - %s", o.guild ? "guild stash" : "stash", o.id,
                          o.rank, o.grid.x, o.grid.y, o.path.c_str());
            Hint(buf);
        }

        // Tabs.
        ImGui::Spacing();
        if (dg.tabsStatus == -3) {
            Hint("Tabs: not read yet (read while a stash run switches tabs).");
        } else {
            const char* st2 = dg.tabsStatus == -2 ? "not supported by this POEFixer version"
                              : dg.tabsStatus == -1 ? "the panel was not open" : "";
            if (dg.tabsStatus >= 0)
                std::snprintf(buf, sizeof(buf), "Tabs of the %s, %.1f s ago: %d; list toggle %s", dg.tabsGuild ? "guild stash" : "stash",
                              v ? static_cast<double>(v->nowMs - (std::min)(v->nowMs, dg.tabsMs)) / 1000.0 : 0.0,
                              dg.tabsStatus, dg.toggleOnScreen ? "visible" : "not available");
            else
                std::snprintf(buf, sizeof(buf), "Tabs: %s", st2);
            Hint(buf);
            if (!dg.tabs.empty() &&
                ImGui::BeginTable("##stashTabsDiag", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Tab", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Selected", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn("On screen", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableHeadersRow();
                for (const auto& t : dg.tabs) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d  %s", t.index, t.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(t.active == 1 ? "yes" : t.active == 0 ? "no" : "?");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(t.onScreen ? "yes" : "no");
                }
                ImGui::EndTable();
            }
        }

        // Plan.
        ImGui::Spacing();
        if (dg.planMs == 0) {
            Hint("Plan: none yet (made when a run could start, and when the stash opens).");
        } else {
            int total = 0;
            for (const auto& g : dg.plan) total += g.items;
            std::snprintf(buf, sizeof(buf), "Plan: %d item(s) to stash, %d kept, %d on protected cells", total, dg.planKept,
                          dg.planProtected);
            Hint(buf);
            for (const auto& g : dg.plan) {
                std::snprintf(buf, sizeof(buf), "    %s: %d", g.tab.empty() ? "the open tab" : g.tab.c_str(), g.items);
                Hint(buf);
            }
        }

        // The run.
        ImGui::Spacing();
        if (!dg.trigger.empty()) {
            std::snprintf(buf, sizeof(buf), "Run: %s - %s; %d moved, %d skipped", dg.trigger.c_str(), dg.phase.c_str(), dg.moved,
                          dg.skipped);
            Hint(buf);
        } else if (running && !dg.whyNot.empty()) {
            std::snprintf(buf, sizeof(buf), "No automatic run: %s", dg.whyNot.c_str());
            Hint(buf);
        }
        if (!dg.lastResult.empty()) {
            std::snprintf(buf, sizeof(buf), "Last run: %s", dg.lastResult.c_str());
            Hint(buf);
        }
        if (!dg.lastFailure.empty()) {
            const double ago = v && dg.lastFailureMs && v->nowMs >= dg.lastFailureMs
                                   ? static_cast<double>(v->nowMs - dg.lastFailureMs) / 1000.0
                                   : 0.0;
            std::snprintf(buf, sizeof(buf), "Last failure (%.0f s ago): %s", ago, dg.lastFailure.c_str());
            ui::ColoredWrapped(pal.Error, buf);
        }
        for (const auto& w : dg.warnings) ui::Warning(w.c_str());
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
