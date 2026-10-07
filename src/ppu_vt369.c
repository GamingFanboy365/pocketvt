/*
 * ppu_vt369.c -- the VT369 "enhanced" picture ($201E != 0), guide s.82.
 *
 * In enhanced mode a VT369 draws backgrounds straight from PRG ROM: packed
 * 4bpp (32-byte tiles, low nibble = left pixel) or 8bpp (64-byte tiles, one
 * byte per pixel).  Both are the GBA's own tile formats, so tiles go from ROM
 * to VRAM with a plain copy.  A tile number is 10 bits: the nametable byte,
 * plus the cell's two attribute bits as bits 8-9 unless $201C bit 3 is set.
 * Colours come from a 1024-byte palette ($3C00-$3FFF through $2007, or DMA):
 * BG colour c is the 15-bit pair at 2c, sprite colour c at 0x200 + 2c
 * (NintendulatorNRS / Furbtendulator OneBus_VT369.cpp, RunNoSkipEnhanced and
 * GetPalIndex).
 *
 * While enhanced mode is on this module owns the BG tile data, the game's
 * tilemap and the GBA palette.  PocketNES's own BG writers are switched off
 * through vt_bkexten_live (the same gates BKEXTEN uses); PocketNES still
 * does the scrolling and scaling (per-line BG0HOFS/VOFS and BG0CNT), and
 * vt369_frame_end only patches the colour depth into each line's BG0CNT.
 *
 * A screen can use more distinct tiles than fit in GBA VRAM next to the UI
 * layer and the map (Fire Fighter's title: 743 8bpp tiles), so tiles are
 * cached: each distinct tile number gets a VRAM slot, reference-counted by
 * the map cells showing it.  Slots come from the free BG VRAM below the map
 * first; the 32K at 0x06008000 (the VRAM copy of PRG, guide s.77a) is used
 * only when those run out, after vt_prg_evict has moved the PRG copy.
 */
#include "includes.h"
#include "ppu_vt.h"
#include "vt_regs.h"
#include "config.h"

#if VT_MODE

extern u8 vt_console;
extern u8 vt_bkexten_live;
extern u8 vt_reg_2010;
extern u8 _bg_cache_full;
extern u8 _ppuctrl0;
extern u8 vt_dma_settings;
extern u32 vt_nes_ram_mask;
extern u8 vt_chr_reg[6];
extern u32 _vram_write_tbl[16];
extern const u32 vt_bk_consts[4];      /* ppu.s: BG_CACHE, NES_VRAM2, NES_VRAM4, AGB_BG */
extern void vt369_pal_W(void);         /* mapVT.s: $3C00-$3FFF $2007 writes */
extern int vt369_high_slots_ok(void);  /* ppu_vt.c: PRG copy moved out of 0x06008000? */
extern int vt369_obj_ok(void);         /* ppu_vt.c: PRG page 0 moved out of OBJ VRAM? */
extern u8 YSCALE_LOOKUP[];             /* mem.s */

EWRAM_BSS u8  vt369_reg[0x40];         /* $2000-$203F as last written (extended part) */
EWRAM_BSS u8  vt369_enh;               /* enhanced picture is live */
EWRAM_BSS u8  vt369_pal[1024] __attribute__((aligned(4)));
EWRAM_BSS u8  vt369_pal_dirty;
/* s.89: one byte a palette word, set by the writers (vt369_dma_4014 when a
 * word changes, vt369_pal_W in mapVT.s); vt369_pal_upload converts only
 * those.  Comparing all 256 words every vblank cost Fire Fighter ~13K
 * cycles a NES frame (Thumb from ROM, EWRAM loads). */
EWRAM_BSS u8  vt369_pal_dw[256] __attribute__((aligned(4)));
EWRAM_BSS u8  vt369_oam[512] __attribute__((aligned(4)));
EWRAM_BSS u8  vt369_oam_hi;            /* $2008 bit 0: which 256-byte OAM half */
EWRAM_BSS u8  vt369_oam_live;          /* s.88: a fast DMA has filled vt369_oam */
EWRAM_BSS u32 vt369_saved_palW;        /* vram_write_tbl[15] before we took it */
EWRAM_BSS u32 vt369_dbg[8];            /* 0 full sweeps 1 tile copies 2 alloc fails 3 stomps 4 cells 5 cfg */

/* ---- tile cache -------------------------------------------------------- */
#define VT369_NSLOT 832
EWRAM_BSS u8  vt369_nt_shadow[0x800] __attribute__((aligned(4)));
EWRAM_BSS u16 vt369_cell[0x800];       /* tile shown by each map cell, 0xFFFF = none */
EWRAM_BSS u16 vt369_t2s[1024];         /* tile -> slot, 0xFFFF = not cached */
EWRAM_BSS u16 vt369_s2t[VT369_NSLOT];  /* slot -> tile, 0xFFFF = empty */
EWRAM_BSS u16 vt369_rc[VT369_NSLOT];   /* map cells using the slot */
EWRAM_BSS u16 vt369_alloc_cur;
EWRAM_BSS u16 vt369_verify_cur;
EWRAM_BSS u8  vt369_full;              /* re-derive every map cell */
EWRAM_BSS u8  vt369_retry;             /* a cell found no slot: sweep again */
/* s.85: when no slot is free, say so at once until one is freed (a count
 * drops to 0) or more slots open up.  Each failing cell used to scan all
 * ~830 slots and force a full rebuild the next frame, which failed the
 * same way: ~1 s of GBA time per NES frame while a screen holds more
 * distinct tiles than there are slots (the boot screens of Jewel Master
 * and Fire Fighter). */
EWRAM_BSS u16 vt369_free_gen;          /* bumps when a slot's count drops to 0 */
EWRAM_BSS u16 vt369_full_gen;          /* free_gen when alloc last found none */
EWRAM_BSS u16 vt369_full_n;            /* slot count then */
EWRAM_BSS u8  vt369_nofree;
EWRAM_BSS u32 vt369_cfg;               /* cfg the cache was built for */
EWRAM_BSS u16 vt369_gba_pal[512] __attribute__((aligned(4)));   /* 256 BG + 256 OBJ (s.88) */

/* Slot ranges, as GBA tile indices from char base 0.  8bpp tiles are 64
 * bytes, 4bpp 32.  Skipped: 0x0000-0x003F (the UI layer's off-screen cells
 * point at tiles 0 and 1), 0x2000-0x3FFF (the UI layer), 0x6000-0x6FFF (the
 * game's tilemap).  The last range of each list is 0x06008000+ (needs the
 * PRG copy moved); 4bpp cannot reach it from char base 0. */
typedef struct { u16 first, count; } vt369_range;
static const vt369_range vt369_r8[4] = { {1, 127}, {256, 128}, {448, 64}, {512, 512} };
static const vt369_range vt369_r4[3] = { {2, 254}, {512, 256}, {896, 128} };
#define VT369_LOW8  (127 + 128 + 64)
#define VT369_ALL8  (VT369_LOW8 + 512)
#define VT369_ALL4  (254 + 256 + 128)

static inline u16 vt369_col(u32 lo, u32 hi)
{
    const u32 c = (hi << 8 | lo) & 0x7FFF;           /* 0RRRRRGGGGGBBBBB */
    return (u16)(((c & 0x1F) << 10) | (c & 0x03E0) | (c >> 10));
}

static inline int vt369_bpp8(void) { return (vt369_reg[0x1C] & 3) == 2; }
/* s.88: ProcessSpritesEnhanced's "Cross River" rule, which Sky Fighter
 * meets too ($201E = $0F, $201D = $0B, BKEXTEN): tile bits 8-10 move down
 * to 6-8.  Without it every sprite from tile 256 up (Sky Fighter's ship and
 * enemies) drew someone else's pattern as striped blocks. */
static inline u32 vt369_tile_fix(u32 tile)
{
    if (vt369_reg[0x1E] == 0x0F && vt369_reg[0x1D] == 0x0B && (vt_reg_2010 & 0x10))
        tile = (tile & 0x3F) | ((tile >> 2) & ~0x3Fu);
    return tile;
}

/* s.88: 8bpp sprites, 8 wide, a byte a pixel: $201D bit 2 (one 16-row tile)
 * or $201C bit 5, unless $201D bit 1 makes them 16 wide 4bpp
 * (ProcessSpritesEnhanced).  Jumper and Sky Fighter use $201C = $22. */
static inline int vt369_spr8(void)
{
    return !(vt369_reg[0x1D] & 2) && ((vt369_reg[0x1D] & 4) || (vt369_reg[0x1C] & 0x20));
}

/* Byte offset in PRG of tile 0 (OneBus_VT369.cpp: vt369bgData when $201E
 * bit 0 is set, else the $2012/$2016 bank).  The outer-bank terms
 * (relative8K, AND/OR) are 0 on the single-game carts seen so far. */
static u32 vt369_bg_base(void)
{
    if (vt369_reg[0x1E] & 1)
        return ((u32)(vt369_reg[0x20] | vt369_reg[0x21] << 8) & 0xFFF) << 13;
    const u32 bank = (_ppuctrl0 & 0x10) ? vt_chr_reg[0] : vt_chr_reg[4];
    return bank << ((vt369_reg[0x1C] & 8) ? 10 : 13) << (vt369_reg[0x1C] & 3);
}

static inline const u8 *vt369_prg(void) { return vt_chr_src ? vt_chr_src : rombase; }
static inline u32 vt369_prg_mask(void)
{
    const u32 m = vt_chr_src ? vt_chr_mask : rommask;
    return m ? m : 0xFFFFFFFFu;
}

static u32 vt369_slot_idx(u32 s)
{
    const vt369_range *r = vt369_bpp8() ? vt369_r8 : vt369_r4;
    for (;;) {
        if (s < r->count) return r->first + s;
        s -= r->count; r++;
    }
}

/* s.88: tiles with identical pattern bytes share one slot.  Each tile
 * number maps to the first tile seen with its 32/64 bytes (hash table,
 * reset with the cache).  Sky Fighter's 8bpp forest uses 960 tile numbers
 * but only 603 distinct tiles; the pool holds at most 831, so its last map
 * rows found no slot and showed stale tiles as a smeared band. */
EWRAM_BSS u16 vt369_canon[1024];
EWRAM_BSS u16 vt369_chash[2048];

static u32 vt369_canon_tile(u32 tile)
{
    const u32 c = vt369_canon[tile];
    if (c != 0xFFFF) return c;
    const u32 tsz = vt369_bpp8() ? 64u : 32u, base = vt369_bg_base(), m = vt369_prg_mask();
    const u8 *prg = vt369_prg();
    const u32 *d = (const u32 *)(prg + ((base + tile * tsz) & m));
    u32 h = 0;
    for (u32 i = 0; i < tsz / 4; i++) h = (h ^ d[i]) * 0x9E3779B1u;
    for (u32 k = h >> 21;; k = (k + 1) & 2047) {
        const u32 t = vt369_chash[k];
        if (t == 0xFFFF) { vt369_chash[k] = (u16)tile; vt369_canon[tile] = (u16)tile; return tile; }
        const u32 *e = (const u32 *)(prg + ((base + t * tsz) & m));
        u32 i = 0;
        while (i < tsz / 4 && e[i] == d[i]) i++;
        if (i == tsz / 4) { vt369_canon[tile] = (u16)t; return t; }
    }
}

static void vt369_cache_reset(void)
{
    for (int i = 0; i < 1024; i++) vt369_canon[i] = 0xFFFF;
    for (int i = 0; i < 2048; i++) vt369_chash[i] = 0xFFFF;
    for (int i = 0; i < 1024; i++) vt369_t2s[i] = 0xFFFF;
    for (int i = 0; i < 0x800; i++) vt369_cell[i] = 0xFFFF;
    for (int i = 0; i < VT369_NSLOT; i++) { vt369_s2t[i] = 0xFFFF; vt369_rc[i] = 0; }
    vt369_alloc_cur = 0;
    vt369_verify_cur = 0;
    vt369_full = 1;
    vt369_nofree = 0;
}

static void vt369_copy_tile(u32 s, u32 tile)
{
    const u32 tsz = vt369_bpp8() ? 64u : 32u;
    const u32 *src = (const u32 *)(vt369_prg() + ((vt369_bg_base() + tile * tsz) & vt369_prg_mask()));
    volatile u32 *dst = (volatile u32 *)(0x06000000u + vt369_slot_idx(s) * tsz);
    for (u32 i = 0; i < tsz / 4; i += 4) {
        const u32 a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
        dst[i] = a; dst[i + 1] = b; dst[i + 2] = c; dst[i + 3] = d;
    }
    vt369_dbg[1]++;
}

static u32 vt369_nslot_now(void)
{
    if (!vt369_bpp8()) return VT369_ALL4;
    return vt369_high_slots_ok() ? VT369_ALL8 : VT369_LOW8;
}

/* A slot no map cell uses, preferring the oldest; 0xFFFF if none. */
static u32 vt369_alloc(void)
{
    const u32 n = vt369_nslot_now();
    if (vt369_nofree && vt369_full_gen == vt369_free_gen && vt369_full_n == n) return 0xFFFF;
    u32 s = vt369_alloc_cur;
    for (u32 k = 0; k < n; k++, s++) {
        if (s >= n) s = 0;
        if (vt369_rc[s] == 0) { vt369_alloc_cur = (u16)(s + 1); return s; }
    }
    vt369_nofree = 1; vt369_full_gen = vt369_free_gen; vt369_full_n = (u16)n;
    return 0xFFFF;
}

__attribute__((noinline))
static void vt369_cell_set(u32 o, u32 tile)
{
    const u32 old = vt369_cell[o];
    if (old == tile) return;
    if (old != 0xFFFF) {
        const u32 so = vt369_t2s[old];
        if (so != 0xFFFF && vt369_rc[so] && --vt369_rc[so] == 0) vt369_free_gen++;
    }
    u32 s = vt369_t2s[tile];
    if (s == 0xFFFF) {
        s = vt369_alloc();
        if (s == 0xFFFF) { vt369_cell[o] = 0xFFFF; vt369_retry = 1; vt369_dbg[2]++; return; }
        const u32 prev = vt369_s2t[s];
        if (prev != 0xFFFF) vt369_t2s[prev] = 0xFFFF;
        vt369_s2t[s] = (u16)tile;
        vt369_t2s[tile] = (u16)s;
        vt369_copy_tile(s, tile);
    }
    vt369_rc[s]++;
    vt369_cell[o] = (u16)tile;
    ((volatile u16 *)vt_bk_consts[3])[o] = (u16)vt369_slot_idx(s);
    vt369_dbg[4]++;
}

/* Tile number of NES nametable cell o (0-0x7FF; o & 0x3FF < 0x3C0). */
static inline u32 vt369_cell_tile(const u8 *nt, u32 o)
{
    u32 tile = nt[o];
    if (!(vt369_reg[0x1C] & 8)) {
        const u32 t = o & 0x3FF, col = t & 31, row = t >> 5;
        const u32 ab = nt[(o & 0x400) + 0x3C0 + ((row >> 2) << 3) + (col >> 2)];
        tile |= ((ab >> (((row & 2) << 1) | (col & 2))) & 3) << 8;
    }
    return vt369_canon_tile(tile);
}

/* One changed nametable word: 4 name cells, or 4 attribute bytes (16 cells
 * each).  Kept out of line so the diff loop below stays in registers (it
 * runs on the EWRAM stack, where a spill costs 3 cycles). */
__attribute__((noinline))
static void vt369_nt_word(const u8 *nt, u32 w, u32 was, u32 v)
{
    const u32 o = w << 2, t = o & 0x3FF;
    if (t < 0x3C0) {
        for (u32 k = 0; k < 4; k++) vt369_cell_set(o + k, vt369_cell_tile(nt, o + k));
        return;
    }
    for (u32 k = 0; k < 4; k++) {
        if (!(((was ^ v) >> (k * 8)) & 0xFF)) continue;
        const u32 a = t - 0x3C0 + k, ax = (a & 7) << 2, ay = (a >> 3) << 2;
        const u32 scr = o & 0x400;
        for (u32 ry = 0; ry < 4 && ay + ry < 30; ry++)
            for (u32 rx = 0; rx < 4; rx++) {
                const u32 c = scr + (ay + ry) * 32 + ax + rx;
                vt369_cell_set(c, vt369_cell_tile(nt, c));
            }
    }
}

/* s.89: writeBG (ppu.s) marks each nametable word it stores to, through
 * vt369_nt_mark (mapVT.s) in its spare slot writeBG_mapper_9_mod, so the
 * diff below visits only those words.  Comparing both 2K nametables every
 * vblank cost Fire Fighter ~12K cycles a NES frame.  A write that bypasses
 * writeBG is still found by the scrub: one eighth of the map a frame. */
EWRAM_BSS u8  vt369_nt_dw[0x200] __attribute__((aligned(4)));
EWRAM_BSS u32 vt369_nt_slot_old;
EWRAM_BSS u8  vt369_nt_scrub;
EWRAM_BSS u8  vt369_nt_any;                /* vt369_nt_mark set a mark */
extern u32 writeBG_mapper_9_mod[];
extern void vt369_nt_mark(void);

static u32 vt369_nt_branch(void)
{
    const u32 at = (u32)writeBG_mapper_9_mod;
    return 0xEA000000u | ((((u32)vt369_nt_mark - (at + 8)) >> 2) & 0xFFFFFFu);
}

/* Installs (on != 0) or removes the writeBG hook.  Re-installed from the
 * vblank if the mapper-9 set-up in ppu.s has put its nop back. */
static void vt369_nt_hook(int on)
{
    volatile u32 *slot = (volatile u32 *)writeBG_mapper_9_mod;
    const u32 b = vt369_nt_branch();
    if (on) {
        if (*slot == b) return;
        vt369_nt_slot_old = *slot;
        for (u32 i = 0; i < 0x80; i++) ((u32 *)vt369_nt_dw)[i] = 0x01010101u;
        vt369_nt_any = 1;
        *slot = b;
    } else if (*slot == b) {
        *slot = vt369_nt_slot_old;
    }
}

__attribute__((noinline))
static void vt369_nt_diff(const u8 *nt)
{
    const u32 *cur = (const u32 *)nt;
    u32 *sh = (u32 *)vt369_nt_shadow;
    u32 *dw = (u32 *)vt369_nt_dw;
    vt369_nt_hook(1);
    const u32 s0 = (vt369_nt_scrub++ & 7) * 0x40;     /* the scrub's 64 words */
    for (u32 w = s0; w < s0 + 0x40; w += 4) {
        if (!((cur[w] ^ sh[w]) | (cur[w + 1] ^ sh[w + 1]) | (cur[w + 2] ^ sh[w + 2]) | (cur[w + 3] ^ sh[w + 3])))
            continue;
        vt369_nt_any = 1;
        dw[w / 4] = 0x01010101u;
    }
    if (!vt369_nt_any) return;
    vt369_nt_any = 0;
    for (u32 k = 0; k < 0x80; k++) {
        u32 m = dw[k];
        if (!m) continue;
        dw[k] = 0;
        for (u32 w = k * 4; m; w++, m >>= 8) {
            if (!(m & 0xFF)) continue;
            const u32 v = cur[w], was = sh[w];
            if (v == was) continue;
            sh[w] = v;
            vt369_nt_word(nt, w, was, v);
        }
    }
}

__attribute__((noinline))
static void vt369_bg_update(void)
{
    const u32 cfg = (u32)vt369_bpp8() << 31 | (vt369_reg[0x1C] & 8) << 27 | vt369_bg_base();
    if (cfg != vt369_cfg) { vt369_cfg = cfg; vt369_cache_reset(); vt369_dbg[5]++; }
    const u8 *nt = (const u8 *)vt_bk_consts[1];
    u32 *sh = (u32 *)vt369_nt_shadow;
    const u32 *cur = (const u32 *)nt;

    if (vt369_full || (vt369_retry && (!vt369_nofree || vt369_free_gen != vt369_full_gen
                                       || vt369_nslot_now() != vt369_full_n))) {
        vt369_full = vt369_retry = 0;
        vt369_dbg[0]++;
        for (u32 i = 0; i < 0x200; i++) sh[i] = cur[i];
        for (u32 scr = 0; scr < 0x800; scr += 0x400)
            for (u32 t = 0; t < 0x3C0; t++)
                vt369_cell_set(scr + t, vt369_cell_tile(nt, scr + t));
    } else {
        vt369_nt_diff(nt);
    }

    /* Other writers of low BG VRAM (the inherited 2bpp tile cache) could
     * overwrite a slot: check a few slots a frame against ROM, re-copy. */
    const u32 n = vt369_nslot_now(), tsz = vt369_bpp8() ? 64u : 32u;
    for (u32 k = 0; k < 16 && n; k++) {
        u32 s = vt369_verify_cur++;
        if (s >= n) { s = 0; vt369_verify_cur = 1; }
        const u32 tile = vt369_s2t[s];
        if (tile == 0xFFFF) continue;
        const u32 *src = (const u32 *)(vt369_prg() + ((vt369_bg_base() + tile * tsz) & vt369_prg_mask()));
        const volatile u32 *dst = (const volatile u32 *)(0x06000000u + vt369_slot_idx(s) * tsz);
        if (dst[0] != src[0] || dst[tsz / 4 - 1] != src[tsz / 4 - 1]) {
            vt369_copy_tile(s, tile);
            vt369_dbg[3]++;
        }
    }
}

EWRAM_BSS u8 vt369_pal_nbg, vt369_pal_nobj, vt369_pal_age;   /* s.89 */

__attribute__((noinline))
static void vt369_pal_upload(void)
{
    /* s.89: changed colours go straight to palette RAM; a full copy only when
     * the BG or OBJ depth changed, and every 64 frames in case something
     * else wrote it (run_palette no longer does while vt_pal_owned, s.87).
     * The full copy every frame cost Fire Fighter and Jewel Master ~14K
     * cycles a NES frame. */
    volatile u32 *bg = (volatile u32 *)0x05000000;
    volatile u32 *obj = (volatile u32 *)0x05000200;
    const u32 nbg = vt369_bpp8() ? 128 : 8;          /* words: 256 or 16 colours */
    const u32 nobj = vt369_spr8() ? 128 : 64;        /* s.88: 8bpp sprites use all 256 */
    const int full = nbg != vt369_pal_nbg || nobj != vt369_pal_nobj || ++vt369_pal_age >= 64;
    if (vt369_pal_dirty) {                 /* the carts re-DMA it every frame */
        vt369_pal_dirty = 0;
        const u32 *p = (const u32 *)vt369_pal;
        u32 *g = (u32 *)vt369_gba_pal;
        u32 *dw = (u32 *)vt369_pal_dw;
        for (u32 k = 0; k < 64; k++) {
            u32 m = dw[k];
            if (!m) continue;
            dw[k] = 0;
            for (u32 i = k * 4; m; i++, m >>= 8) {
                if (!(m & 0xFF)) continue;
                const u32 v = p[i];
                const u32 c = vt369_col(v & 0xFF, v >> 8 & 0xFF) | (u32)vt369_col(v >> 16 & 0xFF, v >> 24) << 16;
                g[i] = c;
                if (i < 128) { if (i < nbg) bg[i] = c; }
                else if (i - 128 < nobj) obj[i - 128] = c;
            }
        }
    }
    if (full) {
        vt369_pal_nbg = (u8)nbg; vt369_pal_nobj = (u8)nobj; vt369_pal_age = 0;
        const u32 *src = (const u32 *)vt369_gba_pal;
        for (u32 i = 0; i < nbg; i++) bg[i] = src[i];
        for (u32 i = 0; i < nobj; i++) obj[i] = src[128 + i];
    }
}

/* ---- sprites ----------------------------------------------------------- */
/* OAM, new arrangement ($201E bit 2): Y at n, tile low byte at 0x80+n,
 * attribute at 0x100+n, X at 0x180+n (ProcessSpritesEnhanced).  The carts
 * seen so far use 16x16 4bpp sprites ($201D = $0F): 8 bytes a row, low
 * nibble left, like GBA 4bpp rows, so a GBA 16x16 sprite is four 8x8 tiles
 * made of the halves of each row.  A converted tile lives in one of 128
 * 128-byte slots at OBJ VRAM 0x06010000 (GBA tiles 0-511), cached by tile
 * number; a slot used this frame is never evicted. */
#define VT369_OSLOT 128
#ifndef VT369_SPR_DY
#define VT369_SPR_DY 0      /* affine sprite Y correction, guide s.82 */
#endif
EWRAM_BSS u16 vt369_ot2s[2048];
EWRAM_BSS u16 vt369_os2t[VT369_OSLOT];
EWRAM_BSS u8  vt369_orc[VT369_OSLOT];     /* s.89: sprites using each slot */
EWRAM_BSS u8  vt369_orebuild;             /* s.89: every sprite rebuilds this pass */
EWRAM_BSS u16 vt369_ocur;
EWRAM_BSS u32 vt369_ocfg;
EWRAM_BSS u32 vt369_oparam;
EWRAM_BSS u8  vt369_ohi_clean;      /* s.88: OAM 64-127 written hidden */
EWRAM_BSS u32 vt369_oshadow[128];    /* s.88: OAM words at the last pass 1 */

static void vt369_obj_reset(void)
{
    vt369_oparam = 0xFFFFFFFFu;             /* every sprite rebuilds */
    for (int i = 0; i < 2048; i++) vt369_ot2s[i] = 0xFFFF;
    for (int i = 0; i < VT369_OSLOT; i++) { vt369_os2t[i] = 0xFFFF; vt369_orc[i] = 0; }
    vt369_ocur = 0;
}

/* GBA y (8-bit, wraps) of NES line index k as update_sprites computes it:
 * YSCALE_LOOKUP[k] -- k + 1 unscaled; 0.75k + (LOOKUP[0]) scaled. */
static u32 vt369_yscale(int k, int scaled)
{
    if (!scaled) return (u32)(k + 1) & 0xFF;
    return (u32)(((s32)(s8)YSCALE_LOOKUP[0] << 24) + k * 0x00C00000) >> 24 & 0xFF;
}

EWRAM_BSS u32 vt369_okey[128];        /* OAM bytes y|tile|attr|x the entry was built from */
EWRAM_BSS u16 vt369_oslot[128];       /* its slot, 0xFFFF hidden, 0xFFFE rebuild */
EWRAM_BSS u16 vt369_oent[128 * 4] __attribute__((aligned(4)));   /* its GBA attr0-2 (s.89: 4 a sprite, for word copies) */
EWRAM_BSS u8  vt369_oblank[VT369_OSLOT];  /* s.89: slot holds no opaque pixel */

/* s.89: sprite n lets go of its slot */
static inline void vt369_orel(u32 n)
{
    const u32 s = vt369_oslot[n];
    if (s < VT369_OSLOT && vt369_orc[s]) vt369_orc[s]--;
    vt369_oslot[n] = 0xFFFF;
}


__attribute__((noinline))
static void vt369_sprite_build(int n, u32 key, u32 r1d, u32 w, u32 h, u32 base, int affine, int scaled)
{
    u16 *ent = &vt369_oent[n * 4];
    vt369_okey[n] = key;
    vt369_orel(n);
    ent[0] = 0x0200;                                   /* hidden */
    int y = key & 0xFF, x = key >> 24;
    const u32 at = key >> 16 & 0xFF;
    u32 tile = (key >> 8 & 0xFF) | (at << 6 & 0x700);
    u32 pal, flip = 0, behind = 0;
    if (r1d & 8) {                                     /* negative X/Y, no flips */
        if (at & 0x40) x -= 256;
        if (at & 0x80) y -= 256;
        pal = (at & 3) | (at >> 3 & 4);
    } else {
        flip = at >> 6 & 3;                            /* bit0 = h, bit1 = v */
        behind = (at & 0x20) != 0;
        pal = at & 3;
    }
    if (vt369_reg[0x1C] & 0x80) tile &= 0xFF;
    tile = vt369_tile_fix(tile);
    /* s.86: 16 rows through $2000 bit 5 (NES 8x16) use tiles t & ~1 and
     * t | 1 (64-byte stride); through $201D bit 2, one 128-byte tile. */
    const int s8 = vt369_spr8();                       /* s.88: a byte a pixel */
    const u32 tstride = (s8 ? 8u : w / 2) * 8 * ((r1d & 4) ? 2u : 1u);
    if (h == 16 && !(r1d & 4)) tile &= ~1u;
    if (y >= 240 || y <= -(int)h || x >= 256 || x <= -(int)w) return;

    u32 s = vt369_ot2s[tile];
    if (s == 0xFFFF) {
        u32 k = 0, c = vt369_ocur;
        for (; k < VT369_OSLOT; k++, c = (c + 1) & (VT369_OSLOT - 1))
            if (!vt369_orc[c]) break;           /* s.89: no sprite uses it */
        if (k == VT369_OSLOT) return;
        s = c; vt369_ocur = (u16)((c + 1) & (VT369_OSLOT - 1));
        if (vt369_os2t[s] != 0xFFFF) vt369_ot2s[vt369_os2t[s]] = 0xFFFF;
        vt369_os2t[s] = (u16)tile; vt369_ot2s[tile] = (u16)s;
        const u32 *src = (const u32 *)(vt369_prg() + ((base + tile * tstride) & vt369_prg_mask()));
        volatile u32 *dst = (volatile u32 *)(0x06010000u + s * 128u);
        u32 any = 0;
        if (s8) {                                      /* 8 wide: GBA 8bpp rows as they are */
            for (u32 i = 0; i < h * 2; i++) { dst[i] = src[i]; any |= src[i]; }
        } else {
            /* row r, 8-pixel half q -> GBA tile (r/8)*(w/8) + q, row r%8 */
            const u32 rowb = w / 2;
            for (u32 r = 0; r < h; r++)
                for (u32 q = 0; q < w / 8; q++) {
                    const u32 v = src[r * (rowb / 4) + q];
                    dst[((r >> 3) * (w / 8) + q) * 8 + (r & 7)] = v;
                    any |= v;
                }
        }
        vt369_oblank[s] = !any;
        vt369_dbg[6]++;
    }
    vt369_orc[s]++;
    vt369_oslot[n] = (u16)s;
    /* s.89: a sprite with no opaque pixel stays hidden.  Fire Fighter parks
     * 70 of its 128 sprites at (0,0) with a blank tile: invisible, but each
     * took its share of the GBA's per-line OBJ time, and the real sprites on
     * those lines (its HUD digits, the window fires) were dropped. */
    if (vt369_oblank[s]) return;

    const u32 shape = (w == h) ? 0 : (w > h) ? 0x4000 : 0x8000;   /* square / wide / tall */
    const u32 size = (w == 16 && h == 16) ? 0x4000 : 0;  /* 16x16; 16x8 and 8x8 are size 0 */
    u32 a0, a1;
    const int yk = y - (int)(u8)wtop;
    if (affine) {
        /* same centre as update_sprites' 8x8 box at lookup[Y].  s.89: a
         * normal-size box, not double: the matrix only shrinks (0.75
         * vertically), and a double box cost the GBA 2*2w+10 OBJ cycles a
         * line instead of 2w+10, so ~16 16-wide sprites filled a line */
        a0 = vt369_yscale(yk + ((int)h - 8) / 2, 1) - (h - 8) + h / 2 + VT369_SPR_DY;
        a1 = (u32)(x - 8) & 0x1FF;
        a0 = (a0 & 0xFF) | 0x100 | shape;
        a1 |= size | flip << 12;                       /* matrices 0/8/16/24 */
    } else {
        a0 = (vt369_yscale(yk, scaled) & 0xFF) | shape;
        a1 = ((u32)(x - 8) & 0x1FF) | size | flip << 12;
    }
    ent[0] = (u16)(a0 | (s8 ? 0x2000u : 0));          /* 256-colour */
    ent[1] = (u16)a1;
    ent[2] = (u16)(s * 4 | (behind ? 3u : 2u) << 10 | pal << 12);
}

__attribute__((noinline))
static void vt369_sprites(void)
{
    const u32 r1d = vt369_reg[0x1D];
    const int wide = (r1d & 2) != 0;
    /* s.86: NES 8x16 mode ($2000 bit 5) makes enhanced sprites 16 rows tall
     * too (Furbtendulator ProcessSpritesEnhanced: spriteHeight); Zuma's balls
     * showed their top halves only. */
    const int tall = (r1d & 4) != 0 || (_ppuctrl0 & 0x20) != 0;
    /* 4bpp 16x16, 16x8, 8x16 and 8x8, and (s.88) 8bpp 8x8 and 8x16.  The
     * NES OAM arrangement ($201E bit 2 clear) is read once a fast DMA has
     * filled vt369_oam; before that PocketNES's own sprite path has them. */
    const int planar = (vt369_reg[0x1E] & 4) != 0;
    const int ok = vt369_obj_ok() && (planar || vt369_oam_live);
    const u32 w = wide ? 16 : 8, h = tall ? 16 : 8;
    const u32 base = (u32)(vt369_reg[0x22] | vt369_reg[0x23] << 8) << 13;
    const u32 cfg = (u32)r1d << 24 | (vt369_reg[0x1E] & 1) << 23 | (u32)tall << 22 | (base >> 13)
                  ^ ((u32)vt369_spr8() << 21);
    if (cfg != vt369_ocfg) { vt369_ocfg = cfg; vt369_obj_reset(); }
    const int stype = (emuflags >> 8) & 0xFF;
    const int affine = stype == 3, scaled = stype >= 2;
    const int nspr = (r1d & 1) ? 128 : 64;
    const u32 param = cfg ^ (u32)stype << 8 ^ (u32)(u8)wtop << 12 ^ (u32)ok << 20
                    ^ (u32)(vt369_reg[0x1C] & 0x80) << 13 ^ (u32)planar << 31;
    if (param != vt369_oparam) {
        vt369_oparam = param;
        for (int n = 0; n < 128; n++) vt369_oslot[n] = 0xFFFE;
        for (int i = 0; i < VT369_OSLOT; i++) vt369_orc[i] = 0;
        vt369_ohi_clean = 0;
        vt369_orebuild = 1;
    }
    const int rb = vt369_orebuild;
    vt369_orebuild = 0;
    u32 todo[4] = { 0, 0, 0, 0 };

    /* pass 1: find the changed sprites.  s.89: only the nspr sprites in
     * use (64-127 are hidden once below); slots are reference-counted, so
     * an unchanged sprite costs nothing, and a group of four whose OAM
     * words did not change is skipped whole.  Per-frame stamps on every
     * sprite cost Fire Fighter ~25K cycles a frame. */
    const u32 *ow = (const u32 *)vt369_oam;
    for (int n = 0; n < nspr; n += 4) {
        /* s.88: four sprites a step; a group whose OAM words did not change
         * (vt369_oshadow) only refreshes its slots' stamps */
        const u32 g = (u32)n >> 2;
        const u32 i0 = planar ? g : (u32)n, st = planar ? 32 : 1;
        const u32 w0 = ow[i0], w1 = ow[i0 + st], w2 = ow[i0 + 2 * st], w3 = ow[i0 + 3 * st];
        u32 *sh = &vt369_oshadow[i0];
        if (!((sh[0] ^ w0) | (sh[st] ^ w1) | (sh[2 * st] ^ w2) | (sh[3 * st] ^ w3)) && !rb) continue;
        sh[0] = w0; sh[st] = w1; sh[2 * st] = w2; sh[3 * st] = w3;
        for (int j = 0; j < 4; j++) {
            const u32 key = planar ? ((w0 >> (8 * j) & 0xFF) | (w1 >> (8 * j) & 0xFF) << 8
                                      | (w2 >> (8 * j) & 0xFF) << 16 | (w3 >> (8 * j)) << 24)
                                   : ow[n + j];         /* y, tile, attr, x */
            if (rb || key != vt369_okey[n + j]) {
                todo[(n + j) >> 5] |= 1u << ((n + j) & 31);
                vt369_okey[n + j] = key;
            }
        }
    }
    /* pass 2: changed sprites */
    for (int k = 0; k < 4; k++) {
        u32 bits = todo[k];
        for (int n = k * 32; bits; bits >>= 1, n++) {
            if (!(bits & 1)) continue;
            if (!ok) {
                vt369_orel(n);
                vt369_oent[n * 4] = 0x0200;
                continue;
            }
            vt369_sprite_build(n, vt369_okey[n], r1d, w, h, base, affine, scaled);
        }
    }
    /* pass 3: all 128 entries (update_sprites rewrote 0-63 this vblank);
     * attr3 holds PocketNES's affine matrices and is left alone */
    volatile u16 *oam = (volatile u16 *)0x07000000;
    const u16 *e = vt369_oent;
    /* s.88: PocketNES rewrites 0-63 every vblank; 64-127 only change with
     * the sprite count or a rebuild (vt369_ohi_clean) */
    const int nw = (nspr == 128 || !vt369_ohi_clean) ? 128 : 64;
    if (nspr == 64 && !vt369_ohi_clean) {
        for (int n = 64; n < 128; n++) {
            vt369_orel(n); vt369_oent[n * 4] = 0x0200; vt369_okey[n] = 0;
        }
        vt369_ohi_clean = 1;
    } else if (nspr == 128) vt369_ohi_clean = 0;
    for (int n = 0; n < nw; n++, oam += 4, e += 4) {
        *(volatile u32 *)oam = *(const u32 *)e; oam[2] = e[2];
    }
}

/* ---- entry points ------------------------------------------------------ */

/* s.92: update_sprites (ppu.s) writes PocketNES's NES sprites to OAM 0-63
 * early in the vblank, and vt369_sprites rewrites all of them at its end.  On
 * a cart at the slow timing (s.91) that end can come after line 0, and the top
 * of the picture then showed PocketNES's entries: Zuma's top balls vanished
 * for a frame, every eighth frame or so.  While enhanced mode is on, the
 * `mov r2,#AGB_OAM` there becomes `mov r2,#0x10000000` (unmapped: the GBA
 * ignores the stores), so OAM holds the last frame's VT369 sprites until
 * vt369_sprites replaces them. */
extern u32 vt_oam_dest_mod[];
static void vt369_oam_redirect(int on)
{
    const u32 to_oam = 0xE3A02407u, to_void = 0xE3A02201u;   /* mov r2,#0x07000000 / #0x10000000 */
    volatile u32 *ins = (volatile u32 *)vt_oam_dest_mod;
    if (on && *ins == to_oam) *ins = to_void;
    else if (!on && *ins == to_void) *ins = to_oam;
}

static void vt369_set_mode(int on)
{
    if (on == vt369_enh) return;
    vt369_enh = (u8)on;
    vt369_oam_redirect(on);
    if (on) {
        vt369_saved_palW = _vram_write_tbl[15];
        _vram_write_tbl[15] = (u32)vt369_pal_W;
        vt_bkexten_live = 1;               /* PocketNES BG map writers off */
        vt369_cfg = 0xFFFFFFFFu;           /* cache reset on the next vblank */
        vt369_ocfg = 0xFFFFFFFFu;
        vt369_oparam = 0xFFFFFFFFu;
        vt369_pal_dirty = 1;
        for (int i = 0; i < 64; i++) ((u32 *)vt369_pal_dw)[i] = 0x01010101u;
        vt369_pal_nbg = 0;                 /* s.89: full palette copy next vblank */
    } else {
        if (vt369_saved_palW) _vram_write_tbl[15] = vt369_saved_palW;
        vt369_nt_hook(0);                  /* s.89: writeBG logs to the ring again */
        vt_bkexten_live = (vt_reg_2010 & 0x12) == 0x12;
        _bg_cache_full = 1;                /* legacy whole-map redraw */
    }
}

void vt369_reset(void)
{
    if (vt369_enh) vt369_set_mode(0);
    for (int i = 0; i < 0x40; i++) vt369_reg[i] = 0;
    vt369_oam_hi = 0;
    vt369_oam_live = 0;
    vt369_cfg = 0xFFFFFFFFu;
    { extern u32 vt369_s0_key[3]; vt369_s0_key[2] = 0xFFFFFFFFu; }   /* s.88: no cached sprite-0 answer */
}

/* $2000-$203F writes (vt_ppu_reg_write, VT369 carts only). */
void vt369_reg_write(u32 offset, u32 val)
{
    vt369_reg[offset] = (u8)val;
    if (offset == 0x08) vt369_oam_hi = val & 1;
    else if (offset == 0x1E) vt369_set_mode(val != 0);
}

/* $4014 in enhanced mode ($411C bit 7 "fast DMA"): 256 bytes from page
 * (val << 8) to $2004 or $2007 by $4034 bit 0.  Returns 0 when the caller
 * should run its own $2007 path instead (a target below $3C00). */
/* One source byte over the CPU bus: RAM, or PRG/$6000 through the memmap
 * the 6502 core fetches with (mapVT.s's video DMA reads $8000+ the same
 * way).  $2000-$5FFF (registers) never occurs in practice: 0. */
static inline u32 vt369_dma_byte(u32 a)
{
    a &= 0xFFFF;
    if (a < 0x2000) return ((const u8 *)NES_RAM)[a & vt_nes_ram_mask];
    if (a < 0x6000) return 0;
    return _memmap_tbl[a >> 13][a];
}

/* s.89: a video DMA into the nametables ($2000-$3BFF), for quadrants mapped
 * to NES_VRAM2 (vram_write_tbl entries VRAM_name0/1).  It stores straight
 * into the nametable and marks the words that changed for vt369_nt_diff:
 * through $4014 -> vmdata_W -> writeBG -> vt369_nt_mark each byte cost ~100
 * cycles, and Sky Fighter's title DMAs ~1.5K nametable bytes a frame (46
 * fps).  Returns 0, having done nothing, for anything else. */
extern void VRAM_name0(void), VRAM_name1(void);
static int vt369_dma_nt(u32 src, u32 len)
{
    const u32 step = (_ppuctrl0 & 4) ? 32u : 1u;
    const u32 a0 = _vramaddr & 0x3FFF, a1 = a0 + (len - 1) * step;
    if (a0 < 0x2000 || a1 >= 0x3C00) return 0;
    if ((src & 0xFFFF) + len > 0x10000 || (src < 0x8000 && src + len > 0x2000)) return 0;
    u16 base[15];
    for (u32 q = a0 >> 10; q <= a1 >> 10; q++) {
        const u32 h = _vram_write_tbl[q];
        if (h == (u32)VRAM_name0) base[q] = 0;
        else if (h == (u32)VRAM_name1) base[q] = 0x400;
        else return 0;
    }
    u8 *nt = (u8 *)vt_bk_consts[1];
    const u8 *s8 = src < 0x2000 ? (const u8 *)NES_RAM : 0;
    const u32 rmask = vt_nes_ram_mask;
    u32 a = a0, any = 0;
    for (u32 i = 0; i < len; i++, a += step) {
        const u32 v = s8 ? s8[(src + i) & rmask] : _memmap_tbl[(src + i) >> 13][src + i];
        const u32 off = base[a >> 10] | (a & 0x3FF);
        if (nt[off] == v) continue;
        nt[off] = (u8)v;
        vt369_nt_dw[off >> 2] = 1;
        any = 1;
    }
    if (any) vt369_nt_any = 1;
    _vramaddr = (_vramaddr & 0xFFFF0000u) | (u16)(_vramaddr + len * step);   /* as vmdata_W's strh */
    return 1;
}

int vt369_dma_4014(u32 val)
{
    extern u8 vt_dma_lo;
    const u32 len = 1u << (((vt_dma_settings >> 1) & 7) ? ((vt_dma_settings >> 1) & 7) : 8);
    /* s.86: the source is ($4014 << 8) | low byte ($4024 in enhanced mode),
     * and may be PRG: Zuma DMAs its palette from $8080.  The first version
     * took page ($4014 << 8) and handed PRG sources back to mapVT.s. */
    const u32 src = val << 8 | vt_dma_lo;
    const u8 *ram = (const u8 *)NES_RAM;
    const u32 rmask = vt_nes_ram_mask;
    const int inram = src + len <= 0x2000;
    if (vt_dma_settings & 1) {
        u32 a = _vramaddr & 0x3FFF;
        if ((a & 0x3C00) != 0x3C00) return vt369_dma_nt(src, len);
        const u32 step = (_ppuctrl0 & 4) ? 32u : 1u;
        if (inram && step == 1 && len >= 16 && !((a | src | len) & 3) && (a & 0x3FF) + len <= 0x400 && src + len <= rmask + 1) {
            u32 *d = (u32 *)&vt369_pal[a & 0x3FF];
            const u32 *sw = (const u32 *)&ram[src];
            u8 *dw = &vt369_pal_dw[(a & 0x3FF) >> 2];
            for (u32 i = 0; i < len / 4; i += 4) {   /* len >= 16 here */
                const u32 v0 = sw[i], v1 = sw[i + 1], v2 = sw[i + 2], v3 = sw[i + 3];
                if (!((d[i] ^ v0) | (d[i + 1] ^ v1) | (d[i + 2] ^ v2) | (d[i + 3] ^ v3))) continue;
                for (u32 k = i; k < i + 4; k++)
                    if (d[k] != sw[k]) { d[k] = sw[k]; dw[k] = 1; }
            }
            a += len;
        } else {
            for (u32 i = 0; i < len; i++, a += step) {
                vt369_pal[a & 0x3FF] = (u8)vt369_dma_byte(src + i);
                vt369_pal_dw[(a & 0x3FF) >> 2] = 1;
            }
        }
        _vramaddr = (_vramaddr & ~0x3FFFu) | (a & 0x3FFF);
        vt369_pal_dirty = 1;
    } else {
        /* s.88: with 64 sprites ($201D bit 0 clear) the high half stays 0
         * (OneBus_VT369.cpp $2004: SprAddrHigh = 0).  Toggling it per
         * 256-byte transfer put every other frame's sprites where nothing
         * reads them. */
        if (!(vt369_reg[0x1D] & 1)) vt369_oam_hi = 0;
        u32 a = (u32)vt369_oam_hi << 8;
        vt369_oam_live = 1;
        if (inram && !((src | len) & 3) && a + len <= 0x200 && src + len <= rmask + 1) {
            u32 *d = (u32 *)&vt369_oam[a];
            const u32 *sw = (const u32 *)&ram[src];
            for (u32 i = 0; i < len / 4; i++) d[i] = sw[i];
        } else {
            for (u32 i = 0; i < len; i++) vt369_oam[(a + i) & 0x1FF] = (u8)vt369_dma_byte(src + i);
        }
        if (len >= 256) vt369_oam_hi ^= 1;
        /* PocketNES's sprite path must see no sprites (Y = $FF): it would
         * otherwise cache tiles over ours and fill OAM 0-63 each vblank. */
        u32 *nes = (u32 *)_nesoambuff;
        for (int i = 0; i < 64; i++) nes[i] = 0xFFFFFFFFu;
    }
    return 1;
}

/* s.88: sprite 0 hit on VT369 in enhanced mode, as OneBus_VT369.cpp does it
 * (ProcessSpritesEnhanced, then the pixel loop): the first opaque pixel of
 * sprite 0, whatever the background has there.  PocketNES's test reads the
 * sprite and BG patterns from NES_VRAM, which enhanced mode does not fill,
 * and the fast $4014 DMA leaves its OAM copy at Y = $FF: Jumper (an SMB
 * hack) waited for the hit forever on a black screen.  Called from
 * screen_on_sprite0 (ppu.s) with $2001; returns (line << 8 | x), line being
 * the sprite's Y plus the row, or -1 for no hit this frame. */
EWRAM_BSS u32 vt369_s0_key[3];
EWRAM_BSS int vt369_s0_res;

int vt369_spr0_hit(u32 ctrl1)
{
    if (!vt369_enh || !(ctrl1 & 0x10)) return -1;
    const u32 r1d = vt369_reg[0x1D], r1c = vt369_reg[0x1C], r1e = vt369_reg[0x1E];
    const u8 *nes = (const u8 *)_nesoambuff;
    int y, x;
    u32 tile, at;
    if (*(const u32 *)nes != 0xFFFFFFFFu) {            /* normal $4014 DMA */
        y = nes[0]; tile = nes[1] | (nes[2] << 6 & 0x700); at = nes[2]; x = nes[3];
    } else if (!(r1e & 4)) {                           /* NES OAM layout */
        y = vt369_oam[0]; tile = vt369_oam[1] | (vt369_oam[2] << 6 & 0x700);
        at = vt369_oam[2]; x = vt369_oam[3];
    } else {
        y = vt369_oam[0]; tile = vt369_oam[0x80] | (vt369_oam[0x100] << 6 & 0x700);
        at = vt369_oam[0x100]; x = vt369_oam[0x180];
    }
    u32 fx = 0, fy = 0;
    if (r1d & 8) { if (at & 0x40) x -= 256; if (at & 0x80) y -= 256; }
    else { fx = at & 0x40; fy = at & 0x80; }
    if (y >= 240 || y <= -16 || x > 255 || x <= -16) return -1;    /* off screen */
    if (r1c & 0x80) tile &= 0xFF;
    tile = vt369_tile_fix(tile);
    const u32 right = (_ppuctrl0 & 0x20) ? (tile & 1) : (_ppuctrl0 & 0x08);
    if (_ppuctrl0 & 0x20) tile &= ~1u;
    const int h = ((r1d & 4) || (_ppuctrl0 & 0x20)) ? 16 : 8;
    const int w = (r1d & 2) ? 16 : 8;
    const int nib = (r1d & 2) || !((r1d & 4) || (r1c & 0x20));   /* 4bpp rows */
    u32 base;
    if (r1e & 1) base = ((u32)(vt369_reg[0x22] | vt369_reg[0x23] << 8) & 0xFFF) << 13;
    else base = (u32)vt_chr_reg[right ? 0 : 4] << ((r1c & 0x80) ? 10 : 13) << ((r1c >> 4) & 3);
    const u8 *prg = vt369_prg();
    const u32 m = vt369_prg_mask();
    /* the same sprite 0 as last time: the same answer (it is scanned from
     * ROM, pixel by pixel, ~20K cycles when mostly transparent) */
    const u32 k0 = ((u32)y & 0x1FF) | ((u32)x & 0x1FF) << 9 | tile << 18 | (ctrl1 & 0x14) << 25;
    const u32 k1 = at | r1c << 8 | r1d << 16 | (u32)(_ppuctrl0 & 0x28) << 24;
    if (vt369_s0_key[0] == k0 && vt369_s0_key[1] == k1 && vt369_s0_key[2] == base)
        return vt369_s0_res;
    vt369_s0_key[0] = k0; vt369_s0_key[1] = k1; vt369_s0_key[2] = base;
    vt369_s0_res = -1;
    const u32 rb = nib ? (u32)w >> 1 : (u32)w;          /* bytes a row */
    for (int sl = 0; sl < h; sl++) {
        if (y + sl < 0 || y + sl >= 240) continue;
        u32 a = tile << 6;
        if (r1d & 4) a <<= 1;
        a += (u32)(fy ? h - 1 - sl : sl) << 3;
        if (!(r1d & 2) && !(r1d & 4) && !(r1c & 0x20)) a >>= 1;
        a += base;
        {   u32 any = 0;                                /* a transparent row: next */
            for (u32 j = 0; j < rb; j++) any |= prg[(a + j) & m];
            if (!any) continue; }
        for (int i = 0; i < w; i++) {
            const int c = fx ? w - 1 - i : i;          /* source pixel at screen i */
            const u32 b = prg[(a + (nib ? (u32)c >> 1 : (u32)c)) & m];
            const u32 px = nib ? ((c & 1) ? b >> 4 : b & 0x0F) : b;
            const int sx = x + i;
            if (!px || sx < 0 || sx > 255 || (sx < 8 && !(ctrl1 & 0x04))) continue;
            return vt369_s0_res = (y + sl) << 8 | sx;
        }
    }
    return -1;
}

/* NES line 242, before the effect buffers swap (vt_palette_rebuild_gba):
 * 8bpp or 4bpp and char base 0 on every line of BG0. */
void vt369_frame_end(void)
{
    u16 *b = (u16 *)_bg0cntbuff;
    const u16 set = vt369_bpp8() ? 0x0080 : 0;
    for (int i = 0; i < 240; i++) b[i] = (u16)((b[i] & ~0x008C) | set);
}

/* GBA vblank, last palette writer (vt_16c_palette_fixup, EWRAM stack). */
void vt369_vblank(void)
{
    volatile u16 *ime = (volatile u16 *)0x04000208;
    const u16 saved = *ime;
    *ime = 0;
    vt369_bg_update();
    vt369_pal_upload();
    vt369_sprites();
    *ime = saved;
}

#endif
