# PocketVT 4bpp (16-colour) Render Path -- Design & Implementation

## Why (root cause, data-backed)
furb_cli traces prove the test ROMs run COLCOMP=0 (NES-style palette) with
$2010 = $1E/$1F => BK16EN+SP16EN+SPEXTEN+BKEXTEN(+PIX16EN). i.e. 16-colour
(4bpp) tiles. PocketVT has no 4bpp render path: vt_chr_sync_from_prg
de-interleaves and keeps only planes 0-1 (a 2bpp half), then the inherited
PocketNES 2bpp tile cache (CHR_DECODE) renders it -> garbled.

## Verified VT 4bpp tile layout (from Furb OneBus.cpp:720-810, ground truth)
Rendering address bits: 10.B TTTTTTTT PRRR
  bits0-2 (RRR) = tile row, bit3 (P) = bitplane(0/1 select within low pair),
  bits4-11 = tile number, bit12 = pattern table half, bit13 = BG marker.
- Low planes (0,1): at RenderAddr (base).
- High planes (2,3): at RenderAddr | 0x4000  (bit14 set) -- i.e. a SEPARATE
  CHR region 16KB above the low planes, NOT interleaved within the tile.
- EVA (extension video addr) is OR'd into the low 10 bits when SPEXTEN/
  BKEXTEN set (the $2010 bits 3,4 our ROMs also set).
- Pixel assembly (Furb): plane0|plane1 -> low 2 bits; plane2|plane3 ->
  shifted up (<<5 in Furb's internal TileData packing) = the high 2 bits of
  a 4-bit pixel. Net: 4-bit (0..15) pixel index.

## Mapping to GBA
GBA 4bpp tiles are 32 bytes (8 rows x 4 bytes), 4 bits/pixel, 16-entry
sub-palettes. PocketNES already stores GBA tiles at tilenum<<5 (4bpp) but
only fills low 2 bits via CHR_DECODE (2 planes). For VT 4bpp we must fill
all 4 bits per pixel and use 16-colour GBA sub-palettes.

## Implementation plan (incremental, each piece reviewable)

### Piece 1 -- keep all 4 planes in a VT CHR buffer (C side, ppu_vt.c)  [DONE + VERIFIED + CRASH FIXED]
STATUS: IMPLEMENTED, VERIFIED pixel-exact. TWO crashes found via the user's
devkitPro builds; first fixed, second diagnosed (fix pending).

CRASH #2 -- RESOLVED (verified on the user's build). Diagnosis was correct:
vt_chr_sync_from_prg ran the full 32K-iteration 4bpp assembly + an 8KB copy
on EVERY $2012-$2017 write (6+ per frame) = ~200K ops in the CPU write
handler -> timing/stack crash. FIX APPLIED: per-write path made cheap again
(only the NES_VRAM 2bpp fill, same cost as the old working path) and sets
vt_chr4_dirty + snapshots the banks; the heavy 4bpp assembly moved into a
separate vt_chr4_rebuild_if_dirty() meant to run at most once per frame; the
redundant double-work removed. RESULT: the user's build NOW BOOTS -- no
crash, no register dump. Shows PocketVT "Loaded battery" status then a solid
screen; runs indefinitely (confirmed under VBA here). Blocker cleared.

CURRENT STATE: boots cleanly but NO game graphics yet -- expected, because
Piece 2 is not done. vt_chr4_rebuild_if_dirty() is verified but NOT YET
CALLED (no frame hook) and vt_chr4_buf is NOT yet wired into the renderer.
The 2bpp fallback fills NES_VRAM, which for a 4bpp game looks blank/garbled
(the brief garble flash the user saw is consistent with a 1-2 frame
transient, too fast for ~0.4s capture). NEXT: Piece 2.

(historical detail) CRASH #2 PC was ~0x080036D4 inside the 4bpp loop; the
per-write cost was the cause, not the inner-loop reads (which were correctly
masked). Timing faults move around, which is why the PC was hard to pin.

#### (historical) CRASH #1 -- fixed

CRASH (found via the user's devkitPro build + VBA): the first Piece-1 build
crashed inside vt_chr_sync_from_prg (PocketNES crash handler, PC ~0x080036E4
= func base 0x080035E8 + 0xFC; faulting load was `ldrb r3,[r3,#8]`, i.e.
lt[r+8]/ht[r+8]). CAUSE: I masked only the BASE tile pointer
(lt = rombase + (off & mask)) but then read lt[r+8] WITHOUT re-masking, so a
32-byte source block near the top of ROM read up to 15 bytes past the end ->
data abort. R0 in the dump = 0x00004DA0 (a bad low pointer), consistent.
FIX: mask EVERY source byte access individually:
    l0 = rombase[(lo_base + r)     & mask];
    l1 = rombase[(lo_base + r + 8) & mask];  (etc for h0,h1)
Re-verified after the fix: still 0 pixel mismatches vs Furb on both ROMs.
Output-buffer writes confirmed in-bounds (max index 16383 < 16384).
This crash was only catchable with a real build+run -- exactly why the
user's devkitPro .gba was the missing piece. My from-scratch stub-header
build cannot reproduce the devkitARM runtime (it shows black regardless),
so the user's build remains the source of truth for runtime behaviour.

### Earlier Piece 1 notes
STATUS: IMPLEMENTED and VERIFIED pixel-exact against Furbtendulator.
What changed: vt_chr_sync_from_prg's four_bpp branch now assembles full GBA
4bpp tiles into a new 16 KB buffer vt_chr4_buf (8 pages * 64 tiles * 32 B).
Proven layout: a VT 4bpp tile = 32 contiguous source bytes; bytes 0-15 =
low 2bpp tile (planes 0,1), bytes 16-31 = high 2bpp tile (planes 2,3);
pixel = p0|p1<<1|p2<<2|p3<<3. (Matches Furb h_OneBus.cpp:322-328 split.)
VERIFICATION: a standalone harness reproduced Furb's chrLow/chrHigh split
and compared every pixel of every tile vs my assembly:
  Lonely Island: 4096 tiles, 262144 pixels, 0 mismatches.
  Star Ally:     8192 tiles, 524288 pixels, 0 mismatches.
The exact inner loop now in ppu_vt.c was re-verified the same way: 0
mismatches on both ROMs. Compiles clean (arm-none-eabi, -DVT_MODE=1).
The function ALSO still fills NES_VRAM with the low 2bpp half as a no-worse
fallback until Piece 2 wires vt_chr4_buf into the renderer.

### Original plan for Piece 1 (for reference)
Replace the "planes-0-1 half" extraction with a full 4bpp-aware copy.
Because the high planes live at +0x4000 in VT CHR space, the cleanest
approach is a dedicated VT 4bpp tile buffer laid out as GBA-ready 4bpp
tiles, built directly in vt_chr_sync_from_prg:
  - For each tile, fetch low-plane bytes (planes 0,1) from the page's
    source region and high-plane bytes (planes 2,3) from source+0x4000
    (masked), then assemble 8 rows x 4 bytes of GBA 4bpp pixels.
  - Write into a NEW buffer VT_CHR4 (8KB*2 = 16KB for 512 tiles*32B), not
    NES_VRAM (which the 2bpp cache owns).
This keeps the 2bpp path untouched and isolates 4bpp.

### Piece 2 -- STATUS: implemented; BLACK SCREEN -- needs Piece 3 to be visible
The vblank handler now calls vt_chr4_rebuild_if_dirty(), which assembles the
4bpp tiles and copies them into BG_VRAM at the cache's exact tile addresses
(including the +0x2000 split for tiles >= 0x100, ppu.s:984-988).
User's build = BLACK SCREEN.
DIAGNOSIS (code-grounded): black is the EXPECTED result of putting 4bpp
pixels (values 0..15) through a 4-COLOUR palette. PocketVT sets only FOUR
entries per GBA sub-palette (ppu_vt.c:222-225). Entries 4..15 are unset =
black, so 4bpp pixels >= 4 render black -> black screen. Piece 2 is likely
WORKING (tiles in VRAM) but invisible without Piece 3. Lesson: for 4bpp the
tiles and the 16-colour palette MUST ship together; combine 2+3 next build.
LOCAL BUILD: got a from-scratch build to RUN this session (rendered the real
PocketVT menu) after building gbafix from devkitPro's gba-tools GitHub repo
-- so prior local black screens were the missing gbafix, not the code. But
the appended ROM still isn't FOUND by find_nes_header in my local build
(placement detail vs builder.py), so the user's devkitARM build is still
ground truth for the actual game.

### Piece 2 -- point the GBA BG/OBJ char base at VT_CHR4 in 4bpp mode
When four_bpp, the BG/sprite character base must read VT_CHR4 instead of
the 2bpp cache. Two options:
  (a) DMA/copy VT_CHR4 into the GBA BG char block the renderer uses, or
  (b) set BGxCNT char base to a VRAM region we fill with VT_CHR4.
Option (a) is simplest and matches PocketVT's existing copy model.

### Piece 3 -- THE HARD PART: palette index bit layout (why builds are black)
DISCOVERY (furb_cli OneBus.cpp 740-812 + GetPalIndex 1348): COLCOMP=0
16-colour palette index is NOT contiguous 0..15. Furb packs the pixel as
  low 2 planes -> bits 0,1 ; high 2 planes -> bits 5,6 ; set bits -> 2,3,4
and returns Palette[that index] directly.
THREE BUGS made my Piece 3 black:
  1. vt_palette_write_lo truncates offset & 0x1F -> palette entries past
     0x1F never stored.
  2. palette writes set vt_palette_dirty, not vt_chr4_dirty -> my 16-colour
     rebuild (only called from the chr4 rebuild) often never runs.
  3. my GBA sub-palette layout assumed contiguous group*16+slot; real VT
     index is bits {0,1,5,6}+set, so entries didn't line up with the
     contiguous 0..15 pixels I packed into vt_chr4_buf.
CORRECT DESIGN: for each GBA sub-palette entry v (0..15), load the VT colour
whose VT index = (v&1)|((v>>1&1)<<1)|((v>>2&1)<<5)|((v>>3&1)<<6)|setbits:
   GBA_subpal[group][v] = nes_rgb[ Palette[vtIndex] & 0x3F ]   (COLCOMP=0)
plus: store full palette RAM (not &0x1F), trigger rebuild on palette writes,
run rebuild when chr OR palette changed. Implement all together next build.

### Piece 3 -- 16-colour palette + BGxCNT colour-depth bit
- BGxCNT bit7 (colour mode): leave 4bpp (PocketNES already uses 4bpp GBA
  tiles); the change is we now use all 16 entries per sub-palette, not 4.
- Palette: in COLCOMP=0 + 16-colour, each pixel is a 4-bit pattern index
  plus attribute bits selecting the sub-palette. Build 16-entry GBA
  sub-palettes from nes_palette/MAPPED_RGB indexed by the VT colour-address
  decode (digest S9: bits1,2,6,7 = pattern colour; 3,4 = set; 5 = spr/bg).
  Must extend vt_palette_rebuild / run_palette to emit 16-wide sub-palettes
  in 16-colour mode.

### Piece 4 -- the tilemap palette field
PocketNES packs the 2-bit NES attribute into GBA tilemap bits12-15 (palette
0..3). In 16-colour mode the pixel already spans the sub-palette; the
attribute still selects WHICH 16-colour group. Verify the tilemap entry
still encodes the right sub-palette and that 16 entries are reserved per
group (GBA has 16 BG palettes of 16 -> only room for some groups; may need
256-colour BG mode instead). THIS is the trickiest interaction and needs
the most care / testing.

## Risk / honesty
- This is a substantial change across ppu_vt.c (C) and ppu.s (hand ARM),
  plus GBA palette/BGCNT setup. It cannot be fully validated without
  running the real devkitPro build on hardware/emulator.
- GBA has only 16 BG sub-palettes of 16 colours; if VT uses >16 distinct
  16-colour groups on screen, 256-colour BG mode is required (bigger
  change). Start with 16-colour sub-palettes; escalate only if needed.
- Plan: implement Piece 1 (self-contained, testable in isolation by
  dumping VT_CHR4), then 2/3/4. Verify with furb_cli's framebuffer as the
  golden reference where possible.

## SESSION UPDATE (local build+test loop now working)
Got a faithful local repro: build with arm-none-eabi-gcc + real libgba headers
+ gbafix (from devkitPro gba-tools GitHub) + builder.py-exact ROM injection,
run under VBA headless. Behaves like the user's device.

ROOT CAUSES FOUND & FIXED this session:
1. ARM/THUMB INTERWORKING (the long-standing black screen): vt_chr4_rebuild_
   if_dirty was THUMB but vblank calls it via bl_long (ARM, mov lr,pc; ldr pc,=
   -- no thumb bit). Fixed with __attribute__((target("arm"))). Confirmed via a
   magenta debug write that the function then executed (~10 colours on screen).
2. IRQ-TIME OVERRUN (crash in vt_chr4_assemble, LR=0, PC in the arithmetic
   inner loop, independent of stack size): the 32K-iteration per-bit assembly
   was too slow to run in the vblank IRQ -> tripped check_canaries / corrupted
   timing. PROVED by assembling only 1 page -> no crash (but mostly black).
   FIX: replaced per-bit assembly with a 256-entry SPREAD lookup table
   (vt_spread[b] scatters a plane byte to GBA 4bpp nibble order; verified
   0-mismatch vs the per-bit method). Crash gone.
   NOTE: tried (a) splitting into helpers, (b) a private-stack inline-asm
   trampoline, (c) enlarging the IRQ stack (0x200/0x400). The stack was NOT the
   real cause; time/volume was. Current IRQ stack left at 0x200 (harmless).
3. NEW REGRESSION (current): after the fast-assembler change, vt_chr4_rebuild_
   if_dirty is NOT being reached (a green debug write before the dirty check
   doesn't show; screen is a uniform light-blue backdrop, NOT the status-text
   flash seen earlier and NOT a crash screen). The bl_long call IS present in
   the vblank literal pool (verified). Hypothesis: the new EWRAM_BSS globals
   (vt_spread[256], vt_spread_ready, vt_chr4_stack[1024]) shifted memory /
   boot now hangs before normal rendering, OR vt_spread_init on first call
   misbehaves. NEXT: confirm vblank runs at all (write a colour at TOP of the
   vblank C-callable path), check whether boot reaches gameplay, and re-check
   that the new globals didn't collide with a fixed address. Consider building
   vt_spread at init time (not lazily inside the IRQ) and dropping the now-
   unused vt_chr4_stack buffer.

VERIFIED-CORRECT PIECES (unchanged, still good):
- 4bpp tile assembly: pixel-exact vs Furbtendulator (both ROMs).
- 16-colour palette index bit layout: p0|p1<<1|set<<2|bgspr<<4|p2<<5|p3<<6,
  transparent if (idx & 0x63)==0; COLCOMP=0 -> nes_rgb (NOT SAT/LUM/PHA).
- spread table: 0-mismatch vs per-bit assembly.
