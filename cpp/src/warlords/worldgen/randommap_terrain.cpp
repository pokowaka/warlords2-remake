/**
 * @file
 * @brief Classic random map landform generation: coastlines, mountain/hill ridges, and erosion.
 */
#include <algorithm>
#include <cstdlib>

#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

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

bool Generator::OnEdge(Point point) {
    return point.x == 0 || point.y == 0 || point.x == kMapW - 1 || point.y == kMapH - 1;
}

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

Point Generator::Turn(Point dir) {
    int idx = NbIndex(dir.x, dir.y);
    int next_dir = Dice(1, 5, (idx < 0 ? 8 : idx) - 2) % 8;
    return Point{Nx(next_dir), Ny(next_dir)};
}

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

int Generator::EdgeOf(Point point) {
    if (point.x == 0) return 3;
    if (point.y == 0) return 0;
    if (point.x == kMapW - 1) return 1;
    if (point.y == kMapH - 1) return 2;
    return -1;
}

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

void Generator::CoastAlongEdge(Point from, Point to_pt) {
    int edge_a = EdgeOf(from), edge_b = EdgeOf(to_pt);
    if (edge_a == edge_b) return Straight(from, to_pt);
    int apart = std::abs(edge_a - edge_b);
    if (apart == 2) return CoastLine(from, to_pt);
    Point corner = Pt(kCorners + 4 * (apart == 3 ? 0 : std::max(edge_a, edge_b)));
    Straight(from, corner);
    Straight(to_pt, corner);
}

void Generator::CoastSegment(Point from, Point to_pt) {
    if (OnEdge(from) && OnEdge(to_pt))
        CoastAlongEdge(from, to_pt);
    else
        CoastLine(from, to_pt);
}

void Generator::CoastJoin() {
    for (int i = 0; i < 4; i++) {
        int base = kEdgePts + 0x50 * i, count = R16(kEdgeCount + 2 * i);
        int idx = 0;
        for (; idx < count - 1; idx++) CoastSegment(Pt(base + 4 * idx), Pt(base + 4 * (idx + 1)));
        int next_edge = (i + 1) % 4;
        CoastSegment(Pt(base + 4 * idx), Pt(kEdgePts + 0x50 * next_edge));
    }
}

bool Generator::Floods(int x, int y) const {
    int terrain = At(x, y);
    if (terrain == kShore || terrain == kBridge) return false;
    bool inside = x != 0 && y != 0 && x != kMapW - 1 && y != kMapH - 1;
    if (!inside && terrain == 0) return true;
    return At(x + 1, y) == kWater || At(x - 1, y) == kWater || At(x, y + 1) == kWater ||
           At(x, y - 1) == kWater;
}

void Generator::CoastFill() {
    for (int i = 0; i < kMapW * kMapH; i++)
        memory_[kGrid + i] = memory_[kGrid + i] == kPlain ? kShore : 0;
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
        memory_[kGrid + i] = memory_[kGrid + i] == kWater ? kWater : kPlain;
}

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

void Generator::Coast() {
    for (int i = 0; i < 4; i++) CoastEnds(i);
    CoastOrder();
    for (int i = 0; i < 4; i++) CoastPoints(i);
    CoastJoin();
    CoastFill();
    Shores();
    if (Dice(1, 100, 0) < 50) {
        // 0-2 channels straight across, west to east.
        int channels = Dice(1, 3, -1);
        for (int i = 0; i < channels; i++) River(true, false, true);
        WaterTidy();
    }
    Shores();
}

// --- mountains and hills ------------------------------------------------

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
            lay(side2.x, side2.y);  // Matches the reference: clamps only the first side tile.
        }
        if (width == 3) {
            Clamp(outer1);
            lay(outer1.x, outer1.y);
            lay(outer2.x, outer2.y);
        }
        if (done) return;
    }
}

void Generator::Blob(Point start_pt, int terrain) {
    Point end_pt;
    end_pt.x = start_pt.x + Dice(1, 5, -10);
    end_pt.y = start_pt.y + Dice(1, 5, -10);
    Clamp(end_pt);
    int width = Dice(1, 2, 1);
    Ridge(start_pt, end_pt, width, terrain);
}

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

void Generator::Foothills() {
    for (int x = 0; x < kMapW; x++) {
        for (int y = 0; y < kMapH; y++) {
            int terrain = At(x, y);
            if (terrain != kMountains && terrain != kHills && Near(x, y, kMountains))
                Put(x, y, kHills);
        }
    }
}

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
            for (const auto& off : offs) {
                Point pos{cur.x + off[0], cur.y + off[1]};
                Clamp(pos);
                Put(pos.x, pos.y, target_terr);
            }
        }
        cur.x += dir.x;
        cur.y += dir.y;
    }
}

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

void Generator::Erosion() {
    for (int i = 0; i < R16(kPErosion); i++) Wear(false);
    for (int i = 0; i < R16(kPPasses); i++) Wear(true);
    Surrounded(kHills, kMountains);
    Surrounded(kPlain, kHills);
    Foothills();
}

}  // namespace w2::randommap
