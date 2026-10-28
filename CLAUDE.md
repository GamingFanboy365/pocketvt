# PocketVT -- handoff for Claude Code

PocketVT is a fork of PocketNES (GBA NES emulator) extended to run VT02/VT03/VT09
"OneBus" famiclone ROMs (NES 2.0 mapper 256 and friends).
**Read MAINTAINERS_GUIDE.md sections 60-73 before touching the PPU**: they record
what is proven, what was disproved, and why. Sections are numbered; newest last.

## Working agreement
- Do not ask to test or debug. Verify in-sandbox, ship complete trees.
- Honest tone: say what is unverified; correct earlier claims explicitly.
- Every session: ship playable .gba files plus the source tree.

## Build
```
LIBGBA=/path/to/libgba bash build_pvt.sh   # -> ../pvt_build/pocketvt.gba (core only)
EXTRA_CFLAGS=-DFOO BUILD=/some/dir bash build_pvt.sh   # variant build
tools/restage.sh [builddir]            # ALWAYS, right after build_pvt.sh
python3 builder.py rom1.nes [rom2.nes ...]   # run IN the build dir -> play_me.gba
sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make  # devkitARM build -> ./pocketvt.gba
```
Toolchain: gcc-arm-none-eabi, libnewlib-arm-none-eabi, libgba headers
(git clone https://github.com/devkitPro/libgba). Harnesses use libmgba (`libmgba-dev`).
**Docker `make` is incremental and does not rebuild .s files when config.h
changes** (no asm header deps): `sudo rm -rf build` first after touching any
define (guide s.79g).
The repo-root pocketvt.elf / pocketvt.gba are committed and come from the Docker
build; CI (.github/workflows/build.yml) rebuilds and commits them on every PR, so
after pushing, pull before committing again. The Docker build uses a different GCC than build_pvt.sh: its core is NOT
byte-comparable with a build_pvt.sh core. Compare within one path only.
`EXTRA_CFLAGS` reaches the assembler too (`.s` files use `#if`).

## Reference emulator: furb_cli + compare_furb.py (USE THIS instead of asking for screenshots)
Furbtendulator (the reference) builds as a headless Linux CLI from the gitignored
source in reference/Furbtendulator-main (tools/furb_cli/README.md):
```
apt install g++-multilib libmgba-dev; pip install numpy pillow
python3 tools/furb_cli/build.py                  # -> tools/furb_cli/build/furb_cli
python3 tools/compare_furb.py testroms/Scramble.nes --at 300,700 --input "320-325:Start" \
        [--core ../pvt_build/pocketvt.gba]        # -> furbcmp/cmp_t*.png + report.json
```
Same ROM, same input, keyed to NES frames on both sides (PocketVT's `frametotal`),
so slow carts stay aligned. `struct` = palette-independent picture match; the
mismatch lines list disagreeing colour pairs (black<->colour = positional,
colour<->colour = palette). First results: Time Pilot 100%, Scramble title 99.97% / gameplay
99.0% (residue = the 1-px terrain edge, +-1 row; decimation or a real 1-row
offset, undecided), Push the Ball 97.5%, Add 'em Up 81.8% with the top strip
flagged (open item 1).
furb_cli alone dumps PPM + raw palette indices + $2000/$4100 registers + palette
RAM + CPU RAM per frame. Games can diverge over long runs; compare early frames.
It also does what the GUI does (s.76): FDS/NSF/VS, any setting (--set/--config),
any controller (--device, p1..p4:, key:, mouse:, trigger), WAV/AVI, savestates,
movies, cheats, DIP, --trace. `python3 tools/furb_cli/selftest.py` checks them.

## THE TRAP THAT HAS BITTEN SIX TIMES
`build_pvt.sh` **rm -rf's the build directory**, deleting builder.py, every .nes
and every harness binary. Afterwards builder.py silently reports
"Successfully compiled 0 game(s)" and every test runs a bare ~105 KB core --
which looks exactly like a catastrophic regression. **Run `tools/restage.sh`
after every build** (it copies builder.py, testroms/*.nes and every .nes in
`$PVT_ROMS`; `$PVT_HARNESS` = dir of harness .c files to rebuild), and check that
a play ROM is larger than the ~107 KB core. builder.py now exits non-zero when
it injected 0 games.

## Regression procedure (do this for ANY change)
Controls: Star Ally, Lonely Island, Scramble, Lucky Lawn Mower (VT09), VG Pocket.
1. Palette RAM + framebuffer at frame 700 vs the previous core -> byte-identical
   is the goal (LI, Scramble, VG normally are).
2. **Frame-SET test** over 800 frames: the set of distinct frames must be a
   subset of the old core's. Any code-cost change re-times Star Ally (a blink
   shifts phase), so "differs at f700" alone is NOT a regression if every frame
   it renders was rendered before.
3. Colours: compare_furb's `exact5` against Furbtendulator is the reference
   now (the user retired the gg.png/lawn.png photo checks, s.79h). Compare in
   5-BIT space (`>>3` both sides) -- mgba expands 5->8 bit differently.
4. `tools/compare_furb.py` on each testrom at a few frames: `struct` must not drop.
   (struct is keyed in 5-bit RGB since s.93; re-score the old core with the
   same tool, older numbers are not comparable.)
5. **Emulation speed = NMIs per 60 frames.** nmi_handler (timeout.s) increments
   the byte at `nmi_count` (read its address from the ELF; until s.97 it was
   the fixed 0x020007DF, which wrote into CHR_DECODE); read it before/after 60
   runFrame calls.

## Diagnostic habits that paid off
- Read symbols fresh after every link (`arm-none-eabi-nm pocketvt.elf`); never
  hardcode addresses -- they move.
- Measure before theorising. Most dead ends here came from reasoning off
  end-of-frame snapshots. Instrument the pipeline stage by stage (see s.72).
- `-DVALUE_SENTINEL` build: BG pixels encode their palette slot (s.58).
- Tile-partition matching against a reference identifies which ROM tile a
  screen cell uses independently of palette (s.68).
- `-DFORCE_BK_REPAIR`: if a BKEXTEN screen looks scrambled, a clean result
  here means a cache stomp, not a decode fault (s.69). `-DBK_STOMP_PROBE`
  counts slot pages that differ from a fresh assembly (vt_bk_probe), then
  ftwatch the first address for the writer (s.85b).
- tools/probes/: one-question mGBA harnesses (speed, frame hashes, VRAM/RAM
  dumps, live PC/lastbank, single-step watchpoints, stack depth). README there.
  gbaplay drives the L+R menu (GBA-frame keys), carries SRAM across runs and
  dumps state at exact NES frames: the savestate replay test (s.95).
- Speed A/B on a VT369 cart: key the input to NES frames (gbaplay KEYSN,
  cycprof KEYSPROF) and check correctness on -DVT369_SND_OFF builds, whose
  NES RAM and pictures must match byte for byte; with sound on the HLE runs
  on GBA time and the game state drifts (s.96a). cycprof's OPT/LB line says
  where the 6502 fetches its code from (VRAM 1 cycle a byte, cart ROM 4).
  cycprof charged each step to the instruction before it until s.96: older
  per-function numbers put a function's entry on the code ahead of it.
- Diagnostic hooks used this project: DMA_LOG (mapVT.s video DMA), TLOG /
  NMIDBG style counters; s.87 adds -DVT_NO_SPEEDHACK, -DVT_DIAG_NOVSYNC
  (frame end never waits) and -DVT_TIMER_LOG (VT timer expiry/re-arm ring);
  s.97 adds -DVT_NO_JMPSKIP (no "jmp *" skip in either JMP wrapper).
  The idle-loop finder samples the 6502 on GBA time, so a cart that installs
  hacks during a race (VG Pocket's game launch) can differ in NES RAM between
  two correct cores: confirm with -DVT_NO_SPEEDHACK builds (s.97e). Always build them into a SEPARATE build dir and
  verify the shipping tree is clean afterwards.

## Current state (after s.97)
See MAINTAINERS_GUIDE.md s.97 (five carts checked against s.96 on both
toolchains: the VT JMP wrapper runs from the IWRAM overlay (vto_jmp) and a JMP
speed hack passes other JMPs to vt_jmp_norm; nmi_handler's debug counter is
the symbol nmi_count, no longer a fixed address inside CHR_DECODE; the 2bpp
renderer marks the BG pages it writes (vt_bg2_stomp) and the 4bpp copy puts
them back in the same vblank, which also ends Table Soccer VT369's stale
"BRA" tile; eleven carts checked in all, s.97e), s.96 (Jumper 53.6 -> 59.5 fps: its code moved into
the VRAM PRG copy, "jmp *" loops skipped exactly, spriteinit from tables, an
IWRAM overlay over dead mapper code for the opcode-table toggle, the VT369
sound decode and the frame-end patch; op_table copied back at every cart
load; nested sound fills queued; the IRQ stack at 0x03007F00), s.95 (EZ-Flash Omega: doReset restores WAITCNT 0
before the flash-cart reset; savestates: buffers inside the heap, a VT-safe
redecompress, the VTST block of VT machine state; the menu font on 8bpp
VT369 pictures; the label "PocketVT 1.0"), s.94 (the UMC UM6578, NES 2.0 mapper 405, internal
251: Star Ally UM6578 runs through src/um6578.c + src/Mappers/mapUM.s with its
own BG tile cache, per-line scroll segments and Furbtendulator's UM colours;
mapper 405 was never a VT variant), s.93 (Soccer 2009: $A000 mirroring applied at once;
$4119 from the header's region with the NTSC VT03 palette-DMA offset; COLCOMP
with SPEXTEN sprites; eight 2 KB extension sprite slots; VT03 COLCOMP colours
are Furbtendulator's, tools/gen_vt03_lut.py; compare_furb struct keyed in
5-bit RGB; MMC3 bank fast path and scrub chunk signatures), s.92 (find_idle_loop: a 6502 simulator speed-hacks
idle loops that JSR and read the pads -- Aero/Hex titles 60; the VT369 has no
DMC, so $4010/$4015 bit 4 are filtered once $2010/$201E is set -- Funny
Coins' IRQ storm; update_sprites' OAM writes redirected while VT369 enhanced
mode is on -- Zuma's top row at the slow timing), s.91 (flash carts: WAITCNT is set by a start-up
read test, vt_waitcnt_probe, not blindly; SELECT at power-on keeps the slow
timing; SRAM_V marker), s.90 (Sky Fighter's black line: the VT369 sound
refill delayed the vblank HBlank DMA set-up; the render now waits for the end
of a vblank handler), s.89 (VT369 slowdown and flicker: the GBA OBJ line
budget, reference-counted sprite slots, palette and nametable dirty marks,
the writeBG hook, nametable DMA, $4107/$4108 and the divider in vt_w4_bank,
the poll-loop finder's back-up), s.88 (Jumper and Sky Fighter: VT369 CPU x3 by
patching the op-cost immediates, $2007 CHR reads, VT369 sprite-0 hit, NES
OAM layout and 8bpp sprites, the Cross River tile rule, BG tiles shared by
content, $4169 toggles without a table rebuild, poll-loop speed hacks), s.87 (frame pacing with vblank credits; VRAM fast
path for $4107/$4108 and the multiplier; Star Ally's vblank-overrun glitches;
VT timer reload), s.86 (Zuma: VT369 DMA low byte $4024, 16-row
sprites from $2000 bit 5; speed hacks under every opcode encryption), s.85 (VT369 start-up stall; Jewel Master VT03 sprites
and BKEXTEN slot checksums), s.84 (VT369 sound CPU HLE; corrects s.83), s.83 (bank switching), s.82 (VT369 enhanced picture, $3000 nametable window),
s.81 (VT369 $6000 ROM, 15-bit palette), s.80 (VT369 CPU side), s.79 (Table Soccer, VG colours, real BIOS, wait
states, sound) and s.77-78 (raster-split slots, speed).
- Working: Star Ally, Lonely Island, Scramble, Lucky Lawn Mower VT09, VG Pocket
  (all 50; colours now Furbtendulator's), Push the Ball, Time Pilot, Add 'em Up,
  Aero Gyrodine, Hex City X, Table Soccer VT03 (mapper 419; menus/match 96-99%),
  Jewel Master VT03 (256.15, BKEXTEN + 16-colour sprites; title 99.9%),
  Table Soccer VT369 (non-enhanced renderer; select screen 99.9%, match 96.7%),
  Lucky Lawn Mower VT369 (99.7%), Jewel Master (99.9%) and Fire Fighter VT369
  (96%; game-state drift) through the enhanced renderer, src/ppu_vt369.c.
  Zuma (VT369, 256.13; title 99.6%, gameplay 98% -- its ball colours are
  random, like Jewel Master's jewels), Jumper (VT369 SMB hack, 99.9%; 59.5
  fps mean, 54 at a level reload, s.96), Sky Fighter (VT369, gameplay 96-98%, 60 fps), Funny Coins (VT369,
  title 100%, gameplay 98% to NES 1800, 60 fps; 1-line streaks at its
  raster-split edges) and Soccer 2009 (VT03 with a 256K CHR ROM, 256.0;
  menus 99.5%, matches 97-99% with exact colours), Star Ally UM6578 (UM6578,
  405; title 100%, gameplay 98-99% where the game state has not drifted,
  54-61 fps).
- CPU x3 ($411C bit 7, every VT369 test cart sets it): vt_cpu_x3_set
  rewrites every `cyc`-recorded cost immediate in RAM (section vt_cycpatch,
  6502mac.h) between 3 and 1 dots a cycle. New cycle charges in RAM code
  MUST use the `cyc` macro or `fetch`; ROM handlers keep 3 dots (VT extra
  opcodes), NMI entry and OAM DMA read vt_cpu_x3.
- VT369 sound CPU: high-level, src/vt369_snd.c (programs $40AE and $0293 are
  exercised; host test tools/probes/vt369snd_test.c must report 0 failing).
  Starts are rising edges; the per-sample loops are ARM in IWRAM (the space
  apack.s gave up). DirectSound B while it runs: two DMA buffers, timer 0
  restarted in vt_adpcm_mix_gba. Misc ROM at $1000, GPIO $4140-$415F.
  furb_cli dumps sound RAM as `.snd`; `-DVT369_SND_OFF` turns the HLE off.
- VT369 enhanced mode ($201E != 0) owns BG VRAM, the tilemap, the GBA palette
  and OAM (vt369_enh); PocketNES's BG writers are off via vt_bkexten_live.
  8bpp BG tiles may use 0x06008000+ after vt_prg_evict (s.77a).
- vt_palette_ram holds FULL bytes (VT369 colours are 15-bit); VT03/VT09
  readers must mask & 0x3F. On VT369 vt_reg_write ignores $4140-$41FF.
- $4119 reads vt_4119 (loadcart.c): $00 NTSC, $18 PAL/Dendy from NES 2.0 byte
  12, $00 on VT32/VT369, $18 with no console type. An NTSC VT03 lands a video
  DMA's palette bytes one entry early (vt_pal_dma_shift; games DMA to $3F01).
  That case has its own copy of the DMA loop: per-byte work added to the
  shared loop exposed the stale tile cache on Table Soccer VT369 (s.93).
- VT03 COLCOMP colours: src/vt03_palette_lut.h is generated by
  tools/gen_vt03_lut.py (Furbtendulator's NTSC model, VT03Palette=0; --check
  against furb_cli --dump-palette). The 64 standard colours are the s.57 DACs.
- Raster-split slots: 3 (block 2, block 3, and the primary's unused pattern
  half when free), tallest bands first, 4bpp and 2bpp, MMC3 CHR writes mark
  bands too. `-DVT_SPLIT_SLOTS=0` turns them off.
- VT timer: N+1 lines free-running, N+2 after an in-picture rephase, N from
  vblank (`VT_TIMER_NPLUS1`); split lines match Furbtendulator exactly.
- WAITCNT = 0x4317 (`VT_FAST_WAITCNT`): 3/1 + prefetch, set by vt_waitcnt_probe
  (gba_crt0_my.s, EWRAM) only if the cart reads back the same at that timing
  (s.91); measure speeds on the normal path (vt_waitcnt_mode 1). Speed (NES fps):
  Time Pilot 57-60, Scramble 60, SA 60 (47 in its heaviest stretch), Aero/Hex
  60 (titles too since s.92), Table Soccer VT03 60 (s.88), LLM VT09 60, LI/VG/Add 'em Up 60,
  Soccer 2009 menus 60 and matches 42-44 (CPU-bound: no idle time to skip, s.93);
  VT369 (all at CPU x3): Table Soccer, LLM, Zuma, Jewel Master, Sky Fighter
  60, Fire Fighter 54-60 in gameplay, Jumper 54-60 (s.96).
- Frame pacing (s.87a): every GBA vblank is a credit, every NES frame spends
  one; the frame end waits only without credit (vt_vsync_ahead). Speed probes
  can show 61-62 while a slow stretch is made up; the average stays 60.
- The vblank handler must not run into the next frame (s.87c): BKEXTEN map
  work runs in vt_bk_late (from vrom_update_tiles) after the HBlank DMA
  set-up, and run_palette skips itself while vt_pal_owned. Anything heavy
  added before the DMA set-up brings back shifted frames and grey lines.
- writemem_4 on VT carts (not 419) is write_vt4xxx_v in .vram1 (s.87b);
  ROM handler in vt_w4_next = vt_w4_bank ($4107/$4108 and $4136/$4137, s.89c),
  then vt_w4_slow. Write handlers may clobber only r0-r2 and addy: r3 is
  m6502_nz. `.vram1` ends at 0x06003FF0: 16 bytes left.
- The VT369 sound refill (~55 scanlines) must never run between a vblank
  handler's entry and its HBlank DMA set-up (s.90): vt_adpcm_mix_gba defers it
  to vt_16c_palette_fixup while `_inside_gba_vblank` is set. Measure the set-up
  line with bpcount PEEK=04000004 on vblankinterrupt and vbl5.
- VT369 enhanced mode patches writeBG_mapper_9_mod (ppu.s) into a B to
  vt369_nt_mark (`.ewram`) to mark nametable writes (s.89b); nametable video
  DMA goes through vt369_dma_nt. readmem_0 must keep jumping to ram_R_mask:
  loadcart.c patches it for 4K RAM (s.89e).
- Speed hacks: the finder and set_cpu_hack decode opcodes through vt_op_dec
  (filled by vt_rebuild_optable) and patch op_table at the RAW byte, for any
  encryption submapper (s.86d). They run from ROM. find_idle_loop (s.92)
  reads A/X/Y from vt_isim_regs (stored by the ROM stub
  speedhack_manager_regs), keeps all its state in EWRAM, and is rate-limited
  with back-off: running it every attempt cost Sky Fighter 4-7 fps.
- UM6578 (s.94): mapUMinit patches two literal words in ppu.s
  (vt_frame_end_call, vt_vbl_fixup_call) to um_frame_end / um_vblank_stacked;
  cart.s restores them on every cart load, so VT carts run unchanged code.
  Its big tables (~53K, um_mem_size) sit above the ROM cache; the cart runs
  without the turbo PRG copy. Its BG cache owns char base 2 and screen blocks
  27-31. The 6502 reset writes $4017=$40 for it (frame IRQ off at power-on).
- Savestates (s.95): the VTST block (vt_state.h; vt_regs.c vt_state_io /
  vt_state_apply, with a VT_SV list in ppu_vt.c, ppu_vt369.c, vt369_snd.c)
  holds the VT machine state. ANY new VT register, RAM or chip state must be
  added there (bump VT_STATE_MAGIC on layout changes) or a loaded state runs
  with it at power-on. Caches are rebuilt, never saved. sram.c's BUFFER1/2/3
  are heap+0/64K/128K and must stay below ewram_heap_end (loadcart.c):
  state_buffers_fit refuses otherwise (UM6578: always). After an SRAM or
  state menu, redecompress on VT/UM carts repeats only init_cache's recorded
  PRG copies (rec_copy); VT vblank work is skipped while ewram_owner_is_sram.
  Check with tools/probes/gbaplay (FTDUMP replay; guide s.95).
- Menus: ui_palette_claim(1/0) (pocketnes_text.c) around ui() and the ROM
  menu; vt369_pal_upload leaves BG colours 64-95 (the font, banks 4-5) alone
  while vt_ui_pal is set.
- doReset (io.s) stores WAITCNT 0 before the flash-cart reset writes and
  SoftReset (s.95, as the PocketNES fork: the Omega's menu).
- memset32 (memcopy.s) is memset16 underneath: it only works for a value
  whose two halves are equal (the tail and any fill under 24 bytes use strh
  of the low half). Fill words with distinct halves with a loop (s.94).
- PRG page 0 lives in OBJ VRAM until the first VT sprite slot is written, then
  moves to its ROM/EWRAM twin (vt_prg_evict_obj). Never write OBJ slots 0-7
  without it.
- Hardware: build_pvt.sh cores now get a bootable header (tools/gbafix.py);
  every test ROM boots through the real BIOS (`compare_furb.py --bios
  reference/gba_bios.bin`, BIOS supplied by the user, gitignored).
- IWRAM overlay (s.96): the IWRAM code of konami.o, map023771.o, map1.o,
  map4.o and map9.o sits at __iwram_ovl_start-__iwram_ovl_end (1388 bytes);
  on a VT cart loadcart.c (iwram_ovl_select) puts src/vt_ovl.s there (1368
  bytes since s.97, 20 free on the devkitARM link; position-independent ARM,
  a link ASSERT checks the fit) and sets vt_ovl_swap / vt_ovl_dec48 /
  vt_ovl_fend / vt369_mixp / vt_ovl_jmp; any other cart gets the mapper code
  back. NULL pointers mean the C versions run. Nothing a VT cart runs may live
  in those five objects' .iwram. -DVT369_DEC48_CHECK runs the IWRAM and C
  decoders side by side.
- JMP abs (s.97): vt_patch_optable puts vto_jmp (the overlay's copy of
  op_vt_JMP_abs: VT09 commit + "jmp *" skip, then _4C) in op_table[$4C] and in
  vt_jmp_norm; op_table_reset sets vt_jmp_norm to _4C for every other cart.
  op_vt_JMP_absy (a JMP speed hack) sends the JMPs it does not hack there.
  Anything that installs another JMP handler must keep vt_jmp_norm in step.
- No fixed RAM addresses in code (s.97): the NMI counter at 0x020007DF sat in
  CHR_DECODE (or vt_isim, depending on the link) for its whole life.
- 4bpp BG pages (s.97): render_recent_tiles ORs the 2K pages each run wrote
  into vt_bg2_stomp; vt_chr4_copy_to_vram copies marked pages whatever its
  four probe words say, and vt_16c_palette_fixup_body repeats it at the end
  of a top-level vblank. VT carts never take render_a_kilobyte.
- loadcart.c copies op_table back from its load address for every cart
  (s.96): a VT cart's encryption leaves it permuted. VT369 carts whose code
  runs from one 32K of cart ROM get it copied into the VRAM PRG copy after 60
  NES frames (vt_prg_hot_check); the 8bpp tile slots evict that copy only
  when the 319 low slots are full.
- r10 is globalptr for the interrupt handlers: never a scratch register in
  code that runs with interrupts on (spriteinit, s.96d).
- The IRQ stack starts at 0x03007F00 (s.96f): each nested interrupt takes 40
  bytes (BIOS 24, interruptdispatcher.s 16); main nested four deep (160
  bytes, all it had) when a game restarts from the ROM menu. spmin KEYSG=
  measures it (mode 0x12).
- The IWRAM user stack is ~150 bytes above .bss; ALL new C globals go in
  EWRAM_BSS (`__bss_end__` must stay <= 0x03007B84; 0x03007AE0 build_pvt /
  0x03007AD0 devkitARM after s.97's 64 bytes of render_recent_tiles; the user
  stack's lowest point measured 0x03007BAC on Hex City X from power-on and
  0x03007C24 through a menu Restart). Heavy VT C runs on
  the 3K EWRAM stack. Measure with tools/probes/spmin from power-on.

## Open work, in priority order
0. s.97 checked s.96 on Aero Gyrodine, Hex City X, Lonely Island, LLM VT09,
   Star Ally VT03, Jumper, Push the Ball, Star Ally UM6578, Table Soccer VT03
   and VT369 and VG Pocket, on both toolchains (identical to main or better,
   speeds at or above main's). Scramble, Time Pilot, Add 'em Up, Jewel Master
   (both), LLM VT369, Fire Fighter, Zuma, Sky Fighter, Funny Coins and Soccer
   2009 are owed the regression set, compare_furb and their speed (s.97f).
1. VT369 leftovers (s.82): 8bpp 16-row sprites through $201D bit 2 are done
   but untested, hi-res mode ($201C bit 2), CPU x3 in ROM handlers (s.88d).
   Speed at x3: Fire Fighter 54-60 (s.89, before the s.96 overlay; not
   re-measured), Jumper's level reloads 54 (s.96g: tile-cache cell updates,
   ~530K cycles in the first reload frame, Thumb from ROM; the overlay has
   20 bytes free since s.97). Jumper waits on the sound CPU's tick counter $18F6, which
   the HLE advances per 128-tick fill (s.96g). Sound CPU: the $40AE per-channel rate
   divider, and the programs no test cart uses (s.84e).
2. Sound (s.79d): VT ADPCM $4120-$412F only mixes while NES DMC plays; the
   TK-8007 voice (Table Soccer) is not played; second APU $4020-$402F is
   absent. The APU plays ~2.2x louder than Furbtendulator's mix (s.84b).
   Measure with tools/probes/pvtwav + wavcmp.py (not for real-time streams:
   its envelope stretches for game speed, s.84b).
3. Stale BG tile cache (item 21): Push the Ball's 20 px; per-byte cost added to
   the video DMA loop made Table Soccer VT369 lose two bracket corners (s.93);
   EWRAM layout alone (s.93 core + 1600 B of EWRAM_BSS) re-exposes it, and
   s.94 leaves one stale tile on Table Soccer VT369's knockout screen (s.95:
   the select screen's Brazil label, 98.56% -> 98.54%). s.97 found two
   causes and fixed both: the NMI counter's fixed address wrote into
   CHR_DECODE (which entry depends on the layout), and 2bpp re-renders of
   4bpp pages that the 4-word probe of vt_chr4_copy_to_vram missed (now
   marked). Table Soccer VT369's "BRA" is whole again (98.59% on both
   toolchains, s.97e); Push the Ball is at 97.49%. The VG band of
   missing tiles was a BKEXTEN slot stomp, fixed by the slot checksums (s.85c);
   the VG games at 96-98% (3/4, 3/5, 4/1, 4/2) still have a small residue.
4. Table Soccer formation screen: two 6-line strips need a sixth char block.
5. Funny Coins: 1-line streaks at the board's raster-split edges (s.92).
6. Soccer 2009 matches at 42-44 fps: the 6502 core alone is ~272K of the 281K
   cycles a frame; left over: sprite slot update ~15K, palette rebuild ~14K.
7. Star Ally UM6578: savestates refused (its 53K block leaves the heap too
   small for the savestate buffers, s.95); 8x16 sprites ~0.5 GBA line low
   (stock scaling); boot $2002 polling drifts game state against
   Furbtendulator (s.94). VT369 sound HLE runs on GBA timers, so a loaded
   state replays Funny Coins / Table Soccer VT369 with small RAM timing
   differences (s.95).
8. Scramble's shot (s.70c): one-pixel sprite on texture row 7 dropped by the
   sprite affine matrix.

## Reference material
LOCAL ONLY, gitignored, never commit (the user supplies them as test.zip):
- reference/nrs/: NintendulatorNRS OneBus sources (h_OneBus.cpp, OneBus.cpp,
  mapper256.cpp, ...). The authority on VT behaviour.
- reference/NESdev_Wiki-*.xml: NESdev wiki exports (VT02+ pages, MMC3).
- reference/gba_bios.bin: the real GBA BIOS (user-supplied) for
  `compare_furb.py --bios` and tools/probes/biosboot.
- reference/Furbtendulator-main/: the full Furbtendulator source (from
  Furbtendulator-main.zip); tools/furb_cli builds it headless.
- testroms/: Add 'em Up, Push the Ball, Scramble, Table Soccer (VT03), Time
  Pilot; supplied later: Aero Gyrodine, Hex City X, VG Pocket VT09, Lucky Lawn
  Mower VT369, Table Soccer VT369, Fire Fighter VT369, Jewel Master VT369,
  Star Ally, Lonely Island, LLM VT09, Jewel Master VT03, Funny Coins, Soccer
  2009, Star Ally UM6578 (UM6578 docs in reference/um6578/).

In git:
- DATASHEET_DIGEST*.md: VT02/VT03 datasheet notes. Bit numbering there is
  1-INDEXED; the code is 0-indexed. DATASHEET_DIGEST.md appendices A/B hold the
  spec-vs-code verdicts (no VT extra opcodes; NMI polarity; $410F).
- MAINTAINERS_GUIDE.md (s.74 maps the retired docs and their open items),
  CHANGELOG.md (0.1-0.5.2 only), README.md.
- Reference screenshots supplied (NOT in the tree; ask for them):
  gg.png / 1.png (LLM opening), lawn.png (LLM gameplay), vgp.png / vgp2.png
  (VG menus), aero.png, hex.png, add.png, add2.png, bug.png, shot.png.
