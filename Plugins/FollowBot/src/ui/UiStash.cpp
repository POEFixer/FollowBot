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

// The category names in the program language (built every frame: the language may change).
std::vector<const char*> CategoryNames() {
    std::vector<const char*> names;
    names.reserve(kItemCategoryCount);
    for (int i = 0; i < kItemCategoryCount; ++i) names.push_back(I18n::Tr(ItemCategoryName(static_cast<ItemCategory>(i))));
    return names;
}

const char* YesNo365(bool v) { return v ? I18n::Tr("common_yes") : I18n::Tr("stash_no_365"); }

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
    Tip(I18n::Tr("stash_protect_grid_tip"));
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
    const char* cells = I18n::Key("unit_cells");
    const char* ms = I18n::Key("unit_ms");

    // --- Stash ----------------------------------------------------------------
    PluginUi::BeginCard("##stash");
    Heading(ICON_FA_BOX_ARCHIVE, I18n::Tr("stash_title"), I18n::Tr("stash_title_help"));
    changed |= Check(I18n::Tr("stash_on"), "stashOn", &st.enabled, I18n::Tr("stash_on_tip"));
    ImGui::BeginDisabled(!st.enabled);
    {
        const LabelColumn col({I18n::Tr("stash_target")});
        col.Row(I18n::Tr("stash_target"));
        int target = static_cast<int>(st.target);
        if (PluginUi::Combo("##target", target, {I18n::Tr("stash_target_own"), I18n::Tr("stash_target_guild")})) {
            st.target = static_cast<StashTarget>(target);
            changed = true;
        }
        Tip(I18n::Tr("stash_target_tip"));
    }
    ImGui::Spacing();
    const bool running = m_running.load();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button(I18n::TrId("general_hk_stash"))) RequestStashNow();
    ImGui::EndDisabled();
    Tip(running ? I18n::Tr("stash_now_tip") : I18n::Tr("stash_now_stopped_tip"));
    ImGui::SameLine();
    if (dg.trigger && dg.trigger[0]) {
        const std::string pill = I18n::TrF("stash_pill_run", {{"trigger", I18n::Tr(dg.trigger)},
                                                               {"phase", dg.phase && dg.phase[0] ? I18n::Tr(dg.phase)
                                                                                                 : I18n::Tr("stash_starting")}});
        PluginUi::StatusPill(pill.c_str(), ui::U32(pal.Success));
    } else {
        PluginUi::StatusPill(running ? I18n::Tr("stash_idle") : I18n::Tr("stash_bot_stopped"), ui::U32(pal.TextSecondary));
    }
    if (v && !v->hostHasStashObjects) ui::HostNote(I18n::Tr("stash_objects_old_host"));
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    ImGui::BeginDisabled(!st.enabled);

    // --- When ---------------------------------------------------------------------
    PluginUi::BeginCard("##stashWhen");
    Heading(ICON_FA_CLOCK, I18n::Tr("stash_when"), I18n::Tr("stash_when_help"));
    changed |= Check(I18n::Tr("stash_opportunistic"), "opportunistic", &st.opportunistic, I18n::Tr("stash_opportunistic_tip"));
    {
        ImGui::BeginDisabled(!st.opportunistic);
        const LabelColumn col({I18n::Tr("stash_at"), I18n::Tr("stash_leader_idle"), I18n::Tr("stash_trip_at"),
                               I18n::Tr("loot_no_enemy"), I18n::Tr("general_key_portal"), I18n::Tr("stash_portal_wait"),
                               I18n::Tr("stash_portal_search"), I18n::Tr("stash_fail_cooldown")});
        changed |= ui::IntRow(col, I18n::Tr("stash_at"), "##stashAt", &st.stashWhenUsedCells, 1, kBackpackCells,
                              I18n::Key("stash_unit_used_cells"), I18n::Tr("stash_used_cells_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_leader_idle"), "##leaderIdle", &st.leaderIdleMs, 0, 30000, ms,
                              I18n::Tr("stash_leader_idle_tip"));
        ImGui::EndDisabled();
        ImGui::Spacing();
        changed |= Check(I18n::Tr("stash_leave_trip"), "leaveTrip", &st.leaveMapTrip, I18n::Tr("stash_leave_trip_tip"));
        ImGui::BeginDisabled(!st.leaveMapTrip);
        changed |= ui::IntRow(col, I18n::Tr("stash_trip_at"), "##tripAt", &st.tripWhenUsedCells, 1, kBackpackCells,
                              I18n::Key("stash_unit_used_cells"), I18n::Tr("stash_used_cells_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("loot_no_enemy"), "##tripEnemy", &st.tripNoEnemyRadius, 0.f, 120.f, cells, "%.0f",
                                I18n::Tr("stash_trip_no_enemy_tip"));
        col.RowNoWidth(I18n::Tr("general_key_portal"));
        KeyRow("##stashPortalKey", &st.portalKey, I18n::Tr("stash_portal_key_tip"), false);
        changed |= ui::IntRow(col, I18n::Tr("stash_portal_wait"), "##portalWait", &st.portalWaitMs, 1000, 10000, ms,
                              I18n::Tr("stash_portal_wait_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("stash_portal_search"), "##portalSearch", &st.portalSearchCells, 5.f, 40.f, cells,
                                "%.0f", I18n::Tr("stash_portal_search_tip"));
        ImGui::EndDisabled();
        int coolS = st.failCooldownMs / 1000;
        if (ui::IntRow(col, I18n::Tr("stash_fail_cooldown"), "##failCool", &coolS, 0, 600, I18n::Key("unit_s"),
                       I18n::Tr("stash_fail_cooldown_tip"))) {
            st.failCooldownMs = coolS * 1000;
            changed = true;
        }
    }
    if (st.leaveMapTrip && st.portalKey == 0) ui::Warning(I18n::Tr("stash_no_portal_key"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Tabs ---------------------------------------------------------------------
    PluginUi::BeginCard("##stashTabs");
    Heading(ICON_FA_TABLE_LIST, I18n::Tr("stash_tabs"), I18n::Tr("stash_tabs_help"));
    int removeAt = -1;
    if (!st.routes.empty() &&
        ImGui::BeginTable("##routes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        const float delW = PluginUi::ButtonWidth(ICON_FA_TRASH_CAN);
        ImGui::TableSetupColumn(I18n::TrId("loot_category"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(I18n::TrId("stash_col_tab_name"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, delW);
        ImGui::TableHeadersRow();
        const std::vector<const char*> categoryNames = CategoryNames();
        for (size_t i = 0; i < st.routes.size(); ++i) {
            StashRoute& r = st.routes[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            int c = static_cast<int>(r.category);
            if (PluginUi::Combo("##cat", c, categoryNames.data(), kItemCategoryCount)) {
                r.category = static_cast<ItemCategory>(c);
                changed = true;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (PluginUi::InputString("##tab", I18n::Tr("stash_tab_name_hint"), r.tab)) {
                if (r.tab.size() > 64) r.tab.resize(64);
                changed = true;
            }
            ImGui::TableNextColumn();
            if (PluginUi::SubtleButton(ICON_FA_TRASH_CAN)) removeAt = static_cast<int>(i);
            Tip(I18n::Tr("stash_remove_route"));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeAt >= 0) {
        st.routes.erase(st.routes.begin() + removeAt);
        changed = true;
    }
    ImGui::BeginDisabled(st.routes.size() >= 64);
    if (PluginUi::SubtleButton(ui::IconLabel(ICON_FA_PLUS, I18n::Tr("stash_add_route"), "addRoute").c_str())) {
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
    if (st.routes.empty()) Hint(I18n::Tr("stash_no_routes"));
    {
        const LabelColumn col({I18n::Tr("stash_default_tab")});
        col.Row(I18n::Tr("stash_default_tab"));
        if (PluginUi::InputString("##defaultTab", I18n::Tr("stash_open_tab"), st.defaultTab)) {
            if (st.defaultTab.size() > 64) st.defaultTab.resize(64);
            changed = true;
        }
        Tip(I18n::Tr("stash_default_tab_tip"));
    }
    changed |= Check(I18n::Tr("stash_missing_tab_open"), "missingTabOpen", &st.missingTabUsesOpenTab,
                     I18n::Tr("stash_missing_tab_open_tip"));
    if (v && !v->hostHasStashTabs) ui::HostNote(I18n::Tr("stash_tabs_old_host"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- What stays -------------------------------------------------------------
    PluginUi::BeginCard("##stashKeep");
    Heading(ICON_FA_LOCK, I18n::Tr("stash_keep"), I18n::Tr("stash_keep_help"));
    ImGui::TextUnformatted(I18n::Tr("stash_protected_cells"));
    changed |= ProtectedCellsGrid(st, dg, ui);
    if (PluginUi::SubtleButton(I18n::TrId("stash_clear"))) {
        st.protectedCells = 0;
        changed = true;
    }
    Tip(I18n::Tr("stash_clear_tip"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!dg.backpackValid);
    if (PluginUi::SubtleButton(I18n::TrId("stash_protect_now"))) {
        for (int i = 0; i < kBackpackCells; ++i)
            if (dg.occupied[static_cast<size_t>(i)]) st.SetProtected(i % kBackpackCols, i / kBackpackCols, true);
        changed = true;
    }
    ImGui::EndDisabled();
    Tip(I18n::Tr("stash_protect_now_tip"));
    ImGui::Spacing();
    ImGui::TextUnformatted(I18n::Tr("stash_kept_categories"));
    // As many columns as the longest category name allows (1..3).
    float longest = 0.f;
    for (int i = 0; i < kItemCategoryCount; ++i)
        longest = (std::max)(longest, ImGui::CalcTextSize(I18n::Tr(ItemCategoryName(static_cast<ItemCategory>(i)))).x);
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
                Check(I18n::Tr(ItemCategoryName(cat)), id, &on, I18n::Tr("stash_quest_stays"));
                ImGui::EndDisabled();
                continue;
            }
            bool on = st.Keeps(cat);
            if (Check(I18n::Tr(ItemCategoryName(cat)), id, &on, nullptr)) {
                st.SetKeep(cat, on);
                changed = true;
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(I18n::Tr("stash_kept_names"));
    PluginUi::HelpMarker(I18n::Tr("stash_kept_names_help"));
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
    Heading(ICON_FA_STOPWATCH, I18n::Tr("stash_timing"), I18n::Tr("stash_timing_help"));
    {
        const LabelColumn col({I18n::Tr("stash_own_gap"), I18n::Tr("stash_guild_gap"), I18n::Tr("stash_confirm_window"),
                               I18n::Tr("stash_max_windows"), I18n::Tr("stash_tab_full"), I18n::Tr("stash_open_wait"),
                               I18n::Tr("stash_open_tries"), I18n::Tr("stash_ring"), I18n::Tr("stash_tab_wait"),
                               I18n::Tr("stash_dwell")});
        changed |= ui::IntRow(col, I18n::Tr("stash_own_gap"), "##ownGap", &st.ownClickDelayMs, 30, 1000, ms,
                              I18n::Tr("stash_own_gap_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_guild_gap"), "##guildGap", &st.guildClickDelayMs, 100, 2000, ms,
                              I18n::Tr("stash_guild_gap_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_confirm_window"), "##confirmWin", &st.confirmWindowMs, 300, 5000, ms,
                              I18n::Tr("stash_confirm_window_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_max_windows"), "##maxWin", &st.maxConfirmWindows, 1, 10, nullptr,
                              I18n::Tr("stash_max_windows_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_tab_full"), "##tabFull", &st.tabFullSkips, 1, 10, nullptr,
                              I18n::Tr("stash_tab_full_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_open_wait"), "##openWait", &st.openTimeoutMs, 1000, 10000, ms,
                              I18n::Tr("stash_open_wait_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_open_tries"), "##openTries", &st.openRetries, 1, 10, nullptr,
                              I18n::Tr("stash_open_tries_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("stash_ring"), "##ring", &st.approachRingCells, 3.f, 20.f, cells, "%.0f",
                                I18n::Tr("stash_ring_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_tab_wait"), "##tabWait", &st.tabSwitchWaitMs, 100, 2000, ms,
                              I18n::Tr("stash_tab_wait_tip"));
        changed |= ui::IntRow(col, I18n::Tr("stash_dwell"), "##dwell", &st.clickDwellMs, 40, 600, ms,
                              I18n::Tr("stash_dwell_tip"));
    }
    PluginUi::EndCard();
    ImGui::EndDisabled();   // !st.enabled
    ImGui::Spacing();

    // --- Diagnostics ----------------------------------------------------------------
    if (PluginUi::BeginDiagnostics(I18n::Tr("stash_diag_tip"), "stashDiag")) {
        Hint(I18n::TrF("stash_diag_host", {{"names", YesNo365(v && v->hostHasControlNames)},
                                           {"tabs", YesNo365(v && v->hostHasStashTabs)},
                                           {"objects", YesNo365(v && v->hostHasStashObjects)}})
                 .c_str());
        const char* scan = dg.scanFlags ? I18n::Tr("stash_diag_on") : I18n::Tr("stash_diag_off");
        if (dg.objectsStatus >= 0) {
            Hint(I18n::TrF("stash_diag_objects_count", {{"count", Num("%d", dg.objectsStatus)}, {"scan", scan}}).c_str());
        } else {
            const char* objStatus = dg.objectsStatus == kStashObjUnsupported    ? I18n::Tr("stash_diag_objects_unsupported")
                                    : dg.objectsStatus == kStashObjNotRequested ? I18n::Tr("stash_diag_objects_not_requested")
                                                                                : I18n::Tr("stash_diag_objects_not_ready");
            Hint(I18n::TrF("stash_diag_objects", {{"status", objStatus}, {"scan", scan}}).c_str());
        }

        // Stash objects the bot knows in this area.
        int known = 0;
        if (v)
            for (const auto& it : v->interactables)
                if (it.kind == InteractKind::Stash || it.kind == InteractKind::GuildStash) ++known;
        if (known > 0 &&
            ImGui::BeginTable("##stashObjs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn(I18n::TrId("stash_target"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(I18n::TrId("stash_col_id"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(I18n::TrId("stash_col_cell"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(I18n::TrId("loot_col_distance"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableHeadersRow();
            for (const auto& it : v->interactables) {
                if (it.kind != InteractKind::Stash && it.kind != InteractKind::GuildStash) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (it.visibleNow) ImGui::TextUnformatted(I18n::Tr(InteractKindName(it.kind)));
                else ImGui::TextUnformatted(I18n::TrF("stash_diag_remembered", {{"kind", I18n::Tr(InteractKindName(it.kind))}}).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", it.id);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f, %.0f", it.grid.x, it.grid.y);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    I18n::TrF("stash_diag_cells", {{"cells", Num("%.0f", Dist(it.grid, v->selfGrid))}}).c_str());
            }
            ImGui::EndTable();
        } else {
            Hint(I18n::Tr("stash_diag_no_objects"));
        }
        for (const auto& o : dg.objects) {
            Hint(I18n::TrF("stash_diag_host_object", {{"kind", o.guild ? I18n::Tr("stash_domain_guild") : I18n::Tr("stash_domain_own")},
                                                      {"id", Num("%u", o.id)},
                                                      {"rank", Num("%d", o.rank)},
                                                      {"x", Num("%.0f", o.grid.x)},
                                                      {"y", Num("%.0f", o.grid.y)},
                                                      {"path", o.path}})
                     .c_str());
        }

        // Tabs.
        ImGui::Spacing();
        if (dg.tabsStatus == -3) {
            Hint(I18n::Tr("stash_diag_tabs_unread"));
        } else {
            if (dg.tabsStatus >= 0)
                Hint(I18n::TrF("stash_diag_tabs",
                               {{"stash", dg.tabsGuild ? I18n::Tr("stash_domain_guild") : I18n::Tr("stash_domain_own")},
                                {"seconds", Num("%.1f", v ? static_cast<double>(v->nowMs - (std::min)(v->nowMs, dg.tabsMs)) / 1000.0
                                                          : 0.0)},
                                {"count", Num("%d", dg.tabsStatus)},
                                {"toggle", dg.toggleOnScreen ? I18n::Tr("stash_diag_toggle_visible")
                                                             : I18n::Tr("stash_diag_toggle_unavailable")}})
                         .c_str());
            else
                Hint(I18n::TrF("stash_diag_tabs_status", {{"status", dg.tabsStatus == -2 ? I18n::Tr("stash_diag_tabs_unsupported")
                                                                                         : I18n::Tr("stash_diag_panel_closed")}})
                         .c_str());
            if (!dg.tabs.empty() &&
                ImGui::BeginTable("##stashTabsDiag", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn(I18n::TrId("stash_col_tab"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn(I18n::TrId("stash_col_selected"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn(I18n::TrId("stash_col_on_screen"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableHeadersRow();
                for (const auto& t : dg.tabs) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", t.index);
                    ImGui::SameLine(0.0f, ImGui::GetFontSize());
                    ImGui::TextUnformatted(t.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(t.active == 1 ? I18n::Tr("common_yes") : t.active == 0 ? I18n::Tr("common_no") : "?");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ui::YesNo(t.onScreen));
                }
                ImGui::EndTable();
            }
        }

        // Plan.
        ImGui::Spacing();
        if (dg.planMs == 0) {
            Hint(I18n::Tr("stash_diag_no_plan"));
        } else {
            int total = 0;
            for (const auto& g : dg.plan) total += g.items;
            Hint(I18n::TrF("stash_diag_plan", {{"items", Num("%d", total)},
                                               {"kept", Num("%d", dg.planKept)},
                                               {"protected", Num("%d", dg.planProtected)}})
                     .c_str());
            ImGui::Indent();
            for (const auto& g : dg.plan)
                Hint(I18n::TrF("stash_diag_plan_group", {{"tab", g.tab.empty() ? std::string(I18n::Tr("stash_open_tab")) : g.tab},
                                                         {"items", Num("%d", g.items)}})
                         .c_str());
            ImGui::Unindent();
        }

        // The run.
        ImGui::Spacing();
        if (dg.trigger && dg.trigger[0]) {
            Hint(I18n::TrF("stash_diag_run", {{"trigger", I18n::Tr(dg.trigger)},
                                              {"phase", dg.phase && dg.phase[0] ? I18n::Tr(dg.phase) : ""},
                                              {"moved", Num("%d", dg.moved)},
                                              {"skipped", Num("%d", dg.skipped)}})
                     .c_str());
        } else if (running && dg.whyNot && dg.whyNot[0]) {
            Hint(I18n::TrF("stash_diag_no_auto_run", {{"why", I18n::Tr(dg.whyNot)}}).c_str());
        }
        if (!dg.lastResult.empty()) Hint(I18n::TrF("stash_diag_last_run", {{"result", ui::Text(dg.lastResult)}}).c_str());
        if (!dg.lastFailure.empty()) {
            const double ago = v && dg.lastFailureMs && v->nowMs >= dg.lastFailureMs
                                   ? static_cast<double>(v->nowMs - dg.lastFailureMs) / 1000.0
                                   : 0.0;
            ui::ColoredWrapped(pal.Error, I18n::TrF("travel_diag_last_failure", {{"seconds", Num("%.0f", ago)},
                                                                                 {"why", ui::Text(dg.lastFailure)}})
                                              .c_str());
        }
        for (const auto& w : dg.warnings) ui::Warning(ui::Text(w).c_str());
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
