/**
 * @file
 * @brief Classic random map terrain generation: coastlines, highlands, erosion, rivers, forests,
 * marshes.
 */
#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

/**
 * @brief Determines where edge `edge_idx`'s stretch of coast begins and
 *        ends: the map's corners drawn in 10 along the diagonal, then jittered.
 */
void Generator::CoastEnds(int edge_idx) {
    int next_edge = (edge_idx + 1) % 4;
    if (R16(kPEdgePoints + 2 * edge_idx) < 3) {
        SetPt(kEdgeStart + 4 * edge_idx, Pt(kEdges + 8 * edge_idx));
        SetPt(kEdgeEnd + 4 * edge_idx, Pt(kEdges + 8 * edge_idx + 4));
        return;
    }
    Point start_pt = Pt(kEdges + 8 * edge_idx);
    start_pt.x -= 10 * R16(kDiagonals + 4 * edge_idx);
    start_pt.y -= 10 * R16(kDiagonals + 4 * edge_idx + 2);
    Jitter(start_pt, 16, true);
    Put(start_pt.x, start_pt.y, kPlain);
    SetPt(kEdgeStart + 4 * edge_idx, start_pt);
    Point end_pt = Pt(kEdges + 8 * edge_idx + 4);
    end_pt.x -= 10 * R16(kDiagonals + 4 * next_edge);
    end_pt.y -= 10 * R16(kDiagonals + 4 * next_edge + 2);
    Jitter(end_pt, 16, true);
    Put(end_pt.x, end_pt.y, kPlain);
    SetPt(kEdgeEnd + 4 * edge_idx, end_pt);
}

/**
 * @brief Swaps an edge's end with the next one's start where that keeps
 *        the outline turning around the middle.
 */
void Generator::CoastOrder() {
    for (int i = 0; i < 4; i++) {
        int next_edge = (i + 1) % 4;
        Point end_pt = Pt(kEdgeEnd + 4 * i), start_pt = Pt(kEdgeStart + 4 * next_edge);
        if (static_cast<double>(56 - end_pt.x) / static_cast<double>(78 - end_pt.y) <
            static_cast<double>(56 - start_pt.x) / static_cast<double>(78 - start_pt.y)) {
            SetPt(kEdgeEnd + 4 * i, start_pt);
            SetPt(kEdgeStart + 4 * next_edge, end_pt);
        }
    }
}

/** @brief True if `point` lies on the outer border of the map. */
bool Generator::OnEdge(Point point) {
    return point.x == 0 || point.y == 0 || point.x == kMapW - 1 || point.y == kMapH - 1;
}

/**
 * @brief Places intermediate points along edge `edge_idx`, each a step on
 *        from the last towards the end and jittered by up to half a step.
 */
void Generator::CoastPoints(int edge_idx) {
    int base = kEdgePts + 0x50 * edge_idx, want = R16(kPEdgePoints + 2 * edge_idx);
    Point start_pt = Pt(kEdgeStart + 4 * edge_idx), end_pt = Pt(kEdgeEnd + 4 * edge_idx);
    SetPt(base, start_pt);
    int count = 1;
    if (want < 3 || (OnEdge(start_pt) && OnEdge(end_pt))) {
        SetPt(base + 4, end_pt);
        W16(kEdgeCount + 2 * edge_idx, 2);
        return;
    }
    int step_len = Reach(end_pt.x, end_pt.y, start_pt.x, start_pt.y) / (want - 1);
    int idx = 0;
    for (;;) {
        Point cur = Pt(base + 4 * idx);
        Point dir = Step(cur.x, cur.y, end_pt.x, end_pt.y);
        Point next_pt{cur.x + dir.x * step_len, cur.y + dir.y * step_len};
        Jitter(next_pt, step_len / 2, false);
        idx++;
        Put(next_pt.x, next_pt.y, kPlain);
        SetPt(base + 4 * idx, next_pt);
        count++;
        if (Reach(end_pt.x, end_pt.y, next_pt.x, next_pt.y) < step_len || count >= want - 1) break;
    }
    SetPt(base + 4 * (idx + 1), end_pt);
    W16(kEdgeCount + 2 * edge_idx, count + 1);
}

/** @brief Turns a step vector by -1 to +3 eighths of a circle. */
Point Generator::Turn(Point dir) {
    int idx = NbIndex(dir.x, dir.y);
    int next_dir = Dice(1, 5, (idx < 0 ? 8 : idx) - 2) % 8;
    return Point{Nx(next_dir), Ny(next_dir)};
}

/** @brief Draws a wandering line of plain from `from` to `to_pt`. */
void Generator::CoastLine(Point from, Point to_pt) {
    Point cur = from;
    Point first = Step(from.x, from.y, to_pt.x, to_pt.y);
    for (int step = 0; step < kSafety; step++) {
        Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
        int roll = Reach(cur.x, cur.y, to_pt.x, to_pt.y) < 3 ? 1 : Dice(1, 3, 0);
        if (roll == 2)
            dir = first;
        else if (roll == 3) {
            dir = Turn(dir);
            first = dir;
        }
        cur.x += dir.x;
        cur.y += dir.y;
        Clamp(cur);
        Put(cur.x, cur.y, kPlain);
        if (cur.x == to_pt.x && cur.y == to_pt.y) return;
    }
}

/** @brief Which edge (0..3) a point lies on, or -1 for none. */
int Generator::EdgeOf(Point point) {
    if (point.x == 0) return 3;
    if (point.y == 0) return 0;
    if (point.x == kMapW - 1) return 1;
    if (point.y == kMapH - 1) return 2;
    return -1;
}

/** @brief Draws a straight run of plain from `from` to `to_pt`. */
void Generator::Straight(Point from, Point to_pt) {
    Point dir = Step(from.x, from.y, to_pt.x, to_pt.y);
    Point cur = from;
    for (int step = 0; step < kSafety && !(cur.x == to_pt.x && cur.y == to_pt.y); step++) {
        cur.x += dir.x;
        cur.y += dir.y;
        Clamp(cur);
        Put(cur.x, cur.y, kPlain);
    }
}

/**
 * @brief Joins two points on the map's edge along the border, around a
 *        corner if needed; across the map they wander like any coastline.
 */
void Generator::CoastAlongEdge(Point from, Point to_pt) {
    int edge_a = EdgeOf(from), edge_b = EdgeOf(to_pt);
    if (edge_a == edge_b) return Straight(from, to_pt);
    int apart = std::abs(edge_a - edge_b);
    if (apart == 2) return CoastLine(from, to_pt);
    Point corner = Pt(kCorners + 4 * (apart == 3 ? 0 : std::max(edge_a, edge_b)));
    Straight(from, corner);
    Straight(to_pt, corner);
}

/** @brief Connects two coastline control points `from` and `to_pt`. */
void Generator::CoastSegment(Point from, Point to_pt) {
    if (OnEdge(from) && OnEdge(to_pt))
        CoastAlongEdge(from, to_pt);
    else
        CoastLine(from, to_pt);
}

/** @brief Joins every point of the coastline outline to the next. */
void Generator::CoastJoin() {
    for (int i = 0; i < 4; i++) {
        int base = kEdgePts + 0x50 * i, count = R16(kEdgeCount + 2 * i);
        int idx = 0;
        for (; idx < count - 1; idx++) CoastSegment(Pt(base + 4 * idx), Pt(base + 4 * (idx + 1)));
        int next_edge = (i + 1) % 4;
        CoastSegment(Pt(base + 4 * idx), Pt(kEdgePts + 0x50 * next_edge));
    }
}

/** @brief True if sea water may flood into (x, y). */
bool Generator::Floods(int x, int y) const {
    int terrain = At(x, y);
    if (terrain == kShore || terrain == kBridge) return false;
    bool inside = x != 0 && y != 0 && x != kMapW - 1 && y != kMapH - 1;
    if (!inside && terrain == 0) return true;
    return At(x + 1, y) == kWater || At(x - 1, y) == kWater || At(x, y + 1) == kWater ||
           At(x, y - 1) == kWater;
}

/**
 * @brief Floods sea water in from the edges up to the coastline outline;
 *        everything unreached becomes plain.
 */
void Generator::CoastFill() {
    for (int i = 0; i < kMapW * kMapH; i++)
        memory[kGrid + i] = memory[kGrid + i] == kPlain ? kShore : 0;
    auto flood = [&](int x, int y) {
        if (Floods(x, y)) Put(x, y, kWater);
    };
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < kMapW; x++)
            for (int y = 0; y < kMapH; y++) flood(x, y);
        for (int y = 0; y < kMapH; y++)
            for (int x = 0; x < kMapW; x++) flood(x, y);
        for (int x = kMapW - 1; x >= 0; x--)
            for (int y = kMapH - 1; y >= 0; y--) flood(x, y);
        for (int y = kMapH - 1; y >= 0; y--)
            for (int x = kMapW - 1; x >= 0; x--) flood(x, y);
    }
    for (int i = 0; i < kMapW * kMapH; i++)
        memory[kGrid + i] = memory[kGrid + i] == kWater ? kWater : kPlain;
}

/** @brief Marks water tiles adjacent to land as shore. */
void Generator::Shores() {
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            if (At(x, y) != kWater) continue;
            if (NearPlain(x, y)) Put(x, y, kShore);
            if (Near(x, y, kHills)) Put(x, y, kShore);
            if (Near(x, y, kMountains)) Put(x, y, kShore);
        }
    }
}

/** @brief Generates the landmass, coastline, and optional sea channels. */
void Generator::Coast() {
    for (int i = 0; i < 4; i++) CoastEnds(i);
    CoastOrder();
    for (int i = 0; i < 4; i++) CoastPoints(i);
    CoastJoin();
    CoastFill();
    Shores();
    if (Dice(1, 100, 0) < 50) {
        // 0-2 channels straight across, west to east
        int channels = Dice(1, 3, -1);
        for (int i = 0; i < channels; i++) River(true, false, true);
        WaterTidy();
    }
    Shores();
}

// --- mountains and hills ------------------------------------------------

/** @brief Places `count` seed nodes of `terrain` (`kind` 1=hill, 2=mountain) on random plain
 * tiles. */
void Generator::Seeds(int count, int terrain, int kind) {
    for (int idx = 0; idx < count; idx++) {
        int x = 0, y = 0;
        for (int tries = 0; tries < kSafety; tries++) {
            x = Dice(1, kMapW, -1);
            y = Dice(1, kMapH, -1);
            if (At(x, y) == kPlain) break;
        }
        Put(x, y, terrain);
        int cur_count = R16(kNodeCount), offset = Node(cur_count);
        W16(offset, x);
        W16(offset + 2, y);
        W16(offset + 4, kind);
        W16(offset + 6, 0);
        W16(kNodeCount, cur_count + 1);
    }
}

/**
 * @brief Links each highland node to its nearest neighbours: a hill to
 *        0..1, a mountain to 0..3.
 */
void Generator::Link() {
    int count = R16(kNodeCount);
    if (count < 2) return;
    for (int i = 0; i < count; i++) {
        int offset = Node(i);
        int links = R16(offset + 4) == 1 ? Dice(1, 2, -1) : Dice(1, 4, -1);
        if (links >= count - 1) links = count - 1;
        W16(offset + 6, links);
        for (int link_idx = 0; link_idx < links; link_idx++) {
            int best = 10000, best_j = -1;
            for (int j = 0; j < count; j++) {
                if (j == i) continue;
                int prev = 0;
                while (prev < link_idx && R16(offset + 8 + 2 * prev) != j) prev++;
                if (prev != link_idx) continue;
                int other = Node(j);
                int dist = Reach(R16(other), R16(other + 2), R16(offset), R16(offset + 2));
                if (dist < best) {
                    best = dist;
                    best_j = j;
                }
            }
            W16(offset + 8 + 2 * link_idx, best_j);
        }
    }
}

/**
 * @brief Draws a ridge from `from` to `to_pt`: `width` 2 adds a tile to
 *        either side, 3 adds one two tiles out. Only plain and hills give way.
 */
void Generator::Ridge(Point from, Point to_pt, int width, int terrain) {
    Point cur = from;
    Point first = Step(from.x, from.y, to_pt.x, to_pt.y);
    int dir_idx = 0;
    auto lay = [&](int x, int y) {
        int cur_terr = At(x, y);
        if (cur_terr == kPlain || cur_terr == kHills) Put(x, y, terrain);
    };
    for (int step = 0; step < kSafety; step++) {
        Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
        int roll = Reach(cur.x, cur.y, to_pt.x, to_pt.y) < 3 ? 1 : Dice(1, 3, 0);
        if (roll == 2)
            dir = first;
        else if (roll == 3) {
            dir = Turn(dir);
            first = dir;
        }
        cur.x += dir.x;
        cur.y += dir.y;
        int found_dir = NbIndex(dir.x, dir.y);
        if (found_dir >= 0) dir_idx = found_dir;
        int left = (dir_idx + 2) % 8, right = (dir_idx + 6) % 8;
        Point side1{cur.x + Nx(left), cur.y + Ny(left)};
        Point side2{cur.x + Nx(right), cur.y + Ny(right)};
        Point outer1{cur.x + 2 * Nx(left), cur.y + 2 * Ny(left)};
        Point outer2{cur.x + 2 * Nx(right), cur.y + 2 * Ny(right)};
        Clamp(cur);
        lay(cur.x, cur.y);
        bool done = cur.x == to_pt.x && cur.y == to_pt.y;
        if (width == 2) {
            Clamp(side1);
            lay(side1.x, side1.y);
            lay(side2.x, side2.y);  // the original clamps only the one
        }
        if (width == 3) {
            Clamp(outer1);
            lay(outer1.x, outer1.y);
            lay(outer2.x, outer2.y);
        }
        if (done) return;
    }
}

/** @brief Gives a node with no links a short ridge of its own, up and to the left. */
void Generator::Blob(Point start_pt, int terrain) {
    Point end_pt;
    end_pt.x = start_pt.x + Dice(1, 5, -10);
    end_pt.y = start_pt.y + Dice(1, 5, -10);
    Clamp(end_pt);
    int width = Dice(1, 2, 1);
    Ridge(start_pt, end_pt, width, terrain);
}

/** @brief Draws ridges along all highland node links. */
void Generator::Ridges() {
    int count = R16(kNodeCount);
    if (count < 2) return;
    for (int i = 0; i < count; i++) {
        int offset = Node(i);
        Point from{R16(offset), R16(offset + 2)};
        bool hill = R16(offset + 4) == 1;
        int links = R16(offset + 6);
        if (links == 0) {
            Blob(from, hill ? kHills : kMountains);
            continue;
        }
        for (int link_idx = 0; link_idx < links; link_idx++) {
            int other = Node(R16(offset + 8 + 2 * link_idx));
            Point to_pt{R16(other), R16(other + 2)};
            if (hill && R16(other + 4) == 1)
                Ridge(from, to_pt, 3, kHills);
            else {
                int width = Dice(1, 2, 1);
                Ridge(from, to_pt, width, kMountains);
            }
        }
    }
}

/**
 * @brief Tidies mountains: plain hemmed in on all four sides joins them,
 *        mountains on the shore come down, and diagonal runs are thickened.
 */
void Generator::MountainTidy() {
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            if (x != 0 && y != 0 && x != kMapW - 1 && y != kMapH - 1 && At(x, y) == kPlain) {
                for (int terrain : {kMountains, kHills}) {
                    if (At(x, y + 1) == terrain && At(x + 1, y) == terrain &&
                        At(x - 1, y) == terrain && At(x, y - 1) == terrain)
                        Put(x, y, terrain);
                }
            }
            if (At(x, y) == kMountains && Near(x, y, kShore)) Put(x, y, kPlain);
        }
    }
    for (int x = 1; x < kMapW - 1; x++) {
        for (int y = 1; y < kMapH - 1; y++) {
            if (At(x, y) != kMountains) continue;
            auto is_mtn = [&](int dx, int dy) { return At(x + dx, y + dy) == kMountains; };
            if (is_mtn(1, 1) && is_mtn(-1, -1) && !is_mtn(1, -1) && !is_mtn(-1, 1)) {
                if (Dice(1, 10, 0) > 5)
                    Put(x - 1, y + 1, kMountains);
                else
                    Put(x + 1, y - 1, kMountains);
            } else if (is_mtn(-1, 1) && is_mtn(1, -1) && !is_mtn(-1, -1) && !is_mtn(1, 1)) {
                if (Dice(1, 10, 0) > 5)
                    Put(x + 1, y + 1, kMountains);
                else
                    Put(x - 1, y - 1, kMountains);
            }
        }
    }
}

/** @brief Rings mountains with foothills. */
void Generator::Foothills() {
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            int terrain = At(x, y);
            if (terrain != kMountains && terrain != kHills && Near(x, y, kMountains))
                Put(x, y, kHills);
        }
    }
}

/** @brief Generates mountains, hills, ridges, and foothills. */
void Generator::Highlands() {
    W16(kNodeCount, 0);
    Seeds(R16(kPMountains), kMountains, 2);
    Seeds(R16(kPHills), kHills, 1);
    Link();
    Ridges();
    MountainTidy();
    Foothills();
}

// --- erosion and passes --------------------------------------------------

/**
 * @brief Walks between two random tiles: erosion wears mountains to hills,
 *        while a pass cuts mountains and hills to plain, five tiles across.
 */
void Generator::Wear(bool pass) {
    Point from, to_pt;
    from.x = Dice(1, kMapW, -1);
    from.y = Dice(1, kMapH, -1);
    to_pt.x = Dice(1, kMapW, -1);
    to_pt.y = Dice(1, kMapH, -1);
    Point cur = from;
    while (!(cur.x == to_pt.x && cur.y == to_pt.y)) {
        Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
        int dir_idx = NbIndex(dir.x, dir.y);
        int left = (dir_idx + 1) % 8, right = (dir_idx + 7) % 8;
        int terrain = At(cur.x, cur.y);
        bool hit = pass ? terrain == kMountains || terrain == kHills : terrain == kMountains;
        if (hit) {
            int target_terr = pass ? kPlain : kHills;
            const int offs[5][2] = {{0, 0},
                                    {Nx(left), Ny(left)},
                                    {Nx(right), Ny(right)},
                                    {2 * Nx(left), 2 * Ny(left)},
                                    {2 * Nx(right), 2 * Ny(right)}};
            for (auto& off : offs) {
                Point pos{cur.x + off[0], cur.y + off[1]};
                Clamp(pos);
                Put(pos.x, pos.y, target_terr);
            }
        }
        cur.x += dir.x;
        cur.y += dir.y;
    }
}

/**
 * @brief Converts tiles of `terrain` with at least three orthogonal
 *        neighbours of `surrounding` into `surrounding`.
 */
void Generator::Surrounded(int terrain, int surrounding) {
    for (int x = 1; x < kMapW - 1; x++) {
        for (int y = 1; y < kMapH - 1; y++) {
            if (At(x, y) != terrain) continue;
            int count = (At(x, y + 1) == surrounding) + (At(x, y - 1) == surrounding) +
                        (At(x + 1, y) == surrounding) + (At(x - 1, y) == surrounding);
            if (count > 2) Put(x, y, surrounding);
        }
    }
}

/** @brief Applies erosion and mountain passes. */
void Generator::Erosion() {
    for (int i = 0; i < R16(kPErosion); i++) Wear(false);
    for (int i = 0; i < R16(kPPasses); i++) Wear(true);
    Surrounded(kHills, kMountains);
    Surrounded(kPlain, kHills);
    Foothills();
}

// --- rivers --------------------------------------------------------------

/**
 * @brief Carves a river or east-west sea channel across the map.
 */
void Generator::River(bool wide, bool stops, bool channel) {
    Point from, to_pt;
    if (channel) {
        from.x = 0;
        from.y = Dice(1, kMapH, -1);
        to_pt.x = kMapW - 1;
        to_pt.y = Dice(1, kMapH, -1);
    } else {
        for (int i = 0; i < 200; i++) {
            from.x = Dice(1, kMapW, -1);
            from.y = Dice(1, kMapH, -1);
            int terrain = At(from.x, from.y);
            if (terrain == kHills || terrain == kMountains) break;
        }
        for (int i = 0; i < 200; i++) {
            to_pt.x = Dice(1, kMapW, -1);
            to_pt.y = Dice(1, kMapH, -1);
            int terrain = At(to_pt.x, to_pt.y);
            if (terrain == kWater || terrain == kShore) break;
        }
    }
    Point cur = from;
    int met = 0;
    bool done = cur.x == to_pt.x && cur.y == to_pt.y;
    for (int step = 0; !done && step < kSafety; step++) {
        Point dir = Step(cur.x, cur.y, to_pt.x, to_pt.y);
        int dir_idx = NbIndex(dir.x, dir.y);
        int left = (dir_idx + 1) % 8, right = (dir_idx + 7) % 8;
        int aside1 = (dir_idx + 2) % 8, aside2 = (dir_idx + 6) % 8;
        if (stops) {
            // nb(-1) reads the word before the table, as the original's did
            int ahead = At(cur.x + Nx(dir_idx), cur.y + Ny(dir_idx));
            if (ahead == kWater && met == 1) done = true;
            if (ahead == kWater && met == 0) met++;
        }
        auto lay = [&](int off_x, int off_y) {
            Point pos{cur.x + off_x, cur.y + off_y};
            Clamp(pos);
            Put(pos.x, pos.y, kWater);
        };
        lay(0, 0);
        lay(Nx(left), Ny(left));
        lay(Nx(right), Ny(right));
        if (wide) {
            lay(2 * Nx(left), 2 * Ny(left));
            lay(2 * Nx(right), 2 * Ny(right));
            if (channel) {
                lay(3 * Nx(left), 3 * Ny(left));
                lay(3 * Nx(right), 3 * Ny(right));
            }
        }
        cur.x += dir.x;
        cur.y += dir.y;
        if (cur.x == to_pt.x && cur.y == to_pt.y) done = true;
        int roll = Dice(1, 4, 0);
        if (roll == 2) {
            cur.x += Nx(aside1);
            cur.y += Ny(aside1);
        } else if (roll == 3) {
            cur.x += Nx(aside2);
            cur.y += Ny(aside2);
        }
    }
}

/**
 * @brief Tidies water and shores: land surrounded on three sides by water
 *        submerges, shore far from land becomes open water, and diagonal
 *        water runs receive an adjacent shore tile.
 */
void Generator::WaterTidy() {
    auto land = [](int terrain) {
        return terrain == kPlain || terrain == kHills || terrain == kMountains;
    };
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            if (!land(At(x, y))) continue;
            int wet_adj =
                    Wet(At(x, y + 1)) + Wet(At(x, y - 1)) + Wet(At(x + 1, y)) + Wet(At(x - 1, y));
            if (wet_adj > 2) Put(x, y, kWater);
        }
    }
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            if (At(x, y) == kShore && !Near(x, y, kPlain) && !Near(x, y, kHills) &&
                !Near(x, y, kMountains))
                Put(x, y, kWater);
        }
    }
    auto dry = [](int terrain) {
        return terrain == kHills || terrain == kForest || terrain == kPlain;
    };
    for (int x = 1; x < kMapW - 1; x++) {
        for (int y = 1; y < kMapH - 1; y++) {
            if (!Wet(At(x, y))) continue;
            auto adj = [&](int dx, int dy) { return At(x + dx, y + dy); };
            if (Wet(adj(1, 1)) && Wet(adj(-1, -1)) && dry(adj(1, -1)) && dry(adj(-1, 1))) {
                if (Dice(1, 10, 0) > 4)
                    Put(x - 1, y + 1, kShore);
                else
                    Put(x + 1, y - 1, kShore);
            } else if (Wet(adj(-1, 1)) && Wet(adj(1, -1)) && dry(adj(-1, -1)) && dry(adj(1, 1))) {
                if (Dice(1, 10, 0) > 4)
                    Put(x + 1, y + 1, kShore);
                else
                    Put(x - 1, y - 1, kShore);
            }
        }
    }
}

/** @brief Generates narrow and wide rivers and tidies water and shores. */
void Generator::Rivers() {
    for (int i = 0; i < R16(kPRivers); i++) River(false, true, false);
    for (int i = 0; i < R16(kPWideRivers); i++) River(true, true, false);
    WaterTidy();
    Shores();
    MountainTidy();
    Foothills();
    WaterTidy();
    Shores();
}

// --- forests -------------------------------------------------------------

/**
 * @brief Grows a wood around a random plain tile: eight arms with two side
 *        shoots off each step that shorten along the arm.
 */
void Generator::Wood() {
    int x = 0, y = 0;
    for (int tries = 0; tries < kSafety; tries++) {
        x = Dice(1, kMapW, -1);
        y = Dice(1, kMapH, -1);
        if (At(x, y) == kPlain) break;
    }
    int arms[8];
    int span, base;
    if (Dice(1, 100, 0) < 65) {
        for (int i = 0; i < 8; i++) arms[i] = Dice(1, 8, 2);
        span = 4;
        base = 2;
    } else {
        for (int i = 0; i < 8; i++) arms[i] = Dice(1, 10, 5);
        span = 6;
        base = 4;
    }
    auto grow = [&](Point& pos) {
        Clamp(pos);
        if (At(pos.x, pos.y) == kPlain) {
            Put(pos.x, pos.y, kForest);
            AddForest();
        }
    };
    Put(x, y, kForest);
    AddForest();
    for (int i = 0; i < 8; i++) {
        Point cur{x, y};
        int dist = 0;
        for (int step = 0; step < arms[i]; step++) {
            cur.x += Nx(i);
            cur.y += Ny(i);
            grow(cur);
            int left = (i + 2) % 8, right = (i + 6) % 8;
            dist++;
            int num_left = Dice(1, span - dist / 2, base - dist / 4);
            int num_right = Dice(1, span - dist / 2, base - dist / 3);
            Point left_pt = cur;
            for (int j = 0; j < num_left; j++) {
                left_pt.x += Nx(left);
                left_pt.y += Ny(left);
                grow(left_pt);
            }
            Point right_pt = cur;
            for (int j = 0; j < num_right; j++) {
                right_pt.x += Nx(right);
                right_pt.y += Ny(right);
                grow(right_pt);
            }
        }
    }
}

/**
 * @brief Tidies forests: plain with forest on three sides becomes forest,
 *        and diagonal runs of forest get an adjacent forest tile.
 */
void Generator::ForestTidy() {
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            if (At(x, y) != kPlain) continue;
            int adj_forest = (At(x, y + 1) == kForest) + (At(x, y - 1) == kForest) +
                             (At(x + 1, y) == kForest) + (At(x - 1, y) == kForest);
            if (adj_forest > 2) {
                Put(x, y, kForest);
                AddForest();
            }
        }
    }
    auto open = [](int terrain) {
        return terrain == kHills || terrain == kShore || terrain == kPlain;
    };
    for (int x = 1; x < kMapW - 1; x++) {
        for (int y = 1; y < kMapH - 1; y++) {
            if (At(x, y) != kForest) continue;
            auto adj = [&](int dx, int dy) { return At(x + dx, y + dy); };
            if (adj(1, 1) == kForest && adj(-1, -1) == kForest && open(adj(1, -1)) &&
                open(adj(-1, 1))) {
                if (Dice(1, 10, 0) < 5)
                    Put(x + 1, y - 1, kForest);
                else
                    Put(x - 1, y + 1, kForest);
            } else if (adj(-1, 1) == kForest && adj(1, -1) == kForest && open(adj(-1, -1)) &&
                       open(adj(1, 1))) {
                if (Dice(1, 10, 0) < 5)
                    Put(x - 1, y - 1, kForest);
                else
                    Put(x + 1, y + 1, kForest);
            }
        }
    }
}

/** @brief Grows woods until they cover land / 100 x the Forest parameter. */
void Generator::Forests() {
    int land = 0;
    for (int i = 0; i < kMapW * kMapH; i++) {
        int terrain = memory[kGrid + i];
        if (terrain == kPlain || terrain == kHills || terrain == kMountains) land++;
    }
    W16(kForestWanted, land / 100 * R16(kPForest));
    W16(kForestDone, 0);
    for (int step = 0; step < 10000 && R16(kForestDone) < R16(kForestWanted); step++) {
        Wood();
        ForestTidy();
    }
}

// --- marshes -------------------------------------------------------------

/** @brief Scatters 1d5+3 tiles of marsh around (x, y). */
void Generator::MarshAround(int x, int y) {
    int count = Dice(1, 5, 3);
    for (int i = 0; i < count; i++) {
        Point pos;
        int dist1 = Dice(1, 3, 0);
        int dir1 = Dice(1, 8, -1);
        pos.x = x + dist1 * Nx(dir1);
        int dist2 = Dice(1, 3, 0);
        int dir2 = Dice(1, 8, -1);
        pos.y = y + dist2 * Ny(dir2);
        Clamp(pos);
        if (At(pos.x, pos.y) == kPlain) Put(pos.x, pos.y, kMarsh);
    }
}

/** @brief Places a marsh cluster, near the shore if one of four tries finds it. */
void Generator::Marsh() {
    int x = 0, y = 0, tries = 0;
    for (int step = 0; step < kSafety; step++) {
        x = Dice(1, 102, 5);
        y = Dice(1, 146, 5);
        if (At(x, y) != kPlain) continue;
        if (tries >= 4) break;
        tries++;
        if (Near(x, y, kShore) || Near(x + 1, y, kShore) || Near(x, y + 1, kShore) ||
            Near(x + 1, y + 1, kShore))
            break;
    }
    Put(x, y, kMarsh);
    MarshAround(x, y);
    MarshAround(x + 1, y + 1);
    MarshAround(x - 1, y - 1);
    MarshAround(x - 1, y + 1);
    MarshAround(x + 1, y - 1);
}

/** @brief Places 1d3 marsh clusters. */
void Generator::Marshes() {
    int count = Dice(1, 3, 0);
    for (int i = 0; i < count; i++) Marsh();
}

}  // namespace w2::randommap
