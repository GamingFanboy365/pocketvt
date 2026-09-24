#!/usr/bin/env python3
"""compare_furb.py -- run one .nes through Furbtendulator (furb_cli, the
reference) and through PocketVT (builder.py + mGBA), with the same input, and
compare the frames.  No screenshots needed.

    python3 tools/compare_furb.py ROM.nes --at 300,700 [--input "320-325:Start"]
        [--core pocketvt.gba] [--elf pocketvt.elf] [--port 1|2] [--window 8]
        [--search 4] [--set VT03Palette=1] [--out DIR]

Frames and input are in NES frames on both sides: PocketVT's own emulated-frame
counter (frametotal) drives its input and captures, so a cart PocketVT runs
below full speed still gets Start on the same NES frame as the reference.

For each --at frame the PocketVT screen (240x160) is compared with every
Furbtendulator frame within +-window, mapped through PocketVT's own per-line
row table (dma0buff: scale75's 3-in-4 decimation, raster splits removed; the
8-column crop), with a small +-search pixel shift to absorb the window offset.  Scores:
  struct  palette-independent: how consistently each reference colour maps to
          one PocketVT colour and back.  100% = same picture, any palette.
          Wrong tiles, missing sprites, bad scroll all lower it.
  exact5  pixels equal in 5-bit colour space (both >>3).  Only meaningful when
          the palettes are expected to match (they are calibrated differently).
Writes DIR/cmp_tNNNN.png (reference-mapped | PocketVT | mismatches in red, 2x),
DIR/furb_tNNNN.png (the reference frame, full 256x240) and DIR/report.json.

Needs: tools/furb_cli/build/furb_cli (python3 tools/furb_cli/build.py),
libmgba-dev, arm-none-eabi-nm, numpy, pillow.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FB = os.path.join(HERE, 'furb_cli')

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('rom')
ap.add_argument('--at', required=True, help='comma-separated NES frames to compare')
ap.add_argument('--input', action='append', default=[], help='furb_cli input script (repeatable)')
ap.add_argument('--core', default=os.path.join(REPO, 'pocketvt.gba'))
ap.add_argument('--elf', help='default: pocketvt.elf next to --core')
ap.add_argument('--port', type=int, default=1, help='NES port furb_cli feeds (PocketVT has one pad)')
ap.add_argument('--window', type=int, default=8, help='reference frames searched either side')
ap.add_argument('--search', type=int, default=4, help='pixel shift searched either way')
ap.add_argument('--set', action='append', default=[], help='furb_cli setting, e.g. VT03Palette=1')
ap.add_argument('--furb', default=os.path.join(FB, 'build', 'furb_cli'))
ap.add_argument('--out', default='furbcmp')
ap.add_argument('--keep', action='store_true', help='keep raw captures/dumps in OUT/raw')
a = ap.parse_args()

targets = sorted({int(t) for t in a.at.split(',') if t.strip()})
elf = a.elf or os.path.join(os.path.dirname(os.path.abspath(a.core)), 'pocketvt.elf')
for p, what in ((a.rom, 'ROM'), (a.core, 'core'), (elf, 'ELF'), (a.furb, 'furb_cli (python3 tools/furb_cli/build.py)')):
    if not os.path.exists(p):
        sys.exit('compare_furb: missing %s: %s' % (what, p))
os.makedirs(a.out, exist_ok=True)
tmp = os.path.join(a.out, 'raw') if a.keep else tempfile.mkdtemp(prefix='furbcmp_')
os.makedirs(tmp, exist_ok=True)

# ---------------------------------------------------------------- input script
KEYS = {'a': 1, 'b': 2, 'select': 4, 'sel': 4, 'start': 8, 'st': 8, 'right': 16, 'r': 16,
        'left': 32, 'l': 32, 'up': 64, 'u': 64, 'down': 128, 'd': 128}
script = ';'.join(a.input)
spans = []
for item in filter(None, (s.strip() for s in script.split(';'))):
    rng, _, rest = item.partition(':')
    rest = re.sub(r'^[pP][12]:', '', rest)
    first, _, last = rng.partition('-')
    mask = 0
    for b in filter(None, rest.split('+')):
        if b.lower() not in KEYS and b.lower() != 'none':
            sys.exit('compare_furb: unknown button %r' % b)
        mask |= KEYS.get(b.lower(), 0)
    spans.append((int(first), int(last or first), mask))
with open(os.path.join(tmp, 'keys.txt'), 'w') as f:
    for s in spans:
        f.write('%d %d %d\n' % s)

# ---------------------------------------------------------------- PocketVT side
harness = os.path.join(FB, 'build', 'pvt_run')
src = os.path.join(FB, 'pvt_run.c')
if not os.path.exists(harness) or os.path.getmtime(harness) < os.path.getmtime(src):
    os.makedirs(os.path.dirname(harness), exist_ok=True)
    subprocess.check_call(['gcc', '-O2', src, '-o', harness, '-lmgba'])

shutil.copy(a.core, os.path.join(tmp, 'pocketvt.gba'))
subprocess.check_call([sys.executable, os.path.join(REPO, 'builder.py'), os.path.abspath(a.rom)],
                      cwd=tmp, stdout=subprocess.DEVNULL)
play = os.path.join(tmp, 'play_me.gba')
if os.path.getsize(play) <= os.path.getsize(a.core):
    sys.exit('compare_furb: play ROM is core-sized -- builder.py injected nothing')

syms = {}
for line in subprocess.check_output(['arm-none-eabi-nm', elf], text=True).splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = parts[0]
for s in ('frametotal', '_dma0buff'):
    if s not in syms:
        sys.exit('compare_furb: symbol %s not in %s' % (s, elf))
pv = os.path.join(tmp, 'pv')
r = subprocess.run([harness, play, syms['frametotal'], syms['_dma0buff'], pv,
                    ','.join(map(str, targets)), os.path.join(tmp, 'keys.txt')],
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
if r.returncode:
    sys.exit('compare_furb: pvt_run failed:\n' + r.stderr[-2000:])
timeline = [tuple(map(int, l.split())) for l in open(pv + '_timeline.txt')]

# ---------------------------------------------------------------- reference side
dumps = sorted({f for t in targets for f in range(max(0, t - a.window), t + a.window + 1)})
cmd = [a.furb, os.path.abspath(a.rom), '--frames', str(max(dumps) + 1), '--dump', ','.join(map(str, dumps)),
       '--out', os.path.join(tmp, 'fb'), '--port', str(a.port), '--quiet']
if script:
    cmd += ['--input', script]
for s in a.set:
    cmd += ['--set', s]
r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
if r.returncode:
    sys.exit('compare_furb: furb_cli failed:\n' + r.stderr[-2000:])

def read_ppm(path):
    data = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():], np.uint8).reshape(h, w, 3)

# ---------------------------------------------------------------- scoring
def consistency(ref_ids, pv_ids):
    """Fraction of pixels agreeing with the dominant colour correspondence,
    taken in both directions (min), so merges and splits both count."""
    def one_way(a_, b_):
        pair = a_.astype(np.int64) << 32 | b_.astype(np.int64)
        u, cnt = np.unique(pair, return_counts=True)
        key = u >> 32
        order = np.lexsort((-cnt, key))
        first = np.ones(len(order), bool)
        first[1:] = key[order][1:] != key[order][:-1]
        best = dict(zip(key[order][first].tolist(), (u[order][first] & 0xFFFFFFFF).tolist()))
        return cnt[order][first].sum() / len(a_), best
    fwd, fmap = one_way(ref_ids, pv_ids)
    rev, _ = one_way(pv_ids, ref_ids)
    return min(fwd, rev), fmap

def screen_rows(vofs, top=16):
    """NES screen row shown on each GBA line.  dma0buff's vofs = NES scroll +
    (source row - line), so line-to-line steps are scale75's decimation (1 or
    2) except where the game changes scroll mid-frame (a raster split, e.g. a
    HUD); there the step is taken from the decimation pattern instead.  Row 0
    is the window top (16); remaining offset is found by the --search."""
    v = np.arange(len(vofs)) + vofs
    d = np.diff(v)
    normal = (d == 1) | (d == 2)
    doubled = np.nonzero(d == 2)[0]
    period_phase = np.bincount(doubled % 3, minlength=3).argmax() if len(doubled) else 0
    rows, seg = [top], [0]
    for i, step in enumerate(d):
        if not normal[i]:
            prev = doubled[doubled < i]		# continue the 3-line rhythm from the last doubled step
            step = 2 if (len(prev) and (i - prev[-1]) % 3 == 0) or (not len(prev) and i % 3 == period_phase) else 1
        rows.append(rows[-1] + int(step))
        seg.append(seg[-1] + (0 if normal[i] else 1))
    return np.array(rows), np.array(seg)	# seg: raster-split segment of each line

report = []
for t in targets:
    rgb = np.fromfile(pv + '_t%04d.rgb' % t, np.uint32).reshape(160, 240)
    pv_rgb = np.stack([rgb & 255, rgb >> 8 & 255, rgb >> 16 & 255], -1).astype(np.uint8)
    pv5 = (pv_rgb >> 3).astype(np.int64)
    pv_ids = (pv5[..., 0] << 10 | pv5[..., 1] << 5 | pv5[..., 2])
    geom = np.loadtxt(pv + '_t%04d.geom' % t, dtype=np.int64).reshape(160, 2)
    rows0, segs = screen_rows(geom[:, 1])
    rows0 = rows0[:, None]
    cols0 = np.arange(240)[None, :] + 8                                     # no horizontal scaling; 8-column crop

    best = None
    # nearest frames first; strict '>' keeps the nearest on ties (static screens)
    for f in sorted(range(max(0, t - a.window), t + a.window + 1), key=lambda f: (abs(f - t), f)):
        ref = read_ppm(os.path.join(tmp, 'fb_f%04d.ppm' % f))
        ref_ids_full = (ref[..., 0].astype(np.int64) << 16 | ref[..., 1].astype(np.int64) << 8 | ref[..., 2])
        for dy in range(-a.search, a.search + 1):
            for dx in range(-a.search, a.search + 1):
                ry, rx = rows0 + dy, cols0 + dx
                ok = (ry >= 0) & (ry < ref.shape[0]) & (rx >= 0) & (rx < ref.shape[1])
                if ok.mean() < 0.5:
                    continue
                ry_c, rx_c = np.clip(ry, 0, ref.shape[0] - 1), np.clip(rx, 0, ref.shape[1] - 1)
                ids = ref_ids_full[ry_c, rx_c]
                score, fmap = consistency(ids[ok], pv_ids[ok])
                if best is None or score > best[0]:
                    best = (score, f, dy, dx, fmap, ok, ref, ry_c, rx_c)
    score, f, dy, dx, fmap, ok, ref, ry_c, rx_c = best
    # each raster-split segment has its own vertical scroll: refine its offset
    seg_dy = {k: dy for k in range(segs.max() + 1)}
    if segs.max() > 0:
        ref_ids_full = (ref[..., 0].astype(np.int64) << 16 | ref[..., 1].astype(np.int64) << 8 | ref[..., 2])
        def eval_offsets(offs):
            ry = rows0 + np.array([offs[k] for k in segs])[:, None]
            rx = cols0 + dx
            ok_ = (ry >= 0) & (ry < ref.shape[0]) & (rx >= 0) & (rx < ref.shape[1])
            ry_, rx_ = np.clip(ry, 0, ref.shape[0] - 1), np.clip(rx, 0, ref.shape[1] - 1)
            sc, fm = consistency(ref_ids_full[ry_, rx_][ok_], pv_ids[ok_])
            return sc, fm, ok_, ry_, rx_
        for k in seg_dy:
            for cand in range(-a.search, a.search + 1):
                trial = {**seg_dy, k: cand}
                res = eval_offsets(trial)
                if res[0] > score:
                    score, (fmap, ok, ry_c, rx_c) = res[0], res[1:]
                    seg_dy = trial
    mapped = ref[ry_c, rx_c]
    mapped[~ok] = 0
    ids = (mapped[..., 0].astype(np.int64) << 16 | mapped[..., 1].astype(np.int64) << 8 | mapped[..., 2])
    expect = np.vectorize(lambda c: fmap.get(c, -1))(ids)
    struct_bad = ok & (expect != pv_ids)
    exact = ((mapped >> 3) == (pv_rgb >> 3)).all(-1) & ok

    # disagreeing colour pairs: ref colour -> (PocketVT colour it usually
    # becomes, colour it became instead, pixels).  black<->colour pairs are
    # usually positional (a row or sprite off); colour<->colour pairs point at
    # a palette difference.
    hexrgb = lambda c: '#%06x' % c
    pv24 = lambda c: '#%02x%02x%02x' % ((c >> 10 & 31) << 3, (c >> 5 & 31) << 3, (c & 31) << 3)
    bad_pairs = {}
    for rc, pc in zip(ids[struct_bad].tolist(), pv_ids[struct_bad].tolist()):
        bad_pairs[(rc, pc)] = bad_pairs.get((rc, pc), 0) + 1
    mismatch_pairs = [dict(ref=hexrgb(rc), usually=pv24(fmap[rc]) if rc in fmap else None, got=pv24(pc), px=n)
                     for (rc, pc), n in sorted(bad_pairs.items(), key=lambda kv: -kv[1])[:8]]

    diff = (pv_rgb // 3).copy()
    diff[struct_bad] = (255, 0, 0)
    diff[~ok] = (0, 0, 96)
    sheet = np.concatenate([mapped, np.full((160, 4, 3), 255, np.uint8), pv_rgb,
                            np.full((160, 4, 3), 255, np.uint8), diff], 1)
    Image.fromarray(sheet).resize((sheet.shape[1] * 2, 320), Image.NEAREST).save(os.path.join(a.out, 'cmp_t%04d.png' % t))
    Image.fromarray(ref).save(os.path.join(a.out, 'furb_t%04d.png' % t))

    gba_at = next((g for g, n in timeline if n >= t + 1), None)
    speed = None
    if gba_at is not None and gba_at >= 60:
        speed = timeline[gba_at][1] - timeline[gba_at - 60][1]	# NES frames per 60 GBA frames
    dy = seg_dy[0] if len(set(seg_dy.values())) == 1 else '/'.join('%+d' % seg_dy[k] for k in sorted(seg_dy))
    entry = dict(target=t, ref_frame=f, shift_x=dx, shift_y=dy, splits=int(segs.max()),
                 struct=round(100 * score, 2), exact5=round(100 * exact.sum() / ok.sum(), 2),
                 mismatched_px=int(struct_bad.sum()), mismatch_pairs=mismatch_pairs,
                 gba_frame=gba_at, nes_frames_per_60=speed,
                 image=os.path.join(a.out, 'cmp_t%04d.png' % t))
    report.append(entry)
    print('t=%-5d ref f%-5d shift %+d,%s  struct %6.2f%%  exact5 %6.2f%%  mismatched %5d px  '
          'speed %s/60  -> %s' % (t, f, dx, dy if isinstance(dy, str) else '%+d' % dy, entry['struct'], entry['exact5'], entry['mismatched_px'],
                                  speed, entry['image']))
    for cf in mismatch_pairs[:4]:
        print('        mismatch: ref %s (PocketVT usually %s) shows as %s on %d px' % (cf['ref'], cf['usually'], cf['got'], cf['px']))

json.dump(dict(rom=a.rom, core=a.core, input=script, port=a.port, results=report),
          open(os.path.join(a.out, 'report.json'), 'w'), indent=1)
if not a.keep:
    shutil.rmtree(tmp)
