// The random map generator: see randommap.hpp and docs/re/random_map.md.
//
// RANDOM\RANDOM.DAT is both the generator's parameters and its memory: the
// original reads the whole 0xa560 bytes into one buffer and works in it, the
// 112x156 grid of terrain types at +0x6120 included. This does the same, so
// the offsets below are RANDOM.DAT's own, and the few reads the original makes
// off the edge of the grid land where they did. Writes off the grid are
// dropped; the original's would corrupt its own tables.
//
// Every roll is its own statement: C++ leaves the order of a call's arguments
// and of an operator's operands open, and the dice must fall in the order the
// web port's do.
#include "warlords/randommap.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include "util/util.hpp"
#include "warlords/armytype.hpp"
#include "warlords/bytes.hpp"
#include "warlords/move.hpp"
#include "warlords/travel.hpp"
#include "warlords/types.hpp"

namespace w2::randommap {

const char* const FILES[6] = {"SCN", "MAP", "RD", "SGN", "CTY", "SPC"};
const int SLIDER_SHOWS[4][7] = {{5, 7, 9, 11, 13, 15, 17},
                                {5, 7, 9, 11, 13, 15, 17},
                                {70, 75, 80, 85, 90, 95, 100},
                                {9, 11, 13, 15, 17, 19, 21}};
const char* const SLIDER_FORMATS[4] = {"(%d%%)", "(%d%%)", "(%d)", "(%d%%)"};

namespace {

constexpr int kW = 112, kH = 156;

// terrain type ids, as the grid and the scenario's tile table number them
constexpr int kRoad = 0, kBridge = 1, kWater = 2, kShore = 3, kForest = 4, kHills = 5;
constexpr int kMountains = 6, kPlain = 7, kMarsh = 8, kTower = 9, kCity = 10, kSite = 11;

// RANDOM.DAT: the parameters (the sliders add to these)
constexpr int kPCities = 0x2a, kPEdgePoints = 0x2c;
constexpr int kPMountains = 0x34, kPHills = 0x36, kPWideRivers = 0x38, kPRivers = 0x3a;
constexpr int kPForest = 0x3c, kPPasses = 0x3e, kPErosion = 0x40;
constexpr int kSliderHills = 0x44, kSliderWater = 0x52, kSliderForest = 0x60, kSliderCities = 0x6e;
constexpr int kCorners = 0x7c, kEdges = 0x8c, kDiagonals = 0xac;
constexpr int kNb = 0xbc;      // the 8 neighbours, S first and anticlockwise
constexpr int kNbRoad = 0xdc;  // the 8 again, N first and clockwise: road shapes
// RANDOM.DAT: the generator's working space and its tables
constexpr int kEdgeCount = 0xfc, kEdgePts = 0x104, kEdgeStart = 0x244, kEdgeEnd = 0x254;
constexpr int kShape = 0x264;      // neighbour mask -> tile variant, -1 for none
constexpr int kRoadShape = 0x364;  // neighbour mask -> road id - 1, -1 for none
constexpr int kTiles = 0x464;      // per terrain type, 16 variants x 2 checkerboard tiles
constexpr int kSideNames = 0x764, kSiteFormats = 0xed2;
constexpr int kSignCity = 0x1888, kSignLeagues = 0x189c, kSigns = 0x18b0;
constexpr int kSideClass = 0x1d62, kRegionClass = 0x1d72, kProduction = 0x1d82;
constexpr int kForestDone = 0x1f52, kForestWanted = 0x1f54;
constexpr int kNodes = 0x5dd6, kNodeCount = 0x611e, kGrid = 0x6120;
constexpr int kDatSize = 0xa560;

// .SCN offsets (docs/formats/scenario.md)
constexpr int kSSideRec = 387, kSRandom = 0x120, kSTerrainSet = 0x161;
constexpr int kSTerrain = 0x710, kSSiteCount = 0x80f, kSSites = 0x811, kSiteStride = 31;
constexpr int kSCityCount = 5499, kSCities = 5501, kCityStride = 65;
constexpr int kNSites = 40;

// directions the signposts give (4125:2b7c)
const char* const kCompass[8] = {"north", "northeast", "east", "southeast",
                                 "south", "southwest", "west", "northwest"};
// the twelve tiles round a city's 2x2 footprint, where its roads start (DS:0382)
const int kRoundCity[12][2] = {{-1, -1}, {0, -1}, {1, -1}, {2, -1}, {2, 0},  {2, 1},
                               {2, 2},   {1, 2},  {0, 2},  {-1, 2}, {-1, 1}, {-1, 0}};
// the road builder's terrain costs, pseudo-player 14 (DS:01e0)
const int kRoadCost[12] = {1, 1, 3, 3, 4, 5, 7, 2, 5, 2, 1, 7};
const int kFootprint[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};  // DS:02c4, 02cc
constexpr int kSafety = 200000;  // a loop the original might never leave

struct P {
    int x = 0, y = 0;
};

/** Chebyshev distance (4c49:0fed). */
int Reach(int x1, int y1, int x2, int y2) {
    return std::max(std::abs(x1 - x2), std::abs(y1 - y2));
}

/** map_distance (1a8b:0acc): the straight line, rounded down. */
int Distance(int x1, int y1, int x2, int y2) {
    double dx = x1 - x2, dy = y1 - y2;
    return static_cast<int>(std::floor(std::sqrt(dx * dx + dy * dy)));
}

/** 4c49:0dd0: the step from one tile towards another, one of 8 compass
 *  directions by the slope, cut at tan 22.5 and tan 67.5 (4125:0292). */
P Step(int x1, int y1, int x2, int y2) {
    if (x2 == x1) return P{0, y2 == y1 ? 0 : (y2 < y1 ? -1 : 1)};
    if (y2 == y1) return P{x2 < x1 ? -1 : 1, 0};
    double s = static_cast<double>(y2 - y1) / static_cast<double>(x2 - x1);
    if (x2 < x1) {
        if (s >= 2.414) return P{0, -1};
        if (s >= 0.414) return P{-1, -1};
        if (s >= -0.414) return P{-1, 0};
        if (s >= -2.414) return P{-1, 1};
        return P{0, 1};
    }
    if (s >= 2.414) return P{0, 1};
    if (s >= 0.414) return P{1, 1};
    if (s >= -0.414) return P{1, 0};
    if (s >= -2.414) return P{1, -1};
    return P{0, -1};
}

/** The direction from one tile to another for a signpost (828e:0b51). */
int Compass(int x1, int y1, int x2, int y2) {
    if (x1 == x2) return y1 < y2 ? 4 : 0;
    if (y1 == y2) return x1 < x2 ? 2 : 6;
    if (x2 < x1 && y2 < y1) return 7;
    if (x2 < x1 && y1 < y2) return 5;
    if (x1 < x2 && y2 < y1) return 1;
    return 3;
}

/** The checkerboard: 1 where x + y is even (C's (n)/2 != (n-1)/2). */
int Parity(int x, int y) {
    int n = x + y;
    return n == 0 || n / 2 != (n - 1) / 2 ? 1 : 0;
}

void Clamp(P& p) {  // 4bed:0470
    if (p.x < 1) p.x = 0;
    if (p.y < 1) p.y = 0;
    if (p.x > kW - 1) p.x = kW - 1;
    if (p.y > kH - 1) p.y = kH - 1;
}

int I8(int b) {
    return b > 127 ? b - 256 : b;
}

int GetI16(const std::vector<uint8_t>& t, int o) {
    return static_cast<int16_t>(t[o] | (t[o + 1] << 8));
}
void SetI16(std::vector<uint8_t>& t, int o, int v) {
    t[o] = static_cast<uint8_t>(v & 0xff);
    t[o + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
}
std::string CstrOf(const std::vector<uint8_t>& t, int o, int n) {
    std::string s;
    for (int i = o; i < o + n && i < static_cast<int>(t.size()); i++) {
        if (t[i] == 0) break;
        s += static_cast<char>(t[i]);
    }
    return s;
}

struct SpotList {
    int x[50], y[50], ew[50], city_dist[50], chosen[50], chosen_dist[50];
};

struct Surroundings {
    bool shore, forest, road, hills, marsh;
};

}  // namespace

class Generator {
  public:
    std::vector<uint8_t> m, scn;
    Types types;
    Rng* rng;
    bool allies;
    bool fair = false;        // Options::fairCapitals
    bool modern = false;      // Options::scenario: the scenario generator
    scengen::Settings world;  // Options::world
    scengen::World plan;      // what the scenario generator made
    std::vector<uint16_t> tiles;
    std::vector<uint8_t> rd;
    std::string cty, spc, sgn;
    int site_cell = -1;  // 4125:02d4, where the sites' round begins

    // the phases, as the original's progress bar moves through them
    int stage = 0;
    int site_i = 0, tenth = 0;
    std::vector<uint8_t> served;
    int shown = 4;
    std::optional<std::pair<int, int>> pending;

    // --- memory -------------------------------------------------------------

    int R16(int o) const { return GetI16(m, o); }
    void W16(int o, int v) { SetI16(m, o, v); }
    std::string Str(int o, int n) const { return CstrOf(m, o, n); }
    int S16(int o) const { return GetI16(scn, o); }
    void Ws16(int o, int v) { SetI16(scn, o, v); }
    void Wstr(int o, const std::string& s) {  // strcpy into the scenario image
        for (size_t i = 0; i < s.size(); i++) scn[o + i] = static_cast<uint8_t>(s[i]);
        scn[o + s.size()] = 0;
    }

    /** The terrain grid. Off the grid a read gets whatever RANDOM.DAT holds
     *  there, as the original's did. */
    int At(int x, int y) const {
        int o = kGrid + y * kW + x;
        return o >= 0 && o < kDatSize ? m[o] : 0;
    }
    void Put(int x, int y, int t) {
        int i = y * kW + x;
        if (i >= 0 && i < kW * kH) m[kGrid + i] = static_cast<uint8_t>(t);
    }
    int Nx(int i) const { return R16(kNb + 4 * i); }
    int Ny(int i) const { return R16(kNb + 4 * i + 2); }
    /** The index of a step among the 8 neighbours, or -1. */
    int NbIndex(int dx, int dy) const {
        int k = -1;
        for (int i = 0; i < 8; i++)
            if (Nx(i) == dx && Ny(i) == dy) k = i;
        return k;
    }

    // the tile map: the low byte is the tile, bit 15 a crossing
    int Tile(int x, int y) const {
        return x >= 0 && y >= 0 && x < kW && y < kH ? tiles[y * kW + x] & 0xff : 0;
    }
    void SetTile(int x, int y, int t) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        uint16_t& v = tiles[y * kW + x];
        v = static_cast<uint16_t>((v & 0xff00) | (t & 0xff));
    }
    int Road(int x, int y) const {
        return x >= 0 && y >= 0 && x < kW && y < kH ? rd[y * kW + x] & 0x1f : 0;
    }
    void SetRoad(int x, int y, int r) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        uint8_t& v = rd[y * kW + x];
        v = static_cast<uint8_t>((v & 0xe0) | (r & 0x1f));
    }
    int TerrainOf(int t) const { return scn[kSTerrain + t]; }
    /** Variant `v` of terrain type `t` from RANDOM.DAT's tile table, the
     *  checkerboard picking one of its pair. */
    int TileFor(int t, int v, int x, int y) const {
        return R16(kTiles + 64 * t + 4 * v + 2 * Parity(x, y)) & 0xff;
    }

    // the cities, as the scenario holds them
    int CityCount() const { return S16(kSCityCount); }
    void SetCityCount(int n) { Ws16(kSCityCount, n); }
    int Crec(int i) const { return kSCities + kCityStride * i; }
    int Cx(int i) const { return S16(Crec(i)); }
    int Cy(int i) const { return S16(Crec(i) + 2); }

    /** dice(n, sides, bonus) (6ecb:02bf): no sides gives the bonus, and the
     *  clamp makes fewer than none give n + bonus. */
    int Dice(int n, int sides, int bonus = 0) {
        if (sides == 0) return bonus;
        if (sides < 0) return n + bonus;
        return rng->dice(n, sides, bonus);
    }

    /** terrain_near (4c49:11f5): is type `t` on any of the 8 tiles round. */
    bool Near(int x, int y, int t) const {
        for (int i = 0; i < 8; i++) {
            int ax = x + Nx(i), ay = y + Ny(i);
            if (ax >= 0 && ay >= 0 && ax < kW && ay < kH && At(ax, ay) == t) return true;
        }
        return false;
    }
    bool NearPlain(int x, int y) const { return Near(x, y, kPlain); }

    /** 4bed:04ae: push a point by (1d3-2) x 1dn each way; with `toEdge`, 30%
     *  of the time it then goes to the nearest edge. Clamped to the map. */
    void Jitter(P& p, int n, bool to_edge) {
        int s1 = Dice(1, 3, -2);
        p.x += s1 * Dice(1, n, 0);
        int s2 = Dice(1, 3, -2);
        p.y += s2 * Dice(1, n, 0);
        if (Dice(1, 100, 0) < 30 && to_edge) {
            int l = p.x, t = p.y, r = kW - 1 - p.x, b = kH - 1 - p.y;
            if (l <= t && l <= r && l <= b) p.x = 0;
            if (t <= l && t <= r && t <= b) p.y = 0;
            if (r <= t && r <= l && r <= b) p.x = kW - 1;
            if (b <= t && b <= l && b <= r) p.y = kH - 1;
        }
        Clamp(p);
    }

    // --- 4c49: the coastline --------------------------------------------------

    P Pt(int o) const { return P{R16(o), R16(o + 2)}; }
    void SetPt(int o, P p) {
        W16(o, p.x);
        W16(o + 2, p.y);
    }

    /** 4c49:0087: where edge `i`'s stretch of coast begins and ends -- the
     *  map's corners drawn in 10 along the diagonal, then pushed about. */
    void CoastEnds(int i) {
        int j = (i + 1) % 4;
        if (R16(kPEdgePoints + 2 * i) < 3) {
            SetPt(kEdgeStart + 4 * i, Pt(kEdges + 8 * i));
            SetPt(kEdgeEnd + 4 * i, Pt(kEdges + 8 * i + 4));
            return;
        }
        P a = Pt(kEdges + 8 * i);
        a.x -= 10 * R16(kDiagonals + 4 * i);
        a.y -= 10 * R16(kDiagonals + 4 * i + 2);
        Jitter(a, 16, true);
        Put(a.x, a.y, kPlain);
        SetPt(kEdgeStart + 4 * i, a);
        P b = Pt(kEdges + 8 * i + 4);
        b.x -= 10 * R16(kDiagonals + 4 * j);
        b.y -= 10 * R16(kDiagonals + 4 * j + 2);
        Jitter(b, 16, true);
        Put(b.x, b.y, kPlain);
        SetPt(kEdgeEnd + 4 * i, b);
    }

    /** 4c49:0206: swap an edge's end with the next one's start where that
     *  keeps the outline turning round the middle. */
    void CoastOrder() {
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            P e = Pt(kEdgeEnd + 4 * i), s = Pt(kEdgeStart + 4 * j);
            if (static_cast<double>(56 - e.x) / static_cast<double>(78 - e.y) <
                static_cast<double>(56 - s.x) / static_cast<double>(78 - s.y)) {
                SetPt(kEdgeEnd + 4 * i, s);
                SetPt(kEdgeStart + 4 * j, e);
            }
        }
    }

    static bool OnEdge(P p) { return p.x == 0 || p.y == 0 || p.x == kW - 1 || p.y == kH - 1; }

    /** 4c49:02f9: the points along edge `i`, each a step on from the last
     *  towards the end and pushed about by up to half a step. */
    void CoastPoints(int i) {
        int base = kEdgePts + 0x50 * i, want = R16(kPEdgePoints + 2 * i);
        P s = Pt(kEdgeStart + 4 * i), e = Pt(kEdgeEnd + 4 * i);
        SetPt(base, s);
        int n = 1;
        if (want < 3 || (OnEdge(s) && OnEdge(e))) {
            SetPt(base + 4, e);
            W16(kEdgeCount + 2 * i, 2);
            return;
        }
        int len = Reach(e.x, e.y, s.x, s.y) / (want - 1);
        int k = 0;
        for (;;) {
            P p = Pt(base + 4 * k);
            P d = Step(p.x, p.y, e.x, e.y);
            P q{p.x + d.x * len, p.y + d.y * len};
            Jitter(q, len / 2, false);
            k++;
            Put(q.x, q.y, kPlain);
            SetPt(base + 4 * k, q);
            n++;
            if (Reach(e.x, e.y, q.x, q.y) < len || n >= want - 1) break;
        }
        SetPt(base + 4 * (k + 1), e);
        W16(kEdgeCount + 2 * i, n + 1);
    }

    /** 4c49:104a: turn a step by -1 to +3 eighths. */
    P Turn(P d) {
        int k = NbIndex(d.x, d.y);
        int i = Dice(1, 5, (k < 0 ? 8 : k) - 2) % 8;
        return P{Nx(i), Ny(i)};
    }

    /** 4c49:0a0e: a wandering line of plain from a to b. */
    void CoastLine(P a, P b) {
        P cur = a;
        P first = Step(a.x, a.y, b.x, b.y);
        for (int n = 0; n < kSafety; n++) {
            P d = Step(cur.x, cur.y, b.x, b.y);
            int r = Reach(cur.x, cur.y, b.x, b.y) < 3 ? 1 : Dice(1, 3, 0);
            if (r == 2)
                d = first;
            else if (r == 3) {
                d = Turn(d);
                first = d;
            }
            cur.x += d.x;
            cur.y += d.y;
            Clamp(cur);
            Put(cur.x, cur.y, kPlain);
            if (cur.x == b.x && cur.y == b.y) return;
        }
    }

    /** 4c49:1016: which edge a point lies on, -1 for none. */
    static int EdgeOf(P p) {
        if (p.x == 0) return 3;
        if (p.y == 0) return 0;
        if (p.x == kW - 1) return 1;
        if (p.y == kH - 1) return 2;
        return -1;
    }

    /** A straight run of plain from a to b, one step each. */
    void Straight(P a, P b) {
        P d = Step(a.x, a.y, b.x, b.y);
        P cur = a;
        for (int n = 0; n < kSafety && !(cur.x == b.x && cur.y == b.y); n++) {
            cur.x += d.x;
            cur.y += d.y;
            Clamp(cur);
            Put(cur.x, cur.y, kPlain);
        }
    }

    /** 4c49:0b0e: two points on the map's edge are joined along it, round a
     *  corner if need be; across the map they wander as any other. */
    void CoastAlongEdge(P a, P b) {
        int ea = EdgeOf(a), eb = EdgeOf(b);
        if (ea == eb) return Straight(a, b);
        int apart = std::abs(ea - eb);
        if (apart == 2) return CoastLine(a, b);
        P corner = Pt(kCorners + 4 * (apart == 3 ? 0 : std::max(ea, eb)));
        Straight(a, corner);
        Straight(b, corner);
    }

    void CoastSegment(P a, P b) {
        if (OnEdge(a) && OnEdge(b))
            CoastAlongEdge(a, b);
        else
            CoastLine(a, b);
    }

    /** 4c49:05c7: join every point of the outline to the next. */
    void CoastJoin() {
        for (int i = 0; i < 4; i++) {
            int base = kEdgePts + 0x50 * i, n = R16(kEdgeCount + 2 * i);
            int k = 0;
            for (; k < n - 1; k++) CoastSegment(Pt(base + 4 * k), Pt(base + 4 * (k + 1)));
            int j = (i + 1) % 4;
            CoastSegment(Pt(base + 4 * k), Pt(kEdgePts + 0x50 * j));
        }
    }

    /** 4c49:10bd: may the sea flood into this tile? */
    bool Floods(int x, int y) const {
        int t = At(x, y);
        if (t == kShore || t == kBridge) return false;
        bool inside = x != 0 && y != 0 && x != kW - 1 && y != kH - 1;
        if (!inside && t == 0) return true;
        return At(x + 1, y) == kWater || At(x - 1, y) == kWater || At(x, y + 1) == kWater ||
               At(x, y - 1) == kWater;
    }

    /** 4c49:0867: the sea floods in from the edges up to the outline; what it
     *  does not reach is land. */
    void CoastFill() {
        for (int i = 0; i < kW * kH; i++) m[kGrid + i] = m[kGrid + i] == kPlain ? kShore : 0;
        auto flood = [&](int x, int y) {
            if (Floods(x, y)) Put(x, y, kWater);
        };
        for (int pass = 0; pass < 2; pass++) {
            for (int x = 0; x < kW; x++)
                for (int y = 0; y < kH; y++) flood(x, y);
            for (int y = 0; y < kH; y++)
                for (int x = 0; x < kW; x++) flood(x, y);
            for (int x = kW - 1; x >= 0; x--)
                for (int y = kH - 1; y >= 0; y--) flood(x, y);
            for (int y = kH - 1; y >= 0; y--)
                for (int x = kW - 1; x >= 0; x--) flood(x, y);
        }
        for (int i = 0; i < kW * kH; i++) m[kGrid + i] = m[kGrid + i] == kWater ? kWater : kPlain;
    }

    /** 4c49:0ce7: water beside land is shore. */
    void Shores() {
        for (int x = 0; x < kW; x++) {
            for (int y = 0; y < kH; y++) {
                if (At(x, y) != kWater) continue;
                if (NearPlain(x, y)) Put(x, y, kShore);
                if (Near(x, y, kHills)) Put(x, y, kShore);
                if (Near(x, y, kMountains)) Put(x, y, kShore);
            }
        }
    }

    /** 4c49:0000: the land. */
    void Coast() {
        for (int i = 0; i < 4; i++) CoastEnds(i);
        CoastOrder();
        for (int i = 0; i < 4; i++) CoastPoints(i);
        CoastJoin();
        CoastFill();
        Shores();
        if (Dice(1, 100, 0) < 50) {
            // 4c49:0daa: 0-2 channels straight across, west to east
            int n = Dice(1, 3, -1);
            for (int i = 0; i < n; i++) River(true, false, true);
            WaterTidy();
        }
        Shores();
    }

    // --- 4d71: mountains and hills -----------------------------------------

    int Node(int i) const { return kNodes + 14 * i; }

    /** 4d71:0495 / 03af: seeds on random plain tiles, each a node. */
    void Seeds(int n, int t, int kind) {
        for (int k = 0; k < n; k++) {
            int x = 0, y = 0;
            for (int tries = 0; tries < kSafety; tries++) {
                x = Dice(1, kW, -1);
                y = Dice(1, kH, -1);
                if (At(x, y) == kPlain) break;
            }
            Put(x, y, t);
            int c = R16(kNodeCount), o = Node(c);
            W16(o, x);
            W16(o + 2, y);
            W16(o + 4, kind);
            W16(o + 6, 0);
            W16(kNodeCount, c + 1);
        }
    }

    /** 4d71:057b: each node is linked to its nearest others -- a hill to 0-1,
     *  a mountain to 0-3. */
    void Link() {
        int count = R16(kNodeCount);
        if (count < 2) return;
        for (int i = 0; i < count; i++) {
            int o = Node(i);
            int n = R16(o + 4) == 1 ? Dice(1, 2, -1) : Dice(1, 4, -1);
            if (n >= count - 1) n = count - 1;
            W16(o + 6, n);
            for (int l = 0; l < n; l++) {
                int best = 10000, best_j = -1;
                for (int j = 0; j < count; j++) {
                    if (j == i) continue;
                    int k = 0;
                    while (k < l && R16(o + 8 + 2 * k) != j) k++;
                    if (k != l) continue;
                    int p = Node(j);
                    int d = Reach(R16(p), R16(p + 2), R16(o), R16(o + 2));
                    if (d < best) {
                        best = d;
                        best_j = j;
                    }
                }
                W16(o + 8 + 2 * l, best_j);
            }
        }
    }

    /** 4d71:09a7: a ridge from a to b, wandering as the coast does; `width` 2
     *  adds a tile to either side, 3 one two out. Only plain and hills give
     *  way to it. */
    void Ridge(P a, P b, int width, int t) {
        P cur = a;
        P first = Step(a.x, a.y, b.x, b.y);
        int k = 0;
        auto lay = [&](int x, int y) {
            int v = At(x, y);
            if (v == kPlain || v == kHills) Put(x, y, t);
        };
        for (int n = 0; n < kSafety; n++) {
            P d = Step(cur.x, cur.y, b.x, b.y);
            int r = Reach(cur.x, cur.y, b.x, b.y) < 3 ? 1 : Dice(1, 3, 0);
            if (r == 2)
                d = first;
            else if (r == 3) {
                d = Turn(d);
                first = d;
            }
            cur.x += d.x;
            cur.y += d.y;
            int ki = NbIndex(d.x, d.y);
            if (ki >= 0) k = ki;
            int l = (k + 2) % 8, rr = (k + 6) % 8;
            P p1{cur.x + Nx(l), cur.y + Ny(l)};
            P p2{cur.x + Nx(rr), cur.y + Ny(rr)};
            P p3{cur.x + 2 * Nx(l), cur.y + 2 * Ny(l)};
            P p4{cur.x + 2 * Nx(rr), cur.y + 2 * Ny(rr)};
            Clamp(cur);
            lay(cur.x, cur.y);
            bool done = cur.x == b.x && cur.y == b.y;
            if (width == 2) {
                Clamp(p1);
                lay(p1.x, p1.y);
                lay(p2.x, p2.y);  // the original clamps only the one
            }
            if (width == 3) {
                Clamp(p3);
                lay(p3.x, p3.y);
                lay(p4.x, p4.y);
            }
            if (done) return;
        }
    }

    /** 4d71:0884 / 08f3: a node with no links gets a short ridge of its own,
     *  up and to the left. */
    void Blob(P p, int t) {
        P q;
        q.x = p.x + Dice(1, 5, -10);
        q.y = p.y + Dice(1, 5, -10);
        Clamp(q);
        int width = Dice(1, 2, 1);
        Ridge(p, q, width, t);
    }

    /** 4d71:06d2: the ridges along the links. */
    void Ridges() {
        int count = R16(kNodeCount);
        if (count < 2) return;
        for (int i = 0; i < count; i++) {
            int o = Node(i);
            P p{R16(o), R16(o + 2)};
            bool hill = R16(o + 4) == 1;
            int links = R16(o + 6);
            if (links == 0) {
                Blob(p, hill ? kHills : kMountains);
                continue;
            }
            for (int l = 0; l < links; l++) {
                int q = Node(R16(o + 8 + 2 * l));
                P to{R16(q), R16(q + 2)};
                if (hill && R16(q + 4) == 1)
                    Ridge(p, to, 3, kHills);
                else {
                    int width = Dice(1, 2, 1);
                    Ridge(p, to, width, kMountains);
                }
            }
        }
    }

    /** 4d71:002e: plain hemmed in by mountains (or hills) on all four sides
     *  joins them, mountains on the shore come down, and a diagonal run of
     *  three mountains is thickened on one side. */
    void MountainTidy() {
        for (int x = 0; x < kW; x++) {
            for (int y = 0; y < kH; y++) {
                if (x != 0 && y != 0 && x != kW - 1 && y != kH - 1 && At(x, y) == kPlain) {
                    for (int t : {kMountains, kHills}) {
                        if (At(x, y + 1) == t && At(x + 1, y) == t && At(x - 1, y) == t &&
                            At(x, y - 1) == t)
                            Put(x, y, t);
                    }
                }
                if (At(x, y) == kMountains && Near(x, y, kShore)) Put(x, y, kPlain);
            }
        }
        for (int x = 1; x < kW - 1; x++) {
            for (int y = 1; y < kH - 1; y++) {
                if (At(x, y) != kMountains) continue;
                auto mm = [&](int dx, int dy) { return At(x + dx, y + dy) == kMountains; };
                if (mm(1, 1) && mm(-1, -1) && !mm(1, -1) && !mm(-1, 1)) {
                    if (Dice(1, 10, 0) > 5)
                        Put(x - 1, y + 1, kMountains);
                    else
                        Put(x + 1, y - 1, kMountains);
                } else if (mm(-1, 1) && mm(1, -1) && !mm(-1, -1) && !mm(1, 1)) {
                    if (Dice(1, 10, 0) > 5)
                        Put(x + 1, y + 1, kMountains);
                    else
                        Put(x - 1, y - 1, kMountains);
                }
            }
        }
    }

    /** 4d71:033a: mountains are ringed with hills. */
    void Foothills() {
        for (int x = 0; x < kW; x++) {
            for (int y = 0; y < kH; y++) {
                int t = At(x, y);
                if (t != kMountains && t != kHills && Near(x, y, kMountains)) Put(x, y, kHills);
            }
        }
    }

    /** 4d71:0000. */
    void Highlands() {
        W16(kNodeCount, 0);
        Seeds(R16(kPMountains), kMountains, 2);
        Seeds(R16(kPHills), kHills, 1);
        Link();
        Ridges();
        MountainTidy();
        Foothills();
    }

    // --- 4f5f: erosion and passes --------------------------------------------

    /** 4f5f:0044: a walk between two random tiles; erosion wears its
     *  mountains to hills, a pass cuts mountains and hills to plain, five tiles
     *  across. */
    void Wear(bool pass) {
        P a, b;
        a.x = Dice(1, kW, -1);
        a.y = Dice(1, kH, -1);
        b.x = Dice(1, kW, -1);
        b.y = Dice(1, kH, -1);
        P cur = a;
        while (!(cur.x == b.x && cur.y == b.y)) {
            P d = Step(cur.x, cur.y, b.x, b.y);
            int k = NbIndex(d.x, d.y);
            int l = (k + 1) % 8, r = (k + 7) % 8;
            int t = At(cur.x, cur.y);
            bool hit = pass ? t == kMountains || t == kHills : t == kMountains;
            if (hit) {
                int to = pass ? kPlain : kHills;
                const int offs[5][2] = {{0, 0},
                                        {Nx(l), Ny(l)},
                                        {Nx(r), Ny(r)},
                                        {2 * Nx(l), 2 * Ny(l)},
                                        {2 * Nx(r), 2 * Ny(r)}};
                for (auto& o : offs) {
                    P p{cur.x + o[0], cur.y + o[1]};
                    Clamp(p);
                    Put(p.x, p.y, to);
                }
            }
            cur.x += d.x;
            cur.y += d.y;
        }
    }

    /** Hills with three mountains on their four sides become mountain
     *  (4f5f:04d6); plain with three hills, hills (05ae). */
    void Surrounded(int t, int by) {
        for (int x = 1; x < kW - 1; x++) {
            for (int y = 1; y < kH - 1; y++) {
                if (At(x, y) != t) continue;
                int n = (At(x, y + 1) == by) + (At(x, y - 1) == by) + (At(x + 1, y) == by) +
                        (At(x - 1, y) == by);
                if (n > 2) Put(x, y, by);
            }
        }
    }

    /** 4f5f:0000. */
    void Erosion() {
        for (int i = 0; i < R16(kPErosion); i++) Wear(false);
        for (int i = 0; i < R16(kPPasses); i++) Wear(true);
        Surrounded(kHills, kMountains);
        Surrounded(kPlain, kHills);
        Foothills();
    }

    // --- 4eb7: rivers ----------------------------------------------------------

    /** 4eb7:005d: a river. Without `channel` it runs from a random hill or
     *  mountain to a random water or shore tile, 200 tries each; a channel
     *  runs from the west edge to the east. Each step lays water three tiles
     *  across (`wide`: five, a channel seven), turns aside a quarter of the
     *  time each way, and with `stops` the river ends on meeting water the
     *  second time. */
    void River(bool wide, bool stops, bool channel) {
        P a, b;
        if (channel) {
            a.x = 0;
            a.y = Dice(1, kH, -1);
            b.x = kW - 1;
            b.y = Dice(1, kH, -1);
        } else {
            for (int i = 0; i < 200; i++) {
                a.x = Dice(1, kW, -1);
                a.y = Dice(1, kH, -1);
                int t = At(a.x, a.y);
                if (t == kHills || t == kMountains) break;
            }
            for (int i = 0; i < 200; i++) {
                b.x = Dice(1, kW, -1);
                b.y = Dice(1, kH, -1);
                int t = At(b.x, b.y);
                if (t == kWater || t == kShore) break;
            }
        }
        P cur = a;
        int met = 0;
        bool done = cur.x == b.x && cur.y == b.y;
        for (int n = 0; !done && n < kSafety; n++) {
            P d = Step(cur.x, cur.y, b.x, b.y);
            int k = NbIndex(d.x, d.y);
            int l = (k + 1) % 8, r = (k + 7) % 8, aside1 = (k + 2) % 8, aside2 = (k + 6) % 8;
            if (stops) {
                // nb(-1) reads the word before the table, as the original's did
                int ahead = At(cur.x + Nx(k), cur.y + Ny(k));
                if (ahead == kWater && met == 1) done = true;
                if (ahead == kWater && met == 0) met++;
            }
            auto lay = [&](int ox, int oy) {
                P p{cur.x + ox, cur.y + oy};
                Clamp(p);
                Put(p.x, p.y, kWater);
            };
            lay(0, 0);
            lay(Nx(l), Ny(l));
            lay(Nx(r), Ny(r));
            if (wide) {
                lay(2 * Nx(l), 2 * Ny(l));
                lay(2 * Nx(r), 2 * Ny(r));
                if (channel) {
                    lay(3 * Nx(l), 3 * Ny(l));
                    lay(3 * Nx(r), 3 * Ny(r));
                }
            }
            cur.x += d.x;
            cur.y += d.y;
            if (cur.x == b.x && cur.y == b.y) done = true;
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

    static bool Wet(int t) { return t == kShore || t == kWater; }

    /** 4eb7:060c: land with water on three of its four sides goes under, shore
     *  out of sight of land becomes open water, and a diagonal of water is
     *  given a shore tile beside it. */
    void WaterTidy() {
        auto land = [](int t) { return t == kPlain || t == kHills || t == kMountains; };
        for (int x = 0; x < kW; x++) {
            for (int y = 0; y < kH; y++) {
                if (!land(At(x, y))) continue;
                int n = Wet(At(x, y + 1)) + Wet(At(x, y - 1)) + Wet(At(x + 1, y)) +
                        Wet(At(x - 1, y));
                if (n > 2) Put(x, y, kWater);
            }
        }
        for (int x = 0; x < kW; x++) {
            for (int y = 0; y < kH; y++) {
                if (At(x, y) == kShore && !Near(x, y, kPlain) && !Near(x, y, kHills) &&
                    !Near(x, y, kMountains))
                    Put(x, y, kWater);
            }
        }
        auto dry = [](int t) { return t == kHills || t == kForest || t == kPlain; };
        for (int x = 1; x < kW - 1; x++) {
            for (int y = 1; y < kH - 1; y++) {
                if (!Wet(At(x, y))) continue;
                auto a = [&](int dx, int dy) { return At(x + dx, y + dy); };
                if (Wet(a(1, 1)) && Wet(a(-1, -1)) && dry(a(1, -1)) && dry(a(-1, 1))) {
                    if (Dice(1, 10, 0) > 4)
                        Put(x - 1, y + 1, kShore);
                    else
                        Put(x + 1, y - 1, kShore);
                } else if (Wet(a(-1, 1)) && Wet(a(1, -1)) && dry(a(-1, -1)) && dry(a(1, 1))) {
                    if (Dice(1, 10, 0) > 4)
                        Put(x + 1, y + 1, kShore);
                    else
                        Put(x - 1, y - 1, kShore);
                }
            }
        }
    }

    /** 4eb7:0000. */
    void Rivers() {
        for (int i = 0; i < R16(kPRivers); i++) River(false, true, false);
        for (int i = 0; i < R16(kPWideRivers); i++) River(true, true, false);
        WaterTidy();
        Shores();
        MountainTidy();
        Foothills();
        WaterTidy();
        Shores();
    }

    // --- 4e47: forests --------------------------------------------------------

    void AddForest() { W16(kForestDone, R16(kForestDone) + 1); }

    /** 4e47:00b5: a wood round a random plain tile: eight arms, and off each
     *  step of an arm two side shoots that shorten as it goes. */
    void Wood() {
        int x = 0, y = 0;
        for (int tries = 0; tries < kSafety; tries++) {
            x = Dice(1, kW, -1);
            y = Dice(1, kH, -1);
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
        auto grow = [&](P& p) {
            Clamp(p);
            if (At(p.x, p.y) == kPlain) {
                Put(p.x, p.y, kForest);
                AddForest();
            }
        };
        Put(x, y, kForest);
        AddForest();
        for (int i = 0; i < 8; i++) {
            P cur{x, y};
            int k = 0;
            for (int s = 0; s < arms[i]; s++) {
                cur.x += Nx(i);
                cur.y += Ny(i);
                grow(cur);
                int l = (i + 2) % 8, r = (i + 6) % 8;
                k++;
                int nl = Dice(1, span - k / 2, base - k / 4);
                int nr = Dice(1, span - k / 2, base - k / 3);
                P p = cur;
                for (int j = 0; j < nl; j++) {
                    p.x += Nx(l);
                    p.y += Ny(l);
                    grow(p);
                }
                P q = cur;
                for (int j = 0; j < nr; j++) {
                    q.x += Nx(r);
                    q.y += Ny(r);
                    grow(q);
                }
            }
        }
    }

    /** 4e47:03d9: plain with forest on three sides is forest, and a diagonal
     *  of forest is given a tile beside it. */
    void ForestTidy() {
        for (int x = 0; x < kW; x++) {
            for (int y = 0; y < kH; y++) {
                if (At(x, y) != kPlain) continue;
                int n = (At(x, y + 1) == kForest) + (At(x, y - 1) == kForest) +
                        (At(x + 1, y) == kForest) + (At(x - 1, y) == kForest);
                if (n > 2) {
                    Put(x, y, kForest);
                    AddForest();
                }
            }
        }
        auto open = [](int t) { return t == kHills || t == kShore || t == kPlain; };
        for (int x = 1; x < kW - 1; x++) {
            for (int y = 1; y < kH - 1; y++) {
                if (At(x, y) != kForest) continue;
                auto a = [&](int dx, int dy) { return At(x + dx, y + dy); };
                if (a(1, 1) == kForest && a(-1, -1) == kForest && open(a(1, -1)) &&
                    open(a(-1, 1))) {
                    if (Dice(1, 10, 0) < 5)
                        Put(x + 1, y - 1, kForest);
                    else
                        Put(x - 1, y + 1, kForest);
                } else if (a(-1, 1) == kForest && a(1, -1) == kForest && open(a(-1, -1)) &&
                           open(a(1, 1))) {
                    if (Dice(1, 10, 0) < 5)
                        Put(x - 1, y - 1, kForest);
                    else
                        Put(x + 1, y + 1, kForest);
                }
            }
        }
    }

    /** 4e47:0000: woods until they cover land / 100 x the Forest parameter. */
    void Forests() {
        int land = 0;
        for (int i = 0; i < kW * kH; i++) {
            int t = m[kGrid + i];
            if (t == kPlain || t == kHills || t == kMountains) land++;
        }
        W16(kForestWanted, land / 100 * R16(kPForest));
        W16(kForestDone, 0);
        for (int n = 0; n < 10000 && R16(kForestDone) < R16(kForestWanted); n++) {
            Wood();
            ForestTidy();
        }
    }

    // --- 4fc9: marshes ----------------------------------------------------------

    /** 4fc9:0029: 1d5+3 tiles of marsh scattered round (x, y). */
    void MarshAround(int x, int y) {
        int n = Dice(1, 5, 3);
        for (int i = 0; i < n; i++) {
            P p;
            int r1 = Dice(1, 3, 0);
            int k1 = Dice(1, 8, -1);
            p.x = x + r1 * Nx(k1);
            int r2 = Dice(1, 3, 0);
            int k2 = Dice(1, 8, -1);
            p.y = y + r2 * Ny(k2);
            Clamp(p);
            if (At(p.x, p.y) == kPlain) Put(p.x, p.y, kMarsh);
        }
    }

    /** 4fc9:010a: a marsh, near the shore if one of four tries finds it. */
    void Marsh() {
        int x = 0, y = 0, tries = 0;
        for (int n = 0; n < kSafety; n++) {
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

    /** 4f5f:06a0: 1d3 marshes. */
    void Marshes() {
        int n = Dice(1, 3, 0);
        for (int i = 0; i < n; i++) Marsh();
    }

    // --- 513d: cities and sites ---------------------------------------------

    /** 513d:0331: a city's 2x2 footprint, clear of water, hills and other
     *  cities by a tile; on the coast if one of four tries finds it. */
    void PlaceCity() {
        int x = 0, y = 0, tries = 0;
        for (int n = 0; n < kSafety; n++) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            bool ok = true;
            for (auto& f : kFootprint) {
                int dx = f[0], dy = f[1];
                int t = At(x + dx, y + dy);
                if (t == kShore || t == kWater || t == kCity || t == kMountains || t == kHills)
                    ok = false;
                if (Near(x + dx, y + dy, kCity) || Near(x + dx + 1, y + dy + 1, kCity) ||
                    Near(x + dx - 1, y + dy - 1, kCity))
                    ok = false;
            }
            if (!ok) continue;
            if (tries >= 4) break;
            tries++;
            if (Near(x, y, kShore) || Near(x + 1, y, kShore) || Near(x, y + 1, kShore) ||
                Near(x + 1, y + 1, kShore))
                break;
        }
        int c = CityCount();
        Ws16(Crec(c), x);
        Ws16(Crec(c) + 2, y);
        SetCityCount(c + 1);
        for (auto& f : kFootprint) Put(x + f[0], y + f[1], kCity);
    }

    /** 513d:0000. */
    void Cities() {
        SetCityCount(0);
        for (int i = R16(0xa84); i < R16(kPCities); i++) PlaceCity();
    }

    /** 513d:0a77: somewhere for a site. The map is taken a sixteenth at a time,
     *  round in turn: 50 tries there for plain clear of cities and of other
     *  sites by three, then 50 anywhere, then 50 for anything not a city. */
    P SiteSpot() {
        if (site_cell == -1) site_cell = Dice(1, 16, -1);
        int cxx = site_cell % 4, cyy = site_cell / 4;
        int x = 0, y = 0;
        bool found = false;
        for (int i = 0; i < 50 && !found; i++) {
            x = Dice(1, 20, cxx * kW / 4 + 3);
            y = Dice(1, 31, cyy * kH / 4 + 3);
            found = At(x, y) == kPlain && !Near(x, y, kCity) && !Near(x, y, kSite) &&
                    !Near(x - 3, y, kSite) && !Near(x + 3, y, kSite) && !Near(x, y - 3, kSite) &&
                    !Near(x, y + 3, kSite);
        }
        for (int i = 0; i < 50 && !found; i++) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            found = At(x, y) == kPlain && !Near(x, y, kCity) && !Near(x, y, kSite);
        }
        if (!found) {
            x = Dice(1, 102, 5);
            y = Dice(1, 146, 5);
            for (int i = 0; i < 50 && !found; i++) {
                x = Dice(1, 102, 5);
                y = Dice(1, 146, 5);
                found = At(x, y) != kCity && At(x, y) != kSite;
            }
        }
        site_cell = (site_cell + 1) % 16;
        return P{x, y};
    }

    /** 513d:003a, its start: forty sites, every one unplaced. */
    void SitesBegin() {
        Ws16(kSSiteCount, kNSites);
        for (int i = 0; i < kNSites; i++) {
            Ws16(kSSites + kSiteStride * i + 2, -1);
            Ws16(kSSites + kSiteStride * i, -1);
        }
    }

    /** 513d:003a: site `i`, keeping the template's temples and ruins, with
     *  names and descriptions from RANDOM.DAT's word lists. */
    void Site(int i) { SiteAt(i, SiteSpot()); }

    /** Site `i` at `s`: its record, its name and its description. */
    void SiteAt(int i, P s) {
        int o = kSSites + kSiteStride * i;
        Ws16(o, s.x);
        Ws16(o + 2, s.y);
        Put(s.x, s.y, kSite);
        std::string name, text;
        if (scn[o + 24] == 1) {
            int k = Dice(1, 10, -1);
            name = Str(0x1000 + 10 * k, 10) + " Temple";
            text = "#%03d|The %s can bless your|armies or give you|quests|\n";
        } else {
            int f = Dice(1, R16(kSiteFormats), -1);
            int k1 = Dice(1, 10, -1);
            std::string end = Str(0xf9c + 10 * k1, 10);
            int k2 = Dice(1, 10, -1);
            std::string word = Str(0xf38 + 10 * k2, 10) + end;
            name = format(Str(kSiteFormats + 2 + 20 * f, 20), word);
            text = "#%03d|%s is|inhabited by monsters and|full of treasure!|\n";
        }
        Wstr(o + 4, name);
        spc += format(text, i, name);
    }

    // --- 4fef: terrain to tiles -----------------------------------------------

    /** 4fef:005c: every terrain type its first tile, a city its castle and the
     *  first eight sites their own. */
    void ToTiles() {
        for (int y = 0; y < kH; y++) {
            for (int x = 0; x < kW; x++) {
                int t = At(x, y);
                if (t == kCity && At(x, y - 1) != kCity && At(x - 1, y) != kCity && x > 0 &&
                    y > 0) {
                    int c = R16(kTiles + 64 * kCity) & 0xff;
                    SetTile(x, y, c);
                    SetTile(x + 1, y, c + 1);
                    SetTile(x, y + 1, c + 16);
                    SetTile(x + 1, y + 1, c + 17);
                } else if (t == kSite) {
                    SetTile(x, y, TileFor(kSite, 0, x, y));
                } else if (t != kCity) {
                    SetTile(x, y, TileFor(t, 0, x, y));
                }
            }
        }
        for (int i = 0; i < 8; i++) {
            int o = kSSites + kSiteStride * i;
            SetTile(S16(o), S16(o + 2), R16(kTiles + 64 * kSite + 4) & 0xff);
        }
    }

    /** The 8 neighbours as a mask (bit i for neighbour i), counting the map's
     *  edge in: 4fef:0c9d (water, shore, bridge), 0d4f (forest), 0dcb
     *  (mountains), 0e47 (hills or mountains). */
    int Mask(int x, int y, const std::function<bool(int)>& test) const {
        int mk = 0;
        for (int i = 0; i < 8; i++) {
            int ax = x + Nx(i), ay = y + Ny(i);
            bool off = ax < 0 || ay < 0 || ax >= kW || ay >= kH;
            if (off || test(At(ax, ay))) mk |= 1 << i;
        }
        return mk;
    }

    int Shape(int mk) const { return I8(m[kShape + mk]); }

    template <class F>
    void Each(int t, F f) {
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++)
                if (At(x, y) == t) f(x, y);
    }

    /** 4fef:0464: the tiles that join each kind of ground to its neighbours. A
     *  shape the tile set has no tile for gives way: water to marsh, forest
     *  and hills often to plain, mountains to hills. */
    void Shapes() {
        for (int y = 0; y < kH; y++) {
            for (int x = 0; x < kW; x++) {
                int t = At(x, y);
                if (t != kWater && t != kShore) continue;
                int v = Shape(Mask(
                        x, y, [](int n) { return n == kWater || n == kShore || n == kBridge; }));
                if (v < 0) {
                    Put(x, y, kMarsh);
                    SetTile(x, y, TileFor(kMarsh, 0, x, y));
                    MarshAround(x, y);
                    tiles[y * kW + x] &= 0x7fff;
                } else {
                    Put(x, y, kWater);
                    SetTile(x, y, TileFor(kWater, v, x, y));
                }
            }
        }
        Each(kForest, [&](int x, int y) {
            int v = Shape(Mask(x, y, [](int n) { return n == kForest; }));
            if (v >= 0) {
                SetTile(x, y, TileFor(kForest, v, x, y));
                Put(x, y, kForest);
            } else if (Dice(1, 10, 0) < 6) {
                SetTile(x, y, TileFor(kPlain, 0, x, y));
                Put(x, y, kPlain);
            } else {
                SetTile(x, y, TileFor(kForest, 13, x, y));
            }
        });
        Each(kMountains, [&](int x, int y) {
            int v = Shape(Mask(x, y, [](int n) { return n == kMountains; }));
            if (v >= 0) {
                SetTile(x, y, TileFor(kMountains, v, x, y));
                Put(x, y, kMountains);
            } else {
                SetTile(x, y, TileFor(kHills, 0, x, y));
                Put(x, y, kHills);
            }
        });
        Each(kHills, [&](int x, int y) {
            int v = Shape(Mask(x, y, [](int n) { return n == kMountains || n == kHills; }));
            if (v >= 0) {
                SetTile(x, y, TileFor(kHills, v, x, y));
                Put(x, y, kHills);
            } else if (Dice(1, 10, 0) < 6) {
                SetTile(x, y, TileFor(kPlain, 0, x, y));
                Put(x, y, kPlain);
            } else {
                Put(x, y, kHills);
                SetTile(x, y, TileFor(kHills, 13, x, y));
            }
        });
        Each(kMarsh, [&](int x, int y) {
            // mostly the plain marsh; three in ten one of four others
            int o = Dice(1, 10, 0) < 7 ? 2 * Parity(x, y) : 4 * Dice(1, 4, 0);
            SetTile(x, y, R16(kTiles + 64 * kMarsh + o) & 0xff);
            Put(x, y, kMarsh);
        });
    }

    // --- 5311: bridges, crossings and roads -----------------------------------

    /** 5311:0a7f: ground a bridge may land on. */
    bool Firm(int x, int y) const {
        int t = At(x, y);
        return t == kPlain || t == kForest || t == kHills;
    }

    /** The nearest city, by map distance, to each listed spot (shared by
     *  5311:03c3 and 08f6; a spot's figure is worked out once). */
    void NearestCities(SpotList& list, int n) {
        if (list.city_dist[0] != -1) return;
        for (int i = n - 1; i >= 0; i--) {
            if (list.x[i] == -1) continue;
            int best = 10000;
            for (int c = CityCount() - 1; c >= 0; c--)
                best = std::min(best, Distance(list.x[i], list.y[i], Cx(c), Cy(c)));
            list.city_dist[i] = best;
        }
    }

    /** 5311:08f6 (bridges) and 03c3 (crossings): the best spot still free --
     *  a die roll, plus more the nearer a city is. Bridges keep 10 apart. */
    int PickSpot(SpotList& list, int n, bool apart) {
        NearestCities(list, n);
        for (int i = n - 1; i >= 0; i--) {
            if (list.chosen[i] != 0 || list.x[i] == -1) continue;
            int best = 10000;
            for (int j = n - 1; j >= 0; j--) {
                if (list.chosen[j] != 0)
                    best = std::min(best, Distance(list.x[i], list.y[i], list.x[j], list.y[j]));
            }
            list.chosen_dist[i] = best;
        }
        int best_score = -1, pick = -1;
        for (int i = n - 1; i >= 0; i--) {
            if (list.chosen[i] != 0 || list.x[i] == -1) continue;
            if (apart && !(list.chosen_dist[i] > 9)) continue;
            int roll = Dice(1, 15, 1);
            int near_by = list.city_dist[i] < 31 ? 30 - list.city_dist[i] : 0;
            if (best_score < near_by + roll) {
                best_score = near_by + roll;
                pick = i;
            }
        }
        return pick;
    }

    /** 5311:05b4: bridges over two-tile rivers. As the original has it, only
     *  a river running north and south (tiles 0x26 and 0x28) is ever bridged. */
    void Bridges(SpotList& list) {
        int n = 0;
        for (int y = 1; y < kH - 2; y++) {
            for (int x = 1; x < kW - 2; x++) {
                int t = Tile(x, y);
                bool across = (t == 0x26 || t == 0x16) &&
                              (Tile(x + 1, y) == 0x28 || Tile(x + 1, y) == 0x18);
                bool along = (t == 0x21 || t == 0x11) &&
                             (Tile(x, y + 1) == 0x24 || Tile(x, y + 1) == 0x14);
                if (!across && !along) continue;
                int ew = t == 0x26 ? 1 : 0;
                bool ok = ew && ((Firm(x - 1, y) && Firm(x + 2, y)) ||
                                 (Firm(x, y - 1) && Firm(x, y + 2)));
                if (ok && n < 50) {
                    list.x[n] = x;
                    list.y[n] = y;
                    list.ew[n] = ew;
                    n++;
                }
            }
        }
        if (n == 0) return;
        for (int i; (i = PickSpot(list, n, true)) != -1;) {
            list.chosen[i] = 1;
            int x = list.x[i], y = list.y[i];
            if (list.ew[i] == 0) {
                SetTile(x, y, 0x84);
                SetTile(x, y + 1, 0x94);
                Put(x, y + 1, kBridge);
                Put(x, y, kBridge);
                SetRoad(x, y + 2, 1);
                SetRoad(x, y - 1, 1);
            } else {
                SetTile(x, y, 0x85);
                SetTile(x + 1, y, 0x86);
                Put(x + 1, y, kBridge);
                Put(x, y, kBridge);
                SetRoad(x + 2, y, 1);
                SetRoad(x - 1, y, 1);
            }
        }
    }

    /** 5311:01f6: up to ten crossings -- shore that looks out on three tiles of
     *  open water, at least 15 from the others. */
    void Crossings(SpotList& list) {
        for (int y = 1; y < kH - 2; y++) {
            for (int x = 1; x < kW - 2; x++) {
                if (Dice(1, 2, -1) != 0) continue;
                int dx = 0, dy = 0;
                switch (Tile(x, y)) {
                case 0x20:
                case 0x23:
                    dx = -1;
                    break;
                case 0x21:
                    dy = -1;
                    break;
                case 0x22:
                case 0x25:
                    dx = 1;
                    break;
                case 0x24:
                    dy = 1;
                    break;
                default:
                    continue;
                }
                bool open = true;
                for (int k = 1; k <= 3; k++)
                    if (At(x + k * dx, y + k * dy) != kWater) open = false;
                if (!open) continue;
                int near_by = 10000;
                for (int i = 49; i >= 0; i--) {
                    if (list.x[i] != -1)
                        near_by = std::min(near_by, Distance(x, y, list.x[i], list.y[i]));
                }
                if (near_by <= 14) continue;
                for (int i = 0; i < 50; i++) {
                    if (list.x[i] == -1) {
                        list.x[i] = x;
                        list.y[i] = y;
                        break;
                    }
                }
            }
        }
        list.city_dist[0] = -1;
        for (int n = 0, i; n < 10 && (i = PickSpot(list, 50, false)) != -1; n++) {
            list.chosen[i] = 1;
            tiles[list.y[i] * kW + list.x[i]] |= 0x8000;
        }
    }

    bool IsServed(int c) const {
        return c >= 0 && c < static_cast<int>(served.size()) ? served[c] != 0 : false;
    }
    void Serve(int c) {
        if (c >= 0 && c < static_cast<int>(served.size())) served[c] = 1;
    }

    /** 5311:0e22: a pair of cities to join -- a random one not yet served, and
     *  of the unserved 30 to 70 away, the one that rolls highest. */
    std::optional<std::pair<int, int>> RoadPair() {
        for (;;) {
            int a = -1;
            for (int n = 80; n > 0; n--) {
                a = Dice(1, CityCount(), -1);
                if (!IsServed(a)) break;
                a = -1;
            }
            if (a == -1) return std::nullopt;
            Serve(a);
            int b = -1, best = -1;
            for (int c = CityCount() - 1; c >= 0; c--) {
                if (IsServed(c)) continue;
                int d = Distance(Cx(a), Cy(a), Cx(c), Cy(c));
                if (d > 29 && d < 71) {
                    int roll = Dice(1, 1000, 0);
                    if (best < roll) {
                        best = roll;
                        b = c;
                    }
                }
            }
            if (b != -1) {
                Serve(b);
                return std::make_pair(a, b);
            }
        }
    }

    /** 5311:0ad0: a tile round a city's footprint to start a road from. */
    std::optional<P> RoadEnd(int c) {
        for (int n = 8; n > 0; n--) {
            const int* r = kRoundCity[Dice(1, 12, -1)];
            int x = Cx(c) + r[0], y = Cy(c) + r[1];
            int t = At(x, y);
            if (t == kPlain || t == kForest || t == kHills) return P{x, y};
        }
        return std::nullopt;
    }

    /** 5311:0c1c: lay a road along the path the game's own pathfinder finds,
     *  as pseudo-player 14 -- not over water, shore or bridges -- and every six
     *  steps count the cities within 10 as served. */
    void LayRoad(P from, P to) {
        move::RoadGround ground;
        ground.terrain = [this](int x, int y) { return TerrainOf(Tile(x, y)); };
        ground.road = [this](int x, int y) { return Road(x, y) != 0; };
        ground.crossing = [this](int x, int y) {
            return x >= 0 && y >= 0 && x < kW && y < kH && (tiles[y * kW + x] & 0x8000) != 0;
        };
        auto route = move::roadRoute(ground, kRoadCost, kW, kH, from.x, from.y, to.x, to.y);
        if (!route) return;
        int x = from.x, y = from.y, since = 0;
        for (size_t i = 0; i < route->size() && i < 200; i++) {
            since++;
            if (x == to.x && y == to.y) break;
            x = (*route)[i].first;
            y = (*route)[i].second;
            int t = At(x, y);
            if (t != kShore && t != kWater && t != kBridge) SetRoad(x, y, 1);
            if (since > 5) {
                since = 0;
                for (int c = CityCount() - 1; c >= 0; c--) {
                    if (Distance(x, y, Cx(c), Cy(c)) < 10) Serve(c);
                }
            }
        }
    }

    /** 5311:0000, its tiles: then bridges and crossings. */
    void RoadTiles() {
        ToTiles();
        Shapes();
        Shapes();
        for (int i = 0; i < kW * kH; i++) {
            tiles[i] &= 0x7fff;
            rd[i] &= 0xe0;
        }
    }

    void RoadCrossings() {
        SpotList list;
        for (int i = 0; i < 50; i++) {
            list.x[i] = list.y[i] = list.ew[i] = list.city_dist[i] = list.chosen_dist[i] = -1;
            list.chosen[i] = 0;
        }
        Bridges(list);
        for (int i = 49; i >= 0; i--) {
            if (list.chosen[i] == 0)
                list.x[i] = list.y[i] = list.ew[i] = list.city_dist[i] = list.chosen_dist[i] = -1;
        }
        Crossings(list);
        served.assign(100, 0);
        shown = 4;
    }

    /** The cities joined by roads, a pair at a time; a progress figure when
     *  the original's bar moves on, nothing once every city is served. */
    std::optional<int> RoadsUntilShown() {
        for (;;) {
            std::pair<int, int> pair;
            if (pending) {
                pair = *pending;
                pending.reset();
            } else {
                auto p = RoadPair();
                if (!p) break;
                pair = *p;
                if (shown < 10 && Dice(1, 3, -1) == 0) {
                    pending = pair;
                    return 80 + shown++;
                }
            }
            auto a = RoadEnd(pair.first);
            std::optional<P> b;
            if (a) b = RoadEnd(pair.second);
            if (a && b && !(a->x == b->x && a->y == b->y)) LayRoad(*a, *b);
        }
        for (int i = 0; i < kW * kH; i++)
            if (m[kGrid + i] == kSite) rd[i] &= 0xe0;
        return std::nullopt;
    }

    /** 4fef:0a4b: each road its shape from the roads and bridges round it; a
     *  shape there is no piece for leaves plain or hills. Then a dozen or two
     *  straight pieces take their other look. */
    void RoadShapes() {
        for (int y = 0; y < kH; y++) {
            for (int x = 0; x < kW; x++) {
                if (Road(x, y) == 0) continue;
                int mk = 0;
                for (int i = 0; i < 8; i++) {
                    int ax = x + R16(kNbRoad + 4 * i), ay = y + R16(kNbRoad + 4 * i + 2);
                    if (ax >= 0 && ay >= 0 && ax < kW && ay < kH &&
                        (Road(ax, ay) != 0 || TerrainOf(Tile(ax, ay)) == kBridge))
                        mk |= 1 << i;
                }
                int v = I8(m[kRoadShape + mk]);
                if (v >= 0)
                    SetRoad(x, y, v + 1);
                else if (Dice(1, 10, 0) < 6) {
                    SetTile(x, y, TileFor(kPlain, 0, x, y));
                    Put(x, y, kPlain);
                } else {
                    SetTile(x, y, TileFor(kHills, 13, x, y));
                }
            }
        }
        const int swaps[2][2] = {{2, 17}, {1, 16}};
        for (auto& sw : swaps) {
            int n = Dice(1, 10, 10);
            for (int k = 0; k < n; k++) {
                for (int tries = 0; tries < 10000; tries++) {
                    int x = Dice(1, kW, -1);
                    int y = Dice(1, kH, -1);
                    if (Road(x, y) == sw[0]) {
                        SetRoad(x, y, sw[1]);
                        break;
                    }
                }
            }
        }
    }

    // --- 513d / 5132: the sides and the cities' details ---------------------

    /** 513d:05e3: the cities, read back off the tile map in rows. */
    void CityList() {
        int n = 0;
        for (int y = 0; y < kH; y++) {
            for (int x = 0; x < kW; x++) {
                if (Tile(x, y) != 0x60) continue;
                int o = Crec(n);
                Ws16(o, x);
                Ws16(o + 2, y);
                scn[o + 21] = 15;
                scn[o + 20] = static_cast<uint8_t>(Dice(1, 3, 2));
                n++;
            }
        }
        SetCityCount(n);
    }

    /** 513d:08f0: a random city in one of the map's eight regions, four
     *  across and two down; after 202 tries, wherever the last one was. */
    int CityIn(int region) {
        int x0 = (region % 4) * 28, y0 = region / 4 * 78;
        int c = 0;
        for (int tries = 0; tries < 202; tries++) {
            c = Dice(1, CityCount(), -1);
            int x = Cx(c), y = Cy(c);
            if (x >= x0 && x < x0 + 28 && y >= y0 && y < y0 + 78) break;
        }
        return c;
    }

    static int SideRec(int s) { return kSSideRec + 20 * s; }

    /** 513d:0689: each side a capital in a region of its own, 12 from the
     *  edge and 20 from the others both ways; ten rounds of 100 tries a side,
     *  the last round taking what it gets. A side's armies favour its region. */
    void Capitals() {
        for (int round = 1;; round++) {
            bool used[8] = {false};
            for (int s = 0; s < 8; s++) {
                Ws16(SideRec(s) + 6, -100);
                Ws16(SideRec(s) + 8, -100);
            }
            bool again = false;
            for (int s = 0; s < 8 && !again; s++) {
                int region;
                do region = Dice(1, 8, -1);
                while (used[region]);
                used[region] = true;
                int c = 0, tries = 0;
                bool ok = false;
                for (; !ok && tries < 100; tries++) {
                    c = CityIn(region);
                    int x = Cx(c), y = Cy(c);
                    if (x < 12 || x > 100 || y < 12 || y > 144) continue;
                    ok = true;
                    for (int o = 0; o < 8; o++) {
                        if (std::abs(x - S16(SideRec(o) + 6)) < 20 ||
                            std::abs(y - S16(SideRec(o) + 8)) < 20) {
                            ok = false;
                            break;
                        }
                    }
                }
                // a find on the hundredth try counts as none, as the original has it
                if (tries > 99 && round < 10) {
                    again = true;
                    break;
                }
                W16(kRegionClass + 2 * region, R16(kSideClass + 2 * s));
                Ws16(SideRec(s) + 6, Cx(c));
                Ws16(SideRec(s) + 8, Cy(c));
            }
            if (!again) break;
        }
        Crown();
    }

    /** The capitals in the side records take their sides: the city's owner
     *  byte, and the castle in the side's colours (513d:09b3). */
    void Crown() {
        for (int s = 0; s < 8; s++) {
            int x = S16(SideRec(s) + 6), y = S16(SideRec(s) + 8);
            for (int c = 0; c < CityCount(); c++) {
                if (Cx(c) == x && Cy(c) == y) {
                    scn[Crec(c) + 21] = static_cast<uint8_t>(s);
                    break;
                }
            }
            // 513d:09b3: the castle in the side's colours
            int t = s < 6 ? 0x62 + 2 * s : 0x80 + 2 * (s - 6);
            SetTile(x, y, t);
            SetTile(x + 1, y, t + 1);
            SetTile(x, y + 1, t + 16);
            SetTile(x + 1, y + 1, t + 17);
        }
    }

    /** Not the original's (Options::fairCapitals): the eight capitals chosen
     *  among the cities for a fair start, by travel cost between them
     *  (warlords/travel). What a fair start is was learned from the hand-made
     *  scenarios and from votes on generated maps (docs/mapeval.md):
     *   - no city far from every capital (a swathe nobody is near becomes one
     *     side's free hinterland),
     *   - each side about as many cities it reaches first as the next,
     *   - capitals at least FAIR_GAP turns apart (none of the designers' maps
     *     comes nearer than 2.5).
     *  A local search from a few random starts, swapping one capital at a
     *  time while the start gets fairer. */
    void FairCapitals() {
        constexpr int kSides = 8;
        constexpr double kFairGap = 3.0;  // turns
        constexpr double kRemote = 5.0;   // turns from the nearest capital
        constexpr int kStarts = 12, kPasses = 40;
        const int n = CityCount();
        if (n < kSides) {
            Capitals();
            return;
        }
        travel::Ground g;
        g.w = kW;
        g.h = kH;
        g.terrain.resize(static_cast<size_t>(kW) * kH);
        g.road.resize(static_cast<size_t>(kW) * kH);
        g.crossing.resize(static_cast<size_t>(kW) * kH);
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                int i = y * kW + x, t = TerrainOf(Tile(x, y));
                g.terrain[i] = t;
                g.road[i] = Road(x, y) != 0;
                g.crossing[i] = (tiles[i] & 0x8000) != 0 || t == kBridge;
            }
        auto footprint = [&](int c) {
            std::vector<int> f;
            for (auto& d : kFootprint)
                if (Cx(c) + d[0] < kW && Cy(c) + d[1] < kH)
                    f.push_back((Cy(c) + d[1]) * kW + Cx(c) + d[0]);
            return f;
        };
        // a port city is a crossing (1555:109d): its footprint touches the sea
        for (int c = 0; c < n; c++) {
            bool port = false;
            for (auto& r : kRoundCity) {
                int t = TerrainOf(Tile(Cx(c) + r[0], Cy(c) + r[1]));
                if (t == kBridge || t == kWater || t == kShore) port = true;
            }
            if (port)
                for (int i : footprint(c)) g.crossing[i] = 1;
        }
        std::vector<std::vector<int>> d(n, std::vector<int>(n, travel::UNREACHED));
        for (int c = 0; c < n; c++) {
            auto field = travel::costs(g, footprint(c));
            for (int o = 0; o < n; o++)
                for (int i : footprint(o)) d[c][o] = std::min(d[c][o], field[i]);
        }
        // how unfair a choice is; lower is better
        std::vector<int> owner(n);
        auto unfairness = [&](const std::array<int, kSides>& caps) {
            double gap = 1e9;
            for (int a = 0; a < kSides; a++)
                for (int b = a + 1; b < kSides; b++)
                    gap = std::min(gap, static_cast<double>(d[caps[a]][caps[b]]));
            if (gap >= travel::UNREACHED) return 1e12;  // a capital cut off from another
            gap /= travel::TURN;
            std::array<int, kSides> share{};
            int remote = 0;
            double reach = 0;
            for (int c = 0; c < n; c++) {
                if (std::find(caps.begin(), caps.end(), c) != caps.end()) continue;
                int best = travel::UNREACHED, who = 0;
                for (int s = 0; s < kSides; s++)
                    if (d[caps[s]][c] < best) {
                        best = d[caps[s]][c];
                        who = s;
                    }
                if (best == travel::UNREACHED) {
                    remote++;
                    continue;
                }
                share[who]++;
                reach += static_cast<double>(best) / travel::TURN;
                if (best > kRemote * travel::TURN) remote++;
            }
            auto [lo, hi] = std::minmax_element(share.begin(), share.end());
            return 3.0 * remote + 1.0 * (*hi - *lo) + reach / n +
                   (gap < kFairGap ? 10.0 * (kFairGap - gap) : 0.0);
        };
        std::array<int, kSides> best{};
        double best_score = 1e18;
        for (int start = 0; start < kStarts; start++) {
            std::array<int, kSides> caps{};
            for (int s = 0; s < kSides; s++) {
                int c;
                do c = Dice(1, n, -1);
                while (std::find(caps.begin(), caps.begin() + s, c) != caps.begin() + s);
                caps[s] = c;
            }
            double score = unfairness(caps);
            for (int pass = 0; pass < kPasses; pass++) {
                bool better = false;
                for (int s = 0; s < kSides; s++)
                    for (int c = 0; c < n; c++) {
                        if (std::find(caps.begin(), caps.end(), c) != caps.end()) continue;
                        int was = caps[s];
                        caps[s] = c;
                        double now = unfairness(caps);
                        if (now < score - 1e-9) {
                            score = now;
                            better = true;
                        } else
                            caps[s] = was;
                    }
                if (!better) break;
            }
            if (score < best_score) {
                best_score = score;
                best = caps;
            }
        }
        // the sides take the capitals in a random order; a side's armies favour
        // the region its capital stands in, as 513d:0689 has it
        for (int s = kSides - 1; s > 0; s--) std::swap(best[s], best[Dice(1, s + 1, -1)]);
        for (int s = 0; s < kSides; s++) {
            int x = Cx(best[s]), y = Cy(best[s]);
            int region = std::min(3, x / 28) + 4 * std::min(1, y / 78);
            W16(kRegionClass + 2 * region, R16(kSideClass + 2 * s));
            Ws16(SideRec(s) + 6, x);
            Ws16(SideRec(s) + 8, y);
        }
        Crown();
    }

    /** 5132:0014 / 006b: each side a name from five, and 3d50+20 gold. */
    void Sides() {
        for (int s = 0; s < 8; s++) {
            int k = Dice(1, 5, -1);
            std::string name = Str(kSideNames + 100 * s + 20 * k, 20);
            for (int i = 0; i < 20; i++) scn[20 * s + i] = 0;
            Wstr(20 * s, name);
        }
        for (int s = 0; s < 8; s++) Ws16(SideRec(s) + 2, Dice(3, 50, 20));
    }

    /** What touches a city's footprint (513d:0ce0, into 4125:02d6-02de). */
    Surroundings SurroundingsOf(int c) const {
        int x = Cx(c), y = Cy(c);
        auto by = [&](int t) {
            return Near(x, y, t) || Near(x + 1, y, t) || Near(x, y + 1, t) || Near(x + 1, y + 1, t);
        };
        return Surroundings{by(kShore), by(kForest), by(kRoad), by(kHills), by(kMarsh)};
    }

    /** random_city_value (513d:1104): a capital 9; else 1d4-1, +4 by the
     *  shore, +2 by a road, -1 by forest, -2 by marsh, within 0..9. */
    int Value(int c, const Surroundings& f) {
        if (scn[Crec(c) + 21] != 15) return 9;
        int v = Dice(1, 4, -1);
        if (f.shore) v += 4;
        if (f.road) v += 2;
        if (f.forest) v -= 1;
        if (f.marsh) v -= 2;
        if (v > 8) v = 9;
        if (v < 1) v = 0;
        return v;
    }

    /** 513d:1468: a name of two or three syllables, ending most often in a
     *  word for what lies round it. */
    std::string CityName(int c, const Surroundings& f) {
        bool three = Dice(1, 10, 0) < 7;
        int k = Dice(1, 20, -1);
        std::string name = Str(0xa86 + 10 * k, 10);
        if (three) {
            int k2 = Dice(1, 20, -1);
            name += Str(0xb4e + 10 * k2, 10);
        }
        int end = -1;
        if (Dice(1, 10, 0) < 7) {
            if (f.marsh)
                end = 0xda6;
            else if (f.forest)
                end = 0xcde;
            else if (f.shore)
                end = 0xd42;
            else if (f.hills)
                end = 0xe0a;
            else if (Dice(1, 10, 0) < 5)
                end = 0xe6e;
        }
        if (end != -1) {
            int k3 = Dice(1, 10, -1);
            name += Str(end + 10 * k3, 10);
        } else {
            int k3 = Dice(1, 20, -1);
            name += Str(0xc16 + 10 * k3, 10);
        }
        name = name.substr(0, 15);
        int o = Crec(c) + 4;
        for (int i = 0; i < 16; i++) scn[o + i] = 0;
        Wstr(o, name);
        return name;
    }

    /** Does a city before `c` already have this name? */
    bool NameTaken(int c, const std::string& name) const {
        for (int j = 0; j < c; j++)
            if (CstrOf(scn, Crec(j) + 4, 16) == name) return true;
        return false;
    }

    /** 513d:1171: the city's name, income -- value x 2 + 1d8 + 14 -- and its
     *  three lines of description, grander as the city is richer. */
    void CityText(int c, int v, const Surroundings& f) {
        std::string name = CityName(c, f);
        // the scenario generator's names never repeat (the original's may)
        for (int tries = 0; modern && tries < 200 && NameTaken(c, name); tries++)
            name = CityName(c, f);
        Ws16(Crec(c) + 42, v * 2 + Dice(1, 8, 0) + 14);
        auto grade = [&]() { return std::min(9, std::max(0, Dice(1, 3, v - 2))); };
        int g1 = grade();
        int g2 = grade();
        auto w = [&](int o, int i) { return Str(o + 16 * i, 16); };
        std::string a, b, cc;
        bool plain = !f.forest && !f.marsh && !f.hills;
        if (plain || Dice(1, 100, 0) > 79) {
            if (Dice(1, 100, 0) < 50) {
                a = w(0x16a8, Dice(1, 10, -1));
                b = w(0x1748, Dice(1, 10, -1));
                cc = w(0x17e8, Dice(1, 10, -1));
            } else {
                a = w(0x14c8, Dice(1, 10, -1));
                b = w(0x1568, Dice(1, 10, -1));
                cc = w(0x1608, Dice(1, 10, -1));
            }
        } else {
            int k;
            if (f.marsh)
                k = Dice(1, 2, 7);
            else if (f.hills)
                k = Dice(1, 4, 3);
            else
                k = Dice(1, 4, -1);
            a = w(0x12e8, Dice(1, 10, -1));
            b = w(0x1388, Dice(1, 10, -1));
            cc = w(0x1428, k);
        }
        std::string kind = w(0x1248, g2);
        int k = Dice(1, 3, -1);
        std::string adj = Str(0x1068 + 0x30 * g1 + 16 * k, 16);
        cty += format("#%03d|%s is a %s|%s, %s|%s %s|\n", c, name, adj, kind, a, b, cc);
    }

    /** The army types' flags the generator asks after (build_army_move_flags,
     *  6715:0000): ARMYTYPE +54 flies, +48 magical -- an ally. */
    bool Flies(int t) const {
        const ArmyType* a = types.byId(t);
        return a && a->bonus[54] != 0;
    }
    bool Magical(int t) const {
        const ArmyType* a = types.byId(t);
        return a && a->bonus[48] != 0;
    }

    void SetSlot(int c, int k, int e) {
        int o = Crec(c);
        const ArmyType* a = types.byId(e);
        scn[o + 22 + k] = static_cast<uint8_t>(e);
        scn[o + 26 + k] = static_cast<uint8_t>(a ? a->time : 0);
        scn[o + 30 + k] = static_cast<uint8_t>(a ? a->strength : 0);
        scn[o + 34 + k] = static_cast<uint8_t>(a ? a->move : 0);
        scn[o + 38 + k] = static_cast<uint8_t>(a ? a->cost : 0);
    }

    /** 513d:161d: what a city can make. value / 2 + 1d4 - 1 slots, one more
     *  each by forest and hills, at most 4, from RANDOM.DAT's list in order:
     *  each type rolls its chance, and must suit the city's region, or its
     *  forest, hills or shore, or be at home anywhere. A rich city sometimes
     *  adds a flier. Allies only where the option lets cities make them. */
    void Production(int c, int v, const Surroundings& f) {
        int x = Cx(c), y = Cy(c);
        int region = x * 4 / kW + y * 2 / kH * 4;
        int slots = v / 2;
        slots += Dice(1, 4, -1);
        if (f.forest) slots++;
        if (f.hills) slots++;
        if (slots > 3) slots = 4;
        if (slots < 1) slots = 0;
        auto rec = [](int e) { return kProduction + 16 * e; };
        auto suits = [&](int e) {
            int terr = R16(rec(e) + 6);
            return terr == 7 || (terr == 4 && f.forest) || (terr == 5 && f.hills) ||
                   R16(rec(e) + 4) == R16(kRegionClass + 2 * region);
        };
        int k = 0;
        for (int e = 0; e < 29 && k < slots; e++) {
            int t = R16(rec(e));
            if (!allies && Magical(t)) continue;
            if (Dice(1, 10, -1) >= R16(rec(e) + 2)) continue;
            bool ok = suits(e) || (R16(rec(e) + 6) == 3 && f.shore);
            if (t == 5 && !f.shore) ok = false;
            if (ok) SetSlot(c, k++, t);
        }
        if (k == 0) SetSlot(c, k++, R16(rec(0)));
        if (v > 6 && Dice(1, 10, -1) < 5 && k < 4) {
            bool flier = false;
            for (int j = 0; j < k; j++)
                if (Flies(scn[Crec(c) + 22 + j])) flier = true;
            if (!flier && k < slots) {
                for (int e = 0; e < 29; e++) {
                    int t = R16(rec(e));
                    if (Flies(t) && !Magical(t) && suits(e)) {
                        SetSlot(c, k++, t);
                        break;
                    }
                }
            }
        }
        for (; k < 4; k++) scn[Crec(c) + 22 + k] = 255;
    }

    /** random_magical_type (6563:1a9b). */
    int MagicalType() {
        int n = 0;
        for (int t = 0; t < 28; t++)
            if (Magical(t)) n++;
        int pick = Dice(1, n, -1);
        int i = 0, t = 0;
        for (; t < 28; t++) {
            if (!Magical(t)) continue;
            if (i == pick) break;
            i++;
        }
        return t > 27 || !Magical(t) ? 25 : t;
    }

    /** random_add_production (513d:1b3f): with allies on, 2d3 cities may also
     *  make one. */
    void AllyProduction() {
        int n = Dice(2, 3, 0);
        for (int tries = 0; n > 0 && tries < 100; tries++) {
            int c = Dice(1, CityCount(), -1), o = Crec(c);
            int k = 0;
            for (int j = 0; j < 4; j++)
                if (scn[o + 22 + j] < 128) k++;
            if (k < 4) {
                SetSlot(c, k, MagicalType());
                n--;
            }
        }
    }

    /** 513d:1c1d: a capital adds a strong army it cannot make yet -- strength
     *  5 or more, not a navy or an ally -- in its last empty slot. */
    void CapitalArmies() {
        for (int s = 0; s < 8; s++) {
            int x = S16(SideRec(s) + 6), y = S16(SideRec(s) + 8);
            int c = -1;
            for (int i = 0; i < CityCount() && c < 0; i++)
                if (Cx(i) == x && Cy(i) == y) c = i;
            if (c < 0) continue;
            int o = Crec(c);
            // the highest empty slot, else the last
            int k = 3;
            while (k >= 0 && scn[o + 22 + k] < 128) k--;
            if (k < 0) k = 3;
            int draws = 0, pick = -1;
            while (draws <= 39) {
                int t = Dice(1, 28, -1);
                if (t == 5 || Magical(t)) continue;
                draws++;
                bool has = false;
                for (int j = 0; j < 4; j++)
                    if (scn[o + 22 + j] == t) has = true;
                const ArmyType* a = types.byId(t);
                if (!has && a && a->strength >= 5) {
                    pick = t;
                    break;
                }
            }
            // one found on the fortieth draw is dropped, as the original has it
            if (pick >= 0 && draws < 40) scn[o + 22 + k] = static_cast<uint8_t>(pick);
        }
    }

    /** random_map_cities (513d:0ce0). */
    void CityDetails() {
        for (int c = 0; c < CityCount(); c++) {
            Surroundings f = SurroundingsOf(c);
            int v = Value(c, f);
            CityText(c, v, f);
            Production(c, v, f);
        }
        if (allies) AllyProduction();
        CapitalArmies();
    }

    /** auto_file_random_sgn (4fef:113d): 41-70 signposts on open plain off
     *  the roads, most pointing the way to the nearest city, a few with
     *  RANDOM.DAT's own words on them. */
    std::string Signs() {
        int n = Dice(1, 30, 40);
        std::vector<uint8_t> out(2 + 104 * n, 0);
        SetI16(out, 0, n);
        auto put_str = [&](int o, const std::string& s) {
            for (size_t i = 0; i < s.size() && i < 49; i++) out[o + i] = static_cast<uint8_t>(s[i]);
        };
        int fixed = 0;
        for (int i = 0; i < n; i++) {
            int x = 0, y = 0;
            for (int tries = 0; tries < kSafety; tries++) {
                x = Dice(1, kW, -1);
                y = Dice(1, kH, -1);
                if (TerrainOf(Tile(x, y)) == kPlain && !Near(x, y, kCity) && !Near(x, y, kTower) &&
                    Road(x, y) == 0)
                    break;
            }
            SetTile(x, y, 0);
            int o = 2 + 104 * i;
            SetI16(out, o, x);
            SetI16(out, o + 2, y);
            if (Dice(1, 10, 0) < 4 && fixed < R16(kSigns)) {
                put_str(o + 4, Str(kSigns + 2 + 60 * fixed, 30));
                put_str(o + 54, Str(kSigns + 2 + 60 * fixed + 30, 30));
                fixed++;
            } else {
                int c = -1, best = 1000;
                for (int j = 0; j < CityCount(); j++) {
                    int d = Distance(x, y, Cx(j), Cy(j));
                    if (d < best) {
                        best = d;
                        c = j;
                    }
                }
                if (c < 0) continue;
                int cxx = Cx(c), cyy = Cy(c);
                put_str(o + 4, format(Str(kSignCity, 20), CstrOf(scn, Crec(c) + 4, 16)));
                put_str(o + 54, format(Str(kSignLeagues, 20), Distance(x, y, cxx, cyy) * 2,
                                       std::string(kCompass[Compass(x, y, cxx, cyy)])));
            }
        }
        return std::string(out.begin(), out.end());
    }

    // --- the whole -------------------------------------------------------------

    /** The world as files: the scenario image, the tile map and the roads, the
     *  signposts and the descriptions. */
    void WriteFiles(Files& files) {
        std::string map(2UZ * kW * kH, '\0');
        for (int i = 0; i < kW * kH; i++) {
            map[2UZ * i] = static_cast<char>(tiles[i] & 0xff);
            map[2 * i + 1] = static_cast<char>(tiles[i] >> 8);
        }
        std::string road(static_cast<size_t>(kW) * kH, '\0');
        for (int i = 0; i < kW * kH; i++) road[i] = static_cast<char>(rd[i] & 0x1f);
        files["SCN"] = std::string(scn.begin(), scn.end());
        files["MAP"] = map;
        files["RD"] = road;
        files["SGN"] = sgn;
        files["CTY"] = cty;
        files["SPC"] = spc;
    }

    // --- not the original's: the scenario generator (warlords/scengen) -------

    /** The terrain grid as the planners take it. */
    std::vector<uint8_t> Grid() const {
        std::vector<uint8_t> t(static_cast<size_t>(kW) * kH);
        for (int i = 0; i < kW * kH; i++) t[i] = m[kGrid + i];
        return t;
    }

    /** Where a river may be bridged: two water tiles in a row, drawn as a
     *  straight stretch of river (the tiles 5311:05b4 looks for), with firm
     *  ground beyond both ends. */
    std::vector<scengen::Span> Spans() const {
        std::vector<scengen::Span> out;
        for (int y = 1; y < kH - 2; y++)
            for (int x = 1; x < kW - 2; x++) {
                int t = Tile(x, y);
                bool across = (t == 0x26 || t == 0x16) &&
                              (Tile(x + 1, y) == 0x28 || Tile(x + 1, y) == 0x18);
                bool along = (t == 0x21 || t == 0x11) &&
                             (Tile(x, y + 1) == 0x24 || Tile(x, y + 1) == 0x14);
                if (across && Firm(x - 1, y) && Firm(x + 2, y)) out.push_back({x, y, true});
                if (along && Firm(x, y - 1) && Firm(x, y + 2)) out.push_back({x, y, false});
            }
        return out;
    }

    /** The planned roads and bridges onto the map. */
    void LayPlannedRoads() {
        std::vector<std::pair<int, int>> where;
        for (int c = 0; c < CityCount(); c++) where.push_back({Cx(c), Cy(c)});
        auto sp = Spans();
        auto plan = scengen::planRoads(Grid(), kW, kH, where, sp, *rng);
        SpotList list;
        for (int i = 0; i < 50; i++) {
            list.x[i] = list.y[i] = list.ew[i] = list.city_dist[i] = list.chosen_dist[i] = -1;
            list.chosen[i] = 0;
        }
        int n = 0;
        for (int k : plan.bridges) {
            auto& b = sp[k];
            if (b.ew) {
                SetTile(b.x, b.y, 0x85);
                SetTile(b.x + 1, b.y, 0x86);
                Put(b.x, b.y, kBridge);
                Put(b.x + 1, b.y, kBridge);
            } else {
                SetTile(b.x, b.y, 0x84);
                SetTile(b.x, b.y + 1, 0x94);
                Put(b.x, b.y, kBridge);
                Put(b.x, b.y + 1, kBridge);
            }
            if (n < 50) {
                list.x[n] = b.x;
                list.y[n] = b.y;
                list.chosen[n] = 1;
                n++;
            }
        }
        for (int i = 0; i < kW * kH; i++)
            if (plan.road[i]) SetRoad(i % kW, i / kW, 1);
        // the crossings keep their distance from the bridges, as 5311:01f6 does
        Crossings(list);
        Landings();
    }

    /** Every stretch of land with a city or a site on it has a way to the sea:
     *  a port city, or a crossing -- else a river or a strait could leave it
     *  out of reach of every army that cannot fly. Where there is neither, a
     *  shore tile beside it becomes a crossing. */
    void Landings() {
        auto sea = [&](int x, int y) {
            int t = At(x, y);
            return t == kWater || t == kShore;
        };
        std::vector<int> part(static_cast<size_t>(kW) * kH, -1);
        int parts = 0;
        for (int s0 = 0; s0 < kW * kH; s0++) {
            if (part[s0] >= 0 || sea(s0 % kW, s0 / kW)) continue;
            std::vector<int> todo{s0};
            part[s0] = parts;
            for (size_t k = 0; k < todo.size(); k++) {
                int x = todo[k] % kW, y = todo[k] / kW;
                for (int d = 0; d < 8; d++) {
                    int ax = x + Nx(d), ay = y + Ny(d);
                    if (ax < 0 || ay < 0 || ax >= kW || ay >= kH || sea(ax, ay)) continue;
                    int j = ay * kW + ax;
                    if (part[j] < 0) {
                        part[j] = parts;
                        todo.push_back(j);
                    }
                }
            }
            parts++;
        }
        std::vector<uint8_t> needs(parts, 0), served(parts, 0);
        for (int c = 0; c < CityCount(); c++) {
            int p = part[Cy(c) * kW + Cx(c)];
            if (p < 0) continue;
            needs[p] = 1;
            for (auto& r : kRoundCity)
                if (sea(Cx(c) + r[0], Cy(c) + r[1])) served[p] = 1;
        }
        for (int i = 0; i < kNSites; i++) {
            int x = S16(kSSites + kSiteStride * i), y = S16(kSSites + kSiteStride * i + 2);
            if (x >= 0 && y >= 0 && x < kW && y < kH && part[y * kW + x] >= 0)
                needs[part[y * kW + x]] = 1;
        }
        for (int i = 0; i < kW * kH; i++)
            if (tiles[i] & 0x8000) {
                int x = i % kW, y = i / kW;
                for (int d = 0; d < 8; d++) {
                    int ax = x + Nx(d), ay = y + Ny(d);
                    if (ax >= 0 && ay >= 0 && ax < kW && ay < kH && part[ay * kW + ax] >= 0)
                        served[part[ay * kW + ax]] = 1;
                }
            }
        for (int i = 0; i < kW * kH; i++) {
            int x = i % kW, y = i / kW;
            if (!sea(x, y) || (tiles[i] & 0x8000)) continue;
            int touched = -1;
            for (int d = 0; d < 8; d += 2) {  // the four sides
                int ax = x + Nx(d), ay = y + Ny(d);
                if (ax >= 0 && ay >= 0 && ax < kW && ay < kH && part[ay * kW + ax] >= 0)
                    touched = part[ay * kW + ax];
            }
            if (touched >= 0 && needs[touched] && !served[touched]) {
                tiles[i] |= 0x8000;
                served[touched] = 1;
            }
        }
    }

    /** Each road piece from the roads and bridges beside it, north, east,
     *  south and west (docs/formats/map.md > .RD), and an arm into the city a
     *  road ends at. */
    void ModernRoadShapes() {
        // the piece for each N 1, E 2, S 4, W 8 combination
        static const int kPiece[16] = {0, 15, 14, 10, 13, 2, 11, 7, 12, 9, 1, 6, 8, 5, 4, 3};
        const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        std::vector<int> shape(static_cast<size_t>(kW) * kH, 0);
        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                if (Road(x, y) == 0) continue;
                int mk = 0, city = -1;
                for (int d = 0; d < 4; d++) {
                    int ax = x + dx[d], ay = y + dy[d];
                    if (ax < 0 || ay < 0 || ax >= kW || ay >= kH) continue;
                    if (Road(ax, ay) != 0 || At(ax, ay) == kBridge)
                        mk |= 1 << d;
                    else if (At(ax, ay) == kCity && city < 0)
                        city = d;
                }
                // a road's end runs into the city beside it
                if (city >= 0 && std::popcount(static_cast<unsigned>(mk)) <= 1) mk |= 1 << city;
                if (mk == 0) {
                    SetRoad(x, y, 0);
                    continue;
                }
                shape[y * kW + x] = kPiece[mk];
            }
        for (int i = 0; i < kW * kH; i++)
            if (shape[i]) SetRoad(i % kW, i / kW, shape[i]);
        // a few straight pieces take their standing-stone look
        const int swaps[2][2] = {{2, 17}, {1, 16}};
        for (auto& sw : swaps) {
            int n = Dice(1, 6, 3);
            for (int k = 0; k < n; k++)
                for (int tries = 0; tries < 2000; tries++) {
                    int x = Dice(1, kW, -1), y = Dice(1, kH, -1);
                    if (Road(x, y) == sw[0]) {
                        SetRoad(x, y, sw[1]);
                        break;
                    }
                }
        }
    }

    /** Signposts by the roads, away from the cities, each pointing the way
     *  to the nearest city, a few with RANDOM.DAT's own words. */
    std::string ModernSigns() {
        std::vector<P> spots;
        for (int tries = 0; tries < 20000 && static_cast<int>(spots.size()) < 60; tries++) {
            int x = Dice(1, kW - 6, 2), y = Dice(1, kH - 6, 2);
            if (At(x, y) != kPlain || Road(x, y) != 0 || Tile(x, y) == 0) continue;
            bool by_road = false, clear = true;
            for (int dyy = -1; dyy <= 1; dyy++)
                for (int dxx = -1; dxx <= 1; dxx++)
                    if (Road(x + dxx, y + dyy) != 0) by_road = true;
            for (int dyy = -3; dyy <= 3 && clear; dyy++)
                for (int dxx = -3; dxx <= 3 && clear; dxx++) {
                    int t = At(x + dxx, y + dyy);
                    if (t == kCity || t == kSite) clear = false;
                }
            for (auto& p : spots)
                if (Reach(p.x, p.y, x, y) < 7) clear = false;
            if (by_road && clear) spots.push_back(P{x, y});
        }
        int n = static_cast<int>(spots.size());
        std::vector<uint8_t> out(2 + 104 * n, 0);
        SetI16(out, 0, n);
        auto put_str = [&](int o, const std::string& s) {
            for (size_t i = 0; i < s.size() && i < 49; i++) out[o + i] = static_cast<uint8_t>(s[i]);
        };
        int fixed = 0;
        for (int i = 0; i < n; i++) {
            int x = spots[i].x, y = spots[i].y, o = 2 + 104 * i;
            SetTile(x, y, 0);
            SetI16(out, o, x);
            SetI16(out, o + 2, y);
            if (Dice(1, 10, 0) < 3 && fixed < R16(kSigns)) {
                put_str(o + 4, Str(kSigns + 2 + 60 * fixed, 30));
                put_str(o + 54, Str(kSigns + 2 + 60 * fixed + 30, 30));
                fixed++;
                continue;
            }
            int c = -1, best = 1 << 30;
            for (int j = 0; j < CityCount(); j++) {
                int d = Distance(x, y, Cx(j), Cy(j));
                if (d < best) {
                    best = d;
                    c = j;
                }
            }
            if (c < 0) continue;
            put_str(o + 4, format(Str(kSignCity, 20), CstrOf(scn, Crec(c) + 4, 16)));
            put_str(o + 54, format(Str(kSignLeagues, 20), Distance(x, y, Cx(c), Cy(c)) * 2,
                                   std::string(kCompass[Compass(x, y, Cx(c), Cy(c))])));
        }
        return std::string(out.begin(), out.end());
    }

    /** The scenario generator, a phase a call, as next() is for the original. */
    std::optional<int> NextModern(Files& files) {
        switch (stage++) {
        case 0:
            return 0;
        case 1: {
            plan = scengen::makeWorld(world, *rng, kW, kH);
            for (int i = 0; i < kW * kH; i++) {
                m[kGrid + i] = plan.terrain[i];
                rd[i] &= 0xe0;
            }
            Shores();
            return 40;
        }
        case 2: {
            SetCityCount(0);
            for (auto& [x, y] : plan.cities) {
                int c = CityCount();
                Ws16(Crec(c), x);
                Ws16(Crec(c) + 2, y);
                SetCityCount(c + 1);
            }
            SitesBegin();
            for (int i = 0; i < kNSites && i < static_cast<int>(plan.sites.size()); i++)
                SiteAt(i, P{plan.sites[i].first, plan.sites[i].second});
            return 60;
        }
        case 3:
            RoadTiles();
            return 70;
        case 4:
            LayPlannedRoads();
            return 85;
        case 5:
            ModernRoadShapes();
            return 90;
        case 6:
            CityList();
            FairCapitals();
            Sides();
            return 94;
        case 7:
            CityDetails();
            return 96;
        case 8:
            sgn = ModernSigns();
            return 98;
        case 9:
            scn[kSRandom] = 0;
            scn[kSRandom + 1] = 0;  // an ordinary scenario
            WriteFiles(files);
            return 100;
        default:
            return std::nullopt;
        }
    }

    /** random_map_generate (4bed:011c), a phase a call: the progress figure
     *  the original's bar shows after it, or nothing once the world is made. */
    std::optional<int> Next(Files& files) {
        if (modern) return NextModern(files);
        switch (stage++) {
        case 0:
            return 0;
        case 1:
            // 4bed:036b: the map starts as sea
            for (int i = 0; i < kW * kH; i++) {
                m[kGrid + i] = kWater;
                rd[i] &= 0xe0;
            }
            Coast();
            return 10;
        case 2:
            Highlands();
            return 20;
        case 3:
            Erosion();
            return 30;
        case 4:
            Rivers();
            return 40;
        case 5:
            Forests();
            return 50;
        case 6:
            Marshes();
            return 60;
        case 7:
            Cities();
            return 70;
        case 8:
            SitesBegin();
            [[fallthrough]];
        case 9:
            // the sites, the bar moving on a tenth of the way through them each
            while (site_i < kNSites) {
                int now = site_i * 10 / kNSites;
                if (now != tenth) {
                    tenth = now;
                    stage = 9;
                    return 70 + now;
                }
                Site(site_i++);
            }
            return 80;
        case 10:
            return 81;
        case 11:
            RoadTiles();
            return 82;
        case 12:
            RoadCrossings();
            return 83;
        case 13:
            if (auto pct = RoadsUntilShown()) {
                stage = 13;
                return *pct;
            }
            return 90;
        case 14:
            // 4fef:0014
            RoadShapes();
            return 92;
        case 15:
            CityList();
            if (fair)
                FairCapitals();
            else
                Capitals();
            Sides();
            return 94;
        case 16:
            CityDetails();
            return 96;
        case 17:
            sgn = Signs();
            return 98;
        case 18:
            scn[kSRandom] = 1;
            scn[kSRandom + 1] = 0;
            WriteFiles(files);
            return 100;
        default:
            return std::nullopt;
        }
    }
};

std::array<int, 4> settle(std::array<int, 4> sliders, Rng& rng) {
    for (int& v : sliders) {
        if (v == RANDOM_SLIDER) v = rng.dice(1, 7, -1);
    }
    return sliders;
}

namespace {
/** random_map_load_params (4bed:0000): RANDOM.DAT with the sliders added. */
std::vector<uint8_t> Params(const std::string& data_dir, const std::array<int, 4>& sliders) {
    auto s = readFile(data_dir + "/RANDOM/RANDOM.DAT");
    if (!s) throw std::runtime_error("Random data not found");
    std::vector<uint8_t> m(s->begin(), s->end());
    if (m.size() < static_cast<size_t>(kDatSize)) m.resize(kDatSize, 0);
    auto add = [&](int o, int t, int i) { SetI16(m, o, GetI16(m, o) + GetI16(m, t + 2 * i)); };
    int water = sliders[0], hills = sliders[1], cities = sliders[2], forest = sliders[3];
    add(kPCities, kSliderCities, cities);
    add(kPForest, kSliderForest, forest);
    add(kPHills, kSliderHills, hills);
    add(kPMountains, kSliderHills, hills);
    add(kPWideRivers, kSliderWater, water);
    add(kPRivers, kSliderWater, water);
    return m;
}
}  // namespace

Run::Run(const Options& opts) {
    if (!opts.rng) throw std::runtime_error("random map: no dice");
    auto sliders = settle(opts.sliders, *opts.rng);
    gen_ = std::make_unique<Generator>();
    Generator& g = *gen_;
    g.m = Params(opts.dataDir, sliders);
    // auto_file_x_scn (4fef:1001): the scenario of the chosen terrain set,
    // Erythea for the only one there is, is the template
    auto t = readFile(opts.dataDir + "/ERYTHEA/ERYTHEA.SCN");
    if (!t) throw std::runtime_error("cannot open: ERYTHEA.SCN");
    g.scn.assign(t->begin(), t->end());
    armytype::load(opts.dataDir + "/TERRAIN0/ARMYTYPE.DAT", g.types);
    g.rng = opts.rng;
    g.allies = opts.allies;
    g.fair = opts.fairCapitals;
    g.modern = opts.scenario;
    g.world = opts.world;
    g.tiles.assign(static_cast<size_t>(kW) * kH, 0);
    g.rd.assign(static_cast<size_t>(kW) * kH, 0);
    g.scn[kSTerrainSet] = static_cast<uint8_t>(opts.terrainSet);
}

Run::~Run() = default;

bool Run::step() {
    auto pct = gen_->Next(files_);
    if (!pct) return false;
    pct_ = *pct;
    return true;
}

Files generateNow(const Options& opts) {
    Run run(opts);
    while (run.step()) {
    }
    return run.files();
}

void install(const std::string& data_dir, const Files& files) {
    for (auto& [ext, bytes] : files)
        installFile(data_dir + "/" + DIR + "/" + DIR + "." + ext, bytes);
}

Files installed(const std::string& data_dir) {
    Files out;
    for (const char* ext : FILES) {
        if (auto b = readFile(data_dir + "/" + DIR + "/" + DIR + "." + ext)) out[ext] = *b;
    }
    return out;
}

std::string terrainSetName(const std::string& data_dir, int set) {
    auto s = readFile(data_dir + "/DATA/TERRAIN.DAT");
    if (!s || set >= u16(*s, 0)) return "";
    return cstr(*s, 2 + 52 * set + 2, 50);
}

int terrainSets(const std::string& data_dir) {
    auto s = readFile(data_dir + "/DATA/TERRAIN.DAT");
    return s ? std::max(1, u16(*s, 0)) : 1;
}

}  // namespace w2::randommap
