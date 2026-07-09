# PocketVT 4bpp Render Fix — SESSION SENDOFF (resume here)

## ONE-LINE STATUS
The game RENDERS (Lonely Island overworld appeared: trees/paths/water/
buildings/sprite — proof the whole 4bpp pipeline works). Two remaining issues:
(1) it reverts to a blue backdrop after ~2 frames (2bpp cache overwrites our
VRAM), (2) colours are off (too much green/grey), plus a known-pending
(3) 256x240 -> 240x160 GBA scaling check.

## THE BIG WINS THIS SESSION (all verified locally under VBA)
- I GOT A WORKING LOCAL BUILD+RUN LOOP. No more depending on the user's builds:
  - Build objects: `/home/claude/build_pvt2.sh` (uses arm-none-eabi-gcc +
    REAL libgba headers at /home/claude/libgba-master/include).
  - Link: `arm-none-eabi-gcc -g -mthumb -mthumb-interwork -Wl,-z,muldefs
    -T /home/claude/pocketvt/src/gba_cart_my.ld -nostartfiles $OFILES
    gba_crt0_my.o -o pocketvt.elf` (OFILES = all *.o in pvt_build2 except
    gba_crt0_my.o; biosstubs.o provides LZ77UnCompVram).
  - objcopy -O binary, then `/home/claude/gbafix pocketvt.gba -cPNES
    -tPocketNES` (gbafix BUILT from devkitPro gba-tools GitHub; NO -p pad).
  - Inject ROM exactly like builder.py: emu .gba + struct.pack("<32sIIII",
    name32, len(nes),0,0,0) + nes bytes. (48-byte header, NO padding.)
  - Run headless: Xvfb :99 + `vba play_me.gba`, screenshot with
    `import -window $(xdotool search --name VisualBoy|head -1)`, count colours
    with PIL. This FAITHFULLY reproduces the user's device behaviour.
  - NOTE: devkitARM/devkitpro apt is BLOCKED by the sandbox egress proxy
    (403); github.com IS reachable (that's how gbafix was obtained). Docker
    unavailable. So this hand-rolled pipeline is the way to build locally.

- ROOT CAUSES FOUND & FIXED (these were the long black-screen saga):
  1. ARM/THUMB INTERWORKING: vt_chr4_rebuild_if_dirty was THUMB, but the vblank
     handler calls it via bl_long (ARM: mov lr,pc; ldr pc,=label — no thumb
     bit). Jumped into thumb code as ARM -> garbage -> black. FIX:
     __attribute__((target("arm"))) on it + its helpers. THIS was the main
     black-screen cause. Confirmed via a debug colour-write that it then ran.
  2. IRQ TIME OVERRUN: the 32K-iteration per-bit tile assembly was too slow to
     run inside the vblank IRQ -> tripped check_canaries (crash screen, PC in
     vt_chr4_assemble, LR=0, independent of stack size). PROVED by assembling
     1 page -> no crash. FIX: replaced per-bit assembly with a 256-entry SPREAD
     lookup table (vt_spread[]) — verified 0-mismatch vs the per-bit method,
     ~8x faster, fits the IRQ budget. Crash gone, GAME RENDERED.
     (Stack was a red herring; tried split helpers, private-stack inline-asm
     trampoline, and enlarging the IRQ stack — none was the real fix. IRQ stack
     left at __iwram_top - 0x200 in gba_cart_my.ld; harmless, can stay.)

## EXACTLY WHERE WE STOPPED (the current bug to fix FIRST next session)
After it rendered for ~2 frames it reverts to a uniform light-blue backdrop
(0x57B9FF-ish, GBA palette entry 0). DIAGNOSIS: PocketVT's inherited PocketNES
2bpp tile cache rewrites BG_VRAM (0x06000000) every frame, OVERWRITING our 4bpp
tiles after frame 1. Last edit changed vt_chr4_rebuild_if_dirty so that in
16-colour mode it re-copies tiles+palette to VRAM EVERY frame (not just when
dirty) — but the build still went blue, so that alone isn't enough.
NEXT STEP: the 2bpp BG decode in ppu.s writes BG_VRAM during the frame; our
vblank overlay either runs at the WRONG TIME (before the 2bpp cache writes) or
the 2bpp cache also runs in/after vblank. Options to try:
  (a) In 16-colour mode, SUPPRESS the 2bpp BG/sprite tile-cache writes entirely
      (skip render_recent_tiles / render_tiles_2 path in ppu.s when
      vt_reg_2010 & 0x06), since our 4bpp overlay fully replaces them.
  (b) Or ensure vt_chr4_copy_to_vram runs at the LATEST point in the frame,
      after all 2bpp cache writes (move the bl_long later in vblank, or to the
      hblank/just-before-display point).
  (a) is cleaner and likely correct: the 2bpp cache is meaningless in 4bpp mode.
Find where ppu.s decides to run the BG tile cache and gate it on
vt_reg_2010 & 0x06 == 0.

## THEN (in order)
- COLOURS: frame-3 capture was mostly green/grey. The palette index layout is
  verified (see below) but the COLOUR-SET (sub-palette) bits come from the NES-
  style attribute table -> GBA tilemap palette field (ppu.s do_attribute,
  ~line 1320, puts 2-bit attr into tilemap bits 12-13). Verify our 4 sub-
  palettes line up with what the tilemap selects; the green/grey suggests
  sub-palette/group mismatch or the bg/spr bit. Re-trace vs furb_cli if needed.
- SCALING (user-raised, pending): NES internal 256x240 -> GBA 240x160. PocketNES
  has SCALED mode (ppu.s vblscaled ~line 2857+, emuflags SCALED bit, dispcnt/
  bg scaling). Our overlay writes the same BG_VRAM tiles the normal renderer
  scales, so it SHOULD inherit scaling — but verify the 4bpp path looks right
  in scaled mode (and that horizontal 256->240 squeeze / crop is applied).
- Then sprites (OBJ) 4bpp: we fill OBJ palette (0x05000200) but haven't
  verified sprite tiles use vt_chr4 / 16-colour. BG first, then sprites.
- AUDIO loop bug (separate, deferred): likely timer-IRQ $4101/$4103/$4104 or
  DWS/PCM restart. Untouched. Do last.

## VERIFIED-CORRECT PIECES (do not re-derive; trust these)
- 4bpp TILE LAYOUT: a VT 4bpp tile = 32 contiguous source bytes; bytes 0-15 =
  low 2bpp tile (plane0=0-7, plane1=8-15), bytes 16-31 = high tile (plane2,
  plane3); pixel = p0|p1<<1|p2<<2|p3<<3. Pixel-EXACT vs Furbtendulator on both
  ROMs (262144 + 524288 pixels, 0 mismatches). In ppu_vt.c vt_chr4_assemble.
- SPREAD TABLE: vt_spread[b] scatters plane byte b to GBA 4bpp nibble order;
  row = spread[l0]|spread[l1]<<1|spread[h0]<<2|spread[h1]<<3. 0-mismatch vs
  per-bit method. Built lazily by vt_spread_init (consider moving to init).
- 16-COLOUR PALETTE INDEX (COLCOMP=0): bit-scattered, from furb_cli +
  DATASHEET_DIGEST.md p.19: idx = p0 | p1<<1 | colorset<<2 | bgspr<<4 | p2<<5
  | p3<<6; transparent if (idx & 0x63)==0 -> entry 0. COLCOMP=0 => colour =
  nes_rgb[ vt_palette_ram[idx] & 0x3F ] (NOT the SAT/LUM/PHA LUT — that's
  COLCOMP=1 only). In ppu_vt.c vt_build_16color_palette.
- WHY 4bpp at all: furb_cli proved both test ROMs run $2010 = $1E/$1F
  (BK16EN|SP16EN, COLCOMP=0) = 16-colour tiles with NES-style palette.

## KEY FACTS / CONSTRAINTS (from the user, keep honouring)
- Furbtendulator (/home/claude/Furbtendulator-main, /home/claude/furb_cli) is
  the PERFECT reference. NEVER change its emulation logic; logging-only
  instrumentation is OK. furb_cli is built & instrumented (g_ppu_log).
- Datasheet digests are OFFICIAL VT reference: DATASHEET_DIGEST.md (VT03),
  DATASHEET_DIGEST_VT02.md (VT02). Use them; DON'T confuse NES vs VT — VT is a
  heavily-modified architecture. PocketNES source shows NES->GBA translation
  only; don't conflate with PocketVT's VT paths.
- User wants the games PLAYABLE, not just non-crashing.
- The user builds on real devkitARM; our local VBA build is for our own
  debugging. Both should agree now.

## FILE MAP
- Source: /home/claude/pocketvt/src/ — ppu_vt.c (all the 4bpp C: assemble,
  copy_to_vram, build_16color_palette, rebuild_if_dirty, vt_spread, palette
  writes), ppu.s (vblank handler ~line 2812 calls `bl_long
  vt_chr4_rebuild_if_dirty` after showfps_ ~line 2847; 2bpp BG cache
  render_recent_tiles ~line 972 / render_tiles_2; nes_rgb exported .global
  ~line 207; do_attribute ~1320), gba_cart_my.ld (__sp_irq = top-0x200),
  vt_regs.c ($2012-2017 CHR bank writes call vt_chr_sync_from_prg).
- Test ROMs: /home/claude/testroms/Lonely Island.nes, Star Ally (VT03).nes.
- Docs (all also in /mnt/user-data/outputs): FIX_4BPP_DESIGN.md (full design +
  history), CODE_AUDIT.md (root-cause analysis), DATASHEET_DIGEST*.md, this
  SENDOFF.md.
- Build dir: /home/claude/pvt_build2/ (objects + pocketvt.elf/.gba +
  play_me.gba). Rebuild+run one-liner is in build_pvt2.sh + the link/gbafix/
  inject/VBA sequence above.
- Verified-logic test harnesses: /home/claude/piece1_test/ (tile assembly +
  spread-table equivalence vs Furb).

## IMMEDIATE NEXT ACTION
Open ppu.s, find where the 2bpp BG tile cache writes BG_VRAM each frame
(render_recent_tiles / render_tiles_2, ~line 960-1160), and gate it OFF when
(vt_reg_2010 & 0x06) so it stops overwriting our 4bpp overlay. Rebuild via the
local pipeline, run under VBA, confirm the overworld map PERSISTS (not just 2
frames). Then move to colour correctness, then 256x240->240x160 scaling.
