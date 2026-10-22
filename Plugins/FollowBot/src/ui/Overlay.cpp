// Overlay.cpp - the world debug overlay and the in-game status window (render
// thread; drawn only while the game is in front; no game reads beyond one
// player-filtered snapshot for a fresh camera matrix).
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../world/Projection.h"
#include "Tabs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace fb;

namespace {

constexpr ImU32 kColPath = IM_COL32(80, 220, 120, 230);
constexpr ImU32 kColLook = IM_COL32(250, 220, 60, 255);
constexpr ImU32 kColCursor = IM_COL32(255, 150, 40, 255);
constexpr ImU32 kColLeader = IM_COL32(240, 70, 70, 255);
constexpr ImU32 kColLast = IM_COL32(240, 140, 140, 200);
constexpr ImU32 kColRoute = IM_COL32(220, 90, 220, 180);
constexpr ImU32 kColFollow = IM_COL32(90, 200, 240, 160);
constexpr ImU32 kColStop = IM_COL32(90, 240, 200, 120);
constexpr ImU32 kColObject = IM_COL32(120, 180, 255, 230);
constexpr ImU32 kColStuck = IM_COL32(255, 60, 60, 255);
constexpr ImU32 kColCombat = IM_COL32(255, 120, 60, 120);

struct Painter {
    ImDrawList*    dl;
    ProjectionData proj;
    float          fallbackZ;

    bool Grid(const Vec2& g, ImVec2& out) const {
        Vec2 s;
        if (!ProjectGrid(proj, g, fallbackZ, s)) return false;
        out = ImVec2(s.x, s.y);
        return true;
    }
    bool WorldPt(const Vec3& w, ImVec2& out) const {
        Vec2 s;
        if (!ProjectWorld(proj, w, s)) return false;
        out = ImVec2(s.x, s.y);
        return true;
    }
    void Polyline(const std::vector<Vec2>& pts, ImU32 col, float thick) const {
        ImVec2 prev;
        bool havePrev = false;
        for (const auto& p : pts) {
            ImVec2 s;
            if (!Grid(p, s)) {
                havePrev = false;
                continue;
            }
            if (havePrev) dl->AddLine(prev, s, col, thick);
            prev = s;
            havePrev = true;
        }
    }
    void Ring(const Vec2& center, float radius, ImU32 col, float thick) const {
        constexpr int N = 48;
        ImVec2 pts[N];
        for (int i = 0; i < N; ++i) {
            const float a = 2.f * kPi * static_cast<float>(i) / static_cast<float>(N);
            if (!Grid(center + Vec2{std::cos(a), std::sin(a)} * radius, pts[i])) return;
        }
        dl->AddPolyline(pts, N, col, ImDrawFlags_Closed, thick);
    }
    void Dot(const Vec2& g, float r, ImU32 col) const {
        ImVec2 s;
        if (Grid(g, s)) dl->AddCircleFilled(s, r, col);
    }
    void Label(const Vec2& g, ImU32 col, const char* text) const {
        ImVec2 s;
        if (Grid(g, s)) PluginUi::TextOutlined(dl, ImVec2(s.x + 6.f, s.y - 8.f), col, text);
    }
};

}  // namespace

void FollowBotPlugin::DrawUI() {
    if (!m_enabled || !ctx()->ImGuiContext) return;
    const DebugSettings& d = m_settings.global.debug;
    const bool status = d.statusWindow && (m_running.load() || m_debugActive.load());
    if (!d.overlay && !status) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
    if (!ctx()->Game.IsInGame() || !ctx()->Game.IsForeground() || ctx()->Game.IsMenuVisible()) return;
    m_uiView = LatestView();
    if (!m_uiView) return;
    if (d.overlay) DrawOverlay();
    if (status) DrawStatusWindow();
}

void FollowBotPlugin::DrawOverlay() {
    const BotView& v = *m_uiView;
    const DebugSettings& d = m_settings.global.debug;
    // A fresh camera matrix (the worker's is one tick old and the camera follows the player).
    const PluginSDK::Snapshot snap = ctx()->Game.GetSnapshot(PluginSDK::EntityType::Player);
    Painter p{ImGui::GetBackgroundDrawList(), v.proj, snap.Player.WorldZ};
    p.proj.valid = snap.ScreenWidth > 0 && snap.ScreenHeight > 0;
    for (int i = 0; i < 16; ++i) p.proj.m[i] = snap.WorldToScreenMatrix[i];
    p.proj.screenW = snap.ScreenWidth;
    p.proj.screenH = snap.ScreenHeight;
    if (!p.proj.valid) return;

    if (d.showPath && v.path.size() >= 2) p.Polyline(v.path, kColPath, 2.0f);
    if (d.showLookahead && v.hasLookahead) p.Dot(v.lookahead, 5.f, kColLook);
    if (d.showCursor && v.hasCursor) {
        const ImVec2 c(v.cursorClient.x, v.cursorClient.y);
        p.dl->AddLine(ImVec2(c.x - 7.f, c.y), ImVec2(c.x + 7.f, c.y), kColCursor, 2.0f);
        p.dl->AddLine(ImVec2(c.x, c.y - 7.f), ImVec2(c.x, c.y + 7.f), kColCursor, 2.0f);
    }
    if (d.showLeader && v.leaderConfigured) {
        if (v.leaderVisible) {
            ImVec2 s;
            if (p.WorldPt(v.leaderWorld, s)) {
                p.dl->AddCircle(s, 12.f, kColLeader, 0, 2.5f);
                PluginUi::TextOutlined(p.dl, ImVec2(s.x + 14.f, s.y - 8.f), kColLeader, v.leaderName.c_str());
            }
            if (d.showCircles) {
                p.Ring(v.leaderGrid, v.followDistance, kColFollow, 1.5f);
                p.Ring(v.leaderGrid, v.stopDistance, kColStop, 1.0f);
            }
        } else if (v.leaderDistance >= 0.f) {
            p.Ring(v.leaderLastGrid, 2.5f, kColLast, 2.0f);
            char lbl[96];
            std::snprintf(lbl, sizeof(lbl), "last seen%s%s", v.vanish != VanishKind::None ? ": " : "",
                          v.vanish != VanishKind::None ? VanishKindName(v.vanish) : "");
            p.Label(v.leaderLastGrid, kColLast, lbl);
        }
    }
    if (d.showRoute && v.leaderRoute.size() >= 2) p.Polyline(v.leaderRoute, kColRoute, 1.0f);
    if (d.showInteractables) {
        for (const auto& it : v.interactables) {
            if (Dist(it.grid, v.selfGrid) > 120.f) continue;
            const ImU32 col = it.visibleNow ? kColObject : IM_COL32(120, 180, 255, 120);
            p.Dot(it.grid, 4.f, col);
            char lbl[64];
            if (it.portalType >= 0) std::snprintf(lbl, sizeof(lbl), "%s (%d)", InteractKindName(it.kind), it.portalType);
            else std::snprintf(lbl, sizeof(lbl), "%s", InteractKindName(it.kind));
            p.Label(it.grid, col, lbl);
        }
    }
    if (d.showLoot) {
        for (const auto& m : v.markers) {
            p.Dot(m.grid, 4.f, m.color);
            if (!m.label.empty()) p.Label(m.grid, m.color, m.label.c_str());
        }
    }
    if (d.showCombat) {
        p.Ring(v.selfGrid, v.combatRadius, kColCombat, 1.0f);
        if (v.leaderVisible) p.Ring(v.leaderGrid, v.leashRadius, kColCombat, 1.0f);
    }
    if (d.showStuck) {
        for (const auto& s : v.stuckMarks) {
            ImVec2 c;
            if (!p.Grid(s, c)) continue;
            p.dl->AddLine(ImVec2(c.x - 5.f, c.y - 5.f), ImVec2(c.x + 5.f, c.y + 5.f), kColStuck, 2.0f);
            p.dl->AddLine(ImVec2(c.x - 5.f, c.y + 5.f), ImVec2(c.x + 5.f, c.y - 5.f), kColStuck, 2.0f);
        }
        for (const auto& b : v.blocks) p.Ring(b.center, static_cast<float>(b.radius) + 0.5f, kColStuck, 1.0f);
    }
}

void FollowBotPlugin::DrawStatusWindow() {
    const BotView& v = *m_uiView;
    const DebugSettings& d = m_settings.global.debug;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float scale = d.statusScale;
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * scale;

    char lines[6][192];
    int n = 0;
    std::snprintf(lines[n++], sizeof(lines[0]), "Follow Bot: %s", v.paused ? "Paused" : v.mode.c_str());
    if (!v.taskDetail.empty()) std::snprintf(lines[n++], sizeof(lines[0]), "%s", v.taskDetail.c_str());
    if (v.leaderConfigured) {
        if (v.leaderVisible)
            std::snprintf(lines[n++], sizeof(lines[0]), "Leader %s: %.0f cells", v.leaderName.c_str(), v.leaderDistance);
        else
            std::snprintf(lines[n++], sizeof(lines[0]), "Leader %s: %s", v.leaderName.c_str(),
                          v.vanish != VanishKind::None ? VanishKindName(v.vanish) : "not in sight");
    }
    if (!v.currentRule.empty()) std::snprintf(lines[n++], sizeof(lines[0]), "Skill: %s", v.currentRule.c_str());
    if (v.deathsSession > 0)
        std::snprintf(lines[n++], sizeof(lines[0]), "Deaths: %d (map) / %d", v.deathsMap, v.deathsSession);
    if (!v.lastError.empty() && n < 6) std::snprintf(lines[n++], sizeof(lines[0]), "%s", v.lastError.c_str());

    float w = 0.f;
    for (int i = 0; i < n; ++i) w = (std::max)(w, font->CalcTextSizeA(fs, 10000.f, 0.f, lines[i]).x);
    const ImVec2 pad = PluginUi::PanelPadding();
    const float lineH = fs * 1.2f;
    const ImVec2 size(w + pad.x * 2.f, lineH * static_cast<float>(n) + pad.y * 2.f);
    const float margin = ImGui::GetFontSize();
    float x = margin, y = margin * 4.f;
    switch (d.statusAnchor) {
        case 1: x = (disp.x - size.x) * 0.5f; break;
        case 2: x = disp.x - size.x - margin; break;
        case 3: y = disp.y - size.y - margin * 9.f; break;
        case 4: x = (disp.x - size.x) * 0.5f; y = disp.y - size.y - margin * 9.f; break;
        case 5: x = disp.x - size.x - margin; y = disp.y - size.y - margin * 9.f; break;
        default: break;
    }
    PluginUi::Panel(dl, ImVec2(x, y), ImVec2(x + size.x, y + size.y));
    for (int i = 0; i < n; ++i) {
        ImU32 col = IM_COL32(230, 230, 230, 255);
        if (i == 0) col = v.paused || !v.gateReason.empty() ? IM_COL32(240, 190, 80, 255) : IM_COL32(120, 220, 140, 255);
        PluginUi::TextOutlined(dl, font, fs, ImVec2(x + pad.x, y + pad.y + lineH * static_cast<float>(i)), col, lines[i]);
    }
}
