// NavGrid.cpp - see NavGrid.h.
#include "NavGrid.h"

#include <cstring>

namespace fb {

std::shared_ptr<NavGrid> NavGrid::FromPacked(const uint8_t* data, size_t sizeBytes, int width, int height,
                                             uint64_t generation) {
    if (!data || width <= 0 || height <= 0 || width > 20000 || height > 20000) return nullptr;
    const int bytesPerRow = (width + 1) / 2;
    if (sizeBytes < static_cast<size_t>(bytesPerRow) * static_cast<size_t>(height)) return nullptr;
    auto g = std::make_shared<NavGrid>();
    g->m_w = width;
    g->m_h = height;
    g->m_gen = generation;
    g->m_cells.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(bytesPerRow);
        uint8_t* out = g->m_cells.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) {
            const uint8_t b = row[x >> 1];
            out[x] = (x & 1) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 0x0F);
        }
    }
    g->BuildClearance();
    return g;
}

std::shared_ptr<NavGrid> NavGrid::FromAscii(const std::vector<const char*>& rows, uint64_t generation) {
    if (rows.empty() || !rows[0]) return nullptr;
    const int w = static_cast<int>(std::strlen(rows[0]));
    const int h = static_cast<int>(rows.size());
    if (w <= 0) return nullptr;
    auto g = std::make_shared<NavGrid>();
    g->m_w = w;
    g->m_h = h;
    g->m_gen = generation;
    g->m_cells.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        const char* r = rows[static_cast<size_t>(y)];
        const int len = r ? static_cast<int>(std::strlen(r)) : 0;
        for (int x = 0; x < w; ++x) {
            const char c = x < len ? r[x] : '#';
            g->m_cells[g->Index(x, y)] = (c == '#') ? uint8_t(0) : uint8_t(1);
        }
    }
    g->BuildClearance();
    return g;
}

void NavGrid::BuildClearance() {
    const size_t n = m_cells.size();
    m_clear.assign(n, 0);
    constexpr uint8_t kUnset = 0xFF;
    std::vector<uint32_t> queue;
    queue.reserve(n / 8 + 16);
    for (int y = 0; y < m_h; ++y) {
        for (int x = 0; x < m_w; ++x) {
            const size_t i = Index(x, y);
            if (m_cells[i] == 0) continue;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (!InBounds(nx, ny) || m_cells[Index(nx, ny)] == 0) { edge = true; break; }
                }
            if (edge) {
                m_clear[i] = 1;
                queue.push_back(static_cast<uint32_t>(i));
            } else {
                m_clear[i] = kUnset;
            }
        }
    }
    size_t head = 0;
    while (head < queue.size()) {
        const uint32_t i = queue[head++];
        const uint8_t k = m_clear[i];
        if (k >= kMaxClearance) continue;
        const int x = static_cast<int>(i % static_cast<uint32_t>(m_w));
        const int y = static_cast<int>(i / static_cast<uint32_t>(m_w));
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx, ny = y + dy;
                if (!InBounds(nx, ny)) continue;
                const size_t j = Index(nx, ny);
                if (m_clear[j] != kUnset) continue;
                m_clear[j] = static_cast<uint8_t>(k + 1);
                queue.push_back(static_cast<uint32_t>(j));
            }
    }
    for (auto& c : m_clear)
        if (c == kUnset) c = kMaxClearance;
}

// ---------------------------------------------------------------------------
// NavOverlay
// ---------------------------------------------------------------------------

void NavOverlay::AddBlock(const Vec2& center, int radius, uint64_t untilMs) {
    Block b;
    b.cx = NavGrid::CellOf(center.x);
    b.cy = NavGrid::CellOf(center.y);
    b.r = radius < 0 ? 0 : radius;
    b.untilMs = untilMs;
    m_blocks.push_back(b);
    if (m_blocks.size() > 64) m_blocks.erase(m_blocks.begin());
    ++m_gen;
}

bool NavOverlay::Expire(uint64_t nowMs) {
    const size_t before = m_blocks.size();
    std::erase_if(m_blocks, [nowMs](const Block& b) { return nowMs >= b.untilMs; });
    if (m_blocks.size() != before) {
        ++m_gen;
        return true;
    }
    return false;
}

bool NavOverlay::IsBlocked(int x, int y) const {
    for (const Block& b : m_blocks) {
        const int dx = x - b.cx, dy = y - b.cy;
        if (dx >= -b.r && dx <= b.r && dy >= -b.r && dy <= b.r) return true;
    }
    return false;
}

void NavOverlay::SetDoorCells(const std::vector<std::pair<int, int>>& cells) {
    std::unordered_set<uint64_t> next;
    for (const auto& c : cells) next.insert(Key(c.first, c.second));
    if (next != m_doors) {
        m_doors = std::move(next);
        ++m_gen;
    }
}

bool NavOverlay::IsDoor(int x, int y) const { return !m_doors.empty() && m_doors.count(Key(x, y)) != 0; }

void NavOverlay::Clear() {
    if (m_blocks.empty() && m_doors.empty()) return;
    m_blocks.clear();
    m_doors.clear();
    ++m_gen;
}

bool NearestPassable(const NavGrid& g, const NavOverlay* ov, int x, int y, int radius, int& outX, int& outY) {
    if (IsPassable(g, ov, x, y)) {
        outX = x;
        outY = y;
        return true;
    }
    for (int r = 1; r <= radius; ++r) {
        int bestX = 0, bestY = 0, bestD = 1 << 30;
        bool found = false;
        auto consider = [&](int cx, int cy) {
            if (!IsPassable(g, ov, cx, cy)) return;
            const int d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
            if (d < bestD) { bestD = d; bestX = cx; bestY = cy; found = true; }
        };
        for (int dx = -r; dx <= r; ++dx) {
            consider(x + dx, y - r);
            consider(x + dx, y + r);
        }
        for (int dy = -r + 1; dy <= r - 1; ++dy) {
            consider(x - r, y + dy);
            consider(x + r, y + dy);
        }
        if (found) {
            outX = bestX;
            outY = bestY;
            return true;
        }
    }
    return false;
}

}  // namespace fb
