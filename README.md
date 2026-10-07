# PocketVT

A Game Boy Advance emulator for VT02 / VT03 / VT09 "OneBus" famiclone
hardware (NES-on-a-chip plug-and-play systems), forked from PocketNES.

VT chips are supersets of the NES: 16-colour (4bpp) tiles, extended CHR/PRG
banking through `$2010-$201A` and `$4100-$41FF`, an enhanced palette,
extension addressing for background and sprite tiles, video DMA, and on
some carts a per-submapper opcode bit-permutation ("encryption"). The main
CPU is a stock 6502. The so-called "VT extra opcodes" belong to the VT369
sound coprocessor and are not emulated; see DATASHEET_DIGEST.md appendix A.

Carts are recognised from their NES 2.0 header: mappers 256, 405 and 419 are
routed to the VT core (internally mapper 253, `mapVTinit`), and the
submapper selects encryption and register mangling at runtime. The header's
extended console type (VT03, VT09, VT32, VT369) switches on console-specific
hardware such as the VT32/VT369 multiply/divide unit, and a header nibble can
name the console's colour DAC. Other mappers still go through the inherited
PocketNES mapper library.

## Status

Every figure below comes from `tools/compare_furb.py`, which runs the same ROM
with the same input through PocketVT and through Furbtendulator (the reference
emulator) and scores the pictures frame by frame. "Picture" is the
palette-independent structural match; speed is NES frames emulated per 60 GBA
frames.

| Cart | State |
|------|-------|
| Star Ally, Lonely Island, Scramble | working (regression controls); 97-99% picture |
| Jewel Master VT03 | working (Jungletac opcode encryption, 4bpp background with BKEXTEN, 16-colour sprites); title 99.9%, gameplay 98-99% picture |
| Zuma (VT369) | working (Cube Tech opcode encryption, enhanced picture, sound CPU); title 99.6%, gameplay 98% picture (ball colours are random), 60 fps. On carts that run at the slow timing, the top row of balls no longer vanishes |
| Jumper (VT369, a Super Mario Bros. hack) | working (CPU x3, 8bpp sprites, sprite-0 split, opcode encryption toggled four times a frame); 99.9% picture, 43-60 NES fps (lowest in the death sequence) |
| Sky Fighter (VT369) | working (CPU x3, 16x16 sprites, 8bpp scrolling background); title 100%, gameplay 96-98% picture (enemy waves drift from the reference), 60 NES fps |
| Lucky Lawn Mower (VT09) | working; 99.7% picture, colours match the reference exactly |
| VG Pocket 50-in-1 (VT09) | all 50 games run; mean exact-colour match 98.3% across all 50 |
| Push the Ball, Time Pilot | working; 97-100% picture |
| Add 'em Up | working, full speed; 98-99.9% picture |
| Aero Gyrodine, Hex City X | working; raster-split titles 99.99%, colours exactly the reference's, full speed (60 NES fps; 42-43 before the idle-loop speed hack of guide s.92) |
| Funny Coins (VT369) | working; no longer crashes a few seconds into play (a stray sound interrupt); title 100%, gameplay 98% picture to NES frame 1800, 60 NES fps. One-line streaks remain at the edges of the board's raster splits |
| Table Soccer (VT03, mapper 419) | working; menus and match 96-99%, 60 NES fps; no voice samples |
| Soccer 2009 (VT03, CHR ROM) | working; menus 99.5%, matches 97-99% picture with the reference's colours; menus 60 NES fps, matches 42-44 (the game uses every CPU cycle of the frame, so nothing can be skipped) |
| Table Soccer VT369 | working, with its streamed music; team select 99.9%, match 96.7%, 60 NES fps |
| Lucky Lawn Mower VT369 | working (VT369 enhanced renderer, 4bpp); 99.7% picture, full speed |
| Fire Fighter VT369, Jewel Master VT369 | working (enhanced renderer, 8bpp) with sound effects; Jewel Master 99.9% picture, Fire Fighter 96% (game-state drift). Both run the CPU at x3 as the hardware does; Jewel Master 60 NES fps, Fire Fighter 54-60 in gameplay with its game logic updating every frame (at 1x it updated on 70% of frames); Fire Fighter's HUD digits no longer drop out (too many sprites on a GBA line) |
| Mapper 405 (VT168, zero-vector boot) | does not boot yet |

Colours: games that use the VT03's 12-bit colour mode (COLCOMP) are drawn with
the reference emulator's NTSC colour model since this release, so Aero
Gyrodine, Hex City X, Add 'em Up, Table Soccer VT03 and Soccer 2009 match it
exactly where they used to be close (their skies, pitches and logos shift hue
compared with earlier builds). The 64 standard colours are unchanged.

Raster splits (a game switching CHR banks partway down the screen from a
timer IRQ) are drawn by giving each band its own GBA character block, up to
three extra bands per frame, in both 4bpp and 2bpp modes. The VT timer is
modelled after the reference's scanline counter, so split lines land on the
same scanlines as in Furbtendulator.

Sound: the standard 2A03 channels play, and match the reference where a game
runs at full speed. The VT-specific PCM hardware is incomplete. The VT02+
ADPCM channels (`$4120-$412F`) are only mixed while NES DMC audio happens to
be playing, the Table Soccer TK-8007 voice chip is answered but not played,
and the second APU (`$4020-$402F`) is not emulated. The VT369 sound CPU is
emulated at a high level, as Furbtendulator does it by default: Lucky Lawn
Mower, Fire Fighter and Jewel Master play their ADPCM effects, and Table
Soccer VT369 plays its streamed music.

Open work, in priority order, is listed in CLAUDE.md. The detailed record
is MAINTAINERS_GUIDE.md.

## Real hardware

Cores built either way get a valid GBA header (Nintendo logo and complement
check), which a real GBA's BIOS requires before it will start a cart; the
Docker build runs devkitARM's `gbafix`, and `build_pvt.sh` runs
`tools/gbafix.py`, which copies the logo from the committed core. `builder.py`
warns if a play ROM's header would not boot. Every test ROM has been booted
through a real GBA BIOS dump in mGBA (`compare_furb.py --bios`), including the
BIOS intro and header check, and renders the same as under mGBA's built-in
BIOS. None of this has been tried on a physical GBA yet.

### Cartridge speed, flash carts and reproduction carts

Since September 28 the core runs the cartridge at the faster timing retail
games use (3/1 wait states with the prefetch buffer, `WAITCNT = 0x4317`),
which is worth up to 70% speed on some carts. Some flash carts and
reproduction carts use memory too slow for that timing, and on real
hardware with such a cart those builds can fail to boot or crash. This is
the most likely reason a September 24 build (which left the GBA's slower
power-on timing in place) was reported to boot on a flash cart where a
later one did not; it has not been confirmed on hardware.

Since October 5 the core checks this itself at start-up: it reads its own
image at the power-on timing, switches to the faster one, reads it again
four times, and keeps the slow timing if anything differs. Holding SELECT
while the GBA starts skips the check and keeps the slow timing, for a cart
that passes the check but still misbehaves. The slow timing is the same as
the September 24 builds and plays the same pictures, but slower (in mGBA:
Time Pilot 45 instead of 59 NES fps, Star Ally 49-51 instead of 60, Scramble
53-56 instead of 60). Building with `VT_FAST_WAITCNT 0` in `src/config.h`
never tries the fast timing. The check costs about a quarter of a second at
start-up.

The ROM also carries the `SRAM_V` save-type marker, so flash carts that pick
a save type from the ROM give it the SRAM it keeps settings and saves in.
Without one the core still starts (it re-initialises a blank or foreign save
area), but settings and saves are not kept.

## Building

With Docker (no local toolchain needed), from the repository root:

```bash
sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make
```

This produces `pocketvt.gba` (the core, no games) and `pocketvt.elf` in the
repository root. Both are committed. On every pull request, the "Build core"
GitHub Action (`.github/workflows/build.yml`) rebuilds them with this same
image and commits them back to the PR branch if they changed, so the
committed binaries always match the source.

Without devkitPro, `build_pvt.sh` builds with plain `gcc-arm-none-eabi`
plus the libgba headers (clone https://github.com/devkitPro/libgba):

```bash
LIBGBA=/path/to/libgba bash build_pvt.sh     # -> ../pvt_build/pocketvt.gba
BUILD=/some/dir EXTRA_CFLAGS=-DFOO bash build_pvt.sh   # variant build
tools/restage.sh                             # ALWAYS run after build_pvt.sh
```

`build_pvt.sh` deletes its build directory first, including any `.nes`
files and `builder.py` you put there. `tools/restage.sh` copies them back
(from `testroms/` and from `$PVT_ROMS`). The two build paths use different
GCC versions, so compare cores only within one path.

## Packaging games

Run `builder.py` in the directory that holds `pocketvt.gba`:

```bash
python3 builder.py game1.nes [game2.nes ...]   # -> play_me.gba
```

One game boots straight in; two or more give the PocketNES ROM menu. The
script exits with an error if it injected no ROMs. A working single-game
file is always larger than the ~107 KB core.

## Local test material (not in git)

No ROMs are ever committed: not test carts, not packaged play ROMs. The only
ROM-like files in git are the core itself, `pocketvt.gba` and `pocketvt.elf`.
`.gitignore` enforces this for `*.nes`, `*.fds`, `*.unf` and the other NES
formats, every `*.gba` except the core, `testroms/` and `reference/`.

`testroms/` (ROMs) and `reference/` (NESdev wiki XML exports, the
NintendulatorNRS OneBus sources in `reference/nrs/`, the full Furbtendulator
source in `reference/Furbtendulator-main/`, and a GBA BIOS dump for real-BIOS
testing) live in the working tree but are gitignored, as are captures
(`*.png`) and build output. Keep them locally and never commit them.

## Checking against the reference emulator

`tools/furb_cli` builds Furbtendulator as a headless Linux program, and
`tools/compare_furb.py` runs a ROM through it and through PocketVT with the
same input, then writes side-by-side images and scores. See
`tools/furb_cli/README.md`.

```bash
python3 tools/furb_cli/build.py
python3 tools/compare_furb.py testroms/Scramble.nes --at 300,700 --input "320-325:Start"
python3 tools/compare_furb.py testroms/Scramble.nes --at 300 --bios reference/gba_bios.bin
```

`tools/probes/` holds small single-question mGBA harnesses: speed, frame
hashes, memory dumps, watchpoints, stack depth, a cycle-weighted profiler,
real-BIOS boot (`biosboot`) and audio capture (`pvtwav`, compared against
`furb_cli --wav` by `wavcmp.py`). Its README lists them.

## Repository layout

| Path | What |
|------|------|
| `src/` | the emulator: PocketNES core (ARM asm + C) plus the VT additions |
| `src/vt_regs.c`, `src/ppu_vt.c`, `src/Mappers/mapVT.s`, `src/6502_vt.s` | VT registers, VT video, VT mapper hooks, encryption wrappers |
| `tools/` | test harnesses (libmgba), scoring and helper scripts, `gbafix.py` |
| `tools/probes/` | single-question mGBA probes (see its README) |
| `tools/furb_cli/`, `tools/compare_furb.py` | headless Furbtendulator (reference emulator) and a one-command frame comparison against PocketVT |
| `build_pvt.sh`, `Makefile` | toolchain-only build, devkitARM build |
| `builder.py` | packs `.nes` files onto the core |

## Documentation

| File | What |
|------|------|
| CLAUDE.md | handoff: build, regression procedure, current state, open work |
| MAINTAINERS_GUIDE.md | the full engineering record; numbered sections, newest last |
| DATASHEET_DIGEST.md | VT03 datasheet digest, plus spec-vs-code verdicts (appendices) |
| DATASHEET_DIGEST_VT02.md | VT02 datasheet digest and VT02-vs-VT03 deltas |
| CHANGELOG.md | the early releases, 0.1.0 to 0.5.2 |

## Heritage

PocketVT is built on the maintainer's PocketNES fork, which is based on
https://github.com/catskull/pocketnes, a mirror of the last PocketNES
release (9.98, July 2013) taken from the archive.org copy of
nes.pocketheaven.com. That fork added:

- a Makefile and linker scripts that build under modern devkitARM (Docker
  image `devkitpro/devkitarm`)
- fixes for strict C99/C23 pointer errors and inline-asm clobber lists
- a NES 2.0 header parser in `loadcart.c` (extended ROM sizes,
  NTSC/PAL/Dendy timing), replacing the old DiskDude hack
- the Python `builder.py` multicart packer
- mappers 30 (UNROM 512), 38 (Crime Busters), 41 (Caltron / Myriad 6-in-1),
  89 (Sunsoft-2 IC02), 113 (HES NTD-8), 146 (via mapper 79), 185 (via
  CNROM), 11 (Color Dreams / Wisdom Tree), and partial 225 (110-in-1: the
  menu boots, some games are garbled) and 28 (Action 53: the menu boots,
  games crash)

The maintainer builds this project with AI assistance.

## Credits

PocketNES by Loopy et al. (later maintained by FluBBa and Dwedit). VT hardware behaviour
comes from the VRT VT02/VT03 datasheets, the NESdev wiki VT02+ pages, and
NewRisingSun's NintendulatorNRS / Furbtendulator OneBus implementation.
