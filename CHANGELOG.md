# PocketVT Changelog

## 0.5.2 (this release) — strip mapVTinit down to the bare minimum

### What the video showed

The recording from 2026-05-14 was decisive.  Frame-by-frame analysis
of the 0:04-0:06 window (`ffmpeg -ss 4.0 -t 2.5 -vf fps=30`) revealed:

- The PocketVT *menu itself works correctly* -- "Jewel Quest" and
  "Lonely Island" both render with the selection asterisk.
- After a game is selected, VBA enters a sustained cycle on a
  **~750ms period** (= about 45 GBA frames):

      WHITE flash (1 frame, ~33ms)
      Bright fade through grey (4 frames, ~133ms, menu text visible)
      Dim fade to black (4 frames, ~133ms)
      Pure black (~350ms)
      WHITE flash again -- next cycle begins

- The greenish-pixel count spikes to ~64,000 during the "grey" phase
  of every cycle, confirming the *PocketNES menu* is re-rendering
  each cycle (the menu's bright background + green ROM-list text).
- The cycle period (~45 frames) is much faster than the 3-second
  watchdog (180 frames), so this isn't `check_canaries` firing.
- VBA's title bar starts reporting "VisualBoyAdvance- 0%" partway
  through, suggesting the GBA CPU is mostly halted between cycles.

The pattern is: *cart load completes, something fails immediately, the
menu loop in `rommenu.c` retries the cart load, the cycle repeats at
the tempo of one cart-load.*  Looking at rommenu.c lines 54-66, the
do-while loops on `(rommenu_state != FINISHED || selectedrom !=
romnumber)`, so if anything wedges `rommenu_state` or the cart-load
path itself never properly transitions to "running" state, the loop
spins forever reloading.

### The fix in 0.5.2

`mapVTinit` was carrying two stale calls from earlier iterations that
*loadcart_asm already does itself*:

1. `bl_long chr01234567_` -- removed.  `loadcart_asm` calls
   `chr01234567_(0)` at cart.s:454-455 *before* invoking mapperinit,
   so seeding CHR bank 0 inside mapVTinit was always redundant.

2. `bl_long mirror2V_` -- removed.  `loadcart_asm` calls `mirror2H_`
   at cart.s:507-509 *after* mapperinit returns, *with proper EQ/NE
   flags from `tst r1, #MIRROR`*.  Our call inside mapVTinit was both
   redundant (loadcart_asm's call overrides it) *and dangerous*:

       mirror2V_:
           ldreq r0, =m0101    @ uses caller's EQ flag
           ldrne r0, =m0011    @ uses caller's NE flag
           b mirrorchange      @ falls through into ubg2_

   The instruction immediately before our call was `bl_long
   chr01234567_`, whose `bx lr` tail doesn't set any flag in a defined
   direction.  So `mirror2V_` picked m0011 vs m0101 essentially at
   random, then fell through into `mirrorchange` -> `ubg2_` which
   read `chrold`/`chrline` and clobbered r3-r5 -- not necessarily
   crashing the CPU but quite possibly leaving PocketNES's
   nametable/CHR-cache state inconsistent enough that the *next*
   step (CPU_reset at cart.s:511) saw something it couldn't resolve.

   That's a *strong* candidate for the fast reset cycle: every cart
   load wedged some bit of PocketNES bookkeeping, the rommenu loop
   retried, the cycle repeated at one-cart-load tempo (~45 frames,
   matching the video).

### What `mapVTinit` is now

After the strip:

    mapVTinit:
        .word write_vt_rom x4       @ writemem_8/A/C/E hooks (MMC3 forwarder)
        stmfd sp!, {lr}
        ldr r0,=vt_active; mov r1,#1; strb r1,[r0]    @ mark as VT cart
        bl vt_reset                                   @ zero VT state
        bl_long vt_resync_prg_banks                   @ compute initial banks
        adr r1, write_vt4xxx; str_ r1, writemem_4     @ install $4xxx hook
        #if VT_EXTRA_OPCODES
        bl_long vt_patch_optable                      @ install VT09 opcodes
        #endif
        ldmfd sp!, {pc}

That's the minimum needed for a VT cart to even *try* to boot.  CHR
banks and mirroring are now handled exclusively by `loadcart_asm`'s
standard pre/post-mapperinit calls.

### If 0.5.2 still cycles

Then the wedged state is somewhere else -- most likely:

- **`vt_resync_prg_banks` corrupting m6502_pc.**  It calls map89_,
  mapAB_, mapCD_, mapEF_ in sequence; each calls `flush` which uses
  `lastbank` to rebase m6502_pc.  If `lastbank` isn't right at the
  point this runs (we're called from mapperinit, when m6502_pc has
  been pre-set to 0 -- per cart.s:388 -- but never actually
  encodePC'd yet), the rebase math could land m6502_pc somewhere
  invalid.  The fix would be a bare-bones initial bank setup using
  `bl_long map89_` etc. *directly* with the right page numbers,
  matching how mapper11init does it -- skipping the
  resync-via-C-function path until after CPU_reset has run.
- **A subtle bug in `vt_reset` or `vt_mmc3_reset`.**  Neither of
  these touches lastbank or m6502_pc, but it'd be worth verifying
  they don't accidentally write into pinned PocketNES state.
- **The MMC3 forwarder triggering on a `STA` to high RAM** if
  writemem_8/A/C/E ends up installed for some address range the
  game writes to *before* getting to a bank-switch.  Worth tracing
  exactly what the cart's reset code does for the first ~50 cycles.

### What to look for in the next test

The most useful new datapoint would be a screen photo at the
*brightest moment of the cycle* (the WHITE flash) zoomed in enough
that we can see whether the title bar reads "VisualBoyAdvance" (CPU
running) or "VisualBoyAdvance- 0%" (CPU halted).  The video has
both at different points and the transition timing is the smoking
gun for whether the loop is happening inside PocketNES's C code or
inside the emulated 6502.

## 0.5.1 — strip risky C-to-asm calls from MMC3 forwarder

The libgba upload was actually a useful clue, but the smoking gun was in
PocketNES's own `ppu.s`: there's a **3-second watchdog** in
`check_canaries` that calls `crash()` if the NES PPU's prerender scanline
hasn't been reached for 180 GBA VBlanks.  `crash()` does `cls(1)` (clear
screen) and then displays "Oh no! PocketNES crashed!" -- the white flash
you saw before the "reset" is `cls(1)` and the "reset" was almost
certainly the crash dialog appearing (which loops forever waiting for
SELECT, but on hardware can be hard to read).

So: 0.5.0 was actually making real progress -- the CPU was running
*briefly*, long enough to write palette colour $30 (white) to $3F00,
then it was hanging.  The watchdog tripped and the crash screen flashed.

### The hang itself

Two of the C-to-asm calls I added in 0.5.0 were unsafe and almost
certainly the proximate cause of the CPU hang:

- **`mirror2H_` / `mirror2V_` were being called from the MMC3 forwarder
  when the game wrote `$A000`.**  Look at `cart.s` 707-710:

      mirror2H_:
          ldreq r0,=m0011
          ldrne r0,=m0101
          b mirrorchange

  Both possible loads are *conditional on EQ/NE flags supplied by the
  caller* -- the cart.s post-mapperinit dispatcher does
  `tst r1, #MIRROR; bl_long mirror2H_`, supplying those flags from the
  iNES MIRROR cartflag bit.  From C, those flags are whatever the
  compiler last set them to (probably from the `if (val & 0x01)` test
  immediately above, which is unrelated).  So the function picks a
  basically-arbitrary mirror layout, then falls through into
  `mirrorchange` -> `ubg2_` which do nontrivial register manipulation
  the C ABI doesn't expect to lose track of.  Calling these from C is
  unsound until we have a flag-safe wrapper.

- **`chr0_`-`chr7_` were being called from both the MMC3 forwarder
  (cmd 0-5 of $8001) and `vt_ppu_reg_write` (for $2012-$2017).**
  These routines assume CHR-ROM-backed carts and update PocketNES's
  vram_map / instant_chr_banks tables.  But every OneBus VT cart has
  *no CHR ROM* -- per the wiki "VT02+ PRG-ROM Bankswitching":

      regular OneBus ROM images will only contain PRG ROM and no
      CHR ROM, and PRG ROM ($41xx) and CHR ROM ($201x) bankswitch
      registers apply to the same address space.

  Feeding VT bank-numbers into PocketNES's CHR-ROM-shaped routines
  installed wrong VRAM pointers and was wedging the PPU's BG fetch on
  the first frame.  That hangs the CPU, the watchdog trips ~3s later,
  cls(1) + "Oh no!" = the user-visible white-flash-then-reset.

### What 0.5.1 actually changes

- **`vt_mmc3_forward` no longer calls `mirror2H_`/`mirror2V_` or
  `chr0_-7_`.**  For `$A000` writes the mirroring bit is still shadowed
  into `vt.reg[0x06]`.  For `$8001` cmd 6/7 (PRG banks), full bank
  recompute still happens via `vt_resync_prg_banks()` -- this is the
  critical path that lets games actually load gameplay code into
  $8000-$BFFF.  For `$8001` cmd 0-5 (CHR banks), shadow only -- no
  effect on screen until ppu.s grows CHR-from-PRG support.

- **`vt_ppu_reg_write` no longer calls `chr0_-7_` for $2012-$2017.**
  Same reason.  The $2018/$201A/$2107 extended-mode bytes are still
  stored.

- **Performance side note**: stripping those calls also makes the
  MMC3-write hot path much cheaper.  Real OneBus games can hit it
  hundreds of times per frame during normal scrolling.

### What to expect now

The visible symptoms depend on what the games actually do:

- Best case: CPU now runs all the way into the game's main loop.  You
  might see a static logo, a partially-rendered title screen with junk
  tiles where the CHR data should be, or just a static background.
  No watchdog reset.
- Likely case: same as best case, except some games loop forever
  waiting on the VT timer IRQ ($4101-$4104) which is still TODO.  No
  watchdog reset because the prerender line still gets hit -- just
  the game is logically frozen.
- Worst case: another C-to-asm call I didn't find is still wedging
  something, watchdog fires again, same white-flash-reset.  If this
  happens after 0.5.1, I need a different angle -- maybe a screen
  photo of what's visible *before* the white flash, since that's a
  fingerprint of how far the CPU got.

### About libgba

The `IntrTable` size warning in your build log (56 bytes from
`interrupt.o` vs 120 bytes from `libgba.a(interrupt.o)`) is real but
harmless: PocketNES brings its own IntrTable and interrupt dispatcher
that match each other (flat array of 14 function pointers).  The
warning is just the linker noting that the libgba version is larger
and ignored under `-z muldefs`.  Vanilla PocketNES has lived with this
warning since the libgba upgrade and it's not the cause of any
PocketVT problem.

## 0.5.0 — MMC3 forwarding

The 0.4.0 release added the OneBus dynamic $4100+ bank mapping, which
was real and necessary, but games still infinite-reset-looped because
**they don't bank-switch via $4100+ in the first place** -- they
bank-switch via the standard MMC3 register block at $8000-$FFFF, and
the VT silicon forwards those writes into the VT-native registers
under the hood.  This release implements that forwarding.

### Fixed — the reset-loop

- **`$8000-$FFFF` writes are now translated into the VT-native register
  effects, the way real silicon does.**

  Quoting the NESdev wiki article "VT02+ MMC3 Compatibility Registers"
  (which you uploaded as `NESdev_Wiki-20260514155623.xml`):

      Backwards compatibility is realized by way of forwarding CPU
      writes to $8000-$FFFF to the appropriate VT02+ register.
      V.R. Technology's data sheets call this the "old compatible
      mode".  Forwarding can be disabled by setting register $410B
      bit 3 (FWEN).

  My 0.3.x/0.4.x writes installed `.word void, void, void, void` as
  the four writemem handlers for $8000-$9FFF / $A000-$BFFF /
  $C000-$DFFF / $E000-$FFFF, because at the time I'd only seen ROM
  init code writing to $4100+.  The wiki article makes it explicit
  that the majority of OneBus games actually bank-switch via the MMC3
  protocol -- and on chip, those $8000 writes get forwarded into the
  same $4100+ register updates.

  Dropping every $8000+ write meant the games' early init "STA $8000;
  STA $8001" pair to load gameplay code from a different PRG bank
  silently disappeared.  Each game then jumped to the address it
  expected to contain that loaded bank, found stale ROM (or RAM)
  there, crashed within a few instructions, and PocketNES's crash
  handler returned to the menu.  On a single-ROM cartridge the menu
  auto-relaunches the only entry -- which is precisely the visible
  reset-loop-with-flicker the user reported.

  The fix:

  - New `vt_mmc3_forward(addr, val)` in `vt_regs.c` implements the
    exact translation table from the wiki: $8000 saves the MMC3 cmd
    byte and updates $4105's COMR6 bit; $8001 dispatches by `cmd & 7`
    into the six CHR registers ($2012-$2017) and two PRG registers
    ($4107/$4108); $A000 updates $4106 bit 0 and calls PocketNES's
    `mirror2H_`/`mirror2V_`; $A001 is ignored (VT has no $6000 WRAM);
    $C000-$E001 shadow into $4101-$4104 (the VT timer/IRQ registers;
    actual IRQ wiring is still TODO but the byte gets stored).
  - The FWEN bit ($410B bit 3) is respected -- if a multicart menu
    sets it, forwarding stops, matching how "Classic Max Lite
    120-in-1" prevents `Bolt Fighter`'s gameplay garbage writes from
    corrupting the bank registers.
  - New asm wrapper `write_vt_rom` in `mapVT.s` forwards to the C
    function, and the `mapVTinit` header now reads `.word
    write_vt_rom, write_vt_rom, write_vt_rom, write_vt_rom` instead
    of four `void`s.
  - The MMC3 cmd byte is held in a file-scope static (`vt_mmc3_cmd`)
    rather than added to `VTState`, because `VTState`'s binary layout
    is pinned by `_Static_assert` checks that 6502_vt.s depends on.
    `vt_reset()` now also calls `vt_mmc3_reset()` to clear the cmd
    byte on every cartridge reload.

  All six CHR bank routines (`chr0_` through `chr7_`) and both mirror
  routines (`mirror2H_`, `mirror2V_`) are called directly from C.
  This relies on `-ffixed-r10` (added in 0.3.0) keeping globalptr
  intact across the call boundary.  Performance: every `STA $8xxx-$Fxxx`
  now goes through a C call rather than the asm dispatch
  table.  If profiling on real hardware shows this is a hot-path
  bottleneck, the dispatcher can be inlined back into `mapVT.s` using
  `map4.s`'s MMC3 command-table pattern; the protocol is identical,
  only the destination registers differ.

### Status of test ROMs after 0.5.0

If something still resets or shows black after this:

- For `Jewel_Quest`, `Rubble_the_Engineer`, `Lucky_Lawn_Mower_VT369`:
  these are all mapper-256 sub-0.  After the four-fix stack
  (writemem addr from r12 / opcode positions / dynamic $4100 mapping /
  MMC3 forwarding) they should at minimum get past their RAM-clear
  loops and reach their main game loop.  Whether they actually render
  graphics depends on whether they use the VT enhanced palette /
  hi-res sprites / 4bpp modes -- none of which is currently wired up
  in `ppu_vt.c`.  So black-but-CPU-running is still possible.

- For `Lonely_Island`, `Star_Ally__VT03_`: sub-15 with Jungletac
  opcode encryption.  The wiki says encryption is controlled by
  $4169 bit 0 (default 0 = enabled), NOT $410F as I had earlier.
  This iteration does NOT yet fix the $4169 vs $410F mismatch -- if
  those two ROMs still reset-loop, that's likely why.

- For `Space_War`: mapper 405.  Zero-vector boot stub still pending.

If after 0.5.0 you see something other than a reset loop (e.g.
garbled graphics, frozen on a logo, partial menu rendering, etc.),
that's positive progress -- it means the CPU is now running real game
code and we've moved from "CPU crashes immediately" to "PPU isn't
emulating something the game needs."

## 0.4.0 — dynamic OneBus bank mapping

The 0.3.1 release confirmed `mapVTinit` was being entered correctly and
the writemem hooks were sane, but every test ROM still booted to a
black screen.  This release adds the piece that was actually missing:
**real OneBus bank-recompute on every relevant $41xx write**, ported
directly from MAME.

### Fixed

- **OneBus PRG mapping is now dynamic, like real hardware.**  Real VT
  chips recompute all four 8KB PRG windows ($8000/$A000/$C000/$E000)
  every time one of seven registers changes -- $4100 (PA21), $4105
  (COMR6), $4107-$4109 (PQ0/PQ1/PQ2), $410A (PA8), $410B (PS / PQ2EN).
  0.3.x only remapped *individual* windows when their specific PQn
  register was written, and never touched $E000-$FFFF at all -- so the
  reset-vector bank was permanently stuck at iNES "last bank" forever
  after power-on.

  This was specifically lethal for `Jewel_Quest.nes` and
  `Rubble_the_Engineer.nes`.  Both have a two-instruction stub at
  $F856:

      LDA #$8A
      STA $410B    ; PS=2 in the page-size/PQ2EN register

  On real silicon the `STA $410B` re-runs the bank recompute and
  $E000-$FFFF *changes physical bank mid-execution* -- from bank 31
  (the iNES last bank, holding the tiny stub) to bank 15 (which holds
  the actual reset code: CLD/SEI/LDX #$FF/TXS/...).  Without the
  recompute the CPU just executed the `BRK` ($00) that immediately
  follows the stub and trampolined to the IRQ vector at $FFFE, which
  on these ROMs is $0000 -- i.e. uninitialised RAM.  Instant crash,
  black screen.

  The new `vt_resync_prg_banks()` (in `vt_regs.c`) is a direct port of
  MAME's `nes_vt02_vt03_soc_device::update_banks()` and `::get_banks()`
  (see `nes_vt_soc.cpp` lines 205-260 in your `headz` upload).  The
  eight-case switch on `PS = $410B & 7` handles every page-size and
  high-bit overlay configuration the OneBus hardware supports.

- **`vt_reset()` now runs before bank setup in `mapVTinit`.**
  Required because `vt_resync_prg_banks` reads `vt.reg[]`, which
  `vt_reset` zeros.  No functional difference for boot-time defaults
  (everything is zero either way), but it now matches the conceptual
  flow and lets the resync function read clean state.

- **`write_vt4xxx` is much simpler.**  The inline PRG bank-switch
  ladder (the `.Lcheck_pq1` / `.Lcheck_pq2` / `.Lalso_shadow` blocks)
  is gone.  Every $4100-$41FF write now just forwards to
  `vt_reg_write`, which calls `vt_resync_prg_banks` internally on the
  seven trigger registers.  Matches MAME's flow.

### Removed

- **`test_roms/`** is no longer in the source tree.  Test ROMs belong
  in your own collection, not the repo.

### About the rest of `src/Mappers/`

You asked whether the standard NES mappers are necessary.  For *VT*
ROMs, no -- VT mapper-256 and mapper-405 ROMs only ever go through
`mapVTinit`.  Removing them would shrink the GBA binary by ~30-50 KB
and speed up the build noticeably.  But removing them safely requires
editing `cart.s` to delete the corresponding `.byte` entries in
`mappertbl` AND the matching `.word` entries in `mappertbl2` in
lockstep -- the index of each `.byte` in mappertbl must equal the
index of its `.word` in mappertbl2, and the dispatcher reads them by
position, not by mapper number.  Get that wrong and the build silently
misdispatches.  I'd recommend doing that as its own pass once the boot
issues are fully settled, so the diff is small and reviewable.

### Status of test ROMs

After this release I'd *expect*:

- `Jewel_Quest.nes`, `Rubble_the_Engineer.nes`: should now get past
  the `STA $410B` bank-swap stub and reach the actual reset code in
  bank 15.  Whether they make it to a title screen depends on whether
  they need IRQs (still TODO) or VT-specific PPU features I haven't
  hit yet.
- `Lucky_Lawn_Mower__VT09_.nes`: its reset writes `LDA #$0B; STA $410B`
  (PS=3, prgAND=0x07).  For a 64KB ROM this maps $E000-$FFFF to bank 7
  which is already the last bank, so the resync should be a no-op in
  practice and the ROM should reach its main loop.
- `Lucky_Lawn_Mower__VT369_.nes`: doesn't touch $410B at all and has
  a perfectly standard reset.  Should reach its main loop.
- `Lonely_Island.nes`, `Star_Ally__VT03_.nes`: sub-15 (Jungletac
  encryption).  Should reach the first JMP that enables the XOR gate,
  then either run or crash on the unimplemented opcodes 0xA7 / 0xBF.
- `Space_War.nes`: mapper 405 zero-vector boot stub still unimplemented
  -- will load but won't boot.

If any of these still black-screen after 0.4.0 I need a more
specific symptom -- is the screen literally never updating (PPU not
enabled), or does the screen flash some color and then go dark (CPU
ran briefly, then crashed/hung), or does the menu reappear (PocketNES
crash handler tripped)?  Each one points at a different bug.

### Still pending (known issues)

- Mapper 405 zero-vector boot stub.
- IRQ from the VT timer wired to the 6502 IRQ line.
- VT369 16x16-step / 4-mode ADPCM.
- Opcodes 0xA7 (LDAXD (zp),Y) and 0xBF (LDAD abs,X).

## 0.3.1 — `.word void` header fix

**0.3.0 fixed five real bugs that needed fixing, but it did not fix the
black screen.** ALL of those fixes were necessary, none of them were
sufficient, and the actual reason every ROM was black-screening is in
this release.

### Fixed

- **`mapVTinit` was missing its 4-word writemem-handler header.**

  PocketNES's `loadcart_asm` dispatch loop expects every mapperinit
  function to begin with exactly four `.word` entries giving the
  writemem handlers for $8000-$9FFF / $A000-$BFFF / $C000-$DFFF /
  $E000-$FFFF.  The dispatcher reads those four words, installs them
  into the writemem table, and then jumps to *mapperinit + 16*.

  PocketVT's `mapVTinit` started directly with `stmfd sp!,{lr}`.  So
  the dispatcher:
    (a) installed the first four ARM instructions of `mapVTinit` --
        raw machine-code bytes -- as the writemem handlers for the
        $8000-$FFFF range.  Any $8000+ write would jump into the
        middle of an instruction byte sequence.
    (b) jumped 16 bytes into `mapVTinit`, skipping the `stmfd sp!,{lr}`
        and the `vt_active=1` store.
    (c) when `mapVTinit` eventually ran its `ldmfd sp!,{pc}`, that
        popped a stack slot that nothing had pushed, returning to a
        random address.

  Net effect: every VT cart silently corrupted the writemem dispatch
  for $8000+ and then jumped to garbage long before the reset vector
  fired.  Black screen on all submappers regardless of all the 0.3.0
  fixes -- those fixes were needed for *what happens once the CPU
  starts running*, but the CPU was never even getting to its reset
  vector.

  Now starts with `.word void, void, void, void` (VT carts don't bank
  via $8000+ writes; the actual bank registers live at $4100+).  Cross-
  checked against every other mapperinit in `src/Mappers/` -- they all
  follow this convention; mapVTinit was the only one that didn't.

### Changed

- **Removed `test_roms/` from the source tree.**  It was a convenience
  from earlier iterations.  README still documents which ROMs were
  tested against and what the expected status of each is.

- **README now explains what's in `src/Mappers/`** and how to slim
  the build if you want to.  The short version: every NES mapper file
  except `mapVT.s` is dead code from PocketVT's perspective, but
  removing them needs care because some files implement multiple
  mappers (e.g. `map023771.s` is mappers 0, 2, 3, 7, 71) and they're
  referenced by explicit function pointers in `cart.s`'s mapper
  dispatch table, not via weak symbols the linker can drop.

## 0.3.0 — five real bug fixes, but not the one that mattered

### Fixed — the showstoppers from 0.2.x

- **`write_vt4xxx` was reading the NES address from the wrong register.**
  This is the bug that produced black screens on every ROM regardless of
  submapper.  The PocketNES writemem dispatch macro is

      mvn r1, addy, lsr#PRG_BANK_SHIFT
      ldr pc, [m6502_mmap, r1, lsl#2]

  so by the time `write_vt4xxx` is entered, `r1` is the negated bank
  index (some small negative number), **not** the bus address.  The
  original code did `mov r5, r1` and then tested `r5 >> 8 == 0x40`,
  which always failed, so the entire $4000-$5FFF range — APU $4015,
  sprite DMA $4014, controllers $4016/$4017, frame counter $4017, plus
  every VT register at $4100+ — was silently dropped.  Fixed by reading
  from `addy` (r12) directly.  This alone is enough to make every
  mapper-256/submapper-0 ROM (Jewel_Quest, Rubble_the_Engineer,
  Lucky_Lawn_Mower_VT369) progress past the boot stub.

- **VT extra opcodes were patched at completely wrong positions.**
  The 0.2.x notes claimed PHX=0x89, PHY=0x8B, PLX=0xAB, PLY=0xBB,
  TAD=0x4B, TDA=0x6B, ADX=0x03 (zp,X).  Cross-checked against
  Furbtendulator's `OneBus_VT369.cpp` opcode dispatch table; real chip
  positions are:

      0x34  PLY  (impl, 1 byte)
      0x3C  PLX  (impl, 1 byte)
      0x5A  TAD  (impl, 1 byte)
      0x62  ADX  (abs,  3 bytes)
      0x7A  TDA  (impl, 1 byte)
      0xC2  PHX  (impl, 1 byte)
      0xD2  PHY  (impl, 1 byte)

  Note 0x62 ADX uses absolute, not zero-page indexed, addressing.
  Lonely_Island's reset code at $FFD4 contains a literal `C2` byte
  (PHX) that the old code routed to the standard `_xx` illegal-opcode
  handler, which misaligned PC and crashed the CPU into garbage.

- **Encryption was force-on at power-on for submapper 13-15.**  The
  reset code in those ROMs is actually unencrypted (`78 C9 00 8D 00 20`
  at $FFD4 in Lonely_Island = SEI/CMP/STA, plainly readable).  Real
  hardware enables the XOR gate later via a $410F=$00 write, which the
  emulator now commits at the next JMP.  Forcing it on at reset caused
  the unencrypted SEI to decode through the gate as `D9` (CMP abs,Y)
  and walk the CPU off a cliff.

- **PRG bank switches were not actually performed.**  `vt_reg_write`
  recorded writes to $4107-$410A in `vt.reg[]` but never called
  `map89_`/`mapAB_`/`mapCD_`.  Games that bank-switched into
  $8000-$BFFF after the reset stub were running against whatever bank
  happened to be there from boot, producing immediate crashes once they
  jumped to a switched-in address.  Now dispatched inline in assembly
  inside `write_vt4xxx`:

      $4107 -> map89_(val)    PQ0 -> $8000-$9FFF
      $4108 -> mapAB_(val)    PQ1 -> $A000-$BFFF
      $4109 -> mapCD_(val)    PQ2 -> $C000-$DFFF (only if PQ2EN)

  Doing it in asm keeps globalptr (r10) trivially correct for the
  PocketNES bank routines.

- **CHR bank switches were not performed either.**  Same problem on
  the PPU side: writes to $2012-$2017 (OneBus CHR bank registers)
  reached `vt_ppu_reg_write` but never propagated to `chr0_-chr7_`.
  Now wired through, with the 2KB-granular `$2016` and `$2017` writes
  setting paired 1KB banks the same way h_OneBus.cpp does (`val & ~1`
  and `val | 1`).

- **`-ffixed-r10` added to CFLAGS.**  Some PocketVT C functions
  (`vt_ppu_reg_write`) now call into ARM-mode globalptr-relative
  assembly helpers.  Without `-ffixed-r10`, GCC could spill a temporary
  into r10 and silently corrupt globalptr before the asm call.
  PocketNES never needed this because PocketNES C code never called
  those routines; PocketVT does.

- **Reset bank layout matches OneBus hardware.**  `mapVTinit` now sets
  up $8000=bank 0, $A000=bank 0, $C000=bank 0xFE, $E000=bank 0xFF —
  the real-chip default — instead of the old `map89ABCDEF_(-1)` which
  mapped the last 32KB across $8000-$FFFF.  All known VT mapper-256
  reset vectors live in $E000-$FFFF (the fixed last bank), so this
  doesn't change the reset path, but it does mean games that read from
  $8000-$BFFF before doing their first bank switch see bank 0 (real
  behaviour) instead of bank N-2.

### Restored

- **`builder.py`**, rewritten and cleaned up.  Same 48-byte header
  format as PocketNES's builder.py (32-byte name + four little-endian
  u32 fields).  Now does an iNES magic-byte sanity check, gives clearer
  error messages, and handles paths that aren't in the current
  directory.  Reads `pocketvt.gba`, writes `play_me.gba`.

### Still pending (known issues; see `IMPLEMENTATION_NOTES.md`)

- Mapper 405 (Space_War) has all-zero NMI/reset/IRQ vectors and relies
  on an internal boot stub that reads a high-byte-first jump table at
  PRG offset $10.  Not yet implemented; the ROM will load but won't
  boot.
- IRQ from the VT timer ($410C-$410E) is shadowed in state but not yet
  wired to the 6502 IRQ line.  Games that wait on it will hang.
- The ADPCM mixer uses the standard 89-step IMA table and Δ-mode-only
  predictor.  VT369's real 16x16 step table and 4-mode predictor are
  not yet implemented.  Sound will be approximately correct but not
  bit-accurate.
- Opcodes 0xA7 (LDAXD, (zp),Y) and 0xBF (LDAD, abs,X) are not yet
  implemented.  Their addressing modes are nonstandard and need
  dedicated assembly handlers; ROMs that hit them will crash on a
  misaligned PC.

## 0.2.1

### Fixed
- **`_6C` not exported from `6502.s`.**  The VT09 JMP-indirect
  encryption-commit wrapper in `6502_vt.s` tail-calls into the original
  JMP-ind handler at label `_6C`, but `6502.s` only marked `_4C`
  (JMP abs) as `.global`.  Plain `make` against devkitARM 15.2.0
  failed at link time with `undefined reference to `_6C``.  Added
  `global_func _6C` alongside `_4C`.

## 0.2.0 (this release)

### Fixed
- **Build failure.**  Plain `make` previously failed at link time with
  four undefined references (`vt_ppumode_write`, `vt_ppumode`,
  `vt_reset`, `vt_reg_write`).  Root cause: assembly always referenced
  these symbols but C side gated them on `#if VT_MODE`.  Fixed by
  making VT support unconditional -- see "Removed" below.
- **PPU `$2008+` mirror divert breaking on non-VT carts.**  The
  extended-register hook in `ppu.s` rerouted *all* `$2008-$3FFF` writes
  to `vt_ppu_reg_write`, which broke standard NES PPU register
  mirroring.  Now gated on a runtime `vt_active` byte set only by
  `mapVTinit`; non-VT mappers leave it at 0 and get correct mirroring.
- **Makefile `VT=VT09` flag silently dropped.**  `-D VT09` was appended
  to `CFLAGS` before `CFLAGS` was initialized with `:=`, so the define
  never made it to the compiler.  Reordered so the flag survives.

### Added
- **Runtime submapper-driven encryption.**  `loadcart.c` extracts the
  NES 2.0 submapper from header byte 8 and stores it in `vt.submapper`.
  `vt_reset()` reads it and enables `encryption_active` automatically
  for mapper 256 submappers 13-15 (Cube Tech / Karaoto / Jungletac).
  This means `Lonely_Island.nes` and `Star_Ally__VT03_.nes` boot with
  encryption on without any user-visible build flag.
- **ADPCM rate counter.**  New `u16 rate_counter` in `VTAdpcmChan`
  decrements per sample tick; nibble decode only fires on underflow,
  then reloads from `ch->period`.  Higher period -> slower playback,
  matching observed VT hardware behaviour.
- **ADPCM end-of-sample detection.**  New `u32 end_addr` field and two
  new register offsets (`$4126`/`$4127`/`$412E`/`$412F`,
  `VT_ADPCM_END_LO`/`HI`).  Channel stops when `cur_addr >= end_addr`
  (or runs forever if `end_addr == 0`).
- **`$4140-$417F` palette and `$4180-$41FF` OAM-ext forwarding.**
  `vt_reg_write` now actually calls `vt_palette_write()` /
  `vt_oam_ext_write()` for these ranges; previously they were TODO
  comments and silently dropped writes.
- **Static asserts on `VTState` layout.**  `6502_vt.s` hardcodes the
  byte offsets of the three encryption fields.  `_Static_assert`s in
  `vt_regs.c` now lock them down at compile time -- a future struct
  edit that reorders these fields will fail the build instead of
  silently breaking VT09 encryption.
- **Mapper 405 marker.**  Bit 7 of `vt.submapper` is set by
  `loadcart.c` when mapper 405 is detected, so the eventual
  zero-vector boot stub in `mapVT.s` has the info it needs.

### Removed
- **Plain NES compatibility build.**  PocketVT is now a VT-only
  emulator.  `make` builds a single binary with all VT features
  enabled.  `VT_MODE`, `VT_EXTRA_OPCODES`, `VT_ENHANCED_PALETTE`,
  `VT_HICOLOR_SPRITES`, `VT_ADPCM_SOUND`, `VT09_ENCRYPTION` are now
  unconditionally `1` in `config.h`.  Use upstream PocketNES for
  non-VT NES emulation.

### Known issues still open
- Mapper 405 zero-vector boot ROMs (e.g. *Space_War*) do not yet boot.
  The detection and marker are in place; the bootstrap stub itself
  isn't.  See `IMPLEMENTATION_NOTES.md` for the format.
- Mapper 256 submapper byte-mangling tables (CPU/PPU/MMC3) are not
  implemented for submappers 1-5.  Submapper 0 and 15 (Jungletac) use
  identity tables and don't need the work, which covers both shipped
  test ROMs that should work.
- ADPCM uses a generic 89-step IMA table instead of the VT369 16x16
  table with 4 prediction modes.  Sounds approximately right but not
  bit-exact.
- 16x8 sprite rendering, LDAXD, and LDAD opcodes are still TODO.

## 0.1.0

Initial PocketVT fork from PocketNES.
