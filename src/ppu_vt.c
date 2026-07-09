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

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

#if VT_ENHANCED_PALETTE
// vt_palette_ram[] layout (matches Furbtendulator OneBus.cpp:540):
//   [0x00 .. 0x1F]  low  6-bit half written via PPU $3F00-$3F1F
//   [0x80 .. 0x9F]  high 6-bit half written via PPU $3F80-$3F9F  (VT03+)
// Other indices [0x20..0x7F] and [0xA0..0x1FF] are unused but the buffer
// stays sized for the spec'd 512-entry future extension (VT09 enhanced).
EWRAM_BSS u8  vt_palette_ram[VT_PALETTE_SIZE];
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
EWRAM_BSS u8 vt_chr4_buf[8 * 64 * 32];

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

void ppu_vt_reset(void)
{
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
    offset = (offset & 0x1F) | 0x80;
    val    &= 0x3F;
    vt_palette_ram[offset] = val;
    // Same bg-color mirroring within the hi bank
    if ((offset & 0x03) == 0) {
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
    // ========================================================================
    // CRITICAL CORRECTNESS GATES -- if either of these is wrong, every game
    // boots to a black screen.  History (see commit message and PPU_S_PATCH_
    // INSTRUCTIONS.md):  the 0.4 cut of this function unconditionally
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
    // ppu_vt_sprites.s) -- see IMPLEMENTATION_NOTES.md for the design.
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

static inline u32 vt_compute_chr_bank(u32 inner_bank)
{
    u32 inner_mask    = vt_inner_bank_mask();
    u32 middle        = (u32)vt_chr_reg_201A & 0xF8u;   // RV6 in-place (bits 3-7)
    u32 intermediate  = ((u32)vt_chr_reg_2018 >> 4) & 0x07u;  // VA18-VA20 (3 bits)
    u32 outer         = (u32)vt_chr_outer_4100 & 0x0Fu; // VA21-VA24 (4 bits)

    return ((inner_bank & inner_mask)
          | (middle    & ~inner_mask))
         | (intermediate << 8)
         | (outer       << 11);
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
    u32 page_bank[8];
    page_bank[0] = (vt_chr_reg[4] & 0xFEu);          // $2016 even
    page_bank[1] = (vt_chr_reg[4] | 0x01u);          // $2016 odd
    page_bank[2] = (vt_chr_reg[5] & 0xFEu);          // $2017 even
    page_bank[3] = (vt_chr_reg[5] | 0x01u);          // $2017 odd
    page_bank[4] =  vt_chr_reg[0];                   // $2012
    page_bank[5] =  vt_chr_reg[1];                   // $2013
    page_bank[6] =  vt_chr_reg[2];                   // $2014
    page_bank[7] =  vt_chr_reg[3];                   // $2015

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
    u32 mask = rommask ? rommask : 0xFFFFFFFFu;
    u8 *dst = (u8*)NES_VRAM;

    if (!four_bpp) {
        // Fast path: raw 1KB memcpy per CHR page.
        for (int p = 0; p < 8; p++) {
            u32 src_off = vt_chr_bank_byte_offset(page_bank[p]) & mask;
            u8 *src = rombase + src_off;
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
            u8 *src = rombase + src_off;
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

__attribute__((always_inline)) static inline u16 nes_index_to_bgr555(u8 idx)
{
    if (vt_active) return vt_compat_rgb555[idx & 0x3F];
    const unsigned char *p = &nes_rgb[(idx & 0x3F) * 3];
    u8 r = p[0], g = p[1], b = p[2];
    return (u16)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

__attribute__((target("arm")))
static void vt_build_16color_palette(void)
{
    // Only meaningful in 16-colour mode with COLCOMP=0.
    if (!(vt_reg_2010 & 0x06)) return;   // not 16-colour
    if (vt_reg_2010 & 0x80)    return;   // COLCOMP=1 handled elsewhere

    volatile u16 *gba_bg  = (volatile u16*)0x05000000; // BG palette
    volatile u16 *gba_obj = (volatile u16*)0x05000200; // OBJ palette

    // The CRITICAL part (verified vs furb_cli OneBus.cpp + VT03 digest p.19):
    // a 16-colour pixel's palette-RAM index is BIT-SCATTERED, not 0..15:
    //   bits 0,1 = pattern low planes      (digest "color-address bits 1,2")
    //   bits 2,3 = colour-set / attribute  (digest bits 3,4) = GBA sub-palette
    //   bit  4   = 0 background / 1 sprite  (digest bit 5)
    //   bits 5,6 = pattern high planes     (digest bits 6,7)
    // Furb: GetPalIndex COLCOMP=0 returns Palette[index]; transparent rule:
    //   if (index & 0x63) == 0 (all pattern bits 0) -> index 0 (backdrop).
    //
    // Our GBA 4bpp tile pixel is CONTIGUOUS v = p0|p1<<1|p2<<2|p3<<3, rendered
    // through GBA sub-palette entry v. So GBA sub-palette[group][v] must hold
    // the VT colour at the scattered index built from v's bits + group + bgspr.
    for (int group = 0; group < 4; group++) {
        for (int v = 0; v < 16; v++) {
            int p0 = (v >> 0) & 1, p1 = (v >> 1) & 1;
            int p2 = (v >> 2) & 1, p3 = (v >> 3) & 1;
            // ----- background -----
            int idx_bg = p0 | (p1 << 1) | (group << 2) | (0 << 4) | (p2 << 5) | (p3 << 6);
            if (!(idx_bg & 0x63)) idx_bg = 0;                  // transparent -> backdrop
            u8 ci_bg = vt_palette_ram[idx_bg & (VT_PALETTE_SIZE - 1)] & 0x3F;
            gba_bg[group * 16 + v] = nes_index_to_bgr555(ci_bg);
            // ----- sprite (bit 4 = 1) -----
            int idx_sp = p0 | (p1 << 1) | (group << 2) | (1 << 4) | (p2 << 5) | (p3 << 6);
            if (!(idx_sp & 0x63)) idx_sp = 0;
            u8 ci_sp = vt_palette_ram[idx_sp & (VT_PALETTE_SIZE - 1)] & 0x3F;
            gba_obj[group * 16 + v] = nes_index_to_bgr555(ci_sp);
        }
    }
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
// vt_spread history in FIX_4BPP_DESIGN.md).
EWRAM_BSS static u16 vt_chr4_sigoff[8];

// Tight, fast 4bpp tile assembler using the spread table.
__attribute__((target("arm"), noinline))
static void vt_chr4_assemble(void)
{
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = rommask ? rommask : 0xFFFFFFFFu;
    u8 *g = vt_chr4_buf;
    for (int p = 0; p < 8; p++) {
        u32 src_base = (vt_chr_bank_byte_offset(vt_chr4_page_bank[p]) >> 10) * 2048u;
        u16 sig = 0xFFFF;
        u16 widx = 0;
        for (int t = 0; t < 64; t++, src_base += 32) {
            for (int r = 0; r < 8; r++, widx++) {
                u32 lo = src_base + r, hi = lo + 16;
                u32 row = vt_spread[rombase[lo & mask]]
                        | (vt_spread[rombase[(lo + 8) & mask]] << 1)
                        | (vt_spread[rombase[hi & mask]] << 2)
                        | (vt_spread[rombase[(hi + 8) & mask]] << 3);
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
__attribute__((target("arm")))
void vt_chr4_rebuild_if_dirty(void)
{
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
    vt_build_16color_palette();
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

#define VT_EVA_SLOTS 4
EWRAM_BSS static u16 vt_eva_key[VT_EVA_SLOTS];   // (page<<3)|eva, +1;  0 = empty
EWRAM_BSS static u32 vt_eva_age[VT_EVA_SLOTS];
EWRAM_BSS static u32 vt_eva_clock;

// Assemble one 1KB CHR page (64 tiles) straight into an OBJ VRAM slot as GBA
// 4bpp tiles.  Same pixel math as vt_chr4_assemble, no intermediate buffer.
__attribute__((target("arm"), noinline))
static void vt_eva_assemble(int slot, u32 phys_bank)
{
    if (!vt_spread_ready) vt_spread_init();
    u32 mask = rommask ? rommask : 0xFFFFFFFFu;
    u32 src_base = (vt_chr_bank_byte_offset(phys_bank) >> 10) * 2048u;
    u32 *d = (u32*)(0x06010000u + (u32)slot * 2048u);
    for (int t = 0; t < 64; t++, src_base += 32) {
        for (int r = 0; r < 8; r++) {
            u32 lo = src_base + r, hi = lo + 16;
            *d++ = vt_spread[rombase[lo & mask]]
                 | (vt_spread[rombase[(lo + 8) & mask]] << 1)
                 | (vt_spread[rombase[hi & mask]] << 2)
                 | (vt_spread[rombase[(hi + 8) & mask]] << 3);
        }
    }
}

__attribute__((target("arm"), noinline))
static void vt_spr_eva_update(void)
{
    // Need 4bpp sprites (SP16EN, bit 2) AND address extension (SPEXTEN, bit 3),
    // with the compatibility palette (COLCOMP=0).  Otherwise leave the stock
    // sprite path completely alone.
    if (!vt_active || (vt_reg_2010 & 0x0C) != 0x0C || (vt_reg_2010 & 0x80)) {
        vt_spr16_active = 0;
        return;
    }

    const u8 *oam = (const u8 *)_dmanesoambuff;
    if (!oam) { vt_spr16_active = 0; return; }

    int assembled = 0;                       // at most one new page per vblank
    for (int i = 0; i < 256; i += 4) {
        u8 y = oam[i];
        if (y >= 0xEF) continue;             // hidden
        u8 tile = oam[i + 1];
        u8 eva  = (oam[i + 2] >> 2) & 7;
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
            vt_eva_assemble(slot, (vt_chr4_page_bank[page] << 3) | eva);
            vt_eva_key[slot] = key;
            assembled = 1;
        }
        vt_eva_age[slot] = ++vt_eva_clock;
        // Publish the slot where ppu.s will look for it.  Must stay
        // non-negative or need_to_fetch_sprite_data would try to recache it.
        spr_cache_map[64u + (page << 3) + eva] = (u8)slot;
    }
    vt_spr16_active = 1;
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
    if (vt_reg_2010 & 0x80)    return;   // COLCOMP=1 sprite path not wired

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

    u32 mask = rommask ? rommask : 0xFFFFFFFFu;
    for (int p = 0; p < 8; p++) {
        u32 phys_1k  = vt_chr_bank_byte_offset(vt_chr4_page_bank[p]) >> 10;
        u32 src_base = phys_1k * 2048u;              // 1KB page = 2KB src
        u8 *outp = vt_chr4_buf + (p * 64 * 32);      // 64 tiles * 32 bytes
        for (int t = 0; t < 64; t++) {
            u32 lo_base = src_base + (u32)t * 32u;
            u32 hi_base = lo_base + 16u;
            u8 *g = outp + t * 32;
            for (int r = 0; r < 8; r++) {
                // Mask EVERY byte fetch (not just the base): a block near the
                // top of ROM must not read past it -- this was crash #1.
                u8 l0 = rombase[(lo_base + r)     & mask];
                u8 l1 = rombase[(lo_base + r + 8) & mask];
                u8 h0 = rombase[(hi_base + r)     & mask];
                u8 h1 = rombase[(hi_base + r + 8) & mask];
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
            if (vt_chr_reg[offset - 0x12] != val) {
                vt_chr_reg[offset - 0x12] = val;
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
                // $2011: Extended Graphics Control 2 -- mostly LCD-side
                // controls.  Shadow only for now; the emulator doesn't
                // expose them to anything observable.
                break;

            case 0x18:
                // $2018: Video Bank 1 Register, BKPAGE, V/R/W bank.
                //   bits 2:0  Video bank when accessing video data
                //   bit  3    BKPAGE address is EVA12 when EVA12S=0
                //   bits 6:4  Video Bank 1 Register (intermediate CHR)
                if (vt_chr_reg_2018 != val) {
                    vt_chr_reg_2018 = val;
                    vt_chr_sync_from_prg();
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
