// AStar.cpp - see AStar.h. The search core (node table, neighbour order,
// relaxation, lazy duplicates) follows core/expedition/ExpeditionPathfinder.cpp;
// the step cost adds the clearance penalty and passability goes through the
// NavOverlay.
#include "AStar.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>

namespace fb {

const char* AStarFailName(AStarFail f) {
    switch (f) {
        case AStarFail::None:         return "none";
        case AStarFail::NoGrid:       return "no walkable grid";
        case AStarFail::StartBlocked: return "start not walkable";
        case AStarFail::GoalBlocked:  return "goal not walkable";
        case AStarFail::TooFar:       return "goal too far";
        case AStarFail::NoPath:       return "no path";
        case AStarFail::IterationCap: return "search limit reached";
    }
    return "?";
}

namespace {

using Cell = std::pair<int, int>;

struct Step {
    int dx, dy;
    float cost;
};
constexpr Step kNeighbors[8] = {
    {0, -1, 1.0f}, {1, -1, 1.41421356f}, {1, 0, 1.0f}, {1, 1, 1.41421356f},
    {0, 1, 1.0f},  {-1, 1, 1.41421356f}, {-1, 0, 1.0f}, {-1, -1, 1.41421356f},
};

// Open-addressing node table: packed (x,y) -> {g, state}; state = parent
// direction (1..8 = kNeighbors index + 1, 9 = start) | closed bit; 0 = empty.
class NodeTable {
public:
    static constexpr uint8_t kDirMask = 0x0F;
    static constexpr uint8_t kClosed = 0x80;
    static constexpr uint8_t kStart = 9;

    struct Node {
        uint64_t key = 0;
        float g = 0.f;
        uint8_t state = 0;
    };

    explicit NodeTable(size_t expected = 4096) {
        size_t cap = 64;
        while (cap < expected * 2) cap <<= 1;
        m_slots.resize(cap);
        m_mask = cap - 1;
    }

    static uint64_t Pack(int x, int y) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
    }

    Node& Acquire(uint64_t key) {
        if ((m_count + 1) * 5 >= m_slots.size() * 3) Grow();
        size_t i = Hash(key) & m_mask;
        for (;;) {
            Node& n = m_slots[i];
            if (n.state == 0) {
                n.key = key;
                ++m_count;
                return n;
            }
            if (n.key == key) return n;
            i = (i + 1) & m_mask;
        }
    }

    Node& Existing(uint64_t key) {
        size_t i = Hash(key) & m_mask;
        for (;;) {
            Node& n = m_slots[i];
            if (n.state != 0 && n.key == key) return n;
            i = (i + 1) & m_mask;
        }
    }

private:
    static uint64_t Hash(uint64_t k) {
        k ^= k >> 33;
        k *= 0xFF51AFD7ED558CCDull;
        k ^= k >> 33;
        k *= 0xC4CEB9FE1A85EC53ull;
        k ^= k >> 33;
        return k;
    }

    void Grow() {
        std::vector<Node> old = std::move(m_slots);
        m_slots.assign(old.size() * 2, Node{});
        m_mask = m_slots.size() - 1;
        for (const Node& n : old) {
            if (n.state == 0) continue;
            size_t i = Hash(n.key) & m_mask;
            while (m_slots[i].state != 0) i = (i + 1) & m_mask;
            m_slots[i] = n;
        }
    }

    std::vector<Node> m_slots;
    size_t m_mask = 0;
    size_t m_count = 0;
};

std::vector<Cell> Reconstruct(NodeTable& nodes, Cell current, Cell start) {
    std::vector<Cell> cells;
    cells.push_back(current);
    while (current != start) {
        const NodeTable::Node& n = nodes.Existing(NodeTable::Pack(current.first, current.second));
        const Step& s = kNeighbors[(n.state & NodeTable::kDirMask) - 1];
        current = {current.first - s.dx, current.second - s.dy};
        cells.push_back(current);
    }
    std::reverse(cells.begin(), cells.end());
    return cells;
}

bool LineCells(const NavGrid& grid, const NavOverlay* ov, int x0, int y0, int x1, int y1, int minClearance) {
    const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;
    for (;;) {
        if (!IsPassable(grid, ov, x, y)) return false;
        if (minClearance > 1 && PassableClearance(grid, ov, x, y) < minClearance) return false;
        if (x == x1 && y == y1) break;
        const int e2 = err * 2;
        const bool stepX = e2 > -dy, stepY = e2 < dx;
        if (stepX && stepY) {
            // Diagonal step: never squeeze between two blocked cells.
            if (!IsPassable(grid, ov, x + sx, y) || !IsPassable(grid, ov, x, y + sy)) return false;
        }
        if (stepX) { err -= dy; x += sx; }
        if (stepY) { err += dx; y += sy; }
    }
    return true;
}

}  // namespace

bool LineOfSight(const NavGrid& grid, const NavOverlay* overlay, const Vec2& a, const Vec2& b, int minClearance) {
    return LineCells(grid, overlay, NavGrid::CellOf(a.x), NavGrid::CellOf(a.y), NavGrid::CellOf(b.x),
                     NavGrid::CellOf(b.y), minClearance);
}

std::vector<Vec2> SmoothPath(const NavGrid& grid, const NavOverlay* overlay, const std::vector<Vec2>& raw) {
    if (raw.size() <= 2) return raw;
    constexpr size_t kWindow = 160;   // look at most this far ahead (bounds the cost)
    std::vector<uint8_t> clear(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
        clear[i] = PassableClearance(grid, overlay, NavGrid::CellOf(raw[i].x), NavGrid::CellOf(raw[i].y));

    std::vector<Vec2> out;
    out.push_back(raw[0]);
    size_t cur = 0;
    std::vector<uint8_t> runMin;
    while (cur < raw.size() - 1) {
        const size_t last = (std::min)(raw.size() - 1, cur + kWindow);
        // Lowest clearance of the raw path from cur to each candidate.
        runMin.assign(last - cur + 1, 0);
        uint8_t m = clear[cur];
        for (size_t k = cur; k <= last; ++k) {
            m = (std::min)(m, clear[k]);
            runMin[k - cur] = m;
        }
        size_t next = cur + 1;
        for (size_t j = last; j > cur + 1; --j) {
            const int need = (std::min)(2, static_cast<int>(runMin[j - cur]));
            if (LineCells(grid, overlay, NavGrid::CellOf(raw[cur].x), NavGrid::CellOf(raw[cur].y),
                          NavGrid::CellOf(raw[j].x), NavGrid::CellOf(raw[j].y), (std::max)(1, need))) {
                next = j;
                break;
            }
        }
        cur = next;
        out.push_back(raw[cur]);
    }
    return out;
}

AStarResult FindPath(const NavGrid& grid, const NavOverlay* overlay, const Vec2& start, const Vec2& goal,
                     const AStarParams& params) {
    AStarResult res;
    if (grid.Width() <= 0 || grid.Height() <= 0) {
        res.fail = AStarFail::NoGrid;
        return res;
    }
    int sx = NavGrid::CellOf(start.x), sy = NavGrid::CellOf(start.y);
    int gx = NavGrid::CellOf(goal.x), gy = NavGrid::CellOf(goal.y);

    if (!IsPassable(grid, overlay, sx, sy)) {
        int ox = 0, oy = 0;
        if (!NearestPassable(grid, overlay, sx, sy, params.snapRadius, ox, oy)) {
            res.fail = AStarFail::StartBlocked;
            return res;
        }
        sx = ox;
        sy = oy;
        res.startSnapped = true;
    }
    {
        const float ddx = static_cast<float>(gx - sx), ddy = static_cast<float>(gy - sy);
        if (std::sqrt(ddx * ddx + ddy * ddy) > params.maxStraightDist) {
            res.fail = AStarFail::TooFar;
            return res;
        }
    }
    if (!IsPassable(grid, overlay, gx, gy)) {
        int ox = 0, oy = 0;
        if (!NearestPassable(grid, overlay, gx, gy, params.snapRadius, ox, oy)) {
            res.fail = AStarFail::GoalBlocked;
            return res;
        }
        gx = ox;
        gy = oy;
        res.goalSnapped = true;
    }
    res.start = NavGrid::CellCenter(sx, sy);
    res.goal = NavGrid::CellCenter(gx, gy);

    auto finish = [&](const std::vector<Cell>& cells, float cost) {
        res.found = true;
        res.cost = cost;
        res.raw.reserve(cells.size());
        for (const Cell& c : cells) res.raw.push_back(NavGrid::CellCenter(c.first, c.second));
        res.path = params.smooth ? SmoothPath(grid, overlay, res.raw) : res.raw;
        if (!res.path.empty()) {
            if (!res.startSnapped) res.path.front() = start;
            if (!res.goalSnapped) {
                if (res.path.size() == 1) res.path.push_back(goal);
                else res.path.back() = goal;
            }
        }
    };

    if (sx == gx && sy == gy) {
        finish({{sx, sy}}, 0.f);
        return res;
    }

    const Cell startKey{sx, sy}, goalKey{gx, gy};
    using QEntry = std::pair<float, Cell>;
    std::priority_queue<QEntry, std::vector<QEntry>, std::greater<QEntry>> open;
    NodeTable nodes;
    const auto h = [gx, gy](int x, int y) {
        const float dx = static_cast<float>(gx - x), dy = static_cast<float>(gy - y);
        return std::sqrt(dx * dx + dy * dy);
    };
    {
        NodeTable::Node& s = nodes.Acquire(NodeTable::Pack(sx, sy));
        s.g = 0.f;
        s.state = NodeTable::kStart;
    }
    open.push({h(sx, sy), startKey});

    int iterations = 0;
    const float penalty = params.wallPenalty;
    while (!open.empty() && iterations < params.maxIterations) {
        ++iterations;
        const Cell current = open.top().second;
        open.pop();
        NodeTable::Node& cur = nodes.Existing(NodeTable::Pack(current.first, current.second));
        if (current == goalKey) {
            res.iterations = iterations;
            finish(Reconstruct(nodes, current, startKey), cur.g);
            return res;
        }
        if (cur.state & NodeTable::kClosed) continue;
        cur.state |= NodeTable::kClosed;
        const float curG = cur.g;
        for (int si = 0; si < 8; ++si) {
            const Step& st = kNeighbors[si];
            const int nx = current.first + st.dx, ny = current.second + st.dy;
            if (!IsPassable(grid, overlay, nx, ny)) continue;
            if (st.dx != 0 && st.dy != 0) {
                if (!IsPassable(grid, overlay, current.first + st.dx, current.second) ||
                    !IsPassable(grid, overlay, current.first, current.second + st.dy))
                    continue;
            }
            const float tentative =
                curG + st.cost + ClearancePenalty(PassableClearance(grid, overlay, nx, ny), penalty);
            NodeTable::Node& nb = nodes.Acquire(NodeTable::Pack(nx, ny));
            if (nb.state == 0 || (!(nb.state & NodeTable::kClosed) && tentative < nb.g)) {
                nb.g = tentative;
                nb.state = static_cast<uint8_t>(si + 1);
                open.push({tentative + h(nx, ny), Cell{nx, ny}});
            }
        }
    }
    res.iterations = iterations;
    res.fail = open.empty() ? AStarFail::NoPath : AStarFail::IterationCap;
    return res;
}

}  // namespace fb
