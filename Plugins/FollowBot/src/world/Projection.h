// Projection.h - world -> screen with the snapshot matrix (design §5.1).
// Pure: no SDK.
//
// Render.WorldToScreen refuses off-screen points, but steering toward a target
// off the screen still needs its direction: this projects with the snapshot's
// row-major WorldToScreenMatrix itself and only rejects points behind the
// camera (w < 0.001). Grid points take their Z from the terrain height grid
// (the old bot used the player's Z for every waypoint: wrong on slopes).
#pragma once

#include "World.h"

#include <algorithm>

namespace fb {

// Client-pixel projection of a world point; false behind the camera only.
inline bool ProjectWorld(const ProjectionData& p, const Vec3& w, Vec2& out) {
    if (!p.valid || p.screenW <= 0 || p.screenH <= 0) return false;
    const float* m = p.m;
    const float rx = w.x * m[0] + w.y * m[4] + w.z * m[8] + m[12];
    const float ry = w.x * m[1] + w.y * m[5] + w.z * m[9] + m[13];
    const float rw = w.x * m[3] + w.y * m[7] + w.z * m[11] + m[15];
    if (rw < 0.001f) return false;
    out.x = (rx / rw + 1.0f) * (static_cast<float>(p.screenW) * 0.5f);
    out.y = (1.0f - ry / rw) * (static_cast<float>(p.screenH) * 0.5f);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

// Terrain Z at a grid point (fallback when the height grid is missing).
inline float TerrainZ(const ProjectionData& p, const Vec2& grid, float fallback) {
    return p.height ? p.height->At(grid.x, grid.y, fallback) : fallback;
}

// Grid point on the ground -> client pixels; `fallbackZ` (usually the player's
// world Z) when the height grid is missing.
inline bool ProjectGrid(const ProjectionData& p, const Vec2& grid, float fallbackZ, Vec2& out) {
    return ProjectWorld(p, GridToWorld(grid, TerrainZ(p, grid, fallbackZ)), out);
}

// A point `amount` world units "up" the object at `w` (tall portals: aim at the
// middle, not the feet). The world Z axis sign is not assumed: both directions
// are projected and the one higher on the screen wins.
inline bool ProjectLifted(const ProjectionData& p, const Vec3& w, float amount, Vec2& out) {
    if (amount <= 0.f) return ProjectWorld(p, w, out);
    Vec2 a, b;
    const bool okA = ProjectWorld(p, Vec3{w.x, w.y, w.z + amount}, a);
    const bool okB = ProjectWorld(p, Vec3{w.x, w.y, w.z - amount}, b);
    if (okA && okB) {
        out = a.y < b.y ? a : b;
        return true;
    }
    if (okA) { out = a; return true; }
    if (okB) { out = b; return true; }
    return false;
}

inline bool OnScreen(const ProjectionData& p, const Vec2& s, float margin = 0.f) {
    return s.x >= margin && s.y >= margin && s.x <= static_cast<float>(p.screenW) - margin &&
           s.y <= static_cast<float>(p.screenH) - margin;
}

// The area the bot may put the cursor in: 4 % side margins, 6 % from the top,
// at most 82 % of the height (the HUD, flask belt and skill bar are below).
struct SafeRect {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    Vec2 Clamp(const Vec2& v) const { return {std::clamp(v.x, x0, x1), std::clamp(v.y, y0, y1)}; }
    bool Contains(const Vec2& v) const { return v.x >= x0 && v.x <= x1 && v.y >= y0 && v.y <= y1; }
};

inline SafeRect MakeSafeRect(int screenW, int screenH) {
    SafeRect r;
    r.x0 = static_cast<float>(screenW) * 0.04f;
    r.x1 = static_cast<float>(screenW) * 0.96f;
    r.y0 = static_cast<float>(screenH) * 0.06f;
    r.y1 = static_cast<float>(screenH) * 0.82f;
    return r;
}

// Clamps `target` toward `origin` so it lies inside the rect (keeps the
// direction instead of sliding along an edge).
inline Vec2 ClampAlongRay(const SafeRect& r, const Vec2& origin, const Vec2& target) {
    if (r.Contains(target)) return target;
    const Vec2 o = r.Clamp(origin);
    const Vec2 d = target - o;
    float t = 1.f;
    if (d.x > 0.f) t = (std::min)(t, (r.x1 - o.x) / d.x);
    if (d.x < 0.f) t = (std::min)(t, (r.x0 - o.x) / d.x);
    if (d.y > 0.f) t = (std::min)(t, (r.y1 - o.y) / d.y);
    if (d.y < 0.f) t = (std::min)(t, (r.y0 - o.y) / d.y);
    t = std::clamp(t, 0.f, 1.f);
    return r.Clamp(o + d * t);
}

}  // namespace fb
