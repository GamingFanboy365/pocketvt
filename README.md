# PocketVT

A Game Boy Advance emulator for VT02 / VT03 / VT09 "OneBus" famiclone
hardware (NES-on-a-chip plug-and-play systems), forked from PocketNES.

VT chips are supersets of the NES: 16-colour (4bpp) tiles, extended CHR/PRG
banking through `$2010-$201A` and `$4100-$41FF`, an enhanced palette,
extension addressing for background and sprite tiles, video DMA, and on
some carts a per-submapper opcode bit-permutation ("encryption"). The main
CPU is a stock 6502. The so-called "VT extra opcodes" belong to the VT369
sound coprocessor and are not emulated; see DATASHEET_DIGEST.md appendix A.

Carts are recognised from their NES 2.0 header: mappers 256 and 405 are
routed to the VT core (internally mapper 253, `mapVTinit`), and the
submapper selects encryption and register mangling at runtime. Other
mappers still go through the inherited PocketNES mapper library.

## Status

| Cart | State |
|------|-------|
| Star Ally, Lonely Island, Scramble | working (regression controls) |
| Lucky Lawn Mower (VT09) | working, ~96.5% / 97.1% pixel match vs reference |
| VG Pocket 50-in-1 (VT09) | all 50 games run |
| Push the Ball, Time Pilot | working |
| Add 'em Up | working, full speed |
| Aero Gyrodine, Hex City X | gameplay works; titles are raster-split and slow |
| Table Soccer (mapper 419) | not supported |
| Mapper 405 (VT168, zero-vector boot) | does not boot yet |

Open work, in priority order, is listed in CLAUDE.md. The detailed record
is MAINTAINERS_GUIDE.md.

## Building

With Docker (no local toolchain needed), from the repository root:

```bash
sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make
```

This produces `pocketvt.gba` (the core, no games) in the repository root.

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

`testroms/` (ROMs) and `reference/` (NESdev wiki XML exports and the
NintendulatorNRS OneBus sources in `reference/nrs/`) live in the working
tree but are gitignored, as are all `*.nes`, captures (`*.png`) and build
output. Keep them locally and never commit them.

## Repository layout

| Path | What |
|------|------|
| `src/` | the emulator: PocketNES core (ARM asm + C) plus the VT additions |
| `src/vt_regs.c`, `src/ppu_vt.c`, `src/Mappers/mapVT.s`, `src/6502_vt.s` | VT registers, VT video, VT mapper hooks, encryption wrappers |
| `tools/` | test harnesses (libmgba), scoring and helper scripts |
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
