// Geometry.h - small 2D/3D vector math shared by every module.
//
// Grid coordinates (cells of the walkable grid, ~10.87 world units each) are
// the bot's working unit; world coordinates are only used for projection.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace fb {

inline constexpr float kPi = 3.14159265358979323846f;
// One grid cell in world units (TileToWorld 250 / TileToGrid 23).
inline constexpr float kGridToWorld = 250.0f / 23.0f;

struct Vec2 {
    float x = 0.f, y = 0.f;
    Vec2() = default;
    Vec2(float xx, float yy) : x(xx), y(yy) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2& o) const { return !(*this == o); }
};

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    Vec3() = default;
    Vec3(float xx, float yy, float zz) : x(xx), y(yy), z(zz) {}
};

inline float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float Cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
inline float Length(const Vec2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline float Length2(const Vec2& v) { return v.x * v.x + v.y * v.y; }
inline float Dist(const Vec2& a, const Vec2& b) { return Length(a - b); }
inline float Dist2(const Vec2& a, const Vec2& b) { return Length2(a - b); }

inline Vec2 Normalize(const Vec2& v) {
    const float l = Length(v);
    return l > 1e-6f ? v / l : Vec2{0.f, 0.f};
}

// Perpendicular (rotated +90 degrees in grid space).
inline Vec2 Perp(const Vec2& v) { return {-v.y, v.x}; }

inline Vec2 Rotate(const Vec2& v, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

inline Vec2 Lerp(const Vec2& a, const Vec2& b, float t) { return a + (b - a) * t; }

// Angle between two directions in radians (0..pi); 0 for a zero vector.
inline float AngleBetween(const Vec2& a, const Vec2& b) {
    const float la = Length(a), lb = Length(b);
    if (la < 1e-6f || lb < 1e-6f) return 0.f;
    const float c = std::clamp(Dot(a, b) / (la * lb), -1.f, 1.f);
    return std::acos(c);
}

// Distance from p to the segment ab, and the parameter t (0..1) of the closest point.
inline float PointSegmentDistance(const Vec2& p, const Vec2& a, const Vec2& b, float* tOut = nullptr) {
    const Vec2 ab = b - a;
    const float len2 = Length2(ab);
    float t = 0.f;
    if (len2 > 1e-9f) t = std::clamp(Dot(p - a, ab) / len2, 0.f, 1.f);
    if (tOut) *tOut = t;
    return Dist(p, a + ab * t);
}

// Distance from p to a polyline (the single point when it has one); a large
// value for an empty line. `segOut` receives the index of the nearest segment.
inline float PolylineDistance(const Vec2& p, const std::vector<Vec2>& line, size_t* segOut = nullptr) {
    if (line.empty()) return 1e9f;
    if (line.size() == 1) {
        if (segOut) *segOut = 0;
        return Dist(p, line[0]);
    }
    float best = 1e9f;
    size_t bestSeg = 0;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const float d = PointSegmentDistance(p, line[i], line[i + 1]);
        if (d < best) { best = d; bestSeg = i; }
    }
    if (segOut) *segOut = bestSeg;
    return best;
}

// Total length of a polyline.
inline float PolylineLength(const std::vector<Vec2>& line) {
    float total = 0.f;
    for (size_t i = 1; i < line.size(); ++i) total += Dist(line[i - 1], line[i]);
    return total;
}

// Length of the polyline from the point nearest to p to its end.
inline float RemainingLength(const Vec2& p, const std::vector<Vec2>& line) {
    if (line.empty()) return 0.f;
    if (line.size() == 1) return Dist(p, line[0]);
    size_t seg = 0;
    PolylineDistance(p, line, &seg);
    float t = 0.f;
    PointSegmentDistance(p, line[seg], line[seg + 1], &t);
    const Vec2 onLine = Lerp(line[seg], line[seg + 1], t);
    float total = Dist(onLine, line[seg + 1]);
    for (size_t i = seg + 2; i < line.size(); ++i) total += Dist(line[i - 1], line[i]);
    return total;
}

inline Vec3 GridToWorld(const Vec2& g, float z) { return {g.x * kGridToWorld, g.y * kGridToWorld, z}; }
inline Vec2 WorldToGrid(float wx, float wy) { return {wx / kGridToWorld, wy / kGridToWorld}; }

inline bool IsFinite(const Vec2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }

}  // namespace fb
