/*
 * um6578.c -- the UMC UM6578 famiclone chip (NES 2.0 mapper 405, console
 * type $0B), guide s.94.  Star Ally's UM6578 version runs on it.
 *
 * Next to an NES-like CPU and PPU the chip has 8K of CPU RAM ($0000-$1FFF,
 * and 2K more at $5000-$57FF), 4K PRG banks ($4040-$4047), a DMA engine
 * ($4048-$404F: ROM or CPU space to CPU or PPU space, all at once), a timer
 * IRQ counted on scanlines or CPU cycles ($4032-$4036), and its own PPU:
 * nametables of 16-bit words (a 12-bit tile number and a 4-bit palette for
 * every tile) in 10K of VRAM, tiles anywhere in a 64K PPU space (VRAM
 * $0000-$27FF, CHR-RAM $8000-$FFFF), a palette mapped into CPU space
 * ($2040-$207F) and $2008 = [C... PP.N]: 4bpp tiles, the sprite pattern
 * bank, and one nametable mirrored both ways at $2000-$27FF (N) or four at
 * $0000-$1FFF.  Reference: Furbtendulator UM6578.cpp and mapper405.cpp,
 * and the NESdev wiki's UM6576/UM6578 page.
 *
 * PocketNES keeps the CPU, the APU, $2000-$2004, the NMI, the sprites and
 * the scaling.  The UM6578's VRAM $0000-$1FFF is NES_VRAM, so the CHR-RAM
 * sprite path finds the game's sprites ($0000-$0FFF while PP = 0) in its
 * own pattern tables; every write there marks dirty_tiles as $2007 does.
 * This module owns the rest: the memory and $40xx registers mapUM.s routes
 * here, and the background -- a tile cache in char base 2 (0x06008000, 704
 * slots), the four nametables as one 64x64 map (screen blocks 27-30), the
 * single nametable as a 32x32 map (screen block 31), the scroll and map of
 * every line (scrollbuff / bg0cntbuff, written at NES line 242) and the GBA
 * palette.
 */
#include "includes.h"
#include "config.h"

#if VT_MODE

#define UM_NSLOT  704          /* BG tiles 0x06008000-0x0600D7FF; slot 0 stays blank */
#define UM_HSIZE  2048         /* key -> slot hash, open addressing */
#define UM_MAP    ((volatile u16 *)0x0600D800)   /* cell w: the four NTs 0-0xFFF, the single NT 0x1000-0x13FF */
#define UM_CNT4   0xDB0A       /* BG0CNT: 64x64 map at screen block 27, char base 2, priority 2 */
#define UM_CNT1   0x1F0A       /* BG0CNT: 32x32 map at screen block 31 */
#define UM_NSEG   48

/* The big tables live above the ROM cache (loadcart.c reserves um_mem_size
 * bytes for a UM6578 cart only); the SRAM menu code borrows the heap below. */
typedef struct {
    u8  ram[0x1800];           /* CPU $0800-$1FFF */
    u8  ram2[0x800];           /* CPU $5000-$57FF */
    u8  chr[0x8000];           /* PPU $8000-$FFFF */
    u16 hash[UM_HSIZE];        /* stale entries (slot since given another key) are skipped */
    u16 s2k[UM_NSLOT];         /* slot -> key, 0xFFFF free */
    u16 rc[UM_NSLOT];          /* map cells showing the slot */
    u8  sdirty[UM_NSLOT];      /* slot to convert again */
    u8  tcnt[0x1000];          /* slots holding each tile (writes to others mark nothing) */
    u8  tdirty[512];           /* tiles (PPU address >> 4) written since the last pass */
    u8  ntdirty[0x280];        /* nametable words (PPU address >> 1) written */
} um_mem_t;

const u32 um_mem_size = (sizeof(um_mem_t) + 3) & ~3u;

#define UMF_SCHED  1           /* mapUM.s: arm the timer timeout at um_tsched */
#define UMF_CANCEL 2           /* ... or take it out of the queue */
#define UMF_IRQ    4           /* the IRQ line is asserted */
#define UMF_PRG    8           /* map um_prg8 */

extern u8 _ppuctrl0;           /* ppu.s: $2000 shadow; _ppuctrl1 two bytes on */

/* read by mapUM.s */
EWRAM_BSS u8  um_active;
EWRAM_BSS u8 *um_ram_ptr;      /* um_mem->ram - 0x800, indexed by the CPU address */
EWRAM_BSS u8 *um_ram2_ptr;
EWRAM_BSS u8  um_pal[64];
EWRAM_BSS u8  um_pal_dirty;
EWRAM_BSS u8  um_2008;
EWRAM_BSS u8  um_w;            /* $2005/$2006 latch, cleared by $2002 reads */
EWRAM_BSS u16 um_prg8[4];      /* 8K banks at $8000/$A000/$C000/$E000 */
EWRAM_BSS u32 um_tsched;       /* timestamp of the next timer expiry */
EWRAM_BSS u32 um_dbg[8];       /* 0 unaligned PRG pairs 1 DMAs 2 tile conversions 3 alloc fails 4 cells 5 cache resets */

EWRAM_BSS um_mem_t *um_mem;
EWRAM_BSS u16 um_t, um_v;
EWRAM_BSS u8  um_x, um_buf;
EWRAM_BSS u8  um_prg[8], um_r4016;
EWRAM_BSS u8  um_irq_mask, um_irq_status, um_tctl, um_tlatch, um_tval, um_tpresc;
EWRAM_BSS u32 um_tlast;        /* time of the last timer count applied */
EWRAM_BSS u8  um_dma_ctl, um_dma_page;
EWRAM_BSS u16 um_dma_src, um_dma_dst, um_dma_len;
EWRAM_BSS u32 um_spread[256];  /* byte -> 8 nibbles, leftmost pixel in the low nibble */
EWRAM_BSS u8  um_mode4, um_full, um_nofree, um_tany, um_ntany, um_sany;
EWRAM_BSS u8  um_grey, um_pal_age;
EWRAM_BSS u16 um_alloc_cur, um_hcount, um_ncur, um_scur;

typedef struct { u8 line, cfg; u16 h, y; } um_seg_t;
EWRAM_BSS um_seg_t um_seg[UM_NSEG];    /* this frame's scroll segments */
EWRAM_BSS u8  um_nseg, um_fs;          /* um_fs: segment 0 is open */
/* Map rows the picture shows (s.94): bit 2r+h = row r (0-63) of the 64x64
 * map, left or right half (bits 0-127); bit 128+r = row r of the single NT.
 * um_frame_end computes um_vis_next from the frame's segments; um_vblank
 * applies it.  Only shown rows hold cache slots: Star Ally keeps its sprite
 * patterns at $0000-$0FFF, which the four-NT map reads as ~950 junk tiles. */
EWRAM_BSS u32 um_vis[5], um_vis_next[5];

void um_set_mem(void *p) { um_mem = (um_mem_t *)p; }

/* ---- time ------------------------------------------------------------- */
#define UM_FT() ((u32)TimeoutState[2])          /* frame_timestamp: pre-render line, dot 0 */
#define UM_ARM __attribute__((target("arm")))  /* umull: Thumb would call __muldi3 */

static inline UM_ARM u32 um_div341(u32 r) { return (u32)(((u64)r * 12595096u) >> 32); }   /* exact below 2^26 */

/* NES line (-1 pre-render, 0-239 picture, 240-260 vblank) and dot of `now`. */
static UM_ARM void um_line_of(u32 now, int *l, int *d)
{
    const s32 rel = (s32)(now - UM_FT());
    if (rel < 0) { *l = 260; *d = 0; return; }
    const u32 q = um_div341((u32)rel);
    *l = (int)q - 1;
    *d = (int)((u32)rel - q * 341u);
}

/* ---- timer ($4032-$4036) ------------------------------------------------
 * Counted at dot 280 of every line ($4034 bit 5) or every CPU cycle, as
 * Furbtendulator's mapper405.cpp clockTimer: the prescaler divides by
 * $4034 & 15; a count from 0 reloads the $4035 value, a count to 0 sets
 * $4033 bit 7 and, without bit 6 (repeat), stops the timer.  The count runs
 * lazily: every access first applies the counts since um_tlast. */
static UM_ARM s32 um_clk_idx(u32 t)
{
    if (!(um_tctl & 0x20)) return (s32)(u32)(((u64)t * 0xAAAAAAABu) >> 33);   /* t / 3 */
    const s32 r = (s32)(t - UM_FT()) - 280;
    if (r >= 0) return (s32)um_div341((u32)r);
    return -(s32)um_div341((u32)(-r - 1)) - 1;
}

static UM_ARM u32 um_clk_time(s32 i)
{
    if (!(um_tctl & 0x20)) return (u32)i * 3u;
    return UM_FT() + 280u + (u32)i * 341u;
}

/* n counts at once: the prescaler's share, then whole reload periods
 * (latch + 1 counts, each ending at 0) skipped -- only the first sets
 * anything new. */
static UM_ARM void um_tm_clock(u32 n)
{
    if (!n || !(um_tctl & 0x80)) return;
    const u32 p = um_tctl & 15;
    u32 e;
    if (p <= 1) { e = n; um_tpresc = 0; }
    else {
        const u32 need = (um_tpresc + 1u >= p) ? 1u : p - um_tpresc;
        if (n < need) { um_tpresc = (u8)(um_tpresc + n); return; }
        const u32 rest = n - need;
        e = 1u + rest / p;
        um_tpresc = (u8)(rest % p);
    }
    while (e) {
        if (!um_tval) {                      /* a count from 0 reloads */
            um_tval = um_tlatch;
            e--;
            if (!um_tlatch) return;          /* 0 for ever */
            continue;
        }
        const u32 k = e < um_tval ? e : um_tval;
        um_tval = (u8)(um_tval - k);
        e -= k;
        if (!um_tval) {
            um_irq_status |= 0x80;
            if (!(um_tctl & 0x40)) { um_tctl &= ~0x80; um_tpresc = 0; return; }
            e %= (u32)um_tlatch + 1u;
        }
    }
}

static UM_ARM void um_tm_sync(u32 now)
{
    if (now - um_tlast > 0x60000u) um_tlast = now - 0x60000u;   /* ~4 frames: keeps the maths in range */
    const s32 i1 = um_clk_idx(now), i0 = um_clk_idx(um_tlast);
    if (i1 > i0) {
        um_tm_clock((u32)(i1 - i0));
        um_tlast = um_clk_time(i1);
    }
}

/* After a sync at `now`: when the count next reaches zero. */
static UM_ARM u32 um_tm_plan(u32 now)
{
    if (!(um_tctl & 0x80)) return UMF_CANCEL;
    u32 eff;
    if (um_tval) eff = um_tval;
    else if (um_tlatch) eff = 1u + um_tlatch;
    else return UMF_CANCEL;                  /* reloads 0 for ever */
    const u32 p = um_tctl & 15, per = p <= 1 ? 1u : p;
    const u32 first = (p <= 1 || um_tpresc + 1u >= p) ? 1u : p - um_tpresc;
    um_tsched = um_clk_time(um_clk_idx(now) + (s32)(first + (eff - 1u) * per));
    return UMF_SCHED;
}

static inline u32 um_irq_line(void) { return (um_irq_status & ~um_irq_mask) ? UMF_IRQ : 0; }

/* mapUM.s's timeout handler, at the planned expiry. */
__attribute__((target("arm")))
u32 um_timer_fire(u32 now)
{
    um_tm_sync(now);
    const u32 f = um_tm_plan(now);
    return (f & UMF_SCHED) | um_irq_line();
}

/* ---- PRG ($4040-$4047) ----------------------------------------------------
 * 4K banks; $4016 bit 1 is bank bit 8 (submapper 0).  PocketNES maps 8K, so
 * a pair shows only when it is an aligned 8K bank -- what Star Ally writes,
 * $4040 then $4041.  Anything else maps the 8K bank holding the low half
 * and is counted in um_dbg[0]. */
static void um_prg_calc(void)
{
    const u32 hi = (u32)(um_r4016 & 2) << 7;
    for (u32 p = 0; p < 4; p++) {
        const u32 a = um_prg[2 * p] | hi, b = um_prg[2 * p + 1] | hi;
        if ((a & 1) || b != a + 1) um_dbg[0]++;
        um_prg8[p] = (u16)(a >> 1);
    }
}

/* ---- CPU and PPU space ------------------------------------------------- */
static u32 um_cpu_rd(u32 a)
{
    if (a < 0x2000) return a < 0x800 ? NES_RAM[a] : um_mem->ram[a - 0x800];
    if (a < 0x4000) {
        a &= 0xFFF;
        if (a >= 0x40 && a < 0x80) return um_pal[a & 63];
        return a == 8 ? um_2008 : 0;
    }
    if (a >= 0x5000 && a < 0x5800) return um_mem->ram2[a - 0x5000];
    if (a >= 0x6000 && a < 0x8000) return *(const u8 *)((u32)NES_RAM - 0x5800u + a);   /* PRG-RAM, as memmap_6 */
    return 0;
}

static inline void um_tmark(u32 t)
{
    if (!um_mem->tcnt[t]) return;      /* in no slot: converted fresh when needed */
    um_mem->tdirty[t >> 3] |= (u8)(1u << (t & 7));
    um_tany = 1;
}

/* Visibility bit of map cell w (nametable word, PPU address >> 1). */
static inline u32 um_cell_vi(u32 w)
{
    if (w >= 0x1000) return 128u + ((w >> 5) & 31);
    const u32 n = w >> 10;
    return ((((n >> 1) << 5) | ((w >> 5) & 31)) << 1) | (n & 1);
}
static inline u32 um_vis_on(u32 vi) { return um_vis[vi >> 5] >> (vi & 31) & 1; }

/* A PPU-space write: VRAM (nametables, and below $2000 PocketNES's CHR-RAM
 * too) or CHR-RAM; $2800-$7FFF is open bus. */
static void um_ppu_mark(u32 a);
static void um_ppu_wr(u32 a, u32 v)
{
    u8 *p;
    if (a < 0x2800) p = NES_VRAM + a;
    else if (a >= 0x8000) p = um_mem->chr + (a - 0x8000);
    else return;                             /* open bus */
    if (*p == v) return;
    *p = (u8)v;
    um_ppu_mark(a);
}

static u32 um_ppu_rd(u32 a)
{
    if (a < 0x2800) return NES_VRAM[a];
    if (a >= 0x8000) return um_mem->chr[a - 0x8000];
    return 0;
}

/* ---- DMA ($4048-$404F) ----------------------------------------------------
 * All at once, $404E/$404F + 1 bytes; source PRG ROM (bit 15, the 32K page
 * in $4049) or CPU space, target CPU space ($4048 bit 5) or PPU space.  The
 * registers keep their values (Furbtendulator's APU_UM6578::IntWrite).
 * Copied in runs that stay inside one memory: a byte at a time through
 * um_cpu_rd/um_ppu_wr cost Star Ally ~18K cycles a frame. */

/* Where CPU address a lives for a run of *run bytes; 0 if no memory. */
static u8 *um_cpu_ptr(u32 a, u32 *run)
{
    if (a < 0x800) { *run = 0x800 - a; return NES_RAM + a; }
    if (a < 0x2000) { *run = 0x2000 - a; return um_mem->ram + (a - 0x800); }
    if (a < 0x4000) {
        const u32 o = a & 0xFFF;
        if (o >= 0x40 && o < 0x80) { *run = 0x80 - o; return um_pal + (o & 63); }
        *run = 1;
        return 0;
    }
    if (a >= 0x5000 && a < 0x5800) { *run = 0x5800 - a; return um_mem->ram2 + (a - 0x5000); }
    if (a >= 0x6000 && a < 0x8000) { *run = 0x8000 - a; return (u8 *)NES_RAM + (a - 0x5800); }
    *run = 1;
    return 0;
}

/* PPU address a written with v != the old byte: the marks um_ppu_wr makes. */
static void um_ppu_mark(u32 a)
{
    um_tmark(a >> 4);
    if (a < 0x2800) {
        if (a < 0x2000) {                    /* as VRAM_chr: dirty_tiles, then dirty_rows */
            dirty_tiles[a >> 4] = 1;
            dirty_rows[a >> 8] = 1;
        }
        if (um_vis_on(um_cell_vi(a >> 1))) {
            um_mem->ntdirty[a >> 4] |= (u8)(1u << ((a >> 1) & 7));
            um_ntany = 1;
        }
    }
}

static void um_dma_run(void)
{
    u32 src = um_dma_src, dst = um_dma_dst, n = (u32)um_dma_len + 1u;
    const int to_cpu = um_dma_ctl & 0x20;
    um_dbg[1]++;
    while (n) {
        u32 k = n, run;
        const u8 *sp;
        if (src & 0x8000) {                  /* PRG ROM, 32K page $4049 */
            const u32 off = ((src & 0x7FFF) | (u32)um_dma_page << 15) & rommask;
            sp = rombase + off;
            run = 0x8000 - (src & 0x7FFF);
            if (run > rommask + 1 - off) run = rommask + 1 - off;
        } else {
            sp = um_cpu_ptr(src, &run);
        }
        if (k > run) k = run;
        u8 *dp;
        if (to_cpu) {
            dp = um_cpu_ptr(dst, &run);
        } else if (dst < 0x2800) {
            dp = NES_VRAM + dst; run = 0x2800 - dst;
        } else if (dst >= 0x8000) {
            dp = um_mem->chr + (dst - 0x8000); run = 0x10000 - dst;
        } else {
            dp = 0; run = 0x8000 - dst;      /* open bus */
        }
        if (k > run) k = run;
        if (dp) {
            if (to_cpu) {
                for (u32 i = 0; i < k; i++) {
                    const u32 v = sp ? sp[i] : um_cpu_rd(src + i);
                    if (dp[i] != v) {
                        dp[i] = (u8)v;
                        if (dp >= um_pal && dp < um_pal + 64) um_pal_dirty = 1;
                    }
                }
            } else {
                for (u32 i = 0; i < k; i++) {
                    const u32 v = sp ? sp[i] : um_cpu_rd(src + i);
                    if (dp[i] == v) continue;
                    dp[i] = (u8)v;
                    um_ppu_mark(dst + i);
                }
            }
        }
        src = (src + k) & 0xFFFF;
        dst = (dst + k) & 0xFFFF;
        n -= k;
    }
}

/* ---- $4016 and $4031-$404F ------------------------------------------- */
__attribute__((target("arm")))
u32 um_reg_W(u32 v, u32 off, u32 now)
{
    u32 f = 0;
    v &= 0xFF;
    switch (off) {
    case 0x16:                         /* after PocketNES's pads: PRG bit 8, DMA page bit 5 */
        if ((v ^ um_r4016) & 2) f |= UMF_PRG;
        um_r4016 = (u8)v;
        um_dma_page = (u8)((um_dma_page & 0x1F) | ((v << 4) & 0xE0));
        if (f) um_prg_calc();
        break;
    case 0x32:                         /* IRQ mask; a write also acknowledges */
        um_tm_sync(now);
        um_irq_mask = (u8)v;
        um_irq_status &= (u8)~v;
        break;
    case 0x34:                         /* E R S . PPPP */
        um_tm_sync(now);
        if ((v ^ um_tctl) & 0x20) um_tlast = now;    /* another count source from here on */
        um_tctl = (u8)v;
        if (!(v & 0x80)) um_irq_status &= ~0x80;
        f |= um_tm_plan(now);
        break;
    case 0x35:                         /* reload value; the count restarts from 0 */
        um_tm_sync(now);
        um_tlatch = (u8)v;
        um_tval = 0;
        um_irq_status &= ~0x80;
        f |= um_tm_plan(now);
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        um_prg[off & 7] = (u8)v;
        um_prg_calc();
        f |= UMF_PRG;
        break;
    case 0x48: um_dma_ctl = (u8)v; break;
    case 0x49: um_dma_page = (u8)((um_dma_page & ~0x1F) | (v & 0x1F)); break;
    case 0x4A: um_dma_src = (u16)((um_dma_src & 0xFF00) | v); break;
    case 0x4B: um_dma_src = (u16)((um_dma_src & 0x00FF) | v << 8); break;
    case 0x4C: um_dma_dst = (u16)((um_dma_dst & 0xFF00) | v); break;
    case 0x4D: um_dma_dst = (u16)((um_dma_dst & 0x00FF) | v << 8); break;
    case 0x4E: um_dma_len = (u16)((um_dma_len & 0xFF00) | v); break;
    case 0x4F: um_dma_len = (u16)((um_dma_len & 0x00FF) | (v << 8 & 0x7F00)); break;
    }
    if (um_dma_ctl & 0x80) {
        um_dma_ctl &= ~0x80;
        um_dma_run();
    }
    return f | um_irq_line();
}

__attribute__((target("arm")))
u32 um_reg_R(u32 off, u32 now)
{
    switch (off) {
    case 0x32: return um_irq_mask;
    case 0x33: return um_irq_status;
    case 0x34: return um_tctl;
    case 0x35: return um_tlatch;
    case 0x36: um_tm_sync(now); return um_tval;
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        return um_prg[off & 7];
    case 0x48: return um_dma_ctl;
    case 0x49: return um_dma_page;
    case 0x4A: return um_dma_src & 0xFF;
    case 0x4B: return um_dma_src >> 8;
    case 0x4C: return um_dma_dst & 0xFF;
    case 0x4D: return um_dma_dst >> 8;
    case 0x4E: return um_dma_len & 0xFF;
    case 0x4F: return um_dma_len >> 8;
    }
    return 0x40;                       /* open bus */
}

/* ---- the scroll of every line ------------------------------------------
 * Loopy's t/v/x with the UM6578's 16-bit $2006 and 32-row nametables: the
 * Y coordinate is NT bit 11, coarse Y and fine Y, 0-511, and moving down a
 * line is +1 mod 512 (IncrementV wraps row 31 into the other nametable).
 * Register writes during the picture start a new segment at the first line
 * they reach -- the next one from dot 128 on, PocketNES's rule
 * (get_scanline_2) -- and um_frame_end turns the segments into each line's
 * BG0 scroll and map. */
static inline u32 um_H(void) { return (um_t & 0x400 ? 256u : 0u) | (um_t & 0x1F) << 3 | um_x; }
static inline u32 um_Y(u32 v) { return (v & 0x800 ? 256u : 0u) | ((v >> 5) & 31) << 3 | ((v >> 12) & 7); }

static void um_frame_start(void)
{
    um_seg[0].line = 0;
    um_seg[0].cfg = um_2008 & 1;
    um_seg[0].h = (u16)um_H();
    um_seg[0].y = (u16)um_Y(um_t);     /* the pre-render line copies t to v */
    um_nseg = 1;
    um_fs = 1;
}

/* The first picture line a write at `now` can still change, or -1. */
static int um_ev_line(u32 now, int *pl, int *pd)
{
    int l, d;
    um_line_of(now, &l, &d);
    *pl = l; *pd = d;
    if (!um_fs) {
        if (!((l >= 0 && l < 240) || (l == -1 && d >= 304))) return -1;
        um_frame_start();
    }
    if (l >= 240) return -1;
    const int n = l < 0 ? 0 : (d < 128 ? l : l + 1);
    return n < 240 ? n : -1;
}

#define UM_YCONT 0xFFFF
static void um_seg_put(int n, u32 h, u32 y, u32 cfg)
{
    um_seg_t *s = &um_seg[um_nseg - 1];
    if (n < s->line) n = s->line;
    if (y == UM_YCONT) y = (s->y + (u32)(n - s->line)) & 511;
    if (n != s->line) {
        if (um_nseg < UM_NSEG) { s++; um_nseg++; }
        s->line = (u8)n;
    }
    s->h = (u16)h;
    s->y = (u16)y;
    s->cfg = (u8)cfg;
}

static inline u32 um_inc(void) { return (_ppuctrl0 & 4) ? 32u : 1u; }

/* $2000 (after PocketNES's ctrl0_W), $2005-$2008 writes. */
__attribute__((target("arm")))
void um_ppu_reg(u32 v, u32 reg, u32 now)
{
    int l, d;
    const int n = um_ev_line(now, &l, &d);
    v &= 0xFF;
    switch (reg) {
    case 0:
        um_t = (u16)((um_t & ~0x0C00) | (v & 3) << 10);
        if (n >= 0) um_seg_put(n, um_H(), UM_YCONT, um_2008 & 1);
        break;
    case 5:
        if (!um_w) {
            um_t = (u16)((um_t & ~0x1F) | v >> 3);
            um_x = (u8)(v & 7);
            if (n >= 0) um_seg_put(n, um_H(), UM_YCONT, um_2008 & 1);
        } else {
            um_t = (u16)((um_t & ~0x73E0) | (v & 7) << 12 | (v & 0xF8) << 2);
        }
        um_w ^= 1;
        break;
    case 6:                            /* all 16 bits (Write6 allows bit 15) */
        if (!um_w) {
            um_t = (u16)((um_t & 0x00FF) | v << 8);
        } else {
            um_t = (u16)((um_t & 0xFF00) | v);
            um_v = um_t;
            /* before dot 256 the line's own Y increment still comes */
            if (n >= 0) um_seg_put(n, um_H(), (um_Y(um_v) + ((n == l + 1 && d < 256) ? 1u : 0u)) & 511, um_2008 & 1);
        }
        um_w ^= 1;
        break;
    case 7:
        um_ppu_wr(um_v, v);
        um_v = (u16)(um_v + um_inc());
        break;
    case 8:
        um_2008 = (u8)v;
        if (n >= 0) um_seg_put(n, um_H(), UM_YCONT, v & 1);
        break;
    }
}

__attribute__((target("arm")))
u32 um_r2007(void)
{
    const u32 r = um_buf;
    um_buf = (u8)um_ppu_rd(um_v);
    um_v = (u16)(um_v + um_inc());
    return r;
}

/* NES line 242 (vt_palette_rebuild_gba), before the effect buffers swap:
 * each line's BG0 scroll and map.  scrollbuff keeps PocketNES's layout --
 * X + 8 with the 8x8 sprite pattern table in bit 15 (update_sprites reads
 * it), and (Y - line) in the high half. */
static void um_fill16(u16 *p, u32 v, u32 n)
{
    if (((u32)p & 2) && n) { *p++ = (u16)v; n--; }
    if (n >= 2) { memset32(p, v | v << 16, (int)((n & ~1u) * 2)); p += n & ~1u; }
    if (n & 1) *p = (u16)v;
}

__attribute__((target("arm")))
void um_frame_end(void)
{
    const u32 now = (u32)TimeoutState[1];
    if (um_tctl & 0x80) um_tm_sync(now);  /* keeps the count's time base within a frame */
    else um_tlast = now;
    if (!um_fs) um_frame_start();
    u32 *sb = (u32 *)_scrollbuff;
    u16 *cb = (u16 *)_bg0cntbuff;
    const u32 spr = (um_2008 & 0x10) ? 0x8000u : 0u;
    for (u32 i = 0; i < um_nseg; i++) {
        const um_seg_t *s = &um_seg[i];
        const u32 end = (i + 1 < um_nseg) ? um_seg[i + 1].line : 240u;
        if (end <= s->line) continue;
        const u32 word = (((s->h + 8u) & 0x1FF) | spr) | (((u32)(s->y - s->line) & 0x1FF) << 16);
        /* not memset32: its tail stores halfwords of the low half (memcopy.s,
         * shared with memset16), which put X + 8 in the last line's vofs */
        for (u32 *p = sb + s->line, *e = sb + end; p < e; p++) *p = word;
        um_fill16(cb + s->line, (s->cfg & 1) ? UM_CNT1 : UM_CNT4, end - s->line);
    }
    um_fs = 0;

    u32 nv[5] = { 0, 0, 0, 0, 0 };
    for (u32 i = 0; i < um_nseg; i++) {
        const um_seg_t *s = &um_seg[i];
        const u32 end = (i + 1 < um_nseg) ? um_seg[i + 1].line : 240u;
        if (end <= s->line) continue;
        const u32 r0 = s->y >> 3, r1 = (s->y + (end - s->line) - 1) >> 3;
        if (s->cfg & 1) {
            for (u32 r = r0; r <= r1; r++) { const u32 b = 128u + (r & 31); nv[b >> 5] |= 1u << (b & 31); }
        } else {
            const u32 hm = 1u << ((s->h >> 8) & 1) | 1u << (((s->h + 255u) >> 8) & 1);
            for (u32 r = r0; r <= r1; r++) {
                const u32 b = (r & 63) << 1;
                if (hm & 1) nv[b >> 5] |= 1u << (b & 31);
                if (hm & 2) nv[b >> 5] |= 2u << (b & 31);
            }
        }
    }
    for (u32 k = 0; k < 5; k++) um_vis_next[k] = nv[k];
}

/* ---- background tile cache ---------------------------------------------
 * A key is one GBA tile: 2bpp, (tile << 2 | palette & 3) -- the low two
 * palette bits are baked into the pixels (4p + c), so GBA BG palette bank
 * b colour e is UM colour 16b + e for 2bpp and 4bpp alike, and a map entry
 * carries the palette >> 2 as its bank; 4bpp, 0x8000 | tile & ~1. */
static inline const u8 *um_pat(u32 a)
{
    if (a < 0x2800) return NES_VRAM + a;
    if (a >= 0x8000) return um_mem->chr + (a - 0x8000);
    return 0;
}

static void um_tile_conv(u32 s, u32 key)
{
    volatile u32 *d = (volatile u32 *)(0x06008000u + s * 32u);
    const u32 *sp = um_spread;
    if (key & 0x8000) {
        const u32 a = (key & 0xFFE) << 4;
        const u8 *p = um_pat(a), *q = um_pat(a | 0x10);
        for (u32 r = 0; r < 8; r++) {
            u32 row = p ? (sp[p[r]] | sp[p[r + 8]] << 1) : 0;
            if (q) row |= sp[q[r]] << 2 | sp[q[r + 8]] << 3;
            d[r] = row;
        }
    } else {
        const u8 *p = um_pat((key >> 2) << 4);
        const u32 a2 = (key & 3) << 2;
        for (u32 r = 0; r < 8; r++) {
            u32 row = p ? (sp[p[r]] | sp[p[r + 8]] << 1) : 0;
            if (a2) row |= ((row | row >> 1) & 0x11111111u) * a2;
            d[r] = row;
        }
    }
    um_dbg[2]++;
}

static void um_hash_rebuild(void)
{
    um_mem_t *m = um_mem;
    for (u32 i = 0; i < UM_HSIZE; i++) m->hash[i] = 0xFFFF;
    um_hcount = 0;
    for (u32 s = 1; s < UM_NSLOT; s++) {
        const u32 key = m->s2k[s];
        if (key == 0xFFFF) continue;
        u32 k = (key * 0x9E3779B1u) >> 21;
        while (m->hash[k] != 0xFFFF) k = (k + 1) & (UM_HSIZE - 1);
        m->hash[k] = (u16)s;
        um_hcount++;
    }
}

/* A slot no cell shows, the oldest first; 0 if none. */
static u32 um_alloc(void)
{
    if (um_nofree) return 0;
    const u16 *rc = um_mem->rc;
    u32 s = um_alloc_cur;
    for (u32 n = 1; n < UM_NSLOT; n++, s++) {
        if (s >= UM_NSLOT) s = 1;
        if (!rc[s]) { um_alloc_cur = (u16)(s + 1); return s; }
    }
    um_nofree = 1;                     /* until a count drops to 0 */
    um_dbg[3]++;
    return 0;
}

/* The slot holding key, 0 if none. */
static u32 um_find(u32 key)
{
    const um_mem_t *m = um_mem;
    for (u32 k = (key * 0x9E3779B1u) >> 21;; k = (k + 1) & (UM_HSIZE - 1)) {
        const u32 s = m->hash[k];
        if (s == 0xFFFF) return 0;
        if (m->s2k[s] == key) return s;
    }
}

static u32 um_slot_for(u32 key)
{
    um_mem_t *m = um_mem;
    u32 k = (key * 0x9E3779B1u) >> 21;
    for (;;) {
        const u32 s = m->hash[k];
        if (s == 0xFFFF) break;
        if (m->s2k[s] == key) return s;
        k = (k + 1) & (UM_HSIZE - 1);
    }
    const u32 s = um_alloc();
    if (!s) return 0;
    const u32 prev = m->s2k[s];
    if (prev != 0xFFFF) {
        if (prev & 0x8000) { m->tcnt[prev & 0xFFE]--; m->tcnt[(prev & 0xFFE) | 1]--; }
        else m->tcnt[prev >> 2]--;
    }
    if (key & 0x8000) { m->tcnt[key & 0xFFE]++; m->tcnt[(key & 0xFFE) | 1]++; }
    else m->tcnt[key >> 2]++;
    m->s2k[s] = (u16)key;
    m->sdirty[s] = 0;
    um_tile_conv(s, key);
    if (um_hcount >= UM_HSIZE * 3 / 4) um_hash_rebuild();
    else { m->hash[k] = (u16)s; um_hcount++; }
    return s;
}

/* Map cell w from its nametable word; 0 if it found no slot. */
static u32 um_cell_set(u32 w)
{
    um_mem_t *m = um_mem;
    volatile u16 *e = &UM_MAP[w];
    const u32 old = *e & 0x3FF;
    const u8 *nt = NES_VRAM + w * 2;
    const u32 lo = nt[0], hi = nt[1];
    const u32 tile = lo | (hi & 15) << 8;
    const u32 key = um_mode4 ? (0x8000u | (tile & 0xFFE)) : (tile << 2 | ((hi >> 4) & 3));
    const u32 s = um_slot_for(key);
    if (s != old) {
        if (old && old < UM_NSLOT && m->rc[old] && --m->rc[old] == 0) um_nofree = 0;
        if (s) m->rc[s]++;
    }
    *e = (u16)(s | (hi >> 6) << 12);
    um_dbg[4]++;
    return s;
}

static void um_cache_reset(void)
{
    um_mem_t *m = um_mem;
    for (u32 i = 0; i < UM_HSIZE; i++) m->hash[i] = 0xFFFF;
    for (u32 i = 0; i < UM_NSLOT; i++) { m->s2k[i] = 0xFFFF; m->rc[i] = 0; m->sdirty[i] = 0; }
    for (u32 i = 0; i < 0x1400 / 2; i++) ((volatile u32 *)UM_MAP)[i] = 0;    /* every cell blank */
    for (u32 i = 0; i < 8; i++) ((volatile u32 *)0x06008000)[i] = 0;         /* slot 0 */
    for (u32 i = 0; i < 0x280 / 4; i++) ((u32 *)m->ntdirty)[i] = 0xFFFFFFFFu;
    for (u32 i = 0; i < 512 / 4; i++) ((u32 *)m->tdirty)[i] = 0;
    for (u32 i = 0; i < 0x1000 / 4; i++) ((u32 *)m->tcnt)[i] = 0;
    for (u32 k = 0; k < 5; k++) um_vis[k] = 0;    /* the shown rows come back on at once */
    um_tany = um_sany = 0;
    um_ntany = 1;
    um_nofree = 0;
    um_alloc_cur = 1;
    um_hcount = 0;
    um_ncur = um_scur = 0;
    um_full = 0;
    um_dbg[5]++;
}

/* Slots whose tiles were written: converted again, at most `budget` a
 * vblank (a level's CHR-RAM load dirties all of them at once). */
static void um_tiles_refresh(u32 budget)
{
    um_mem_t *m = um_mem;
    if (um_tany) {                         /* only tiles some slot holds get marked */
        um_tany = 0;
        u32 *td = (u32 *)m->tdirty;
        for (u32 k = 0; k < 512 / 4; k++) {
            u32 bits = td[k];
            if (!bits) continue;
            td[k] = 0;
            for (; bits; bits &= bits - 1) {
                const u32 t = k * 32 + (u32)__builtin_ctz(bits);
                for (u32 key = t << 2; key < (t << 2) + 4; key++) {
                    const u32 s = um_find(key);
                    if (s) { m->sdirty[s] = 1; um_sany = 1; }
                }
                const u32 s = um_find(0x8000u | (t & 0xFFE));
                if (s) { m->sdirty[s] = 1; um_sany = 1; }
            }
        }
    }
    if (!um_sany) return;
    u32 s = um_scur;
    for (u32 n = 0; n < UM_NSLOT; n++, s++) {
        if (s >= UM_NSLOT) s = 0;
        if (!m->sdirty[s]) continue;
        if (!budget) { um_scur = (u16)s; return; }
        budget--;
        m->sdirty[s] = 0;
        if (m->s2k[s] != 0xFFFF) um_tile_conv(s, m->s2k[s]);
    }
    um_sany = 0;
}

/* Cells whose nametable words were written, at most `budget` a vblank. */
static void um_cells_refresh(u32 budget)
{
    u32 *nd = (u32 *)um_mem->ntdirty;
    u32 keep_any = 0;
    u32 k = um_ncur;
    for (u32 i = 0; i < 0x280 / 4; i++, k++) {
        if (k >= 0x280 / 4) k = 0;
        u32 bits = nd[k], keep = 0;
        if (!bits) continue;
        while (bits) {
            if (!budget) {
                nd[k] = bits | keep;
                um_ncur = (u16)k;
                return;                    /* um_ntany stays set */
            }
            budget--;
            const u32 b = (u32)__builtin_ctz(bits);
            bits &= bits - 1;
            const u32 w = k * 32 + b;
            if (!um_vis_on(um_cell_vi(w))) continue;    /* hidden: nothing to show */
            if (!um_cell_set(w)) keep |= 1u << b;
        }
        nd[k] = keep;
        keep_any |= keep;
    }
    /* cells that found no slot stay marked: again next vblank if one came
     * free meanwhile, else with the next pass a nametable write starts */
    um_ntany = (keep_any && !um_nofree) ? 1 : 0;
}

/* The 64 colours, Furbtendulator's NTSC palette for this console (furb_cli
 * --dump-palette, RGB888 >> 3): the colour reference here (guide s.79h).
 * PocketNES's own table (MAPPED_RGB) matched it in 13 of 64. */
static const u16 um_rgb[64] = {
    0x294A, 0x4040, 0x4C00, 0x4406, 0x2C0B, 0x080D, 0x000C, 0x0048,
    0x00A1, 0x00E0, 0x00E0, 0x08C0, 0x2880, 0x0000, 0x0000, 0x0000,
    0x5294, 0x6D20, 0x7CA3, 0x742F, 0x5816, 0x2819, 0x0097, 0x0112,
    0x01AA, 0x01E0, 0x0200, 0x1DE0, 0x4D80, 0x0000, 0x0000, 0x0000,
    0x7FFF, 0x7EA0, 0x7E10, 0x7DBA, 0x7D7F, 0x5D9F, 0x21FF, 0x027F,
    0x0317, 0x0349, 0x0B60, 0x4360, 0x7700, 0x2108, 0x0000, 0x0000,
    0x7FFF, 0x7F75, 0x7F59, 0x7F1D, 0x7F1F, 0x771F, 0x5F3F, 0x4F5F,
    0x4FBE, 0x47B8, 0x53D4, 0x67D2, 0x7BB2, 0x5AD6, 0x0000, 0x0000,
};

static void um_pal_upload(void)
{
    const u32 grey = (*(const u8 *)((u32)&_ppuctrl0 + 2) & 1) ? 0x30u : 0x3Fu;   /* _ppuctrl1 */
    if (!um_pal_dirty && grey == um_grey && ++um_pal_age < 16) return;
    um_pal_dirty = 0;
    um_grey = (u8)grey;
    um_pal_age = 0;
    const u16 *rgb = um_rgb;
    volatile u16 *bg = (volatile u16 *)0x05000000;
    volatile u16 *obj = (volatile u16 *)0x05000200;
    for (u32 i = 0; i < 64; i++) bg[i] = rgb[um_pal[i] & grey];
    for (u32 n = 0; n < 4; n++)
        for (u32 c = 1; c < 4; c++) obj[n * 16 + c] = rgb[um_pal[0x10 + n * 4 + c] & grey];
}

/* Rows newly shown get their cells built; rows no longer shown give their
 * slots back. */
static void um_vis_apply(void)
{
    um_mem_t *m = um_mem;
    for (u32 k = 0; k < 5; k++) {
        const u32 nx = um_vis_next[k], cur = um_vis[k];
        if (nx == cur) continue;
        um_vis[k] = nx;
        for (u32 ch = nx ^ cur; ch; ch &= ch - 1) {
            const u32 vi = k * 32 + (u32)__builtin_ctz(ch);
            u32 w0;
            if (vi >= 128) w0 = 0x1000 + (vi - 128) * 32;
            else {
                const u32 row = vi >> 1, n = (row >> 5) * 2 + (vi & 1);
                w0 = n * 1024 + (row & 31) * 32;
            }
            if (nx >> (vi & 31) & 1) {
                ((u32 *)m->ntdirty)[w0 >> 5] = 0xFFFFFFFFu;
                um_ntany = 1;
            } else {
                for (u32 w = w0; w < w0 + 32; w++) {
                    const u32 old = UM_MAP[w] & 0x3FF;
                    if (old && old < UM_NSLOT && m->rc[old] && --m->rc[old] == 0) um_nofree = 0;
                    UM_MAP[w] = 0;
                }
            }
        }
    }
}

/* GBA vblank, the last picture work of the handler, on the EWRAM stack: in
 * place of vt_16c_palette_fixup while um_hooks has repointed the call. */
__attribute__((target("arm")))
void um_vblank(void)
{
    volatile u16 *ime = (volatile u16 *)0x04000208;
    const u16 saved = *ime;
    *ime = 0;
    const u32 m4 = (um_2008 & 0x80) ? 1u : 0u;
    if (m4 != um_mode4 || um_full) { um_mode4 = (u8)m4; um_cache_reset(); }
    um_vis_apply();
    um_tiles_refresh(128);
    if (um_ntany) um_cells_refresh(384);
    um_pal_upload();
    *ime = saved;
}

/* ---- hooks -------------------------------------------------------------
 * ppu.s calls vt_palette_rebuild_gba at NES line 242 and
 * vt_16c_palette_fixup_stacked late in the GBA vblank, each as
 * `mov lr,pc; ldr pc,=fn`.  For a UM6578 cart the two literals point at
 * um_frame_end and um_vblank_stacked (mapUM.s) instead; any other cart gets
 * them back.  A test in those VT functions cost a few cycles a frame on
 * every VT cart, and that alone re-timed Table Soccer VT369's stale BG tiles
 * (open item 3): 710 of 800 frames changed. */
extern u32 vt_frame_end_call[], vt_vbl_fixup_call[];
extern u8 vt_pal_owned;
void vt_palette_rebuild_gba(void);
void vt_16c_palette_fixup_stacked(void);
void um_vblank_stacked(void);

static u32 *um_call_lit(u32 *call)
{
    const u32 w = call[1];
    if (call[0] != 0xE1A0E00Fu || (w & 0xFF7FF000u) != 0xE51FF000u) return 0;   /* mov lr,pc; ldr pc,[pc,#+-n] */
    const u32 n = w & 0xFFF;
    return (u32 *)((u32)&call[1] + 8u + ((w & 0x00800000u) ? n : (u32)-n));
}

__attribute__((target("arm")))
void um_hooks(u32 on)
{
    u32 *a = um_call_lit(vt_frame_end_call), *b = um_call_lit(vt_vbl_fixup_call);
    if (a) *a = on ? (u32)um_frame_end : (u32)vt_palette_rebuild_gba;
    if (b) *b = on ? (u32)um_vblank_stacked : (u32)vt_16c_palette_fixup_stacked;
    vt_pal_owned = (u8)(on ? 1 : 0);   /* run_palette keeps off the UM palette */
    if (!a || !b) um_dbg[6] = 0xDEAD;   /* a call site no longer has that shape */
}

/* The vblank work as vt_16c_palette_fixup_body did it for a cart that owns
 * the palette: no VCOUNT palette splits, then um_vblank. */
__attribute__((target("arm")))
void um_vblank_hook(void)
{
    vt_pal_owned = 1;
    *(volatile u16 *)0x04000004 &= (u16)~0x0020;   /* DISPSTAT: no VCOUNT IRQ */
    um_vblank();
}

__attribute__((target("arm")))
void um_reset(void)
{
    um_mem_t *m = um_mem;
    memset32(m, 0, um_mem_size);
    um_ram_ptr = m->ram - 0x800;
    um_ram2_ptr = m->ram2;
    for (u32 b = 0; b < 256; b++) {
        u32 v = 0;
        for (u32 x = 0; x < 8; x++) if (b & (0x80u >> x)) v |= 1u << (4 * x);
        um_spread[b] = v;
    }
    for (u32 i = 0; i < 8; i++) um_prg[i] = (u8)i;
    um_r4016 = 0;
    um_prg_calc();
    um_irq_mask = 0xFF;
    um_irq_status = um_tctl = um_tlatch = um_tval = um_tpresc = 0;
    um_tlast = (u32)TimeoutState[1];
    um_tsched = 0;
    um_dma_ctl = um_dma_page = 0;
    um_dma_src = um_dma_dst = um_dma_len = 0;
    um_2008 = um_w = um_x = um_buf = 0;
    um_t = um_v = 0;
    for (u32 i = 0; i < 64; i++) um_pal[i] = 0;
    um_pal_dirty = 1;
    um_fs = um_nseg = 0;
    um_mode4 = 0;
    um_full = 1;                       /* the cache and maps start over at the first vblank */
    for (u32 i = 0; i < 8; i++) um_dbg[i] = 0;
}

#endif
