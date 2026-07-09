#ifndef __CONFIG_H__
#define __CONFIG_H__

#define GCC 1

//#define COMPY 1  //can be defined in the makefile with -D COMPY

#define DEBUG 0     //Set to 1 to have code called at the end of each instruction

#define VERSION_NUMBER "PocketVT 0.2.0"

// ============================================================================
// VT CHIP SELECTION
//
// PocketVT is a VT emulator: the VT register block, ADPCM mixer, enhanced
// palette, hi-res sprite OAM extension, the extra 6502 opcodes, AND the
// VT09 opcode-encryption infrastructure are ALWAYS compiled in.  Encryption
// is enabled at runtime when the loaded ROM is mapper 256 submapper 13-15
// (Cube Tech / Karaoto / Jungletac) or any VT09-family mapper.  The Makefile
// flag `VT=VT09` is accepted for backward compatibility but does not change
// the build any more.
//
// Old PocketNES-only behaviour is no longer a build option; use the upstream
// PocketNES project for that.
// ============================================================================

// VT support is always present in PocketVT.
#define VT_MODE 1

// VT09 alias kept for backward compatibility; the build is identical.
#if defined(VT09) && !defined(VT03)
  #define VT03 1
#endif

// ============================================================================
// LOCKED OPTIONS  (do not change)
// ============================================================================
#define REDUCED_FONT      1  // Use reduced font to save memory
#define RESET_ALL         1  // Zero-fill variables when loading ROMs
#define VERSION_IN_ROM    1  // Jumps between sections are long jumps
#define OLDSPEEDHACKS     0  // Use old speed hack system (off)
#define BRANCHHACKDETAIL  0
#define EDITBRANCHHACKS   0
#define APACK             1  // Include APLIB compression

// ============================================================================
// CPU OPTIONS
// ============================================================================
#define HAPPY_CPU_TESTER  1  // More accurate CPU emulation
#define FULL_DMC          0  // Emulate DMC cycle stealing (off for speed)
#define LESSMAPPERS       0  // Set 1 to strip uncommon mappers

// VT CPU extensions -- always enabled (PocketVT is a VT emulator)
// Extra opcodes: ADX, LDAXD, LDAD, PHX, PLX, PHY, PLY, TAD, TDA
// (patched into the 6502 dispatch table in 6502_vt.s at mapper init)
#define VT_EXTRA_OPCODES  1

// VT09 opcode encryption (XOR 0xA1) is always compiled in.  Enabled at
// runtime by mapVTinit when the loaded ROM uses mapper 256 submapper 13-15
// or another encryption-using VT mapper.
#define VT09_ENCRYPTION 1

// ============================================================================
// GRAPHICS OPTIONS
// ============================================================================
#define DIRTYTILES        1  // Buffer changed CHR-RAM tiles
#define USE_BG_CACHE      1  // Background tile cache
#define SPRITESCAN        1  // 8-sprite-per-scanline check
#define DRAW_ATTRIBUTE_TABLES 0
#define MIXED_VRAM_VROM   1  // Support mixed CHR-RAM/CHR-ROM

// VT enhanced graphics -- always enabled
// 6-bit RGB palette (512 colors) instead of standard NES 64-color table
#define VT_ENHANCED_PALETTE  1
// 16x8-pixel sprite mode supported by VT03/VT09
#define VT_HICOLOR_SPRITES   1

// ============================================================================
// SOUND OPTIONS
// ============================================================================
// IMA-ADPCM sample channels present on VT03/VT09 (two channels) -- always enabled
#define VT_ADPCM_SOUND  1

// ============================================================================
// MEMORY / SAVE OPTIONS
// ============================================================================
#define SAVE              1
#define SAVE32            0
#define SAVE_FORBIDDEN    0
#define SAVESTATES_FORBIDDEN 0

#define USE_GAME_SPECIFIC_HACKS 1
#define USE_ACCELERATION  1
#define MULTIBOOT         0
#define GOMULTIBOOT       0

// ============================================================================
// MISC OPTIONS
// ============================================================================
#define CHEATFINDER       1
#define EDITFOLLOW        1
#define LINK              1
#define RTCSUPPORT        1
#define PREVIEWBUILD      0
#define MOVIEPLAYER       0
#define CRASH             1
#define VISOLY            1

// ============================================================================
// COMPY / GBAMP BUILD OVERRIDES
// ============================================================================
#if defined COMPY
  #undef  USE_ACCELERATION
  #define USE_ACCELERATION 0
  #undef  SAVE
  #define SAVE 0
  #undef  GOMULTIBOOT
  #define GOMULTIBOOT 1
  #undef  MULTIBOOT
  #define MULTIBOOT 1
  #undef  CRASH
  #define CRASH 0
  #undef  USE_GAME_SPECIFIC_HACKS
  #define USE_GAME_SPECIFIC_HACKS 0
  #undef  RTCSUPPORT
  #define RTCSUPPORT 0
  #undef  LESSMAPPERS
  #define LESSMAPPERS 1
  #undef  VISOLY
  #define VISOLY 0
  #undef  CHEATFINDER
  #define CHEATFINDER 0
#elif defined GBAMP
  #undef  CHEATFINDER
  #define CHEATFINDER 0
  #undef  SAVE
  #define SAVE 0
  #undef  MOVIEPLAYER
  #define MOVIEPLAYER 1
  #undef  CRASH
  #define CRASH 0
#endif

#ifndef CARTSAVE
#define CARTSAVE    SAVE
#endif
#ifndef SAVESTATES
#define SAVESTATES  (SAVE | MOVIEPLAYER)
#endif

#ifndef GCC
  #define GCC 0
#endif

#endif // __CONFIG_H__
