# Findings: the "VT extra opcodes" are not VT03/VT09 CPU instructions

Recorded after reading the primary VRT datasheets
(`VT02 Data Sheet RevisionA5_ENG`, `VT03 Data Sheet RevisionA6_ENG`).
This corrects a false premise that runs through the README,
IMPLEMENTATION_NOTES, config.h, and `6502_vt.s`.

## What the datasheets actually say

Both datasheets carry a full CPU instruction table. VT02 prints the
opcode matrix on its page 47; VT03 prints it on page 48 (revision A6,
dated Oct 13 2008). The two matrices are identical and are the **stock
NMOS 6502 set**. Every cell that the project treats as a "VT extra"
opcode is blank in the official table:

    0x3C  (PLX)    -- blank
    0x34  (PLY)    -- blank
    0x5A  (TAD)    -- blank
    0x7A  (TDA)    -- blank
    0xC2  (PHX)    -- blank
    0xD2  (PHY)    -- blank
    0x62  (ADX)    -- blank
    0xA7  (LDAXD)  -- blank
    0xBF  (LDAD)   -- blank

There is no PHX/PLX/PHY/PLY/TAD/TDA/ADX/LDAXD/LDAD on the VT02 or VT03
main CPU. The VT02/VT03 6502 is plain.

## Where the opcodes really come from

Furbtendulator defines all of these only in `OneBus_VT369.cpp`, inside
the class `CPU_VT369_Sound` -- the **audio coprocessor** 6502 of the
VT369 / VT32 / VT168 generation, not the main CPU and not VT03. They
are dispatched only when `reg4100[0x62] == 0x0D`, i.e. when that second
CPU core is the one fetching.

`LDAXD` (0xA7) and `LDAD` (0xBF) both do the same thing on that core:

    A = PRGROMData[((dataBank << 16 | calc_addr) + vt369relative)
                   & (PRGROMSize - 1)];
    set N, Z from A

That is a far-PRG streaming read: a 24-bit address built from a data
bank register (set via TAD) plus a fixed relative base, masked to the
PRG image size. Its purpose on real hardware is to pull sample data out
of PRG ROM to feed the ADPCM sound channels.

## Why this matters for PocketVT specifically

1. **VT03 is the named target of this emulator, and VT03 has none of
   these opcodes.** Patching them into the main dispatch table in
   `vt_patch_optable` models instructions the target chip does not have.

2. **PocketVT does not emulate the VT369 sound coprocessor**, and on the
   GBA it does not need to: the sample streaming that `LDAXD`/`LDAD`
   perform on hardware is handled here by the ADPCM mixer
   (`vt_adpcm_mix_gba`) reading PRG directly. There is no second CPU in
   this port for these instructions to belong to.

3. **The two test ROMs are not blocked on these opcodes.**
   `Lonely Island.nes` and `Star Ally (VT03).nes` are both
   mapper 256 / submapper 15 (Jungletac). Their reset code decodes to
   garbage even after a flat XOR-0xA1 over fetched bytes, which means
   the real opcode-encryption gate is a bit-permutation-plus-XOR, not
   the flat XOR-0xA1 the README describes. The blocker is the encryption
   model (and, for any genuinely sound-CPU code, the absence of that
   second core) -- not a missing main-CPU `0xA7`/`0xBF` handler.

## Consequence / recommendation

- Do **not** wire `LDAXD`/`LDAD` into the main 6502. The stub lines in
  `vt_patch_optable` stay commented out. (If a main-CPU stream ever did
  reach `0xA7`/`0xBF`, the only defensible stock-6502 behaviour is
  `LAX` -- load A and X from the addressed byte -- but the datasheet
  says VT02/VT03 never emit those bytes in the first place.)

- The already-implemented TAD/TDA/PHX/PHY/ADX handlers are, by the same
  evidence, not VT02/VT03 instructions either. They are harmless on
  carts that never execute those bytes, but the project's framing of
  them as "VT03/VT09 extra opcodes" is incorrect; they are VT369-era
  sound-CPU opcodes. Leaving the handlers in place is fine for now, but
  the documentation should stop attributing them to VT03.

- To actually boot the sub-15 test ROMs, the real work is the
  Jungletac/Cube-Tech opcode-encryption permutation, not opcodes.
