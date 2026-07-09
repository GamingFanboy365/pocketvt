# PocketVT VT03 / VT09 playability patch — v0.5

## TL;DR — what's new in v0.5

Two changes in `ppu_vt.c`, both required for playability:

1. **Black-screen root cause fixed.** `vt_palette_rebuild_gba()` was
   splattering black across every GBA sub-palette on every VBlank during
   COLCOMP=0 (legacy NES) mode, because `vt03_palette_lut[0..63]` is all
   `0x0000`. Now gated so the legacy `run_palette` path handles colours
   in NES-compat mode, and the COLCOMP=1 path writes per-sub-palette
   correctly so attribute bytes 1/2/3 actually pick up their own colours
   instead of falling back to black.
2. **VT03 4bpp CHR deinterleave added.** When a cart engages true 4bpp
   mode (COLCOMP=1 + BK16EN/SP16EN), `vt_chr_sync_from_prg()` now
   deinterleaves the 32-byte-per-tile 4bpp layout into the 16-byte-per-tile
   layout that PocketNES's tile cache expects. Without this, 4bpp games
   render scrambled tiles even after the palette path is correct.

After this patch, VT03 and VT09 carts should boot past the PocketVT menu
and actually render graphics (instead of an all-black screen). Sound and
inputs were already wired in v0.4.

If you only have time to patch one file from this drop: it's **`ppu_vt.c`**.

## Goal

Boot real VT03 / VT09 OneBus cartridges past the PocketNES menu and render
something on screen (ideally the title screen + working gameplay, not perfect
colours).

## Files in this drop

All paths relative to `pocketvt_extracted/pocketvt/src/`:

  - **`ppu_vt.c`**         — VT03/VT09 PPU extensions **(v0.5 palette fix)**
  - `ppu_vt.h`             — exports for ppu_vt.c
  - `vt_regs.c`            — $4100-$41FF register handler + MMC3 forwarding
  - `vt_regs.h`            — exports for vt_regs.c
  - `vt03_palette_lut.h`   — 4096-entry BGR555 colour LUT (≈8 KiB)
  - `ppu.s`                — VRAM_pal patched to route $3F80-$3F9F via C

## Build

Apply the file drop, then build as normal:

```
cd pocketvt_extracted/pocketvt
make clean
make
```

Output is `pocketvt.gba`.

---

## The v0.5 fix in detail

### Symptom

On v0.4: PocketVT menu rendered fine, but every VT03 / VT09 game booted to
a fully black screen. Sound (where the cart enabled it) was correct; the CPU
was clearly running; input was being read. Only the picture was broken.

### Root cause

`vt_palette_rebuild_gba()` in `ppu_vt.c` ran on every NES VBlank
(`ppu.s::newframe_nes_vblank`, called via `bl_long`). The v0.4 implementation:

1. Did **not** gate on COLCOMP — it ran the VT03 composite-palette path even
   when the cart was still in plain NES (2bpp, COLCOMP=0) mode, which every
   VT cart is in at reset until its boot code sets `$2010` bit 7.
2. Called `vt03_composite_to_gba(lo, 0)` for every entry. In COLCOMP=0 that
   returned `vt03_palette_lut[lo & 0x3F]`. The LUT's first 64 entries
   (indices 0..63 = `hi=0` row of the HSL2RGB table) are all **0x0000 (pure
   black)** — they correspond to "saturation=0, luminance=0" in the COLCOMP=1
   colour-space, which is just black.
3. Splattered that black across all 4 BG and 4 OBJ sub-palettes
   (`0x05000000..0x0500027F`), 16 entries each, every single VBlank.

PocketNES's standard `run_palette` (`ppu.s::vblank_handler`) ran *after*
`vt_palette_rebuild_gba` and refreshed slots **0..3** of each sub-palette
from `nes_palette` via the `MAPPED_RGB` table — which would have produced
correct NES colours **if** the palette were structured the way run_palette
expects. But v0.4 *also* wrote 16 entries of bg_colours[0..15] across all
four GBA palettes (using identical colour data), so the per-sub-palette
indexing was wrong: NES attribute bytes 1/2/3 land in GBA palettes 1/2/3,
and those palettes were getting either black (from gate-1 failure) or the
sub-palette 0 colours (from gate-2 failure) instead of their own colours.

The net effect: only NES BG attribute = 0 tiles had any chance of looking
correct, and even those only in slots 0..3. Combined with most carts using
attr=0 for boot logos and having backdrop colours that happened to be black
anyway, the user saw "screen is totally black".

### Fix (in `ppu_vt.c::vt_palette_rebuild_gba`)

Three early-out gates, in order:

```c
// Gate 1: not a VT cart at all -> let PocketNES handle palette normally
if (!vt_active) return;

// Gate 2: VT cart but still in COLCOMP=0 mode -> let run_palette handle it
//         via nes_palette + MAPPED_RGB.  Do NOT touch GBA palette here.
if (!(vt_reg_2010 & 0x80)) {
    vt_palette_dirty = false;   // ack so we don't spin
    return;
}

// Gate 3: nothing changed since last rebuild
if (!vt_palette_dirty) return;
```

Then the COLCOMP=1 path now writes **per-sub-palette** correctly:

```
GBA BG palette 0, entries 0..3  =  NES $3F00..$3F03  (via vt03 LUT composite)
GBA BG palette 1, entries 0..3  =  NES $3F04..$3F07
GBA BG palette 2, entries 0..3  =  NES $3F08..$3F0B
GBA BG palette 3, entries 0..3  =  NES $3F0C..$3F0F
GBA OBJ palette 0, entries 0..3 =  NES $3F10..$3F13
GBA OBJ palette 1, entries 0..3 =  NES $3F14..$3F17
GBA OBJ palette 2, entries 0..3 =  NES $3F18..$3F1B
GBA OBJ palette 3, entries 0..3 =  NES $3F1C..$3F1F
```

This matches exactly the layout that `run_palette` writes in NES mode, so
the two paths produce identical *positions* and differ only in the colour
table they consult (MAPPED_RGB[lo] vs vt03_palette_lut[hi:lo]).

### Why this also explains "VT09 menu works but games don't"

The PocketVT menu (your shell) is not a VT cart — `vt_active=0` — so
gate 1 immediately returned. The menu's GBA palette was therefore left
untouched, and PocketNES's normal run_palette path coloured it correctly.

The moment you launched a game, `mapVTinit` ran, which set `vt_active=1`.
The next VBlank then hit the v0.4 splatter code and went black.
Gate 2 is what prevents that for COLCOMP=0 boots.

---

## What's still wired end-to-end (carried from v0.4)

### 1. Real VT03 palette LUT

`ppu_vt.c::vt03_composite_to_gba()` indexes the canonical 4096-entry LUT
from `vt03_palette_lut.h` (extracted from EmuVT's HSL2RGB.TAB).

  * **COLCOMP=1** (`$2010` bit 7 set): `idx = (hi << 6) | lo`, look up in LUT.
  * **COLCOMP=0**: not used for palette anymore — see fix above; the legacy
    NES path handles colours.

### 2. $2010 / $2011 Extended Graphics Control

`vt_ppu_reg_write()` shadows `$2010` into `vt_reg_2010` and sets
`vt_palette_dirty=true` on COLCOMP transitions. Gate 2 above is what
consumes that signal.

### 3. $2012-$2017, $2018, $201A, $4100[3:0] CHR bank registers

Shadowed into `vt_chr_reg[0..5]`, `vt_chr_reg_2018`, `vt_chr_reg_201A`,
`vt_chr_outer_4100` in `vt_regs.c`. Every write that actually changes the
value triggers `vt_chr_sync_from_prg()`.

### 4. CHR-from-PRG sync (2bpp + 4bpp)

`ppu_vt.c::vt_chr_sync_from_prg()` copies 8 KiB of CHR pattern data from PRG
ROM into `NES_VRAM` based on the current bank registers, using the OneBus
MMC3-style mapping:

```
PPU $0000-$03FF -> ($2016 & 0xFE) * 1K
PPU $0400-$07FF -> ($2016 | 0x01) * 1K
PPU $0800-$0BFF -> ($2017 & 0xFE) * 1K
PPU $0C00-$0FFF -> ($2017 | 0x01) * 1K
PPU $1000-$13FF -> ($2012)        * 1K
PPU $1400-$17FF -> ($2013)        * 1K
PPU $1800-$1BFF -> ($2014)        * 1K
PPU $1C00-$1FFF -> ($2015)        * 1K
```

The 1K bank number is OR'd with `($4100[3:0] << 8)` to form the OneBus 12-bit
physical address.

**2bpp path (default):** raw `memcpy` per 1KB CHR page. Matches
Furbtendulator's `setCHR(bit4pp=false)` path (confirmed by code-reading
`h_OneBus.cpp::setCHR()`).

**4bpp path (engaged when `$2010` has COLCOMP=1 AND (BK16EN or SP16EN)):**
for each output byte `j` in 0..0x1FFF, source byte is
`(j & 0xF) | ((j & ~0xF) << 1)`, pulling from a 2KB source window per 1KB
output page. This picks out the planes-0-1 half of each 32-byte 4bpp tile
so PocketNES's existing 2bpp tile cache continues to work. Matches
Furbtendulator's `setCHR(bit4pp=true)` reference.

A mode transition on `$2010` bits 7/2/1 forces a re-sync from the new path,
so games that flip into 4bpp mid-frame still get the right CHR window.

After the copy, every byte in `dirty_tiles[]` (512 bytes, 1 per 16-byte tile)
and `dirty_rows[]` (32 bytes, 1 per 256-byte row) is set to `0xFF`, so the
GBA-side tile cache re-decodes every tile and repaints BG/sprite VRAM on
the next half-screen or vblank trigger.

### 5. ppu.s `VRAM_pal` palette routing

`$3F00-$3F1F` writes still update legacy `nes_palette[]` AND notify
`vt_palette_write_lo()`. `$3F80-$3F9F` writes route to
`vt_palette_write_hi()`, which sets `vt_palette_active`. With the v0.5
gates above, this only matters once a cart engages COLCOMP=1.

---

## What still doesn't work (known gaps)

These remain follow-up items once we confirm games boot past the splash
screen with this patch.

  * **Hi-res 16x8 sprites**: `vt_oam_ext[]` is shadowed but not yet consumed
    by the GBA OAM build.

  * **VT09 16-bit video bus** (`V16BEN`): not yet implemented.

  * **Real-time CHR fetch from PRG** at PPU read time: we sync on bank-register
    writes only. Games that change CHR via direct `$2007` writes will see
    those go into `NES_VRAM` (PocketNES's existing CHR-RAM path handles them);
    games that modify CHR via a non-standard route may need additional hooks.

---

## Verification

`ppu_vt.c` compiles clean with the project's flags:

```
arm-none-eabi-gcc -mthumb -mcpu=arm7tdmi -mthumb-interwork -mtune=arm7tdmi \
  -fomit-frame-pointer -ffast-math -ffixed-r10 -std=gnu99 -fcommon -Os \
  -include $(DEVKITARM)/.../gba_types.h \
  -I. -I$(DEVKITARM)/.../libgba/include \
  -c ppu_vt.c
```

Object-file symbol check confirms `vt_palette_rebuild_gba`,
`vt_chr_sync_from_prg`, `vt_palette_write_lo/hi`, `vt_colour_to_gba` are all
defined in `ppu_vt.o` and referenced correctly from `vt_regs.o` and from the
assembly side (`ppu.s::newframe_nes_vblank`, `ppu.s::VRAM_pal`). External
symbols (`_rombase`, `_rommask`, `NES_VRAM`, `dirty_tiles`, `dirty_rows`,
`vt_active`, `vt_reg_2010`) all resolve to existing `.global` exports in
`cart.s` / `ppu.s` / `Mappers/mapVT.s` / `vt_regs.c`.

## Test plan

1. Rebuild `pocketvt.gba` with this drop.
2. Boot PocketVT menu — should look identical to v0.4 (gated by `vt_active==0`).
3. Launch a VT09 cart (these tend to stay in COLCOMP=0 mode) — should now
   render its title screen via PocketNES's standard palette path.
4. Launch a VT03 cart — title screen should also render. If it engages
   COLCOMP=1 mid-game, you'll see colours but tiles may look "double-imaged"
   or have wrong pixels — that's the still-pending 4bpp CHR deinterleave.
5. If anything still boots black, capture a save state and the cart filename
   so we can check whether `vt_active` is being set and what `$2010` shows
   at the moment of the black frame.
