// NavGrid.h - the walkable grid the bot plans on (design §5.4). Pure: no SDK.
//
// Built once per walkable-grid publication from the host's packed buffer
// (4 bits per cell, two cells per byte, row y at y * bytesPerRow, EVEN x = LOW
// nibble; 0 = blocked, 1..15 walkable). Adds a clearance map: the 8-connected
// BFS distance from each walkable cell to the nearest blocked cell (or the map
// edge), 1 next to a wall, capped at kMaxClearance; 0 for blocked cells.
//
// Dynamic state (temporary blocks from stuck recovery, door override cells)
// lives in NavOverlay, owned by the PathPlanner: the grid itself is immutable
// and shared as shared_ptr<const NavGrid>.
#pragma once

#include "../common/Geometry.h"

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace fb {

class NavGrid {
public:
    static constexpr uint8_t kMaxClearance = 6;

    // `width` cells = bytesPerRow * 2 (the SDK handle's Width), `height` rows.
    // Returns nullptr on bad arguments (null data, too short a buffer).
    static std::shared_ptr<NavGrid> FromPacked(const uint8_t* data, size_t sizeBytes, int width, int height,
                                               uint64_t generation);

    // Test helper: one char per cell, '#' blocked, anything else walkable; a row
    // shorter than the first is padded with blocked cells.
    static std::shared_ptr<NavGrid> FromAscii(const std::vector<const char*>& rows, uint64_t generation = 1);

    int Width() const { return m_w; }
    int Height() const { return m_h; }
    uint64_t Generation() const { return m_gen; }
    bool InBounds(int x, int y) const { return x >= 0 && y >= 0 && x < m_w && y < m_h; }

    uint8_t Value(int x, int y) const { return InBounds(x, y) ? m_cells[Index(x, y)] : 0; }
    bool Walkable(int x, int y) const { return Value(x, y) != 0; }
    uint8_t Clearance(int x, int y) const { return InBounds(x, y) ? m_clear[Index(x, y)] : 0; }

    // Cell containing a grid position.
    static int CellOf(float v) { return static_cast<int>(std::floor(v)); }
    // Centre of a cell as a grid position.
    static Vec2 CellCenter(int x, int y) { return {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}; }

private:
    size_t Index(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(m_w) + static_cast<size_t>(x); }
    void BuildClearance();

    int m_w = 0, m_h = 0;
    uint64_t m_gen = 0;
    std::vector<uint8_t> m_cells;   // nibble value per cell
    std::vector<uint8_t> m_clear;   // clearance per cell
};

// Mutable navigation state on top of the immutable grid.
class NavOverlay {
public:
    struct Block {
        int cx = 0, cy = 0, r = 0;
        uint64_t untilMs = 0;
    };

    // Blocks every cell within `radius` (Chebyshev) of `center` until `untilMs`.
    void AddBlock(const Vec2& center, int radius, uint64_t untilMs);
    // Drops expired blocks; true when something changed.
    bool Expire(uint64_t nowMs);
    bool IsBlocked(int x, int y) const;
    const std::vector<Block>& Blocks() const { return m_blocks; }

    // Cells forced walkable (an open door the grid marks blocked).
    void SetDoorCells(const std::vector<std::pair<int, int>>& cells);
    bool IsDoor(int x, int y) const;

    void Clear();
    uint64_t Generation() const { return m_gen; }

private:
    static uint64_t Key(int x, int y) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
    }
    std::vector<Block> m_blocks;
    std::unordered_set<uint64_t> m_doors;
    uint64_t m_gen = 1;
};

// Walkable for planning: a door cell is open, a blocked cell is closed, else the grid.
inline bool IsPassable(const NavGrid& g, const NavOverlay* ov, int x, int y) {
    if (ov) {
        if (ov->IsDoor(x, y)) return g.InBounds(x, y);
        if (ov->IsBlocked(x, y)) return false;
    }
    return g.Walkable(x, y);
}

// Clearance for planning (doors count as 1, temporary blocks as 0).
inline uint8_t PassableClearance(const NavGrid& g, const NavOverlay* ov, int x, int y) {
    if (ov) {
        if (ov->IsDoor(x, y)) return g.InBounds(x, y) ? uint8_t(1) : uint8_t(0);
        if (ov->IsBlocked(x, y)) return 0;
    }
    return g.Clearance(x, y);
}

// Nearest passable cell to (x, y) within `radius` (square rings, nearest ring
// first, then nearest by distance inside the ring). False when none.
bool NearestPassable(const NavGrid& g, const NavOverlay* ov, int x, int y, int radius, int& outX, int& outY);

}  // namespace fb
