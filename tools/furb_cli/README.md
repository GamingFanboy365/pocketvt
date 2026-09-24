# furb_cli: Furbtendulator as a headless reference

`furb_cli` is Furbtendulator (NewRisingSun's NintendulatorNRS fork, the
authority on VT/OneBus behaviour) built as a 32-bit Linux command-line
program. It runs a `.nes` for N frames with a scripted pad and dumps the
frames you ask for, so a PocketVT build can be checked against the reference
without anyone taking screenshots. `tools/compare_furb.py` does the whole
comparison in one command.

The Furbtendulator source is **not** in git. Unzip `Furbtendulator-main.zip`
into `reference/` (gitignored) so that `reference/Furbtendulator-main/src`
exists. Nothing in that tree is modified: `build.py` copies it into
`tools/furb_cli/build/src`, and only the copy is adjusted.

## Build

```
apt install g++-multilib libmgba-dev      # 32-bit g++ (the code assumes Win32's 32-bit long)
pip install numpy pillow                  # for compare_furb.py
python3 tools/furb_cli/build.py           # ~2 min first time, incremental after
```

Output: `tools/furb_cli/build/furb_cli` and `tools/furb_cli/build/Mappers/iNES.so`.
Both link the C++ runtime statically, so they need only 32-bit glibc to run
(`libc6-i386`). Build on an old distro (e.g. `docker run ubuntu:20.04`) for a
binary that runs on older systems too; building there needs glibc 2.29 or newer.
The same `build.py` works standalone: put the Furbtendulator `src` directory
next to it as `Furbtendulator-src/` (that is how the archived release package
is laid out) or pass `--furb`.
The source lists come from Furbtendulator's own Visual Studio projects, so a
newer Furbtendulator drops in without editing the build.

## Compare a ROM against PocketVT

```
python3 tools/compare_furb.py testroms/Scramble.nes --at 300,700 --input "320-325:Start"
```

This packs the ROM onto the core with `builder.py` (default: the committed
repo-root `pocketvt.gba` + `pocketvt.elf`, or pass `--core`/`--elf` for a
`build_pvt.sh` build), runs it in mGBA and in `furb_cli` with the same input,
and writes `furbcmp/cmp_tNNNN.png` (reference mapped to the GBA screen |
PocketVT | mismatches in red), the full reference frame, and `report.json`.

Input and frame numbers are **NES frames on both sides**. PocketVT's own
emulated-frame counter (`frametotal`) keys its input and captures, so a cart
PocketVT runs below full speed still gets Start on the same NES frame. Each
PocketVT capture is matched against the reference frames within `--window`
(default 8; PocketVT's screen can trail its counter by a few frames when it
runs slowly), through PocketVT's per-line row table (`dma0buff`: scale75's
3-in-4 decimation, raster splits handled per segment) and the 8-column crop.

`struct` is palette-independent: how consistently each reference colour maps
to one PocketVT colour and back. 100% means the same picture in any palette;
wrong tiles, missing sprites or bad scroll lower it. The mismatch lines
list the colour pairs that disagree: black against a colour is usually
positional (a row or a sprite in a different place), colour against colour
points at a palette difference. `exact5` compares 5-bit RGB and is only
meaningful where the palettes are meant to agree; Furbtendulator generates
its own NTSC palette while PocketVT uses calibrated console DACs.
`--set VT03Palette=N` (and other settings, see `furb_cli` with no arguments)
changes the reference side.

The two emulators can diverge in game state over long runs (a ship that
dies in one and not the other). The side-by-side image makes that obvious;
compare early frames or re-script the input rather than trusting a late
score.

## furb_cli on its own

```
furb_cli ROM.nes --frames 900 --dump 300,899 --out shots/scr \
         --input "320-325:Start;500-900:Right" [--port 2] [--hashes] [--set VT03Palette=1]
```

For each dumped frame it writes `PREFIX_fNNNN.ppm` (exactly the region
Furbtendulator's own screenshot saves), `.idx` (the PPU's raw uint16 palette
indices), `.txt` (`$2000-$20FF`, `$4100-$41FF` and palette RAM) and `.ram`
(CPU RAM). `--hashes` prints one hash per frame, for frame-set tests. Input
items are `FIRST-LAST:BUTTONS` with buttons `A B Select Start Up Down Left
Right` joined by `+`, optionally prefixed `p2:` for the second pad (some
famiclone games read player 2). It is deterministic: the same arguments give
the same frames.

## How the port works

`compat/` is a small Win32/DirectX shim. Types and macros are real; every
GUI, DirectDraw, DirectSound, DirectInput, registry and dialog call is an
inert stub, so the emulator runs with no window, no sound, and every setting
at its compiled-in default. The iNES mapper pack is built as `Mappers/iNES.so`
and loaded through the shimmed `LoadLibrary`/`GetProcAddress` (`dlopen`), just
as the Windows build loads `iNES.dll`. That also keeps the main program's and
the pack's same-named globals apart. `furb_cli.cpp` replaces `WinMain` and
runs the same CPU loop as `NES::Thread`. It owns the keyboard state that the
standard controller reads, so no DirectInput device is needed. `prep_src.py`
fixes `#include` case and backslashes for Linux, and patches the copy for
three MSVC-only constructs g++ rejects (listed in the file; each patch must
match exactly once, so an upstream change fails loudly instead of silently).
