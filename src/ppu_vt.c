/*
 * ppu_vt.c -- VT03/VT09 enhanced PPU emulation for PocketVT
 *
 * Implements:
 *   - 512-entry extended palette (vt_palette_ram[])
 *   - GBA BGR555 conversion table (vt_palette_to_gba[])
 *   - Hi-res sprite OAM extension (vt_oam_ext[])
 *   - $2107 VT PPU mode register handler
 *
 * GBA colour conversion
 * ---------------------
 * VT palette entries are 6-bit: bits[5:4] = R, bits[3:2] = G, bits[1:0] = B
 * (each component is 2 bits, giving 4 shades per channel).
 * GBA expects BGR555: bits[14:10]=B, bits[9:5]=G, bits[4:0]=R (5 bits each).
 * We scale 2-bit -> 5-bit by repeating bits: 0b00->00000, 0b01->01010,
 * 0b10->10100, 0b11->11111 (i.e., multiply by 10.something; use * 10 + 1).
 *
 * Actually the simplest accurate formula for a 2-bit component c is:
 *   gba_component = (c << 3) | (c << 1) | (c >> 1)
 * which gives 0,10,20,31 -- a reasonable perceptual spread across 5 bits.
 *
 * TODO: cross-reference with the VT03 datasheet once available on NESdev wiki.
 */

#include "includes.h"
#include "ppu_vt.h"
#include "vt_regs.h"
#include "config.h"

#if VT_MODE

// Canonical 4096-entry VT03 colour LUT (EmuVT's HSL2RGB.TAB) -- extracted
// from the NESdev wiki "VT03+ Enhanced Palette" article and converted to
// BGR555.  Indexed by 12-bit composited colour number = (hi<<6)|lo where:
//   hi = vt_palette_ram[idx|0x80]  (bits 11:6  -> SAT[3:0], LUM[3:2])
//   lo = vt_palette_ram[idx|0x00]  (bits  5:0  -> LUM[1:0], HUE[3:0])
// Lives in ROM (.rodata) as a `const u16` -- 8 KiB, fine for a GBA cart.
#include "vt03_palette_lut.h"

// $2010 Extended Graphics Control 1 register shadow (see VT02+ Registers wiki).
//   bit 7 = COLCOMP : 1 = new (composited 12-bit) colour mode
//                     0 = old (single-byte) NES-compatible mode
//   bit 6 = V16BEN  : video 16-bit data bus enable (VT09)
//   bit 5 = SPOPEN  : sprite address extension enable
//   bit 4 = BKEXTEN : background address extension enable
//   bit 3 = SPEXTEN : sprite extension enable
//   bit 2 = SP16EN  : 16-colour / 16-pixel sprites enable
//   bit 1 = BK16EN  : 16-colour backgrounds enable
//   bit 0 = PIX16EN : 16-pixel mode select
EWRAM_BSS u8 vt_reg_2010 = 0;

// BKEXTEN module (defined later in this file, session 18)
extern u8 vt_bkexten_live;
extern u8 vt_bk_pending_whole;
extern u32 vt_bk_dbg[8];
void vt_bk_banks_recheck(void);
void vt_bk_frame_check(void);
void vt_bk_invalidate(void);
void vt_bk_whole(void);
void vt_bk_scrub(void);
// SESSION 20: throttled replacement for the 1920-cell vt_bk_whole() blast.
// Processes a bounded chunk of the map per vblank and returns 1 when the full
// map has been swept, so a BKEXTEN mode flip / cache-invalidate rebuild no
// longer overruns the frame (see vt_chr4_rebuild_if_dirty for the crash note).
static int vt_bk_whole_step(void);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

#if VT_ENHANCED_PALETTE
// vt_palette_ram[] layout (matches Furbtendulator OneBus.cpp:540):
//   [0x00 .. 0x1F]  low  6-bit half written via PPU $3F00-$3F1F
//   [0x80 .. 0x9F]  high 6-bit half written via PPU $3F80-$3F9F  (VT03+)
// Other indices [0x20..0x7F] and [0xA0..0x1FF] are unused but the buffer
// stays sized for the spec'd 512-entry future extension (VT09 enhanced).
EWRAM_BSS u8  vt_palette_ram[VT_PALETTE_SIZE] __attribute__((aligned(4)));  /* word loops in vt_pal_same */
#ifdef PAL_WRITE_LOG
EWRAM_BSS u32 pal_log[256];
EWRAM_BSS u32 pal_log_idx;
#endif
#ifdef DMA_LOG
EWRAM_BSS u32 dma_log[128];
EWRAM_BSS u32 dma_log_idx;
#endif
EWRAM_BSS u8  vt_dac_variant;
EWRAM_BSS u8  *vt_chr_src;    /* see ppu_vt.h -- CHR-ROM if present, else PRG */
EWRAM_BSS u32  vt_chr_mask;   /* VT_DAC_* -- see ppu_vt.h */
EWRAM_BSS u16 vt_palette_to_gba[VT_PALETTE_SIZE];
bool vt_palette_dirty = true;  /* exported: ppu.s inline palette store sets it */
// vt_palette_active: set the first time the running game writes to the VT
// extended palette via the hi-byte path ($3F80-$3F9F) or via the $2140-$217F
// register window.  Until that happens we MUST NOT touch the GBA palette RAM
// -- doing so wipes out (a) the PocketNES menu font palette at 0x05000080
// (FONT_PALETTE_NUMBER=4) and (b) the standard NES BG/sprite palette areas
// that run_palette in ppu.s manages from the 32-byte nes_palette.  Plain
// $3F00-$3F1F lo writes alone do NOT flip this -- standard NES ROMs use
// the lo range too and we don't want them stealing the GBA palette.
static bool   vt_palette_active  = false;
#endif

#if VT_HICOLOR_SPRITES
EWRAM_BSS u8 vt_oam_ext[VT_OAM_EXT_SIZE];
#endif

EWRAM_BSS u8 vt_ppumode = 0;

// PIECE 1: holds assembled GBA-ready 4bpp (16-colour) tiles when the VT cart
// runs BK16EN/SP16EN. 8 CHR pages * 64 tiles * 32 bytes = 16 KB. Filled by
// vt_chr_sync_from_prg's 4bpp branch; consumed by the 4bpp render path
// (Piece 2). Verified pixel-exact against Furbtendulator on the test ROMs.
EWRAM_BSS u8 vt_chr4_buf[8 * 64 * 32] __attribute__((aligned(4)));
// ^ MUST stay word-aligned: every copy/probe below reads this through a
// (const u32*) cast. A u8 array gets byte alignment by default, and when the
// EWRAM_BSS layout drifted it landed at ...9A (addr % 4 == 2). ARM7TDMI
// rotates unaligned LDRs, so every u32 load became lo(row)|hi(prev_row) --
// the session-20 "dashes everywhere" skew on Lonely Island's map. The
// byte-store assembler was always correct; only the word readers broke.

// Set when the 4bpp CHR banks change; the heavy full-4bpp assembly into
// vt_chr4_buf is deferred to vt_chr4_rebuild_if_dirty() (called at most once
// per frame) instead of running on every CPU register write (crash #2 fix).
EWRAM_BSS u8 vt_chr4_dirty = 0;
// Snapshot of the 8 CHR page banks at the time dirty was set, so the
// deferred rebuild assembles the correct pages.
EWRAM_BSS u32 vt_chr4_page_bank[8];

// ---------------------------------------------------------------------------
// Init / reset
// ---------------------------------------------------------------------------

void ppu_vt_init(void)
{
    ppu_vt_reset();
}

void vt_bk_attr_shadow_reset(void);
void ppu_vt_reset(void)
{
    vt_bk_attr_shadow_reset();
#if VT_ENHANCED_PALETTE
    memset(vt_palette_ram,    0, sizeof(vt_palette_ram));
    memset(vt_palette_to_gba, 0, sizeof(vt_palette_to_gba));
    vt_palette_dirty  = true;
    vt_palette_active = false;
#endif
#if VT_HICOLOR_SPRITES
    memset(vt_oam_ext, 0, sizeof(vt_oam_ext));
#endif
    vt_ppumode = 0;
    vt_reg_2010 = 0;
}

// ---------------------------------------------------------------------------
// $2107 VT PPU mode register
// ---------------------------------------------------------------------------

void vt_ppumode_write(u8 val)
{
    vt_ppumode = val;
    // TODO: when bit 0 transitions from 0 to 1, flush the GBA palette cache
    // and switch the background rendering path in ppu.s to VT mode.
    // When transitioning back, restore the standard NES palette mapping.
}

// ---------------------------------------------------------------------------
// Extended palette
// ---------------------------------------------------------------------------

#if VT_ENHANCED_PALETTE

// Convert a single 6-bit VT colour index to GBA BGR555.
// VT colour format: bits[5:4]=R  bits[3:2]=G  bits[1:0]=B  (2 bits each)
static inline u16 vt_colour_to_gba(u8 vtcol)
{
    u8 r2 = (vtcol >> 4) & 0x03;
    u8 g2 = (vtcol >> 2) & 0x03;
    u8 b2 = (vtcol >> 0) & 0x03;

    // Scale 2-bit -> 5-bit: 0->0, 1->10, 2->20, 3->31
    u16 r5 = (r2 << 3) | (r2 << 1) | (r2 >> 1);
    u16 g5 = (g2 << 3) | (g2 << 1) | (g2 >> 1);
    u16 b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);

    return (b5 << 10) | (g5 << 5) | r5;
}

void vt_palette_write(u8 offset, u8 val)
{
#ifdef PAL_WRITE_LOG
    /* s21b55: the $214x register window is a SECOND palette write path,
     * separate from the $3F00 PPU-bus path hooked in ppu.s.  Tag these with
     * 0x800000 so the offline reader can tell them apart. */
    { extern u32 pal_log[256], pal_log_idx;
      pal_log[pal_log_idx] = 0x800000u | ((u32)offset << 8) | val;
      pal_log_idx = (pal_log_idx + 1) & 255u; }
#endif
    if (offset >= VT_PALETTE_SIZE) return;
    vt_palette_ram[offset] = val & 0x3F;    // 6 bits only
    vt_palette_to_gba[offset] = vt_colour_to_gba(val);
    vt_palette_dirty  = true;
    vt_palette_active = true;     // a real VT game has spoken; rebuild is safe
}

// ---------------------------------------------------------------------------
// PPU-bus bridges -- called from VRAM_pal in ppu.s on every $3F00-$3F1F or
// $3F80-$3F9F write (regardless of vt_active state, so they must stay cheap
// and idempotent for non-VT carts).
// ---------------------------------------------------------------------------
//
// Both are called via `bl_long` from ARM-mode .vram1 code (VRAM_pal lives in
// the .vram1 section).  bl_long expands to `mov lr,pc ; ldr pc,=label` which
// does NOT switch to Thumb -- so these MUST be compiled in ARM mode.  See
// vt_regs.c::vt_resync_prg_banks() for the full interworking writeup.
//
// NES background-color mirroring rule:
//   Indices $00 / $04 / $08 / $0C mirror with $10 / $14 / $18 / $1C.
//   The PPU writes both halves automatically.  ppu.s already mirrors the
//   legacy 32-byte nes_palette[], so we apply the same rule here for the VT
//   shadow buffers so a hi-byte $3F90 write reaches both $3F90 and $3F80.

__attribute__((target("arm")))
void vt_palette_write_lo(u8 offset, u8 val)
{
    val &= 0x3F;
    // Store the full 7-bit offset linearly, ALWAYS -- regardless of the
    // $2010 mode bits at write time.  Games (Lonely Island among them)
    // upload the whole 128-byte 16-colour palette BEFORE enabling
    // BK16EN/SP16EN, so any mode-conditional storage destroys the
    // entries >= $20 that pixel values 4..15 index through the scattered
    // layout.  The legacy NES 32-byte view (nes_palette, with $x0<->$1x
    // mirroring) is maintained separately by the ppu.s caller, so nothing
    // here needs the destructive &0x1F aliasing.
    if (offset < VT_PALETTE_SIZE) vt_palette_ram[offset] = val;
    // Legacy backdrop mirroring within the low 32 bytes only.  In the
    // 16-colour scattered layout these slots ((offset & 3) == 0 with
    // bits 0,1,5,6 clear) are forced to backdrop at render time by the
    // (idx & 0x63) == 0 transparency rule, so the mirror writes are
    // harmless to 16-colour data while preserving the COLCOMP=1 path's
    // expectations for the low window.
    if (offset < 0x20 && (offset & 0x03) == 0) {
        vt_palette_ram[offset ^ 0x10] = val;
    }
    vt_palette_dirty = true;
    // Do NOT set vt_chr4_dirty here: tile pixel data does not depend on the
    // palette, and flagging it forced the heavy vt_chr4_assemble to run on
    // every frame the game touched $3Fxx (LI does so continuously in-game),
    // dragging the whole emulator to ~7 fps -- which starved the per-guest-
    // frame joypad refresh and made the game feel input-dead.  The GBA
    // sub-palettes are re-derived every frame by vt_16c_palette_fixup, which
    // reads vt_palette_ram directly and needs no dirty flag.
    // NOTE: do NOT flip vt_palette_active here.  Plain NES carts write the
    // lo range during normal operation; we only want to take over the GBA
    // palette when a *real* VT game writes the hi range.
}

__attribute__((target("arm")))
void vt_palette_write_hi(u8 offset, u8 val)
{
    offset = (offset & 0x7F) | 0x80;   // s21b59: all 128 hi entries (was & 0x1F)
    val    &= 0x3F;
    vt_palette_ram[offset] = val;
    // Backdrop mirroring within the hi bank, NintendulatorNRS rule
    // (addr & 0x63) == 0 -- entries 0x80,0x84..0x9C only.
    if ((offset & 0x63) == 0) {
        vt_palette_ram[offset ^ 0x10] = val;
    }
    vt_palette_dirty  = true;
    vt_palette_active = true;     // first real VT hi-byte write -> take over
}

// ---------------------------------------------------------------------------
// VT03 palette compositor + GBA palette rebuild
// ---------------------------------------------------------------------------
// vt_palette_rebuild_gba is called from ARM-mode ppu.s via bl_long, so MUST
// be compiled in ARM mode (LDR PC doesn't interwork on ARMv4T).
//
// Furbtendulator (OneBus.cpp:1336, GetPalIndex):
//   if (reg2000[0x10] & COLCOMP)
//        idx = (Palette[TC | 0x80] << 6) + Palette[TC | 0x00] + PALETTE_VT03;
//   else idx = Palette[TC];
//
// We replicate that here for the 32 standard NES palette slots.  Result is
// a 12-bit index (0..4095).  GBA hardware palette only has 256 entries, so
// we MAP into the existing 4 BG sub-palettes (slots 0..15, 16..31, 32..47,
// 48..63 in NES palette indexing) -- meaning four entries per sub-palette,
// 4 BG + 4 sprite sub-palettes = 32 colours that map to GBA palette slots
// 0..15 (BG, at 0x05000000) and 0x100..0x10F (sprites, at 0x05000200).
//
// Composite a (lo, hi) palette byte pair into a BGR555 value via the
// canonical 4096-entry LUT.
//
// When COLCOMP=0 (old NES-compat mode) the hi byte is ignored and we use
// the legacy 2-bits-per-channel gradient based on the lo byte alone -- this
// keeps standard NES carts looking right when run on a VT chip.
//
// When COLCOMP=1 we look up vt03_palette_lut[((hi<<6)|lo) & 0xFFF].
static inline u16 vt03_composite_to_gba(u8 lo, u8 hi)
{
    if (vt_reg_2010 & 0x80) {
        // COLCOMP=1: full 12-bit composited index into the canonical LUT.
        u16 idx = (((u16)hi & 0x3F) << 6) | ((u16)lo & 0x3F);
        return vt03_palette_lut[idx & 0xFFF];
    }
    // COLCOMP=0: standard NES 64-colour palette, but accessed via the lo
    // byte's 6-bit field.  Use the lo byte directly as a 6-bit index into
    // the LUT's low rows (which match the canonical NES YUV palette for
    // hi=0).  This keeps non-VT-aware games looking sensible.
    return vt03_palette_lut[lo & 0x3F];
}

__attribute__((target("arm")))
void vt_palette_rebuild_gba(void)
{
    /* s21b62: newframe_nes_vblank (NES line 242) calls this first; the frame's
     * catch-ups have already filled bg0cntbuff to line 240 and the buffer swap
     * has not happened yet -- the right moment to apply raster-split bands
     * (guide s.73).  Called from here because the VRAM code section that holds
     * newframe_nes_vblank has no room for another call. */
    vt_bands_frame_end();

    // ========================================================================
    // CRITICAL CORRECTNESS GATES -- if either of these is wrong, every game
    // boots to a black screen.  History (the old PPU_S_PATCH_INSTRUCTIONS.md
    // was folded into MAINTAINERS_GUIDE.md):  the 0.4 cut of this function
    // splattered vt03_palette_lut[lo&0x3F] across all 4 BG and 4 OBJ
    // sub-palettes.  The LUT's first 64 entries are all 0x0000 (because
    // they map to the COLCOMP=1 colourspace's "saturation=0, luminance=0"
    // row, i.e. pure black).  So in COLCOMP=0 mode (every plain NES cart,
    // every VT09 cart that hasn't enabled enhanced colour, the menu, and
    // every VT03 cart before its init code flips bit 7 of $2010), the
    // function painted the entire GBA palette black on the very first
    // VBlank after mapVTinit set vt_active=1.  PocketNES's run_palette
    // then refreshed only slots 0-3 of each sub-palette from nes_palette,
    // leaving slots 4-15 stuck at the black we wrote.  Since NES attribute
    // bytes 1/2/3 land in GBA palettes 1/2/3 (and their pixel values 0-3
    // still fall in slots 0-3 of those palettes -- which run_palette DID
    // refresh), the user actually saw mostly-NES colours on sub-palette 0
    // tiles only and black everywhere else; combined with most carts using
    // BG attribute = 0 for the boot logo screens, the net effect was
    // "screen mostly black with a faint flash on frame 1".
    // ========================================================================

    // Gate 1: not even a VT cart?  Bail.  vt_active is set to 1 by
    // mapVTinit in Mappers/mapVT.s; non-VT carts leave it at 0 (cleared
    // before mapperinit dispatch in cart.s::loadcart_asm).
    if (!vt_active) return;

    // Gate 2: COLCOMP=0 (legacy NES-compatible mode)?  Bail.  When the
    // game is running in 2bpp / NES-compat mode (which is the power-on
    // default for every VT cart -- $2010 = 0 at reset), the standard
    // run_palette pipeline in ppu.s already produces correct visuals via
    // nes_palette + MAPPED_RGB.  We must NOT touch the GBA palette in this
    // mode, or we'll either (a) write black over good NES colours (the
    // 0.4 bug), or (b) write the wrong sub-palette colours over the right
    // ones (any version that writes-then-lets-run_palette-fix-up only
    // sub-palette 0).  Real VT silicon takes the same approach: the 64-
    // colour NES YUV palette path is hardwired and only the COLCOMP=1
    // composite mode engages the 4096-entry HSL LUT.
    if (!(vt_reg_2010 & 0x80)) {
        vt_palette_dirty = false;   // ack the dirty flag so we don't spin
        return;
    }

    // Gate 3: nothing actually changed since the last rebuild?  Bail.
    if (!vt_palette_dirty) return;

    // ========================================================================
    // VT03 COLCOMP=1 path -- composite (lo|hi) -> 12-bit index -> BGR555
    // ========================================================================
    // NES palette memory layout (32 bytes at $3F00-$3F1F, mirrored to
    // vt_palette_ram[0..0x1F]):
    //   $3F00      backdrop
    //   $3F01-$3F03 BG sub-palette 0 colours 1,2,3
    //   $3F04      mirrored backdrop
    //   $3F05-$3F07 BG sub-palette 1 colours 1,2,3
    //   $3F08      mirrored backdrop
    //   $3F09-$3F0B BG sub-palette 2 colours 1,2,3
    //   $3F0C      mirrored backdrop
    //   $3F0D-$3F0F BG sub-palette 3 colours 1,2,3
    //   $3F10-$3F1F same layout for OBJ (sprite) sub-palettes 0..3
    //
    // PocketNES's BG renderer encodes the NES 2-bit attribute byte (which
    // selects sub-palette 0..3) into the GBA tilemap entry's bits 12-15
    // (palette field), so attr=N -> GBA palette N.  Inside each GBA
    // palette, the 2-bit pixel value (0..3) selects entries 0..3.  We
    // therefore lay out the 4 GBA BG sub-palettes as:
    //   GBA BG palette 0, entries 0..3  =  NES $3F00..$3F03
    //   GBA BG palette 1, entries 0..3  =  NES $3F04..$3F07
    //   GBA BG palette 2, entries 0..3  =  NES $3F08..$3F0B
    //   GBA BG palette 3, entries 0..3  =  NES $3F0C..$3F0F
    // And similarly for OBJ palettes at 0x05000200+.
    //
    // This matches the layout run_palette writes in COLCOMP=0 mode, so
    // the two paths produce identical *positions* and differ only in the
    // colour table they consult (MAPPED_RGB[lo] vs vt03_palette_lut[hi:lo]).

    volatile u16 *gba_bg  = (u16*)0x05000000;
    volatile u16 *gba_obj = (u16*)0x05000200;

    for (int sub = 0; sub < 4; sub++) {
        for (int slot = 0; slot < 4; slot++) {
            // BG entry
            u8  bg_idx = (sub * 4 + slot) & 0x1F;
            u8  bg_lo  = vt_palette_ram[bg_idx]        & 0x3F;
            u8  bg_hi  = vt_palette_ram[bg_idx | 0x80] & 0x3F;
            u16 bg_col = vt03_palette_lut[ (((u16)bg_hi << 6) | bg_lo) & 0xFFF ];
            gba_bg[sub * 16 + slot] = bg_col;

            // OBJ entry (sprite palettes start at $3F10)
            u8  obj_idx = (0x10 + sub * 4 + slot) & 0x1F;
            u8  obj_lo  = vt_palette_ram[obj_idx]        & 0x3F;
            u8  obj_hi  = vt_palette_ram[obj_idx | 0x80] & 0x3F;
            u16 obj_col = vt03_palette_lut[ (((u16)obj_hi << 6) | obj_lo) & 0xFFF ];
            gba_obj[sub * 16 + slot] = obj_col;
        }
    }

    // Also refresh the 512-entry conversion table used by future
    // 8bpp / scanline-select rendering paths.  Cheap and keeps
    // downstream work composable.
    for (int i = 0; i < VT_PALETTE_SIZE; i++) {
        vt_palette_to_gba[i] = vt_colour_to_gba(vt_palette_ram[i]);
    }

    vt_palette_dirty = false;
}

#endif // VT_ENHANCED_PALETTE


// ---------------------------------------------------------------------------
// Hi-res sprite OAM extension
// ---------------------------------------------------------------------------

#if VT_HICOLOR_SPRITES

void vt_oam_ext_write(u8 offset, u8 val)
{
    if (offset >= VT_OAM_EXT_SIZE) return;
    vt_oam_ext[offset] = val;
    // TODO: set a dirty flag so the sprite renderer re-fetches OAM extension
    // data for the affected sprite on the next visible scanline.
    // The actual 16x8 rendering path needs to be added to ppu.s (or a new
    // ppu_vt_sprites.s) -- see MAINTAINERS_GUIDE.md s.74 for the design.
}

#endif // VT_HICOLOR_SPRITES

// ---------------------------------------------------------------------------
// VT CHR-from-PRG sync (NEW -- bootstrap for OneBus CHR-RAM carts)
// ---------------------------------------------------------------------------
// On real VT silicon, OneBus cartridges have no CHR ROM -- the CHR pattern
// data lives in PRG ROM and is fetched via the $2012-$2017 + $2018 + $201A
// + $4100 bank registers (see VT02+ CHR-ROM Bankswitching wiki).
//
// PocketNES treats no-CHR-ROM carts as CHR-RAM and points its bank pointer
// (vrombase) at NES_VRAM (8 KB in IWRAM).  The GBA tile cache then refreshes
// from NES_VRAM whenever it's written.  To make VT carts visible, we need
// to COPY the appropriate 8 KB slice of PRG into NES_VRAM whenever the CHR
// bank registers change.
//
// This function performs the MMC3-style 2K+2K+1K+1K+1K+1K (= 8 KB total)
// mapping per VT02+ CHR-ROM Bankswitching, then copies from PRG into
// NES_VRAM accordingly.  Call it:
//   - Once at vt_reset() (using the hard-reset CHR bank defaults)
//   - After any write to $2012-$2017 / $2018 / $201A / $4100[3:0]
//
// The copy is small (8192 bytes) and IWRAM-to-IWRAM (rombase is in EWRAM
// but the CPU copies in 4-byte chunks, ~ a few thousand cycles per sync)
// so it's affordable even from a hot register-write path on a real GBA.
//
// NOTE: this is a BOOTSTRAP for getting tiles on screen.  The full design
// also needs (a) GBA tile cache invalidation after each copy, and (b)
// proper 4bpp planes-2/3 handling at PPU $4000-$5FFF for VT03 4bpp mode.
// Those depend on changes in ppu.s that are out of scope for this patch.
#if VT_MODE
// rombase, rommask: PocketNES exports these via asmcalls.h:
//   extern u8 *_rombase;       #define rombase _rombase
//   extern u32 _rommask;       #define rommask _rommask
// Picked up automatically through includes.h -> asmcalls.h.
//
// NES_VRAM lives at a fixed EWRAM address -- see equates.h.  PocketNES already
// exports the symbol from cart.s/ppu.s via `.global NES_VRAM` and declares it
// in asmcalls.h as `extern u8 NES_VRAM[8192];`.  We just use the same symbol.
// dirty_tiles is a 512-byte bitmap (1 byte per 16-byte tile across all of
// NES_VRAM).  Writing any non-zero value flags the tile for re-render.
// dirty_rows is a 32-byte bitmap (1 byte per 256-byte row).
// Both symbols are exported from ppu.s when DIRTYTILES is enabled.
extern u8 dirty_tiles[512];
extern u8 dirty_rows[32];

// 1 KB CHR page index helper.  Returns the byte offset into PRG ROM that
// holds the requested 1KB CHR page, given the current bank registers.
//
// The OneBus mapping (VT02+ CHR-ROM Bankswitching, MMC3-style A12-low):
//   PPU $0000-$03FF  ->  ($2016 & 0xFE)         * 1K  (2KB pair, A0 forced 0)
//   PPU $0400-$07FF  ->  ($2016 | 0x01)         * 1K
//   PPU $0800-$0BFF  ->  ($2017 & 0xFE)         * 1K
//   PPU $0C00-$0FFF  ->  ($2017 | 0x01)         * 1K
//   PPU $1000-$13FF  ->  ($2012)                * 1K
//   PPU $1400-$17FF  ->  ($2013)                * 1K
// VT02+ CHR bank-number formula per NESDev wiki "VT02+ CHR-ROM Bankswitching":
//
//   Without Address Extension:
//     BankNumber = (InnerBank & InnerBankMask)
//                | (MiddleBank & ~InnerBankMask)
//                | (IntermediateBank << 8)
//                | (OuterBank << 11);
//
//   With Address Extension (BKEXTEN or SPEXTEN in $2010):
//     BankNumber = EVA | (((InnerBank & InnerBankMask)
//                       | (MiddleBank & ~InnerBankMask)) << 3)
//                       | (OuterBank << 11);
//
// Where:
//   InnerBank        = the relevant per-slot $2012-$2017 byte
//   InnerBankMask    = depends on $201A bits 0-2 (VB0S) via lookup:
//                      0 -> $FF (full 256KB inner), 1 -> $7F (128KB),
//                      2 -> $3F (64KB), 4 -> $1F (32KB), 5 -> $0F (16KB),
//                      6 -> $07 (8KB).  Values 3,7 invalid; treat as 0.
//   MiddleBank       = $201A bits 3-7 (RV6) -- but kept in-place ($201A & $F8),
//                      so it's pre-shifted; OR directly into the low byte.
//   IntermediateBank = $2018 bits 4-6 (VA18-VA20), 3 bits, shifts to bit 8.
//   OuterBank        = $4100 bits 0-3 (VA21-VA24), 4 bits, shifts to bit 11.
//   EVA              = per-fetch-tile, from attribute data or OAM byte 2.
//                      Not modelable in bulk sync; left as 0.
//
// Cross-reference with Furb h_OneBus.cpp:
//   #define VB0S    (reg2000[0x1A] & 0x07)        // bits 0-2
//   #define RV6     (reg2000[0x1A] & 0xF8)        // bits 3-7 in-place
//   #define VA18    (reg2000[0x18] >> 4 & 0x07)   // bits 4-6, 3 bits
//   #define VA21    (reg4100[0x00] & 0x0F)        // bits 0-3, 4 bits
//   static const uint8_t VB0STable[8] = { 0, 1, 2, 0, 3, 4, 5, 0 };
//   int chrAND = 0xFF >> VB0STable[VB0S];         // InnerBankMask
//   int chrOR  = RV6 & ~chrAND;                   // MiddleBank component
//
// VT02+ Bankswitching wiki notes that $4105 bit 7 (COMR7) = 1 inverts PPU A12,
// swapping the $0000-$0FFF banks with $1000-$1FFF -- not yet handled here
// because Lonely Island and Star Ally both leave COMR7=0; add if needed.

static inline u32 vt_inner_bank_mask(void)
{
    static const u8 vb0s_to_shift[8] = { 0, 1, 2, 0, 3, 4, 5, 0 };
    u8 vb0s = vt_chr_reg_201A & 0x07;
    return 0xFFu >> vb0s_to_shift[vb0s];
}

// SESSION 21b18: the OUTER bank is $4100.0-3 (four bits) in 2bpp but only
// $4100.0-2 (THREE bits) in 4bpp -- see the wiki's final-address diagrams:
// 2bpp puts the outer field at address bits 21-24, 4bpp at 22-24, because a
// 4bpp tile eats one more low bit.  Masking with 0x0F unconditionally sends
// any 4bpp fetch with $4100 bit 3 set to a bank 8 slots away.  Star Ally and
// Lonely Island never set that bit so they never noticed; the VG Pocket game
// list runs $4100 = $0B and was fetching its tiles from the wrong place.
static inline u32 vt_compute_chr_bank_n(u32 inner_bank, int fourbpp)
{
    u32 inner_mask    = vt_inner_bank_mask();
    u32 middle        = (u32)vt_chr_reg_201A & 0xF8u;   // RV6 in-place (bits 3-7)
    u32 intermediate  = ((u32)vt_chr_reg_2018 >> 4) & 0x07u;  // VA18-VA20 (3 bits)
    u32 outer         = (u32)vt_chr_outer_4100 & (fourbpp ? 0x07u : 0x0Fu);

    return ((inner_bank & inner_mask)
          | (middle    & ~inner_mask))
         | (intermediate << 8)
         | (outer       << 11);
}

static inline u32 vt_compute_chr_bank(u32 inner_bank)
{
    return vt_compute_chr_bank_n(inner_bank, 0);
}

/* s21b59: CHR fetch source -- see ppu_vt.h. */
static inline const u8 *vt_chr_base(void) { return vt_chr_src ? vt_chr_src : rombase; }
static inline u32 vt_chr_mask_get(void)
{
    u32 m = vt_chr_src ? vt_chr_mask : rommask;
    return m ? m : 0xFFFFFFFFu;
}

static inline u32 vt_chr_bank_byte_offset_n(u32 onebus_1k_bank, int fourbpp)
{
    return vt_compute_chr_bank_n(onebus_1k_bank, fourbpp) * 1024u;
}

static inline u32 vt_chr_bank_byte_offset(u32 onebus_1k_bank)
{
    // Compose the 1KB bank into a byte offset within PRG-ROM.
    // 1KB granularity in non-4bpp; 2KB in 4bpp (handled by caller).
    return vt_compute_chr_bank(onebus_1k_bank) * 1024u;
}

static u8 vt_chr_sync_pending = 0;

void vt_chr_sync_from_prg(void)
{
    /* LAZY since the 1.4-emulated-fps diagnosis: this used to do the full
       8KB CHR window copy + mark all 512 tiles dirty EAGERLY on every
       banking/chr-reg write.  The Lonely Island level transition storms
       those writes, so each emulated frame cost dozens of real frames
       (profiled: 96% of host time in this pipeline; vblank cadence 750ms).
       Now we only note that a sync is wanted; vt_chr_sync_flush() applies
       it at most once per frame (and skips entirely when the effective
       banks did not change).  Tradeoff: mid-frame CHR bank raster tricks
       lose sub-frame granularity -- acceptable until something needs it. */
    if (!rombase) return;
    vt_chr_sync_pending = 1;
}

static void vt_chr_sync_flush(void)
{
    if (!vt_chr_sync_pending || !rombase) return;
    vt_chr_sync_pending = 0;

    // ========================================================================
    // CHR copy path selection
    // ------------------------------------------------------------------------
    // VT03 supports two CHR encodings:
    //   * "2bpp" / NES-compat: each 8x8 tile is 16 bytes (planes 0-1 only).
    //     Tile N lives at PRG_OFFSET + N*16.  Power-on default.  Used by
    //     plain NES carts and by every VT cart before it engages enhanced
    //     graphics.
    //   * "4bpp" enhanced: each 8x8 tile is 32 bytes (planes 0-1 + planes
    //     2-3, interleaved).  Engaged only when (a) COLCOMP=1 is set
    //     ($2010 bit 7) AND (b) at least one of BK16EN/SP16EN is set
    //     ($2010 bits 1 or 2).  PocketNES's GBA-side renderer is fixed
    //     at 4bpp-into-16-colour mode, so we only need the planes-0-1
    //     half here (the GBA tile cache uses 16 colours per palette and
    //     decodes 2bpp NES patterns into 4bpp GBA patterns; for VT 4bpp
    //     we want the *low* 2 planes again so existing run_palette /
    //     nes_chr_update logic still works, but pulled from the
    //     interleaved source layout).
    //
    // Furbtendulator reference (h_OneBus.cpp::setCHR, bit4pp==true):
    //     shiftedAddress = (i & 0xF) | ((i >> 1) & ~0xF);
    //     // bytes with (i & 0x10)==0 -> chrLow plane01
    //     // bytes with (i & 0x10)!=0 -> chrLow plane23  (we skip these)
    // So for each output byte j (0..0x1FFF) we want input byte:
    //     src_j = (j & 0xF) | ((j & ~0xF) << 1)
    // which doubles the upper bits of j and leaves the low 4 bits alone.
    // That picks bytes 0..15, 32..47, 64..79, ... from the source, i.e.
    // the planes-0-1 half of each 32-byte 4bpp tile.
    // ========================================================================

    // VT03 4bpp graphics mode detection.
    //
    // Per Furb h_OneBus.cpp:
    //   #define BK16EN  !!(reg2000[0x10] &0x02)
    //   #define SP16EN  !!(reg2000[0x10] &0x04)
    //
    // And setCHR is called with bit4pp = BK16EN (for BG) or SP16EN (for SPR).
    // COLCOMP (bit 7) is a separate "12-bit color compositing" concept --
    // unrelated to 4bpp tile data format.  Treating them as conjoined was
    // the bug: Lonely Island writes $2010=$0E which sets BK16EN+SP16EN
    // but leaves COLCOMP off; PocketVT was then taking the 2bpp memcpy
    // path and copying the raw 4bpp data through as if it were 2bpp,
    // making BG tiles render as scrambled garbage on the GBA side.
    //
    // We're copying a unified 8KB CHR window covering both BG and SPR,
    // so use either-or: if any 16-color mode is on, deinterleave.
    const bool four_bpp = (vt_reg_2010 & 0x06) != 0;  // BK16EN | SP16EN

    // The eight 1KB CHR pages, indexed by PPU $0000-$1FFF in 1KB chunks.
    /* s21b62: when the last NES frame was a raster split, build the
     * primary set from the registers at that frame's END (its last band),
     * not the live registers.  This runs from the GBA vblank hook, which is
     * not phase-locked to the NES timeline, so the live registers hold a
     * different band's banks from one GBA frame to the next -- which marked
     * the set dirty EVERY frame and re-decoded all eight pages from ROM (77%
     * of the ARM on Aero Gyrodine's title; guide section 72).  Non-split
     * frames use the live registers exactly as before. */
    extern u8 vt_split_frame, vt_frame_reg[6];
    const u8 *R = vt_split_frame ? vt_frame_reg : vt_chr_reg;
    u32 page_bank[8];
    page_bank[0] = (R[4] & 0xFEu);          // $2016 even
    page_bank[1] = (R[4] | 0x01u);          // $2016 odd
    page_bank[2] = (R[5] & 0xFEu);          // $2017 even
    page_bank[3] = (R[5] | 0x01u);          // $2017 odd
    page_bank[4] =  R[0];                   // $2012
    page_bank[5] =  R[1];                   // $2013
    page_bank[6] =  R[2];                   // $2014
    page_bank[7] =  R[3];                   // $2015

    {   /* No-op skip: redundant banking rewrites are the common case during
           the transition storm; identical effective banks = nothing to do. */
        static u32 applied[8] = {0xFFFFFFFFu,0,0,0,0,0,0,0};
        static u8  applied_mode = 0xFF;
        int same = (applied_mode == (u8)four_bpp);
        for (int p = 0; same && p < 8; p++) if (applied[p] != page_bank[p]) same = 0;
        if (same) return;
        for (int p = 0; p < 8; p++) applied[p] = page_bank[p];
        applied_mode = (u8)four_bpp;
    }

    // rommask is the PRG-ROM address mask (= romsize-1), guaranteed power-of-2
    // by PocketNES's cart loader.  Mask each per-page source offset against it.
    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    u8 *dst = (u8*)NES_VRAM;

    if (!four_bpp) {
        // Fast path: raw 1KB memcpy per CHR page.
        for (int p = 0; p < 8; p++) {
            u32 src_off = vt_chr_bank_byte_offset(page_bank[p]) & mask;
            u8 *src = cbase + src_off;
            memcpy(dst + (p * 1024), src, 1024);
        }
    } else {
        // 4bpp mode.  IMPORTANT (crash #2 fix): this function is called from
        // the CPU register-write path on EVERY change to $2012-$2017 (six
        // separate call sites) and from vt_ppu_reg_write -- i.e. potentially
        // many times per frame.  The full 4bpp tile assembly (32K iterations)
        // is FAR too expensive to run per write; doing so crashed the build
        // (timing/stack failure inside the write handler).
        //
        // So the per-write path here stays CHEAP: it only fills NES_VRAM with
        // the low 2bpp half (a simple deinterleave copy), exactly as much
        // work as the old 2bpp path.  The expensive full-4bpp assembly into
        // vt_chr4_buf is DEFERRED: we just flag it dirty here and let
        // vt_chr4_rebuild_if_dirty() do it at most once per frame (Piece 2
        // will call that from the frame/vblank hook and display the result).
        for (int p = 0; p < 8; p++) {
            u32 phys_1k = vt_chr_bank_byte_offset(page_bank[p]) >> 10;
            u32 src_off = (phys_1k * 2048u) & mask;
            u8 *src = cbase + src_off;
            u8 *dp  = dst + (p * 1024);
            for (u32 j = 0; j < 1024; j++) {
                u32 src_j = (j & 0xF) | ((j & ~0xFu) << 1);
                dp[j] = src[src_j & 0x7FF];
            }
        }
        vt_chr4_dirty = 1;   // defer the heavy 4bpp assembly to once/frame
        for (int p = 0; p < 8; p++) vt_chr4_page_bank[p] = page_bank[p];
    }

    // Mark every tile dirty so the GBA-side tile cache re-renders the
    // freshly-copied CHR.  Without this, the dirty_tiles bitmap stays
    // clean (since we bypassed the per-byte $2007 store path) and the
    // BG / sprite cache never refreshes -- giving the user a black screen
    // even though the CHR bytes are correct.
    // 512 tile entries (8KB / 16B per tile) + 32 row entries.
    for (int i = 0; i < 512; i++) dirty_tiles[i] = 0xFF;
    for (int i = 0; i <  32; i++) dirty_rows[i]  = 0xFF;
}

// Piece 3: build 16-entry GBA BG sub-palettes for COLCOMP=0 + 16-colour mode.
//
// IMPORTANT NES-vs-VT distinction (kept straight per the datasheet digests):
//   - This is the COLCOMP=0 case (confirmed via furb_cli: the test ROMs run
//     $2010 = $1E/$1F = BK16EN|SP16EN, COLCOMP=0). In COLCOMP=0 the colour
//     mapping is the NES-style one: a palette-RAM byte is a 6-bit NES palette
//     index, turned into RGB by the standard NES master palette (nes_rgb).
//   - We must NOT use the VT03 SAT/LUM/PHA 4096-entry LUT here -- that is the
//     COLCOMP=1 path only. Using it would be the classic NES/VT mix-up.
//   - The only thing that changes vs plain NES 2bpp is WIDTH: a 16-colour
//     tile's pixel is a 4-bit (0..15) index into a 16-entry sub-palette,
//     instead of a 2-bit index into 4 entries.
//
// nes_rgb is 64 entries x 3 bytes (R,G,B). GBA wants BGR555.
extern unsigned char nes_rgb[];   // from ppu.s (now .global)

// VT03 COLCOMP=0 compatibility palette, derived from the NESdev "VT03+
// Enhanced Palette" chart (EmuVT HSL2RGB.TAB) by fitting the game's known
// palette indices against the user's reference capture: LL levels 0-3 map
// to chart rows (S,L) = (8,4),(8,6),(A,8),(5,D) -- saturated for 0-2 and
// pastel for 3, exactly as the wiki describes -- hues 14/15 forced black.
// The VT chip's DAC differs noticeably from a stock NES (PocketNES's
// nes_rgb gave the washed-out mint/salmon look); this table restores the
// saturated greens/tans/blues the hardware shows.

// SESSION 21b16: SECOND compat palette, for the VG Pocket console family.
// Calibrated per INDEX (not per colour) with a sentinel build whose table
// encodes i as a unique colour, so each pixel's palette index is read
// straight out of the framebuffer and joined against Michael's vg.png
// capture -- 87-100% confidence on every entry below.
//
// A single shared table CANNOT serve both consoles: index $1A must be
// blue-violet here and GREEN on Lonely Island's console, and $12/$27 also
// conflict.  That is a per-console DAC difference, not a table bug, which
// is why the s21b14 attempt to satisfy both by editing one table wrecked
// Lonely Island.  Entries not calibrated yet are inherited unchanged.
// s21b57 REFIT under the corrected 16-bit plane order (guide section 68).
// The s21b16-39 fit absorbed the p1/p2 swap.  Recovered by re-voting every
// pixel of the three calibrated menus (title, category, list): target colour
// = the OLD core's calibrated render, index = the NEW decode's palette entry.
// 28 indices, 21 unanimous; 7 contested, all background, settled by majority
// -- the menu captures were interpolated window grabs (section 44), so some
// cross-screen disagreement is the references, not the decode.  Independent
// check: tools/palette_sanity.py suspect entries 9 -> 7, and $1A -- previously
// recorded as a "true per-console difference" -- now sits correctly in its
// hue column.  NOT re-scored against vg.png/1.png/2.png/3.png, which are not
// in the tree: re-score when they are.
static const u16 vt_compat_rgb555_vg[64] = {
    0x39CE, 0x4840, 0x6400, 0x6404, 0x0390, 0x200E, 0x0000, 0x000B,
    0x0171, 0x00C0, 0x0120, 0x0140, 0x2900, 0x0000, 0x0000, 0x0000,
    0x4A52, 0x2900, 0x7CAB, 0x7C6F, 0x6457, 0x4059, 0x08B9, 0x0134,
    0x014A, 0x0246, 0x0260, 0x0200, 0x3D80, 0x0000, 0x0000, 0x0000,
    0x7FFF, 0x4A52, 0x7D4A, 0x7DB9, 0x76FF, 0x607F, 0x10FF, 0x06BE,
    0x7746, 0x0390, 0x0BC9, 0x53A8, 0x7746, 0x2529, 0x0000, 0x0000,
    0x7FFF, 0x7FD7, 0x7F7B, 0x7F3F, 0x7F1F, 0x7746, 0x675F, 0x0390,
    0x439C, 0x43D9, 0x4FF7, 0x67F5, 0x7FF5, 0x673A, 0x0000, 0x0000
};

static const u16 vt_compat_rgb555[64] = {
    0x35AD, 0x4840, 0x6400, 0x6404, 0x480B, 0x200E, 0x000E, 0x000B,
    0x0063, 0x00C0, 0x0120, 0x0120, 0x20A0, 0x0000, 0x0000, 0x0000,
    0x5274, 0x6520, 0x7C83, 0x7C0A, 0x6412, 0x3C15, 0x0035, 0x00B2,
    0x014A, 0x01C4, 0x0200, 0x0200, 0x3D80, 0x0000, 0x0000, 0x0000,
    0x77BD, 0x7E20, 0x7D4A, 0x7CD2, 0x7C7C, 0x607F, 0x10FF, 0x017C,
    0x0252, 0x02CA, 0x0340, 0x1320, 0x62A0, 0x0000, 0x0000, 0x0000,
    0x7FFF, 0x7FD7, 0x7F7B, 0x7F3F, 0x7F1F, 0x7F1F, 0x675F, 0x539F,
    0x3FFF, 0x3FFB, 0x4FF7, 0x67F5, 0x7FF5, 0x56B5, 0x0000, 0x0000
};

/* Single source of truth for which colour-DAC approximation this cart uses.
 * See MAINTAINERS_GUIDE section 57. */
// Lucky Lawn Mower's VT09 board -- a THIRD console DAC, distinct from both
// Star Ally/Lonely Island's and the VG Pocket's.  Calibrated s21b49 by the
// value-sentinel method (guide section 31) against Michael's native 256x240
// gameplay capture (lawn.png), a clean 20-colour framebuffer dump.
//
// VALIDITY CHECK THAT MADE THIS POSSIBLE: our maze and the reference agree to
// 94.7% on a palette-independent row signature, so per-pixel voting maps like
// to like.  ALWAYS run that check first -- two different mazes would produce
// confident nonsense at high vote counts.
//
// 16 indices measured, all at >=95% vote confidence except $21 (61%, 256 px).
// The other 48 are inherited from vt_compat_rgb555 and are NOT evidence-backed.
// s21b57 REFIT: the table above this line was fitted in s21b49 under the WRONG
// 16-bit plane order (p1/p2 swapped, guide section 68), which it silently
// absorbed -- gameplay looked right and the opening did not.  Refitted from
// BOTH references (lawn.png + gg.png) under the corrected decode: 18 indices
// measured, all 18 unanimous across both screens.
static const u16 vt_compat_rgb555_llm[64] = {
    0x294A, 0x4840, 0x6400, 0x6404, 0x480B, 0x200E, 0x000E, 0x0048,
    0x00A1, 0x00E0, 0x0120, 0x08C0, 0x20A0, 0x0000, 0x0000, 0x0000,
    0x5294, 0x6520, 0x7C83, 0x7C0A, 0x6412, 0x3C15, 0x0097, 0x00B2,
    0x01AA, 0x01E0, 0x0200, 0x0200, 0x3D80, 0x0000, 0x0000, 0x0000,
    0x7FFF, 0x7EA0, 0x7D4A, 0x7CD2, 0x7C7C, 0x607F, 0x10FF, 0x027F,
    0x0317, 0x0349, 0x0B60, 0x1320, 0x62A0, 0x0000, 0x0000, 0x0000,
    0x7FFF, 0x7FD7, 0x7F7B, 0x7F3F, 0x7F1F, 0x7F1F, 0x675F, 0x539F,
    0x3FFF, 0x47B8, 0x4FF7, 0x67F5, 0x7FF5, 0x5AD6, 0x0000, 0x0000
};

__attribute__((always_inline)) static inline const u16 *vt_dac_table(void)
{
    if (vt_dac_variant == VT_DAC_DEFAULT)  return vt_compat_rgb555;
    if (vt_dac_variant == VT_DAC_VGPOCKET) return vt_compat_rgb555_vg;
    if (vt_dac_variant == VT_DAC_LLM)      return vt_compat_rgb555_llm;
    return (vt_reg_2010 & 0x40) ? vt_compat_rgb555_vg : vt_compat_rgb555;
}

__attribute__((always_inline)) static inline u16 nes_index_to_bgr555(u8 idx)
{
    // $2010 D6 selects the VG Pocket family (same signal as the 16-bit CHR
    // bus in section 24) -- its DAC palette differs from Star Ally's and
    // Lonely Island's console, so it gets its own table.  If a cart ever
    // needs one without the other, move both to a per-cart header flag.
    // s21b48: $2010 D6 is V16BEN -- the 16-bit CHR BUS WIDTH (guide section
    // 52).  It is NOT a DAC identifier.  It happens to be set on the VG
    // Pocket so it worked as a proxy there, but EVERY VT09-class console sets
    // it and they do not share the VG Pocket's LCD/DAC.  vt_dac_variant is
    // set at cart load from the NES 2.0 header and wins when non-zero; 0
    // keeps the old inference so already-wrapped images are unchanged.
    if (vt_active)
        return vt_dac_table()[idx & 0x3F];
    const unsigned char *p = &nes_rgb[(idx & 0x3F) * 3];
    u8 r = p[0], g = p[1], b = p[2];
    return (u16)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

__attribute__((target("arm")))
EWRAM_BSS static u32 vt_pal_built[VT_PALETTE_SIZE / 4];  /* inputs of the last build */
EWRAM_BSS static u16 vt_pal_built_2010;                    /* $2010 | 0x100 once built */
EWRAM_BSS u8 vt_vbl_outer;   /* ppu.s vblankinterrupt: inside_gba_vblank at entry */
static int vt_pal_same(void)
{
    if (vt_pal_built_2010 != (vt_reg_2010 | 0x100)) return 0;
    const u32 *src = (const u32 *)vt_palette_ram;
    for (int i = 0; i < VT_PALETTE_SIZE / 4; i++) if (vt_pal_built[i] != src[i]) return 0;
    return 1;
}
static void vt_build_16color_palette(void)
{
    // Only meaningful in 16-colour mode with COLCOMP=0.
    if (!(vt_reg_2010 & 0x06)) return;   // neither plane 16-colour
    // s21b59: COLCOMP (12-bit colour) is handled HERE too now -- it used to
    // bail, which left pixel values 4-15 pointing at palette entries nobody
    // filled, so 16-colour COLCOMP games (Aero Gyrodine, Hex City X:
    // $2010=$86) showed only the backdrop.  Same index scatter; the colour is
    // NintendulatorNRS GetPalIndex's Palette[TC|0x80]<<6 | Palette[TC]
    // through the 4096-entry vt03_palette_lut.
    const int colcomp = (vt_reg_2010 & 0x80) != 0;
    // SESSION 20b: gate each plane on ITS OWN mode bit.  Star Ally runs
    // BK16EN=1 with SP16EN=0 (4bpp background, 2bpp sprites); writing the
    // OBJ banks in that state stomps the legacy 2bpp sprite sub-palettes
    // that run_palette maintains for the stock sprite path.
    int do_bg  = (vt_reg_2010 & 0x02) != 0;   // BK16EN
    // SP16EN gates the OBJ banks.  An earlier s20b revision additionally
    // required PIX16EN=0 on the theory that 16-PIXEL sprites use the legacy
    // palettes -- empirically FALSE: Star Ally's title ($2010=$1F) authors
    // the menu cursor's yellow at $3F13, which only the extended sprite
    // index (scatter | bit4) reaches; gating on PIX16EN made the cursor
    // vanish.  PIX16EN halves still draw through the extended OBJ palette
    // space, so write the banks whenever SP16EN is set.
    // SESSION 21b5: SP16EN selects the sprite FETCH WIDTH, not which palette
    // space sprites resolve through.  Leaving the OBJ banks to the stock
    // run_palette path when SP16EN is clear was wrong: in VT mode that path
    // reads the legacy NES palette array, which nothing maintains here, so
    // every OBJ entry came out as NES colour $00 -- a flat grey (0x39CE).
    // That is why Star Ally's ship, enemies and stage art rendered grey as
    // soon as the 2bpp EVA sprites started drawing (s21b3).  With COLCOMP=0
    // the VT palette RAM is the only truth for sprites too, and for a 2bpp
    // sprite the scatter index degenerates to 0x10 + attr*4 + value (p2=p3=0)
    // -- exactly the classic $3F10 sprite-palette layout -- so the same loop
    // below produces the right colours for both widths.
    int do_obj = 1;

    volatile u16 *gba_bg  = (volatile u16*)0x05000000; // BG palette
    volatile u16 *gba_obj = (volatile u16*)0x05000200; // OBJ palette

    // SESSION 20b FINAL (empirically validated, three scenes vs reference
    // captures): the 4bpp palette-RAM index is the PLANE-SCATTERED layout --
    //   bit 0   = pattern plane 0          bit 1  = pattern plane 1
    //   bits2-3 = attribute (BUT: forced to ZERO while BKEXTEN is active,
    //             per the NESdev "VT02+ Video Modes" Address Extension
    //             clause: the attribute bits are consumed as EVA CHR-bank
    //             bits and "are forced to zero when forming the final
    //             palette index")
    //   bit 4   = 0 background / 1 sprite
    //   bits5-6 = pattern planes 2,3
    // Validation: Star Ally title rendered under candidate mappings and
    // scored against the user's reference capture -- plane-scatter with
    // forced-zero attr: d=4.8; linear(attr<<5) rotation: 12.0; linear
    // identity: 25.0; scatter WITHOUT forced-zero: 33.5.  Star Ally
    // gameplay (stars/nebula/HUD groups) and Lonely Island (all-attr-0
    // palette authored across ram[0x20/40/60] plane positions) agree.
    // An earlier s20b change to a linear index was WRONG for BG and is
    // reverted here; what it got right is retained: per-plane gating
    // (do_bg/do_obj above) and the PIX16EN sprite-mode gate.
    // Transparency: all pattern bits zero -> backdrop ((idx & 0x63) == 0).
    int bg_attr_forced0 = (vt_reg_2010 & 0x10) != 0;   // BKEXTEN

    /* s21b48 SPEED: the scatter index is a pure function of (group, v) and
     * bg_attr_forced0, and the DAC table is a pure function of vt_active /
     * vt_dac_variant / $2010 D6 -- none of which change within a frame.
     * Profiling Lucky Lawn Mower (VT09) put this function at 13.4% of ALL
     * executed ARM instructions because it recomputed the bit scatter and
     * re-selected the DAC table 128 times per frame.  Hoisting both took the
     * game from 38.7% to 67.2% of full speed; output is BYTE-IDENTICAL.
     *
     * DO NOT "optimise" further by skipping the rebuild when vt_palette_ram
     * is unchanged.  Tried in s21b48: it REGRESSED 98% of Star Ally's pixels.
     * Other code (run_palette, the legacy sprite path) also writes GBA
     * palette RAM, so this rebuild is partly a REPAIR of those writes, not
     * just a projection of vt_palette_ram.  It is NOT a pure function of its
     * inputs and must run every frame.
     *
     * EWRAM_BSS on the statics is mandatory: IWRAM has ~508 free bytes
     * between .bss and the usr stack (guide section 4), and putting these in
     * IWRAM .bss overflowed into the stacks -- the GBA died on an illegal
     * opcode.  s9's "all new C globals in EWRAM" rule covers function
     * statics too. */
    static EWRAM_BSS u8 idx_bg_tab[64], idx_sp_tab[64];
    static EWRAM_BSS int idx_tab_forced0;
    static EWRAM_BSS bool idx_tab_init;
    if (!idx_tab_init) { idx_tab_init = true; idx_tab_forced0 = -1; }
    if (idx_tab_forced0 != bg_attr_forced0) {
        idx_tab_forced0 = bg_attr_forced0;
        for (int group = 0; group < 4; group++)
            for (int v = 0; v < 16; v++) {
                int p0 = (v >> 0) & 1, p1 = (v >> 1) & 1;
                int p2 = (v >> 2) & 1, p3 = (v >> 3) & 1;
                int a_eff  = bg_attr_forced0 ? 0 : group;
                int idx_bg = p0 | (p1 << 1) | (a_eff << 2) | (p2 << 5) | (p3 << 6);
                int idx_sp = p0 | (p1 << 1) | (group << 2) | (1 << 4) | (p2 << 5) | (p3 << 6);
                if (!(idx_bg & 0x63)) idx_bg = 0;
                if (!(idx_sp & 0x63)) idx_sp = 0;
                idx_bg_tab[group * 16 + v] = (u8)(idx_bg & (VT_PALETTE_SIZE - 1));
                idx_sp_tab[group * 16 + v] = (u8)(idx_sp & (VT_PALETTE_SIZE - 1));
            }
    }

    const u16 *dac;
    static EWRAM_BSS u16 nes_dac[64];
    if (vt_active) {
        /* Must use the SAME rule as nes_index_to_bgr555 -- vt_dac_variant
         * first, $2010 D6 only as the fallback.  Duplicating the raw D6 test
         * here silently ignored the per-cart override (caught in s21b48). */
        dac = vt_dac_table();
    } else {
        for (int i = 0; i < 64; i++) nes_dac[i] = nes_index_to_bgr555((u8)i);
        dac = nes_dac;
    }

#ifdef VALUE_SENTINEL
    /* Calibration build (guide sections 31/58/64).  Encode the GBA palette
     * SLOT in the colour: r5 = i & 31, g5 = i >> 5.  Our value at each pixel
     * is then read DIRECTLY from the framebuffer instead of being inferred
     * through the DAC table -- that inference is what made section 64's
     * confusion matrix circular.  OBJ slots get g5 = 3, which the BG encoding
     * cannot produce, so sprite pixels mask out of the vote. */
    if (do_bg)
        for (int i = 0; i < 64; i++)
            gba_bg[i] = (u16)((i & 31) | ((i >> 5) << 5));
    if (do_obj)
        for (int i = 0; i < 64; i++)
            gba_obj[i] = (u16)((i & 31) | (3 << 5) | ((i >> 5) << 10));  /* b5 carries slot bit 5 */
#else
    if (colcomp) {
        #define VT_CC(ix) vt03_palette_lut[(((u32)(vt_palette_ram[(ix) | 0x80] & 0x3F) << 6) \
                                            | (vt_palette_ram[(ix)] & 0x3F)) & 0xFFF]
        if (do_bg)  for (int i = 0; i < 64; i++) gba_bg[i]  = VT_CC(idx_bg_tab[i]);
        if (do_obj) for (int i = 0; i < 64; i++) gba_obj[i] = VT_CC(idx_sp_tab[i]);
        #undef VT_CC
    } else {
    if (do_bg)
        for (int i = 0; i < 64; i++)
            gba_bg[i] = dac[vt_palette_ram[idx_bg_tab[i]] & 0x3F];
    if (do_obj)
        for (int i = 0; i < 64; i++)
            gba_obj[i] = dac[vt_palette_ram[idx_sp_tab[i]] & 0x3F];
    }
#endif
    /* what this build was made from, for vt_chr4_rebuild_if_dirty's skip */
    {   /* word loop: no library call (8-register push) on the vblank IRQ stack */
        const u32 *src = (const u32 *)vt_palette_ram;
        for (int i = 0; i < VT_PALETTE_SIZE / 4; i++) vt_pal_built[i] = src[i];
    }
    vt_pal_built_2010 = vt_reg_2010 | 0x100;
}

// Deferred heavy 4bpp assembly.  Call AT MOST ONCE PER FRAME (e.g. from the
// vblank/frame hook), NOT from the per-write path -- that was crash #2.
// Assembles full GBA-ready 4bpp (16-colour) tiles into vt_chr4_buf from the
// CHR page banks snapshotted at the last bank change.
//
// VT 4bpp tile layout (verified pixel-exact vs Furbtendulator h_OneBus.cpp
// reset split on Lonely Island + Star Ally): a 4bpp tile = 32 contiguous
// source bytes; bytes 0..15 = low 2bpp tile (plane0=0..7, plane1=8..15),
// bytes 16..31 = high 2bpp tile (plane2, plane3); pixel = p0|p1<<1|p2<<2|p3<<3.
// Spread table: byte -> 32-bit word with bit i of the byte placed at bit 0 of
// nibble (7-i), i.e. matching GBA 4bpp pixel order (leftmost pixel = low
// nibble). Built once. Lets the assembler build a tile row with 4 lookups +
// ORs instead of 8x per-bit shifting -- ~8x faster, so the full 8-page
// assembly fits the vblank IRQ time budget (the per-bit version overran it
// and tripped the crash handler).
#ifndef VT_SPR_SWAP16
#define VT_SPR_SWAP16 1
#endif
#ifndef VT_BG_SWAP16
#define VT_BG_SWAP16  1
#endif
EWRAM_BSS u32 vt_spread[256];
static u8 vt_spread_ready = 0;

__attribute__((target("arm"), noinline))
static void vt_spread_init(void)
{
    for (int b = 0; b < 256; b++) {
        u32 w = 0;
        for (int i = 0; i < 8; i++)
            if (b & (1 << i)) w |= 1u << ((7 - i) * 4);   // pixel (7-i) bit0
        vt_spread[b] = w;
    }
    vt_spread_ready = 1;
}

// Per-page signature word for the OBJ overlay's change detection: index of
// the first u32 in the page containing any high-plane pixel bits
// (word & 0xCCCCCCCC).  The 2bpp sprite-cache conversion emits the SAME
// GBA 4bpp nibble layout but with the high 2 bits of every pixel zero, so
// a slot stomped by render_a_kilobyte is GUARANTEED to differ from the
// 4bpp source at this word -- and a page with no high-plane detail anywhere
// (0xFFFF) renders identically from either source, so skipping it forever
// is exact, not an approximation.
// EWRAM_BSS: plain static .bss lands in IWRAM and shifts the fixed-address
// layout, tripping the boot canary alarm (known fragility; see the
// vt_spread history in MAINTAINERS_GUIDE.md s.74).
EWRAM_BSS static u16 vt_chr4_sigoff[8];

// Tight, fast 4bpp tile assembler using the spread table.
__attribute__((target("arm"), noinline))
static void vt_chr4_assemble(void)
{
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    u8 *g = vt_chr4_buf;
    const int wide16 = (vt_reg_2010 & 0x40) != 0;
    const u32 o1 = wide16 ? 16u : 8u;    /* plane 1 */
    const u32 o2 = wide16 ?  1u : 16u;   /* plane 2 */
    const u32 o3 = wide16 ? 17u : 24u;   /* plane 3 */
    const u32 rs = wide16 ?  2u : 1u;    /* row stride */
    for (int p = 0; p < 8; p++) {
        u32 src_base = (vt_chr_bank_byte_offset_n(vt_chr4_page_bank[p], 1) >> 10) * 2048u;
        u16 sig = 0xFFFF;
        u16 widx = 0;
        for (int t = 0; t < 64; t++, src_base += 32) {
            for (int r = 0; r < 8; r++, widx++) {
                /* s21b57: plane byte offsets are frame constants (o1..o3, rs set
                 * above the page loop).  8-bit bus: {+0,+8,+16,+24}, row stride 1
                 * -- byte-for-byte the old decode.  16-bit bus: {+0,+16,+1,+17},
                 * row stride 2 -- planes 1 and 2 SWAPPED versus the old decode
                 * (guide section 68).  Hoisting also keeps the 8-bit path's cost
                 * identical, so 8-bit carts are not re-timed by this change. */
                const u32 lo2 = src_base + r * rs;
                u32 row = vt_spread[cbase[lo2 & mask]]
                        | (vt_spread[cbase[(lo2 + o1) & mask]] << 1)
                        | (vt_spread[cbase[(lo2 + o2) & mask]] << 2)
                        | (vt_spread[cbase[(lo2 + o3) & mask]] << 3);
                // Signature: first word with high-plane bits (already in a
                // register -- one TST, replaces a 512-word post-scan that
                // overran the load-time IRQ budget).
                if (sig == 0xFFFF && (row & 0xCCCCCCCCu)) sig = widx;
                *g++ = (u8)row;       *g++ = (u8)(row >> 8);
                *g++ = (u8)(row >> 16); *g++ = (u8)(row >> 24);
            }
        }
        vt_chr4_sigoff[p] = sig;
    }
}

// Tight VRAM copy: vt_chr4_buf -> BG_VRAM at the cache's tile addressing.
//
// SESSION 13 -- THIS WAS THE LAG.  This ran unconditionally every guest frame,
// pushing 16KB from EWRAM into VRAM (4096 word loads at EWRAM's wait states).
// Measured: gating it out entirely dropped the frame cost from 1.55 GBA frames
// to exactly 1.00 -- i.e. this single copy was the whole difference between
// 39fps and 60fps.  It cannot simply be made dirty-driven, because PocketVT's
// inherited 2bpp tile cache re-converts and re-stomps BG VRAM pages whenever
// it feels like it, and our 4bpp tiles have to win.
//
// So do per-page exact change detection, exactly as vt_obj4_overlay already
// does for the sprite slots: one word compare per 2KB page against the
// signature offset (the first word that carries high-plane bits, computed in
// vt_chr4_assemble).  If that word still holds our 4bpp content, the 2bpp
// cache has not touched the page and the copy is skipped.  Pages whose sig is
// 0xFFFF have no high-plane bits at all, so their 2bpp and 4bpp conversions
// are identical and they never need copying.  Steady state: 8 word compares
// instead of 4096 word copies.
__attribute__((target("arm"), noinline))
static void vt_chr4_copy_to_vram(void)
{
    if (vt_bkexten_live) return;   // BG char VRAM is slot-managed (session 18)
    for (int p = 0; p < 8; p++) {
        const u32 *s = (const u32*)(vt_chr4_buf + (u32)p * 2048u);
        // Tiles p*64 .. p*64+63; ppu.s puts tiles >= 256 an extra 0x2000 up.
        u32 addr = 0x06000000u + (u32)p * 2048u + ((p >= 4) ? 0x2000u : 0u);
        u32 *d = (u32*)addr;

        // Probe several words rather than one.  NOTE: unlike the sprite path,
        // a BG page with no high-plane bits (sig == 0xFFFF) is NOT "identical
        // under both paths" -- the inherited 2bpp cache treats a tile as 16
        // bytes while a VT 4bpp tile is 32, so it reads different ROM bytes
        // altogether and its output never coincides with ours. Skipping those
        // pages painted the map with 2bpp garbage. Probe spread-out words so a
        // page of blank tiles can't produce a false match on a single zero.
        u16 sig = vt_chr4_sigoff[p];
        u32 i0 = (sig == 0xFFFF) ? 0u : sig;
        if (d[i0] == s[i0] && d[129] == s[129] &&
            d[257] == s[257] && d[385] == s[385])
            continue;                           // still our 4bpp content

        for (int i = 0; i < 512; i += 4) {
            u32 a = s[i], b = s[i+1], c = s[i+2], e = s[i+3];
            d[i] = a; d[i+1] = b; d[i+2] = c; d[i+3] = e;
        }
    }
}

// Unconditional full copy -- used right after a re-assemble, when every page's
// contents changed and the signature compare would be meaningless.
__attribute__((target("arm"), noinline))
static void vt_chr4_copy_to_vram_all(void)
{
    if (vt_bkexten_live) return;   // BG char VRAM is slot-managed (session 18)
    const u32 *s = (const u32*)vt_chr4_buf;
    for (int tile = 0; tile < 512; tile++, s += 8) {
        u32 addr = 0x06000000u + (u32)tile * 32u;
        if (tile & 0x100) addr += 0x2000u;          // matches ppu.s tst #0x100
        volatile u32 *d = (volatile u32*)addr;
        d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        d[4]=s[4]; d[5]=s[5]; d[6]=s[6]; d[7]=s[7];
    }
}

// (private stack buffer removed -- no longer used; the fast assembler fits the
// IRQ time budget so no stack switch is needed.)

__attribute__((target("arm"), noinline, used))
void vt_chr4_do_rebuild(void)
{
    vt_chr4_assemble();
    vt_chr4_copy_to_vram_all();
    vt_build_16color_palette();
}

// GBA 4bpp tile = 8 rows x 4 bytes, leftmost pixel in the low nibble.
// Called via bl_long from the ARM-mode vblank handler in ppu.s, so this MUST
// be compiled in ARM mode (bl_long = mov lr,pc; ldr pc,=label does NOT set
// the thumb bit). Switches to a private stack so the heavy work doesn't
// overflow the tiny vblank IRQ stack.
extern u8 vt_split_frame;
static void vt_split_repair(void);
__attribute__((target("arm")))
void vt_chr4_rebuild_if_dirty(void)
{
    if (vt_bkexten_live) {         // slots replace the page-linear pipeline;
        // Session 19: run the whole BKEXTEN batch with IME masked.  The
        // vblank handler re-enables IME early, so a slow pass here could be
        // NESTED by the next vblank on the same user stack -- which sits
        // directly above the timeout.s event-handler pointer table.  Star
        // Ally's wild-PC crash (PC=0x049563DC, LR in palette RAM) has the
        // signature of that table being trampled.  Masking costs at most a
        // delayed hblank effect for one line; a corrupted handler table
        // costs the machine.
        volatile u16 *ime = (volatile u16*)0x04000208;
        u16 saved = *ime; *ime = 0;
        // SESSION 20: the s19 restructure returned from this branch BEFORE
        // vt_chr_sync_flush() below, so vt_chr4_page_bank[] (populated ONLY
        // inside the flush) stayed all-zero under BKEXTEN.  Every slot then
        // assembled from bank (0<<3)|attr = PRG code/padding: Star Ally's
        // "black screen with fragments" (slot0 matched a bank-0 assembly
        // 512/512).  The flush is change-gated and <=1/frame; running it
        // here, inside the IME mask and BEFORE banks_recheck, both feeds the
        // snapshot and lets recheck reassemble any slot whose bank changed.
        vt_chr_sync_flush();
        if (vt_bk_pending_whole) { // deferred mode-flip rebuild, now spread
            // across frames by vt_bk_whole_step() so the 1920-cell sweep can't
            // overrun the vblank and re-enter the handler (see the note on the
            // stepper).  Stays pending until the full map has been swept.
            if (vt_bk_whole_step()) vt_bk_pending_whole = 0;
        }
        vt_bk_banks_recheck();     // catch page_bank changes made this frame
        vt_bk_frame_check();       // repair any 2bpp-cache stomps
        vt_bk_scrub();             // rotating map refresh (see below)
        *ime = saved;
        return;
    }
    // Apply any pending CHR sync exactly once per frame -- must run in ALL
    // modes (plain 2bpp VT games need their NES_VRAM window refreshed too),
    // so it sits before the 16-colour-only gate below.
    vt_chr_sync_flush();

    // Only active in 16-colour mode.
    if (!(vt_reg_2010 & 0x06)) return;

    // Re-assemble the 4bpp tiles only when CHR/palette actually changed
    // (dirty). But the GBA BG VRAM is continuously rewritten by PocketVT's
    // inherited 2bpp tile cache, so we must re-overlay our 4bpp tiles and
    // 16-colour palette into VRAM EVERY frame, or the 2bpp cache's output
    // overwrites ours after the first frame (-> screen reverts to backdrop).
    if (vt_chr4_dirty) {
        vt_chr4_dirty = 0;
        vt_chr4_assemble();
        vt_chr4_copy_to_vram_all();   // everything changed: unconditional
    } else {
        vt_chr4_copy_to_vram();       // cheap: re-copy only stomped pages
    }
    if (vt_split_frame) vt_split_repair();   /* s21b62: raster-split slots 2/3 */
    /* In a top-level vblank past the first frame, ppu.s then runs run_palette
     * and vt_16c_palette_fixup, which rebuilds this palette as the vblank's
     * LAST writer, so this build is overwritten unseen -- ~12% of Aero's title
     * frame (built twice per GBA vblank).  A nested vblank (vt_vbl_outer != 0)
     * exits before run_palette/fixup, and before firstframeready there is no
     * fixup either: build then.  Unchanged inputs need no build at all -- the
     * GBA palette still holds the last build's output (guide s.78). */
    if (!firstframeready || (vt_vbl_outer && !vt_pal_same())) vt_build_16color_palette();
}

// Public ARM-mode wrapper called from ppu.s AFTER run_palette each vblank.
// run_palette repaints the GBA palette from the legacy 32-byte nes_palette
// view whenever the game touched $3Fxx that frame; in 16-colour mode that
// clobbers the scattered-index sub-palettes, so this re-derives them as the
// frame's final palette writer.  vt_build_16color_palette self-gates on the
// $2010 16-colour bits and COLCOMP=0, making this a cheap no-op everywhere
// else.
static void vt_obj4_overlay(void);
static void vt_spr_eva_update(void);

// Debug input-injection byte for io.s (see no4scr hook).
EWRAM_BSS u8 vt_dbg_pad_or;

// ---------------------------------------------------------------------------
// Sprite Address Extension (SPEVA) -- the "glitched penguin" fix
// ---------------------------------------------------------------------------
// Per NESdev "VT02+ Video Modes", $2010 bit 3 (SPEXTEN) turns each sprite's
// OAM byte 2 bits 2-4 into an Extended Video Address that supplies the LOWEST
// THREE BITS of the CHR bank number for that sprite's pattern fetch.  The
// inner bank register for the PPU 1KB page supplies the higher bits, so the
// effective bank is (page_bank << 3) | EVA.
//
// PocketVT ignored EVA entirely: sprites were fetched from page_bank itself.
// For Lonely Island that meant the player's tiles were read from bank 6, which
// is background tile data -- 100% opaque, three distinct pixel values, no
// transparent border.  That is the dark blob on screen.  The real player art
// lives at (6 << 3) | 4 = bank 52, and a second actor at (6 << 3) | 2 = 50.
//
// Fix: give the extended sprite cache its own entries.  update_sprites (ppu.s)
// indexes spr_cache_map by bank number; PocketVT only ever uses bank numbers
// 0..7 (the PPU pages), so indices 64.. are free.  When vt_spr16_active is set,
// ppu.s looks up spr_cache_map[64 + page*8 + EVA] instead.  We fill those
// entries with OBJ cache slots 0..3, whose VRAM (0x06010000-0x06011FFF) the
// stock sprite cache -- which lives at slots 8..15 -- never touches.
EWRAM_BSS u8 vt_spr16_active;          // read by update_sprites in ppu.s
EWRAM_BSS u8 vt_pix16_active;          // read by update_sprites in ppu.s (s20b4)

#define VT_EVA_SLOTS 4
EWRAM_BSS static u16 vt_eva_key[VT_EVA_SLOTS];   // (page<<3)|eva, +1;  0 = empty
EWRAM_BSS static u32 vt_eva_age[VT_EVA_SLOTS];
EWRAM_BSS static u32 vt_eva_clock;

// Assemble one 1KB CHR page (64 tiles) straight into an OBJ VRAM slot as GBA
// 4bpp tiles.  Same pixel math as vt_chr4_assemble, no intermediate buffer.
__attribute__((target("arm"), noinline))
// Assemble one 64-tile page (2KB of GBA 4bpp data) from an EXTENDED
// (1KB-unit) OneBus bank number to an arbitrary VRAM destination.  This is
// the s14 SPEVA assembler, generalized so the BKEXTEN background path
// (session 18) can reuse it for BG char slots.
EWRAM_BSS u32 vt_asm_calls, vt_asm_from_framecheck, vt_asm_from_fill;
static void vt_assemble_page_to(u32 dest, u32 phys_bank, int swap16)
{
    vt_asm_calls++;
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    // SESSION 21: assemblers are only reached from extension-active paths
    // (BKEXTEN BG slots, SPEVA/PIX16 sprites), so compose per the wiki's
    // extension-ACTIVE formula: EVA | ((inner&mask | middle&~mask) << 3)
    // | (outer << 11), with NO intermediate ($2018.4-6) contribution.
    // Routing the precomposed (inner<<3)|EVA through vt_chr_bank_byte_offset
    // had two latent bugs: the inner mask truncated the composed number to
    // 8 bits (breaks inner banks >= 32) and intermediate<<8 was added even
    // though extension suppresses it.  Value-identical for SA/LI today
    // ($201A=0, $2018.4-6=0, $4100=0).
    u32 inner  = phys_bank >> 3, eva7 = phys_bank & 7u;
    u32 imask  = vt_inner_bank_mask();
    u32 middle = (u32)vt_chr_reg_201A & 0xF8u;
    u32 outer  = (u32)vt_chr_outer_4100 & 0x07u;  // 4bpp: outer is $4100.0-2
    u32 src_base = (eva7 | (((inner & imask) | (middle & ~imask)) << 3)
                         | (outer << 11)) * 2048u;   // final bank: 2KB units in 4bpp
    u32 *d = (u32*)dest;
    /* s21b54 SPEED: the bus-width bit and its derived stride are FRAME
     * constants, but were re-read from vt_reg_2010 twice per row -- 1024
     * redundant tests and loads per call, on a function measured at 27.8% of
     * all executed instructions (tools/arm_profile.c over 4M steps; note the
     * earlier 39.2% figure in section 59 came from a 300k-step window that
     * covered only ~2 frames and over-weighted one call).  Hoisting is
     * output-identical. */
    const u32 wide = (vt_reg_2010 & 0x40u);
    /* EXTENSION path (BKEXTEN BG slots, SPEVA sprites).  s21b57 left this path
     * unswapped because swapping scrambled the VG Pocket title logo; s21b58
     * showed the scramble was a CACHE STOMP, not the decode (see
     * vt_bk_slot_fill), and the swap is correct here too (guide section 69). */
    /* swap16: apply the section 68 p1/p2 swap on the 16-bit bus.  Proven on
     * every path (guide section 69): non-extension BG table-free; sprites by
     * Lucky Lawn Mower's mower; BKEXTEN background by the VG Pocket title.
     * The flag is kept only so a future cart can be diagnosed per path. */
    const u32 o1 = wide ? (swap16 ? 16u : 1u) : 8u;   /* plane 1 */
    const u32 o2 = wide ? (swap16 ?  1u : 16u) : 16u; /* plane 2 */
    const u32 o3 = wide ? 17u : 24u;    /* plane 3 */
    const u32 rs = wide ?  2u : 1u;     /* row stride */
    const u8 *const rb = cbase;
    const u32 *const sp = vt_spread;
    for (int t = 0; t < 64; t++, src_base += 32) {
        for (int r = 0; r < 8; r++) {
            // SESSION 21b15: two plane layouts exist (wiki "VT02+ CHR-ROM
            // Bankswitching", final-address diagrams).  8-BIT BUS puts the
            // tile row in address bits 0-2 and the plane in bits 3-4, i.e.
            // planes at +0/+8/+16/+24.  16-BIT BUS interleaves them: bit 0 =
            // plane D0, bits 1-3 = row, bit 4 = plane D1, i.e. row r lives at
            // +2r/+2r+1/+16+2r/+16+2r+1.  Reading a 16-bit-bus cart with the
            // 8-bit layout pulls each row's halves from different rows, which
            // renders as heavy horizontal striping -- measured on the VG
            // Pocket menu: 89.6 differing pixels per adjacent row pair versus
            // 31.3 in the reference capture; the 16-bit layout gives 33.2.
            //
            // The bus width is a board property with no documented register.
            // EMPIRICAL GATE: $2010 D6, which the VT03 datasheet lists as
            // unused, is SET on the 16-bit-bus cart (VG Pocket, $5E) and
            // CLEAR on both 8-bit-bus carts (Star Ally $1F, Lonely Island
            // $0E).  If a counter-example turns up, replace this with a
            // per-cart flag in builder.py's injected header rather than
            // widening the guess.
            /* s21b57: frame-constant plane offsets (original decode; see the
             * note above -- the section 68 swap is NOT applied on this path). */
            const u32 lo = src_base + r * rs;
            *d++ = sp[rb[lo & mask]]
                 | (sp[rb[(lo + o1) & mask]] << 1)
                 | (sp[rb[(lo + o2) & mask]] << 2)
                 | (sp[rb[(lo + o3) & mask]] << 3);
        }
    }
}

static void vt_eva_assemble(int slot, u32 phys_bank)
{
    vt_assemble_page_to(0x06010000u + (u32)slot * 2048u, phys_bank, VT_SPR_SWAP16);
}

// 2bpp SPEVA sprite assembler -- session 21b3 (the "2bpp-EVA gap", guide 8b).
// SPEXTEN=1 with SP16EN=0 (Star Ally's $2010 = $1A gameplay/attract stages):
// the sprite fetch is an ordinary 2bpp 16-byte tile, but it STILL goes through
// extension addressing, so the low three bank bits come from OAM byte2 bits
// 2-4.  Per the VT03 datasheet p.11 the extension address is
//   ($4100&F)<<21 + VBANK<<13 + EVA<<10
// with the extra one-bit LEFT shift applying only to 16-colour/4bpp fetches --
// so here the composed bank counts 1KB units (a page = 64 tiles x 16 bytes),
// where vt_assemble_page_to's 4bpp version counts 2KB units.  Output is plain
// 2-bit values 0..3 in GBA 4bpp nibbles: with SP16EN clear, vt_build_16color_
// palette deliberately leaves the OBJ banks as the legacy sub-palettes that
// run_palette maintains, and the OBJ attribute's palette field (OAM byte2
// bits 0-1) selects among them exactly as on the stock 2bpp path.
__attribute__((target("arm"), noinline))
static void vt_assemble_page_2bpp_to(u32 dest, u32 phys_bank)
{
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    u32 inner  = phys_bank >> 3, eva7 = phys_bank & 7u;
    u32 imask  = vt_inner_bank_mask();
    u32 middle = (u32)vt_chr_reg_201A & 0xF8u;
    u32 outer  = (u32)vt_chr_outer_4100 & 0x0Fu;
    u32 src_base = (eva7 | (((inner & imask) | (middle & ~imask)) << 3)
                         | (outer << 11)) * 1024u;   // 1KB units in 2bpp
    u32 *d = (u32*)dest;
    for (int t = 0; t < 64; t++, src_base += 16) {
        for (int r = 0; r < 8; r++) {
            u32 lo = src_base + r;
            *d++ = vt_spread[cbase[lo & mask]]
                 | (vt_spread[cbase[(lo + 8) & mask]] << 1);
        }
    }
}

// PIX16EN ($2010 bit 0) sprite assembler -- session 20b4.
// With PIX16EN set a sprite fetch still reads a 4bpp (32-byte) tile, but the
// four bits of each pixel split into TWO horizontally adjacent 2-bit pixels:
// planes 0/1 are the LEFT eight pixels, planes 2/3 the RIGHT eight.  Emit
// TWO GBA tiles per VT tile -- [L(t), R(t)] -- so a 16x16 OBJ's four tiles
// sit consecutively for 1D mapping: pair (t even, t|1) occupies GBA tiles
// 2t..2t+3 = [TL, TR, BL, BR].  Nibble values are chosen to index the
// EXISTING scattered extended OBJ palette banks (see vt_build_16color_palette
// and its s20b empirical note: PIX16EN halves draw through the extended
// space, the menu cursor's yellow is authored at $3F13 = scatter p0|p1|bit4):
//   left  half pixel -> nibble p0|(p1<<1)   right half -> nibble p2|(p3<<1)
// BOTH halves are plain 2-bit pixels indexing entries 0..3 of the sprite's
// palette bank -- verified against the 11.png title reference: the cursor's
// RIGHT half is the same yellow/olive family as its left, i.e. both halves
// resolve through $3F10+pal*4+value.  (A first attempt put the right half's
// bits at nibble positions 2-3 -- the plane-scatter positions $3F30/50/70 --
// and the reference shows those hold grey: wrong.)  Entries 1..3 of each
// scattered bank coincide with the classic $3F11-13 colours, so the existing
// palette fixup already supplies the correct values for both halves.
// Nibble 0 = GBA-transparent, matching each half's own 2 bits == 0.
// A page becomes 128 GBA tiles = 4KB, so PIX16 OBJ slots stride 4096 bytes
// (tiles 0..511, still clear of the stock cache at tiles 512+).
__attribute__((target("arm"), noinline))
static void vt_assemble_page_pix16_to(u32 dest, u32 phys_bank)
{
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    // SESSION 21: assemblers are only reached from extension-active paths
    // (BKEXTEN BG slots, SPEVA/PIX16 sprites), so compose per the wiki's
    // extension-ACTIVE formula: EVA | ((inner&mask | middle&~mask) << 3)
    // | (outer << 11), with NO intermediate ($2018.4-6) contribution.
    // Routing the precomposed (inner<<3)|EVA through vt_chr_bank_byte_offset
    // had two latent bugs: the inner mask truncated the composed number to
    // 8 bits (breaks inner banks >= 32) and intermediate<<8 was added even
    // though extension suppresses it.  Value-identical for SA/LI today
    // ($201A=0, $2018.4-6=0, $4100=0).
    u32 inner  = phys_bank >> 3, eva7 = phys_bank & 7u;
    u32 imask  = vt_inner_bank_mask();
    u32 middle = (u32)vt_chr_reg_201A & 0xF8u;
    u32 outer  = (u32)vt_chr_outer_4100 & 0x07u;  // 4bpp: outer is $4100.0-2
    u32 src_base = (eva7 | (((inner & imask) | (middle & ~imask)) << 3)
                         | (outer << 11)) * 2048u;   // final bank: 2KB units in 4bpp
    u32 *d = (u32*)dest;
    for (int t = 0; t < 64; t++, src_base += 32) {
        for (int r = 0; r < 8; r++) {              // LEFT half: planes 0/1
            u32 lo = src_base + r;
            *d++ = vt_spread[cbase[lo & mask]]
                 | (vt_spread[cbase[(lo + 8) & mask]] << 1);
        }
        for (int r = 0; r < 8; r++) {              // RIGHT half: planes 2/3
            u32 hi = src_base + r + 16;
            *d++ = vt_spread[cbase[hi & mask]]
                 | (vt_spread[cbase[(hi + 8) & mask]] << 1);
        }
    }
}

// ===========================================================================
// BKEXTEN -- Background Address Extension ($2010 bit 4), session 18.
//
// Per the NESdev "VT02+ Video Modes" page, the background's three Extended
// Video Address bits are  P,A1,A0  where A1:A0 are the tile's attribute bits
// and P = ($2011 bit0 EVA12S == 0 ? $2018 bit3 BKPAGE : $4106 bit0 HV).  The
// effective CHR bank of a background fetch becomes (page_bank << 3) | EVA,
// and because the attribute bits are consumed as tile-number bits, the
// palette set of every background pixel is forced to ZERO.
//
// GBA mapping: the same nametable cell can now show a different tile for
// each attribute value, so one GBA tile per NES name is not enough.  We
// allocate 64-tile "slots" of BG character VRAM keyed by (page, attr).  The
// free regions of BG VRAM (everything else is spoken for -- 0x2000-0x3FFF is
// the UI layer, 0x6000-0x6FFF the tilemap, 0x8000-0xFFFF the guest PRG
// window) give ten such slots:
//     GBA tile indices   0..255   (0x0000-0x1FFF)  4 slots
//     GBA tile indices 512..767   (0x4000-0x5FFF)  4 slots
//     GBA tile indices 896..1023  (0x7000-0x7FFF)  2 slots (screenblocks
//                       14-15, unused: VT mirroring is only ever 2-screen)
// Star Ally needs three (page 0 x attrs 0-2).  Slots are (approximately)
// LRU-evicted; if a scene ever needs more than ten live (page,attr) pairs,
// evicted-but-still-displayed cells go stale until rewritten -- accepted
// limitation, revisit if a game hits it.
//
// The map entry itself is written by vt_bk_write_cell(): GBA tile index =
// slot base + (name & 63), palette nibble = 0.  The ppu.s BG-cache consumer
// and whole-map redraw branch to vt_bk_consume()/vt_bk_whole() when
// vt_bkexten_live is set (the legacy paths split a cell's low byte (name)
// and high byte (attr) between two writers, which cannot express a
// slot-packed index).
// ===========================================================================
EWRAM_BSS u8 vt_reg_2011;
EWRAM_BSS u8 vt_bkexten_live;

extern const u32 vt_bk_consts[4];   // ppu.s: BG_CACHE, NES_VRAM2, NES_VRAM4, AGB_BG
extern u8 _bg_cache_full;           // ppu.s storage
extern u8 _ppuctrl0;                // $2000 shadow (ppu.s storage)
extern u8 vt_mirror_value;          // $4106 bit0 (vt_regs.c, session 15)

#define VT_BK_SLOTS 10
static const u16 vt_bk_slot_idx[VT_BK_SLOTS] =
    { 0, 64, 128, 192, 512, 576, 640, 704, 896, 960 };
EWRAM_BSS u8  vt_bk_slot_key[VT_BK_SLOTS];    // (page<<2)|attr; 0xFF = empty
EWRAM_BSS u16 vt_bk_slot_bank[VT_BK_SLOTS];   // extended bank last assembled
EWRAM_BSS u32 vt_bk_slot_sig[VT_BK_SLOTS];    // value at sig word (stomp check)
EWRAM_BSS u16 vt_bk_slot_sigoff[VT_BK_SLOTS]; // index of first NONZERO word; 0xFFFF = page all-zero
EWRAM_BSS u8  vt_bk_slot_age[VT_BK_SLOTS];
EWRAM_BSS u8  vt_bk_clock;
EWRAM_BSS u8  vt_bk_pending_whole;   // whole-map rebuild deferred to vblank
EWRAM_BSS u16 vt_bk_lut[32];         // (page<<2|attr)&31 -> slot base tile, 0xFFFF=miss
EWRAM_BSS u32 vt_bk_dbg[8];          // 0 flips 1 inval 2 wholes 3 allocs 4 cells 5 attrN0 6 lastab 7 lastattr

__attribute__((section(".iwram.vtbk")))
static u32 vt_bk_pbit(void)
{
    return (vt_reg_2011 & 1) ? (u32)(vt_mirror_value & 1)
                             : (u32)((vt_chr_reg_2018 >> 3) & 1);
}

// Slot keys are stored BIASED (+1): 1..17 = live (page<<2|attr)+1, while
// BOTH 0x00 and 0xFF mean "empty".  Something in the inherited engine zeroes
// a few bytes of this table (observed: bytes 0-2 reset to 0x00 after
// allocation -- stomper not yet identified); with the biased encoding a
// zero-stomp degrades to "slot forgotten" instead of "slot claims
// (page0,attr0)", and the per-vblank scrub rewrites any map entries that
// pointed at a forgotten slot within four frames.  Self-healing beats
// silently-wrong.
void vt_bk_invalidate(void)
{
    vt_bk_dbg[1]++;
    for (int s = 0; s < VT_BK_SLOTS; s++) vt_bk_slot_key[s] = 0;
    for (int i = 0; i < 32; i++) vt_bk_lut[i] = 0xFFFF;
}

void vt_bk_lut_refresh(void);

static void vt_bk_slot_fill(int s, u32 bank)
{
    u32 dest = 0x06000000u + (u32)vt_bk_slot_idx[s] * 32u;
    vt_bk_slot_bank[s] = (u16)bank;
    vt_assemble_page_to(dest, bank, VT_BG_SWAP16);
    // SESSION 20: signature = first NONZERO word.  Word 0 alone is blind to
    // zero-stomps whenever tile0 row0 is legitimately blank (Star Ally's
    // attr-0 slot: first art at word 13) -- the 2bpp cache zeroed the slot
    // and frame_check compared 0==0 forever.  Mirrors vt_chr4_sigoff.
    // s21b58: PREFER A WORD WITH HIGH-PLANE BITS (0xCCCCCCCC = pixel-value
    // bits 2-3).  PocketNES's 2bpp cache can only ever write values 0-3, so it
    // cannot reproduce such a word: a stomp is ALWAYS caught.  "First nonzero
    // word" alone let a 2bpp stomp slip through whenever it happened to write
    // the same value at that one word -- with the section 68 plane order that
    // coincidence hit the VG Pocket title, and PocketNES's 2bpp tiles showed
    // through the "Pocket" logo (guide section 69).  Pages with no high-plane
    // bits at all fall back to first-nonzero, which still catches zero-stomps.
    // vt_chr4_assemble has always chosen its signature this way.
    const volatile u32 *d = (const volatile u32*)dest;
    u16 off = 0xFFFF, offnz = 0xFFFF;
    for (int w = 0; w < 512; w++) {
        u32 x = d[w];
        if (!x) continue;
        if (offnz == 0xFFFF) offnz = (u16)w;
        if (x & 0xCCCCCCCCu) { off = (u16)w; break; }
    }
    if (off == 0xFFFF) off = offnz;
    vt_bk_slot_sigoff[s] = off;
    vt_bk_slot_sig[s]    = (off == 0xFFFF) ? 0u : d[off];
}

static inline u32 vt_bk_eva(u32 attr)
{
    // EVA = {P, attr1, attr0} exactly as the VT03 datasheet Table A5 and the
    // NESdev wiki state (P = BKPAGE or the $4106 HV bit per $2011 EVA12S).
    // SESSION 21b2: the s21b1 "BKPAGE suppresses attr" model is REVERTED --
    // it was fitted to a symptom whose real cause was the nametable
    // arrangement (header mirror bit clobbering the $4106 default in
    // cart.s), and it blanked legitimate attr-selected banks (menu earth,
    // HUD glyphs).  Do not re-add without datasheet evidence.
    return (vt_bk_pbit() << 2) | (attr & 3u);
}

static int vt_bk_slot_get(u32 page, u32 attr)
{
    u32 key  = ((page << 2) | attr) + 1u;          // biased; see note above
    // Bank composition per the VT03 datasheet extension formula (2 KB / 4bpp
    // units): bank = VBANK*8 + EVA, EVA = {P, attr1, attr0}.  The session-21
    // note that stood here argued BKPAGE suppresses attr; that was a
    // misdiagnosis (see vt_bk_eva) -- the orange band's real cause was the
    // stacked-vs-side-by-side nametable arrangement, fixed in cart.s.
    u32 bank = ((u32)vt_chr4_page_bank[page] << 3) | vt_bk_eva(attr);
    int freeslot = -1, oldest = 0;
    for (int s = 0; s < VT_BK_SLOTS; s++) {
        if (vt_bk_slot_key[s] == key) {
            if (vt_bk_slot_bank[s] != (u16)bank) vt_bk_slot_fill(s, bank);
            vt_bk_slot_age[s] = ++vt_bk_clock;
            return s;
        }
        u8 k = vt_bk_slot_key[s];
        if ((k == 0 || k == 0xFF) && freeslot < 0) freeslot = s;
    }
    // approximate LRU: pick the slot with the largest (clock - age) distance
    if (freeslot < 0) {
        u8 bestd = 0;
        for (int s = 0; s < VT_BK_SLOTS; s++) {
            u8 d = (u8)(vt_bk_clock - vt_bk_slot_age[s]);
            if (d >= bestd) { bestd = d; oldest = s; }
        }
        freeslot = oldest;
    }
    vt_bk_dbg[3]++;
    vt_bk_slot_key[freeslot] = (u8)key;
    vt_bk_slot_age[freeslot] = ++vt_bk_clock;
    vt_bk_slot_fill(freeslot, bank);
    return freeslot;
}

// Re-check every live slot's effective bank (page_bank / BKPAGE / HV / EVA12S
// changed) and reassemble in place; map entries stay valid.
void vt_bk_banks_recheck(void)
{
    if (!vt_bkexten_live) return;
    for (int s = 0; s < VT_BK_SLOTS; s++) {
        u8 k = vt_bk_slot_key[s];
        if (k == 0 || k == 0xFF) continue;
        u32 kk = k - 1u;
        u32 page = kk >> 2, attr = kk & 3;
        u32 bank = ((u32)vt_chr4_page_bank[page] << 3) | vt_bk_eva(attr);
        if (vt_bk_slot_bank[s] != (u16)bank) vt_bk_slot_fill(s, bank);
    }
}

// The inherited 2bpp tile cache can still write over BG char VRAM (the s13
// lesson).  Once per vblank, verify each live slot's first word and
// reassemble any slot that was stomped.
void vt_bk_frame_check(void)
{
    for (int s = 0; s < VT_BK_SLOTS; s++) {
        u8 k = vt_bk_slot_key[s];
        if (k == 0 || k == 0xFF) continue;
        u16 off = vt_bk_slot_sigoff[s];
        if (off == 0xFFFF) continue;   // page genuinely all-zero: nothing to protect
        u32 dest = 0x06000000u + (u32)vt_bk_slot_idx[s] * 32u;
#ifdef FORCE_BK_REPAIR
        if (1)   /* DIAGNOSTIC: re-assemble every extension slot every frame */
#else
        if (((const volatile u32*)dest)[off] != vt_bk_slot_sig[s])
#endif
            { vt_asm_from_framecheck++; vt_assemble_page_to(dest, vt_bk_slot_bank[s], VT_BG_SWAP16); }
    }
}

// Fast path for bulk passes (scrub / whole): a (page<<2|attr) -> slot-base
// lookup table, refreshed from the slot table once per pass.  The general
// vt_bk_slot_get is only consulted on a LUT miss (0xFFFF), which allocates
// the slot and repairs the LUT entry.  Bulk passes also skip the VRAM write
// when the entry is already correct -- with a static screen that turns the
// whole sweep into reads.  (First version scanned the slot table per cell
// and wrote unconditionally: Star Ally fell from 40.6 to 12.2 fps.  Session
// 18 lesson: 480 x anything is a hot loop.)
void vt_bk_lut_refresh(void)
{
    for (int i = 0; i < 32; i++) vt_bk_lut[i] = 0xFFFF;
    for (int s = 0; s < VT_BK_SLOTS; s++) {
        u8 k = vt_bk_slot_key[s];
        if (k == 0 || k == 0xFF) continue;
        vt_bk_lut[(k - 1u) & 31] = vt_bk_slot_idx[s];
    }
}

// SESSION 20b PERF: hottest non-core function (~290 calls/frame pre-diet,
// ~90-insn Thumb body) -- executing from 16-bit waitstated cart ROM cost
// ~18% of host time.  IWRAM fetches are 32-bit zero-wait: ~2x per call.
__attribute__((section(".iwram.vtbk")))
static void vt_bk_write_cell(u32 o)          // o = resolved NT offset 0..0x7FF
{
    u32 t = o & 0x3FF;
    if (t >= 0x3C0) return;                  // attribute area itself
    const u8 *nes = (const u8*)vt_bk_consts[1];
    u32 scr  = o & 0x400;
    u32 n    = nes[o];
    u32 col  = t & 31, row = t >> 5;
    u32 ab   = nes[scr + 0x3C0 + ((row >> 2) << 3) + (col >> 2)];
    u32 sh   = ((row & 2) << 1) | (col & 2);
    u32 attr = (ab >> sh) & 3;
    u32 page = (n >> 6) + ((_ppuctrl0 & 0x10) ? 4u : 0u);
    u32 key  = (page << 2) | attr;
    u32 base = vt_bk_lut[key & 31];
    if (base == 0xFFFF) {
        int s = vt_bk_slot_get(page, attr);
        base = vt_bk_slot_idx[s];
        vt_bk_lut[key & 31] = (u16)base;
    }
    volatile u16 *e = &((volatile u16*)vt_bk_consts[3])[o];
    // SESSION 20b: the attribute selects the GBA sub-palette (entry bits
    // 12-13), matching the corrected LINEAR palette layout in
    // vt_build_16color_palette (attr = palette-index bits 5-6 = GBA bank).
    // History: this was attempted earlier against the old SCATTERED layout
    // and reverted because banks 1-3 then held garbage (the scatter left
    // ram[0x20+] zeros there) and everything attr!=0 went dark.  With the
    // linear layout those banks hold the game's real $3F20+/$3F40+/$3F60+
    // colours, so the attribute bank bits are required for correctness
    // (Star Ally's title uses attr 1-3 tiles for the planet shading).
    u16 want = (u16)((base + (n & 63)) | (attr << 12));
    if (*e != want) *e = want;
}

// Replacement for the ppu.s BG-cache consumer loop.  cur/lim are byte
// cursors into the 512-byte BG_CACHE ring of halfword NT offsets.
// SESSION 20: MUST be ARM mode.  consume_bg_cache's BKEXTEN branch calls
// this via bl_long (mov lr,pc; ldr pc,=vt_bk_consume) from IWRAM asm, and
// on ARM7TDMI an ldr-to-pc does NOT interwork -- it jumps to the address in
// ARM state with the Thumb bit ignored.  Compiled as Thumb, the very first
// real invocation (mid-frame PPU writes from Star Ally's timer ISR finally
// putting entries in the ring) executed Thumb code as ARM instructions and
// careened into the appended NES ROM (the wild-PC soft-hang at first timer
// IRQ: pc=0x0804D4BC, lr=0x03006930).  Dormant until then because the ring
// was empty at every earlier consume.  Same constraint and fix as
// vt_chr4_rebuild_if_dirty above; audited all 79 bl_long/b_long targets --
// this was the only Thumb one.
EWRAM_BSS static u16 vt_bk_attr_shadow[128];   // last consumed attr byte per screen (0x100 = never)

void vt_bk_attr_shadow_reset(void)
{
    for (int i = 0; i < 128; i++) vt_bk_attr_shadow[i] = 0x100;
}

__attribute__((target("arm")))
void vt_bk_consume(u32 cur, u32 lim)
{
    volatile u16 *ime = (volatile u16*)0x04000208;   // see session-19 note
    u16 saved = *ime; *ime = 0;
    vt_bk_dbg[2] += 0x10000;   // high half: consume-entry count
    vt_bk_lut_refresh();
    const u16 *ring = (const u16*)vt_bk_consts[0];
    while (cur != lim) {
        u32 e = ring[cur >> 1];
        cur = (cur + 2) & (512u - 1u);
        if (e & 0x800) continue;             // 4-screen: never on VT boards
        u32 t = e & 0x3FF;
        if (t >= 0x3C0) {                    // attribute byte: 4x4 cell area
            u32 scr = e & 0x400;
            u32 a = t - 0x3C0;
            // SESSION 20b PERF: Star Ally's timer ISR rewrites attribute
            // bytes every frame with UNCHANGED values; each ring entry then
            // recomputed 16 cells (profiled: write_cell body ~18% of all
            // host time, dominated by this amplification).  Shadow the last
            // value consumed per attr byte and skip redundant rewrites --
            // a genuine change still recomputes all 16 cells.  u16 shadow
            // entries start at 0x100 (impossible byte) so the first
            // consume of each byte always processes; the whole-map sweep
            // keeps cells honest regardless.
            u32 sidx = (scr ? 64u : 0u) + a;
            u8 curv = ((const u8*)vt_bk_consts[1])[scr + t];
            if (vt_bk_attr_shadow[sidx] == (u16)curv) continue;
            vt_bk_attr_shadow[sidx] = (u16)curv;
            u32 ax = (a & 7) << 2, ay = (a >> 3) << 2;
            for (u32 ry = 0; ry < 4 && ay + ry < 30; ry++)
                for (u32 rx = 0; rx < 4; rx++)
                    vt_bk_write_cell(scr + (ay + ry) * 32 + ax + rx);
        } else {
            u32 o = e & ~1u;
            vt_bk_write_cell(o);
            vt_bk_write_cell(o + 1);
        }
    }
    *ime = saved;
}

// Rotating whole-map refresh: one quarter of both nametables per vblank.
//
// Why this exists: PocketNES's incremental BG cache is not a reliable feed
// for VT games.  The producer (writeBG in ppu.s) SELF-MODIFIES into "bx lr"
// when the 256-entry ring fills -- which a VT title's boot-time video-DMA
// screen blast does instantly -- and the re-enable
// (set_bg_cache_available) only happens on a whole-map redraw that is
// itself gated behind bg_cache_updateok.  Star Ally never got a single
// cache entry through in 420 frames.  Rather than re-plumb that machinery,
// BKEXTEN mode sweeps the map continuously: 480 cells/frame means any
// nametable change is on screen within 4 frames, no matter which upload
// path (per-$2007, video DMA, stack blast) the game used.  Cost is ~1ms of
// the 4.9ms vblank; vt_bk_write_cell is idempotent so sweeping clean cells
// is harmless.
EWRAM_BSS u8 vt_bk_scrub_phase;
void vt_bk_scrub(void)
{
    // SESSION 20b PERF: 60 cells/frame (one sixteenth of ONE screen,
    // screens alternating; full coverage every 32 frames).  The sweep is a
    // SAFETY NET -- the ring (vt_bk_consume, measured live at ~1 call/frame
    // in Star Ally gameplay) delivers real nametable changes immediately.
    // The previous 240 cells/frame put vt_bk_write_cell at ~290 calls/frame
    // = the largest single non-CPU-core cost in the profile (~18% of host
    // time; the body executes from waitstated cart ROM).
    u32 q = vt_bk_scrub_phase & 31;
    vt_bk_scrub_phase++;
    vt_bk_lut_refresh();
    u32 scr = (q & 1) ? 0x400u : 0u;
    u32 lo  = (q >> 1) * 60u, hi = lo + 60u;    // 960/16 cells
    for (u32 t = lo; t < hi; t++)
        vt_bk_write_cell(scr + t);
}

void vt_bk_whole(void)
{
    vt_bk_dbg[2]++;
    vt_bk_lut_refresh();
    for (u32 scr = 0; scr < 0x800; scr += 0x400)
        for (u32 t = 0; t < 0x3C0; t++)
            vt_bk_write_cell(scr + t);
}

// SESSION 20: chunked whole-map rebuild.  vt_bk_whole() above sweeps all
// 2*960 map cells in one call; at a BKEXTEN mode flip almost every cell misses
// the 10-slot cache, so it fires a burst of vt_assemble_page_to() ROM
// assembles that runs far past the ~4.9ms vblank.  Even with IME masked, the
// overrun means the frame is missed and the vblank handler re-enters
// (inside_gba_vblank 0->1->2), trampling the user stack and the timeout.s
// event-handler pointer table -> the Star Ally wild-PC crash on Start.  This
// stepper walks the same 1920 cells but only VT_BK_WHOLE_CHUNK per vblank,
// tracked by vt_bk_whole_cur, so no single frame overruns; the caller keeps
// vt_bk_pending_whole set until this returns 1.  The unique-slot assembles are
// naturally spread across chunks as the sweep reaches new (page,attr) regions.
#define VT_BK_WHOLE_TOTAL 1920u          // 2 screens * 0x3C0 drawable cells
#define VT_BK_WHOLE_CHUNK  128u          // cells per vblank -> ~15 frames total
EWRAM_BSS u16 vt_bk_whole_cur;
static int vt_bk_whole_step(void)
{
    vt_bk_lut_refresh();
    u32 i   = vt_bk_whole_cur;
    u32 end = i + VT_BK_WHOLE_CHUNK;
    if (end > VT_BK_WHOLE_TOTAL) end = VT_BK_WHOLE_TOTAL;
    for (; i < end; i++) {
        u32 scr = (i >= 960u) ? 0x400u : 0u;
        u32 t   = (i >= 960u) ? (i - 960u) : i;
        vt_bk_write_cell(scr + t);
    }
    if (i >= VT_BK_WHOLE_TOTAL) { vt_bk_whole_cur = 0; vt_bk_dbg[2]++; return 1; }
    vt_bk_whole_cur = (u16)i;
    return 0;
}

__attribute__((target("arm"), noinline))
static void vt_spr_eva_update(void)
{
    // Need 4bpp sprites (SP16EN, bit 2) AND address extension (SPEXTEN, bit 3),
    // with the compatibility palette (COLCOMP=0).  Otherwise leave the stock
    // sprite path completely alone.
    // Need 4bpp sprites (SP16EN, bit 2) AND address extension (SPEXTEN, bit 3),
    // with the compatibility palette (COLCOMP=0).  Otherwise leave the stock
    // sprite path completely alone.
    // SESSION 21b3 -- the 2bpp-EVA gap (guide 8b) is now CLOSED.  SPEXTEN
    // (bit 3) alone puts sprites on extension addressing; SP16EN (bit 2)
    // selects the FETCH WIDTH, not whether EVA applies:
    //   SPEXTEN=1 SP16EN=1 -> 4bpp EVA pages   (SA menu, $2010=$1F; PIX16EN
    //                                           additionally splits halves)
    //   SPEXTEN=1 SP16EN=0 -> 2bpp EVA pages   (SA gameplay/attract, $1A)
    // In the 2bpp mode ONLY eva!=0 sprites are redirected: eva==0 sprites are
    // left on the stock bankbuffer path, which is a different bank source and
    // renders them correctly today, so redirecting them would be an unverified
    // change to already-good output (this is what sank the s20b4 prototype).
    // vt_spr16_active therefore carries a MODE, not a boolean: 0 = off,
    // 1 = redirect every sprite, 2 = redirect only sprites with EVA != 0.
    if (!vt_active || !(vt_reg_2010 & 0x08) || (vt_reg_2010 & 0x80)) {
        vt_spr16_active = 0;
        vt_pix16_active = 0;
        return;
    }
    int sp16 = (vt_reg_2010 & 0x04) != 0;

    const u8 *oam = (const u8 *)_dmanesoambuff;
    if (!oam) { vt_spr16_active = 0; vt_pix16_active = 0; return; }

    // PIX16EN ($2010 bit 0): 16-pixel-wide 2bpp halves.  Slots switch to the
    // 4KB pair-format; invalidate all assembled slots on any format flip so
    // stale 2KB-format data is never displayed through the doubled index.
    int pix16 = sp16 && (vt_reg_2010 & 0x01);
    {
        // Slot contents differ per format (2bpp / 4bpp / pix16 pair-format),
        // so invalidate every slot on ANY format flip, not just a pix16 flip.
        u8 fmt = (u8)(pix16 ? 2 : (sp16 ? 1 : 0));
        static u8 last_fmt = 0xFF;
        if (fmt != last_fmt) {
            for (int s = 0; s < VT_EVA_SLOTS; s++) vt_eva_key[s] = 0;
            last_fmt = fmt;
        }
    }

    int assembled = 0;                       // at most one new page per vblank
    for (int i = 0; i < 256; i += 4) {
        u8 y = oam[i];
        if (y >= 0xEF) continue;             // hidden
        u8 tile = oam[i + 1];
        u8 eva  = (oam[i + 2] >> 2) & 7;
        // SESSION 21b6: eva==0 sprites are redirected too.  s21b3 left them on
        // the stock bankbuffer path because guide 8b assumed that path already
        // fetched them correctly -- it does not.  Extension addressing applies
        // to EVERY sprite once SPEXTEN is set, so even EVA=0 resolves to
        // VBANK<<13, while the stock path uses the normal-mode VBANK<<10: a
        // different ROM offset entirely, and in Star Ally's gameplay stages it
        // lands on blank data.  Measured at f700: 10 of 24 visible sprites had
        // EVA=0 and ALL TEN drew blank tiles -- the "invisible enemies you
        // crash into".  Redirecting them costs nothing (same slot cache).
        // 8x16 sprites: pattern table from tile bit 0, page from tile bits 6-7.
        // This mirrors update_sprites' own index arithmetic exactly.
        u32 page = (u32)((tile & 1) << 2) | ((tile >> 6) & 3);
        u16 key  = (u16)((page << 3) | eva) + 1u;

        int slot = -1, victim = 0;
        for (int s = 0; s < VT_EVA_SLOTS; s++) {
            if (vt_eva_key[s] == key) { slot = s; break; }
            if (vt_eva_age[s] < vt_eva_age[victim]) victim = s;
        }
        if (slot < 0) {
            if (assembled) continue;         // spread the work across frames
            slot = victim;
            u32 bank = (vt_chr4_page_bank[page] << 3) | eva;
            if (pix16)
                vt_assemble_page_pix16_to(0x06010000u + (u32)slot * 4096u, bank);
            else if (sp16)
                vt_eva_assemble(slot, bank);
            else
                vt_assemble_page_2bpp_to(0x06010000u + (u32)slot * 2048u, bank);
            vt_eva_key[slot] = key;
            assembled = 1;
        }
        vt_eva_age[slot] = ++vt_eva_clock;
        // Publish the slot where ppu.s will look for it.  Must stay
        // non-negative or need_to_fetch_sprite_data would try to recache it.
        // In PIX16 mode publish slot*2: update_sprites' existing slot<<6
        // tile math then lands on tiles slot*128 = the 4KB pair slots, with
        // no change to the asm base arithmetic.
        spr_cache_map[64u + (page << 3) + eva] = (u8)(pix16 ? slot * 2 : slot);
    }
    vt_spr16_active = 1;                    // redirect every sprite (see above)
    vt_pix16_active = (u8)pix16;
}

__attribute__((target("arm")))
void vt_16c_palette_fixup(void)
{
    if (!vt_active) return;
#ifdef VT_AUTOPLAY
    // Debug-only self-playing script.  Drives the user's reported route into
    // the side-scrolling stage: hold Up ~3.5s, Right ~1s, Up ~1s.  Never
    // compiled into deliverable builds.
    {
        static u32 apf = 0;
        static const struct { u16 at; u8 pad; } script[] = {
            {400,0x10},{620,0x00},
            {630,0x80},{690,0x00},
            {700,0x10},{760,0x00},
            {0,0}
        };
        apf++;
        for (int i = 0; script[i].at; i++)
            if (apf == script[i].at) vt_dbg_pad_or = script[i].pad;
    }
#endif
    vt_build_16color_palette();
    vt_spr_eva_update();
    // With SPEVA active every sprite is fetched from an extended cache entry
    // (slots 0..3), so the old whole-page overlay of slots 8..15 has nothing
    // left to correct -- and its 2KB copies are pure cost.  Keep it for VT
    // titles that use 4bpp sprites WITHOUT address extension.
    if (!vt_spr16_active) vt_obj4_overlay();
}

// ---------------------------------------------------------------------------
// 4bpp sprite (OBJ) overlay -- the "invisible player character" fix
// ---------------------------------------------------------------------------
// PocketNES's sprite pipeline (update_sprites / need_to_fetch_sprite_data /
// render_a_kilobyte) caches converted CHR banks as GBA OBJ tiles:
//   spr_cache_map[bank] = signed cache-slot number (>= SPR_CACHE_START when
//   resident, negative when not); slot s occupies OBJ VRAM at
//   0x06010000 + s*2048 (64 tiles * 32 bytes), and update_sprites emits
//   attr2 tile = slot*64 + (NES tile & 0x3F).
// For VT carts the CHR model is the NES_VRAM 2bpp shadow (no PocketNES-side
// banking), so spr_cache_map is indexed by the PPU 1K slot number 0..7 --
// exactly the page index of vt_chr4_buf, which already holds ALL 512 tiles
// of the $0000-$1FFF pattern space as GBA-ready 4bpp (sprites and BG share
// those banked slots on OneBus).  The cache conversions only know the 2bpp
// shadow, so sprite pixels come out garbled in 16-colour games; this
// function re-copies the resident slots from the 4bpp source AFTER all the
// frame's 2bpp conversions (update_sprites recache + consume_recent_tiles
// both re-stomp slots whenever CHR banks change, e.g. on every sprite
// animation bank flip -- hence per-frame, not change-driven).
// Palette + attributes need no changes: update_sprites already routes the
// NES OAM attribute bits into GBA attr2 palette 0..3, attr0 stays 16-colour
// mode, and vt_build_16color_palette already fills the OBJ sub-palettes at
// 0x05000200 from the scattered index with the bg/spr bit set.
// spr_cache_map is declared u8[256] in asmcalls.h (included above); the asm
// side treats entries as SIGNED bytes (ldrsb: negative = bank not resident),
// so reinterpret per-entry here.

__attribute__((target("arm"), noinline))
static void vt_obj4_overlay(void)
{
    if (!vt_active) return;
    if (!(vt_reg_2010 & 0x04)) return;   // SP16EN off: 2bpp sprites correct
    // s21b59: no COLCOMP bail -- COLCOMP is a COLOUR mode, not a tile format;
    // the 4bpp sprite tiles are identical and their colours now come from the
    // COLCOMP-aware vt_build_16color_palette.

    // IRQ-BUDGET NOTE: this runs inside the vblank IRQ alongside the 16KB
    // BG copy.  A naive full 16KB per-frame CPU word-loop here blew the IRQ
    // time budget during cart load (all 8 banks resident) and tripped the
    // stack canary -- the same overrun class as design-doc crash #2.  So:
    // (1) exact change detection via vt_chr4_sigoff (see its comment): one
    //     word compare per resident slot in steady state, guaranteed to
    //     catch any 2bpp stomp, and exact skipping of pages whose 2bpp and
    //     4bpp conversions are identical;
    // (2) memcpy32 (ldm/stm 8-word bursts from memcopy.s) for the actual
    //     2KB copy, ~4x faster than a volatile word loop.
    // (3) per-frame copy cap: at most 2 slot copies per vblank, so the
    //     first-time population (all 8 slots) spreads across 4 frames
    //     instead of stacking ~16KB of copies onto one load-time IRQ.
    int budget = 2;
    for (int p = 0; p < 8; p++) {
        int slot = (signed char)spr_cache_map[p];
        if (slot < 0) continue;          // bank not resident in sprite cache
        u16 sig = vt_chr4_sigoff[p];
        if (sig == 0xFFFF) continue;     // page identical under both paths
        const u32 *s = (const u32*)(vt_chr4_buf + (u32)p * 2048u);
        u32 *d = (u32*)(0x06010000u + (u32)slot * 2048u);
        if (d[sig] == s[sig]) continue;  // still our 4bpp content -> skip
        // Inlined 4-word copy loop: memcpy32's stmfd of 8 registers on the
        // canary-guarded vblank IRQ stack was the final straw of the
        // overflow; this stays within the current frame.
        for (int i = 0; i < 512; i += 4) {
            u32 a = s[i], b = s[i+1], c = s[i+2], e = s[i+3];
            d[i] = a; d[i+1] = b; d[i+2] = c; d[i+3] = e;
        }
        if (--budget == 0) return;       // rest next frame
    }
}

#if 0  /* old inlined body, replaced by the split helpers above */
__attribute__((target("arm")))
void vt_chr4_rebuild_if_dirty_OLD(void)
{
    if (!vt_chr4_dirty) return;
    vt_chr4_dirty = 0;

    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    for (int p = 0; p < 8; p++) {
        u32 phys_1k  = vt_chr_bank_byte_offset_n(vt_chr4_page_bank[p], 1) >> 10;
        u32 src_base = phys_1k * 2048u;              // 1KB page = 2KB src
        u8 *outp = vt_chr4_buf + (p * 64 * 32);      // 64 tiles * 32 bytes
        for (int t = 0; t < 64; t++) {
            u32 lo_base = src_base + (u32)t * 32u;
            u32 hi_base = lo_base + 16u;
            u8 *g = outp + t * 32;
            for (int r = 0; r < 8; r++) {
                // Mask EVERY byte fetch (not just the base): a block near the
                // top of ROM must not read past it -- this was crash #1.
                u8 l0 = cbase[(lo_base + r)     & mask];
                u8 l1 = cbase[(lo_base + r + 8) & mask];
                u8 h0 = cbase[(hi_base + r)     & mask];
                u8 h1 = cbase[(hi_base + r + 8) & mask];
                u32 row = 0;
                for (int c = 0; c < 8; c++) {
                    u32 px = ((l0 >> (7 - c)) & 1)
                           | (((l1 >> (7 - c)) & 1) << 1)
                           | (((h0 >> (7 - c)) & 1) << 2)
                           | (((h1 >> (7 - c)) & 1) << 3);
                    row |= px << (c * 4);
                }
                g[r * 4 + 0] = (u8)(row);
                g[r * 4 + 1] = (u8)(row >> 8);
                g[r * 4 + 2] = (u8)(row >> 16);
                g[r * 4 + 3] = (u8)(row >> 24);
            }
        }
    }

    {
        const u8 *src = vt_chr4_buf;            // 512 tiles * 32 bytes, tile order
        for (int tile = 0; tile < 512; tile++) {
            u32 addr = 0x06000000u + (u32)tile * 32u;
            if (tile & 0x100) addr += 0x2000u;  // matches ppu.s tst #0x100
            volatile u32 *d = (volatile u32*)addr;
            const u32 *s = (const u32*)(src + tile * 32);
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
            d[4]=s[4]; d[5]=s[5]; d[6]=s[6]; d[7]=s[7];
        }
    }

    vt_build_16color_palette();
}
#endif
#endif // VT_MODE

// ---------------------------------------------------------------------------
// vt_ppu_reg_write -- handler for VT extended PPU registers
// ---------------------------------------------------------------------------
// Called from ppu.s vt_ppu_extended_W for writes to $2008-$21FF.
//
// Known VT registers (from ROM analysis of Lonely_Island, Star_Ally):
//   $2012-$2017 -- OneBus CHR bank selection (MMC3-like 2KB+2KB+1KB*4)
//                  Currently shadow-only -- see note below.
//   $2018  -- PPU extended control (palette bank select, etc.)
//   $201A  -- PPU extended control 2 (CHR addressing extension)
//   $2107  -- VT PPU mode register (enhanced colour / hi-res sprites)
//
// page=0 for $20xx, page=1 for $21xx
//
// Note on CHR banks: the 0.3.x cut of this code called PocketNES's
// chr0_-chr7_ routines here, but those routines assume CHR-ROM-backed
// carts and update PocketNES's vram_map/instant_chr_banks tables in
// ways that aren't right for VT OneBus carts (which have NO CHR ROM --
// the CHR data is in PRG, fetched via these bank registers, which
// PocketNES doesn't natively support).  Calling chr*_ on a CHR-RAM
// cart with VT bank-numbers as input was found to wedge the PPU BG
// fetch in a way that hung the CPU within the first frame, tripping
// the 3-second watchdog and producing the "white flicker then reset"
// symptom the user reported.  Until ppu.s grows real CHR-from-PRG
// support, $2012-$2017 writes are just stored.

void vt_ppu_reg_write(u8 page, u8 offset, u8 val)
{
    if (page == 0) {
        // $2012-$2017: OneBus CHR bank registers.  Shadow into vt_chr_reg[]
        // so the values are observable for debug and ready for the future
        // CHR-from-PRG path in ppu.s.  Still NOT calling chr0_-chr7_ here
        // for the reasons described in the file header (PocketNES's chr*_
        // assume CHR-ROM carts; passing VT CHR-RAM bank-numbers wedged the
        // PPU fetch in the 0.3.x bug).
        if (offset >= 0x12 && offset <= 0x17) {
            /* s21b59: mapper 256 submappers 1/3/4/5 route $2012-$2017 to
             * permuted registers (NintendulatorNRS mapper256.cpp write2,
             * ppuMangle[][]).  Identity for every other submapper. */
            static const u8 ppu_mangle[16][6] = {
                {0,1,2,3,4,5},{1,0,5,4,3,2},{0,1,2,3,4,5},{5,4,3,2,0,1},
                {2,5,0,4,3,1},{1,0,5,4,3,2},{0,1,2,3,4,5},{0,1,2,3,4,5},
                {0,1,2,3,4,5},{0,1,2,3,4,5},{0,1,2,3,4,5},{0,1,2,3,4,5},
                {0,1,2,3,4,5},{0,1,2,3,4,5},{0,1,2,3,4,5},{0,1,2,3,4,5}};
            const int ri = ppu_mangle[vt.submapper & 0x0F][offset - 0x12];
            if (vt_chr_reg[ri] != val) {
                vt_chr_reg[ri] = val;
                // CHR bank changed -- re-copy the 8KB CHR window from PRG
                // into NES_VRAM so the GBA tile cache can re-render it.
                vt_chr_sync_from_prg();
            }
        }

        // $20xx extended control bytes
        switch (offset) {
            case 0x10:
                // $2010: Extended Graphics Control 1
                //   bit 7 = COLCOMP : 1 = composited 12-bit colour mode
                //   bit 1 = BK16EN  : 16-colour backgrounds
                //   bit 2 = SP16EN  : 16-colour / 16-pixel sprites
                //   See VT02+ Registers wiki.
                // Setting COLCOMP is the canonical "this is a VT03 game"
                // signal -- some games initialise the palette before they
                // flip COLCOMP, others flip it first.  Either way, once
                // COLCOMP=1 we should be running the LUT path and pushing
                // composited colours to the GBA palette every VBlank.
                {
                    u8 old_2010 = vt_reg_2010;
                    if ((old_2010 ^ val) & 0x80) {
                        // COLCOMP transitioned -- force a palette rebuild
                        // and (if going to COLCOMP=1) take over the GBA
                        // palette.
                        vt_palette_dirty = true;
                        if (val & 0x80) vt_palette_active = true;
                    }
                    vt_reg_2010 = val;

                    // BKEXTEN transition (session 18): live when bit4 set
                    // together with 4bpp backgrounds (BK16EN).
                    {
                        u8 want = ((val & 0x12) == 0x12);
                        if (want != vt_bkexten_live) {
                            vt_bkexten_live = want;
                            vt_bk_dbg[0]++;
                            vt_bk_invalidate();
                            if (want) {
                                // Rebuild the whole tilemap in the packed
                                // slot encoding.  DEFERRED to the vblank
                                // handler: the slot allocator is not
                                // reentrant, and running it here (CPU write
                                // context) races the vblank-side consumer --
                                // observed as three slots all claiming the
                                // same (page,attr) key.
                                vt_bk_pending_whole = 1;
                            } else {
                                // Back to the legacy encoding: force the
                                // asm whole-map redraw.
                                _bg_cache_full = 1;
                            }
                        }
                    }

                    // Re-sync CHR if the 4bpp-mode selection changed.  The
                    // CHR copy path differs between 2bpp (raw memcpy) and
                    // 4bpp (deinterleaved 2KB -> 1KB), so any of COLCOMP,
                    // BK16EN, SP16EN flipping requires a re-copy of the
                    // current CHR window from PRG into NES_VRAM.
                    const u8 fbpp_mask = 0x86;  // COLCOMP | SP16EN | BK16EN
                    if ((old_2010 ^ val) & fbpp_mask) {
                        vt_chr_sync_from_prg();
                    }
                }
                break;

            case 0x11:
                // $2011: Extended Graphics Control 2.  Bit 0 (EVA12S)
                // selects the source of the background EVA bit 2 under
                // BKEXTEN (session 18); the rest are LCD-side controls.
                if ((vt_reg_2011 ^ val) & 0x01) {
                    vt_reg_2011 = val;
                    vt_bk_banks_recheck();
                } else {
                    vt_reg_2011 = val;
                }
                break;

            case 0x18:
                // $2018: Video Bank 1 Register, BKPAGE, V/R/W bank.
                //   bits 2:0  Video bank when accessing video data
                //   bit  3    BKPAGE address is EVA12 when EVA12S=0
                //   bits 6:4  Video Bank 1 Register (intermediate CHR)
                if (vt_chr_reg_2018 != val) {
                    u8 pchanged = (vt_chr_reg_2018 ^ val) & 0x08;
                    vt_chr_reg_2018 = val;
                    vt_chr_sync_from_prg();
                    if (pchanged) vt_bk_banks_recheck();
                }
                // Keep the low nibble of vt_ppumode in sync for backward
                // compatibility with code that watched vt_ppumode.
                vt_ppumode = (vt_ppumode & 0xF0) | (val & 0x0F);
                break;

            case 0x1A:
                // $201A: Video Bank 0 Register 6 + V Bank 0 Selector.
                //   bits 2:0  V Bank 0 selector
                //   bits 7:3  V Bank 0 Register 6 (intermediate CHR bits)
                if (vt_chr_reg_201A != val) {
                    vt_chr_reg_201A = val;
                    vt_chr_sync_from_prg();
                }
                vt_ppumode = (vt_ppumode & 0x0F) | (val & 0xF0);
                break;

            default:
                // Other $20xx registers: ignore for now
                break;
        }
    } else {
        // $21xx extended registers
        switch (offset) {
            case 0x07:
                // $2107: VT mode register (enhanced palette / sprite mode)
                vt_ppumode_write(val);
                break;

#if VT_ENHANCED_PALETTE
            default:
                if (offset >= 0x40 && offset < 0x80) {
                    // $2140-$217F: extended palette entries (alt mapping)
                    vt_palette_write(offset - 0x40, val);
                }
                break;
#endif
        }
    }
}

#endif // VT_MODE


/* ==========================================================================
 * s21b62: RASTER-SPLIT CHR BANKING FOR 4bpp BACKGROUNDS  (guide section 73)
 * --------------------------------------------------------------------------
 * Games like Aero Gyrodine, Hex City X and Add 'em Up change $2016/$2017
 * partway down the screen (from a timer IRQ) so each horizontal band uses
 * different graphics.  The 4bpp background used to be built once per frame
 * from the final banks, so every band but the last showed the wrong tiles.
 *
 * PocketNES's background tile cache is four 8 KB slots -- the first half of
 * each 16 KB character block -- and BG0CNT's character base is written per
 * scanline from bg0cntbuff by HBlank DMA.  On VT carts PocketNES itself only
 * ever occupies slot 0 (its CHR group never changes; VT banks CHR through its
 * own registers), and the VT pipeline builds the $1000 half into slot 1, so
 * slots 2 and 3 are free.  A 4bpp page (64 tiles x 32 bytes) is exactly 2 KB:
 * four pages fill one slot.
 *
 *   vt_band_mark(line)    called from ppu.s after every $2012-$2017 write and
 *                         from mapVT.s after MMC3 bank writes, with the NES
 *                         scanline from get_scanline_2.  Records a band when
 *                         the registers change mid-frame.
 *   vt_bands_frame_end()  called from newframe_nes_vblank (NES line 242,
 *                         after the catch-ups have filled bg0cntbuff, before
 *                         the buffer swap).  The LAST band is primary: the
 *                         existing pipeline already builds it into slot 0.
 *                         Every other band's bank set gets slot 2 or 3 from a
 *                         two-entry LRU cache (assembled only on a miss), and
 *                         its lines in bg0cntbuff get that character base.
 * ========================================================================== */
#define VT_MAXB 6
typedef struct { u8 line; u8 reg[6]; u8 pad; } VtBand;
EWRAM_BSS VtBand vt_band[VT_MAXB];
EWRAM_BSS u8  vt_nband;
EWRAM_BSS u8  vt_band_fresh;          /* frame_end ran; next-frame band 0 open */
EWRAM_BSS u8  vt_split_frame;         /* last completed frame had >1 band */
EWRAM_BSS u8  vt_frame_reg[6];        /* its primary (last-band) registers */
EWRAM_BSS u32 vt_split_key[2][4];     /* 1K banks held by slots 2 and 3 */
EWRAM_BSS u32 vt_split_mode[2];       /* bus width / mode they were built in */
EWRAM_BSS u16 vt_split_sig[2][4];     /* per-page stomp-check word offsets */
#if VT_SPLIT_SLOTS
EWRAM_BSS volatile u8 vt_split_valid[2];   /* volatile: read by the vblank IRQ (vt_split_repair) mid-build */
#else
EWRAM_BSS u8  vt_split_valid[2];
#endif
EWRAM_BSS u8  vt_split_lru;
EWRAM_BSS u32 vt_split_assemblies;    /* diagnostic: cache misses */

static int vt_regs_same(const u8 *a, const u8 *b)
{ for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0; return 1; }
static void vt_regs_copy(u8 *d, const u8 *s) { for (int i = 0; i < 6; i++) d[i] = s[i]; }

void vt_band_mark(u32 line)
{
    if (vt_nband == 0) {                       /* first use after reset */
        vt_band[0].line = 0; vt_regs_copy(vt_band[0].reg, vt_chr_reg);
        vt_nband = 1; vt_band_fresh = 1;
        return;
    }
    if (line >= 240) {                         /* vblank / after render end */
        if (vt_band_fresh)                     /* sets the next frame's band 0 */
            vt_regs_copy(vt_band[0].reg, vt_chr_reg);
        return;                                /* else frame_end captures it */
    }
    vt_band_fresh = 0;
    VtBand *b = &vt_band[vt_nband - 1];
    if (vt_regs_same(b->reg, vt_chr_reg)) return;
    if (line <= b->line) { vt_regs_copy(b->reg, vt_chr_reg); return; }
    if (vt_nband < VT_MAXB) {
        b = &vt_band[vt_nband++];
        b->line = (u8)line;
        vt_regs_copy(b->reg, vt_chr_reg);
    } else {
        vt_regs_copy(vt_band[VT_MAXB - 1].reg, vt_chr_reg);   /* overflow: fold */
    }
}

/* The four 1K banks the background reads for register set r. */
static void vt_band_key(const u8 *r, int half, u32 k[4])
{
    if (!half) { k[0] = r[4] & 0xFEu; k[1] = r[4] | 1u; k[2] = r[5] & 0xFEu; k[3] = r[5] | 1u; }
    else       { k[0] = r[0]; k[1] = r[1]; k[2] = r[2]; k[3] = r[3]; }
}

/* One 4bpp page (64 tiles) for 1K bank `bank`, decoded exactly like
 * vt_chr4_assemble, written straight into VRAM (word stores). */
__attribute__((target("arm"), noinline))
static u16 vt_chr4_assemble_page_vram(volatile u32 *dst, u32 bank)
{
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = vt_chr_mask_get(); const u8 *cbase = vt_chr_base();
    const int wide16 = (vt_reg_2010 & 0x40) != 0;
    const u32 o1 = wide16 ? 16u : 8u, o2 = wide16 ? 1u : 16u;
    const u32 o3 = wide16 ? 17u : 24u, rs = wide16 ? 2u : 1u;
    u32 src_base = (vt_chr_bank_byte_offset_n(bank, 1) >> 10) * 2048u;
    u16 sig = 0xFFFF, widx = 0;
    for (int t = 0; t < 64; t++, src_base += 32) {
        for (int r = 0; r < 8; r++, widx++) {
            const u32 lo2 = src_base + r * rs;
            u32 row = vt_spread[cbase[lo2 & mask]]
                    | (vt_spread[cbase[(lo2 + o1) & mask]] << 1)
                    | (vt_spread[cbase[(lo2 + o2) & mask]] << 2)
                    | (vt_spread[cbase[(lo2 + o3) & mask]] << 3);
            if (sig == 0xFFFF && (row & 0xCCCCCCCCu)) sig = widx;
            *dst++ = row;
        }
    }
    return sig;
}

static u32 vt_split_mode_word(void)
{   /* anything besides the bank numbers that changes the decoded tiles */
    return (u32)(vt_reg_2010 & 0x40) | ((u32)vt_chr_reg_2018 << 8)
         | ((u32)vt_chr_reg_201A << 16) | ((u32)vt_chr_outer_4100 << 24);
}

static void vt_split_build(int s, const u32 k[4])
{
    volatile u32 *base = (volatile u32 *)(0x06000000u + (u32)(2 + s) * 0x4000u);
    /* Invalid while half-written: the vblank IRQ's vt_split_repair can land
     * mid-build, take the unfinished page for a stomp and rebuild it with the
     * OLD key; this build then resumes and leaves a mixed page (Aero title,
     * guide s.77). */
#if VT_SPLIT_SLOTS
    vt_split_valid[s] = 0;
#endif
    for (int p = 0; p < 4; p++) {
        vt_split_sig[s][p] = vt_chr4_assemble_page_vram(base + p * 512, k[p]);
        vt_split_key[s][p] = k[p];
    }
    vt_split_mode[s] = vt_split_mode_word();
    vt_split_valid[s] = 1;
    vt_split_assemblies++;
}

/* 3K EWRAM stack for the heavy VT C work: the vblank IRQ's CHR rebuild and
 * palette fixup (mapVT.s trampolines) and the frame-end chain (timeout.s).
 * IWRAM leaves ~410-470 bytes of user stack, and the IRQ nests on whatever it
 * interrupted (guide s.77b, s.78e). */
EWRAM_BSS u32 vt_ewram_stack[768] __attribute__((aligned(8)));
asm(".global vt_ewram_stack_top\n.set vt_ewram_stack_top, vt_ewram_stack + 3072");

#if VT_SPLIT_SLOTS
/* Guide s.77.  Char blocks 2/3 (0x06008000-0x0600FFFF) are not free on VT
 * carts: loadcart.c's USE_ACCELERATION puts the last 32K of PRG there and
 * the 6502 executes from it, so writing slot tiles over it crashed Add 'em
 * Up.  Keeping PRG elsewhere for good costs ~10% speed on every VT cart,
 * so the move happens only when a cart first needs a slot: repoint the
 * banks to their identical EWRAM twin (vt_prg_shadow, set by loadcart.c)
 * and flag timeout.s, which after newframe_nes_vblank returns re-runs
 * vt_apply_prg_banks -- its map*_ -> flush re-encodes the 6502 PC through
 * the new memmap before another instruction executes.  The CPU is paused
 * while this runs, so the slot tiles can be written straight away. */
EWRAM_BSS u8 *vt_prg_shadow;
EWRAM_BSS u8 vt_prg_evict_pending;
EWRAM_BSS u8 vt_prg_evicted;
EWRAM_BSS u16 *vt_tag_buf[2];               /* the bg0cntbuff pair ... */
EWRAM_BSS u8 vt_tag_lo[2], vt_tag_hi[2];    /* ... and the lines tagged in each */
extern const u8 *_speedhack_pc, *_speedhack_pc2;

static const u8 *vt_prg_reloc(const u8 *p)
{
    const u32 a = (u32)p;
    if (a >= 0x06008000u && a < 0x06010000u) return vt_prg_shadow + (a - 0x06008000u);
    return p;
}

static int vt_prg_evict(void)
{
    if (vt_prg_evicted) return 1;
    if (!vt_prg_shadow) return 0;              /* no twin: slots stay off */
    const int n = rompages * PRG_16;
    for (int b = 0; b < n; b++)
        instant_prg_banks[b] = (u8 *)vt_prg_reloc(instant_prg_banks[b]);
    for (int i = 0; i < 4; i++)
        speedhacks[i].hack_pc = vt_prg_reloc(speedhacks[i].hack_pc);
    _speedhack_pc  = vt_prg_reloc(_speedhack_pc);
    _speedhack_pc2 = vt_prg_reloc(_speedhack_pc2);
    vt_prg_evicted = 1;
    vt_prg_evict_pending = 1;
    return 1;
}

void vt_split_reset(void)
{
    vt_split_valid[0] = vt_split_valid[1] = 0;
    vt_split_lru = 0;
    vt_prg_evicted = 0;
    vt_prg_evict_pending = 0;
    vt_tag_buf[0] = vt_tag_buf[1] = 0;      /* first use scans all 240 lines */
}
#endif

static int vt_split_slot_get(const u32 k[4])
{
    const u32 mode = vt_split_mode_word();
    for (int s = 0; s < 2; s++) {
        if (!vt_split_valid[s] || vt_split_mode[s] != mode) continue;
        if (vt_split_key[s][0] == k[0] && vt_split_key[s][1] == k[1] &&
            vt_split_key[s][2] == k[2] && vt_split_key[s][3] == k[3]) {
            vt_split_lru = (u8)(s ^ 1);        /* the other one is older */
            return 2 + s;
        }
    }
    int s = vt_split_lru;
    vt_split_lru = (u8)(s ^ 1);
    vt_split_build(s, k);
    return 2 + s;
}

/* Called each GBA frame from vt_chr4_rebuild_if_dirty: re-decode a split slot
 * only if something overwrote it (one word compare per page). */
static void vt_split_repair(void)
{
    for (int s = 0; s < 2; s++) {
        if (!vt_split_valid[s]) continue;
        const volatile u32 *base = (const volatile u32 *)(0x06000000u + (u32)(2 + s) * 0x4000u);
        for (int p = 0; p < 4; p++) {
            u16 off = vt_split_sig[s][p];
            if (off == 0xFFFF) continue;
            /* recompute the expected word cheaply by re-decoding that page only if the
             * word no longer carries high-plane bits (a 2bpp stomp cannot produce them) */
            if (!(base[p * 512 + off] & 0xCCCCCCCCu)) { vt_split_build(s, vt_split_key[s]); break; }
        }
    }
}

#if VT_SPLIT_SLOTS
/* bg0cntbuff line tagging, two lines per 32-bit word (the per-line loops cost
 * ~40k cycles per NES frame on Aero's title).  A band line holds the slot's
 * char base (2/3: bit 3 set) in bits 2-3 and its original char base (0/1) in
 * bits 4-5, which BGCNT ignores.  Both helpers are branchless per line. */
static inline __attribute__((always_inline)) u32 vt_line_orig2(u32 v)   /* original char base in bits 2-3 */
{
    const u32 m = ((v >> 3) & 0x00010001u) * 0x000Cu;         /* tagged halves */
    return (v & 0x000C000Cu & ~m) | ((v >> 2) & m);
}
static void vt_lines_restore(u16 *b, int lo, int hi)
{
    if (lo >= hi) return;
    if (((u32)(b + lo)) & 2) { u32 v = b[lo]; b[lo] = (u16)((v & ~0x003Cu) | vt_line_orig2(v)); lo++; }
    u32 *w = (u32 *)(b + lo);
    for (int n = (hi - lo) >> 1; n > 0; n--, w++) { u32 v = *w; *w = (v & ~0x003C003Cu) | vt_line_orig2(v); }
    if ((hi - lo) & 1) { u32 v = b[hi - 1]; b[hi - 1] = (u16)((v & ~0x003Cu) | vt_line_orig2(v)); }
}
static void vt_lines_tag(u16 *b, int lo, int hi, u32 slot)
{
    if (lo >= hi) return;
    const u32 s1 = slot << 2, s2 = s1 * 0x00010001u;
    if (((u32)(b + lo)) & 2) { u32 v = b[lo]; b[lo] = (u16)((v & ~0x003Cu) | (vt_line_orig2(v) << 2) | s1); lo++; }
    u32 *w = (u32 *)(b + lo);
    for (int n = (hi - lo) >> 1; n > 0; n--, w++) { u32 v = *w; *w = (v & ~0x003C003Cu) | (vt_line_orig2(v) << 2) | s2; }
    if ((hi - lo) & 1) { u32 v = b[hi - 1]; b[hi - 1] = (u16)((v & ~0x003Cu) | (vt_line_orig2(v) << 2) | s1); }
}
#endif

/* _bg0cntbuff is declared in a shared header */
extern u8 _ppuctrl0;
void vt_bands_frame_end(void)
{
#if VT_SPLIT_SLOTS
    /* bg0cntbuff persists across frames (double-buffered), so a line that was
     * a split band's line some frames ago would keep pointing at a slot that
     * has since been rebuilt for another bank set (Aero's title top strip,
     * guide s.77).  Band lines carry their original char base in BGCNT bits
     * 4-5 (unused by the hardware); put every such line back first. */
    /* Only the range tagged the last time this buffer was the current one
     * (the pair alternates); a full 240-line pass cost ~6% on Aero's title. */
    u16 *const cur = (u16 *)_bg0cntbuff;
    int side = (cur == vt_tag_buf[0]) ? 0 : (cur == vt_tag_buf[1]) ? 1 : -1;
    if (side < 0) {                        /* not seen yet: claim a side, scan it all */
        side = vt_tag_buf[0] ? 1 : 0;
        vt_tag_buf[side] = cur; vt_tag_lo[side] = 0; vt_tag_hi[side] = 240;
    }
    vt_lines_restore(cur, vt_tag_lo[side], vt_tag_hi[side]);
    int tag_lo = 240, tag_hi = 0;
#endif
    const int n = vt_nband;
    const int ok = vt_active && (vt_reg_2010 & 0x02) && !vt_bkexten_live;   /* 4bpp BG, non-extension */
    if (ok && n > 1) {
        const u8 *preg = vt_band[n - 1].reg;
        /* vt_chr_sync_flush decodes from vt_frame_reg on split frames and from
         * the live registers otherwise, but only runs when a bank WRITE asked
         * for a sync.  When the source it would use changes with no write --
         * a split starts or ends, or the primary band's banks change -- ask for
         * one here, or the tiles stay decoded from the old source (Hex City X:
         * the menu kept the title's banks 12-15, guide s.78). */
        if (!vt_split_frame || !vt_regs_same(vt_frame_reg, preg)) vt_chr_sync_from_prg();
        vt_regs_copy(vt_frame_reg, preg);
        vt_split_frame = 1;
        const int half = (_ppuctrl0 & 0x10) ? 1 : 0;
        u32 pk[4]; vt_band_key(preg, half, pk);
        u16 *buf = (u16 *)_bg0cntbuff;
        for (int i = 0; i < n - 1; i++) {
            u32 k[4]; vt_band_key(vt_band[i].reg, half, k);
            if (k[0] == pk[0] && k[1] == pk[1] && k[2] == pk[2] && k[3] == pk[3]) continue;
#if !VT_SPLIT_SLOTS
            continue;
#else
            if (!vt_prg_evict()) continue;     /* blocks 2/3 still hold PRG (s.77) */
#endif
            const int slot = vt_split_slot_get(k);
            int l0 = vt_band[i].line, l1 = vt_band[i + 1].line;
            if (l1 > 240) l1 = 240;
#if VT_SPLIT_SLOTS
            vt_lines_tag(buf, l0, l1, (u32)slot);   /* slot char base; original kept in bits 4-5 */
#else
            for (int l = l0; l < l1; l++)
                buf[l] = (u16)((buf[l] & ~0x000Cu) | ((u32)slot << 2));
#endif
#if VT_SPLIT_SLOTS
            if (l0 < tag_lo) tag_lo = l0;
            if (l1 > tag_hi) tag_hi = l1;
#endif
        }
    } else {
        if (vt_split_frame) vt_chr_sync_from_prg();   /* back to the live registers */
        vt_split_frame = 0;
    }
#if VT_SPLIT_SLOTS
    vt_tag_lo[side] = (u8)tag_lo; vt_tag_hi[side] = (u8)tag_hi;
#endif
    vt_band[0].line = 0; vt_regs_copy(vt_band[0].reg, vt_chr_reg);
    vt_nband = 1; vt_band_fresh = 1;
}
