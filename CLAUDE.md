# PocketVT -- handoff for Claude Code

PocketVT is a fork of PocketNES (GBA NES emulator) extended to run VT02/VT03/VT09
"OneBus" famiclone ROMs (NES 2.0 mapper 256 and friends).
**Read MAINTAINERS_GUIDE.md sections 60-73 before touching the PPU**: they record
what is proven, what was disproved, and why. Sections are numbered; newest last.

## Working agreement
- Do not ask to test or debug. Verify in-sandbox, ship complete trees.
- Honest tone: say what is unverified; correct earlier claims explicitly.
- Every session: ship playable .gba files plus the source tree.

## Build
```
LIBGBA=/path/to/libgba bash build_pvt.sh   # -> ../pvt_build/pocketvt.gba (core only)
EXTRA_CFLAGS=-DFOO BUILD=/some/dir bash build_pvt.sh   # variant build
tools/restage.sh [builddir]            # ALWAYS, right after build_pvt.sh
python3 builder.py rom1.nes [rom2.nes ...]   # run IN the build dir -> play_me.gba
sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make  # devkitARM build -> ./pocketvt.gba
```
Toolchain: gcc-arm-none-eabi, libnewlib-arm-none-eabi, libgba headers
(git clone https://github.com/devkitPro/libgba). Harnesses use libmgba (`libmgba-dev`).
The repo-root pocketvt.elf / pocketvt.gba are committed and come from the Docker
build; CI (.github/workflows/build.yml) rebuilds and commits them on every PR, so
after pushing, pull before committing again. The Docker build uses a different GCC than build_pvt.sh: its core is NOT
byte-comparable with a build_pvt.sh core. Compare within one path only.
`EXTRA_CFLAGS` reaches the assembler too (`.s` files use `#if`).

## Reference emulator: furb_cli + compare_furb.py (USE THIS instead of asking for screenshots)
Furbtendulator (the reference) builds as a headless Linux CLI from the gitignored
source in reference/Furbtendulator-main (tools/furb_cli/README.md):
```
apt install g++-multilib libmgba-dev; pip install numpy pillow
python3 tools/furb_cli/build.py                  # -> tools/furb_cli/build/furb_cli
python3 tools/compare_furb.py testroms/Scramble.nes --at 300,700 --input "320-325:Start" \
        [--core ../pvt_build/pocketvt.gba]        # -> furbcmp/cmp_t*.png + report.json
```
Same ROM, same input, keyed to NES frames on both sides (PocketVT's `frametotal`),
so slow carts stay aligned. `struct` = palette-independent picture match; the
mismatch lines list disagreeing colour pairs (black<->colour = positional,
colour<->colour = palette). First results: Time Pilot 100%, Scramble title 99.97% / gameplay
99.0% (residue = the 1-px terrain edge, +-1 row; decimation or a real 1-row
offset, undecided), Push the Ball 97.5%, Add 'em Up 81.8% with the top strip
flagged (open item 1).
furb_cli alone dumps PPM + raw palette indices + $2000/$4100 registers + palette
RAM + CPU RAM per frame. Games can diverge over long runs; compare early frames.

## THE TRAP THAT HAS BITTEN SIX TIMES
`build_pvt.sh` **rm -rf's the build directory**, deleting builder.py, every .nes
and every harness binary. Afterwards builder.py silently reports
"Successfully compiled 0 game(s)" and every test runs a bare ~105 KB core --
which looks exactly like a catastrophic regression. **Run `tools/restage.sh`
after every build** (it copies builder.py, testroms/*.nes and every .nes in
`$PVT_ROMS`; `$PVT_HARNESS` = dir of harness .c files to rebuild), and check that
a play ROM is larger than the ~107 KB core. builder.py now exits non-zero when
it injected 0 games.

## Regression procedure (do this for ANY change)
Controls: Star Ally, Lonely Island, Scramble, Lucky Lawn Mower (VT09), VG Pocket.
1. Palette RAM + framebuffer at frame 700 vs the previous core -> byte-identical
   is the goal (LI, Scramble, VG normally are).
2. **Frame-SET test** over 800 frames: the set of distinct frames must be a
   subset of the old core's. Any code-cost change re-times Star Ally (a blink
   shifts phase), so "differs at f700" alone is NOT a regression if every frame
   it renders was rendered before.
3. `tools/score_5bit.sh <builddir>` for Lucky Lawn Mower vs gg.png / lawn.png
   (currently ~96.5% opening, ~97.1% gameplay). Compare pixels in 5-BIT space
   (`>>3` both sides) -- mgba expands 5->8 bit differently from references.
4. `tools/compare_furb.py` on each testrom at a few frames: `struct` must not drop.
5. **Emulation speed = NMIs per 60 frames.** nmi_handler (timeout.s) increments
   a debug byte at 0x020007DF; read it before/after 60 runFrame calls.

## Diagnostic habits that paid off
- Read symbols fresh after every link (`arm-none-eabi-nm pocketvt.elf`); never
  hardcode addresses -- they move.
- Measure before theorising. Most dead ends here came from reasoning off
  end-of-frame snapshots. Instrument the pipeline stage by stage (see s.72).
- `-DVALUE_SENTINEL` build: BG pixels encode their palette slot (s.58).
- Tile-partition matching against a reference identifies which ROM tile a
  screen cell uses independently of palette (s.68).
- `-DFORCE_BK_REPAIR`: if a BKEXTEN screen looks scrambled, a clean result
  here means a cache stomp, not a decode fault (s.69).
- Diagnostic hooks used this project: DMA_LOG (mapVT.s video DMA), TLOG /
  NMIDBG style counters. Always build them into a SEPARATE build dir and
  verify the shipping tree is clean afterwards.

## Current state (s21b62)
See MAINTAINERS_GUIDE.md s.73 for detail.
- Working: Star Ally, Lonely Island, Scramble, Lucky Lawn Mower VT09, VG Pocket
  (all 50), Push the Ball, Time Pilot, Add 'em Up (full speed), Aero Gyrodine
  and Hex City X (gameplay).
- Speed (NMIs/60): Add 'em Up 60, Aero title 39, Hex title 35, LLM 55, SA 51,
  LI/Scramble/VG 60.

## Open work, in priority order
1. **Raster-split background CHR (s.73, WIP behind `-DVT_SPLIT_SLOTS`).**
   Aero Gyrodine's title (3 bands), Hex City X's title and Add 'em Up's top
   strip change $2016/$2017 mid-frame. Band recording (vt_band_mark) and
   frame-end processing (vt_bands_frame_end) are in ppu_vt.c and ON; with
   VT_SPLIT_SLOTS the extra bands are decoded into BG char blocks 2/3 and
   bg0cntbuff gets their char base per scanline. That made Aero's title
   run at 59/60 -- **but hangs Add 'em Up** (no NMIs, blank screen after
   ~f200), so blocks 2/3 are not free on that cart (it has a separate CHR ROM).
   Next step: find what occupies 0x06008000-0x0600DFFF on Add 'em Up
   (VRAM inventory harness in s.72; also check PocketNES's 1K bank cache /
   bank_search path), then either relocate or reserve.
2. Aero/Hex titles still don't reach 60/60 without the split slots.
3. Scramble's shot (s.70c): one-pixel sprite on texture row 7; the sprite
   affine matrix (pd=336, 8x16 double-size) never samples rows 3/7/12. Rotate
   dropped rows per frame like the BG's scale75.
4. VT369 platform port (s.47); Table Soccer (mapper 419, reference in
   reference/nrs/mapper419.cpp).

## Reference material
LOCAL ONLY, gitignored, never commit (the user supplies them as test.zip):
- reference/nrs/: NintendulatorNRS OneBus sources (h_OneBus.cpp, OneBus.cpp,
  mapper256.cpp, ...). The authority on VT behaviour.
- reference/NESdev_Wiki-*.xml: NESdev wiki exports (VT02+ pages, MMC3).
- reference/Furbtendulator-main/: the full Furbtendulator source (from
  Furbtendulator-main.zip); tools/furb_cli builds it headless.
- testroms/: Add 'em Up, Push the Ball, Scramble, Table Soccer, Time Pilot.
  The controls (Star Ally, Lonely Island, LLM, VG Pocket) are NOT in it; ask.

In git:
- DATASHEET_DIGEST*.md: VT02/VT03 datasheet notes. Bit numbering there is
  1-INDEXED; the code is 0-indexed. DATASHEET_DIGEST.md appendices A/B hold the
  spec-vs-code verdicts (no VT extra opcodes; NMI polarity; $410F).
- MAINTAINERS_GUIDE.md (s.74 maps the retired docs and their open items),
  CHANGELOG.md (0.1-0.5.2 only), README.md.
- Reference screenshots supplied (NOT in the tree; ask for them):
  gg.png / 1.png (LLM opening), lawn.png (LLM gameplay), vgp.png / vgp2.png
  (VG menus), aero.png, hex.png, add.png, add2.png, bug.png, shot.png.
