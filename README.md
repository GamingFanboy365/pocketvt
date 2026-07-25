# PocketVT

A Game Boy Advance emulator for **VT03** and **VT09** "NES-on-a-chip"
hardware, forked from PocketNES.

VT03 (VT369) and VT09 (VT32 / VT168) are chips produced by VRT/UMC and
used in dozens of Famiclone plug-and-play systems from the early 2000s
onward.  They are backwards-compatible supersets of the NES 2A03 but add:

- A `$4100-$41FF` extended register block (PRG/CHR bank select, IRQ timer,
  ALU helper, ADPCM, palette RAM, hi-res OAM extension).
- Extended PPU registers at `$2010-$201F` and `$2100-$21FF`.
- An extended palette (512 entries, 6-bit colour) usable as either a
  64-colour or 256-colour mode.
- Hi-res 16x8 pixel sprites via an OAM extension table.
- Extra 6502 opcodes: `PHX/PLX/PHY/PLY/TAD/TDA/ADX` (and on some chips
  `LDAXD/LDAD`).
- IMA-style ADPCM sample channels.
- On VT09 / mapper 256 submapper 13-15: a CPU-fetch XOR-0xA1 opcode
  encryption gate, toggled by `$410F` writes and committed on the next
  `JMP/JMPI`.

## Building

You need DevkitARM.  Set `DEVKITARM` in your environment, then:

```bash
make
```

This produces `pocketvt.gba`, which supports **all** VT variants.
The VT register block, ADPCM mixer, enhanced palette, hi-res sprite
OAM extension, the extra 6502 opcodes, AND the VT09 encryption gate
are all compiled into one binary.  The chip-specific behaviour is
selected at runtime from the loaded ROM's iNES 2.0 mapper/submapper:

- **Mapper 256** -> VT03 OneBus.  Submapper bits 13-15 are
  "Cube Tech / Karaoto / Jungletac" and enable opcode encryption
  from power-on; other submappers run plain.
- **Mapper 405** -> VT168 / Shenzhen Subor (currently partial: bank
  mapping works but zero-vector boot ROMs like *Space_War* do not yet
  boot -- see *Known Issues* below).

The Makefile flag `VT=VT09` is accepted for backward compatibility but
no longer changes the build.

The old "plain NES compatibility build" is gone.  For non-VT NES use the
upstream PocketNES project; PocketVT is dedicated to VT-series hardware.

## Packaging ROMs

Use PocketNES Menu Maker to pack `.nes` files into the `.gba` binary,
exactly as with PocketNES.  The loader (`loadcart.c`) detects NES 2.0
mappers 256 and 405 and re-tags them internally as mapper 253
(`mapVTinit`), so you do **not** need to rewrite the iNES header.

## Test ROMs

The test ROMs are no longer bundled (use your own iNES dumps).  Confirmed-
boot status of the test corpus this fork was developed against:

| ROM                                | Mapper / Sub | Status |
|------------------------------------|--------------|--------|
| `Jewel_Quest.nes`                  | 256 / 0      | should boot |
| `Rubble_the_Engineer.nes`          | 256 / 0      | should boot |
| `Lucky_Lawn_Mower__VT369_.nes`     | 256 / 0      | should boot |
| `Lucky_Lawn_Mower__VT09_.nes`      | 256 / 0      | should boot |
| `Lonely_Island.nes`                | 256 / 15     | blocked on opcode-encryption gate (NOT opcodes -- see FINDINGS_vt_opcodes.md) |
| `Star_Ally__VT03_.nes`             | 256 / 15     | blocked on opcode-encryption gate (NOT opcodes -- see FINDINGS_vt_opcodes.md) |
| `Space_War.nes`                    | 405 / 0      | needs mapper 405 boot stub |

The submapper-0 ROMs use plain 6502 with no extra opcodes and no
encryption, so they exercise the full PocketVT path (writemem_4 hook,
$4107-$410A PRG bank switching, $2012-$2017 CHR bank switching) without
needing every VT extension to be implemented.  These are the right
ROMs to test against first.

## What lives in `src/Mappers/`

PocketVT inherits PocketNES's full mapper library (61 `.s` files for
NES mappers 0-249).  PocketVT itself only needs one: `mapVT.s` (mapper
253 internally, mapper 256/405 in the iNES 2.0 header).  Everything
else is dead code from PocketVT's perspective; the dispatch table in
`cart.s` is what decides whether a given mapper number ever gets
invoked, and only `mapVTinit` is wired into the iNES-256-and-405 paths
in `loadcart.c`.

If you want to slim the build, you can delete the unused files in
`src/Mappers/` and remove the corresponding entries from `mappertbl`
and `mappertbl2` in `cart.s`.  The shared write/CHR helper labels in
each mapper file (`write0`, `write11`, `write0_206`, etc.) are file-
local, so removing the file deletes them.  Be careful with the
multi-mapper files like `map023771.s` (mappers 0, 2, 3, 7, 71 share
init code) and `map70152.s` -- you have to keep the whole file as
long as any of its mappers are referenced.  The linker doesn't drop
unused functions for you here because the mapper table holds explicit
function-pointer references.

For most users it's easiest to leave them alone -- they add roughly
80 KB to the ELF, which the GBA cartridge format will swallow without
complaint.

## Status

| Subsystem                                        | Status |
|--------------------------------------------------|--------|
| `$4100-$41FF` register map + handlers            | done   |
| VT extra opcodes (PHX/PLX/PHY/PLY/TAD/TDA/ADX)   | done   |
| VT09 opcode-encryption gate + JMP commit         | done   |
| Runtime submapper-driven encryption selection    | done   |
| ADPCM: rate counter, end-of-sample detection     | done   |
| ADPCM: GBA DirectSound mixer hook                | done   |
| Hardware ALU (`$4132-$4139`)                     | done   |
| Extended palette storage + BGR555 conversion     | done   |
| Palette rebuild on VBlank                        | done   |
| OAM extension storage                            | done   |
| Mapper 253 registered in `cart.s`                | done   |
| PPU `$2008+` extended-register divert, gated on VT cart | done |
| Static asserts on `VTState` encryption offsets   | done   |
| 16x8 sprite rendering path                       | TODO   |
| `LDAXD` / `LDAD` opcodes                         | not applicable to main CPU (VT369 sound-coprocessor only; see FINDINGS) |
| Mapper 256 submapper byte-mangling tables        | TODO (identity table is correct for submappers 0 and 15, which is everything in the test set) |
| Mapper 405 zero-vector boot stub                 | TODO   |
| VT369-specific ADPCM step table (16x16 + 4 modes)| TODO (currently uses standard IMA placeholder) |

## Known Issues

**Mapper 405 ROMs do not yet boot.**  ROMs that target mapper 405
(VT168 / Shenzhen Subor variants) have all-zero NMI/Reset/IRQ vectors
in their iNES file -- the real chip has an internal boot ROM that reads
a jump table at PRG offset `$10` and dispatches to the first `$80xx`
entry point.  PocketVT currently routes mapper 405 through the standard
`mapVTinit` (last-bank reset-vector anchor), which leaves the CPU
jumping to `$0000` and crashing.  The marker bit for "this ROM is
mapper 405" is plumbed through to `vt.submapper` bit 7, so the bootstrap
just needs to be written in `mapVT.s` / a new `mapVT405init`.  See
`IMPLEMENTATION_NOTES.md` for the entry-point table format observed in
`Space_War.nes`.

**ADPCM algorithm is generic IMA, not VT369-specific.**  The current
decoder uses the standard 89-step IMA-ADPCM table.  Real VT369 hardware
uses a 16x16 step table with four prediction modes selected by lead-byte
bits `[5:4]`, as implemented in Furbtendulator's `ADPCM_VT369.cpp`.
Sample playback rates and pitches will sound close-enough for testing
but not bit-exact.  Replacing the decoder is straightforward once
verified against a few sample ROMs.

## Credits

PocketVT is based on **PocketNES** by Loopy et al.

VT chip research reference implementation: **Furbtendulator** (a
Nintendulator fork by NewRisingSun et al., with VT369/VT32/VT168
support in `src-main/OneBus_VT*` and `src-main/ADPCM_VT*`).

OneBus mapper submapper tables (CPU/PPU/MMC3 byte mangling per
Waixing/Trump Grand/Zechess/Qishenglong/Jungletac variants) are from
the OneBus reference code (`mapper256.cpp`).
