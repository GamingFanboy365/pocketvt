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
**Docker `make` is incremental and does not rebuild .s files when config.h
changes** (no asm header deps): `sudo rm -rf build` first after touching any
define (guide s.79g).
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
It also does what the GUI does (s.76): FDS/NSF/VS, any setting (--set/--config),
any controller (--device, p1..p4:, key:, mouse:, trigger), WAV/AVI, savestates,
movies, cheats, DIP, --trace. `python3 tools/furb_cli/selftest.py` checks them.

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
3. Colours: compare_furb's `exact5` against Furbtendulator is the reference
   now (the user retired the gg.png/lawn.png photo checks, s.79h). Compare in
   5-BIT space (`>>3` both sides) -- mgba expands 5->8 bit differently.
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
- tools/probes/: one-question mGBA harnesses (speed, frame hashes, VRAM/RAM
  dumps, live PC/lastbank, single-step watchpoints, stack depth). README there.
- Diagnostic hooks used this project: DMA_LOG (mapVT.s video DMA), TLOG /
  NMIDBG style counters. Always build them into a SEPARATE build dir and
  verify the shipping tree is clean afterwards.

## Current state (after s.79)
See MAINTAINERS_GUIDE.md s.79 (Table Soccer, VG colours, real BIOS, wait
states, sound) and s.77-78 (raster-split slots, speed).
- Working: Star Ally, Lonely Island, Scramble, Lucky Lawn Mower VT09, VG Pocket
  (all 50; colours now Furbtendulator's), Push the Ball, Time Pilot, Add 'em Up,
  Aero Gyrodine, Hex City X, Table Soccer VT03 (mapper 419; menus/match 96-99%).
- Raster-split slots: 3 (block 2, block 3, and the primary's unused pattern
  half when free), tallest bands first, 4bpp and 2bpp, MMC3 CHR writes mark
  bands too. `-DVT_SPLIT_SLOTS=0` turns them off.
- VT timer: N+1 lines free-running, N+2 after an in-picture rephase, N from
  vblank (`VT_TIMER_NPLUS1`); split lines match Furbtendulator exactly.
- WAITCNT = 0x4317 at boot (`VT_FAST_WAITCNT`): 3/1 + prefetch. Speed (NES fps):
  Time Pilot 58-60, Scramble 59-60, SA 44-56, Aero/Hex titles 41-42 then 60,
  Table Soccer 43, LLM VT09 60, LI/VG/Add 'em Up 60.
- PRG page 0 lives in OBJ VRAM until the first VT sprite slot is written, then
  moves to its ROM/EWRAM twin (vt_prg_evict_obj). Never write OBJ slots 0-7
  without it.
- Hardware: build_pvt.sh cores now get a bootable header (tools/gbafix.py);
  every test ROM boots through the real BIOS (`compare_furb.py --bios
  reference/gba_bios.bin`, BIOS supplied by the user, gitignored).
- The IWRAM user stack is ~150 bytes above .bss; ALL new C globals go in
  EWRAM_BSS (check `__bss_end__` = 0x03007B84 after a link). Heavy VT C runs on
  the 3K EWRAM stack. Measure with tools/probes/spmin from power-on.

## Open work, in priority order
1. VT369 platform port (s.47): Lucky Lawn Mower VT369 and Table Soccer VT369
   (both black in PocketVT). Both run in furb_cli (Table Soccer VT369 only
   warns that VT369-00.BIN is missing; guide s.79h), so compare_furb works.
2. Sound (s.79d): VT ADPCM $4120-$412F only mixes while NES DMC plays; the
   TK-8007 voice (Table Soccer) is not played; second APU $4020-$402F and the
   VT369 sound CPU are absent. Measure with tools/probes/pvtwav + wavcmp.py.
3. Stale BG tile cache (item 21): Push the Ball's 20 px, and a band of missing
   tiles in a few VG games that moves with timing (nametables match exactly).
4. Aero/Hex title speed 41-42/60 (s.78d); profile with cycprof.
5. Table Soccer formation screen: two 6-line strips need a sixth char block.
6. Scramble's shot (s.70c): one-pixel sprite on texture row 7 dropped by the
   sprite affine matrix.

## Reference material
LOCAL ONLY, gitignored, never commit (the user supplies them as test.zip):
- reference/nrs/: NintendulatorNRS OneBus sources (h_OneBus.cpp, OneBus.cpp,
  mapper256.cpp, ...). The authority on VT behaviour.
- reference/NESdev_Wiki-*.xml: NESdev wiki exports (VT02+ pages, MMC3).
- reference/gba_bios.bin: the real GBA BIOS (user-supplied) for
  `compare_furb.py --bios` and tools/probes/biosboot.
- reference/Furbtendulator-main/: the full Furbtendulator source (from
  Furbtendulator-main.zip); tools/furb_cli builds it headless.
- testroms/: Add 'em Up, Push the Ball, Scramble, Table Soccer (VT03), Time
  Pilot; supplied later: Aero Gyrodine, Hex City X, VG Pocket VT09, Lucky Lawn
  Mower VT369, Table Soccer VT369. Star Ally, Lonely Island and LLM VT09 are
  NOT on disk; ask.

In git:
- DATASHEET_DIGEST*.md: VT02/VT03 datasheet notes. Bit numbering there is
  1-INDEXED; the code is 0-indexed. DATASHEET_DIGEST.md appendices A/B hold the
  spec-vs-code verdicts (no VT extra opcodes; NMI polarity; $410F).
- MAINTAINERS_GUIDE.md (s.74 maps the retired docs and their open items),
  CHANGELOG.md (0.1-0.5.2 only), README.md.
- Reference screenshots supplied (NOT in the tree; ask for them):
  gg.png / 1.png (LLM opening), lawn.png (LLM gameplay), vgp.png / vgp2.png
  (VG menus), aero.png, hex.png, add.png, add2.png, bug.png, shot.png.
