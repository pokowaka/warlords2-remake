"""Human votes on generated maps, to check and weight the map evaluator.

The evaluator (bazel-bin/cpp/w2mapeval, docs/mapeval.md) scores maps against the
hand-made scenarios. Votes test whether those scores match what a player
prefers. A ballot is a batch of map pairs, each drawn side by side as A | B.
The voter picks A, B or = (no preference), and the votes are appended to
tools/mapvotes.jsonl so they can be checked against any later version of the
evaluator or of a generator.

    # 1. evaluate and export some maps
    bazel-bin/cpp/w2mapeval --seeds 1-200 --json /tmp/mv/eval.json --export /tmp/mv/maps
    # 2. make a ballot: pair-NN.png (clean), pair-NN-issues.png, ballot-B.json
    python3 tools/mapvote.py ballot --eval /tmp/mv/eval.json --maps /tmp/mv/maps --out DIR --batch 1
    #    or the same seeds from two generators (one --eval file per generator)
    python3 tools/mapvote.py ballot --eval c.json f.json --maps /tmp/mv/maps --out DIR --batch 2 \
        --matched classic,fair
    # 3. record the votes, e.g. pair 1 went to A, 2 to B, 3 no preference
    python3 tools/mapvote.py record --ballot DIR/ballot-1.json 1A 2B 3= --note 2 "roads make sense"
    # 4. how often the evaluator agrees, overall and metric by metric
    python3 tools/mapvote.py agree --eval /tmp/mv/eval.json

A map is named as the evaluator names it, "<generator>#<seed>". With the
default sliders, `bazel-bin/cpp/warlords2 --window --scenario RANDOM --seed N`
opens classic#N in the game.
"""
import argparse
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mapren import render, MAP_W, MAP_H  # noqa: E402
from pal import load as load_pal  # noqa: E402
from pck import write_png  # noqa: E402

VOTES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mapvotes.jsonl')
TERRAIN = 'original/TERRAIN0'
SCALE = 5                   # pixels a tile
GAP, HEADER = 24, 44        # between the two maps; the label bar

# colours added after the game's 16
EXTRA = [(255, 0, 255),     # 16 a hard-rule issue
         (255, 140, 0),     # 17 a soft-rule issue
         (110, 60, 20),     # 18 road
         (32, 32, 32)]      # 19 background
HARD, SOFT, ROAD, BACK = 16, 17, 18, 19
BLACK, WHITE, YELLOW = 0, 15, 7

# .SCN offsets (docs/formats/scenario.md)
SIDE_RECS, SIDE_REC_STRIDE, SIDE_COLOURS = 387, 20, 0xa0
SITES_COUNT, SITES, SITE_STRIDE = 0x80f, 0x811, 31
CITIES_COUNT, CITIES, CITY_STRIDE = 5499, 5501, 65

# what each road piece joins: N 1, E 2, S 4, W 8 (docs/formats/map.md > .RD)
ROAD_SHAPE = [0, 10, 5, 15, 14, 13, 11, 7, 12, 9, 3, 6, 8, 4, 2, 1, 10, 5]

GLYPHS = {   # 5 x 7
    'A': ['01110', '10001', '10001', '11111', '10001', '10001', '10001'],
    'B': ['11110', '10001', '10001', '11110', '10001', '10001', '11110'],
    '#': ['01010', '11111', '01010', '01010', '11111', '01010', '00000'],
    '0': ['01110', '10011', '10101', '10101', '11001', '10001', '01110'],
    '1': ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
    '2': ['01110', '10001', '00001', '00110', '01000', '10000', '11111'],
    '3': ['11110', '00001', '00001', '01110', '00001', '00001', '11110'],
    '4': ['00010', '00110', '01010', '10010', '11111', '00010', '00010'],
    '5': ['11111', '10000', '11110', '00001', '00001', '10001', '01110'],
    '6': ['00110', '01000', '10000', '11110', '10001', '10001', '01110'],
    '7': ['11111', '00001', '00010', '00100', '01000', '01000', '01000'],
    '8': ['01110', '10001', '10001', '01110', '10001', '10001', '01110'],
    '9': ['01110', '10001', '10001', '01111', '00001', '00010', '01100'],
    ' ': ['00000'] * 7,
}


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


class Canvas:
    def __init__(self, w, h, fill):
        self.w, self.h = w, h
        self.px = bytearray([fill]) * (w * h)

    def rect(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.h, y + h)):
            o = yy * self.w
            x0, x1 = max(0, x), min(self.w, x + w)
            if x1 > x0:
                self.px[o + x0:o + x1] = bytes([c]) * (x1 - x0)

    def outline(self, x, y, w, h, c, t=1):
        self.rect(x, y, w, t, c)
        self.rect(x, y + h - t, w, t, c)
        self.rect(x, y, t, h, c)
        self.rect(x + w - t, y, t, h, c)

    def blit(self, x, y, w, h, px):
        for yy in range(h):
            o = (y + yy) * self.w + x
            self.px[o:o + w] = px[yy * w:(yy + 1) * w]

    def text(self, x, y, s, c, size=4):
        for ch in s:
            g = GLYPHS.get(ch, GLYPHS[' '])
            for r, row in enumerate(g):
                for k, bit in enumerate(row):
                    if bit == '1':
                        self.rect(x + k * size, y + r * size, size, size, c)
            x += 6 * size


def base(folder):
    """The scenario's files in a folder: RANDOM.* or NAME.* for a folder NAME."""
    name = os.path.basename(os.path.normpath(folder))
    for stem in (name, 'RANDOM'):
        if os.path.exists(os.path.join(folder, stem + '.SCN')):
            return os.path.join(folder, stem)
    sys.exit('no scenario (.SCN) in %s' % folder)


def features(folder):
    """Cities (x, y, capital colour or None), sites and roads, from the files."""
    scn = open(base(folder) + '.SCN', 'rb').read()
    rd = open(base(folder) + '.RD', 'rb').read()
    caps = {}
    for i in range(8):
        o = SIDE_RECS + SIDE_REC_STRIDE * i
        caps[(u16(scn, o + 6), u16(scn, o + 8))] = u16(scn, SIDE_COLOURS + 2 * i) % 16
    cities = []
    for i in range(u16(scn, CITIES_COUNT)):
        o = CITIES + CITY_STRIDE * i
        x, y = u16(scn, o), u16(scn, o + 2)
        cities.append((x, y, caps.get((x, y))))
    sites = []
    for i in range(u16(scn, SITES_COUNT)):
        o = SITES + SITE_STRIDE * i
        sites.append((u16(scn, o), u16(scn, o + 2)))
    return cities, sites, rd


def draw_map(folder, issues=None):
    """The map in the game's own tiles, with roads, cities and sites drawn
    bold over it so they read at 5 pixels a tile; issues ringed if given."""
    w, h, px = render(base(folder) + '.MAP', TERRAIN, SCALE)
    cv = Canvas(w, h, BACK)
    cv.blit(0, 0, w, h, px)
    cities, sites, rd = features(folder)
    s, mid = SCALE, SCALE // 2
    for y in range(MAP_H):
        for x in range(MAP_W):
            piece = rd[y * MAP_W + x] % 32
            if not piece or piece > 17:
                continue
            shape, cx, cy = ROAD_SHAPE[piece], x * s + mid, y * s + mid
            cv.rect(cx - 1, cy - 1, 2, 2, ROAD)
            if shape & 1: cv.rect(cx - 1, y * s, 2, mid + 1, ROAD)
            if shape & 4: cv.rect(cx - 1, cy, 2, s - mid, ROAD)
            if shape & 8: cv.rect(x * s, cy - 1, mid + 1, 2, ROAD)
            if shape & 2: cv.rect(cx, cy - 1, s - mid, 2, ROAD)
    for x, y in sites:
        cv.rect(x * s, y * s, s, s, YELLOW)
        cv.outline(x * s, y * s, s, s, BLACK)
    for x, y, cap in cities:
        if cap is None:
            cv.rect(x * s, y * s, 2 * s, 2 * s, WHITE)
            cv.outline(x * s, y * s, 2 * s, 2 * s, BLACK)
        else:
            cv.rect(x * s - 2, y * s - 2, 2 * s + 4, 2 * s + 4, BLACK)
            cv.rect(x * s, y * s, 2 * s, 2 * s, cap)
            cv.outline(x * s - 2, y * s - 2, 2 * s + 4, 2 * s + 4, WHITE)
    for i in issues or []:
        c = SOFT if i['check'] == 'spur-road' else HARD
        cv.outline((i['x'] - 2) * s, (i['y'] - 2) * s, 5 * s, 5 * s, c, 2)
    return cv


def draw_pair(n, left, right, maps, issues_of=None):
    a = draw_map(os.path.join(maps, folder_of(left)), issues_of and issues_of[left])
    b = draw_map(os.path.join(maps, folder_of(right)), issues_of and issues_of[right])
    cv = Canvas(a.w * 2 + GAP, a.h + HEADER, BACK)
    cv.blit(0, HEADER, a.w, a.h, a.px)
    cv.blit(a.w + GAP, HEADER, b.w, b.h, b.px)
    cv.text(a.w // 2 - 12, 8, 'A', WHITE, 4)
    cv.text(a.w + GAP + b.w // 2 - 12, 8, 'B', WHITE, 4)
    cv.text(a.w + GAP // 2 - 24, 14, '#%d' % n, WHITE, 2)
    return cv


def folder_of(name):
    return name.replace('#', '-')


def load_eval(paths):
    """Every map in one or more evaluator JSON files, by name."""
    out = {}
    for path in paths if isinstance(paths, list) else [paths]:
        out.update({m['name']: m for m in json.load(open(path))['maps']})
    return out


def voted():
    out = []
    if os.path.exists(VOTES):
        for line in open(VOTES):
            if line.strip():
                out.append(json.loads(line))
    return out


def choose_pairs(maps, count, rng):
    """A spread of pairs: clear-cut ones (to test the score's direction),
    close ones whose problems differ (to learn what matters more), and
    hard-rule ones (a map that fails a rule but scores well otherwise)."""
    seen = {v['a'] for v in voted()} | {v['b'] for v in voted()}
    pool = [m for m in maps.values() if '#' in m['name'] and m['name'] not in seen]
    pool.sort(key=lambda m: m['score']['penalty'])
    used, pairs = set(), []

    def take(a, b, kind):
        used.update((a['name'], b['name']))
        pairs.append((a, b, kind) if rng.random() < 0.5 else (b, a, kind))

    def free(lst):
        return [m for m in lst if m['name'] not in used]

    q = max(1, len(pool) // 4)
    per = count // 3
    best, worst = pool[:q], pool[-q:]
    for _ in range(per):
        a, b = free(best), free(worst)
        if a and b:
            take(rng.choice(a[:6]), rng.choice(b[-6:]), 'clear')

    def devs(m):
        return m['score'].get('deviations', {})

    for _ in range(per):
        cand, top = free(pool), None
        for i, a in enumerate(cand):
            for b in cand[i + 1:i + 12]:
                pa, pb = a['score']['penalty'], b['score']['penalty']
                if abs(pa - pb) > 0.15 * max(pa, pb, 1):
                    continue
                keys = set(devs(a)) | set(devs(b))
                d = sum(abs(abs(devs(a).get(k, 0)) - abs(devs(b).get(k, 0))) for k in keys)
                if top is None or d > top[0]:
                    top = (d, a, b)
        if top:
            take(top[1], top[2], 'close')

    while len(pairs) < count:
        cand = free(pool)
        good = [m for m in cand if not m['passed']][:8]
        clean = [m for m in cand if m['passed']][-8:]
        if good and clean:
            take(rng.choice(clean), rng.choice(good), 'rules')
        elif len(cand) >= 2:
            a, b = rng.sample(cand, 2)
            take(a, b, 'random')
        else:
            break
    rng.shuffle(pairs)
    return pairs


def matched_pairs(maps, gens, count, rng):
    """The same seed from two generators, side by side in a random order:
    the same world, made two ways, so only what differs is judged."""
    a, b = gens
    seen = {v['a'] for v in voted()} | {v['b'] for v in voted()}
    seeds = sorted({n.split('#')[1] for n in maps if n.startswith(a + '#')} &
                   {n.split('#')[1] for n in maps if n.startswith(b + '#')}, key=int)
    seeds = [s for s in seeds if '%s#%s' % (a, s) not in seen and '%s#%s' % (b, s) not in seen
             and 'classic#%s' % s not in seen]
    pairs = []
    picked = rng.sample(seeds, min(count, len(seeds)))
    for k, s in enumerate(picked):
        x, y = maps['%s#%s' % (a, s)], maps['%s#%s' % (b, s)]
        # each generator on the left exactly half the time
        pairs.append((x, y, 'matched') if k % 2 == 0 else (y, x, 'matched'))
    rng.shuffle(pairs)
    return pairs


def cmd_ballot(args):
    maps = load_eval(args.eval)
    rng = random.Random(args.batch)
    if args.matched:
        pairs = matched_pairs(maps, args.matched.split(','), args.pairs, rng)
    else:
        pairs = choose_pairs(maps, args.pairs, rng)
    os.makedirs(args.out, exist_ok=True)
    pal = load_pal(os.path.join(TERRAIN, 'WAR2.PAL')) + EXTRA
    issues_of = {m['name']: m['issues'] for m in maps.values()}
    ballot = {'batch': args.batch, 'eval': [os.path.abspath(e) for e in args.eval],
              'maps': os.path.abspath(args.maps),
              'pairs': []}
    for n, (a, b, kind) in enumerate(pairs, 1):
        cv = draw_pair(n, a['name'], b['name'], args.maps)
        write_png(os.path.join(args.out, 'pair-%02d.png' % n), cv.w, cv.h, bytes(cv.px), pal)
        cv = draw_pair(n, a['name'], b['name'], args.maps, issues_of)
        write_png(os.path.join(args.out, 'pair-%02d-issues.png' % n), cv.w, cv.h, bytes(cv.px), pal)
        ballot['pairs'].append({'pair': n, 'a': a['name'], 'b': b['name'], 'kind': kind,
                                'penalty': [a['score']['penalty'], b['score']['penalty']],
                                'passed': [a['passed'], b['passed']]})
        print('pair %2d  A %-12s %6.2f %-5s  B %-12s %6.2f %-5s  %s' % (
            n, a['name'], a['score']['penalty'], 'pass' if a['passed'] else 'FAIL',
            b['name'], b['score']['penalty'], 'pass' if b['passed'] else 'FAIL', kind))
    path = os.path.join(args.out, 'ballot-%d.json' % args.batch)
    json.dump(ballot, open(path, 'w'), indent=1)
    print('wrote %s and %d pair images to %s' % (path, len(pairs), args.out))


def cmd_record(args):
    ballot = json.load(open(args.ballot))
    pairs = {p['pair']: p for p in ballot['pairs']}
    notes = {int(n): t for n, t in (args.note or [])}
    lines = []
    for v in args.votes:
        n, pick = int(v[:-1]), v[-1].upper()
        if n not in pairs or pick not in 'AB=':
            sys.exit('bad vote %r: give pair number then A, B or =' % v)
        p = pairs[n]
        lines.append({'batch': ballot['batch'], 'pair': n, 'a': p['a'], 'b': p['b'], 'kind': p['kind'],
                      'winner': {'A': 'a', 'B': 'b', '=': 'tie'}[pick], 'note': notes.get(n, ''),
                      'penalty': p['penalty'], 'passed': p['passed']})
    with open(VOTES, 'a') as f:
        for line in lines:
            f.write(json.dumps(line) + '\n')
    print('recorded %d vote(s) in %s' % (len(lines), VOTES))


def cmd_agree(args):
    maps = load_eval(args.eval)
    votes = [v for v in voted() if v['winner'] != 'tie' and v['a'] in maps and v['b'] in maps]
    if not votes:
        sys.exit('no decided votes on maps in %s' % ' '.join(args.eval))

    def pen(n):
        return maps[n]['score']['penalty']

    agree = sum((pen(v['a']) < pen(v['b'])) == (v['winner'] == 'a') for v in votes
                if pen(v['a']) != pen(v['b']))
    decided = sum(pen(v['a']) != pen(v['b']) for v in votes)
    print('%d decided vote(s); the lower penalty won %d of %d (%.0f%%)' % (
        len(votes), agree, decided, 100.0 * agree / max(1, decided)))
    rules = [v for v in votes if maps[v['a']]['passed'] != maps[v['b']]['passed']]
    if rules:
        ok = sum(maps[v[v['winner']]]['passed'] for v in rules)
        print('where one map broke a hard rule, the clean one won %d of %d' % (ok, len(rules)))
    keys = sorted({k for m in maps.values() for k in m['score'].get('deviations', {})})
    print('\n%-26s %6s %6s %8s' % ('metric', 'votes', 'agree', 'rate'))
    for k in keys:
        n = a = 0
        for v in votes:
            da = abs(maps[v['a']]['score'].get('deviations', {}).get(k, 0))
            db = abs(maps[v['b']]['score'].get('deviations', {}).get(k, 0))
            if da == db:
                continue
            n += 1
            a += (da < db) == (v['winner'] == 'a')
        if n:
            print('%-26s %6d %6d %7.0f%%' % (k, n, a, 100.0 * a / n))


def cmd_render(args):
    pal = load_pal(os.path.join(TERRAIN, 'WAR2.PAL')) + EXTRA
    cvs = [draw_map(f) for f in args.folders]
    w = sum(c.w for c in cvs) + GAP * (len(cvs) - 1)
    out = Canvas(w, cvs[0].h, BACK)
    x = 0
    for c in cvs:
        out.blit(x, 0, c.w, c.h, c.px)
        x += c.w + GAP
    write_png(args.out, out.w, out.h, bytes(out.px), pal)
    print('wrote %s' % args.out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('ballot')
    b.add_argument('--eval', required=True, nargs='+')
    b.add_argument('--maps', required=True)
    b.add_argument('--matched', metavar='GEN_A,GEN_B',
                   help='pair the same seeds from two generators instead')
    b.add_argument('--out', required=True)
    b.add_argument('--batch', type=int, required=True)
    b.add_argument('--pairs', type=int, default=12)
    r = sub.add_parser('record')
    r.add_argument('--ballot', required=True)
    r.add_argument('--note', nargs=2, action='append', metavar=('PAIR', 'TEXT'))
    r.add_argument('votes', nargs='+')
    a = sub.add_parser('agree')
    a.add_argument('--eval', required=True, nargs='+')
    d = sub.add_parser('render', help='draw scenario folders side by side')
    d.add_argument('out')
    d.add_argument('folders', nargs='+')
    args = p.parse_args()
    {'ballot': cmd_ballot, 'record': cmd_record, 'agree': cmd_agree, 'render': cmd_render}[args.cmd](args)


if __name__ == '__main__':
    main()
