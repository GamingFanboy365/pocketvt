# PocketVT -- Implementation Notes

Last updated after the build-fix + VT03/VT09-correctness pass.

---

## Architectural Decision: VT Support is Always On

The previous build had three modes (`make`, `make VT=VT03`, `make
VT=VT09`) with `make` claiming to be a "Plain NES compatibility build".
That mode never linked because `mapVT.s` and `vt_regs.c` had
inconsistent gates -- the assembly always called `vt_reset` and
`vt_reg_write`, but those symbols only existed when `VT_MODE=1`.

PocketVT is now a VT-only emulator.  `VT_MODE`, `VT_EXTRA_OPCODES`,
`VT_ENHANCED_PALETTE`, `VT_HICOLOR_SPRITES`, `VT_ADPCM_SOUND`, and
`VT09_ENCRYPTION` are all unconditionally `1` in `config.h`.  The
chip-specific behaviour that used to be tied to `-D VT09` is now
selected at runtime from the iNES 2.0 submapper.  This is the right
shape for mapper 256 submappers 13-15 ("Cube Tech / Karaoto /
Jungletac") which need opcode encryption regardless of build flags --
both `Lonely_Island.nes` and `Star_Ally__VT03_.nes` are in this group.

The `VT=VT09` Makefile flag still works for backward compatibility but
no longer changes the build.

## Submapper-Driven Runtime Configuration

`loadcart.c` extracts the NES 2.0 submapper (high nibble of header
byte 8) when remapping mapper 256/405 to internal 253, and writes it
to `vt.submapper`.  Bit 7 of `vt.submapper` is reused as a "this is
mapper 405" marker so `mapVTinit` can branch on it later.

`vt_reset()` (called from `mapVTinit` in assembly) consults
`vt.submapper & 0x0F`; if the result is 13, 14, or 15, it sets
`vt.encryption_active = true` so the CPU starts in the encrypted
state on power-on.

## Runtime PPU Mirror Gate (`vt_active`)

Real NES hardware mirrors PPU `$2000-$2007` across `$2008-$3FFF`.  VT
hardware decodes `$2010-$201F`, `$2100-$21FF` etc. as separate
extended registers.  The previous patch in `ppu.s` diverted *every*
`$2008+` write to `vt_ppu_reg_write`, which broke standard NES
behaviour on any cart that wasn't a VT cart.

Fixed by adding a runtime `u8 vt_active` byte in `vt_regs.c`:

- `cart.s` `loadcart_asm` clears it to 0 before dispatching to any
  mapper init.
- `mapVTinit` sets it to 1, so VT carts route `$2008+` to the extended
  handler.
- Any other mapper init leaves it at 0, so the PPU falls through to
  the standard `and r2,addy,#7` mirror dispatch in `PPU_W_dispatch`.

In the rewritten `PPU_W` in `ppu.s`, the check is two instructions and
a branch; one mispredicted branch per non-VT PPU write on a VT build.

## Static Asserts on `VTState` Layout

`6502_vt.s` hardcodes the byte offsets of the three encryption fields
(`encryption_active = 0x20`, `encryption_pending = 0x21`,
`encryption_next = 0x22`) inside `VTState`.  If a future edit reorders
the struct, the assembly will silently read the wrong bytes.
`vt_regs.c` now contains `_Static_assert`s for all three offsets,
gated on `#if VT09_ENCRYPTION` so they only fire when the assembly
actually depends on them.

## ADPCM Improvements

Two TODOs from the original implementation notes are now closed:

1. **Rate counter.**  `VTAdpcmChan` gained a `u16 rate_counter` that
   decrements once per GBA sample tick.  The decoder fetches a new
   nibble only when it underflows and reloads from `ch->period`.
   Higher `period` -> slower playback.

2. **End-of-sample detection.**  `VTAdpcmChan` gained `u32 end_addr`
   and two new register offsets (`$4126`/`$4127`/`$412E`/`$412F`,
   `VT_ADPCM_END_LO`/`HI`).  When `end_addr > 0` and the current read
   pointer crosses it, the channel sets `playing = false`.  When
   `end_addr == 0` the channel runs forever (the previous behaviour).

The shared advance helper `_adpcm_advance()` is used by both
`vt_adpcm_tick()` and `vt_adpcm_mix_gba()`.

## Open Items

### Mapper 405 zero-vector boot

Space_War (`mapper 405 / sub 0`) has all-zero reset/NMI/IRQ vectors
in its iNES file.  The real VT168 has an internal boot ROM that reads
a jump table at PRG offset `$10` (which becomes `$8010` after bank 0
is mapped to `$8000-$BFFF`) and dispatches to one of eight `$80xx`
entry points.

Observed table in `Space_War.nes` PRG offset 0x10:

```
80 57 80 AE 80 08 80 5F 80 B6 80 17 80 6E 80 C5
```

Entries (high byte first, low byte second):

```
$8057, $80AE, $8008, $805F, $80B6, $8017, $806E, $80C5
```

The boot ROM picks one of these based on some power-on signal
(likely tied to a hardware test mode pin).  For emulator purposes
the first entry (`$8057`) is the safest default.

Loadcart already detects mapper 405 and sets `vt.submapper |= 0x80`.
What's missing is the actual boot stub installation in `mapVTinit`
(or a separate `mapVT405init`).  Two implementation approaches:

(a) **Patch the ROM cache.**  After `init_cache` mirrors the last
    PRG bank into EWRAM, write `4C 57 80` at `$FFD0` of the cached
    last bank and `D0 FF` at `$FFFC`.  Requires the cache to be in
    writable EWRAM, not cart ROM -- needs verification.

(b) **Override `memmap_F`.**  Allocate a 4KB EWRAM buffer, fill it
    with the last 4KB of PRG, patch in the boot stub and reset
    vector, then set `memmap_F` to point at that buffer.

Approach (b) is more robust.  See `cache.c` for memmap patterns.

### Mapper 256 submapper byte mangling

The OneBus reference (`mapper256.cpp`) defines three permutation
tables:

```
ppuMangle[16][6]   applied to $2012-$2017 PPU writes
cpuMangle[16][4]   applied to $4107-$410A CPU writes
mmc3Mangle[16][8]  applied to MMC3 bank-select low nibble when
                   writing to $8000-$9FFF (even addresses only)
```

Submappers 0, 6-12, and 13-15 (Jungletac) all use **identity** for
every table, so the two confirmed-working test ROMs do not need the
mangling.  Submappers 1 (Waixing VT03), 2 (Trump Grand), 3 (Zechess),
4 (Qishenglong), and 5 (Waixing VT02) do need it.

`vt.submapper` is plumbed through and accessible from `mapVT.s`; the
remaining work is to translate the three tables into assembly lookup
tables and apply them in `write_vt4xxx` (CPU table) and in a new
`$20xx` write hook (PPU table) and in a new `$8xxx` write hook (MMC3
table).

### VT369 ADPCM step table

`vt_regs.c` currently uses the standard 89-step IMA-ADPCM table.
Per `Furbtendulator/src-main/src/ADPCM_VT369.cpp`, real VT369
hardware uses a 16x16 step table with the decoder algorithm:

```c
int nibble = adpcm_frame >> (adpcm_position & 1 ? 4 : 0);
int index  = adpcm_lead
           - (adpcm_position >= 24 && adpcm_lead & 0x40 ? 1 : 0)
           + (adpcm_position >= 24 && adpcm_lead & 0x80 ? 2 : 0);
int step   = stepTable[index & 0xF][nibble & 0xF];
// then one of 4 prediction modes selected by (adpcm_lead >> 4) & 3
```

The 89-step table will produce reasonable-sounding output but not
bit-exact playback.  Replacing the decoder is mechanical once the
register-to-state mapping is verified against a few ROMs.

### 16x8 sprite rendering

When `vt_ppumode & 0x04` is set, sprites are 16x8 pixels: each OAM
entry's tile index applies to the left half, and `vt_oam_ext[i*2]`
provides the right-half tile index.  Hooking this into PocketNES's
sprite scan in `ppu.s` requires extending the per-scanline 8-sprite
limit check and emitting two GBA OBJ entries per VT sprite.

### `LDAXD` (0x1B) and `LDAD` (0x2B)

Stubs in `6502_vt.s` are not yet wired; the two patch lines in
`vt_patch_optable` are commented out.  Best reference is Furbtendulator
`CPU_VT369.cpp` `IN_LDAXD` / `IN_LDAD`.  Uncomment the patch lines
once handlers exist.

---

## File Map of VT-Specific Code

| File                       | Role                                                             |
|----------------------------|------------------------------------------------------------------|
| `src/config.h`             | Feature flags (now all unconditionally on)                       |
| `src/vt_regs.h`            | `VTState`, `VTAdpcmChan`, register-offset macros, public decls   |
| `src/vt_regs.c`            | Register dispatchers, ADPCM decoder, encryption helpers, vt_reset|
| `src/ppu_vt.h`             | Palette / OAM extension public API                               |
| `src/ppu_vt.c`             | Palette RAM, BGR555 conversion, OAM ext, `$20xx`/`$21xx` writes  |
| `src/6502_vt.s`            | VT extra opcode handlers, VT09 JMP encryption-commit wrappers    |
| `src/Mappers/mapVT.s`      | `mapVTinit`, `write_vt4xxx` -> `vt_reg_write`                    |
| `src/cart.s`               | Registers mapper 253 -> `mapVTinit`; clears `vt_active` per load |
| `src/loadcart.c`           | Translates NES 2.0 mapper 256/405 -> internal 253; sets submapper|
| `src/ppu.s`                | `PPU_W` dispatch with runtime `vt_active` gate                   |
| `src/sound.s`              | Calls `vt_adpcm_mix_gba` from `timer1interrupt`                  |
