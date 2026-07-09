/*
 * vt_regs.c -- VT03/VT09 extended register emulation for PocketVT
 *
 * Handles the $4100-$41FF extended register bank that exists on VT-series
 * "NES-on-a-chip" hardware.  This file contains:
 *
 *   - vt_reset()          power-on/reset initialisation
 *   - vt_reg_write()      dispatcher for all $4100-$41FF writes
 *   - vt_reg_read()       dispatcher for all $4100-$41FF reads
 *   - vt09_decode_opcode  optional XOR decryption for VT09 CPU fetch
 *   - vt_adpcm_tick()     IMA-ADPCM sample mixer (two channels)
 *   - vt_adpcm_mix_gba()  ADPCM mixing into the GBA DirectSound buffer
 *
 * Architecture notes
 * ------------------
 * The write hook (writemem_4) is installed by mapVT_init (in mapVT.s).
 * It calls vt_reg_write(addr & 0xFF, val) for any bus write in $4100-$41FF.
 * Reads below $4120 are handled inline by vt_reg_read(); the palette RAM
 * ($4140-$417F) and hi-res OAM ($4180-$41FF) are forwarded to ppu_vt.c.
 */

#include "includes.h"
#include "vt_regs.h"
#include "ppu_vt.h"
#include <stddef.h>     // offsetof

// Forward declaration -- definition is in the MMC3 forwarding section
// further down.  vt_reset() calls it to clear the saved $8000 cmd byte
// on every cartridge reload.
static void vt_mmc3_reset(void);

// ---------------------------------------------------------------------------
// Static assertions: 6502_vt.s hardcodes the byte offset of the encryption
// fields inside VTState.  If the struct layout changes, the assembly is
// silently wrong, so lock the offsets down at compile time.
// ---------------------------------------------------------------------------
#if VT09_ENCRYPTION
_Static_assert(offsetof(VTState, encryption_active)  == 0x20,
               "VTState.encryption_active offset must be 0x20 (see 6502_vt.s)");
_Static_assert(offsetof(VTState, encryption_pending) == 0x21,
               "VTState.encryption_pending offset must be 0x21 (see 6502_vt.s)");
_Static_assert(offsetof(VTState, encryption_next)    == 0x22,
               "VTState.encryption_next offset must be 0x22 (see 6502_vt.s)");
#endif

// ---------------------------------------------------------------------------
// IMA-ADPCM step-size table (standard 89-entry table -- placeholder)
// TODO: replace with the VT369 16x16 step table from ADPCM_VT369.cpp.
// ---------------------------------------------------------------------------
static const s16 ima_step_table[89] = {
     7,    8,    9,   10,   11,   12,   13,   14,
    16,   17,   19,   21,   23,   25,   28,   31,
    34,   37,   41,   45,   50,   55,   60,   66,
    73,   80,   88,   97,  107,  118,  130,  143,
   157,  173,  190,  209,  230,  253,  279,  307,
   337,  371,  408,  449,  494,  544,  598,  658,
   724,  796,  876,  963, 1060, 1166, 1282, 1411,
  1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
  3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
  7132, 7845, 8630, 9493,10442,11487,12635,13899,
  15289,16818,18500,20350,22385,24623,27086,29794,
    32767
};

static const s8 ima_index_table[16] = {
    -1, -1, -1, -1,  2,  4,  6,  8,
    -1, -1, -1, -1,  2,  4,  6,  8
};

// ---------------------------------------------------------------------------
// Global VT state (EWRAM)
// ---------------------------------------------------------------------------
EWRAM_BSS VTState vt;

// Runtime flag.  Set by mapVTinit so the PPU $2008+ divert in ppu.s only
// fires for VT cartridges, leaving the standard NES register-mirror path
// intact for non-VT mappers.
EWRAM_BSS u8 vt_active = 0;

// CHR bank shadow registers ($2012-$2017 in PPU space, plus $2016/$2017).
EWRAM_BSS u8 vt_chr_reg[6];      

// Extra CHR/PPU bank registers -- see vt_regs.h for layout.
EWRAM_BSS u8 vt_chr_reg_2018;
EWRAM_BSS u8 vt_chr_reg_201A;
EWRAM_BSS u8 vt_chr_outer_4100;

// Array tracking target PRG assignments to be handled safely in ASM
u8 vt_prg_banks[4];
u8 vt_prg_dirty = 0;
// Set when the nametable arrangement changed; consumed by write_vt4xxx, which
// calls vt_set_mirroring() (cart.s) from ARM context with a valid stack.
EWRAM_BSS u8 vt_mirror_dirty;
EWRAM_BSS u8 vt_mirror_value;

// ---------------------------------------------------------------------------
// VT03 enhanced DMA control ($4034)
// ---------------------------------------------------------------------------
// VT03 datasheet, p.37:
//   $4014 W -- High byte of source address. ($XX[X]0)
//   $4034 W -- Settings:
//     bit 0     SEL47  0 = sprite DMA -> $2004, 1 = video DMA -> $2007
//     bits 1-3  length (000=256, 100=16, 101=32, 110=64, 111=128)
//     bits 4-7  source address mid nibble ($XX[X]0)
//
// Writing $4014 starts the DMA.  Real silicon copies (length) bytes from
// CPU source ($4014<<8 | $4034&0xF0) to either $2004 (sprite OAM) or $2007
// (PPU memory -- typically palette uploads at $3F00).
//
// PocketNES's stock _4014w in ppu.s ignores $4034 and always does the
// 256-byte sprite DMA.  We track $4034 here and the modified $4014 path
// in mapVT.s (write_vt4xxx) dispatches video-DMA via vmdata_W when bit 0
// is set.  Default 0 = sprite DMA (matches power-on behavior).
//
// Lonely Island writes $4034=$0F then $4014=$04 at $E0D0 to push palette
// data from CPU $0400-$047F to PPU $3F00 (128-byte video DMA).  Without
// this fix the palette never gets initialized off the all-$3F boot value,
// producing a black screen even though the rest of the game runs fine.
u8 vt_dma_settings = 0;

// Index helper for vt_chr_reg[] from the raw $20xx low byte.
static inline int vt_chr_reg_index(u8 addr_lo)
{
    if (addr_lo >= 0x12 && addr_lo <= 0x17) return addr_lo - 0x12;
    return -1;
}

// ---------------------------------------------------------------------------
// Power-on / reset
// ---------------------------------------------------------------------------
#include <stddef.h>
EWRAM_BSS u8 vt_timer_render_seen = 0;
EWRAM_BSS u8 vt_timer_armed = 0;
_Static_assert(offsetof(VTState, timer_period)  == 0x68, "sound.s vt_timer asm uses +0x68");
_Static_assert(offsetof(VTState, timer_ctrl)    == 0x6C, "sound.s vt_timer asm uses +0x6C");
_Static_assert(offsetof(VTState, want_timer_irq)== 0x6D, "sound.s vt_timer asm uses +0x6D");
/* ppuctrl1 shadow, derived from a linked symbol so it survives globalptr
   layout drift between tree generations (a hardcoded base already burned
   us twice).  Offsets within the globalptr block: wantirq=+0x420,
   ppuctrl1=+0x45A (stable, defined by the _m_ list in equates.h). */
extern u8 _wantirq;
#define VT_PPUCTRL1_SHADOW (*((volatile u8 *)&_wantirq - 0x420 + 0x45A))

__attribute__((target("arm")))
void vt_timer_tick_frame(void)
{
    /* Per-frame supervisor, called from the vblank handler in ppu.s: ONLY
       maintains the boot-safety latch.  The actual VT timer lives on the
       cycle-accurate timeout system (vt_timer_handler in sound.s) so its
       IRQ has correct MID-FRAME phase -- LI's handler is its raster engine,
       and a vblank-phased IRQ forced stale $2000/$2001 shadows every
       frame, blanking the picture. */

    // Session-11: keep the wait-loop speed hack armed.  cpuhack_reset (and
    // any encryption-mode rebuild) restores stock handlers, wiping the hack
    // seeded at vt_reset; set_cpu_hack early-outs when the hack is already
    // current, so re-arming every 60 frames is idempotent and free.
    if (vt_active && speedhacks[1].hack_pc) {
        // Every frame: speedhack_manager's not-used heuristic periodically
        // uninstalls the hack (the 2.0/2.9 frame oscillation); set_cpu_hack
        // early-outs when already current, so this is ~free when armed.
        set_cpu_hack(1);
    }
    if (!vt_timer_render_seen) {
        if (VT_PPUCTRL1_SHADOW & 0x18) vt_timer_render_seen = 1;
    }
}

void vt_reset(void)
{
    // Preserve submapper across reset -- loadcart.c sets it from the iNES
    // header before mapVTinit runs.
    u8 saved_submapper = vt.submapper;

    memset(&vt, 0, sizeof(VTState));
    vt.submapper = saved_submapper;

    vt_timer_render_seen = 0;
    vt_timer_armed       = 0;

    /* Clear any IRQ lines pended before this cart took over (the ROM menu
       runs the APU frame sequencer with vt_active=0, so a 2A03 frame IRQ
       asserted there sticks -- observed _wantirq==0x40 at cart runtime.
       A VT game cannot ack it ($4015 is never read), so the first CLI
       would storm.  Fresh cart = no pending interrupts. */
    {   extern u8 _wantirq;
        _wantirq = 0;
    }

    // ADPCM channels default period = 0x006F (matches Furbtendulator APU_VT32)
    vt.adpcm[0].period       = 0x006F;
    vt.adpcm[1].period       = 0x006F;
    vt.adpcm[0].rate_counter = 0x006F;
    vt.adpcm[1].rate_counter = 0x006F;
    vt.adpcm[0].step_size = (u8)ima_step_table[0];
    vt.adpcm[1].step_size = (u8)ima_step_table[0];

    // Default clock control: normal speed (no *3 CPU, no half-speed PPU)
    vt.reg[VT_REG_CLKCTRL]  = 0x00;
    vt.reg[VT_REG_SYSCTRL2] = 0x00;
    vt.reg[VT_REG_SYSCTRL3] = 0x00;

    // --- Hard-reset register defaults from Furbtendulator h_OneBus.cpp::reset()
    //
    // Skipping these causes the visible "flat gray screen then reset loop"
    // because all the initial PRG banks map wrong.
    vt.reg[0x00] = 0x00;          // PA21 high-bit overlay
    vt.reg[0x05] = 0x00;          // COMR6 / COMR7
    vt.reg[0x07] = 0x00;          // PQ0  -> $8000-$9FFF
    vt.reg[0x08] = 0x01;          // PQ1  -> $A000-$BFFF
    vt.reg[0x09] = 0xFE;          // PQ2  -> $C000-$DFFF when PQ2EN, else fixed 0xFE
    vt.reg[0x0A] = 0x00;          // PA8 / PQ3
    vt.reg[0x0B] = 0x00;          // PS / PQ2EN -- PS=0 means prgAND=0x3F
    vt.reg[0x0F] = 0xFF;          // Security register reset value

    // CHR bank shadow defaults (mirror reg2000[0x12..0x17] in Furb).
    vt_chr_reg[0] = 0x04;         // $2012
    vt_chr_reg[1] = 0x05;         // $2013
    vt_chr_reg[2] = 0x06;         // $2014
    vt_chr_reg[3] = 0x07;         // $2015
    vt_chr_reg[4] = 0x00;         // $2016
    vt_chr_reg[5] = 0x02;         // $2017

    vt_chr_reg_2018   = 0x00;
    vt_chr_reg_201A   = 0x00;
    vt_chr_outer_4100 = 0x00;

    // OneBus mapper-256 opcode encryption.
    vt.encryption_mode    = vt.submapper;   // 0 if pre-NES2.0
    vt.encryption_active  = (vt.submapper >= 12);
    vt.encryption_pending = false;
    vt.encryption_next    = false;

    // Permute op_table per current encryption state so the 6502 fetch
    // path dispatches encrypted bytes to the right handlers without any
    // changes to the asm-side fetch macro.  See vt_rebuild_optable below.
    vt_rebuild_optable();

    vt_mmc3_reset();

    // Recompute PRG banks internally
    vt_recompute_prg_banks();

    // After the CHR bank defaults are set, do the initial 8KB CHR-from-PRG
    // sync so the first frame the game tries to render has something to
    // pull tile data from.
    vt_chr_sync_from_prg();

    // ------------------------------------------------------------------
    // Session-11 perf: hand-seed the speed hack for the game's vblank
    // wait loop.  Profiling showed ~60% of host time in the canonical
    // _C5/_F0 handlers -- the guest spinning in LDA $26/CMP $26/BEQ at
    // $E094 (fixed bank, hosted in VRAM at 0x0600E094).  The general
    // quickhackfinder pattern-matches raw bytes and can't be trusted
    // under opcode encryption, but THIS loop is provably safe: its
    // branch byte $F0 is invariant under the bit5<->6 swap and the loop
    // has no side effects.  Verify the exact raw signature and install
    // via set_cpu_hack (now encryption-aware: it refuses non-invariant
    // rows and re-homes the default BNE hack to op_table[$B0]).
    // Loop cost ~9 cycles/iteration; num_incs=0 (pure wait).
    {
        static const u8 sig[6] = {0xC5,0x26,0xA5,0x26,0xF0,0xFC};
        const volatile u8 *fb = (const volatile u8 *)0x0600E094u;
        int match = 1;
        for (int i = 0; i < 6; i++) if (fb[i] != sig[i]) { match = 0; break; }
        if (match) {
            speedhacks[1].hack_pc = (const u8 *)0x0600E098u;  // the BEQ
            speedhacks[1].num_incs = 0;
            speedhacks[1].hack_was_used = 0;
            speedhacks[1].frames_hack_not_used = 0;
            speedhacks[1].cycles_per_iteration = 9;
            speedhacks[1].divider = 1;
            set_cpu_hack(1);
        }
    }
}

// ---------------------------------------------------------------------------
// OneBus dynamic PRG bank mapping
// ---------------------------------------------------------------------------
// Real VT chips don't have fixed bank routing -- the four 8KB windows at
// $8000/$A000/$C000/$E000 are recomputed every time the page-size register
// ($410B), the COMR6 control ($4105 bit 6), the high-bit overlay ($4100,
// $410A), or any of the bank-select registers ($4107-$4109) changes.
//
// This decoupled design sets up the banking map within C array vt_prg_banks
// and alerts ASM by setting vt_prg_dirty. This safely avoids AAPCS calling
// convention issues corrupting m6502_pc (`r9`) and m6502_mmap (`r4`) across
// the bridge.

static u32 vt_get_phys_bank(u8 bnk)
{
    u8  ps      = vt.reg[0x0B] & 0x07;
    u32 prgAND  = (ps == 7) ? 0xFF : (u32)(0x3F >> ps);
    u32 pq3     = vt.reg[0x0A];
    u32 pa21    = (vt.reg[0x00] >> 4) & 0x0F;
    u32 prgOR   = (pq3 | (pa21 << 8)) & ~prgAND;
    return ((u32)bnk & prgAND) | prgOR;
}

// ---------------------------------------------------------------------------
// PRG window resync (matches h_OneBus.cpp::syncPRG).
//
// Real OneBus silicon arranges the four 8KB windows under three controls:
//   - PQ2EN ($410B bit 6): if 0, $C000 fixes to 0xFE; if 1, $C000 reads PQ2
//   - COMR6 ($4105 bit 6): if 1, slot 8 and slot C swap destinations
//                          (the windows literally trade addresses)
//   - prgAND / prgOR:      driven by PS / PQ3 (see vt_get_phys_bank above)
//
void vt_recompute_prg_banks(void)
{
    bool pq2en = (vt.reg[0x0B] & 0x40) != 0;
    bool comr6 = (vt.reg[0x05] & 0x40) != 0;

    // Per Furb: flip swaps slots 0x8<->0xC.
    u32 bank_pq0 = vt_get_phys_bank(vt.reg[0x07]);
    u32 bank_pq1 = vt_get_phys_bank(vt.reg[0x08]);
    u32 bank_pq2 = vt_get_phys_bank(pq2en ? vt.reg[0x09] : 0xFE);
    u32 bank_eff = vt_get_phys_bank(0xFF);

    if (comr6) {
        vt_prg_banks[0] = (u8)bank_pq2;     // slot 8 receives what would have gone to slot C
        vt_prg_banks[2] = (u8)bank_pq0;     // slot C receives what would have gone to slot 8
    } else {
        vt_prg_banks[0] = (u8)bank_pq0;
        vt_prg_banks[2] = (u8)bank_pq2;
    }
    vt_prg_banks[1] = (u8)bank_pq1;
    vt_prg_banks[3] = (u8)bank_eff;

    vt_prg_dirty = 1;
}

// ---------------------------------------------------------------------------
// MMC3 compatibility forwarder
// ---------------------------------------------------------------------------
// Per the NESdev wiki article "VT02+ MMC3 Compatibility Registers":
//
//     "Backwards compatibility is realized by way of forwarding CPU writes
//      to $8000-$FFFF to the appropriate VT02+ register."
//
// Without forwarding these writes, games that expect MMC3 bank routing
// hang immediately after boot jumping into unmapped ROM locations.

// Last byte written to $8000 -- selects which entry $8001 modifies.
static u8 vt_mmc3_cmd;

static void vt_mmc3_reset(void)
{
    vt_mmc3_cmd = 0;
}

void vt_mmc3_forward(u16 addr, u8 val)
{
    // FWEN: if $410B bit 3 is set, the silicon stops forwarding.  This is
    // how multicarts like "Classic Max Lite 120-in-1" stop e.g. Bolt Fighter
    // from clobbering bank registers with garbage during gameplay.
    if (vt.reg[0x0B] & 0x08) return;

    switch (addr & 0xE001) {
        case 0x8000:
            // MMC3 cmd register.  Save the byte; the high bits also feed
            // into VT's $4105 (which holds COMR6 etc.).
            vt_mmc3_cmd = val;
            vt.reg[0x05] = (vt.reg[0x05] & 0x20) | (val & 0xDF);
            // COMR6 (bit 6) flip is a PRG bank rearrangement -> recompute.
            vt_recompute_prg_banks();
            break;

        case 0x8001:
            // MMC3 data register.  We currently only act on the two PRG
            // bank cases (cmd 6 and 7).
            switch (vt_mmc3_cmd & 0x07) {
                case 0:
                    if (vt_chr_reg[4] != val) { vt_chr_reg[4] = val; vt_chr_sync_from_prg(); }
                    break;
                case 1:
                    if (vt_chr_reg[5] != val) { vt_chr_reg[5] = val; vt_chr_sync_from_prg(); }
                    break;
                case 2:
                    if (vt_chr_reg[0] != val) { vt_chr_reg[0] = val; vt_chr_sync_from_prg(); }
                    break;
                case 3:
                    if (vt_chr_reg[1] != val) { vt_chr_reg[1] = val; vt_chr_sync_from_prg(); }
                    break;
                case 4:
                    if (vt_chr_reg[2] != val) { vt_chr_reg[2] = val; vt_chr_sync_from_prg(); }
                    break;
                case 5:
                    if (vt_chr_reg[3] != val) { vt_chr_reg[3] = val; vt_chr_sync_from_prg(); }
                    break;
                case 6:
                    vt.reg[0x07] = val;
                    vt_recompute_prg_banks();
                    break;
                case 7:
                    vt.reg[0x08] = val;
                    vt_recompute_prg_banks();
                    break;
            }
            break;

        case 0xA000:
            // Mirroring, via the MMC3 compatibility register.  Same bit-0
            // encoding as $4106 above.  (Lonely Island drives $4106 directly
            // and never touches $A000, but other VT carts do.)
            if ((vt.reg[0x06] ^ val) & 0x01) {
                vt_mirror_value = val & 0x01;
                vt_mirror_dirty = 1;
            }
            vt.reg[0x06] = (vt.reg[0x06] & ~0x01) | (val & 0x01);
            break;

        case 0xA001:
            // WRAM enable / write-protect. VT has no $6000 WRAM by default.
            break;

        // MMC3 IRQ Registers. They map exactly to the underlying VT Timer Registers.
        case 0xC000: vt_reg_write(VT_REG_TIMER_LO, val); break;
        case 0xC001: vt_reg_write(VT_REG_TIMER_HI, val); break;
        case 0xE000: vt_reg_write(VT_REG_TIMER_CTRL, val); break;
        case 0xE001: vt_reg_write(0x04, val); break;
    }
}

// ---------------------------------------------------------------------------
// VT09 opcode decryption
// ---------------------------------------------------------------------------
// The VT09 CPU fetches opcodes through a byte-wide XOR gate wired to 0xA1.
// The gate is enabled/disabled by a flip-flop that is set by writes to
// $410F and actually committed on the next JMP/JMPI instruction.
// See Furbtendulator CPU_VT32::GetOpcode / IN_JMP for the reference impl.

void vt09_set_encryption(bool enable)
{
#if VT09_ENCRYPTION
    vt.encryption_pending = true;
    vt.encryption_next    = enable;
#endif
}

// Bit-permutation per Furb CPU_OneBus::Unscramble (OneBus.cpp:47-73).
// Only the OPCODE byte goes through this; operand bytes are NOT permuted.
u8 vt09_decode_opcode(u8 raw)
{
#if VT09_ENCRYPTION
    if (!vt.encryption_active) return raw;
    switch (vt.encryption_mode) {
        case 12: {
            // bit-swap 6<->7 and 1<->2, mask out 0xC6
            u8 r = raw & (u8)~0xC6;
            if (raw & 0x40) r |= 0x80;
            if (raw & 0x80) r |= 0x40;
            if (raw & 0x02) r |= 0x04;
            if (raw & 0x04) r |= 0x02;
            return r;
        }
        case 13: {
            // bit-swap 1<->4
            u8 r = raw & (u8)~0x12;
            if (raw & 0x10) r |= 0x02;
            if (raw & 0x02) r |= 0x10;
            return r;
        }
        case 14: {
            // bit-swap 6<->7
            u8 r = raw & (u8)~0xC0;
            if (raw & 0x80) r |= 0x40;
            if (raw & 0x40) r |= 0x80;
            return r;
        }
        default: { 
            // sub-mapper 15 (and any other >=12 not enumerated above):
            // bit-swap 5<->6
            u8 r = raw & (u8)~0x60;
            if (raw & 0x40) r |= 0x20;
            if (raw & 0x20) r |= 0x40;
            return r;
        }
    }
#else
    return raw;
#endif
}

// ---------------------------------------------------------------------------
// vt_rebuild_optable()
//
// To implement VT369 opcode encryption *without* touching the (very hot,
// flag-sensitive) `fetch` macro in 6502mac.h, we permute the table itself
// whenever encryption state changes.
//
// __attribute__((target("arm"))) is required because this function is now
// called from ARM-mode assembly in speedhack_asm.s::cpuhack_reset via
// `bl_long`, which expands to `mov lr,pc; ldr pc,=label`.
// Note: op_table is declared in asmcalls.h as extern void* op_table[256]
// EWRAM_BSS: this 1KiB snapshot is read only during table rebuilds (reset +
// encryption-mode commits), far too cold to justify IWRAM.  Evicting it
// gives the user stack the headroom it lost when the mem.s end-block was
// pulled back inside the 32KiB boundary (deep menu call chains reach ~0x140
// below the stack top and were popping IWRAM_CANARY_2 as a return address).
EWRAM_BSS static void *vt_optable_canonical[256];
static u8    vt_optable_saved = 0;

__attribute__((target("arm")))
void vt_rebuild_optable(void)
{
    // Snapshot the pristine table on first call.
    if (!vt_optable_saved) {
        for (int i = 0; i < 256; ++i) vt_optable_canonical[i] = op_table[i];
        vt_optable_saved = 1;
    }

    // Fast path: encryption off -> identity copy.
    if (!vt.encryption_active) {
        for (int i = 0; i < 256; ++i) op_table[i] = vt_optable_canonical[i];
        return;
    }

    // Encrypted: working_table[encrypted_index] = canonical[decrypted_index]
    for (int i = 0; i < 256; ++i) {
        u8 decrypted = vt09_decode_opcode((u8)i);
        op_table[i] = vt_optable_canonical[decrypted];
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline void _adpcm_decode_nibble(VTAdpcmChan *ch, u8 nibble)
{
    nibble &= 0x0F;

    s16 step = ima_step_table[ch->step_index];
    s32 delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 8) delta = -delta;

    s32 pred = (s32)ch->predictor + delta;
    if (pred >  32767) pred =  32767;
    if (pred < -32768) pred = -32768;
    ch->predictor = (s16)pred;
    ch->pcm_out   = ch->predictor;

    int new_idx = (int)ch->step_index + ima_index_table[nibble];
    if (new_idx <  0) new_idx = 0;
    if (new_idx > 88) new_idx = 88;
    ch->step_index = (u8)new_idx;
    ch->step_size  = (u8)ima_step_table[ch->step_index];
}

// Advance a channel by one sample tick.
static inline s16 _adpcm_advance(VTAdpcmChan *ch)
{
    if (!ch->playing) return 0;

    // Rate counter: decode a new nibble only when it underflows.
    if (ch->rate_counter > 1) {
        ch->rate_counter--;
        return ch->pcm_out;
    }
    ch->rate_counter = ch->period ? ch->period : 1;

    // Fetch next byte from ROM
    extern u8 *rombase; 
    u8 byte_val = rombase[ch->cur_addr & 0x7FFFF];
    u8 nibble   = ch->second_nibble ? (byte_val & 0x0F) : (byte_val >> 4);

    if (ch->second_nibble) {
        ch->cur_addr++;
        // End-of-sample detection.
        if (ch->end_addr && ch->cur_addr >= ch->end_addr) {
            ch->playing = false;
            ch->pcm_out = 0;
            return 0;
        }
    }
    ch->second_nibble = !ch->second_nibble;

    _adpcm_decode_nibble(ch, nibble);
    return ch->pcm_out;
}

// ---------------------------------------------------------------------------
// Register write dispatcher
// ---------------------------------------------------------------------------
void vt_reg_write(u8 addr_lo, u8 val)
{
    extern void vt_timer_install_now(void);
    if (!vt_timer_armed) {          /* first $41xx touch: start the free-run count */
        vt_timer_armed = 1;
        vt_timer_install_now();     /* in-core memory-handler context: race-free */
    }
    // $4100-$411F: system control shadow
    if (addr_lo < 0x20) {
        u8 vt_prev_reg = vt.reg[addr_lo];
        vt.reg[addr_lo] = val;

        // $4106 W: "Horizontal/Vertical Scrolling Selector" (NESdev VT02+
        // Registers).  Bit 0 chooses the nametable arrangement, exactly like
        // MMC3's $A000.  PocketVT used to shadow this byte and do nothing
        // else, so a scene that switched to horizontal scrolling kept the
        // iNES header's arrangement: the game's freshly written columns went
        // into a nametable that was never displayed, and the screen stayed
        // black until the camera scrolled far enough for the one nametable it
        // did have to come back around.  Apply it for real.  The switch has to
        // happen from ARM code with a valid stack, so just flag it here and
        // let write_vt4xxx call vt_set_mirroring() on the way out.
        if (addr_lo == 0x06 && ((vt_prev_reg ^ val) & 0x01)) {
            vt_mirror_value = val & 0x01;
            vt_mirror_dirty = 1;
        }

        // $4100 also holds the outer Video Bank 2 in its low nibble
        if (addr_lo == 0x00) {
            u8 new_outer = val & 0x0F;
            if (vt_chr_outer_4100 != new_outer) {
                vt_chr_outer_4100 = new_outer;
                vt_chr_sync_from_prg();
            }
        }

        // OneBus PRG bank mapping depends on these seven registers.
        switch (addr_lo) {
            case 0x00:                  // PA21 high-bit overlay
            case 0x05:                  // COMR6/COMR7 PRG+CHR layout control
            case 0x07:                  // PQ0 -> $8000-$9FFF
            case 0x08:                  // PQ1 -> $A000-$BFFF
            case 0x09:                  // PQ2 -> $C000-$DFFF when enabled
            case 0x0A:                  // PA8 / PQ3 outer PRG bits
            case 0x0B:                  // PS / FWEN / PQ2EN / TSYNEN
                vt_recompute_prg_banks();
                vt_chr_sync_from_prg();
                break;
            default:
                break;
        }

        switch (addr_lo) {
            case VT_REG_TIMER_LO: // 0x01
                // VT03 datasheet $4101 W: "Preload Times of timer interrupt".
                // Furb h_OneBus.cpp:  case 0x101: reloadValue = val;
                // Just set the preload value.  Don't touch counter, don't
                // start anything.  The counter ticks unconditionally; this
                // is just the value it reloads to when it wraps from 0.
                vt.timer_period = val;
                break;
            case VT_REG_TIMER_HI: // 0x02
                // VT03 datasheet $4102 W: "Load preload timer data and start".
                // Furb h_OneBus.cpp:  case 0x102: counter = 0;
                // The "start to count" phrasing is misleading -- the timer
                // is ALWAYS counting on real hardware; there's no enable bit
                // for the counter itself.  Writing $4102 just zeros the
                // counter so the next tick wraps and reloads from $4101.
                // Crucially this does NOT enable the IRQ -- $4104 is the
                // only thing that does that.
                vt.timer_counter = 0;
                vt_timer_install_now();   /* re-phase: counts from this write */
                break;
            case VT_REG_TIMER_CTRL: { // 0x03
                // VT03 datasheet $4103 W: "Disable the timer interrupt".
                // Furb h_OneBus.cpp:  case 0x103: enableIRQ = false;
                //                                 EMU->SetIRQ(1);
                // Disables IRQ generation AND acks any pending IRQ on the
                // CPU side.  Writing any value triggers both.
                vt.timer_ctrl    &= (u8)~0x02;    // bit1 = IRQ enable cleared
                vt.want_timer_irq = 0;
                extern u8 _wantirq;
                _wantirq         &= (u8)~0x02;   // VT_IRQ_MAPPER
                break;
            }
            case 0x04: // $4104 W "Enable the timer interrupt"
                // Furb h_OneBus.cpp:  case 0x104: enableIRQ = true;
                // The only way to actually arm the timer IRQ on real
                // hardware.  Star Ally relies on this -- it writes
                // $4101 (period) + $4103 (clear) + $4104 (arm) in its
                // timer ISR every fire, and never writes $4102.
                vt.timer_ctrl |= 0x02;
                break;
#if VT09_ENCRYPTION
            case VT_REG_SECURITY:
                // $410F: schedule encryption state change
                vt09_set_encryption(val == 0x00);
                break;
#endif
            default:
                break;
        }
        return;
    }

    // $4120-$412F: ADPCM channel registers
    if (addr_lo >= 0x20 && addr_lo < 0x30) {
        u8 ch_idx  = (addr_lo - 0x20) >> 3; 
        u8 reg_off = (addr_lo - 0x20) & 0x07;
        VTAdpcmChan *ch = &vt.adpcm[ch_idx];

        switch (reg_off) {
            case VT_ADPCM_ADDR_LO:
                ch->start_addr = (ch->start_addr & 0xFFFFFF00) | val;
                break;
            case VT_ADPCM_ADDR_HI:
                ch->start_addr = (ch->start_addr & 0xFFFF00FF) | ((u32)val << 8);
                break;
            case VT_ADPCM_VOLUME:
                ch->volume = val & 0x7F;
                break;
            case VT_ADPCM_FLAGS:
                ch->flags = val;
                if (val & 0x01) {
                    ch->cur_addr      = ch->start_addr;
                    ch->predictor     = 0;
                    ch->step_index    = 0;
                    ch->step_size     = (u8)ima_step_table[0];
                    ch->second_nibble = false;
                    ch->rate_counter  = ch->period ? ch->period : 1;
                    ch->playing       = true;
                } else {
                    ch->playing = false;
                    ch->pcm_out = 0;
                }
                break;
            case VT_ADPCM_PERIOD_LO:
                ch->period = (ch->period & 0xFF00) | val;
                break;
            case VT_ADPCM_PERIOD_HI:
                ch->period = (ch->period & 0x00FF) | ((u16)val << 8);
                break;
            case VT_ADPCM_END_LO:
                ch->end_addr = (ch->end_addr & 0xFFFFFF00) | val;
                break;
            case VT_ADPCM_END_HI:
                ch->end_addr = (ch->end_addr & 0xFFFF00FF) | ((u32)val << 8);
                break;
            default:
                break;
        }
        return;
    }

    // $4130-$413F: hardware ALU
    if (addr_lo >= 0x30 && addr_lo < 0x40) {
        u8 reg_off = addr_lo - 0x30;
        switch (reg_off) {
            case VT_ALU_OP_A_LO:
                vt.alu_operand_a = (vt.alu_operand_a & 0xFFFFFF00) | val;
                break;
            case VT_ALU_OP_A_HI:
                vt.alu_operand_a = (vt.alu_operand_a & 0xFFFF00FF) | ((u32)val << 8);
                break;
            case VT_ALU_OP_B_LO:
                vt.alu_operand_b = (vt.alu_operand_b & 0xFFFFFF00) | val;
                vt.alu_result = (u32)((s16)vt.alu_operand_a * (s16)vt.alu_operand_b);
                break;
            case VT_ALU_OP_B_HI:
                vt.alu_operand_b = (vt.alu_operand_b & 0xFF00) | ((u32)val << 8);
                break;
            default:
                break;
        }
        return;
    }

    if (addr_lo == 0x69) {
        if (vt.encryption_mode == 13 || vt.encryption_mode == 15) {
            u8 new_active = (val & 1) ? 0 : 1;
            if (new_active != vt.encryption_active) {
                vt.encryption_active = new_active;
                vt_rebuild_optable();
            }
        }
        return;
    }

    if (addr_lo >= 0x40 && addr_lo < 0x80) {
        vt_palette_write(addr_lo - 0x40, val);
        return;
    }

    if (addr_lo >= 0x80) {
        vt_oam_ext_write(addr_lo - 0x80, val);
        return;
    }
}

u8 vt_reg_read(u8 addr_lo)
{
    if (addr_lo < 0x20) {
        // $4119 read = RS232 Flags (NESdev "VT02+ Registers").  Only two of
        // its bits describe the console rather than the serial port:
        //   bit 3 XPORN   (0 = NTSC, 1 = PAL)
        //   bit 4 XF5OR6  (0 = 60 Hz, 1 = 50 Hz)
        // Lonely Island's NMI palette task ($E0B1) reads this and tests
        // AND #$18 -- i.e. "am I on a PAL/50Hz machine?" -- to pick which of
        // two palette-upload routines to run.  The routines differ in where
        // the 128-byte DMA lands: $3F00 (both bits set) or $3F01 (clear).
        // Returning 0 here sent every entry one slot late, so backgrounds
        // sampled colours meant for other pixel values -- the grass came out
        // pastel mint instead of green, and the whole overworld looked
        // "glitched".  The board these carts ship on is a PAL/50Hz VT03
        // (which is also why the reference capture matches the $3F00 upload),
        // so report that.  $E0B1 is the ONLY reader of $4119 in this ROM, so
        // nothing else is affected.
        if (addr_lo == 0x19) return 0x18;   // XPORN | XF5OR6
        if (addr_lo == VT_REG_TIMER_CTRL) { // 0x03
            u8 status = vt.timer_ctrl & 0x7F; 
            if (vt.want_timer_irq) status |= 0x02;
            return status;
        }
        if (addr_lo >= (u8)(VT_ALU_BASE - VT_REG_BASE + VT_ALU_RESULT_0) &&
            addr_lo <  (u8)(VT_ALU_BASE - VT_REG_BASE + VT_ALU_RESULT_0 + 4)) {
            u8 byte_idx = addr_lo - (u8)(VT_ALU_BASE - VT_REG_BASE + VT_ALU_RESULT_0);
            return (u8)(vt.alu_result >> (byte_idx * 8));
        }
        return vt.reg[addr_lo];
    }

    if (addr_lo >= 0x20 && addr_lo < 0x30) {
        u8 ch_idx = (addr_lo - 0x20) >> 3;
        return vt.adpcm[ch_idx].playing ? 0x01 : 0x00;
    }

    return 0xFF;
}

void vt_adpcm_tick(s16 *buf, int samples)
{
    for (int s = 0; s < samples; s++) {
        s32 mixed = 0;
        for (int c = 0; c < 2; c++) {
            VTAdpcmChan *ch = &vt.adpcm[c];
            if (!ch->playing) continue;
            s16 sample = _adpcm_advance(ch);
            mixed += ((s32)sample * (s32)ch->volume) >> 7;
        }
        if (mixed >  32767) mixed =  32767;
        if (mixed < -32768) mixed = -32768;
        buf[s] = (s16)((s32)buf[s] + mixed);
    }
}

#if VT_ADPCM_SOUND
// Called from ARM-mode sound.s via bl_long -- must be compiled in ARM mode.
__attribute__((target("arm")))
void vt_adpcm_mix_gba(void)
{
    // PCMWAV is a fixed GBA address in palette RAM area (see equates.h)
    s8 * const buf  = (s8*)0x05000280;
    const int  SIZE = 128;   // PCMWAVSIZE from equates.h

    // The GBA timer1interrupt fires at ~15.7kHz.
    // 15.7kHz = 15700 ticks / sec.
    // VT369 timer runs at CPU clock = 1.789 MHz.
    // 1.789 MHz / 15.7 kHz = ~114 CPU cycles per GBA timer tick.
    // This provides a hardware-synchronous clock tick for the VT Timer,
    // avoiding massive architectural changes to PocketNES's internal queue.
    //
    // Per Furb h_OneBus.cpp::clockScanlineCounter:
    //   counter = !counter ? reloadValue : --counter;
    //   if (!counter && enableIRQ && isRendering) { fire IRQ }
    //
    // Key: the counter ALWAYS runs.  There is no "enable timer" bit on
    // real silicon.  The only IRQ gate is enableIRQ (= bit 1 of our
    // timer_ctrl shadow).  PocketVT used to gate counter on bit 0
    // (set by $4102) but that broke games like Star Ally that never
    // write $4102 -- they just write the period ($4101), then disable+
    // enable IRQ ($4103+$4104), expecting the always-running counter
    // to fire IRQs at the configured period.
    {
        int counter = (int)vt.timer_counter - 114;
        while (counter <= 0) {
            counter += vt.timer_period ? vt.timer_period : 1;
            if (vt.timer_ctrl & 0x02) { // IRQ enabled
                vt.want_timer_irq = 1;
                extern u8 _wantirq;
                _wantirq |= 0x02; // VT_IRQ_MAPPER
            }
        }
        vt.timer_counter = (u16)counter;
    }

    for (int s = 0; s < SIZE; s++) {
        s32 mixed = 0;

        for (int c = 0; c < 2; c++) {
            VTAdpcmChan *ch = &vt.adpcm[c];
            if (!ch->playing) continue;

            s16 sample = _adpcm_advance(ch);

            // Scale 16-bit signed -> 8-bit signed range, apply volume.
            s32 v = ((s32)sample * (s32)ch->volume) >> 14;
            mixed += v;
        }

        // Clamp to [-128, 127] and add to existing NES APU output
        if (mixed >  127) mixed =  127;
        if (mixed < -128) mixed = -128;
        s32 sum = (s32)buf[s] + mixed;
        if (sum >  127) sum =  127;
        if (sum < -128) sum = -128;
        buf[s] = (s8)sum;
    }
}
#endif // VT_ADPCM_SOUND