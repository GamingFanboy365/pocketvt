/*
 * ppu_vt.h -- VT03/VT09 enhanced PPU for PocketVT
 *
 * The VT PPU is a superset of the RP2C02:
 *
 *   Standard mode: identical to NES PPU (64-colour palette, 8x8 / 8x16 sprites).
 *
 *   Enhanced colour mode (reg $2107 bit 1 set):
 *     - Palette RAM expands to 512 entries (9-bit index: 3 bits hue, 6 bits saturation/value)
 *     - Background tiles use 4-bit indices into a 16-colour sub-palette
 *     - Extended attribute table bits provide the extra palette bits
 *
 *   Hi-res sprite mode (reg $2107 bit 2 set, VT03 only):
 *     - Sprites expand to 16x8 pixels using the OAM extension at $4180
 *     - Two tile indices per sprite (left half / right half)
 *
 * GBA rendering approach
 * ----------------------
 * PocketNES uses GBA Mode 0 (tiled BG) with the GBA's built-in palette for
 * the NES palette mapping.  For VT enhanced colour mode the NES palette table
 * has more entries than the GBA's 256-colour palette can hold directly.
 *
 * Short-term workaround (implemented here):
 *   Build a 512-entry shadow palette in EWRAM.  At each line, detect which
 *   16 colours are actually used by the visible tiles and sub-select them
 *   into the GBA's 16 available colours for BG0.  This matches the approach
 *   used by handheld FC emulators with similar constraints.
 *
 * Long-term: switch the BG to Mode 4 (8-bit bitmap) for VT enhanced builds,
 * which gives full 256-colour flexibility at the cost of no hardware scroll.
 */

#ifndef __PPU_VT_H__
#define __PPU_VT_H__

#include "includes.h"
#include "config.h"

#if VT_ENHANCED_PALETTE

// ============================================================================
// VT palette RAM
// ============================================================================

// 512 entries of 6-bit colour (0rrggbb packed into u8 low 6 bits)
// Stored in EWRAM because it is 512 bytes and accessed once per frame.
#define VT_PALETTE_SIZE     512
extern u8 vt_palette_ram[VT_PALETTE_SIZE];

// Convert a 6-bit VT colour index to a GBA 15-bit BGR555 value.
// The conversion table is pre-built in vt_palette_to_gba[] by ppu_vt_init().
extern u16 vt_palette_to_gba[VT_PALETTE_SIZE];

// Write to the VT extended palette ($4140+ register window)
void vt_palette_write(u8 offset, u8 val);

// PPU-bus palette bridges called from VRAM_pal in ppu.s.
// Compiled in ARM mode (called via bl_long from .vram1).
//   vt_palette_write_lo : $3F00-$3F1F  (standard NES, also notified for VT)
//   vt_palette_write_hi : $3F80-$3F9F  (VT03+ hi byte; sets vt_palette_active)
void vt_palette_write_lo(u8 offset, u8 val);
void vt_palette_write_hi(u8 offset, u8 val);

// Rebuild the vt_palette_to_gba[] lookup and push composited entries to GBA
// palette RAM after any palette write.  Called once per VBlank from
// newframe_nes_vblank in ppu.s.  No-op until vt_palette_active goes true.
void vt_palette_rebuild_gba(void);

#endif // VT_ENHANCED_PALETTE


#if VT_HICOLOR_SPRITES

// ============================================================================
// Hi-res sprite OAM extension ($4180-$41FF)
// ============================================================================

// Each of the 64 OAM entries gets an 2-byte extension:
//   byte 0: tile index, right half of 16x8 sprite
//   byte 1: attributes for right half (palette, priority, flip)
#define VT_OAM_EXT_SIZE     128
extern u8 vt_oam_ext[VT_OAM_EXT_SIZE];

void vt_oam_ext_write(u8 offset, u8 val);

#endif // VT_HICOLOR_SPRITES


// ============================================================================
// PPU register extensions ($2100-$21FF mirror and new VT regs)
// ============================================================================

// $2107  VT PPU mode register
//   bit 0: enhanced mode enable (0 = NES compat, 1 = VT enhanced)
//   bit 1: extended palette enable (requires bit 0)
//   bit 2: 16x8 sprite mode (requires bit 0)
//   bit 3: extra nametable bit from attribute
#define VT_PPUREG_MODE      0x07   // offset from $2100

extern u8 vt_ppumode;              // shadow of $2107

// Called from ppu.s PPU_W handler when the address is $2107
void vt_ppumode_write(u8 val);

// ============================================================================
// Init / reset
// ============================================================================
void ppu_vt_init(void);
void ppu_vt_reset(void);

// Called from ppu.s vt_ppu_extended_W for writes to $2008-$21FF
void vt_ppu_reg_write(u8 page, u8 offset, u8 val);

// OneBus CHR-from-PRG bootstrap: copy 8KB of CHR pattern data from the
// current rombase + bank registers into NES_VRAM and mark every tile dirty
// so the GBA tile cache re-renders it.  Called from:
//   * vt_reset()           (initial CHR window after cart load)
//   * vt_ppu_reg_write()   ($2012-$2017, $2018, $201A bank-register changes)
//   * vt_reg_write()       ($4100, $4105, $4107-$410B PRG/CHR outer bank changes)
void vt_chr_sync_from_prg(void);
void vt_chr4_rebuild_if_dirty(void);  // deferred 4bpp assembly; call once/frame

#endif // __PPU_VT_H__
