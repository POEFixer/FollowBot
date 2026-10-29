// UiLoot.cpp - the Loot tab: what the bot picks up (categories, name lists,
// value), where and when (constraints), how (pickup timing), and the live
// Diagnostics list of nearby items with each decision and its reason.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../common/Clock.h"
#include "../common/Strings.h"
#include "../loot/LootCategories.h"
#include "Tabs.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

namespace {

constexpr const char* kRarityKeys[] = {I18n::Key("rarity_normal"), I18n::Key("rarity_magic"), I18n::Key("rarity_rare"),
                                       I18n::Key("rarity_unique")};

const char* RarityShort(int r) {
    if (r >= 0 && r < 4) return I18n::Tr(kRarityKeys[r]);
    return I18n::Tr("common_none");
}

// One entry per line (names may contain commas), trimmed, empty lines dropped,
// capped like the settings file.
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string t = Trim(std::string_view(text).substr(start, i - start));
            if (!t.empty()) {
                if (t.size() > kLootNameMaxLen) t.resize(kLootNameMaxLen);
                out.push_back(std::move(t));
                if (out.size() >= kLootNameListMax) break;
            }
            start = i + 1;
        }
    }
    return out;
}

// A multi-line name list editor. The text buffer is kept while the field is
// being edited (re-joining the parsed list on every keystroke would eat a new
// line); it follows the settings again once the field is left or the list
// belongs to another profile. Render thread only.
struct ListEditor {
    std::string text;
    const void* owner = nullptr;
    bool        active = false;
};

bool NameListEditor(const char* id, ListEditor& ed, std::vector<std::string>& list, float height) {
    if (!ed.active || ed.owner != &list) {
        ed.text = JoinLines(list);
        ed.owner = &list;
    }
    const bool edited = PluginUi::InputStringMultiline(id, ed.text, ImVec2(-FLT_MIN, height));
    ed.active = ImGui::IsItemActive();
    if (!edited) return false;
    std::vector<std::string> parsed = SplitLines(ed.text);
    if (parsed == list) return false;
    list = std::move(parsed);
    return true;
}

ListEditor g_includeEditor;
ListEditor g_excludeEditor;

// The host price status, re-read every 2 s (the call copies the host's load view).
struct PriceStatusCache {
    uint64_t readMs = 0;
    PluginSDK::PriceStatus status;
};
PriceStatusCache g_priceStatus;

// "<value> <currency unit>" for a drag field (a printf format: the value is code).
std::string ValueFormat(RefCurrency c) {
    return std::string("%.1f ") + ui::EscapePercent(I18n::Tr(RefCurrencyShort(c)));
}

}  // namespace

void FollowBotPlugin::DrawTabLoot() {
    LootSettings& l = EditProfile().loot;
    bool changed = false;
    const char* cells = I18n::Key("unit_cells");
    const char* ms = I18n::Key("unit_ms");
    const char* px = I18n::Key("unit_px");
    const std::vector<const char*> rarityNames = ui::TrAll(kRarityKeys);

    // --- Loot ------------------------------------------------------------------
    PluginUi::BeginCard("##loot");
    Heading(ICON_FA_COINS, I18n::Tr("loot_title"), I18n::Tr("loot_title_help"));
    changed |= Check(I18n::Tr("loot_pick_up"), "lootOn", &l.enabled, I18n::Tr("loot_pick_up_tip"));
    changed |= Check(I18n::Tr("loot_quest"), "lootQuest", &l.alwaysQuestItems, I18n::Tr("loot_quest_tip"));
    if (!l.enabled && !l.alwaysQuestItems) Hint(I18n::Tr("loot_off_hint"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Categories ----------------------------------------------------------------
    PluginUi::BeginCard("##lootCats");
    Heading(ICON_FA_FILTER, I18n::Tr("loot_categories"), I18n::Tr("loot_categories_help"));
    ImGui::BeginDisabled(!l.enabled);
    if (ImGui::BeginTable("##cats", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        // The two control columns get what their widest content needs; the names take the rest.
        const ImGuiStyle& st = ImGui::GetStyle();
        float widestRarity = 0.f;
        for (const char* name : rarityNames) widestRarity = (std::max)(widestRarity, ImGui::CalcTextSize(name).x);
        const float rarityW = (std::max)(ImGui::CalcTextSize(I18n::Tr("loot_min_rarity")).x,
                                         widestRarity + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight());
        const std::string widestValue = std::string("10000.0 ") + I18n::Tr(RefCurrencyShort(l.currency));
        const float valueW = (std::max)(ImGui::CalcTextSize(I18n::Tr("loot_min_value")).x,
                                        ImGui::CalcTextSize(widestValue.c_str()).x + st.FramePadding.x * 2.0f);
        ImGui::TableSetupColumn(I18n::TrId("loot_category"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(I18n::TrId("loot_min_rarity"), ImGuiTableColumnFlags_WidthFixed, rarityW);
        ImGui::TableSetupColumn(I18n::TrId("loot_min_value"), ImGuiTableColumnFlags_WidthFixed, valueW);
        ImGui::TableHeadersRow();
        const std::string valueFmt = ValueFormat(l.currency);
        size_t orderCount = 0;
        const ItemCategory* order = LootCategoryDisplayOrder(orderCount);
        for (size_t k = 0; k < orderCount; ++k) {
            const ItemCategory c = order[k];
            const int i = static_cast<int>(c);
            LootCategoryRule& r = l.categories[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool questForced = c == ItemCategory::Quest && l.alwaysQuestItems;
            ImGui::BeginDisabled(questForced);
            changed |= Check(I18n::Tr(ItemCategoryName(c)), "on", &r.enabled, I18n::Tr(LootCategoryHint(c)));
            ImGui::EndDisabled();
            if (questForced) Tip(I18n::Tr("loot_quest_forced"));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!r.enabled || !CategoryUsesRarity(c));
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= PluginUi::Combo("##rar", r.minRarity, rarityNames.data(), static_cast<int>(rarityNames.size()));
            ImGui::EndDisabled();
            Tip(CategoryUsesRarity(c) ? I18n::Tr("loot_min_rarity_tip") : I18n::Tr("loot_no_rarity_tip"));
            ImGui::TableNextColumn();
            const bool valueApplies = PricedByBaseName(c) || c == ItemCategory::Uniques;
            ImGui::BeginDisabled(!r.enabled || !valueApplies);
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##val", &r.minValue, 0.1f, 0.f, 100000.f, valueFmt.c_str(), ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            Tip(valueApplies ? I18n::Tr("loot_min_value_tip") : I18n::Tr("loot_no_price_tip"));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Value -------------------------------------------------------------------
    PluginUi::BeginCard("##lootValue");
    Heading(ICON_FA_SACK_DOLLAR, I18n::Tr("loot_value"), I18n::Tr("loot_value_help"));
    ImGui::BeginDisabled(!l.enabled);
    changed |= Check(I18n::Tr("loot_value_on"), "valueOn", &l.valueEnabled, I18n::Tr("loot_value_on_tip"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!l.valueEnabled);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    {
        const std::string fmt = ValueFormat(l.currency);
        changed |= ImGui::DragFloat("##minValue", &l.minValue, 0.1f, 0.f, 100000.f, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::EndDisabled();
    {
        const LabelColumn col({I18n::Tr("loot_ref_currency")});
        col.Row(I18n::Tr("loot_ref_currency"));
        int c = static_cast<int>(l.currency);
        const char* currencies[] = {I18n::Tr(RefCurrencyName(RefCurrency::Chaos)), I18n::Tr(RefCurrencyName(RefCurrency::Divine)),
                                    I18n::Tr(RefCurrencyName(RefCurrency::Exalted))};
        static_assert(sizeof(currencies) / sizeof(currencies[0]) == static_cast<size_t>(RefCurrency::Count),
                      "one name per reference currency");
        if (PluginUi::Combo("##refCur", c, currencies, static_cast<int>(RefCurrency::Count))) {
            l.currency = static_cast<RefCurrency>(c);
            changed = true;
        }
        Tip(I18n::Tr("loot_ref_currency_tip"));
    }
    changed |= Check(I18n::Tr("loot_value_per_cell"), "valueCell", &l.valuePerCell, I18n::Tr("loot_value_per_cell_tip"));
    ImGui::EndDisabled();
    {
        const uint64_t now = NowMs();
        if (g_priceStatus.readMs == 0 || now - g_priceStatus.readMs > 2000) {
            g_priceStatus.readMs = now;
            g_priceStatus.status = ctx()->Prices.GetStatus();
        }
        if (g_priceStatus.status.loaded)
            Hint(I18n::TrF("loot_prices_loaded", {{"count", Num("%d", g_priceStatus.status.totalItems)}}).c_str());
        else
            Hint(I18n::Tr("loot_prices_not_loaded"));
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Name lists ------------------------------------------------------------
    PluginUi::BeginCard("##lootNames");
    Heading(ICON_FA_LIST, I18n::Tr("loot_name_lists"), I18n::Tr("loot_name_lists_help"));
    {
        const float h = ImGui::GetTextLineHeight() * 5.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
        const bool twoCols = PluginUi::WideEnoughForTwoColumns();
        if (twoCols && ImGui::BeginTable("##names", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(I18n::Tr("loot_always"));
            Tip(I18n::Tr("loot_always_tip"));
            changed |= NameListEditor("##include", g_includeEditor, l.includeNames, h);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(I18n::Tr("loot_never"));
            Tip(I18n::Tr("loot_never_tip"));
            changed |= NameListEditor("##exclude", g_excludeEditor, l.excludeNames, h);
            ImGui::EndTable();
        } else if (!twoCols) {
            ImGui::TextUnformatted(I18n::Tr("loot_always"));
            Tip(I18n::Tr("loot_always_tip"));
            changed |= NameListEditor("##include", g_includeEditor, l.includeNames, h);
            ImGui::TextUnformatted(I18n::Tr("loot_never"));
            Tip(I18n::Tr("loot_never_tip"));
            changed |= NameListEditor("##exclude", g_excludeEditor, l.excludeNames, h);
        }
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Where and when -----------------------------------------------------------
    PluginUi::BeginCard("##lootWhere");
    Heading(ICON_FA_LOCATION_CROSSHAIRS, I18n::Tr("loot_where"), I18n::Tr("loot_where_help"));
    {
        const LabelColumn col({I18n::Tr("loot_scan_radius"), I18n::Tr("loot_leader_within"), I18n::Tr("loot_leader_beyond"),
                               I18n::Tr("loot_no_enemy"), I18n::Tr("loot_keep_free")});
        changed |= ui::FloatRow(col, I18n::Tr("loot_scan_radius"), "##scanR", &l.scanRadius, 5.f, 150.f, cells, "%.0f",
                                I18n::Tr("loot_scan_radius_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("loot_leader_within"), "##leaderR", &l.maxLeaderDistance, 10.f, 200.f, cells,
                                "%.0f", I18n::Tr("loot_leader_within_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("loot_leader_beyond"), "##leaderExtra", &l.preemptLeaderExtra, 0.f, 60.f,
                                I18n::Key("unit_plus_cells"), "%.0f", I18n::Tr("loot_leader_beyond_tip"));
        ImGui::BeginDisabled(l.lootDuringCombat);
        changed |= ui::FloatRow(col, I18n::Tr("loot_no_enemy"), "##enemyR", &l.enemyRadius, 0.f, 120.f, cells, "%.0f",
                                I18n::Tr("loot_no_enemy_tip"));
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, I18n::Tr("loot_keep_free"), "##minFree", &l.minFreeCells, 0, kBackpackCells - 1, nullptr,
                              I18n::Tr("loot_keep_free_tip"));
    }
    changed |= Check(I18n::Tr("loot_during_combat"), "lootCombat", &l.lootDuringCombat, I18n::Tr("loot_during_combat_tip"));
    changed |= Check(I18n::Tr("loot_in_town"), "lootTown", &l.lootInTownHideout, I18n::Tr("loot_in_town_tip"));
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Pickup timing ----------------------------------------------------------------
    PluginUi::BeginCard("##lootTiming");
    Heading(ICON_FA_STOPWATCH, I18n::Tr("loot_pickup"), I18n::Tr("loot_pickup_help"));
    {
        const LabelColumn col({I18n::Tr("loot_walk_up"), I18n::Tr("loot_label_offset"), I18n::Tr("travel_hover"),
                               I18n::Tr("loot_click_hold"), I18n::Tr("travel_rehovers"), I18n::Tr("loot_vanish_wait"),
                               I18n::Tr("loot_vanish_moving"), I18n::Tr("loot_tries"), I18n::Tr("loot_blacklist"),
                               I18n::Tr("loot_max_continuous"), I18n::Tr("loot_yield"), I18n::Tr("loot_price_budget")});
        changed |= ui::FloatRow(col, I18n::Tr("loot_walk_up"), "##pickRange", &l.pickupRange, 2.f, 20.f, cells, "%.0f",
                                I18n::Tr("loot_walk_up_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_label_offset"), "##labelOff", &l.labelOffsetPx, 0, 80, px,
                              I18n::Tr("loot_label_offset_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_hover"), "##lootDwell", &l.hoverDwellMs, 40, 400, ms,
                              I18n::Tr("loot_hover_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_click_hold"), "##lootHold", &l.pressHoldMs, 20, 200, ms,
                              I18n::Tr("loot_click_hold_tip"));
        changed |= ui::IntRow(col, I18n::Tr("travel_rehovers"), "##lootReHover", &l.reHovers, 0, 4, nullptr,
                              I18n::Tr("loot_rehovers_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_vanish_wait"), "##vanish", &l.vanishWaitMs, 200, 5000, ms,
                              I18n::Tr("loot_vanish_wait_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_vanish_moving"), "##vanishMove", &l.vanishWaitMovingMs, l.vanishWaitMs, 10000,
                              ms, I18n::Tr("loot_vanish_moving_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_tries"), "##attempts", &l.maxAttempts, 1, 10, nullptr,
                              I18n::Tr("loot_tries_tip"));
        {
            int seconds = l.blacklistMs / 1000;
            if (ui::IntRow(col, I18n::Tr("loot_blacklist"), "##blacklist", &seconds, 5, 600, I18n::Key("unit_s"),
                           I18n::Tr("loot_blacklist_tip"))) {
                l.blacklistMs = seconds * 1000;
                changed = true;
            }
        }
        changed |= ui::IntRow(col, I18n::Tr("loot_max_continuous"), "##maxCont", &l.maxContinuousMs, 1000, 60000, ms,
                              I18n::Tr("loot_max_continuous_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_yield"), "##yieldCd", &l.yieldCooldownMs, 0, 30000, ms,
                              I18n::Tr("loot_yield_tip"));
        changed |= ui::IntRow(col, I18n::Tr("loot_price_budget"), "##priceBudget", &l.priceLookupBudget, 1, 256, nullptr,
                              I18n::Tr("loot_price_budget_tip"));
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Diagnostics -------------------------------------------------------------
    if (PluginUi::BeginDiagnostics(I18n::Tr("loot_diag_tip"), "lootDiag")) {
        const PickupTask& pick = m_brain.Pickup();
        const LootScanner& scanner = pick.Scanner();
        const uint64_t now = NowMs();
        scanner.RequestDiagnostics(now);   // the worker publishes while this card is open
        const std::shared_ptr<const LootDiagnostics> d = scanner.Diagnostics();
        const BotView* v = m_uiView.get();
        Hint(ui::Join({I18n::TrF("loot_diag_status", {{"status", I18n::Tr(pick.Status())}}),
                       I18n::TrF("loot_diag_picked", {{"count", Num("%d", pick.PickedTotal())}})})
                 .c_str());
        if (v && v->taskName == pick.Name() && !v->taskDetail.empty()) Hint(ui::Text(v->taskDetail).c_str());
        if (!d) {
            Hint(I18n::Tr("loot_diag_no_data"));
        } else {
            const double age = static_cast<double>(now >= d->atMs ? now - d->atMs : 0) / 1000.0;
            const std::string backpack =
                !d->inventoryValid ? std::string(I18n::Tr("loot_diag_backpack_unread"))
                : d->inventoryFull ? I18n::TrF("loot_diag_backpack_full", {{"cells", Num("%d", d->freeCells)}})
                                   : I18n::TrF("loot_diag_backpack", {{"cells", Num("%d", d->freeCells)}});
            Hint(ui::Join({backpack, I18n::TrF("loot_diag_gave_up", {{"count", Num("%d", d->blacklisted)}}),
                           I18n::TrF("loot_diag_unresolved", {{"count", Num("%d", d->unresolved)}}),
                           d->inventoryValid ? I18n::TrF("loot_diag_prices_kept", {{"count", Num("%d", static_cast<int>(d->priceMemo))}})
                                             : std::string(),
                           I18n::TrF("loot_diag_age", {{"seconds", Num("%.1f", age)}})})
                     .c_str());
            if (d->rows.empty()) {
                Hint(I18n::Tr("loot_diag_no_items"));
            } else if (ImGui::BeginTable("##lootRows", 6,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX |
                                             ImGuiTableFlags_ScrollY,
                                         ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(I18n::TrId("loot_col_item"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn(I18n::TrId("loot_category"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn(I18n::TrId("loot_col_rarity"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(I18n::TrId("loot_col_distance"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(I18n::TrId("loot_value"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn(I18n::TrId("loot_col_decision"), ImGuiTableColumnFlags_WidthStretch, 4.0f);
                ImGui::TableHeadersRow();
                const PluginUi::Palette pal = PluginUi::Colors();
                const char* dcur = I18n::Tr(RefCurrencyShort(d->currency));
                for (const LootDiagRow& r : d->rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (r.stack > 1) ImGui::Text("%s x%d", r.name.c_str(), r.stack);
                    else ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(I18n::Tr(ItemCategoryName(r.category)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(RarityShort(r.rarity));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", r.dist);
                    ImGui::TableNextColumn();
                    if (r.priceKnown) ImGui::Text("%.2f %s", r.value, dcur);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    const char* reason = I18n::Tr(r.reason);
                    if (r.blacklisted) {
                        ImGui::TextColored(pal.Error, "%s", I18n::TrF("loot_row_left_alone", {{"why", reason}}).c_str());
                    } else if (r.pick && !r.fits) {
                        ImGui::TextColored(pal.Warning, "%s", I18n::TrF("loot_row_no_room", {{"why", reason}}).c_str());
                    } else if (r.pick) {
                        ImGui::TextColored(pal.Success, "%s", I18n::TrF("loot_row_pick", {{"why", reason}}).c_str());
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, pal.TextSecondary);
                        ImGui::TextUnformatted(I18n::TrF("loot_row_leave", {{"why", reason}}).c_str());
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndTable();
            }
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
