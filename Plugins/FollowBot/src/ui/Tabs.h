// Tabs.h - helpers shared by the settings tabs (ui/*.cpp) and the debug tools.
// The tabs themselves are FollowBotPlugin members (DrawTab*), one file each:
// SettingsUi.cpp (tab bar, header, General), UiFollow.cpp (Follow & Movement),
// UiTravel.cpp, UiLoot.cpp, UiSkills.cpp, UiStash.cpp, UiSafety.cpp
// (Death & Safety), UiDebug.cpp, Overlay.cpp (debug overlay + status window),
// DebugTools.cpp (dumps).
//
// Texts follow the program language (PluginI18n.h): every literal is a key of
// lang/en.json drawn through I18n::Tr / TrF / TrId; the worker's texts arrive as
// keys or LocTexts and are translated here, on the render thread (ui::Text).
#pragma once

#include <imgui.h>

#include "../../../../POEFixer/imgui/IconsFontAwesome6.h"
#include "../../../../POEFixer/plugin_sdk/PluginUi.h"
#include "../combat/RuleLabels.h"
#include "../config/Profiles.h"
#include "../core/LocText.h"
#include "../world/World.h"

#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace PluginSDK {
struct Context;
}

namespace I18n = PluginSDK::I18n;

namespace fb::ui {

inline ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// A worker text (a LocText) in the program language.
inline std::string Text(const LocText& t) { return Render(t, &I18n::Tr); }

// A key kept by the worker ("" = none) in the program language.
inline const char* KeyText(const std::string& key) { return key.empty() ? "" : I18n::Tr(key.c_str()); }

// `text` with every '%' doubled: safe inside an ImGui format string.
inline std::string EscapePercent(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (char c : text) {
        out.push_back(c);
        if (c == '%') out.push_back('%');
    }
    return out;
}

// An ImGui slider / drag format from a translated unit text: "{value} cells" with
// `spec` ("%.0f") for {value}; every other '%' escaped. A text without {value}
// is shown as it is ("no requirement").
inline std::string Units(const char* key, const char* spec) {
    const std::string text = EscapePercent(I18n::Tr(key));
    const std::string_view mark = "{value}";
    const size_t at = text.find(mark);
    if (at == std::string::npos) return text;
    return text.substr(0, at) + spec + text.substr(at + mark.size());
}

// Several items of one diagnostics line, joined by a separator drawn by code.
inline std::string Join(std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += "   |   ";
        out += p;
    }
    return out;
}

// "<icon>  <text>###<id>": a button with an icon whose id stays the same in
// every language.
inline std::string IconLabel(const char* icon, const char* text, const char* id) {
    return std::string(icon) + "  " + (text ? text : "") + "###" + id;
}

// A label with a per-item id: "<text>###<id>".
inline std::string WithId(const std::string& text, const std::string& id) { return text + "###" + id; }

// The texts of a table of keys, for a combo (built every frame).
template <size_t N>
inline std::vector<const char*> TrAll(const char* const (&keys)[N]) {
    std::vector<const char*> out;
    out.reserve(N);
    for (const char* k : keys) out.push_back(I18n::Tr(k));
    return out;
}

inline const char* YesNo(bool v) { return v ? I18n::Tr("common_yes") : I18n::Tr("common_no"); }

// A profile name as shown: the names the plugin writes ("Default", New's "Profile",
// either one numbered: "Profile 2") follow the program language while they are
// unchanged (the settings file keeps the English); the user's own names show as typed.
inline std::string ProfileName(const std::string& name) {
    const PluginProfileName p = ParsePluginProfileName(name);
    const std::string number = std::to_string(p.number);
    switch (p.base) {
    case PluginProfileBase::Default:
        return p.number ? I18n::TrF("profile_default_name_n", {{"number", number}})
                        : std::string(I18n::Tr("profile_default_name"));
    case PluginProfileBase::Profile:
        return p.number ? I18n::TrF("profile_name_profile_n", {{"number", number}})
                        : std::string(I18n::Tr("profile_name_profile"));
    default:
        return name;
    }
}

// A skill rule label as shown: the labels the plugin writes (the shipped defaults,
// Add's "Rule 3", Duplicate's "<label> copy") follow the program language while they
// are unchanged (the settings file keeps the English, RuleLabels.h).
inline std::string RuleLabel(const std::string& label) {
    std::string_view original;
    if (CopiedRuleLabel(label, &original))
        return I18n::TrF("skills_copy_label", {{"label", RuleLabel(std::string(original))}});
    if (const int n = NewRuleNumber(label)) return I18n::TrF("skills_new_rule_label", {{"number", std::to_string(n)}});
    if (label == "Life flask") return I18n::Tr("skills_default_life_flask");
    if (label == "Mana flask") return I18n::Tr("skills_default_mana_flask");
    if (label == "New rule") return I18n::Tr("skills_default_new_rule");
    if (label.empty()) return I18n::Tr("skills_no_label");
    return label;
}

// Wrapped text in one colour (warnings, notes).
inline void ColoredWrapped(const ImVec4& color, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text ? text : "");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

inline void Warning(const char* text) { ColoredWrapped(PluginUi::Colors().Warning, text); }

// "Needs POEFixer 365+" style note for a feature the host cannot serve yet.
inline void HostNote(const char* text) { ColoredWrapped(PluginUi::Colors().TextSecondary, text); }

// A float slider row with a stable id; returns true on change. `unitKey` names a
// unit text ("{value} cells"); nullptr shows the bare number with `spec`.
inline bool FloatRow(const PluginUi::LabelColumn& col, const char* label, const char* id, float* v, float lo, float hi,
                     const char* unitKey, const char* spec, const char* tip) {
    const std::string fmt = unitKey ? Units(unitKey, spec) : std::string(spec);
    return PluginUi::SliderFloatRow(col, label, id, v, lo, hi, fmt.c_str(), tip);
}

inline bool IntRow(const PluginUi::LabelColumn& col, const char* label, const char* id, int* v, int lo, int hi,
                   const char* unitKey, const char* tip) {
    const std::string fmt = unitKey ? Units(unitKey, "%d") : std::string("%d");
    return PluginUi::SliderIntRow(col, label, id, v, lo, hi, fmt.c_str(), tip);
}

// A percent threshold where < 0 means "off": a checkbox + a slider.
inline bool OptionalPercent(const char* label, const char* id, float* v, const char* tip) {
    bool changed = false;
    bool on = *v >= 0.f;
    char cid[64];
    std::snprintf(cid, sizeof(cid), "%s_on", id);
    if (PluginUi::Check(label, cid, &on, tip)) {
        *v = on ? 50.f : -1.f;
        changed = true;
    }
    if (on) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        char sid[64];
        std::snprintf(sid, sizeof(sid), "##%s", id);
        const std::string fmt = Units(I18n::Key("unit_percent"), "%.0f");
        if (ImGui::SliderFloat(sid, v, 0.f, 100.f, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp)) changed = true;
    }
    return changed;
}

// --- debug tools (ui/DebugTools.cpp); run on the worker ------------------
// Every visible element under the game UI root with its localized text, rect
// and depth -> <dir>/ui-<time>.txt (English: a debug dump). Returns the result
// for the Debug tab and the log.
LocText DumpUiTree(const PluginSDK::Context* sdk, const std::filesystem::path& dir);
// Entities within `radius` cells of us: path, type, subtype, state, grid,
// components, StateMachine values 0..7, transitionable state, targetable flags
// -> <dir>/entities-<time>.txt. Returns the result line.
LocText DumpEntities(const PluginSDK::Context* sdk, const std::filesystem::path& dir, const Vec2& self, float radius);

}  // namespace fb::ui
