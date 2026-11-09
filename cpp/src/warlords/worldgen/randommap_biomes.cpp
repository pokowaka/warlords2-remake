/**
 * @file
 * @brief Classic random map hydrology and flora passes: rivers, water tidying, forests, and
 *        marshes.
 */
#include "warlords/worldgen/randommap_impl.hpp"

namespace w2::randommap {

// --- rivers --------------------------------------------------------------

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
            // Matches the reference: dir_idx == -1 reads the word before the neighbour table.
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

void Generator::Forests() {
    int land = 0;
    for (int i = 0; i < kMapW * kMapH; i++) {
        int terrain = memory_[kGrid + i];
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

void Generator::Marshes() {
    int count = Dice(1, 3, 0);
    for (int i = 0; i < count; i++) Marsh();
}

}  // namespace w2::randommap
