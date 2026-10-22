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

const char* const kRarityNames[] = {"Normal", "Magic", "Rare", "Unique"};
const char* const kCurrencyNames[] = {"Chaos Orb", "Divine Orb", "Exalted Orb"};
static_assert(sizeof(kCurrencyNames) / sizeof(kCurrencyNames[0]) == static_cast<size_t>(RefCurrency::Count),
              "one name per reference currency");

const char* RarityShort(int r) {
    switch (r) {
        case 0: return "Normal";
        case 1: return "Magic";
        case 2: return "Rare";
        case 3: return "Unique";
        default: return "-";
    }
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

}  // namespace

void FollowBotPlugin::DrawTabLoot() {
    LootSettings& l = EditProfile().loot;
    bool changed = false;
    const char* cur = RefCurrencyShort(l.currency);

    // --- Loot ------------------------------------------------------------------
    PluginUi::BeginCard("##loot");
    Heading(ICON_FA_COINS, "Loot",
            "Which ground items the bot picks up. Items are recognised by their kind in the game data\n"
            "(never by the text on the label), so the rules work with any client language.\n"
            "Order of the rules: the exclude list wins, then quest items, then the include list,\n"
            "then the item's category, then the value threshold.");
    changed |= Check("Pick up loot", "lootOn", &l.enabled,
                     "Master switch. Off: nothing is picked up, except quest items when the option below is on.");
    changed |= Check("Always pick up quest items", "lootQuest", &l.alwaysQuestItems,
                     "Quest items are picked up even with loot off and whatever the Quest items row says,\n"
                     "so a follower never blocks campaign progress. The exclude list still wins.");
    if (!l.enabled && !l.alwaysQuestItems) Hint("Loot is off: the bot picks nothing up.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Categories ----------------------------------------------------------------
    PluginUi::BeginCard("##lootCats");
    Heading(ICON_FA_FILTER, "Categories",
            "Tick the kinds of items to pick up.\n"
            "Min rarity: only for items that have a rarity (equipment, jewels, flasks and charms,\n"
            "waystones, tablets); a unique item always counts as \"Unique items\".\n"
            "Min value: only items the price list knows (currency-like loot and uniques, English client),\n"
            "in the reference currency of the Value card; an item without a known price is picked\n"
            "by the tick alone.");
    ImGui::BeginDisabled(!l.enabled);
    if (ImGui::BeginTable("##cats", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        // The two control columns get what their widest content needs; the names take the rest.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float rarityW = (std::max)(ImGui::CalcTextSize("Min rarity").x,
                                         ImGui::CalcTextSize("Unique").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight());
        const float valueW = (std::max)(ImGui::CalcTextSize("Min value").x,
                                        ImGui::CalcTextSize("10000.0 div").x + st.FramePadding.x * 2.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Min rarity", ImGuiTableColumnFlags_WidthFixed, rarityW);
        ImGui::TableSetupColumn("Min value", ImGuiTableColumnFlags_WidthFixed, valueW);
        ImGui::TableHeadersRow();
        char valueFmt[24];
        std::snprintf(valueFmt, sizeof(valueFmt), "%%.1f %s", cur);
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
            changed |= Check(ItemCategoryName(c), "on", &r.enabled, LootCategoryHint(c));
            ImGui::EndDisabled();
            if (questForced) Tip("Always picked up (Loot card).");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!r.enabled || !CategoryUsesRarity(c));
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= PluginUi::Combo("##rar", r.minRarity, kRarityNames, 4);
            ImGui::EndDisabled();
            Tip(CategoryUsesRarity(c) ? "Lowest rarity picked up in this category." : "Items of this kind have no rarity.");
            ImGui::TableNextColumn();
            const bool valueApplies = PricedByBaseName(c) || c == ItemCategory::Uniques;
            ImGui::BeginDisabled(!r.enabled || !valueApplies);
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##val", &r.minValue, 0.1f, 0.f, 100000.f, valueFmt, ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            Tip(valueApplies ? "0 = any value. Applies only when the price is known (else the tick decides)."
                             : "The price list has no prices for these (only for their uniques).");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Value -------------------------------------------------------------------
    PluginUi::BeginCard("##lootValue");
    Heading(ICON_FA_SACK_DOLLAR, "Value",
            "Prices come from POEFixer's price list (Settings -> Prices); the plugin makes no requests.\n"
            "The list is in English: with another client language most prices are unknown and the\n"
            "category ticks decide. Magic and rare equipment has no price (only uniques do).");
    ImGui::BeginDisabled(!l.enabled);
    changed |= Check("Also pick anything worth at least", "valueOn", &l.valueEnabled,
                     "Picks an item whose known value reaches the threshold, even when its category is off.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!l.valueEnabled);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    {
        char fmt[24];
        std::snprintf(fmt, sizeof(fmt), "%%.1f %s", cur);
        changed |= ImGui::DragFloat("##minValue", &l.minValue, 0.1f, 0.f, 100000.f, fmt, ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::EndDisabled();
    {
        const LabelColumn col({"Reference currency"});
        col.Row("Reference currency");
        int c = static_cast<int>(l.currency);
        if (PluginUi::Combo("##refCur", c, kCurrencyNames, static_cast<int>(RefCurrency::Count))) {
            l.currency = static_cast<RefCurrency>(c);
            changed = true;
        }
        Tip("Every value on this tab is in this currency.");
    }
    changed |= Check("Compare the value per backpack cell", "valueCell", &l.valuePerCell,
                     "Divides an item's value by its size (width x height) before every comparison,\n"
                     "so a big item must be worth more to be picked up.");
    ImGui::EndDisabled();
    {
        const uint64_t now = NowMs();
        if (g_priceStatus.readMs == 0 || now - g_priceStatus.readMs > 2000) {
            g_priceStatus.readMs = now;
            g_priceStatus.status = ctx()->Prices.GetStatus();
        }
        char line[160];
        if (g_priceStatus.status.loaded)
            std::snprintf(line, sizeof(line), "Price list: %d items loaded.", g_priceStatus.status.totalItems);
        else
            std::snprintf(line, sizeof(line), "Price list: not loaded yet - every price counts as unknown.");
        Hint(line);
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Name lists ------------------------------------------------------------
    PluginUi::BeginCard("##lootNames");
    Heading(ICON_FA_LIST, "Name lists",
            "One entry per line, any letter case. An entry matches when it appears in the item's base\n"
            "name, its unique name or its game path (e.g. Currency/CurrencyRerollRare).\n"
            "In double quotes (\"Chaos Orb\") it must be the whole name, so \"Greater Chaos Orb\" is\n"
            "not matched. Names are the ones your game client shows (its language).");
    {
        const float h = ImGui::GetTextLineHeight() * 5.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
        const bool twoCols = PluginUi::WideEnoughForTwoColumns();
        if (twoCols && ImGui::BeginTable("##names", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Always pick up");
            Tip("Picked up even when the category is off (the exclude list still wins).");
            changed |= NameListEditor("##include", g_includeEditor, l.includeNames, h);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Never pick up");
            Tip("Wins over everything, quest items included.");
            changed |= NameListEditor("##exclude", g_excludeEditor, l.excludeNames, h);
            ImGui::EndTable();
        } else if (!twoCols) {
            ImGui::TextUnformatted("Always pick up");
            Tip("Picked up even when the category is off (the exclude list still wins).");
            changed |= NameListEditor("##include", g_includeEditor, l.includeNames, h);
            ImGui::TextUnformatted("Never pick up");
            Tip("Wins over everything, quest items included.");
            changed |= NameListEditor("##exclude", g_excludeEditor, l.excludeNames, h);
        }
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Where and when -----------------------------------------------------------
    PluginUi::BeginCard("##lootWhere");
    Heading(ICON_FA_LOCATION_CROSSHAIRS, "Where and when",
            "Distances are in grid cells (about a tenth of a character's height).\n"
            "Following comes first: the bot loots only while the leader is close, and stops looting\n"
            "when he moves away or enemies come near.");
    {
        const LabelColumn col({"Scan radius", "Leader within", "Stop when leader beyond", "No enemy within",
                               "Keep free cells"});
        changed |= ui::FloatRow(col, "Scan radius", "##scanR", &l.scanRadius, 5.f, 150.f, "%.0f cells",
                                "Items farther than this from you are ignored.");
        changed |= ui::FloatRow(col, "Leader within", "##leaderR", &l.maxLeaderDistance, 10.f, 200.f, "%.0f cells",
                                "Loot only while the leader is this close, and only items this close to him.\n"
                                "Without a leader set, this rule does not apply.");
        changed |= ui::FloatRow(col, "Stop when leader beyond", "##leaderExtra", &l.preemptLeaderExtra, 0.f, 60.f,
                                "+%.0f cells",
                                "A pickup in progress stops once the leader is farther than \"Leader within\" plus this.");
        ImGui::BeginDisabled(l.lootDuringCombat);
        changed |= ui::FloatRow(col, "No enemy within", "##enemyR", &l.enemyRadius, 0.f, 120.f, "%.0f cells",
                                "No looting while a hostile monster is this close to you or to the item.\n"
                                "0 = never wait for enemies.");
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, "Keep free cells", "##minFree", &l.minFreeCells, 0, kBackpackCells - 1, "%d",
                              "Leave this many backpack cells free (quest items and gold ignore it).\n"
                              "A wanted item that does not fit marks the backpack full for the stash trip.");
    }
    changed |= Check("Loot during combat", "lootCombat", &l.lootDuringCombat,
                     "Ignore the enemy distance above (the bot may walk into a pack for an item).");
    changed |= Check("Loot in town and hideout", "lootTown", &l.lootInTownHideout, "Off by default: nothing drops there.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Pickup timing ----------------------------------------------------------------
    PluginUi::BeginCard("##lootTiming");
    Heading(ICON_FA_STOPWATCH, "Pickup",
            "How an item is picked up: walk close, stop, hover its label, click, then wait for it to\n"
            "disappear from the ground. An item that will not go is tried a few times, then left\n"
            "alone for a while.");
    {
        const LabelColumn col({"Walk up to", "Label above item", "Hover before click", "Click hold", "Extra hovers",
                               "Wait to vanish", "Wait while walking", "Tries per item", "Then leave it for",
                               "Loot at most", "Then follow for", "Price lookups per scan"});
        changed |= ui::FloatRow(col, "Walk up to", "##pickRange", &l.pickupRange, 2.f, 20.f, "%.0f cells",
                                "Walk this close to an item before clicking it.");
        changed |= ui::IntRow(col, "Label above item", "##labelOff", &l.labelOffsetPx, 0, 80, "%d px",
                              "Click this far above the item's spot on the ground, where its label is drawn.");
        changed |= ui::IntRow(col, "Hover before click", "##lootDwell", &l.hoverDwellMs, 40, 400, "%d ms",
                              "The game must notice the cursor on the label before the click.");
        changed |= ui::IntRow(col, "Click hold", "##lootHold", &l.pressHoldMs, 20, 200, "%d ms", "How long the button is held.");
        changed |= ui::IntRow(col, "Extra hovers", "##lootReHover", &l.reHovers, 0, 4, "%d",
                              "When the label does not light up, hover a few pixels off this many times\n"
                              "before clicking anyway.");
        changed |= ui::IntRow(col, "Wait to vanish", "##vanish", &l.vanishWaitMs, 200, 5000, "%d ms",
                              "After the click the item must leave the ground within this.");
        changed |= ui::IntRow(col, "Wait while walking", "##vanishMove", &l.vanishWaitMovingMs, l.vanishWaitMs, 10000,
                              "%d ms",
                              "Longer limit while the character is still walking to the item after the click.");
        changed |= ui::IntRow(col, "Tries per item", "##attempts", &l.maxAttempts, 1, 10, "%d",
                              "Clicks on one item before the bot leaves it alone.");
        {
            int seconds = l.blacklistMs / 1000;
            if (ui::IntRow(col, "Then leave it for", "##blacklist", &seconds, 5, 600, "%d s",
                           "How long an item that would not go is left alone.")) {
                l.blacklistMs = seconds * 1000;
                changed = true;
            }
        }
        changed |= ui::IntRow(col, "Loot at most", "##maxCont", &l.maxContinuousMs, 1000, 60000, "%d ms",
                              "Longest looting spell; then the bot follows the leader again.");
        changed |= ui::IntRow(col, "Then follow for", "##yieldCd", &l.yieldCooldownMs, 0, 30000, "%d ms",
                              "After that limit, no looting for at least this long.");
        changed |= ui::IntRow(col, "Price lookups per scan", "##priceBudget", &l.priceLookupBudget, 1, 256, "%d",
                              "New items priced per scan (5 to 7 scans a second); the answers are kept for the area.");
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Diagnostics -------------------------------------------------------------
    if (PluginUi::BeginDiagnostics("Nearby items, what the bot decided for each and why.", "lootDiag")) {
        const PickupTask& pick = m_brain.Pickup();
        const LootScanner& scanner = pick.Scanner();
        const uint64_t now = NowMs();
        scanner.RequestDiagnostics(now);   // the worker publishes while this card is open
        const std::shared_ptr<const LootDiagnostics> d = scanner.Diagnostics();
        const BotView* v = m_uiView.get();
        char line[256];
        std::snprintf(line, sizeof(line), "Loot: %s   |   picked up this session: %d", pick.Status(), pick.PickedTotal());
        Hint(line);
        if (v && v->taskName == pick.Name() && !v->taskDetail.empty()) Hint(v->taskDetail.c_str());
        if (!d) {
            Hint("No data yet: the list fills a moment after this card opens (the game must be running).");
        } else {
            const double age = static_cast<double>(now >= d->atMs ? now - d->atMs : 0) / 1000.0;
            if (d->inventoryValid)
                std::snprintf(line, sizeof(line),
                              "Backpack: %d free cells%s   |   gave up on: %d   |   not readable yet: %d   |   prices kept: %d   |   %.1f s ago",
                              d->freeCells, d->inventoryFull ? " (full for loot)" : "", d->blacklisted, d->unresolved,
                              static_cast<int>(d->priceMemo), age);
            else
                std::snprintf(line, sizeof(line),
                              "Backpack: not read yet   |   gave up on: %d   |   not readable yet: %d   |   %.1f s ago",
                              d->blacklisted, d->unresolved, age);
            Hint(line);
            if (d->rows.empty()) {
                Hint("No items within the scan radius.");
            } else if (ImGui::BeginTable("##lootRows", 6,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX |
                                             ImGuiTableFlags_ScrollY,
                                         ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Rarity", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("Decision", ImGuiTableColumnFlags_WidthStretch, 4.0f);
                ImGui::TableHeadersRow();
                const PluginUi::Palette pal = PluginUi::Colors();
                const char* dcur = RefCurrencyShort(d->currency);
                for (const LootDiagRow& r : d->rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (r.stack > 1) ImGui::Text("%s x%d", r.name.c_str(), r.stack);
                    else ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ItemCategoryName(r.category));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(RarityShort(r.rarity));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", r.dist);
                    ImGui::TableNextColumn();
                    if (r.priceKnown) ImGui::Text("%.2f %s", r.value, dcur);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (r.blacklisted) {
                        ImGui::TextColored(pal.Error, "left alone for now (%s)", r.reason);
                    } else if (r.pick && !r.fits) {
                        ImGui::TextColored(pal.Warning, "wanted, no room (%s)", r.reason);
                    } else if (r.pick) {
                        ImGui::TextColored(pal.Success, "pick up: %s", r.reason);
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, pal.TextSecondary);
                        ImGui::Text("leave: %s", r.reason);
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
