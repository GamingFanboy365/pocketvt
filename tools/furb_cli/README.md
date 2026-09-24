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

Output: `tools/furb_cli/build/furb_cli` and `tools/furb_cli/build/Mappers/{iNES,FDS,NSF,VS}.so`,
plus Furbtendulator's data files (see Data files below).
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
furb_cli ROM --frames 900 --dump 300,899 --out shots/scr \
         --input "320-325:Start;500-900:Right" [--port 2] [--hashes] [--set VT03Palette=1]
```

ROM can be anything Furbtendulator opens: `.nes` (iNES/NES 2.0), `.unf`,
`.fds`/`.qd`, `.nsf`/`.nsfe`, Vs. System dumps. Run `furb_cli` with no
arguments for the option list.

### Frames

For each dumped frame it writes `PREFIX_fNNNN.ppm` (exactly the region
Furbtendulator's own screenshot saves), `.idx` (the PPU's raw uint16 palette
indices), `.txt` (`$2000-$20FF`, `$4100-$41FF` and palette RAM; the full
1024-entry palette on VT369) and `.ram` (CPU RAM). On VT369 in hi-res mode
(`$201C` bit 2) the `.idx` interleaves the even/odd half-pixel arrays, as the
GUI does. `--every K` dumps every Kth frame, `--hashes` prints one hash per
frame for frame-set tests, `--info` prints the ROM, its DIP switch and cheat
definitions and exits. Frames count from power-on starting at 0. The run is
deterministic: the same arguments give the same frames.

### Input script

`--input` takes `FRAMES:ACTION` items separated by `;` (the option can be
repeated). FRAMES is `N` or `FIRST-LAST`. Actions:

| Action | Meaning |
|--------|---------|
| `A+B+Select+Start+Up+Down+Left+Right+TurboA+TurboB`, `bN` | pad buttons (default pad: `--port`) |
| `p1:` .. `p4:`, `exp:` prefix | send them to that pad (Four Score / 4-player adapters: p1..p4) or the expansion device |
| `trigger` | Zapper and other light guns (button 0) |
| `key:NAME+NAME` | keyboard keys by DirectInput name (`a`, `return`, `space`, `f1`, `lshift`, ...) for Family BASIC, Subor and the other keyboards |
| `mouse:X,Y[+left][+right][+middle]` | cursor in NES pixels, for the Zapper aim, mice, Arkanoid paddle and tablet |
| `mic:LEVEL` | karaoke microphone peak level 0..1 |
| `cmd:reset`, `cmd:hardreset` | soft / hard reset |
| `cmd:coin1`, `cmd:coin2` | Vs. System coin slots |
| `cmd:button` | the plug-through device's button (the GUI's "press button" menu item) |
| `cmd:mic` | Famicom controller-2 microphone |
| `cmd:fds-insert`, `fds-eject`, `fds-next`, `fds-prev` | disk side changes |
| `cmd:save=FILE`, `cmd:load=FILE` | savestate to / from a file |
| `cmd:dip=VALUE` | set the DIP switches (hard-resets like the GUI) |
| `cmd:nsf-song=N` | select song N in the NSF player and press Play |
| `cmd:tape-play=FILE`, `tape-record=FILE`, `tape-stop` | Family BASIC / Subor data recorder |

### Devices

`--device PORT=TYPE` plugs a controller into `port1`, `port2`, `fs1`..`fs4`
or `exp`; `--list-devices` prints the type names (standard, zapper,
arkanoid, powerpad-a/b, fourscore, snes, vs-zapper, snes-mouse, subor-mouse,
and on the expansion port fami4play, hori-4play, family-basic-keyboard,
subor-keyboard, family-trainer-a/b, tablet, hyper shots, turbo-file,
sharp-c1-cassette and more). `port1=fourscore` or `exp=fami4play`/`hori-4play`
connects the extra pads so `p3:`/`p4:` work.

### Settings

Settings go through Furbtendulator's own registry loader, so every GUI
setting is available. `--config FILE` loads a regedit export of
`HKCU\SOFTWARE\Nintendulator` (UTF-16 or UTF-8, as regedit writes it) or a
plain `Name=value` file; `--set Name=Value` sets one value (numbers become
DWORDs, `"text"` becomes a string); `--save-config FILE` writes the settings
in effect at exit as a `.reg` file. `--palette FILE.pal` loads a custom
palette for every region.

`--data-dir DIR` (default `~/.furb_cli`) is the GUI's data folder: battery
saves go to `DIR/SRAM` on exit, as they do in the GUI. `--no-save` writes none.

### Audio, video, states, movies

`--wav FILE` records the sound Furbtendulator's mixer produces (48 kHz,
mono, 16-bit, the samples DirectSound would have played). `--avi FILE`
writes uncompressed video plus that audio at the console's exact frame rate
(NTSC 60.0988, PAL/Dendy 50.007). `--load-state FILE` starts from a
savestate; `cmd:save=` and `cmd:load=` save and load mid-run.
`--movie-record FILE.nmv` and `--movie-play FILE.nmv` drive the movie
dialogs, so the files are the GUI's own format.

### Game options, cheats, debugging

`--dip VALUE` sets the DIP switches (see `--info` for the definitions from
`dip.cfg`). `--cheat NAME` enables every cheat from `cheats.cfg` whose name
contains NAME (`all` for all). `--header BYTE=VALUE`, `mapper=N` or
`submapper=N` patch a copy of the iNES header before loading (the original
is untouched). `--trace FILE` writes the debugger's CPU trace log, for
`--trace-frames A-B` only if given.

### NSF

Furbtendulator's NSF player loads a tune and then waits for Play, in the GUI
too. `--nsf-song N` presses Play on song N at frame 0; `cmd:nsf-song=N`
switches song later.

### Data files

`build.py` copies `*.cfg` (`cheats.cfg`, `dip.cfg`, `fastload.cfg`),
`BIOS/` and `samples/` from the Furbtendulator release next to the binary,
where the GUI keeps them. Firmware is not included. For FDS put
`DISKSYS.ROM` in `BIOS/`; VT369 carts that need their internal ROM want
`BIOS/VT369-00.BIN` (file names are matched case-insensitively).

### Self-test

```
python3 tools/furb_cli/selftest.py [--rom testroms/Scramble.nes]
```

Builds tiny ROMs, an NSF, a battery ROM and an FDS image on the fly and
checks every feature above: pads, Four Score, both 4-player adapters,
Zapper, microphones, Vs. coins, savestates, config round trip, trace, header
patch, WAV/AVI, NSF songs, battery saves, palette, cheats, DIP switches, FDS
commands. With `--rom` it also checks that a recorded movie and a savestate
continuation replay frame-identically on a real game.

### What is not tested

Real FDS disks (only a dummy BIOS was available), the keyboards, mice,
Arkanoid, tablet and data recorder (the plumbing works, no test ROM uses
them), and the VT369 hi-res dump path (Lucky Lawn Mower VT369 runs but never
enables hi-res). Anything tied to a real window (fullscreen, window size,
the palette editor's and debugger's views) has no meaning headless: the
settings those dialogs save can still be set with `--set` or `--config`.

## How the port works

`compat/` is a Win32/DirectX shim. Types and macros are real. The window,
DirectDraw and DirectInput are inert; the pieces the emulator depends on for
behaviour are implemented: the registry (a map loaded from and saved to
`.reg` files), dialogs (a dialog script registered for a template ID runs the
real dialog procedure against fake controls; unscripted dialogs are
cancelled), file pickers (answered from a queue), DirectSound (captures the
mixer's samples), the cursor and the microphone meter. Mapper packs (iNES,
FDS, NSF, VS) are built as `Mappers/*.so` and loaded through a dlopen-backed
`LoadLibrary`, exactly like the Windows `.dll`s; the executable exports only
`furb_host_lookup`, through which the packs reach the host's shim functions.
`furb_cli.cpp` replaces `WinMain` and runs the same CPU loop as `NES::Thread`.
Pad buttons are mapped to virtual joysticks (DirectInput device 2+port), so
keyboard devices, which read real key codes, never collide with them.
`prep_src.py` fixes `#include` case and backslashes for Linux and patches the
copy for three MSVC-only constructs g++ rejects (listed in the file; each
patch must match exactly once, so an upstream change fails loudly).
