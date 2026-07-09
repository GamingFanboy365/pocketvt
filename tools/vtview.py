#!/usr/bin/env python3
"""vtview: gives vtref.py eyes.

Adds a PPU write model ($2006 latch, $2007 -> CIRAM/palette with
auto-increment) and a VT03 16-colour COLCOMP=0 background renderer that
reproduces PocketVT's pixel math (4bpp tiles at bank*2048 with the
address-doubling, scattered palette index p0|p1<<1|set<<2|bgspr<<4|
p2<<5|p3<<6, transparency (idx&0x63)==0), colours via the session-11
vt_compat mapping fitted from the NESdev chart.  Sprites: 8x16 4bpp
metasprites from the OAM shadow page, enough to see actors.
"""
import importlib.util, sys
from PIL import Image

spec = importlib.util.spec_from_file_location("vtref", "/home/claude/vtref.py")
m = importlib.util.module_from_spec(spec)
_saved = sys.argv; sys.argv = ['vtref.py', 'none']
spec.loader.exec_module(m); sys.argv = _saved

exec(open('/tmp/vtchart.py').read())   # LUT[0xSLH] = (r,g,b)
exec(open('/tmp/vtcompat.py').read())  # M[LL] = (S,L)
def compat_rgb(idx):
    idx &= 0x3F
    H = idx & 0xF
    if H >= 14: return (0, 0, 0)
    S, L = M[(idx >> 4) & 3]
    return LUT[(S << 8) | (L << 4) | H]

PRG = m.PRG
WRAM = bytearray(0x2000)
CIRAM = bytearray(0x1000)
PALRAM = bytearray(0x80)
state = {'latch': 0, 'addr': 0, 'oam_page': 0x0200}

_og = m.Mem.__getitem__
_os = m.Mem.__setitem__

def g(self, a):
    if 0x6000 <= a < 0x8000:
        return WRAM[a - 0x6000]
    if 0x2000 <= a < 0x4000 and (a & 7) == 7:
        # $2007 read (rare in LI logic; buffered semantics ignored)
        v = CIRAM[state['addr'] & 0xFFF] if state['addr'] < 0x3F00 else PALRAM[state['addr'] & 0x7F]
        state['addr'] = (state['addr'] + (32 if m.SYS.ppu[0] & 4 else 1)) & 0x3FFF
        return v
    return _og(self, a)

def s(self, a, v):
    v &= 0xFF
    if 0x6000 <= a < 0x8000:
        WRAM[a - 0x6000] = v; return
    if 0x2140 <= a < 0x21C0:
        # VT palette window: lo bytes at $2140+offset (the path
        # vt_palette_write_lo serves in PocketVT) -- LI uploads its whole
        # palette here, NOT through $3F00/$2007.
        PALRAM[(a - 0x2140) & 0x7F] = v & 0x3F
        return
    if 0x2000 <= a < 0x4000:
        r = a & 7
        if r == 6:
            if state['latch'] == 0:
                state['addr'] = (state['addr'] & 0x00FF) | ((v & 0x3F) << 8)
            else:
                state['addr'] = (state['addr'] & 0x3F00) | v
            state['latch'] ^= 1
            return
        if r == 7:
            ad = state['addr'] & 0x3FFF
            if ad >= 0x3F00:
                PALRAM[ad & 0x7F] = v & 0x3F
            elif ad >= 0x2000:
                CIRAM[ad & 0xFFF] = v
            state['addr'] = (state['addr'] + (32 if m.SYS.ppu[0] & 4 else 1)) & 0x3FFF
            return
        if r == 2:
            state['latch'] = 0
    if a == 0x4034:
        state['dma_cfg'] = v
        _os(self, a, v); return
    if a == 0x4014:
        cfg = state.get('dma_cfg', 0)
        if cfg & 1:
            # VT VRAM DMA: blast from CPU memory into $2007 at the current
            # PPU address (this is how LI uploads its palette every frame:
            # $4034=$0F -> 128 bytes, source ($4014<<8)|((cfg>>4)<<4)).
            lengths = {0: 256, 4: 16, 5: 32, 6: 64, 7: 128}
            n = lengths.get((cfg >> 1) & 7, 256)
            src = ((v & 0xFF) << 8) | ((cfg >> 4) << 4)
            for k in range(n):
                b = m.SYS.ram[(src + k) & 0x7FF] if src < 0x2000 else 0
                ad = state['addr'] & 0x3FFF
                if ad >= 0x3F00:
                    PALRAM[ad & 0x7F] = b & 0x3F
                elif ad >= 0x2000:
                    CIRAM[ad & 0xFFF] = b
                state['addr'] = (state['addr'] + (32 if m.SYS.ppu[0] & 4 else 1)) & 0x3FFF
            return
        state['oam_page'] = (v & 0xFF) << 8
        _os(self, a, v); return
    if a == 0x4014:
        state['oam_page'] = (v & 0xFF) << 8
    _os(self, a, v)

m.Mem.__getitem__ = g
m.Mem.__setitem__ = s

def chr_fetch32(tile):
    """32 bytes of 4bpp data for pattern-space tile 0-511 via live banks."""
    ppu = m.SYS.ppu
    slot = tile >> 6
    if slot < 2:   bank = (ppu[0x16] & 0xFE) + slot
    elif slot < 4: bank = (ppu[0x17] & 0xFE) + (slot - 2)
    else:          bank = ppu[0x12 + (slot - 4)]
    base = (bank & 0xFF) * 2048 + (tile & 0x3F) * 32
    return PRG[base:base + 32]

def tile_pixels(tile):
    d = chr_fetch32(tile)
    rows = []
    for r in range(8):
        p0, p1, p2, p3 = d[r], d[r + 8], d[r + 16], d[r + 24]
        row = []
        for x in range(8):
            b = 7 - x
            row.append(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1)
                       | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3))
        rows.append(row)
    return rows

def render(path, nt_base=0):
    img = Image.new('RGB', (256, 240))
    px = img.load()
    nt = nt_base & 0x400
    for ty in range(30):
        for tx in range(32):
            tile = CIRAM[nt + ty * 32 + tx]
            if m.SYS.ppu[0] & 0x10: tile += 256
            at = CIRAM[nt + 0x3C0 + (ty // 4) * 8 + (tx // 4)]
            grp = (at >> (((ty & 2) << 1) | (tx & 2))) & 3
            pxs = tile_pixels(tile)
            for r in range(8):
                for c in range(8):
                    p = pxs[r][c]
                    idx = (p & 1) | ((p & 2) >> 1 << 1) | (grp << 2) | ((p & 4) << 3) | ((p & 8) << 3)
                    idx = (p & 3) | (grp << 2) | ((p & 0x4) << 3) | ((p & 0x8) << 3)
                    # scattered: p0|p1<<1|set<<2|bgspr<<4|p2<<5|p3<<6
                    idx = (p & 1) | (p & 2) | (grp << 2) | (((p >> 2) & 1) << 5) | (((p >> 3) & 1) << 6)
                    ci = PALRAM[0] if (idx & 0x63) == 0 else PALRAM[idx]
                    px[tx * 8 + c, ty * 8 + r] = compat_rgb(ci)
    # sprites: 8x16 4bpp, SPEXTEN EVA in attr bits 2-4
    oam = m.SYS.ram[state['oam_page'] & 0x7FF: (state['oam_page'] & 0x7FF) + 256]
    for i in range(0, 256, 4):
        y, t, at, x = oam[i], oam[i+1], oam[i+2], oam[i+3]
        if y >= 0xEF: continue
        eva = (at >> 2) & 7
        grp = at & 3
        base_t = (t & 0xFE) + ((t & 1) << 8) + (eva << 8)  # EVA extends tile number
        for half in range(2):
            tt = (base_t + half) & 0x1FF
            pxs = tile_pixels(tt)
            for r in range(8):
                for c in range(8):
                    p = pxs[r][c]
                    if p == 0: continue
                    idx = (p & 1) | (p & 2) | (grp << 2) | 0x10 | (((p >> 2) & 1) << 5) | (((p >> 3) & 1) << 6)
                    if (idx & 0x63) == 0: continue
                    cc = at & 0x40
                    dx = (7 - c) if cc else c
                    X, Y = x + dx, y + 1 + half * 8 + r
                    if 0 <= X < 256 and 0 <= Y < 240:
                        px[X, Y] = compat_rgb(PALRAM[idx])
    img.resize((512, 480), Image.NEAREST).save(path)
    return path

# expose module handles
M_ = m
if __name__ == '__main__':
    m.run_frames(300)
    render('/tmp/scene_map.png')
    print("map rendered; $2010 =", hex(m.SYS.ppu[0x10]))
