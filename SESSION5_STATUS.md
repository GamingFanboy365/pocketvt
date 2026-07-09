# PocketVT — Session 5 status & handoff

This document captures the exact state of the PocketVT tree and the Lonely
Island (LI) debugging effort as of the end of session 5, so a fresh chat can
resume without re-deriving anything.

## What this package contains
- `pocketvt/` — the full source tree with **all** fixes from sessions 3–5
  replayed and verified compiling (99 objects, clean link).
- `play_li.gba` — the built multicart with Lonely Island injected, ready to
  run in mGBA (`mgba play_li.gba`).
- `pocketvt.elf` — the linked ELF with symbols, for `nm`/`objdump`/debugger use.
- This document.

Build recipe (sandbox, running as root, no devkitPro — plain
`gcc-arm-none-eabi`):
- Compile: `bash build_pvt.sh` (script reproduced at bottom).
- Link: `cd pvt_build && OFILES=$(ls *.o | grep -v '^gba_crt0_my.o$');
  arm-none-eabi-gcc -g -mthumb -mthumb-interwork -Wl,-z,muldefs
  -T ../pocketvt/src/gba_cart_my.ld -nostartfiles $OFILES gba_crt0_my.o
  -o pocketvt.elf`
- `arm-none-eabi-objcopy -O binary pocketvt.elf pocketvt.gba`, then run the
  mini gbafix, then inject with `python3 builder.py "Lonely Island.nes"`
  (builder hardcodes base = `./pocketvt.gba`, treats **every** argv as a ROM,
  and always writes `play_me.gba` — rename to `play_li.gba`).

Addresses are per-generation; re-derive with `nm` after any change. Current
generation: `_wantirq` = 0x0300735C, ppuctrl0 = 0x03007394, ppuctrl1 =
0x03007396, `vt` = 0x02004B88 (+0x68 period, +0x6C ctrl, +0x6D want),
`vt_active` = 0x02004C03, `op_table` = 0x03006F3C, BRK handler `_00` =
0x03002800, `empty_R` = 0x03005820, PRG EWRAM copy base = 0x02004D00,
`memmap_8` (the $8000 slot base) = 0x03006F08, `vt_oam_ext` = 0x02004AB8.

## Fixes in the tree (all verified)
1. **APU frame-IRQ VT gate** (sound.s `frame_irq_handler`): in VT mode never
   assert IRQ_APU — LI's XOP APU behaves as if the 2A03 frame-counter IRQ does
   not exist, and the stock handler otherwise storms the 6502. Phantom-safe
   exit re-checks `wantirq` before entering `CheckI`.
2. **VT timer on the timeout system** (equates.h node; sound.s
   `vt_timer_install_now` + `vt_timer_handler`; vt_regs.c arming hook and
   $4102 rephase; `vt_timer_tick_frame` latch supervisor called from ppu.s).
   The IRQ has correct mid-frame phase; a vblank-phased version blanked the
   picture. Boot-safety latch prevents an IRQ before the game sets its RAM
   vector.
3. **Lazy CHR sync** (ppu_vt.c): pending flag + once-per-frame flush + skip on
   unchanged banks. This is the perf fix — 750 ms/frame → 52 ms/frame
   (~19 emulated fps), confirmed by median vblank gap.
4. **vt_reset hygiene**: clears render_seen/armed and pends nothing.
5. **Symbol-derived ppuctrl1 shadow** (vt_regs.c): the shadow address is now
   computed from `&_wantirq` so it survives globalptr layout drift between
   tree generations (a hardcoded base burned us twice).
6. **Pending-IRQ clear at cart reset** (vt_regs.c vt_reset): the ROM menu runs
   the APU frame sequencer with vt_active=0, so a stale 2A03 frame IRQ
   (observed `_wantirq==0x40`) survives into cart runtime; a VT game can never
   ack it, so we clear it on reset.

## Current diagnosis of the LI "blue screen" — the main open thread
LI boots, shows intro/overworld to ~3 s, then parks on a flat blue backdrop
forever. The blue scene is a **long-crashed 6502**, not a rendering bug:

- The 6502 leaves legitimate code before ~3.3 s and executes EWRAM
  emulator-data-as-junk. Junk writes to $8000+ keep triggering the MMC3
  forwarder, so the memmap slots stay clean-looking (banks flip 7↔0x0A
  normally — 336 healthy base writes seen *during* junk execution).
- **First BRK opcode executes at t≈3.78 s** (Cycle 63477826), caught cleanly
  by a breakpoint on `_00` @0x03002800. Thereafter the CPU orbits
  BRK→$FFFE→$FCAF→($003A)=$FCC3 stub (delay, STA $4103, applies zeroed
  $1C–$23 shadows → $2000=0/$2001=0)→RTI→junk→BRK.
- State at the blue: ppuctrl0=0, ppuctrl1=0, DISPCNT=0x0440, zp$26 frozen 0x15,
  _wantirq=0x00, vt.timer_ctrl/period=0 (game never armed its timer),
  ($003C)=$FC36, ($003A)=$FCC3, zp$18=0xA8, zp$19=0x1E, zp$3E=0, zp$47=0.
  **One clean NMI would fully revive it** — the $FC36 body at $FCA2/$FCA9
  unconditionally re-enables NMI + rendering.
- 6502 stack at a crash pause decodes to return frames $FC02 (mainline was
  inside `JSR $C5D0` at $FC00), $C5ED, $E096 (the wait-$26 loop at $E094), and
  in an earlier dump $FC24 (inside the NMI handler's `JSR $8CB6` music call,
  bank 0x0A).

### Retired false leads (do not revisit)
- "Bank 0x0A content mismatch (EA 60 CD CD)" — an `r/4` **alignment artifact**.
  Reading host 0x020199B6 returns the aligned-down word at 0x020199B4 =
  prg[0x14CB4] = `EA 60 CD CD` exactly. **PRG mapping/content is 100% correct.**
  The banking-bug-crashes-music theory is dead in its simple form.
- "IRQ deliveries during gameplay" — SIGINT pauses masquerade as breakpoint
  hits (mGBA prints "Cycle:" on any pause). A real breakpoint on `irq6502`
  from t=2 s shows **zero** hardware IRQ deliveries during the crash window.
- "_wantirq=0x40 pending at blue" — that reading was at a stale address; the
  correct 0x0300735C reads 0x00 clean. The gate works; vt_active=1.
- r9 outside the core is **C scratch, not the guest PC** (only valid while
  r15 ∈ 0x03000000–0x03005000). The recurring r9=0x02004AB8 = `&vt_oam_ext`,
  a renderer buffer — meaningless as a PC. Do not trust r9 on out-of-core
  pauses or on early trace lines.

## The precise next step (where I was when this handoff was cut)
Mine the large mGBA execution trace to find the **exact poison jump** — the
first in-core instruction whose guest PC (r9, valid only when r15 is in-core)
leaves the legit ranges — then read ~50 lines before it to see the source
instruction that jumped there.

Trace format (compact mGBA `trace` output): each line is 16 space-separated
register hex words `r0 r1 ... r15` followed by `cpsr: XXXX | OPCODE: disasm`.
Field 10 (0-indexed 9) is **r9 = guest PC**; field 16 is **r15 = host PC**.
Filter to lines where r15 ∈ [0x03000000, 0x03005000] (in-core), then walk r9
and flag the first departure from: NES RAM 0x03000000–0x03002000, the PRG copy
window 0x02004D00–0x02024D00, or a valid memmap-slot-relative fetch. The
instruction ~1 emulated op before the departure is the culprit. Candidate
mainline context to disassemble: `$C5D0` (fixed bank, PRG offset 0x1C5D0) and
`$F9CB`, plus the music engine reached via the $05CD=$FE state set at $FBF8.

If instead the root cause is that LI's raster/NMI cadence genuinely needs the
VT timer IRQ and the game never arms it because of an earlier missed event,
the fix path is upstream of the crash — trace back from "what should have set
zp$47 / armed the timer / kept the NMI enabled" during the ~3 s of good play.

Once the crash is fixed, LI should unblock immediately (state is fully armed
for revival). Then: Star Ally regression check; remaining ~3× perf (stop
feeding the inherited 2bpp tile cache in 16-colour mode); per-tile palette;
audio-by-ear; $4106 H/V mirroring application.

## Parked (user's call)
- The docker/devkitPro `make` build "flashes black and white" — parked; the
  sandbox uses a hand-rolled gcc-arm-none-eabi build instead. Prior notes:
  the IWRAM hand-layout has canaries with zero slack; a canary alarm may *be*
  the flashing; `make clean` advised against stale-build hazard.

## build_pvt.sh (reproduce if missing)
```
SRC=/home/claude/pocketvt/src; BUILD=/home/claude/pvt_build
LIBGBA=/home/claude/libgba-master; CC=arm-none-eabi-gcc
ARCH="-mthumb -mthumb-interwork"
CFLAGS="-g -Wall -Os -mcpu=arm7tdmi -mtune=arm7tdmi -fomit-frame-pointer \
  -ffast-math -ffixed-r10 -std=gnu99 -fcommon \
  -Wno-error=incompatible-pointer-types -Wno-error=int-conversion \
  $ARCH -I$BUILD -isystem $LIBGBA/include"
ASFLAGS="$ARCH -I$SRC -isystem $LIBGBA/include"
# compile biosstubs.s (LZ77UnCompVram SWI stub), gba_crt0_my.s, all *.c,
# all *.s (except crt0), and Mappers/*.s -> $BUILD, then link as above.
```
