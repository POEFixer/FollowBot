// Tabs.h - helpers shared by the settings tabs (ui/*.cpp) and the debug tools.
// The tabs themselves are FollowBotPlugin members (DrawTab*), one file each:
// SettingsUi.cpp (tab bar, header, General), UiFollow.cpp (Follow & Movement),
// UiTravel.cpp, UiLoot.cpp, UiSkills.cpp, UiStash.cpp, UiSafety.cpp
// (Death & Safety), UiDebug.cpp, Overlay.cpp (debug overlay + status window),
// DebugTools.cpp (dumps).
#pragma once

#include <imgui.h>

#include "../../../../POEFixer/imgui/IconsFontAwesome6.h"
#include "../../../../POEFixer/plugin_sdk/PluginUi.h"
#include "../world/World.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace PluginSDK {
struct Context;
}

namespace fb::ui {

inline ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

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

// A float slider row with a stable id; returns true on change.
inline bool FloatRow(const PluginUi::LabelColumn& col, const char* label, const char* id, float* v, float lo, float hi,
                     const char* fmt, const char* tip) {
    return PluginUi::SliderFloatRow(col, label, id, v, lo, hi, fmt, tip);
}

inline bool IntRow(const PluginUi::LabelColumn& col, const char* label, const char* id, int* v, int lo, int hi,
                   const char* fmt, const char* tip) {
    return PluginUi::SliderIntRow(col, label, id, v, lo, hi, fmt, tip);
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
        if (ImGui::SliderFloat(sid, v, 0.f, 100.f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    }
    return changed;
}

inline const char* AnchorName(int a) {
    static const char* const kNames[] = {"Top left", "Top centre", "Top right", "Bottom left", "Bottom centre", "Bottom right"};
    return (a >= 0 && a < 6) ? kNames[a] : "?";
}

// --- debug tools (ui/DebugTools.cpp); run on the worker ------------------
// Every visible element under the game UI root with its localized text, rect
// and depth -> <dir>/ui-<time>.txt. Returns a one-line result.
std::string DumpUiTree(const PluginSDK::Context* sdk, const std::filesystem::path& dir);
// Entities within `radius` cells of us: path, type, subtype, state, grid,
// components, StateMachine values 0..7, transitionable state, targetable flags
// -> <dir>/entities-<time>.txt. Returns a one-line result.
std::string DumpEntities(const PluginSDK::Context* sdk, const std::filesystem::path& dir, const Vec2& self, float radius);

}  // namespace fb::ui
