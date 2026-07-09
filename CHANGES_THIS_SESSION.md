# Changes in this tree (cumulative, current session included)

Build exactly as you do: `sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make`
then inject with `python3 builder.py <game.nes>`.

## NEW THIS SESSION - aimed directly at the "compiles but crashes completely" problem

### 1. Linker script fix: orphan `.append` section (gba_cart_my.ld) - PRIME SUSPECT for the crash
The font data (`bindata.s`) lives in a custom section `.append`. The MULTIBOOT
linker script places it explicitly; the CARTRIDGE script (the one `make` uses)
never did - so it was an ORPHAN section, and orphan placement varies between
binutils versions. The appended-ROM scanner requires `__rom_end__` == physical
end of pocketvt.gba (builder.py appends games at file end). My linker happened
to place the orphan harmlessly mid-binary; a different binutils placing it after
`.pad` breaks the invariant - VERIFIED in a simulation: the scanner then reads
font bytes instead of the game header and you get "No ROMS found!" or a garbage-
ROM crash on both games instantly. Fixed: `.append` is now explicitly captured
at the start of `.rodata`, and the linker-generated `.v4_bx` veneer section is
captured in `.text` (both were the only allocated orphans). The invariant is now
deterministic under every toolchain. (Same veneer capture added to gba_mb_my.ld.)

### 2. font2.lz77 header fix
The compressed stream actually decodes to 2756 bytes but the header declared
2752 - a malformed final block. Accurate BIOS handling (mGBA) flags it as
"Improperly compressed LZ77 data ... may crash on hardware". Header now says
2756; warnings gone. This was a latent bug in the original tree.

### 3. bindata.s alignment
`.align 2` before font and fontpal: the BIOS LZ77 call needs a word-aligned
source and memcpy32 needs aligned fontpal - both were aligned only by luck.

## Verified this session (in-sandbox, mGBA 0.10.2 accurate core + VBA)
- The build BOOTS AND RUNS under mGBA's accurate CPU core (title renders,
  main loop alive) - the earlier "mGBA shows blank" finding was stale.
- Rigorous eliminations for the devkitARM crash: ARM/Thumb interworking is
  clean in BOTH directions (all 89 bl_long targets + 141 jump-table function
  pointers are ARM; all Thumb->ARM veneered asm targets return with bx lr);
  IRQ stack overflow ruled out empirically (handlers run in SYSTEM mode on the
  ~3KB user stack, measured ~0x190 deep at vt_chr4_assemble entry; the 160-byte
  IRQ stack only holds 16-byte dispatcher prologues); -z muldefs duplicate
  symbols resolve identically in both link orders; builder.py == my injector.

## Known remaining issues (not the crash; next targets)
- Tile-arrangement scramble + shaking + stuck first-note audio on both games
  (reproduces under accurate mGBA - real emulation bug, now debuggable with
  mGBA's CLI debugger in-sandbox).
- Star Ally blue planet bottom third still mostly unrendered; gameplay needs
  per-tile EVA and VT sprite work.
- Two parked oddities seen in mGBA logs: a stray "SWI F0" executed right after
  the font decompress (no such instruction exists in the disassembly - harmless
  on emulators, hardware landmine); a one-time read of 0x0000FFFA-FFFD (6502
  vectors through an untranslated base) at startup before mapping.

## Earlier fixes still in place
1. Extension-mode CHR addressing (ppu_vt.c) - made Star Ally's title render.
2. Overlay timing: vt_chr4_rebuild_if_dirty moved after run_palette (ppu.s).
3. Palette write offset truncation fix - full 0x00-0x7F scattered palette (ppu.s).
4. Palette backdrop mirror (ppu_vt.c).
5. VT timer ticks per scanline-equivalent instead of 114-cycle units, IRQ flood
   bounded (vt_regs.c) - candidate fix, unverified by ear.
6. Optional VT_DBG_AUTOPLAY input injector (io.s), inert in normal builds.


## SESSION 3 ADDENDUM - the blank-screen / stuck-audio root-cause chain

Root-caused via mGBA debugger + descrambled 6502 disassembly of Lonely
Island itself (the PRG is opcode-scrambled: submapper 15 = bit5<->6 swap;
operands raw):

1. LI is a 16-colour game after all ($2010=0x0E incl. BKEXTEN) -- the old
   "baseline-only" belief came from Furb hanging before LI ever got there.
   The overworld tile scramble is therefore the missing per-tile EVA in the
   BKEXTEN path (known deferred work, now promoted to the identified cause).
2. The post-overworld blank: the game deliberately disables NMI for a
   transition and blocks on a flag ($06FD) that only its TIMER-IRQ handler
   advances (RAM-vectored via JMP ($003A); the IRQ epilogue acks $4103 and
   restores $2006/$2005/$2000 -- the timer IRQ is also LI's raster-split
   engine). A 142-event trace of vt_reg_write over 17.9s shows LI NEVER
   writes $4101/$4102/$4104 and strobes $4103 periodically; its only
   $4104-enable is an indexed STA $4103,X behind a mode flag. The game's own
   code is the spec: the VT timer free-runs from reset and $4103 is an ACK,
   not a disable. Furbtendulator models $4103 as disable + default-off and
   visibly hangs LI even earlier -- Furb is provably wrong here.
3. PocketVT additionally never ticked the timer at all: the tick lived in
   vt_adpcm_mix_gba, which only runs off the GBA TM1 cascade -- and TM0
   (sound sample timer) is only started on demand by DMC/PCM playback.
   For non-PCM games: TM0 off -> TM1 frozen -> mixer never runs -> VT timer
   dead AND the DirectSound path idle while the GBA PSG holds its last tone
   (the literal "stuck first note").

### Fixes landed
- vt_timer_tick_frame(): unconditional once-per-frame tick called from the
  vblank handler in ppu.s (262 scanline-equivalents, wraps collapsed into
  one IRQ assertion). The old in-mixer tick is removed.
- Reset state: timer IRQ enabled, period 0 (= 256-tick wrap ~ 1/frame).
- $4103 = ack-only (pending IRQ cleared, enable preserved).
- Boot-safety gate: timer IRQs are only delivered after the $2001 shadow
  has shown rendering enabled once since reset (latch at vt_timer_render_seen;
  ppuctrl1 shadow = GLOBAL_PTR_BASE+0x45A). Without this, the very first
  frame's IRQ went through LI's not-yet-initialised JMP ($003A) vector and
  crashed the 6502 (verified: r9=0xFFFF8000).

### Verified after the fixes (mGBA accurate core)
- Timer IRQs are produced, delivered, and HANDLED: LI's producer advanced
  $06FD from 0 to 0xE0 (it was frozen at 0 forever before), the 6502 stays
  healthy, the main loop idles correctly in VBlankIntrWait.
- Star Ally title screen: no regression.
- LI now progresses past the original hang but parks in a FURTHER NMI-off
  wait stage (screen still blanked by the game). Next step is to verify the
  timer IRQ cadence is continuous and trace the new wait the same way $06FD
  was traced.

### Still open
- LI's follow-on wait stage (above).
- Overworld tile scramble = per-tile EVA support in the BKEXTEN path.
- Audio beyond the PSG-hold explanation (DirectSound/TM0 start policy).


## SESSION 4 ADDENDUM - phantom IRQs, the 1.4 fps collapse, and major corrections

This session overturned several Session-3 conclusions.  Corrections first,
honestly: $06FD was never an IRQ-fed counter (it is a static target value,
0xE0, present even with the timer fully disabled -- the "producer ran!"
celebration was a misreading).  The "$4103 = ack-only" model was wrong and
is reverted to the datasheet disable semantics; the evidence for it was an
artifact of a real bug elsewhere.  The "timer must free-run" theory is
likewise retired.

### Root causes actually found and fixed

1. PHANTOM IRQ DELIVERIES (emulator bug, mine).  PocketNES's CheckI does
   not re-verify wantirq -- callers guarantee it.  The VT gate added to
   frame_irq_handler (and the gated skips in the new vt_timer_handler)
   branched to CheckI WITHOUT asserting anything, so every gated frame-
   sequencer expiry vectored a phantom IRQ through $FFFE (verified:
   irq6502 entered with _wantirq == 0).  Lonely Island's IRQ stub then
   applied its zeroed raster-restore shadows ($2000 <- 0, $2001 <- 0),
   killing NMI and the screen.  Fixed: gated handler exits now re-check
   wantirq and fall through to _GO, stock-idiom style.

2. THE EMULATOR RAN AT ~1.4 EMULATED FPS (the master bug).  Measured via
   vblank_handler_0 cadence: one emulated frame per ~750 ms of real time
   through the intro, transition, and wait phases.  Everything previously
   read as "hangs" was a 44x slideshow: the overworld "animation" was
   ~11 frames, the input wait was reachable but glacial, and a 3-minute
   marathon covered ~15 emulated seconds.  Profiling (PC sampling) put
   96% of host time in the 16-colour CHR pipeline: vt_chr_sync_from_prg
   ran its full 8KB window copy AND marked all 512 tiles dirty (forcing a
   full inherited-2bpp-cache rebuild) EAGERLY on every banking/CHR write,
   and the level transition storms those writes.
   Fixed: vt_chr_sync_from_prg is now lazy (sets a pending flag); a new
   vt_chr_sync_flush() applies it at most once per frame from
   vt_chr4_rebuild_if_dirty (before its 16c-only gate, so plain-2bpp VT
   games still refresh), and skips wholesale when the effective banks did
   not change.  Result: median vblank gap 750 ms -> 52 ms (~14x), uniform
   across phases.

### Engineering landed alongside

- VT timer ported onto PocketNES's cycle-accurate timeout system
  (vt_timer_timeout node in equates.h before TIMEOUT_END -- covered by the
  timeout_reset memset, so it initialises not-in-queue correctly;
  vt_timer_handler + vt_timer_install_now in sound.s; armed from
  vt_reg_write in-core context; rephased on $4102).  Mid-frame phase is
  now correct when games do use the timer.  $4103 = disable + ack
  (datasheet), default disabled at reset.
- _Static_asserts lock the VTState field offsets the asm relies on.
- VT_DBG_AUTOPLAY input synthesizer exists in io.s (off by default).

### False leads retired with evidence

- $4119 status-read gate: unwired $41xx reads return the address high
  byte (0x41) via empty_R; 0x41 & 0x18 == 0, so the game takes the good
  path already.  (Wiring vt_reg_read into the PPU read hook remains nice-
  to-have; precedent: map163 patches empty_io_r_hook.)
- $2002 vblank-flag/suppress machinery: correct; the game does not even
  poll $2002 in the relevant loop.
- MMC3-style timer aliases ($C000/$C001/$E000/$E001 -> VT timer regs)
  exist in vt_mmc3_forward, but LI contains no absolute-mode writes to
  them.
- The 6502 bank map "corruption" (r9 = 0x0600xxxx) was intentional
  design: the two fixed PRG banks are stashed byte-exact in VRAM
  0x0600C000-0x0600FFFF.

### Where it stands

Lonely Island now runs at ~19 fps (3x slow) with the early phase visibly
intact.  After the transition it parks on a sky-blue (#39bdff backdrop)
scene that does not advance with elapsed time or Start/A -- the exit
condition is game logic still to be traced (the toolkit for that is
proven: guest-PC fetch-watchpoints, mGBA trace with r9 extraction, NMI
cadence counting).  Remaining 3x overhead is the unconditional per-frame
16c copy_to_vram + palette + 2bpp-cache fight; the known next step is to
stop feeding the inherited 2bpp tile cache in 16c mode instead of
overwriting its output every frame.
