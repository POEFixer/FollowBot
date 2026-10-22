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
std::string              g_barResult;        // result line of "Add rules from the skill bar"

constexpr size_t kMaxRules = 64;

const char* const kKindNames[] = {"Attack", "Self buff", "Leader buff", "Movement", "Flask"};
const char* const kWhenNames[] = {"Always", "In combat", "Out of combat"};
const char* const kMoveNames[] = {"Any", "Moving", "Standing still"};
const char* const kRarityNames[] = {"Normal", "Magic", "Rare", "Unique"};
const char* const kModeNames[] = {"Passive", "Assist", "Aggressive"};
const char* const kPressNames[] = {"Tap", "Hold", "Channel"};
const char* const kBeltNames[] = {"Not checked", "Flask 1", "Flask 2", "Charm 1", "Charm 2", "Charm 3"};

struct MouseButtonChoice {
    int         vk;
    const char* name;
};
const MouseButtonChoice kMouseButtons[] = {
    {0x01, "Left mouse button"}, {0x02, "Right mouse button"}, {0x04, "Middle mouse button"},
    {0x05, "Mouse button 4"},    {0x06, "Mouse button 5"},
};

// The keys a rule may not use, with what they are (the engine refuses them too).
const char* ReservedKeyRole(int vk, const Settings& s, const Profile& p) {
    if (vk <= 0) return nullptr;
    const HotkeySettings& h = s.global.hotkeys;
    if (vk == h.startStop) return "Start / stop hotkey";
    if (vk == h.pause) return "Pause hotkey";
    if (vk == h.holdOverride) return "hold-to-override key";
    if (vk == h.stashNow) return "Stash now hotkey";
    const int move = p.follow.clickToMove ? 0x01 : p.follow.moveKey;
    if (vk == move) return p.follow.clickToMove ? "move button (click to move)" : "move key";
    if (vk == p.follow.sprintKey) return "Dodge / Sprint key";
    return nullptr;
}

// Places the next item on this line when `width` still fits, else on a new one.
void SameLineIfFits(float width) {
    // After an item the cursor sits at the start of the next line: its right
    // edge is the content region's right edge.
    const float regionRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= regionRight) ImGui::SameLine();
}

ImVec4 StatusColor(const std::string& s) {
    const PluginUi::Palette pal = PluginUi::Colors();
    if (s.rfind("ready", 0) == 0 || s.rfind("fired", 0) == 0 || s.rfind("holding", 0) == 0) return pal.Success;
    if (s == "off" || s.empty()) return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
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
        if (ImGui::SliderFloat(sid, v, 0.f, 300.f, "%.0f cells", ImGuiSliderFlags_AlwaysClamp)) changed = true;
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
    const float pickW = PluginUi::ButtonWidth("Pick");
    const float avail = ImGui::GetContentRegionAvail().x;
    const float em = ImGui::GetFontSize();
    const float fieldW = (std::max)(em * 6.0f, (std::min)(avail - pickW - ImGui::GetStyle().ItemSpacing.x, em * 15.0f));
    ImGui::SetNextItemWidth(fieldW);
    if (PluginUi::InputString("##name", hint, text)) changed = true;
    Tip(tip);
    ImGui::SameLine();
    if (PluginUi::SubtleButton("Pick###pick")) ImGui::OpenPopup("##pickPopup");
    Tip("Pick a name the bot reads from the game right now.");
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
            if (ImGui::Selectable("Clear###clearName")) {
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
        ImGui::TableSetupColumn("Buff (click to copy)", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Stacks", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Time left", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < buffs.size(); ++i) {
            const BuffInfo& b = buffs[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(b.name.empty() ? "(unnamed)" : b.name.c_str(), false) && !b.name.empty())
                ImGui::SetClipboardText(b.name.c_str());
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (b.charges > 0) ImGui::Text("%d", b.charges);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (b.totalTime > 0.f && b.timeLeft >= 0.f && b.timeLeft < 1e6f) ImGui::Text("%.1f s", b.timeLeft);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    if (PluginUi::SubtleButton(ICON_FA_COPY "  Copy all###copyAll")) {
        std::string all;
        for (const auto& b : buffs) {
            if (b.name.empty()) continue;
            if (!all.empty()) all += '\n';
            all += b.name;
        }
        ImGui::SetClipboardText(all.c_str());
    }
    Tip("Copies every buff name, one per line.");
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
    ImGui::PushID("skillsTab");

    // --- Combat -------------------------------------------------------------------
    PluginUi::BeginCard("##combat");
    Heading(ICON_FA_SHIELD_HALVED, "Combat",
            "Rules press keys you bind; the game's own key bindings cannot be read.\n"
            "Distances are in grid cells (about a tenth of a character's height each).\n"
            "In combat = a hostile within the combat radius of you and within the leash around the leader.");
    changed |= Check("Use skill rules", "skillsOn", &sk.enabled,
                     "Off: no rule presses anything (flask rules included).");
    ImGui::BeginDisabled(!sk.enabled);
    {
        const LabelColumn col({"Combat mode", "Combat radius", "Leash around the leader", "Attack while leader within",
                               "Min time between casts"});
        col.Row("Combat mode");
        int mode = static_cast<int>(sk.mode);
        if (PluginUi::Combo("##mode", mode, kModeNames, 3)) {
            sk.mode = static_cast<CombatMode>(mode);
            changed = true;
        }
        Tip("Passive: attack rules never fire (buffs, movement and flasks still do).\n"
            "Assist: attack rules only target hostiles inside the leash around the leader.\n"
            "Aggressive: attack rules target any hostile within the combat radius.");
        changed |= ui::FloatRow(col, "Combat radius", "##combatR", &sk.combatRadius, 5.f, 150.f, "%.0f cells",
                                "A hostile this close to you puts the bot in combat. Attack rules never target\n"
                                "farther than this, whatever their own range.");
        changed |= ui::FloatRow(col, "Leash around the leader", "##leashR", &sk.leashRadius, 5.f, 200.f, "%.0f cells",
                                "Only hostiles this close to the leader count for combat (and, in Assist mode,\n"
                                "as targets). Ignored while the leader is not visible.");
        ImGui::BeginDisabled(sk.mode == CombatMode::Passive);
        changed |= ui::FloatRow(col, "Attack while leader within", "##maxLeader", &sk.maxCombatLeaderDistance, 10.f, 300.f,
                                "%.0f cells",
                                "Attack rules pause while the leader is farther than this: following comes first.");
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, "Min time between casts", "##castGate", &sk.globalCastGateMs, 50, 2000, "%d ms",
                              "Shortest gap between any two presses of any rules (plus the general input\n"
                              "gate and its random jitter from Death & Safety).");
    }
    changed |= Check("Skip monsters the game marks untargetable", "skipUntgt", &sk.skipUntargetable,
                     "Monsters that are hidden or cannot be targeted right now (burrowed, invulnerable phases)\n"
                     "are neither aimed at nor counted. Turn off if the bot ignores monsters it should attack.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Never target monsters whose path contains:");
    Tip("One entry per line (or comma separated), compared without case against the monster's\n"
        "metadata path, e.g. \"Totem\" or \"Metadata/Monsters/LeagueAzmeri/\". Blacklisted monsters\n"
        "are not targeted and do not count for combat or enemy counts.");
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
    Heading(ICON_FA_LIST_OL, "Rules",
            "Checked top to bottom every tick: the first rule whose conditions all hold presses its key\n"
            "(list order = priority, put the important ones first). Select a rule to edit it below.\n"
            "The Status column says why a rule does not fire right now (while the bot runs).");
    ImGui::BeginDisabled(!sk.enabled);
    if (rules.empty()) {
        Hint("No rules yet: add one, or add one per skill on your skill bar.");
    } else if (ImGui::BeginTable("##ruleTable", 5,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 3.5f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rules.size(); ++i) {
            SkillRule& r = rules[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &r.enabled)) changed = true;
            Tip("Enable or disable this rule.");
            ImGui::TableNextColumn();
            char name[128];
            std::snprintf(name, sizeof(name), "%d. %s###sel", static_cast<int>(i) + 1,
                          r.label.empty() ? "(no label)" : r.label.c_str());
            if (ImGui::Selectable(name, g_selected == static_cast<int>(i))) g_selected = static_cast<int>(i);
            ImGui::TableNextColumn();
            if (r.key == 0) ImGui::TextColored(PluginUi::Colors().Warning, "None");
            else ImGui::TextUnformatted(PluginUi::KeyName(r.key));
            ImGui::TableNextColumn();
            const int k = static_cast<int>(r.kind);
            ImGui::TextUnformatted(k >= 0 && k < 5 ? kKindNames[k] : "?");
            ImGui::TableNextColumn();
            const std::string status = (v && i < v->ruleWhyNot.size()) ? v->ruleWhyNot[i] : std::string();
            ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(status));
            ImGui::TextUnformatted(status.empty() ? "-" : status.c_str());
            ImGui::PopStyleColor();
            if (!status.empty()) Tip(status.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Buttons.
    const bool haveSel = g_selected >= 0 && g_selected < static_cast<int>(rules.size());
    ImGui::BeginDisabled(rules.size() >= kMaxRules);
    if (ImGui::Button(ICON_FA_PLUS "  Add###addRule")) {
        SkillRule r;
        r.label = "Rule " + std::to_string(rules.size() + 1);
        rules.push_back(r);
        g_selected = static_cast<int>(rules.size()) - 1;
        changed = true;
    }
    Tip("Adds an attack rule at the end (bind its key below).");
    SameLineIfFits(PluginUi::ButtonWidth(ICON_FA_CLONE "  Duplicate"));
    ImGui::BeginDisabled(!haveSel);
    if (PluginUi::SubtleButton(ICON_FA_CLONE "  Duplicate###dupRule") && haveSel) {
        SkillRule copy = rules[static_cast<size_t>(g_selected)];
        copy.label += " copy";
        TruncateUtf8(copy.label, 64);
        rules.insert(rules.begin() + g_selected + 1, copy);
        ++g_selected;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Tip("Inserts a copy of the selected rule below it.");
    SameLineIfFits(PluginUi::ButtonWidth(ICON_FA_ARROW_UP "  Up"));
    ImGui::BeginDisabled(!haveSel || g_selected == 0);
    if (PluginUi::SubtleButton(ICON_FA_ARROW_UP "  Up###upRule") && haveSel && g_selected > 0) {
        std::swap(rules[static_cast<size_t>(g_selected)], rules[static_cast<size_t>(g_selected - 1)]);
        --g_selected;
        changed = true;
    }
    ImGui::EndDisabled();
    Tip("Moves the selected rule up: checked earlier, higher priority.");
    SameLineIfFits(PluginUi::ButtonWidth(ICON_FA_ARROW_DOWN "  Down"));
    ImGui::BeginDisabled(!haveSel || g_selected + 1 >= static_cast<int>(rules.size()));
    if (PluginUi::SubtleButton(ICON_FA_ARROW_DOWN "  Down###downRule") && haveSel &&
        g_selected + 1 < static_cast<int>(rules.size())) {
        std::swap(rules[static_cast<size_t>(g_selected)], rules[static_cast<size_t>(g_selected + 1)]);
        ++g_selected;
        changed = true;
    }
    ImGui::EndDisabled();
    Tip("Moves the selected rule down: checked later, lower priority.");
    SameLineIfFits(PluginUi::ButtonWidth(ICON_FA_TRASH "  Delete"));
    ImGui::BeginDisabled(!haveSel);
    if (PluginUi::SubtleButton(ICON_FA_TRASH "  Delete###delRule")) ImGui::OpenPopup("##confirmDelete");
    ImGui::EndDisabled();
    Tip("Deletes the selected rule (asks first).");
    if (haveSel) {
        char q[160];
        std::snprintf(q, sizeof(q), "Delete rule %d \"%s\"?", g_selected + 1, rules[static_cast<size_t>(g_selected)].label.c_str());
        if (PluginUi::ConfirmPopup("##confirmDelete", q, "Delete", "Cancel")) {
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
        SameLineIfFits(PluginUi::ButtonWidth(ICON_FA_WAND_MAGIC_SPARKLES "  Add rules from the skill bar"));
        ImGui::BeginDisabled(bar.empty() || rules.size() >= kMaxRules);
        if (PluginUi::SubtleButton(ICON_FA_WAND_MAGIC_SPARKLES "  Add rules from the skill bar###fromBar")) {
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
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Added %d rule%s (disabled, no key yet)%s.", added, added == 1 ? "" : "s",
                          skipped ? "; skills that already have a rule were skipped" : "");
            g_barResult = buf;
            if (added > 0) {
                g_selected = static_cast<int>(rules.size()) - added;
                changed = true;
            }
        }
        ImGui::EndDisabled();
        Tip(bar.empty() ? "No skills read yet: open this tab while you are in the game (the bot reads the skill bar\n"
                          "while it is open)."
                        : "One disabled attack rule per skill on your bar, with its skill name filled in (the rule\n"
                          "then waits until the skill is ready). Bind each rule's key, check its kind and target, enable it.");
        if (!g_barResult.empty()) Hint(g_barResult.c_str());
    }
    bool flaskRuleOn = false;
    for (const auto& r : rules) flaskRuleOn = flaskRuleOn || (r.enabled && r.kind == RuleKind::Flask);
    if (flaskRuleOn)
        ui::Warning("A flask rule is enabled: if POEFixer's AutoPot drinks the same flasks, turn one of them off.");
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
        char title[128];
        std::snprintf(title, sizeof(title), "Rule %d: %s", g_selected + 1, r.label.empty() ? "(no label)" : r.label.c_str());
        Heading(ICON_FA_PEN_TO_SQUARE, title,
                "What the rule presses and how. The kind decides where the cursor goes:\n"
                "Attack aims at the target below, Self buff at your character, Leader buff at the leader,\n"
                "Movement 25 cells ahead on the path (or toward the leader), Flask does not move the cursor.");
        if (v && static_cast<size_t>(g_selected) < v->ruleWhyNot.size()) {
            const std::string& st = v->ruleWhyNot[static_cast<size_t>(g_selected)];
            ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(st));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("Now: %s", st.empty() ? "-" : st.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        changed |= Check("Enabled", "ruleOn", &r.enabled, nullptr);
        {
            const LabelColumn col({"Label", "Key", "Kind", "Target", "Skill", "Own interval", "Press", "Hold for",
                                   "Channel up to"});
            col.Row("Label");
            if (PluginUi::InputString("##label", "name shown in the list", r.label)) {
                TruncateUtf8(r.label, 64);
                changed = true;
            }
            col.RowNoWidth("Key");
            if (PluginUi::HotkeyCapture("##key", &r.key,
                                        "The key (or mouse button) bound to this skill in the game.\n"
                                        "Click, then press the key; right / middle / side mouse buttons work here too.",
                                        true))
                changed = true;
            ImGui::SameLine();
            if (PluginUi::SubtleButton(ICON_FA_COMPUTER_MOUSE "###mouseKey")) ImGui::OpenPopup("##mousePick");
            Tip("Pick a mouse button (including the left button, which cannot be captured by clicking).\n"
                "Mouse-button skills are pressed at the aimed point.");
            if (ImGui::BeginPopup("##mousePick")) {
                for (const auto& mb : kMouseButtons) {
                    if (ImGui::Selectable(mb.name, r.key == mb.vk)) {
                        r.key = mb.vk;
                        changed = true;
                    }
                }
                ImGui::EndPopup();
            }
            if (const char* role = ReservedKeyRole(r.key, m_settings, prof)) {
                char w[160];
                std::snprintf(w, sizeof(w), "This key is the bot's %s: the rule never fires. Bind another key.", role);
                ui::Warning(w);
            } else if (r.key == 0x01) {
                Hint("Left mouse button: in the game it also moves / interacts; make sure the skill is bound to it.");
            }

            col.Row("Kind");
            int kind = static_cast<int>(r.kind);
            if (PluginUi::Combo("##kind", kind, kKindNames, 5)) {
                r.kind = static_cast<RuleKind>(kind);
                r.target = NaturalTarget(r.kind);
                if (r.kind == RuleKind::Flask || r.kind == RuleKind::Movement) r.minEnemies = 0;
                if (r.kind == RuleKind::Flask && r.flaskSlot < 0) r.flaskSlot = 0;
                changed = true;
            }
            Tip("Attack: aimed at a target, follows the combat mode.\n"
                "Self buff: a skill cast on yourself (guards, auras, warcries).\n"
                "Leader buff: a skill cast on the leader (links, heals).\n"
                "Movement: a dash / blink toward where the bot is going.\n"
                "Flask: a flask or charm key; can check the belt slot's charges.");

            col.Row("Target");
            if (r.kind == RuleKind::Attack) {
                int t = static_cast<int>(r.target);
                const char* names[7];
                for (int i = 0; i < 7; ++i) names[i] = TargetModeName(static_cast<TargetMode>(i));
                if (PluginUi::Combo("##target", t, names, 7)) {
                    r.target = static_cast<TargetMode>(t);
                    changed = true;
                }
                Tip("Nearest / Strongest (rarity, then life) / Lowest life / Densest cluster (most hostiles\n"
                    "within 12 cells): a hostile on screen. Leader / Self: aim there. Cursor unchanged: press\n"
                    "without moving the cursor.");
            } else if (r.kind == RuleKind::Self || r.kind == RuleKind::LeaderBuff) {
                const TargetMode first = r.kind == RuleKind::Self ? TargetMode::Self : TargetMode::Leader;
                int t = r.target == TargetMode::CursorUnchanged ? 1 : 0;
                const char* names[2] = {r.kind == RuleKind::Self ? "Aim at yourself" : "Aim at the leader", "Cursor unchanged"};
                if (PluginUi::Combo("##target2", t, names, 2)) {
                    r.target = t == 1 ? TargetMode::CursorUnchanged : first;
                    changed = true;
                }
                Tip("Cursor unchanged: press without moving the cursor (for skills that need no aim).");
            } else {
                ImGui::BeginDisabled(true);
                int dummy = 0;
                const char* names[1] = {r.kind == RuleKind::Movement ? "25 cells ahead on the path" : "No aim (key only)"};
                PluginUi::Combo("##targetFixed", dummy, names, 1);
                ImGui::EndDisabled();
                Tip(r.kind == RuleKind::Movement ? "Movement rules always aim ahead on the path, or toward the leader."
                                                 : "Flask rules never move the cursor.");
            }

            col.RowNoWidth("Skill");
            if (NameField("skill", "any (no readiness check)", r.skillName, liveSkills,
                          "No skills read yet (in the game, with this tab open).",
                          "Optional: the skill's id as the game names it (see Diagnostics). When set, the rule\n"
                          "waits until the skill is ready (not on cooldown). It does not choose the key.")) {
                TruncateUtf8(r.skillName, 128);
                changed = true;
            }

            col.Row("Own interval");
            if (ImGui::SliderInt("##interval", &r.minIntervalMs, 0, 60000, "%d ms",
                                 ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
                changed = true;
            Tip("This rule fires at most once per this interval (its own cooldown).\n"
                "Ctrl+click to type a value.");

            col.Row("Press");
            int press = r.channelMs > 0 ? 2 : (r.holdMs > 0 ? 1 : 0);
            if (PluginUi::Combo("##press", press, kPressNames, 3)) {
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
            Tip("Tap: a short press.\n"
                "Hold: keep the key down for a fixed time, aimed once (charge-up skills).\n"
                "Channel: keep the key down up to the time, re-aiming at the target and letting go\n"
                "early when it is gone (channelled skills). Holds and channels stand still and only\n"
                "start while the bot follows (not during a pickup, travel or stash run).");
            ImGui::BeginDisabled(press != 1);
            changed |= ui::IntRow(col, "Hold for", "##holdMs", &r.holdMs, press == 1 ? 10 : 0, 5000, "%d ms",
                                  "How long the key stays down.");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(press != 2);
            changed |= ui::IntRow(col, "Channel up to", "##channelMs", &r.channelMs, press == 2 ? 150 : 0, 10000, "%d ms",
                                  "Longest channel; it ends earlier when the target is gone.");
            ImGui::EndDisabled();
        }
        const bool longPress = IsLongPress(r);
        if (longPress) {
            ImGui::BeginDisabled(true);
            bool forced = true;
            Check("Stop moving to cast", "stopForced", &forced, "Holds and channels always stand still.");
            ImGui::EndDisabled();
        } else {
            changed |= Check("Stop moving to cast", "stopToCast", &r.stopToCast,
                             "Release the move key before pressing (the character stops for the cast).\n"
                             "Off: it keeps walking and the cursor returns to the walking direction after the press.");
        }
        PluginUi::EndCard();
        ImGui::Spacing();

        // Conditions.
        PluginUi::BeginCard("##ruleConds");
        Heading(ICON_FA_FILTER, "When it fires",
                "Every condition that is on must hold. The first one that does not is shown as the\n"
                "rule's status. Leave a condition off to ignore it.");
        {
            const LabelColumn col({"Combat", "Movement"});
            col.Row("Combat");
            int when = static_cast<int>(r.when);
            if (PluginUi::Combo("##when", when, kWhenNames, 3)) {
                r.when = static_cast<CombatWhen>(when);
                changed = true;
            }
            Tip("In combat = a hostile within the combat radius of you and the leash of the leader (Combat card).");
            col.Row("Movement");
            int mv = static_cast<int>(r.moveState);
            if (PluginUi::Combo("##moveState", mv, kMoveNames, 3)) {
                r.moveState = static_cast<MoveState>(mv);
                changed = true;
            }
            Tip("Moving / standing still, from the character's own motion in the last ~0.3 s.");
        }

        if (PluginUi::Disclosure("Enemies", "secEnemies", true)) {
            ImGui::Indent();
            const LabelColumn col({"At least", "Within", "Minimum rarity", "Target range"});
            changed |= ui::IntRow(col, "At least", "##minEnemies", &r.minEnemies, 0, 20, r.minEnemies == 0 ? "no requirement" : (r.minEnemies == 1 ? "1 enemy" : "%d enemies"),
                                  aim == AimKind::Monster
                                      ? "Hostiles within the radius below around the TARGET (the target counts).\n0 = no requirement."
                                      : "Hostiles of at least the minimum rarity within the radius below around YOU.\n0 = no requirement.");
            ImGui::BeginDisabled(r.minEnemies == 0);
            changed |= ui::FloatRow(col, "Within", "##enemyR", &r.enemyRadius, 1.f, 150.f, "%.0f cells",
                                    "Radius of the enemy count above.");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(aim != AimKind::Monster && r.minEnemies == 0);
            col.Row("Minimum rarity");
            if (PluginUi::Combo("##minRarity", r.minRarity, kRarityNames, 4)) changed = true;
            Tip("Attack rules: the target's rarity. Other rules: the rarity of the counted enemies.");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(aim != AimKind::Monster);
            changed |= ui::FloatRow(col, "Target range", "##maxDist", &r.maxTargetDistance, 1.f, 150.f, "%.0f cells",
                                    "Farthest target from you (never beyond the combat radius).");
            changed |= Check("Only targets in line of sight", "los", &r.requireLos,
                             "Skips targets behind walls (judged on the walkable grid).");
            ImGui::EndDisabled();
            ImGui::Unindent();
        }

        const bool vitalsOn = r.selfHpBelow >= 0.f || r.selfEsBelow >= 0.f || r.selfManaAbove >= 0.f ||
                              r.selfManaBelow >= 0.f || r.leaderHpBelow >= 0.f;
        if (PluginUi::Disclosure("Life, energy shield and mana", "secVitals", vitalsOn)) {
            ImGui::Indent();
            changed |= ui::OptionalPercent("Own life below", "hpBelow", &r.selfHpBelow, "Fires only while your life is below this.");
            changed |= ui::OptionalPercent("Own energy shield below", "esBelow", &r.selfEsBelow,
                                           "Fires only while your energy shield is below this (never without energy shield).");
            changed |= ui::OptionalPercent("Own mana above", "mpAbove", &r.selfManaAbove,
                                           "Fires only while your mana is above this (keep mana for other skills).");
            changed |= ui::OptionalPercent("Own mana below", "mpBelow", &r.selfManaBelow,
                                           "Fires only while your mana is below this (mana flasks).");
            changed |= ui::OptionalPercent("Leader's life below", "leaderHp", &r.leaderHpBelow,
                                           "Fires only while the leader is visible and his life is below this (heals).");
            ImGui::Unindent();
        }

        const bool buffsOn = !r.buffMissing.empty() || !r.buffPresent.empty() || !r.leaderBuffMissing.empty();
        if (PluginUi::Disclosure("Buffs", "secBuffs", buffsOn)) {
            ImGui::Indent();
            const LabelColumn col({"Only while you lack", "Only while you have", "With at least", "Only while the leader lacks"});
            col.RowNoWidth("Only while you lack");
            if (NameField("buffMissing", "buff id (empty = off)", r.buffMissing, liveSelfBuffs,
                          "No buffs read yet (in the game, with this tab open).",
                          "Fires only while this buff is NOT on you: keep a buff up (e.g. a guard or an aura).\n"
                          "Use the game's buff id: pick it or copy it from Diagnostics.")) {
                TruncateUtf8(r.buffMissing, 128);
                changed = true;
            }
            col.RowNoWidth("Only while you have");
            if (NameField("buffPresent", "buff id (empty = off)", r.buffPresent, liveSelfBuffs,
                          "No buffs read yet (in the game, with this tab open).",
                          "Fires only while this buff IS on you (e.g. a charge or a resource buff).")) {
                TruncateUtf8(r.buffPresent, 128);
                changed = true;
            }
            ImGui::BeginDisabled(TrimView(r.buffPresent).empty());
            changed |= ui::IntRow(col, "With at least", "##minStacks", &r.buffMinStacks, 0, 100,
                                  r.buffMinStacks == 0 ? "any stacks" : "%d stacks",
                                  "Stacks the buff above needs (e.g. 5 for a banner's resource). 0 = any.");
            ImGui::EndDisabled();
            col.RowNoWidth("Only while the leader lacks");
            if (NameField("leaderBuff", "buff id (empty = off)", r.leaderBuffMissing, liveLeaderBuffs,
                          "No leader buffs read yet (leader visible, this tab open).",
                          "Fires only while the leader is visible and this buff is NOT on him (link skills, buffs\n"
                          "you cast on him).")) {
                TruncateUtf8(r.leaderBuffMissing, 128);
                changed = true;
            }
            ImGui::Unindent();
        }

        const bool leaderOn = r.leaderDistanceMin >= 0.f || r.leaderDistanceMax >= 0.f;
        if (PluginUi::Disclosure("Distance to the leader", "secLeader", leaderOn)) {
            ImGui::Indent();
            changed |= OptionalDistance("At least", "ldMin", &r.leaderDistanceMin, 30.f,
                                        "Fires only while the leader is at least this far (e.g. a dash to catch up).");
            changed |= OptionalDistance("At most", "ldMax", &r.leaderDistanceMax, 40.f,
                                        "Fires only while the leader is at most this far (e.g. buffs he must be near for).");
            if (r.leaderDistanceMin >= 0.f && r.leaderDistanceMax >= 0.f && r.leaderDistanceMin > r.leaderDistanceMax)
                ui::Warning("\"At least\" is above \"At most\": the rule can never fire.");
            ImGui::Unindent();
        }
        PluginUi::EndCard();
        ImGui::Spacing();

        // Flask.
        PluginUi::BeginCard("##ruleFlask");
        Heading(ICON_FA_FLASK, "Flask",
                "Flask rules can check the belt: the slot must hold enough charges for one use, and the\n"
                "rule can wait while the flask's effect is still running.");
        if (r.kind != RuleKind::Flask) Hint("Only for rules of the Flask kind.");
        ImGui::BeginDisabled(r.kind != RuleKind::Flask);
        {
            const LabelColumn col({"Belt slot"});
            col.Row("Belt slot");
            int slot = std::clamp(r.flaskSlot + 1, 0, 5);
            if (PluginUi::Combo("##slot", slot, kBeltNames, 6)) {
                r.flaskSlot = slot - 1;
                changed = true;
            }
            Tip("Which belt slot this key uses. Not checked: the key is pressed whenever the other\n"
                "conditions hold (charges and the running effect are unknown).");
        }
        ImGui::BeginDisabled(r.flaskSlot < 0);
        changed |= Check("Skip while its effect is running", "skipActive", &r.flaskSkipIfActive,
                         "Waits until the flask's (or charm's) effect ended before using it again.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        PluginUi::EndCard();
        ImGui::Spacing();

        ImGui::EndDisabled();
        ImGui::PopID();
    } else if (!rules.empty()) {
        Hint("Select a rule above to edit it.");
        ImGui::Spacing();
    }

    // --- Diagnostics -------------------------------------------------------------------
    if (PluginUi::BeginDiagnostics("What the skill rules see right now: each rule's status, your skills and buffs.",
                                   "skillsDiag")) {
        if (!v) {
            Hint("No data yet (the bot reads the game while the plugin is enabled).");
        } else {
            char line[256];
            std::snprintf(line, sizeof(line), "Last fired rule: %s   |   hostiles read: %d   |   bot %s",
                          v->currentRule.empty() ? "none yet" : v->currentRule.c_str(), v->monsters,
                          v->running ? "running" : "stopped (statuses are from the last run)");
            Hint(line);
            if (PluginUi::Disclosure("Why each rule does (not) fire", "diagWhy", true)) {
                if (rules.empty()) Hint("No rules.");
                for (size_t i = 0; i < rules.size(); ++i) {
                    const std::string st = i < v->ruleWhyNot.size() ? v->ruleWhyNot[i] : std::string("not evaluated yet");
                    std::snprintf(line, sizeof(line), "%d. %s: %s", static_cast<int>(i) + 1,
                                  rules[i].label.empty() ? "(no label)" : rules[i].label.c_str(), st.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(st));
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(line);
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            }
            if (PluginUi::Disclosure("Your skills", "diagSkills", true)) {
                if (liveSkills.empty()) {
                    Hint("No skills read yet (in the game, with this tab open).");
                } else if (ImGui::BeginTable("##liveSkills", 5,
                                             ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                    ImGui::TableSetupColumn("Skill (click to copy)", ImGuiTableColumnFlags_WidthStretch, 4.0f);
                    ImGui::TableSetupColumn("Bar slot", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("Ready", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("Cooldown", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                    ImGui::TableSetupColumn("Uses", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < liveSkills.size(); ++i) {
                        const SkillInfo& s = liveSkills[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(s.name.empty() ? "(unnamed)" : s.name.c_str(), false) && !s.name.empty())
                            ImGui::SetClipboardText(s.name.c_str());
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        if (s.onBar && s.barSlot >= 0) ImGui::Text("%d", s.barSlot + 1);
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (s.canBeUsed) ImGui::TextColored(PluginUi::Colors().Success, "yes");
                        else ImGui::TextColored(PluginUi::Colors().Warning, "no");
                        ImGui::TableNextColumn();
                        if (s.totalCooldownMs > 0) ImGui::Text("%.1f s", static_cast<float>(s.totalCooldownMs) / 1000.f);
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (s.maxUses > 0) ImGui::Text("%d / %d", (std::max)(0, s.maxUses - s.activeCooldowns), s.maxUses);
                        else ImGui::TextDisabled("-");
                    }
                    ImGui::EndTable();
                }
                if (!liveSkills.empty() && PluginUi::SubtleButton(ICON_FA_COPY "  Copy all###copySkills")) {
                    std::string all;
                    for (const auto& s : liveSkills) {
                        if (s.name.empty()) continue;
                        if (!all.empty()) all += '\n';
                        all += s.name;
                    }
                    ImGui::SetClipboardText(all.c_str());
                }
                Hint("Cooldown = the skill's full cooldown (the time left is not readable). Uses = charges ready / total.");
            }
            if (PluginUi::Disclosure("Your buffs", "diagSelfBuffs", true))
                BuffTable("selfBuffs", liveSelfBuffs, "No buffs read yet, or none active.");
            if (PluginUi::Disclosure("Leader's buffs", "diagLeaderBuffs", false))
                BuffTable("leaderBuffs", liveLeaderBuffs, "No leader buffs read (leader not visible, or none active).");
            Hint("Skills and buffs are read a few times a second while this tab is open.");
        }
        PluginUi::EndDiagnostics();
    }

    ImGui::PopID();
    if (changed) MarkDirty();
}
