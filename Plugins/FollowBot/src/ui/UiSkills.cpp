// UiSkills.cpp - the Skills tab: combat settings, the rule list (add,
// duplicate, move, delete, enable, "add from the skill bar"), the editor of the
// selected rule with every field, and Diagnostics (live skills, own and leader
// buffs with copy buttons, each rule's "why not firing" line, the last rule).
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../combat/SkillConditions.h"
#include "../common/Strings.h"
#include "Tabs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

namespace {

// Editor state (render thread only; one plugin instance per DLL).
int                      g_selected = 0;      // the first rule opens in the editor
std::string              g_blacklistText;
std::vector<std::string> g_blacklistShown;   // the list g_blacklistText was built from
bool                     g_blacklistEditing = false;
LocText                  g_barResult;        // result line of "Add rules from the skill bar"

constexpr size_t kMaxRules = 64;

// Tables of keys (lang/en.json), translated where they are drawn.
constexpr const char* kKindKeys[] = {I18n::Key("skills_kind_attack"), I18n::Key("skills_kind_self"),
                                     I18n::Key("skills_kind_leader"), I18n::Key("skills_kind_movement"),
                                     I18n::Key("skills_kind_flask")};
constexpr const char* kWhenKeys[] = {I18n::Key("skills_when_always"), I18n::Key("skills_when_in_combat"),
                                     I18n::Key("skills_when_out_of_combat")};
constexpr const char* kMoveKeys[] = {I18n::Key("skills_move_any"), I18n::Key("skills_move_moving"),
                                     I18n::Key("skills_move_standing")};
constexpr const char* kRarityKeys[] = {I18n::Key("rarity_normal"), I18n::Key("rarity_magic"), I18n::Key("rarity_rare"),
                                       I18n::Key("rarity_unique")};
constexpr const char* kModeKeys[] = {I18n::Key("skills_mode_passive"), I18n::Key("skills_mode_assist"),
                                     I18n::Key("skills_mode_aggressive")};
constexpr const char* kPressKeys[] = {I18n::Key("skills_press_tap"), I18n::Key("skills_press_hold"),
                                      I18n::Key("skills_press_channel")};
constexpr const char* kBeltKeys[] = {I18n::Key("skills_belt_not_checked"), I18n::Key("belt_flask_1"), I18n::Key("belt_flask_2"),
                                     I18n::Key("belt_charm_1"), I18n::Key("belt_charm_2"), I18n::Key("belt_charm_3")};

struct MouseButtonChoice {
    int         vk;
    const char* key;
};
constexpr MouseButtonChoice kMouseButtons[] = {
    {0x01, I18n::Key("skills_mouse_left")},   {0x02, I18n::Key("skills_mouse_right")},
    {0x04, I18n::Key("skills_mouse_middle")}, {0x05, I18n::Key("skills_mouse_4")},
    {0x06, I18n::Key("skills_mouse_5")},
};

// The keys a rule may not use, with what they are (the engine refuses them too): a key.
const char* ReservedKeyRole(int vk, const Settings& s, const Profile& p) {
    if (vk <= 0) return nullptr;
    const HotkeySettings& h = s.global.hotkeys;
    if (vk == h.startStop) return I18n::Key("skills_role_start_stop");
    if (vk == h.pause) return I18n::Key("skills_role_pause");
    if (vk == h.holdOverride) return I18n::Key("skills_role_override");
    if (vk == h.stashNow) return I18n::Key("skills_role_stash");
    const int move = p.follow.clickToMove ? 0x01 : p.follow.moveKey;
    if (vk == move) return p.follow.clickToMove ? I18n::Key("skills_role_move_button") : I18n::Key("skills_role_move_key");
    if (vk == p.follow.sprintKey) return I18n::Key("skills_role_sprint");
    return nullptr;
}

// Places the next item on this line when `width` still fits, else on a new one.
void SameLineIfFits(float width) {
    // After an item the cursor sits at the start of the next line: its right
    // edge is the content region's right edge.
    const float regionRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= regionRight) ImGui::SameLine();
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// The status colour, from the reason's key: ready / fired / holding green, off grey.
ImVec4 StatusColor(const LocText& s) {
    const PluginUi::Palette pal = PluginUi::Colors();
    if (StartsWith(s.key, "skill_why_ready") || StartsWith(s.key, "skill_why_fired") || s.key == "skill_why_holding")
        return pal.Success;
    if (s.key == "skill_why_off" || s.empty()) return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    return pal.TextSecondary;
}

// A distance where < 0 means "off": a checkbox + a slider (cells).
bool OptionalDistance(const char* label, const char* id, float* v, float defaultOn, const char* tip) {
    bool changed = false;
    bool on = *v >= 0.f;
    char cid[64];
    std::snprintf(cid, sizeof(cid), "%s_on", id);
    if (Check(label, cid, &on, tip)) {
        *v = on ? defaultOn : -1.f;
        changed = true;
    }
    if (on) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        char sid[64];
        std::snprintf(sid, sizeof(sid), "##%s", id);
        const std::string fmt = ui::Units(I18n::Key("unit_cells"), "%.0f");
        if (ImGui::SliderFloat(sid, v, 0.f, 300.f, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp)) changed = true;
    }
    return changed;
}

// Sorted unique non-empty names of live skills / buffs (anything with `.name`).
template <class T>
std::vector<std::string> UniqueNames(const std::vector<T>& items) {
    std::vector<std::string> out;
    for (const auto& it : items) {
        if (it.name.empty()) continue;
        if (std::find(out.begin(), out.end(), it.name) == out.end()) out.push_back(it.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A text field for a buff / skill name plus a "Pick" button listing the live
// names of `items` (built only while the list is open).
template <class T>
bool NameField(const char* id, const char* hint, std::string& text, const std::vector<T>& items,
               const char* emptyNote, const char* tip) {
    bool changed = false;
    ImGui::PushID(id);
    const float pickW = PluginUi::ButtonWidth(I18n::Tr("skills_pick"));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float em = ImGui::GetFontSize();
    const float fieldW = (std::max)(em * 6.0f, (std::min)(avail - pickW - ImGui::GetStyle().ItemSpacing.x, em * 15.0f));
    ImGui::SetNextItemWidth(fieldW);
    if (PluginUi::InputString("##name", hint, text)) changed = true;
    Tip(tip);
    ImGui::SameLine();
    if (PluginUi::SubtleButton(I18n::TrId("skills_pick"))) ImGui::OpenPopup("##pickPopup");
    Tip(I18n::Tr("skills_pick_tip"));
    if (ImGui::BeginPopup("##pickPopup")) {
        const std::vector<std::string> names = UniqueNames(items);
        if (names.empty()) {
            ImGui::TextDisabled("%s", emptyNote);
        } else {
            for (size_t i = 0; i < names.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(names[i].c_str(), text == names[i])) {
                    text = names[i];
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        if (!text.empty()) {
            ImGui::Separator();
            if (ImGui::Selectable(I18n::TrId("stash_clear"))) {
                text.clear();
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// A buff table with copy-on-click names and a "Copy all" button.
void BuffTable(const char* id, const std::vector<BuffInfo>& buffs, const char* emptyText) {
    ImGui::PushID(id);
    if (buffs.empty()) {
        Hint(emptyText);
        ImGui::PopID();
        return;
    }
    if (ImGui::BeginTable("##buffs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn(I18n::TrId("skills_col_buff"), ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn(I18n::TrId("skills_col_stacks"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(I18n::TrId("skills_col_time_left"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < buffs.size(); ++i) {
            const BuffInfo& b = buffs[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(b.name.empty() ? I18n::Tr("skills_unnamed") : b.name.c_str(), false) && !b.name.empty())
                ImGui::SetClipboardText(b.name.c_str());
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (b.charges > 0) ImGui::Text("%d", b.charges);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (b.totalTime > 0.f && b.timeLeft >= 0.f && b.timeLeft < 1e6f)
                ImGui::TextUnformatted(I18n::TrF("skills_seconds", {{"seconds", Num("%.1f", b.timeLeft)}}).c_str());
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    if (PluginUi::SubtleButton(ui::IconLabel(ICON_FA_COPY, I18n::Tr("skills_copy_all"), "copyAll").c_str())) {
        std::string all;
        for (const auto& b : buffs) {
            if (b.name.empty()) continue;
            if (!all.empty()) all += '\n';
            all += b.name;
        }
        ImGui::SetClipboardText(all.c_str());
    }
    Tip(I18n::Tr("skills_copy_buffs_tip"));
    ImGui::PopID();
}

}  // namespace

void FollowBotPlugin::DrawTabSkills() {
    Profile& prof = EditProfile();
    SkillSettings& sk = prof.skills;
    const BotView* v = m_uiView.get();
    bool changed = false;
    const std::vector<SkillInfo> noSkills;
    const std::vector<BuffInfo> noBuffs;
    const std::vector<SkillInfo>& liveSkills = v ? v->skills : noSkills;
    const std::vector<BuffInfo>& liveSelfBuffs = v ? v->selfBuffs : noBuffs;
    const std::vector<BuffInfo>& liveLeaderBuffs = v ? v->leaderBuffs : noBuffs;
    const char* cells = I18n::Key("unit_cells");
    const char* ms = I18n::Key("unit_ms");
    ImGui::PushID("skillsTab");

    // --- Combat -------------------------------------------------------------------
    PluginUi::BeginCard("##combat");
    Heading(ICON_FA_SHIELD_HALVED, I18n::Tr("skills_combat"), I18n::Tr("skills_combat_help"));
    changed |= Check(I18n::Tr("skills_on"), "skillsOn", &sk.enabled, I18n::Tr("skills_on_tip"));
    ImGui::BeginDisabled(!sk.enabled);
    {
        const LabelColumn col({I18n::Tr("skills_combat_mode"), I18n::Tr("skills_combat_radius"), I18n::Tr("skills_leash"),
                               I18n::Tr("skills_max_leader"), I18n::Tr("skills_cast_gate")});
        col.Row(I18n::Tr("skills_combat_mode"));
        int mode = static_cast<int>(sk.mode);
        const std::vector<const char*> modes = ui::TrAll(kModeKeys);
        if (PluginUi::Combo("##mode", mode, modes.data(), static_cast<int>(modes.size()))) {
            sk.mode = static_cast<CombatMode>(mode);
            changed = true;
        }
        Tip(I18n::Tr("skills_combat_mode_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("skills_combat_radius"), "##combatR", &sk.combatRadius, 5.f, 150.f, cells, "%.0f",
                                I18n::Tr("skills_combat_radius_tip"));
        changed |= ui::FloatRow(col, I18n::Tr("skills_leash"), "##leashR", &sk.leashRadius, 5.f, 200.f, cells, "%.0f",
                                I18n::Tr("skills_leash_tip"));
        ImGui::BeginDisabled(sk.mode == CombatMode::Passive);
        changed |= ui::FloatRow(col, I18n::Tr("skills_max_leader"), "##maxLeader", &sk.maxCombatLeaderDistance, 10.f, 300.f,
                                cells, "%.0f", I18n::Tr("skills_max_leader_tip"));
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, I18n::Tr("skills_cast_gate"), "##castGate", &sk.globalCastGateMs, 50, 2000, ms,
                              I18n::Tr("skills_cast_gate_tip"));
    }
    changed |= Check(I18n::Tr("skills_skip_untargetable"), "skipUntgt", &sk.skipUntargetable,
                     I18n::Tr("skills_skip_untargetable_tip"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(I18n::Tr("skills_blacklist"));
    Tip(I18n::Tr("skills_blacklist_tip"));
    {
        if (!g_blacklistEditing && g_blacklistShown != sk.monsterBlacklist) {
            g_blacklistShown = sk.monsterBlacklist;
            g_blacklistText = JoinLines(sk.monsterBlacklist);
        }
        const float h = ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
        if (PluginUi::InputStringMultiline("##blacklist", g_blacklistText, ImVec2(-FLT_MIN, h))) {
            sk.monsterBlacklist = SplitList(g_blacklistText);
            g_blacklistShown = sk.monsterBlacklist;
            changed = true;
        }
        g_blacklistEditing = ImGui::IsItemActive();
    }
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Rule list ----------------------------------------------------------------
    auto& rules = sk.rules;
    if (g_selected >= static_cast<int>(rules.size())) g_selected = static_cast<int>(rules.size()) - 1;
    PluginUi::BeginCard("##rules");
    Heading(ICON_FA_LIST_OL, I18n::Tr("skills_rules"), I18n::Tr("skills_rules_help"));
    ImGui::BeginDisabled(!sk.enabled);
    if (rules.empty()) {
        Hint(I18n::Tr("skills_no_rules_yet"));
    } else if (ImGui::BeginTable("##ruleTable", 5,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn(I18n::TrId("skills_col_on"), ImGuiTableColumnFlags_WidthFixed, 0.0f);
        ImGui::TableSetupColumn(I18n::TrId("skills_col_rule"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(I18n::TrId("skills_col_key"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(I18n::TrId("skills_col_kind"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn(I18n::TrId("skills_col_status"), ImGuiTableColumnFlags_WidthStretch, 3.5f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rules.size(); ++i) {
            SkillRule& r = rules[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &r.enabled)) changed = true;
            Tip(I18n::Tr("skills_rule_on_tip"));
            ImGui::TableNextColumn();
            const std::string name = ui::WithId(std::to_string(i + 1) + ". " + ui::RuleLabel(r.label), "sel");
            if (ImGui::Selectable(name.c_str(), g_selected == static_cast<int>(i))) g_selected = static_cast<int>(i);
            ImGui::TableNextColumn();
            if (r.key == 0) ImGui::TextColored(PluginUi::Colors().Warning, "%s", I18n::Tr("skills_key_none"));
            else ImGui::TextUnformatted(PluginUi::KeyName(r.key));
            ImGui::TableNextColumn();
            const int k = static_cast<int>(r.kind);
            ImGui::TextUnformatted(k >= 0 && k < 5 ? I18n::Tr(kKindKeys[k]) : "?");
            ImGui::TableNextColumn();
            const LocText status = (v && i < v->ruleWhyNot.size()) ? v->ruleWhyNot[i] : LocText();
            const std::string statusText = status.empty() ? std::string("-") : ui::Text(status);
            ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(status));
            ImGui::TextUnformatted(statusText.c_str());
            ImGui::PopStyleColor();
            if (!status.empty()) Tip(statusText.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Buttons.
    const bool haveSel = g_selected >= 0 && g_selected < static_cast<int>(rules.size());
    ImGui::BeginDisabled(rules.size() >= kMaxRules);
    if (ImGui::Button(ui::IconLabel(ICON_FA_PLUS, I18n::Tr("skills_add"), "addRule").c_str())) {
        SkillRule r;
        // Stored in English ("Rule 3"), shown in the program language until renamed (ui::RuleLabel).
        r.label = NewRuleLabel(rules.size() + 1);
        rules.push_back(r);
        g_selected = static_cast<int>(rules.size()) - 1;
        changed = true;
    }
    Tip(I18n::Tr("skills_add_tip"));
    const std::string dupLabel = ui::IconLabel(ICON_FA_CLONE, I18n::Tr("profile_duplicate"), "dupRule");
    SameLineIfFits(PluginUi::ButtonWidth(dupLabel.c_str()));
    ImGui::BeginDisabled(!haveSel);
    if (PluginUi::SubtleButton(dupLabel.c_str()) && haveSel) {
        SkillRule copy = rules[static_cast<size_t>(g_selected)];
        // Stored in English ("<label> copy"), shown through skills_copy_label (ui::RuleLabel).
        copy.label = CopyRuleLabel(copy.label);
        rules.insert(rules.begin() + g_selected + 1, copy);
        ++g_selected;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Tip(I18n::Tr("skills_duplicate_tip"));
    const std::string upLabel = ui::IconLabel(ICON_FA_ARROW_UP, I18n::Tr("skills_up"), "upRule");
    SameLineIfFits(PluginUi::ButtonWidth(upLabel.c_str()));
    ImGui::BeginDisabled(!haveSel || g_selected == 0);
    if (PluginUi::SubtleButton(upLabel.c_str()) && haveSel && g_selected > 0) {
        std::swap(rules[static_cast<size_t>(g_selected)], rules[static_cast<size_t>(g_selected - 1)]);
        --g_selected;
        changed = true;
    }
    ImGui::EndDisabled();
    Tip(I18n::Tr("skills_up_tip"));
    const std::string downLabel = ui::IconLabel(ICON_FA_ARROW_DOWN, I18n::Tr("skills_down"), "downRule");
    SameLineIfFits(PluginUi::ButtonWidth(downLabel.c_str()));
    ImGui::BeginDisabled(!haveSel || g_selected + 1 >= static_cast<int>(rules.size()));
    if (PluginUi::SubtleButton(downLabel.c_str()) && haveSel && g_selected + 1 < static_cast<int>(rules.size())) {
        std::swap(rules[static_cast<size_t>(g_selected)], rules[static_cast<size_t>(g_selected + 1)]);
        ++g_selected;
        changed = true;
    }
    ImGui::EndDisabled();
    Tip(I18n::Tr("skills_down_tip"));
    const std::string delLabel = ui::IconLabel(ICON_FA_TRASH, I18n::Tr("profile_delete"), "delRule");
    SameLineIfFits(PluginUi::ButtonWidth(delLabel.c_str()));
    ImGui::BeginDisabled(!haveSel);
    if (PluginUi::SubtleButton(delLabel.c_str())) ImGui::OpenPopup("##confirmDelete");
    ImGui::EndDisabled();
    Tip(I18n::Tr("skills_delete_tip"));
    if (haveSel) {
        const std::string q = I18n::TrF("skills_delete_confirm", {{"number", Num("%d", g_selected + 1)},
                                                                  {"label", ui::RuleLabel(rules[static_cast<size_t>(g_selected)].label)}});
        if (PluginUi::ConfirmPopup("##confirmDelete", q.c_str(), I18n::Tr("profile_delete"), I18n::Tr("common_cancel"))) {
            rules.erase(rules.begin() + g_selected);
            if (g_selected >= static_cast<int>(rules.size())) g_selected = static_cast<int>(rules.size()) - 1;
            changed = true;
        }
    }

    // One disabled rule per skill on the bar.
    {
        std::vector<const SkillInfo*> bar;
        for (const auto& s : liveSkills)
            if (s.onBar && !s.name.empty()) bar.push_back(&s);
        std::sort(bar.begin(), bar.end(), [](const SkillInfo* a, const SkillInfo* b) { return a->barSlot < b->barSlot; });
        const std::string barLabel = ui::IconLabel(ICON_FA_WAND_MAGIC_SPARKLES, I18n::Tr("skills_from_bar"), "fromBar");
        SameLineIfFits(PluginUi::ButtonWidth(barLabel.c_str()));
        ImGui::BeginDisabled(bar.empty() || rules.size() >= kMaxRules);
        if (PluginUi::SubtleButton(barLabel.c_str())) {
            int added = 0, skipped = 0;
            for (const SkillInfo* s : bar) {
                bool exists = false;
                for (const auto& r : rules)
                    if (NameEquals(r.skillName, s->name)) { exists = true; break; }
                if (exists || rules.size() >= kMaxRules) { ++skipped; continue; }
                SkillRule r;
                r.enabled = false;
                r.label = s->name;
                TruncateUtf8(r.label, 64);
                r.skillName = s->name;
                r.key = 0;
                rules.push_back(r);
                ++added;
            }
            g_barResult = TrF(skipped ? Key("skills_from_bar_result_skipped") : Key("skills_from_bar_result"),
                              {{"count", Num("%d", added)}});
            if (added > 0) {
                g_selected = static_cast<int>(rules.size()) - added;
                changed = true;
            }
        }
        ImGui::EndDisabled();
        Tip(bar.empty() ? I18n::Tr("skills_from_bar_empty_tip") : I18n::Tr("skills_from_bar_tip"));
        if (!g_barResult.empty()) Hint(ui::Text(g_barResult).c_str());
    }
    bool flaskRuleOn = false;
    for (const auto& r : rules) flaskRuleOn = flaskRuleOn || (r.enabled && r.kind == RuleKind::Flask);
    if (flaskRuleOn) ui::Warning(I18n::Tr("skills_flask_warning"));
    ImGui::EndDisabled();
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Rule editor --------------------------------------------------------------
    if (g_selected >= 0 && g_selected < static_cast<int>(rules.size())) {
        SkillRule& r = rules[static_cast<size_t>(g_selected)];
        const AimKind aim = AimOf(r);
        ImGui::PushID(g_selected);
        ImGui::BeginDisabled(!sk.enabled);

        // Basics.
        PluginUi::BeginCard("##ruleBasics");
        const std::string title =
            I18n::TrF("skills_rule_title", {{"number", Num("%d", g_selected + 1)}, {"label", ui::RuleLabel(r.label)}});
        Heading(ICON_FA_PEN_TO_SQUARE, title.c_str(), I18n::Tr("skills_rule_title_help"));
        if (v && static_cast<size_t>(g_selected) < v->ruleWhyNot.size()) {
            const LocText& st = v->ruleWhyNot[static_cast<size_t>(g_selected)];
            ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(st));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                I18n::TrF("skills_now", {{"status", st.empty() ? std::string("-") : ui::Text(st)}}).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        changed |= Check(I18n::Tr("skills_enabled"), "ruleOn", &r.enabled, nullptr);
        {
            const LabelColumn col({I18n::Tr("skills_label"), I18n::Tr("skills_col_key"), I18n::Tr("skills_col_kind"),
                                   I18n::Tr("skills_target"), I18n::Tr("skills_skill"), I18n::Tr("skills_interval"),
                                   I18n::Tr("skills_press"), I18n::Tr("skills_hold_for"), I18n::Tr("skills_channel_up_to")});
            col.Row(I18n::Tr("skills_label"));
            if (PluginUi::InputString("##label", I18n::Tr("skills_label_hint"), r.label)) {
                TruncateUtf8(r.label, 64);
                changed = true;
            }
            col.RowNoWidth(I18n::Tr("skills_col_key"));
            if (PluginUi::HotkeyCapture("##key", &r.key, I18n::Tr("skills_key_tip"), true)) changed = true;
            ImGui::SameLine();
            if (PluginUi::SubtleButton(ICON_FA_COMPUTER_MOUSE "###mouseKey")) ImGui::OpenPopup("##mousePick");
            Tip(I18n::Tr("skills_mouse_tip"));
            if (ImGui::BeginPopup("##mousePick")) {
                for (const auto& mb : kMouseButtons) {
                    if (ImGui::Selectable(I18n::TrId(mb.key), r.key == mb.vk)) {
                        r.key = mb.vk;
                        changed = true;
                    }
                }
                ImGui::EndPopup();
            }
            if (const char* role = ReservedKeyRole(r.key, m_settings, prof)) {
                ui::Warning(I18n::TrF("skills_reserved_warning", {{"role", I18n::Tr(role)}}).c_str());
            } else if (r.key == 0x01) {
                Hint(I18n::Tr("skills_left_mouse_hint"));
            }

            col.Row(I18n::Tr("skills_col_kind"));
            int kind = static_cast<int>(r.kind);
            const std::vector<const char*> kinds = ui::TrAll(kKindKeys);
            if (PluginUi::Combo("##kind", kind, kinds.data(), static_cast<int>(kinds.size()))) {
                r.kind = static_cast<RuleKind>(kind);
                r.target = NaturalTarget(r.kind);
                if (r.kind == RuleKind::Flask || r.kind == RuleKind::Movement) r.minEnemies = 0;
                if (r.kind == RuleKind::Flask && r.flaskSlot < 0) r.flaskSlot = 0;
                changed = true;
            }
            Tip(I18n::Tr("skills_kind_tip"));

            col.Row(I18n::Tr("skills_target"));
            if (r.kind == RuleKind::Attack) {
                int t = static_cast<int>(r.target);
                const char* names[7];
                for (int i = 0; i < 7; ++i) names[i] = I18n::Tr(TargetModeName(static_cast<TargetMode>(i)));
                if (PluginUi::Combo("##target", t, names, 7)) {
                    r.target = static_cast<TargetMode>(t);
                    changed = true;
                }
                Tip(I18n::Tr("skills_target_tip"));
            } else if (r.kind == RuleKind::Self || r.kind == RuleKind::LeaderBuff) {
                const TargetMode first = r.kind == RuleKind::Self ? TargetMode::Self : TargetMode::Leader;
                int t = r.target == TargetMode::CursorUnchanged ? 1 : 0;
                const char* names[2] = {r.kind == RuleKind::Self ? I18n::Tr("skills_aim_self") : I18n::Tr("skills_aim_leader"),
                                        I18n::Tr("skill_target_cursor")};
                if (PluginUi::Combo("##target2", t, names, 2)) {
                    r.target = t == 1 ? TargetMode::CursorUnchanged : first;
                    changed = true;
                }
                Tip(I18n::Tr("skills_cursor_unchanged_tip"));
            } else {
                ImGui::BeginDisabled(true);
                int dummy = 0;
                const char* names[1] = {r.kind == RuleKind::Movement ? I18n::Tr("skills_aim_path") : I18n::Tr("skills_aim_none")};
                PluginUi::Combo("##targetFixed", dummy, names, 1);
                ImGui::EndDisabled();
                Tip(r.kind == RuleKind::Movement ? I18n::Tr("skills_movement_aim_tip") : I18n::Tr("skills_flask_aim_tip"));
            }

            col.RowNoWidth(I18n::Tr("skills_skill"));
            if (NameField("skill", I18n::Tr("skills_skill_hint"), r.skillName, liveSkills, I18n::Tr("skills_no_skills"),
                          I18n::Tr("skills_skill_tip"))) {
                TruncateUtf8(r.skillName, 128);
                changed = true;
            }

            col.Row(I18n::Tr("skills_interval"));
            {
                const std::string fmt = ui::Units(ms, "%d");
                if (ImGui::SliderInt("##interval", &r.minIntervalMs, 0, 60000, fmt.c_str(),
                                     ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
                    changed = true;
            }
            Tip(I18n::Tr("skills_interval_tip"));

            col.Row(I18n::Tr("skills_press"));
            int press = r.channelMs > 0 ? 2 : (r.holdMs > 0 ? 1 : 0);
            const std::vector<const char*> presses = ui::TrAll(kPressKeys);
            if (PluginUi::Combo("##press", press, presses.data(), static_cast<int>(presses.size()))) {
                if (press == 0) {
                    r.holdMs = 0;
                    r.channelMs = 0;
                } else if (press == 1) {
                    r.channelMs = 0;
                    r.holdMs = (std::max)(r.holdMs, 300);
                } else {
                    r.holdMs = 0;
                    r.channelMs = (std::max)(r.channelMs, 1500);
                }
                changed = true;
            }
            Tip(I18n::Tr("skills_press_tip"));
            ImGui::BeginDisabled(press != 1);
            changed |= ui::IntRow(col, I18n::Tr("skills_hold_for"), "##holdMs", &r.holdMs, press == 1 ? 10 : 0, 5000, ms,
                                  I18n::Tr("skills_hold_for_tip"));
            ImGui::EndDisabled();
            ImGui::BeginDisabled(press != 2);
            changed |= ui::IntRow(col, I18n::Tr("skills_channel_up_to"), "##channelMs", &r.channelMs, press == 2 ? 150 : 0, 10000,
                                  ms, I18n::Tr("skills_channel_up_to_tip"));
            ImGui::EndDisabled();
        }
        const bool longPress = IsLongPress(r);
        if (longPress) {
            ImGui::BeginDisabled(true);
            bool forced = true;
            Check(I18n::Tr("skills_stop_to_cast"), "stopForced", &forced, I18n::Tr("skills_stop_forced_tip"));
            ImGui::EndDisabled();
        } else {
            changed |= Check(I18n::Tr("skills_stop_to_cast"), "stopToCast", &r.stopToCast, I18n::Tr("skills_stop_to_cast_tip"));
        }
        PluginUi::EndCard();
        ImGui::Spacing();

        // Conditions.
        PluginUi::BeginCard("##ruleConds");
        Heading(ICON_FA_FILTER, I18n::Tr("skills_when_fires"), I18n::Tr("skills_when_fires_help"));
        {
            const LabelColumn col({I18n::Tr("skills_cond_combat"), I18n::Tr("skills_cond_movement")});
            col.Row(I18n::Tr("skills_cond_combat"));
            int when = static_cast<int>(r.when);
            const std::vector<const char*> whens = ui::TrAll(kWhenKeys);
            if (PluginUi::Combo("##when", when, whens.data(), static_cast<int>(whens.size()))) {
                r.when = static_cast<CombatWhen>(when);
                changed = true;
            }
            Tip(I18n::Tr("skills_cond_combat_tip"));
            col.Row(I18n::Tr("skills_cond_movement"));
            int mv = static_cast<int>(r.moveState);
            const std::vector<const char*> moves = ui::TrAll(kMoveKeys);
            if (PluginUi::Combo("##moveState", mv, moves.data(), static_cast<int>(moves.size()))) {
                r.moveState = static_cast<MoveState>(mv);
                changed = true;
            }
            Tip(I18n::Tr("skills_cond_movement_tip"));
        }

        if (PluginUi::Disclosure(I18n::Tr("skills_enemies"), "secEnemies", true)) {
            ImGui::Indent();
            const LabelColumn col({I18n::Tr("skills_at_least"), I18n::Tr("skills_within"), I18n::Tr("skills_min_rarity"),
                                   I18n::Tr("skills_target_range")});
            changed |= ui::IntRow(col, I18n::Tr("skills_at_least"), "##minEnemies", &r.minEnemies, 0, 20,
                                  r.minEnemies == 0   ? I18n::Key("skills_enemies_none")
                                  : r.minEnemies == 1 ? I18n::Key("skills_enemies_one")
                                                      : I18n::Key("skills_enemies_count"),
                                  aim == AimKind::Monster ? I18n::Tr("skills_enemies_target_tip")
                                                          : I18n::Tr("skills_enemies_self_tip"));
            ImGui::BeginDisabled(r.minEnemies == 0);
            changed |= ui::FloatRow(col, I18n::Tr("skills_within"), "##enemyR", &r.enemyRadius, 1.f, 150.f, cells, "%.0f",
                                    I18n::Tr("skills_enemy_radius_tip"));
            ImGui::EndDisabled();
            ImGui::BeginDisabled(aim != AimKind::Monster && r.minEnemies == 0);
            col.Row(I18n::Tr("skills_min_rarity"));
            const std::vector<const char*> rarities = ui::TrAll(kRarityKeys);
            if (PluginUi::Combo("##minRarity", r.minRarity, rarities.data(), static_cast<int>(rarities.size()))) changed = true;
            Tip(I18n::Tr("skills_min_rarity_tip"));
            ImGui::EndDisabled();
            ImGui::BeginDisabled(aim != AimKind::Monster);
            changed |= ui::FloatRow(col, I18n::Tr("skills_target_range"), "##maxDist", &r.maxTargetDistance, 1.f, 150.f, cells,
                                    "%.0f", I18n::Tr("skills_target_range_tip"));
            changed |= Check(I18n::Tr("skills_los"), "los", &r.requireLos, I18n::Tr("skills_los_tip"));
            ImGui::EndDisabled();
            ImGui::Unindent();
        }

        const bool vitalsOn = r.selfHpBelow >= 0.f || r.selfEsBelow >= 0.f || r.selfManaAbove >= 0.f ||
                              r.selfManaBelow >= 0.f || r.leaderHpBelow >= 0.f;
        if (PluginUi::Disclosure(I18n::Tr("skills_vitals"), "secVitals", vitalsOn)) {
            ImGui::Indent();
            changed |= ui::OptionalPercent(I18n::Tr("skills_hp_below"), "hpBelow", &r.selfHpBelow, I18n::Tr("skills_hp_below_tip"));
            changed |= ui::OptionalPercent(I18n::Tr("skills_es_below"), "esBelow", &r.selfEsBelow, I18n::Tr("skills_es_below_tip"));
            changed |= ui::OptionalPercent(I18n::Tr("skills_mana_above"), "mpAbove", &r.selfManaAbove,
                                           I18n::Tr("skills_mana_above_tip"));
            changed |= ui::OptionalPercent(I18n::Tr("skills_mana_below"), "mpBelow", &r.selfManaBelow,
                                           I18n::Tr("skills_mana_below_tip"));
            changed |= ui::OptionalPercent(I18n::Tr("skills_leader_hp"), "leaderHp", &r.leaderHpBelow,
                                           I18n::Tr("skills_leader_hp_tip"));
            ImGui::Unindent();
        }

        const bool buffsOn = !r.buffMissing.empty() || !r.buffPresent.empty() || !r.leaderBuffMissing.empty();
        if (PluginUi::Disclosure(I18n::Tr("skills_buffs"), "secBuffs", buffsOn)) {
            ImGui::Indent();
            const LabelColumn col({I18n::Tr("skills_buff_missing"), I18n::Tr("skills_buff_present"), I18n::Tr("skills_min_stacks"),
                                   I18n::Tr("skills_leader_buff")});
            col.RowNoWidth(I18n::Tr("skills_buff_missing"));
            if (NameField("buffMissing", I18n::Tr("skills_buff_hint"), r.buffMissing, liveSelfBuffs, I18n::Tr("skills_no_buffs"),
                          I18n::Tr("skills_buff_missing_tip"))) {
                TruncateUtf8(r.buffMissing, 128);
                changed = true;
            }
            col.RowNoWidth(I18n::Tr("skills_buff_present"));
            if (NameField("buffPresent", I18n::Tr("skills_buff_hint"), r.buffPresent, liveSelfBuffs, I18n::Tr("skills_no_buffs"),
                          I18n::Tr("skills_buff_present_tip"))) {
                TruncateUtf8(r.buffPresent, 128);
                changed = true;
            }
            ImGui::BeginDisabled(TrimView(r.buffPresent).empty());
            changed |= ui::IntRow(col, I18n::Tr("skills_min_stacks"), "##minStacks", &r.buffMinStacks, 0, 100,
                                  r.buffMinStacks == 0 ? I18n::Key("skills_stacks_any") : I18n::Key("skills_stacks_count"),
                                  I18n::Tr("skills_min_stacks_tip"));
            ImGui::EndDisabled();
            col.RowNoWidth(I18n::Tr("skills_leader_buff"));
            if (NameField("leaderBuff", I18n::Tr("skills_buff_hint"), r.leaderBuffMissing, liveLeaderBuffs,
                          I18n::Tr("skills_no_leader_buffs"), I18n::Tr("skills_leader_buff_tip"))) {
                TruncateUtf8(r.leaderBuffMissing, 128);
                changed = true;
            }
            ImGui::Unindent();
        }

        const bool leaderOn = r.leaderDistanceMin >= 0.f || r.leaderDistanceMax >= 0.f;
        if (PluginUi::Disclosure(I18n::Tr("skills_leader_distance"), "secLeader", leaderOn)) {
            ImGui::Indent();
            changed |= OptionalDistance(I18n::Tr("skills_at_least"), "ldMin", &r.leaderDistanceMin, 30.f,
                                        I18n::Tr("skills_distance_min_tip"));
            changed |= OptionalDistance(I18n::Tr("skills_at_most"), "ldMax", &r.leaderDistanceMax, 40.f,
                                        I18n::Tr("skills_distance_max_tip"));
            if (r.leaderDistanceMin >= 0.f && r.leaderDistanceMax >= 0.f && r.leaderDistanceMin > r.leaderDistanceMax)
                ui::Warning(I18n::Tr("skills_distance_conflict"));
            ImGui::Unindent();
        }
        PluginUi::EndCard();
        ImGui::Spacing();

        // Flask.
        PluginUi::BeginCard("##ruleFlask");
        Heading(ICON_FA_FLASK, I18n::Tr("skills_kind_flask"), I18n::Tr("skills_flask_help"));
        if (r.kind != RuleKind::Flask) Hint(I18n::Tr("skills_flask_only"));
        ImGui::BeginDisabled(r.kind != RuleKind::Flask);
        {
            const LabelColumn col({I18n::Tr("skills_belt_slot")});
            col.Row(I18n::Tr("skills_belt_slot"));
            int slot = std::clamp(r.flaskSlot + 1, 0, 5);
            const std::vector<const char*> belt = ui::TrAll(kBeltKeys);
            if (PluginUi::Combo("##slot", slot, belt.data(), static_cast<int>(belt.size()))) {
                r.flaskSlot = slot - 1;
                changed = true;
            }
            Tip(I18n::Tr("skills_belt_slot_tip"));
        }
        ImGui::BeginDisabled(r.flaskSlot < 0);
        changed |= Check(I18n::Tr("skills_skip_active"), "skipActive", &r.flaskSkipIfActive, I18n::Tr("skills_skip_active_tip"));
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        PluginUi::EndCard();
        ImGui::Spacing();

        ImGui::EndDisabled();
        ImGui::PopID();
    } else if (!rules.empty()) {
        Hint(I18n::Tr("skills_select_rule"));
        ImGui::Spacing();
    }

    // --- Diagnostics -------------------------------------------------------------------
    if (PluginUi::BeginDiagnostics(I18n::Tr("skills_diag_tip"), "skillsDiag")) {
        if (!v) {
            Hint(I18n::Tr("common_no_data_enabled"));
        } else {
            Hint(ui::Join({I18n::TrF("skills_diag_last_rule", {{"rule", v->currentRule.empty() ? I18n::Tr("skills_none_yet")
                                                                                               : ui::RuleLabel(v->currentRule)}}),
                           I18n::TrF("skills_diag_hostiles", {{"count", Num("%d", v->monsters)}}),
                           v->running ? std::string(I18n::Tr("skills_diag_running")) : std::string(I18n::Tr("skills_diag_stopped"))})
                     .c_str());
            if (PluginUi::Disclosure(I18n::Tr("skills_diag_why"), "diagWhy", true)) {
                if (rules.empty()) Hint(I18n::Tr("skills_no_rules"));
                for (size_t i = 0; i < rules.size(); ++i) {
                    const LocText st = i < v->ruleWhyNot.size() ? v->ruleWhyNot[i] : Tr("skills_not_evaluated");
                    const std::string line = I18n::TrF("skills_diag_rule_line", {{"number", Num("%d", static_cast<int>(i) + 1)},
                                                                                 {"label", ui::RuleLabel(rules[i].label)},
                                                                                 {"status", ui::Text(st)}});
                    ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(st));
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(line.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            }
            if (PluginUi::Disclosure(I18n::Tr("skills_diag_skills"), "diagSkills", true)) {
                if (liveSkills.empty()) {
                    Hint(I18n::Tr("skills_no_skills"));
                } else if (ImGui::BeginTable("##liveSkills", 5,
                                             ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                    ImGui::TableSetupColumn(I18n::TrId("skills_col_skill_copy"), ImGuiTableColumnFlags_WidthStretch, 4.0f);
                    ImGui::TableSetupColumn(I18n::TrId("skills_col_bar_slot"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn(I18n::TrId("skills_col_ready"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn(I18n::TrId("skills_col_cooldown"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
                    ImGui::TableSetupColumn(I18n::TrId("skills_col_uses"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < liveSkills.size(); ++i) {
                        const SkillInfo& s = liveSkills[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(s.name.empty() ? I18n::Tr("skills_unnamed") : s.name.c_str(), false) && !s.name.empty())
                            ImGui::SetClipboardText(s.name.c_str());
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        if (s.onBar && s.barSlot >= 0) ImGui::Text("%d", s.barSlot + 1);
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (s.canBeUsed) ImGui::TextColored(PluginUi::Colors().Success, "%s", I18n::Tr("common_yes"));
                        else ImGui::TextColored(PluginUi::Colors().Warning, "%s", I18n::Tr("common_no"));
                        ImGui::TableNextColumn();
                        if (s.totalCooldownMs > 0)
                            ImGui::TextUnformatted(
                                I18n::TrF("skills_seconds", {{"seconds", Num("%.1f", static_cast<float>(s.totalCooldownMs) / 1000.f)}})
                                    .c_str());
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (s.maxUses > 0) ImGui::Text("%d / %d", (std::max)(0, s.maxUses - s.activeCooldowns), s.maxUses);
                        else ImGui::TextDisabled("-");
                    }
                    ImGui::EndTable();
                }
                if (!liveSkills.empty() &&
                    PluginUi::SubtleButton(ui::IconLabel(ICON_FA_COPY, I18n::Tr("skills_copy_all"), "copySkills").c_str())) {
                    std::string all;
                    for (const auto& s : liveSkills) {
                        if (s.name.empty()) continue;
                        if (!all.empty()) all += '\n';
                        all += s.name;
                    }
                    ImGui::SetClipboardText(all.c_str());
                }
                Hint(I18n::Tr("skills_cooldown_hint"));
            }
            if (PluginUi::Disclosure(I18n::Tr("skills_diag_buffs"), "diagSelfBuffs", true))
                BuffTable("selfBuffs", liveSelfBuffs, I18n::Tr("skills_no_buffs_active"));
            if (PluginUi::Disclosure(I18n::Tr("skills_diag_leader_buffs"), "diagLeaderBuffs", false))
                BuffTable("leaderBuffs", liveLeaderBuffs, I18n::Tr("skills_no_leader_buffs_active"));
            Hint(I18n::Tr("skills_read_hint"));
        }
        PluginUi::EndDiagnostics();
    }

    ImGui::PopID();
    if (changed) MarkDirty();
}
