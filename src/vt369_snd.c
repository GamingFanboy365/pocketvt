/*
 * vt369_snd.c -- the VT369 sound CPU, high-level (guide s.84).
 *
 * A VT369 has a second 6502 for sound.  The main CPU loads it through
 * $4800-$4FFF (sound RAM $1800-$1FFF), points its timer-IRQ vector
 * ($1FF8/$1FF9; the reference calls it the reset vector) at a program and
 * starts it with $4162 = $0D.  The program lives in the cart's 4K embedded
 * ROM (Lucky Lawn Mower, Fire Fighter, Jewel Master: $40AE, the ROM seen
 * from the sound CPU at $4000) or in the sound RAM (Table Soccer: $0293),
 * and it plays ADPCM channels.  Like Furbtendulator's default "VT369 sound
 * HLE" (OneBus_VT369.cpp, APU_VT369::Run), this does not run that program:
 * it recognises the vector and does what the program does to the shared
 * sound RAM.  One deliberate difference: a start bit acts on its rising
 * edge, as in the cart's own program (vt369_run_48).
 *
 * Output: DirectSound B.  PocketNES plays the NES DMC there (DMA2 -> FIFO B,
 * timer 0 = sample rate, timer 1 counts PCMWAVSIZE samples and interrupts);
 * the VT369 cannot play DMC in these modes, so while the sound CPU runs the
 * channel is ours.  vt_adpcm_mix_gba, called at the end of that interrupt,
 * restarts timer 0 and hands over to vt369_snd_fill (on the EWRAM stack),
 * which renders the next 128 samples into the idle one of two buffers at
 * the HLE's own sample rate: one output sample per sound-CPU timer tick, so
 * no resampling.  The per-sample loops run from IWRAM.
 */
#ifdef VT369_SND_HOSTTEST          /* tools/probes/vt369snd_test.c builds this file natively */
#include "vt369snd_hoststubs.h"
#else
#include "includes.h"
#include "ppu_vt.h"
#include "vt_regs.h"
#include "config.h"
#endif

#if VT_MODE

#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define SR(a)    vt369_sram[(a) - 0x1800]
/* VT369_HOT: ARM in ROM (the 4-channel loop; ARM keeps its state in
 * registers, and it only runs while a sound effect plays).  VT369_FAST:
 * ARM in IWRAM, for the loops that run every sample while anything plays
 * (the 368 bytes the aPLib loop gave up, apack.s).  Code in cart ROM costs
 * ~4 cycles an ARM instruction; the first all-ROM version spent 35K cycles
 * a NES frame on Table Soccer's 3 channels (s.84). */
#if defined(VT369_SND_HOSTTEST)
#define VT369_HOT  __attribute__((noinline))
#define VT369_FAST __attribute__((noinline))
#else
#define VT369_HOT  __attribute__((target("arm"), noinline))
#define VT369_FAST __attribute__((section(".iwram.vt369snd"), target("arm"), long_call, noipa))
#endif
/* 16-bit DAC -> signed 8-bit FIFO sample: (dac * gain) >> 9.  PocketNES
 * plays the APU ~2.2x louder than Furbtendulator's mixer, so the gains put
 * the sound CPU back in proportion (s.84, measured on Jewel Master's
 * effects against the same ROM with the effect's volume patched to 0):
 * x3 for the 4-channel programs; x1.5 for the 3-bit stream, whose music
 * already peaks near full scale in the reference (Table Soccer: 19354). */
#ifndef VT369_SND_GAIN48
#define VT369_SND_GAIN48 6
#endif
#ifndef VT369_SND_GAIN3
#define VT369_SND_GAIN3  3
#endif

EWRAM_BSS u8  vt369_sram[0x800] __attribute__((aligned(4)));  /* $1800-$1FFF */
EWRAM_BSS u8  vt369_snd_on;
EWRAM_BSS const u8 *vt369_misc;          /* 4K embedded ROM, CPU $1000-$1FFF (loadcart.c) */
/* DMA2 sources, two of them: timer1interrupt restarts DMA2 on entry, from
 * the block the previous fill wrote, while this fill writes the other one.
 * (One buffer raced: the DMA was already reading the block being
 * rewritten.)  Each is 128 samples, then 128 holding the last one: the DMA
 * keeps reading until the interrupt restarts it, and when that is late
 * (vt369_vblank masks IME) it ran on past an 8-byte tail into other
 * variables and played them as full-scale clicks (s.84). */
#define VT369_PCM_GUARD 128
EWRAM_BSS s8  vt369_pcm[2][128 + VT369_PCM_GUARD] __attribute__((aligned(4)));
EWRAM_BSS u8  vt369_pcm_next;             /* the block the next fill writes */
EWRAM_BSS u8  vt369_pcm_silent[2];        /* that block holds only zeroes */
EWRAM_BSS u16 vt369_snd_n;                /* timer period in sound-CPU units */
EWRAM_BSS u32 vt369_adpcm_frame[3][2];    /* 3-bit ADPCM: 64-bit frame */
EWRAM_BSS u8  vt369_adpcm_left[3];
EWRAM_BSS u32 vt369_snd_dbg[4];           /* 0 fills 1 ticks 2 unknown vector 3 active ticks */
EWRAM_BSS u8  vt369_st_prev;              /* start mask as last seen: starts are edges */


static inline u32 vt369_reset_vector(void) { return SR(0x1FF8) | SR(0x1FF9) << 8; }

static inline const u8 *vt369_prg_base(void) { return vt_chr_src ? vt_chr_src : rombase; }
static inline u32 vt369_prg_mask(void)
{
    const u32 m = vt_chr_src ? vt_chr_mask : rommask;
    return m ? m : 0xFFFFFFFFu;
}

/* ---- decoders (ADPCM_VT369.cpp, ADPCM_VT1682.cpp) ---------------------- */
static const s8 vt369_step[16][16] = {
    {0,14,28,42,56,70,84,97,-111,-97,-84,-70,-56,-42,-28,-14},
    {0,13,26,39,52,65,78,91,-104,-91,-78,-65,-52,-39,-26,-13},
    {0,11,21,32,43,54,64,75,-86,-75,-64,-54,-43,-32,-21,-11},
    {0,9,18,27,35,44,53,62,-71,-62,-53,-44,-35,-27,-18,-9},
    {0,7,13,20,27,34,40,47,-54,-47,-40,-34,-27,-20,-13,-7},
    {0,6,11,17,22,28,33,39,-44,-39,-33,-28,-22,-17,-11,-6},
    {0,5,9,14,18,23,27,32,-36,-32,-27,-23,-18,-14,-9,-5},
    {0,4,8,11,15,19,23,26,-30,-26,-23,-19,-15,-11,-8,-4},
    {0,3,6,9,12,15,17,20,-23,-20,-17,-15,-12,-9,-6,-3},
    {0,2,5,7,10,12,14,17,-19,-17,-14,-12,-10,-7,-5,-2},
    {0,2,4,6,8,10,12,14,-16,-14,-12,-10,-8,-6,-4,-2},
    {0,2,3,5,6,8,10,11,-13,-11,-10,-8,-6,-5,-3,-2},
    {0,1,2,4,5,6,7,9,-10,-9,-7,-6,-5,-4,-2,-1},
    {0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1},
    {0,1,2,3,3,4,5,6,-7,-6,-5,-4,-3,-3,-2,-1},
    {0,1,1,2,3,4,4,5,-6,-5,-4,-4,-3,-2,-1,-1}
};

static const u8 vt1682_istep[4] = { 0, 0, 3, 5 };
static const u8 vt1682_itab[26] = { 0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,20,20,20,20 };
static const s8 vt1682_step[4][21] = {
    { 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 5, 6, 7, 8,10,11,13,15},
    { 1, 3, 3, 3, 4, 4, 6, 6, 7, 9,10,12,15,16,19,22,25,30,34,40,46},
    { 3, 5, 5, 6, 7, 8,10,11,13,16,18,21,25,28,32,38,43,51,58,68,78},
    { 4, 7, 7, 8,10,11,14,15,18,22,25,29,35,39,45,53,60,71,81,95,109}
};

/* ---- the two programs ------------------------------------------------- */
/* Both are run channel by channel, n ticks at a time (a fill is 128 ticks).
 * That is exact: while the fill runs the main CPU is interrupted, so every
 * input the programs read (start/stop masks, loop flags, command) is
 * constant for the fill, and the channels never touch each other's state.
 * The per-channel loops are ARM so their state stays in registers; the
 * first version ticked all channels together and spilled to the EWRAM
 * stack, which halved the carts' speed with every channel idle (s.84). */

/* Reset $0293: 3-bit ADPCM streaming, commands at $1FA2 (channel $1FA3). */
static void vt369_cmd_0293(void)
{
    const u32 ch = SR(0x1FA3) % 3;
    switch (SR(0x1FA2)) {
        case 0x01:                                    /* start channel */
            SR(0x1CB0 + ch) = 0xFF;
            SR(0x1CB7 + ch * 2) = 0;
            SR(0x1980 + ch * 2) = 0;
            SR(0x1981 + ch * 2) = 0;
            SR(0x1FA2) = 0;
            vt369_adpcm_left[ch] = 0;
            break;
        case 0x02: {                                  /* set rate */
            const s16 p = (s16)((SR(0x1FA0) | SR(0x1FA1) << 8) * 3);
            vt369_snd_n = (u16)-p;
            SR(0x1FA2) = 0;
            break;
        }
        case 0x03:                                    /* stop channel */
            SR(0x1CB0 + ch) = 0;
            SR(0x1FA2) = 0;
            break;
    }
}

/* vt1682 3-bit step, both tables in one lookup: [index*8 + code] = the
 * predictor delta (low byte, signed) | the next index << 8.  Built by
 * vt369_snd_reset; EWRAM reads are cheaper than two ROM table reads. */
EWRAM_BSS u16 vt1682_tab[21 * 8];

static void vt1682_tab_init(void)
{
    for (u32 ix = 0; ix < 21; ix++)
        for (u32 code = 0; code < 8; code++) {
            const s32 d = (code & 4) ? -vt1682_step[code & 3][ix] : vt1682_step[code & 3][ix];
            vt1682_tab[ix * 8 + code] = (u16)((d & 0xFF) | vt1682_itab[ix + vt1682_istep[code & 3]] << 8);
        }
}

/* One 3-bit channel's decoder state for the loop below. */
typedef struct { u32 lo, hi; s32 pred; u32 idx, left, rd; } vt369_adpcm3;

/* n ticks of one 3-bit channel, added into mix[].  A zero frame (or one
 * with bit 63 set, which the program zeroes) at index 0 and predictor 0
 * adds nothing, so those ticks are skipped: an idle stream costs nothing. */
VT369_FAST
static void vt369_adpcm3_run(vt369_adpcm3 *c, const u32 *ring, s32 *mix, u32 n)
{
    const u16 *tab = vt1682_tab;
    u32 lo = c->lo, hi = c->hi, idx = c->idx, left = c->left, rd = c->rd;
    s32 pred = c->pred;
    s32 *end = mix + n;                              /* n > 0 */
    do {
        if (left == 0) {                             /* the ring is 4-aligned, rd a multiple of 8 */
            lo = ring[rd >> 2];
            hi = ring[(rd >> 2) + 1];
            if (hi & 0x80000000u) lo = hi = 0;
            left = 21;
            rd = (rd + 8) & 0x7F;
        }
        if (!(lo | hi | idx | pred)) {               /* silence */
            const u32 k = (u32)(end - mix) < left ? (u32)(end - mix) : left;
            left -= k;
            mix += k;
            continue;                                /* to the do-while test */
        }
        const u32 e = tab[idx * 8 + (lo & 7)];
        s32 p = pred + (s8)e;
        pred = p < -128 ? -128 : p > 127 ? 127 : p;
        idx = e >> 8;
        *mix++ += pred << 7;
        lo = lo >> 3 | hi << 29;
        hi >>= 3;
        left--;
    } while (mix < end);
    c->lo = lo; c->hi = hi; c->idx = idx; c->left = left; c->rd = rd; c->pred = pred;
}

/* One 3-bit channel for n ticks: its state is in sound RAM (read offset,
 * predictor, index) and in vt369_adpcm_frame/left (the frame in flight). */
static void vt369_run_0293(u32 ch, s32 *mix, u32 n)
{
    vt369_adpcm3 c;
    c.rd = SR(0x1CB7 + ch * 2) & 0x78;               /* reachable offsets: steps of 8 */
    c.left = vt369_adpcm_left[ch];
    c.lo = vt369_adpcm_frame[ch][0]; c.hi = vt369_adpcm_frame[ch][1];
    c.pred = (s8)SR(0x1980 + ch * 2);
    c.idx = SR(0x1981 + ch * 2);
    if (c.idx > 20) c.idx = 20;
    vt369_adpcm3_run(&c, (const u32 *)&SR(0x1800 + ch * 0x80), mix, n);
    SR(0x1CB7 + ch * 2) = (u8)c.rd; vt369_adpcm_left[ch] = (u8)c.left;
    vt369_adpcm_frame[ch][0] = c.lo; vt369_adpcm_frame[ch][1] = c.hi;
    SR(0x1980 + ch * 2) = (u8)c.pred; SR(0x1981 + ch * 2) = (u8)c.idx;
}

/* The 48-nibble-frame programs: sample data in PRG (or streamed through
 * sound RAM), 4 channels, start/stop masks. */
typedef struct { u16 masks, period, loop; u8 loopinc, stream; } vt369_prog;

static int vt369_prog_for(u32 rv, vt369_prog *p)
{
    p->loop = 0; p->loopinc = 1; p->stream = 0;
    switch (rv) {
        case 0x0203: case 0x02A0: p->masks = 0x184C; p->period = 0x183B; p->loop = 0x18FC; return 1;
        case 0x02E0: p->masks = 0x184C; p->period = 0x183B; p->loop = 0x1873; p->loopinc = 4; return 1;
        case 0x0250: p->masks = 0x184C; p->period = 0x183B; p->loop = 0x18FC; p->stream = 1; return 1;
        case 0x1C4C: case 0x40AE: p->masks = 0x18A4; p->period = 0x186B; return 1;
    }
    return 0;
}

/* One 48-nibble channel for n ticks.  a: its bit in the active mask at the
 * start, st: 1 if its start bit rose since the last tick (applies to the
 * first tick only), sp: its stop bit (constant for the fill), lp: its loop
 * flag.  Per tick, as OneBus_VT369.cpp does for every channel:
 * active &= ~stop; start ? address = start address; active ? play one
 * nibble (an $FF frame header ends or loops the sample); active |= start.
 * Returns the active bit at the end.
 *
 * Starts are edges, as in the cart's own sound program ($40AE ORs
 * $18A4 & ~(its copy of the last $18A4) into $18A5).  The reference re-applies a
 * held start bit on every tick; that is harmless there because the games
 * clear it one tick later, after seeing the active bit, but a fill here is
 * 128 ticks, and holding the start for all of them restarted the channel
 * every tick and lost the 25-byte frame alignment: Jewel Master's sound
 * effects played PRG as noise (s.84). */
VT369_HOT
static u32 vt369_run_48(u32 ch, u32 a, u32 st, u32 sp, u32 lp, const vt369_prog *p,
                        const u8 *prg, u32 pmask, s32 *mix, u32 n)
{
    u8 *d = &SR(0x1800 + ch * 8);
    u8 *cur = &SR(0x1830 + ch * 4);
    const u8 *start = &SR(0x1860 + ch * 4);
    u8 *scur = &SR(0x1D21 + ch * 2);
    const u8 *ring = &SR(0x1900 + ch * 0x100);
    const u32 stream = p->stream;
    u32 lead = d[0], frame = d[1], vol = d[2] & 0x7F, pos = d[5];
    s32 last = (s8)d[3], out = (s8)d[4];
    u32 wa = cur[0] | cur[1] << 8 | cur[2] << 16;
    const u32 swa0 = *scur;
    u32 swa = swa0;
    int wa_dirty = 0;
    for (u32 t = 0; t < n; t++) {
        if (sp) a = 0;
        if (st) { wa = start[0] | start[1] << 8 | start[2] << 16; wa_dirty = 1; }
        if (a) {
            u32 w = stream ? swa : wa;
            if (pos == 48) {
                if (stream) { lead = ring[w++ & 0xFF]; frame = ring[w++ & 0xFF]; }
                else { lead = prg[w++ & pmask]; frame = prg[w++ & pmask]; }
                if (lead == 0xFF) {
                    if (lp) {
                        wa = start[0] | start[1] << 8 | start[2] << 16; wa_dirty = 1;
                        last = 0; out = 0; pos = 48;
                    } else
                        a = 0;
                    goto next;
                }
            } else if (!(pos & 1)) {
                frame = stream ? ring[w++] : prg[w++ & pmask];
            }
            if (stream) swa = w & 0xFF;
            else { wa = w; wa_dirty = 1; }
            {   /* vt369_decode */
                u32 q = pos;                                /* pos % 48 */
                if (q >= 48) q = (q == 48) ? 0 : q % 48;
                const u32 nib = (q & 1) ? frame >> 4 & 0xF : frame & 0xF;
                const int index = (int)lead - ((q >= 24 && (lead & 0x40)) ? 1 : 0) + ((q >= 24 && (lead & 0x80)) ? 2 : 0);
                const int step = vt369_step[index & 0xF][nib];
                s32 o;
                switch (lead >> 4 & 3) {
                    case 0:  o = step; break;
                    case 1:  o = step + out; break;
                    case 2:  o = step + out * 2 - last; break;
                    default: o = step + out - (last >> 1); break;
                }
                last = out;
                out = (s8)o;
                pos = q + 1;
                mix[t] += out * (s32)vol;
            }
        }
next:
        if (st) { a = 1; st = 0; }
    }
    d[0] = (u8)lead; d[1] = (u8)frame; d[3] = (u8)last; d[4] = (u8)out; d[5] = (u8)pos;
    if (wa_dirty) { cur[0] = (u8)wa; cur[1] = (u8)(wa >> 8); cur[2] = (u8)(wa >> 16); }
    if (swa != swa0) *scur = (u8)swa;
    return a;
}

/* One fill of n ticks into mix[] (zeroed by the caller).  Returns 0 when
 * no channel played, so mix[] is still all zero. */
static u32 vt369_render(s32 *mix, u32 n)
{
    u32 played = 0;
    const u32 rv = vt369_reset_vector();
    if (rv == 0x0293) {
        vt369_cmd_0293();
        for (u32 ch = 0; ch < 3; ch++)
            if (SR(0x1CB0 + ch)) { vt369_run_0293(ch, mix, n); played = 1; }
        return played;
    }
    vt369_prog p;
    if (!vt369_prog_for(rv, &p)) { vt369_snd_dbg[2]++; return 0; }
    const s16 per = (s16)(SR(p.period) | SR(p.period + 4) << 8);
    vt369_snd_n = (u16)-per;
    SR(0x18F6) = (u8)(SR(0x18F6) + n);               /* timer IRQ counter */
    const u32 stl = SR(p.masks), sp = SR(p.masks + 2);
    const u32 st = stl & ~vt369_st_prev;             /* rising edges */
    vt369_st_prev = (u8)stl;
    u32 act = SR(p.masks + 1), fin = act;
    const u8 *prg = vt369_prg_base();
    const u32 pmask = vt369_prg_mask();
    for (u32 ch = 0; ch < 4; ch++) {
        const u32 b = 1u << ch;
        const u32 a0 = (act & b) != 0, s0 = (st & b) != 0, k0 = (sp & b) != 0;
        if (!a0 && !s0) continue;                    /* idle all fill long */
        if (k0 && !s0) { fin &= ~b; continue; }       /* stopped at the first tick */
        const u32 lp = p.loop ? SR(p.loop + ch * p.loopinc) != 0 : 0;
        const u32 a = vt369_run_48(ch, a0, s0, k0, lp, &p, prg, pmask, mix, n);
        fin = a ? (fin | b) : (fin & ~b);
        played = 1;
    }
    if (n)                                           /* unused bits, as the reference */
        fin = (fin & 0x0F) | (((act & ~sp) | st) & (n > 1 ? ~sp : 0xFF) & 0xF0);
    SR(p.masks + 1) = (u8)fin;
    return played;
}

#ifndef VT369_SND_HOSTTEST
/* ---- output ------------------------------------------------------------ */

static void vt369_snd_rate(void)
{
    u32 n = vt369_snd_n ? vt369_snd_n : 768;
    /* one tick = n/6 CPU cycles at 1.789773 MHz = n * 16777216 / 10738636 GBA cycles */
    u32 gba = (n * 25597u + 8192) >> 14;                 /* n * 1.562317 */
    if (gba < 256) gba = 256;                             /* <= 65 kHz */
    REG16(0x04000100) = (u16)(0x10000 - gba);             /* TM0 reload */
}

/* PocketNES's idle-DMC path stops timer 0 on every timer1interrupt; the
 * first version restarted it only after rendering, and the stall stretched
 * every 128-sample block by ~5% (Table Soccer's stream played at 7580 Hz
 * instead of 8008).  Restarted first thing now: the loss is the few hundred
 * cycles between the two, ~0.1%. */
static void vt369_snd_timer(void)
{
    if (!(REG16(0x04000102) & 0x80)) {
        vt369_snd_rate();
        REG16(0x04000102) = 0x80;
    }
}

/* EWRAM stack (vt369_snd_fill_stacked, mapVT.s), from vt_adpcm_mix_gba at
 * the end of timer1interrupt: the next 128 samples. */
EWRAM_BSS s32 vt369_mix[128];

/* mix[] -> signed 8-bit FIFO samples; leaves mix[] zeroed for the next
 * fill. */
VT369_FAST
static void vt369_snd_convert(s32 *mix, s8 *out, s32 gain)
{
    for (u32 i = 0; i < 128; i++) {
        s32 v = (mix[i] * gain) >> 9;
        mix[i] = 0;
        out[i] = (s8)(v > 127 ? 127 : v < -128 ? -128 : v);
    }
}

void vt369_snd_fill(void)
{
    vt369_snd_timer();
    vt369_snd_dbg[0]++;
    SR(0x1FF5) = 0x01;                                    /* "initialised" */
    const u32 b = vt369_pcm_next;
    s8 *pcm = vt369_pcm[b];
    const u32 played = vt369_render(vt369_mix, 128);      /* vt369_mix is zero here */
    vt369_snd_rate();                                     /* from the next overflow */
    if (played) {
        vt369_snd_convert(vt369_mix, pcm,
                          vt369_reset_vector() == 0x0293 ? VT369_SND_GAIN3 : VT369_SND_GAIN48);
        const u32 g = (u8)pcm[127] * 0x01010101u;         /* the guard holds the last sample */
        u32 *o = (u32 *)&pcm[128];
        for (int i = 0; i < VT369_PCM_GUARD / 4; i++) o[i] = g;
        vt369_pcm_silent[b] = 0;
    } else if (!vt369_pcm_silent[b]) {                    /* all channels idle: 0 */
        u32 *o = (u32 *)pcm;
        for (int i = 0; i < (128 + VT369_PCM_GUARD) / 4; i++) o[i] = 0;
        vt369_pcm_silent[b] = 1;
    }
    vt369_snd_dbg[1] += 128;
    REG32(0x040000C8) = (u32)pcm;                         /* DMA2 SAD: played from the next restart */
    vt369_pcm_next = (u8)(b ^ 1);
}

/* $4162 */
void vt369_snd_ctl(u32 val)
{
#ifdef VT369_SND_OFF
    val = 0;                       /* experiment switch: never take DirectSound B */
#endif
    const u8 on = (val == 0x0D);
    if (on == vt369_snd_on) return;
    vt369_snd_on = on;
    if (on) {
        u32 *o = (u32 *)vt369_pcm;
        for (u32 i = 0; i < sizeof vt369_pcm / 4; i++) o[i] = 0;
        vt369_pcm_silent[0] = vt369_pcm_silent[1] = 1;
        for (int c = 0; c < 3; c++) vt369_adpcm_left[c] = 0;
        REG32(0x040000C8) = (u32)vt369_pcm[0];           /* DMA2 SAD: silence first */
        vt369_pcm_next = 1;
        vt369_snd_timer();
    } else {
        REG32(0x040000C8) = 0x05000280;                  /* DMA2 SAD: PCMWAV again */
    }
}

#endif

/* $4800-$4FFF writes (write_vt369_4xxx, mapVT.s).  Commands, starts and
 * stops take effect at once, not at the next fill: the games wait for them
 * (Jewel Master sets a start bit and spins on the active bit before
 * clearing it), and a fill is up to 9 ms away.  On the hardware and in the
 * reference the wait is one sound-CPU tick. */
void vt369_snd_write(u32 addr, u32 val)
{
    const u32 a = 0x1800 | (addr & 0x7FF);
    SR(a) = (u8)val;
    if (!vt369_snd_on || (a != 0x1FA2 && (a & 0xFF00) != 0x1800)) return;  /* $1FA2, $18xx */
    const u32 rv = vt369_reset_vector();
    if (rv == 0x0293) {
        if (a == 0x1FA2) vt369_cmd_0293();
        return;
    }
    vt369_prog p;
    if (!vt369_prog_for(rv, &p)) return;
    if (a == p.masks) {                               /* start: the tick's start half */
        const u32 edge = val & ~vt369_st_prev;
        vt369_st_prev = (u8)val;
        for (u32 ch = 0; ch < 4; ch++)
            if (edge & (1u << ch)) {
                SR(0x1830 + ch * 4) = SR(0x1860 + ch * 4);
                SR(0x1831 + ch * 4) = SR(0x1861 + ch * 4);
                SR(0x1832 + ch * 4) = SR(0x1862 + ch * 4);
            }
        SR(p.masks + 1) |= (u8)edge;
    } else if (a == p.masks + 2) {                    /* stop */
        SR(p.masks + 1) &= (u8)~val;
    }
}

void vt369_snd_reset(void)
{
    extern u8 vt369_gpio_mask[4], vt369_gpio_latch[4];
    for (int i = 0; i < 4; i++) vt369_gpio_mask[i] = vt369_gpio_latch[i] = 0;
    vt1682_tab_init();
#ifndef VT369_SND_HOSTTEST
    for (int i = 0; i < 128; i++) vt369_mix[i] = 0;
#endif
    vt369_snd_on = 0;
    vt369_st_prev = 0;
    for (int i = 0; i < 0x800; i++) vt369_sram[i] = 0;
    vt369_snd_n = 0;
}

#endif
