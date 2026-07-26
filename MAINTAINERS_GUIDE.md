# PocketVT Maintainer's Guide

This document is written so that ANY capable model or developer -- including
ones with less context capacity than the sessions that produced this code --
can pick up PocketVT, reproduce the working environment, verify the current
state, and continue the open work without rediscovering anything. Read it
top to bottom once before touching the code. Everything in here was learned
the hard way; the "gotchas" sections are not optional reading.

## 1. What this project is

PocketVT is a fork of Loopy's PocketNES (a NES emulator for the Game Boy
Advance, mostly ARM assembly with C around the edges) extended to run
VT02/VT03/VT09 "OneBus" famiclone ROMs. These are NES-on-a-chip systems with
extra hardware the NES never had: 4bpp (16-colour) tiles, extended CHR/PRG
banking through $2010-$201A and $4100-$41FF registers, an enhanced palette,
opcode scrambling on some carts, and DMA extensions. The authoritative
hardware references are the NESdev wiki XML exports the user supplies
(pages: "VT02+ Registers", "VT02+ Video Modes", "VT02+ CHR and PRG
Bankswitching", "VT02+ CHR Pattern Data Layout", "VT03+ Enhanced Palette",
"VT02+ Sound", and the MMC3 page) plus DATASHEET_DIGEST*.md in the tree.
When the wiki and this guide disagree, re-read the wiki: it is the ground
truth and every fix that stuck came from reading it exactly.

Test ROMs: "Lonely Island" (128KB, NES 2.0 mapper 256 submapper 15, opcode
bit5<->6 scramble) -- FULLY WORKING; "Star Ally" (256KB, same board) --
in progress; "Lucky Lawn Mower" (VT09) -- future work, untouched.

## 2. Environment: exact recipe

The sandbox resets between sessions. Rebuild it with:

    apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi \
        libnewlib-arm-none-eabi mgba-sdl xvfb imagemagick
    pip install py65 pillow --break-system-packages

libgba HEADERS (only headers are needed) come from
codeload.github.com/devkitPro/libgba (tarball). The source tree lives at
/home/claude/pocketvt; the newest complete tree is always the latest
pocketvt_src_sNN.zip in the deliverables. Build with:

    bash /home/claude/build_pvt.sh          # compiles into /home/claude/pvt_build
                                            # WARNING: rm -rf's pvt_build first,
                                            # so recopy builder.py and the .nes
                                            # files into pvt_build afterwards

Link, header-fix and package (run inside pvt_build):

    OFILES=$(ls *.o | grep -v '^gba_crt0_my.o$')
    arm-none-eabi-gcc -g -mthumb -mthumb-interwork -Wl,-z,muldefs \
        -T ../pocketvt/src/gba_cart_my.ld -nostartfiles \
        $OFILES gba_crt0_my.o -o pocketvt.elf
    arm-none-eabi-objcopy -O binary pocketvt.elf pocketvt.gba
    python3 - <<'EOF'          # GBA header checksum at 0xBD
    d=bytearray(open('pocketvt.gba','rb').read())
    c=sum(d[0xA0:0xBD]); d[0xBD]=(-(0x19+c))&0xFF
    open('pocketvt.gba','wb').write(d)
    EOF
    python3 builder.py "Lonely Island.nes" ["Star Ally.nes" ...]
    # builder.py appends the .nes files to pocketvt.gba -> play_me.gba

A one-game build boots straight into the game; two or more boot into the
PocketNES ROM menu.

TWO TRAPS THAT COST TIME IN SESSION 21 -- read before running anything:
  * builder.py takes ONLY .nes paths.  The core is implicit (it reads
    pocketvt.gba from the working directory) and the output is ALWAYS
    play_me.gba, which you rename.  Passing `pocketvt.gba` as the first
    argument does not "select the core": it treats the core as game #1, so
    you silently get a two-entry MULTICART that boots into the ROM menu and
    every probe you then run measures the menu, not the game.  Sanity-check
    by size: Star Ally single-game ~366KB, Lonely Island ~235KB; a build
    that is core-sized (~104KB) means the .nes file was not found, and one
    that is much larger means you built a multicart.
  * build_pvt.sh does `rm -rf pvt_build` first.  That deletes builder.py,
    the .nes files AND every play ROM you built earlier, so a "control vs
    fixed" comparison run right after a rebuild can end up comparing two
    ROM-less runs -- which of course match perfectly.  Re-copy builder.py
    and the .nes files after every build, and keep control ROMs somewhere
    outside pvt_build (e.g. /mnt/user-data/outputs).

## 3. Debugging cookbook (mGBA headless)

Everything is verified in-sandbox with mGBA's command-line debugger.
General shape:

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    /usr/games/mgba -d -C videoSync=0 -C audioSync=0 game.gba < script.txt

where script.txt is lines of debugger commands. The ones that matter:
"frame" (run one hardware frame, prints registers on pause), "break ADDR",
"watch ADDR" (write watchpoint -- prints the writing instruction's
registers; THE tool for finding memory stompers), "continue", "r/1 ADDR"
"r/2" "r/4" (read byte/half/word), "w/1 ADDR VAL" etc. (write), "quit".

Hard-won rules -- follow all of them:

RE-DERIVE EVERY ADDRESS AFTER EVERY LINK. Symbol addresses move. Use
`arm-none-eabi-nm pocketvt.elf | grep -w NAME | head -1` and GUARD AGAINST
AN EMPTY RESULT (an empty variable silently produced "break 0x" once and
invalidated a whole experiment).

mGBA will NOT inject GBA keys: writes to REG_KEYINPUT (0x04000130) do not
stick. To drive input, poke emulator-side variables instead: the
`vt_dbg_pad_or` byte (io.s hook) ORs a value into the NES pad each frame --
NES bits: A=$80 B=$40 Sel=$20 St=$10 U=$08 D=$04 L=$02 R=$01. To drive the
ROM menu, poke `selectedrom` / `rommenu_state` directly.

mGBA under `-d` with frame-stepping does NOT present a framebuffer:
debugger-driven screenshots come back black. For visuals, run REALTIME
(`-C videoSync=1`) under Xvfb and grab with `import -window root`; the
capture flakes sometimes, so retry and require a 240x160 bounding box. For
deterministic visuals, compile the input script into the ROM: build_ap.sh
sets -DVT_AUTOPLAY, which enables a frame-scheduled pad table in
vt_16c_palette_fixup (ppu_vt.c). build_ap.sh OVERWRITES pvt_build objects,
so rebuild normally afterwards.

Reading odd-address symbols: mGBA r/4 rounds DOWN and fabricates a phantom
shift. Use r/1 for byte variables. More generally: when a bulk dump looks
insane, suspect YOUR read alignment before the code -- one session lost
hours to a verifier reading a table two bytes early and declaring 960
correct map entries "all wrong".

Profiling: the frame-pause r15 sampling trick (hundreds of "frame" commands,
grep 'r15:') tells you WHERE the CPU sits -- but it is vblank-biased. A
constant r15 of 0x00000188 means the CPU is HALTED in the BIOS waking once
per vblank (that is the BIOS IRQ wrapper address). For real speed, break on
`refreshNESjoypads`, take 12 "continue"s, and divide consecutive "Cycle:"
deltas by 280896 (GBA cycles/frame): 1.00 = full 60fps. A guest NES frame
is 29780 6502 cycles.

Call-chain forensics: dump the user stack (0x03007B00-0x03007F00 by r/4)
and symbolize every word that looks like a code address against `nm -n`.
The region 0x03007700-0x03007B40 is DATA (the timeout.s event-handler
table), not stack. __sp_usr = 0x03007D60, __sp_irq = 0x03007E00.

## 4. Architecture map: who does what

Stock PocketNES files you will touch: ppu.s (the PPU: vblank handler,
nametable cache, sprite OAM builder, palette, scanline effects), timeout.s
(the frame engine: run(), the event queue, frame-end/dontstop exit),
io.s (pads, waitframe), cart.s (mirroring tables, banking helpers, per-
scanline BG0CNT fill), sound.s (APU + GBA DirectSound; TIMER 0 is the
sample-rate timer and TIMER 1 CASCADES off it), interruptdispatcher.s
(IntrMain: sets the BIOS IntrWait flags at [0x04000000-8] and dispatches
IntrTable; vblank/timer0/timer1 nest via System mode on the user stack),
loadcart.c (cart setup, scanline-buffer placement), rommenu.c (multicart
menu), ui.c (the L+R options menu -- note it stops TM0 on entry and
restores it on exit).

VT additions: vt_regs.c ($41xx shadow + MMC3-compat forwarding + $4034/$4014
video DMA), ppu_vt.c (everything VT-video: 4bpp CHR assembly, SPEVA sprite
extension, the 16-colour palette fixup, BKEXTEN background extension, the
autoplay hook), Mappers/mapVT.s (the $4100 write/read hooks, PRG banking,
mirroring apply, cartflags VS/SCREEN4 clearing), 6502_vt.s + the op_table
discipline for scrambled carts.

GBA VRAM layout (verified live, do not violate):
0x06000000-0x06001FFF  BG 4bpp tile data, PPU pages 0-3 (GBA tile idx 0-255)
0x06002000-0x06003FFF  UI text layer: its map at 0x2000, its font after
0x06004000-0x06005FFF  BG 4bpp tile data, PPU pages 4-7 (idx 512-767)
0x06006000-0x06006FFF  game tilemap, screenblocks 12-13 (2-screen mirroring)
0x06007000-0x06007FFF  screenblocks 14-15: free for 2-screen games; BKEXTEN
                       uses idx 896-1023 here as extra tile slots
0x06008000-0x0600FFFF  GUEST PRG WINDOW: NES $8000-$FFFF mapped here for the
                       6502 core. IMMOVABLE. (Verified byte-identical to the
                       ROM's fixed bank; the LI wait-loop hack address
                       0x0600E094 is guest $E094.)
0x06010000-0x06011FFF  SPEVA OBJ slots 0-3 (extended sprite pages)
0x06014000+            stock sprite cache slots 8-15

## 5. The five hardest lessons (violate these and you will burn a session)

Memory READ handlers (readmem_4 etc.) are entered with NO usable ARM stack:
answer inline and tail-jump `ldr pc,=IO_R`; never push, never call C.
Applying state changes that need a stack (mirroring, bank switches) is done
by raising a flag in the C write handler and consuming it in write_vt4xxx,
which does have a stack.

IWRAM is at a cliff: __bss_end__ must stay below ~0x03007B00. EVERY new C
global goes in EWRAM_BSS -- including `static` file-scope variables, which
otherwise silently land in IWRAM .bss. The .vram1 section is FULL: four
added instructions once overflowed the link by 0x10 bytes; put new logic in
C or another section.

The op_table is sacred under opcode scrambling: only speedhack rows that are
invariant under the bit5<->6 swap (raw $10/$70/$90/$F0) may be patched.

The vblank IRQ handler's stack is tiny and canary'd (alarm = grey/white
flashing, fatal = DEADBEEE). Long call chains or big memcpys inside the
handler will trip it -- and headless runs HIDE the flashing, so realtime-
verify anything that adds vblank work.

PocketNES's incremental BG cache is NOT a reliable feed for VT games: the
producer (writeBG in ppu.s) SELF-MODIFIES into `bx lr` when its 256-entry
ring fills -- a VT boot-time video-DMA screen blast does that instantly --
and the re-enable only happens on a whole-map redraw gated behind
bg_cache_updateok. Star Ally got ZERO cache entries in 420 frames. The
BKEXTEN path therefore sweeps the map continuously instead (see below).

## 6. What works today (do not regress; verify after any change)

Lonely Island, single-ROM build: 60.0 fps (frame cost 1.00), correct
palette, sprites, background, scrolling, audio confirmed by ear. The
regression suite, all headless: (a) walk test -- boot 600 frames, hold Up
220f / Right 60f / Up 60f via vt_dbg_pad_or, read 0x030006F4 expecting
0x72 -> 0x62 -> 0x72; (b) speed via refreshNESjoypads cycle deltas = 1.00;
(c) zero "Hit breakpoint"/"crashed" lines over 600 frames with a breakpoint
parked at 0x03002800; (d) visually, the sidescrolling stage (house node $0B)
has ZERO fully-black columns.

Fixes in force, chronologically: encryption-gated op_table (s6); palette
write path + 4bpp OBJ overlay (s7); IWRAM relocation (s9); joycfg bit30 --
the famiclone pad is NES PLAYER 2, games read $4017 (s10); encryption-aware
speedhack + VT03 compat palette from the wiki chart (s11); the $41xx READ
hook -- $4119 is RS232 Flags, bit3 XPORN/bit4 XF5OR6, a PAL/50Hz probe that
selects the game's palette-DMA target (s12); change-detected BG VRAM copy,
THE 60fps fix -- and a page whose probe word is 0xFFFF is NOT skippable
(s13); SPEVA -- OAM byte2 bits2-4 are the LOW THREE BITS of the sprite CHR
bank, effective bank = (page_bank<<3)|EVA (s14); real mirroring -- $4106
bit0 (and MMC3 $A000 bit0) select the arrangement, and mapVTinit must clear
SCREEN4+VS from cartflags because the loader misreads NES 2.0 flags7 as VS
Unisystem and mirrorchange force-overrides to four-screen (s15); the
rommenu joycfg gate (s16); resetSIO preserving bit30 (s17); everything in
section 7 (s18); PIX16EN 16-wide sprites (s20b4); the VT mirroring guard --
loadcart must NOT re-apply the iNES header mirror bit on a VT cart, because
the arrangement comes from $4106 whose power-on default (side by side) is
what a OneBus game relies on, and dump headers carry a meaningless flag
(s21b2); the 2bpp SPEVA path -- SPEXTEN alone enables extension addressing,
SP16EN only picks the fetch width (s21b3, section 8b).

Star Ally regression suite, all headless (route: Start f200, Up f400-620,
Right f630-690, Up f700+): (a) f700 gameplay frame shows starfield,
playfield and the HUD strip with NO orange band, and the live vram_map
matches the m0101 (side-by-side) table; (b) EVA sprite OBJs are present in
the $1A stages -- 14+ at f700 with tile indices below 512 -- and every live
EVA slot matches an offline recomputation from the .nes byte for byte;
(c) title f80 and menu f150 framebuffers unchanged from the previous build;
(d) $2010 still reads $1A at f1400 (the game has not died back to the menu).

## 7. Session 18: BKEXTEN background extension -- design and state

Star Ally runs $2010 = $1F: PIX16EN(b0), BK16EN(b1), SP16EN(b2),
SPEXTEN(b3), BKEXTEN(b4). Lonely Island only ever used $0E, so b0/b4 were
never implemented. BKEXTEN is now IN; PIX16EN is NOT (see section 8).

The hardware rule (wiki "VT02+ Video Modes"): with BKEXTEN set, a
background fetch's CHR bank becomes (page_bank << 3) | EVA where EVA =
P,A1,A0 -- A1:A0 are the cell's attribute bits and P is $2018 bit3 (BKPAGE)
when $2011 bit0 (EVA12S) is 0, else $4106 bit0 (HV). Because the attribute
bits become tile-address bits, the palette set of every background pixel is
FORCED TO ZERO.

Implementation (all in ppu_vt.c unless noted): the same nametable cell can
now show a different tile per attribute value, so GBA tiles are allocated
in 64-tile SLOTS keyed by (page,attr). Ten slots exist at GBA tile indices
{0,64,128,192, 512,576,640,704, 896,960} -- the free regions of the VRAM
map in section 4. vt_bk_slot_get() allocates (approximate LRU);
vt_assemble_page_to() renders a slot from ROM using the exact s14 SPEVA
pixel math; slot keys are stored BIASED (+1, so 0x00 and 0xFF both mean
empty) because SOMETHING in the inherited engine zeroes the first bytes of
the table -- stomper never identified, the bias makes a stomp self-heal.
vt_bk_write_cell() computes a cell's entry: tile = slotbase + (name & 63),
palette nibble 0; it reads the true attribute from the NES attribute table
and skips the VRAM write when the entry already matches. A 32-entry
(page<<2|attr) -> slotbase LUT (vt_bk_lut, refreshed per pass) keeps the
hot path allocation-free.

Feeds: (1) the ppu.s BG-cache consumer and display_whole_map branch to
vt_bk_consume()/vt_bk_whole() when vt_bkexten_live is set (hooks are in
ppu.s, clearly commented); (2) because the cache is unreliable (section 5),
vt_bk_scrub() rewrites one eighth of both nametables every vblank --
any upload path (per-$2007, $4034 video DMA, stack blast) is on screen
within 8 frames; (3) vt_bk_banks_recheck() reassembles slots in place when
page_bank / BKPAGE / HV / EVA12S change; (4) vt_bk_frame_check() re-renders
any slot whose first VRAM word was stomped by the legacy 2bpp cache.
Mode transitions live in the $2010 write case; turning BKEXTEN on DEFERS the
first whole-map rebuild to the vblank (vt_bk_pending_whole) because the
slot allocator is not reentrant against the vblank consumer -- running it
from the CPU write path produced three slots all claiming the same key.

Also fixed this session, and it matters for EVERY raster-split game: the
per-scanline effect tables are double-buffered pairs flipped each vblank,
but only one side of two pairs was ever initialized -- _dmadispcntbuff came
from the stale DISPCNTBUFF2 equate and _bg0cntbuff/_dmabg0cntbuff from
nothing at all. After the first flip, the $2001/mirroring mid-frame writers
(ctrl1finish in ppu.s, ubg2 in cart.s) memset16'd through garbage pointers
straight over the tilemap. Star Ally's HUD split triggered it every frame;
Lonely Island never writes those registers mid-frame, which is why it
survived. loadcart.c now gives the orphaned sides real EWRAM storage.
Diagnostic that found it: `watch 0x06006000`.

Verified: the full-screen map encoding is correct against an independent
recomputation from NES_VRAM2 (attr0/1/2 -> slot bases 0/64/128); the title
screen's band structure matches the reference star.png in 6 of 8 bands
including the purple horizon. LI regression is fully clean.

Open residuals, in priority order, with everything known:

(a) SPEED: SA now 20.6 fps (was 40.6 before BKEXTEN, dipped to 12.2 before
the LUT). The scrub (240 cells/frame in Thumb C from EWRAM) is the main
cost. Options: compile the scrub ARM into IWRAM; shrink to 1/16 per frame;
skip scrub entirely on frames where a "nametable possibly changed" flag is
clear (set it from vmdata_W's NT branch and the DMA path -- cheap OR into
an EWRAM byte); profile first with the refreshNESjoypads method.

(b) LATE CRASH: the newest build shows "The game crashed!" (the emulator's
crash screen) within 600 frames of SA where earlier builds ran clean.
Introduced somewhere in the LUT/scrub-optimization patch OR exposed by
timing shifts. Bisect by rebuilding with vt_bk_scrub() body disabled, then
with the LUT bypassed. Suspects: vblank overrun tripping the canary
(realtime-verify: greyish flashing = alarm), or SA's own attract mode
reaching a path that was previously never reached at 12fps.

(c) EARTH DENSITY: bottom two title bands are blacker than the reference.
BG palette bank 0 is verified rich, so suspect pixel-value-0 transparency
vs the reference's dark-blue backdrop entry, or the compat-palette mapping
of set 0's entry 0. Compare per-pixel against star.png once sprites exist.

## 8. Next up: PIX16EN sprites -- the full specification

With $2010 bit0 SET (and SP16EN set, as on SA), a sprite fetch still reads
4bpp data but each pixel's four bits are split: THE LOWER TWO BITS ARE THE
LEFT EIGHT-PIXEL HALF, THE UPPER TWO BITS THE RIGHT HALF -- sixteen pixels
wide, four colours, classic NES sprite palettes ($3F10 + pal*4). SA's
$2000 = $A0 (bit5 set) so sprites are 16x16.

Implementation plan, precise: (1) in vt_spr_eva_update, when bit0 is set,
switch the assembler to a variant that emits TWO GBA tiles per VT tile
(left pixel = v & 3, right = (v >> 2) & 3) in PAIR order so a 16x16 OBJ's
four tiles are consecutive for 1D mapping: for VT pair (t even, t|1), emit
[TL(t), TR(t), BL(t|1), BR(t|1)]. A page becomes 128 GBA tiles = 4KB, so
OBJ slots move to 0x06010000 + slot*4096, four slots max. (2) in ppu.s
update_sprites (BOTH sites -- the 8x8 site masks tile with 0x3F00, the 8x16
site with 0x3E00, each already has the vt_spr16_active SPEVA redirect
block), when a new vt_pix16_active flag is set: OBJ tile index becomes
slotbase + (tile & 0x3E)*2 (the existing code computes slotbase + (tile &
0x3E), so it is a one-instruction lsl); the OBJ shape/size template must
become square/size1 (16x16) instead of vertical 8x16 -- find where the
attr0/attr1 template bits (the r6 "rot/scale bits" and the shape field) are
chosen per sprite-height mode and add the PIX16EN case. (3) palettes: gate
the 16-colour scattered OBJ palette rebuild in vt_16c_palette_fixup on
PIX16EN being CLEAR, and make sure OBJ banks 0-3 carry the classic 4-colour
sprite sets (the stock path's format) when it is set. (4) verify against
1.png: the player ship mid-screen, the HUD digits, no vertical strip of
garbage sprites at the left edge.

## 8b. PIX16EN (s20b4) and the 2bpp-EVA gap (CLOSED s21b3)

PIX16EN is IMPLEMENTED and verified. In vt_spr_eva_update, when $2010 bit0
is set the sprite assembler switches to vt_assemble_page_pix16_to, which emits
TWO GBA tiles per VT tile ([L,R], planes 0/1 = left half, planes 2/3 = right
half, both plain 2-bit values indexing the sprite palette bank 0..3) so a
16x16 OBJ's four tiles are consecutive for 1D mapping; slots stride 4KB at
0x06010000. ppu.s (both the 8x8 and 8x16 sites) reads a new vt_pix16_active
flag: OBJ shape becomes square/size1 (16x16) or horizontal (16x8) and the tile
index is doubled (lsr#7 instead of lsr#8). A format-flip invalidation clears
the EVA slot keys whenever pix16 toggles. Verified against 11.png by template
matching in GBA row space: the menu cursor distance improved 30.2 -> 22.6 and
the 16-wide letter pair "ON" 79.5 -> 36.7 vs the pre-fix half-width build; OAM
dumps confirm SQ 16x16 OBJs with even (doubled) tile indices; the $2010 probe
confirms the menu runs $1F (both flags set) and the flags correctly drop when
the game writes $1A. Residual distance is the same palette-rendering delta as
the established title metric, not a geometry error. The right-half pixel
mapping was corrected once during the session (an early attempt scattered the
right half to nibble bits 2-3 -> grey; the reference shows both halves in the
same palette family, so both are plain 2-bit indices).

THE 2bpp-EVA GAP -- CLOSED in session 21b3.

The gap: SA's gameplay/attract stages run $2010 = $1A, i.e. SPEXTEN(bit3)=1
with SP16EN(bit2)=0 -- 2bpp sprites that still take EVA addressing.  The old
gate required BOTH bits ((vt_reg_2010 & 0x0C) == 0x0C), so EVA was ignored
there and every sprite fetched from the base register instead of OAM byte2
bits 2-4.  That is why the ships and enemies were missing from every $1A
stage.

The fix, as shipped:
  * SPEXTEN alone enables the redirect.  SP16EN now selects only the FETCH
    WIDTH -- 4bpp EVA pages when set (menu, $1F, plus PIX16EN's half-split),
    2bpp EVA pages when clear (gameplay, $1A).
  * vt_assemble_page_2bpp_to() reads a 16-byte-per-tile page and emits plain
    2-bit values 0..3 into GBA 4bpp nibbles.  Bank composition follows the
    datasheet (p.11): in 2bpp the composed bank counts 1KB units, because
    the extra one-bit left shift applies only to 16-colour fetches -- the
    4bpp assembler's *2048 becomes *1024 here.  With SP16EN clear,
    vt_build_16color_palette deliberately leaves the OBJ banks as the legacy
    sub-palettes run_palette maintains, so 0..3 through the OBJ attribute's
    palette field is exactly the stock 2bpp convention.
  * vt_spr16_active is now a MODE, not a boolean: 0 off, 1 redirect every
    sprite, 2 redirect only sprites with EVA != 0.  Both ppu.s SPEVA sites
    collapse "mode 2 and EVA == 0" to off in three instructions using only
    r1, so eva==0 sprites stay on the stock bankbuffer path (this is the
    scoping the s20b4 prototype lacked).
  * Slot invalidation is keyed on the full format (2bpp / 4bpp / pix16), not
    just on a pix16 flip, since a slot's bytes mean different things in each.

Verification that matters, and one criterion that had to be retired:
  * Bit-exactness beats eyeballing.  Every live EVA slot at f700 was
    compared byte-for-byte against an offline recomputation from the .nes
    file (banks 25/26/27 = page 5, EVA 1/2/3): 0 of 2048 bytes differ per
    slot.  That checks the address formula, the plane conversion and the
    destination in one shot, and it does not depend on reaching any
    particular game moment.
  * Sprites appear and behave: 14-15 EVA OBJs on screen in the $1A stages
    where the control build shows zero, positions changing frame to frame,
    game still in $1A at f1400.
  * Title (f80), menu (f150) and Lonely Island (f400) framebuffers are
    BIT-IDENTICAL to the previous build -- the change is scoped to $1A.
  * RETIRED CRITERION: "guest NES state must match the control build".  It
    cannot be met here and it does not mean what section 8b assumed.  A
    throwaway build that does all the new scanning and assembly but
    publishes nothing (rendering therefore identical to the control)
    diverges from the control by the same 80-470 bytes of guest RAM as the
    real fix.  SA polls hardware whose value depends on real elapsed time,
    so ANY change in per-frame host workload shifts its RNG and counters.
    Use it as a canary for accidental writes into guest memory, not as a
    correctness test for rendering work; judge rendering by bit-exact data
    checks plus progression, as above.

## 9. The multicart hang (ROOT-CAUSED AND FIXED in session 21b4)

Symptom was: select a game in a 2+ ROM build, and ~20 frames later you are
back at the menu with the pad dead. It was never a menu bug. The globals
block and its offset table had drifted 12 bytes apart (see section 14), so
`sh_encrypted` physically occupied the macro address of `_dontstop`; every
`set_cpu_hack()` wrote `vt.encryption_active` over run()'s keep-running flag.
With an unencrypted cart that stored 0, and at the next frame boundary run()
took its single-frame return path, popping a stack frame `run(1)` never
pushed and branching through a garbage lr -> BIOS -> ROM entry -> silent
restart. Encrypted carts (Star Ally, Lonely Island) stored 1 and were fine,
which is why this looked multicart-specific for three sessions.

Fixed by repairing the layout drift (timeout.s + equates.h). Verified: both
multicart pairs launch and stay launched, and Time Pilot -- which restarted
eight times in 400 frames even as a SOLO build -- now boots once and runs.

## 13b. VT timer: $4101 counts AD12 transitions, not scanlines

$4101 bit 7 (TSYNEN) picks the count source: 1 = HSYNC transitions, 0 = AD12
high/low transitions.  AD12 only toggles while the PPU is fetching, so with
bit 7 clear the counter STALLS through vblank.  Scheduling expiries as
period * 341 dots of free-running timestamp counts vblank too and lands every
expiry ~22 lines early; in Star Ally that put the raster split above the HUD
text and painted the HUD page across the bottom of the screen (the "orange
band along the bottom").  sound.s now pushes any expiry that lands after
render_end_time on by one vblank, derived at runtime so it holds for PAL.
If a game ever sets bit 7, the plain scanline schedule is the correct one --
honour the bit rather than reverting this.

## 14. The globals block: storage and offsets must stay in lockstep

The IWRAM globals live twice: as labelled storage in the ordered `.data.NNN`
sections (`_dontstop: .byte 0`), and as offsets in equates.h's `_m_` list
that the `str_`/`ldr_` macros index off `globalptr`. Nothing enforces
agreement. Add a variable to one list only and everything after it shifts:
code using the macro and code using the label then touch DIFFERENT words, and
the next variable someone appends can land on top of a live one. Two such
defects were live simultaneously until session 21b4 (a VT timer triple with
no storage; sh_encrypted with no offset entry), and their combination caused
the multicart hang.

Rules: (1) every `_m_ name,size` gets matching storage `_name:` in the same
relative position, and vice versa; (2) run `check_globals.py <elf>` after
every link -- it compares each global's label address against
GLOBAL_PTR_BASE + offset and fails on any nonzero delta; (3) never reference
one of these variables by BOTH forms in different files without checking the
delta is zero first.

## 10. Reference: Lonely Island internals (for regression work)

Pad bits as the game sees them: A=$80 B=$40 Sel=$20 St=$10 U=$08 D=$04
L=$02 R=$01; current state at zp $00 (P1) / $02 (P2), pressed-edge $34/$36.
Map position at 0x030006F4 as YX nibbles, start $72; the sidescrolling
stage is house node $0B; route Up 220f, Right 60f, Up 60f. The game's idle
loop C5 26 A5 26 F0 FC sits at guest $E094 (VRAM 0x0600E094) and is
speedhacked. LI's iNES header lives at offset 0x184B0 inside any single-LI
.gba. Autoplay route frames: {400,0x10},{620,0},{630,0x80},{690,0},
{700,0x10},{760,0} -- starting before ~frame 400 is too early.

## 11. Session 19 addendum: two s18 regressions fixed

(a) Lonely Island "dashes everywhere": the s18 loadcart fix re-pointed the
orphaned scanline-buffer sides at fresh EWRAM -- AFTER reset_buffers() had
already run against the old pointers, so the replacement DISPCNT table held
zeros instead of the 0x0440-per-line idle fill.  The HDMA then showed the
UI text layer (BG2, a map full of dash glyphs) on alternate frames.  Fix:
loadcart.c calls reset_buffers() again right after the re-pointing.  LESSON
FOR ALL FUTURE WORK: headless walk/speed tests DO NOT catch display-path
regressions -- always capture the actual screen (realtime mGBA under Xvfb)
when anything near the display pipeline changes, and diff two captures a
few frames apart (an alternate-frame artifact shows up as a massive pair
diff; the clean map screen differs by ~0.6% from animation).

(b) Star Ally wild-PC crash (PC=0x049563DC, LR in palette RAM): signature
of the timeout.s event-handler pointer table (0x03007700-0x03007B40, which
sits directly BELOW the user stack) being trampled by nested vblanks during
long BKEXTEN passes -- the vblank handler re-enables IME early, so a slow
pass can be re-entered on the same stack.  Fix: all BKEXTEN vblank-side
work (the rebuild_if_dirty branch and vt_bk_consume) now runs with IME
saved/masked/restored, and the scrub dropped to one sixteenth of the map
per vblank (120 cells).  Verified: 1200 frames of Star Ally with zero
crashes; Lonely Island walk test and 60fps unchanged.  If a wild-PC crash
ever returns, bisect by disabling the vt_bk_scrub body first, and check
realtime for grey/white canary flashing.

## §12 — Session 20: the Lonely Island "dashes" and the unaligned-buffer class

Sessions 18 and 19 both misdiagnosed this bug (stale scanline buffers, then
BG2 glyphs). Session 20's investigation went through three further wrong
theories before the data settled it, so the mechanism and the method are both
worth recording.

**Root cause.** `EWRAM_BSS u8 vt_chr4_buf[]` had no alignment attribute; a
layout drift put it at 0x0200169A (address % 4 == 2). Every reader that casts
it to `const u32*` — vt_chr4_copy_to_vram, copy_to_vram_all, vt_obj4_overlay,
and all their change-detection probes — performed unaligned LDRs. ARM7TDMI
does not fault on these: it loads the aligned word ROTATED right by
(addr&3)*8 bits. At offset 2 that yields exactly `lo(row) | hi(previous
row)`, carrying across tile boundaries. The byte-store assembler was always
correct; only the word readers were poisoned. The probes compared
rotated-staging against skewed-VRAM — equal by construction — so the damage
was never repaired. On screen: every affected tile's right half (pixels 4-7)
shifted down one row; on Lonely Island's map that reads as a grid of black
"dashes everywhere."

**Fix.** `__attribute__((aligned(4)))` on vt_chr4_buf. Rule: ANY u8 buffer
read through u32*/u16* casts must carry an explicit alignment attribute.
Note LDM/STM behave differently (they force-align, no rotation), so mixed
codegen can produce two different corruption shapes from one root cause.

**Verification numbers (all vs the user's known-good s15 build, native
240x160, same emulator).** Broken: 5613 differing px (14.6%), 1710 dash px,
100% in tile columns 4-7. Fixed: 32 differing px (0.08%, water animation
rows only); 1707/1710 dash coordinates now match s15 exactly; walk test 1150
frames clean; guest speed parity 8/8 animation steps per 60 GBA frames.

**Method lessons (hard-won, do not relearn):**
- mGBA writes `<rom>.sav` beside the ROM and loads it on the next run:
  cross-process runs are NOT deterministic replicas. `rm pvt_build/*.sav`
  before EVERY harness run. Two days of contradictory comparisons in s20
  trace to this.
- Re-derive symbol addresses per link applies to YOUR OWN TOOLS: harness10
  hardcoded the staging address and silently read garbage after the relink.
- Before declaring a visual bug "reproduced", diff against a known-good
  reference build/capture FIRST. Screenshots at non-integer scales cannot be
  pixel-diffed (resampling noise swamps content); a known-good .gba build is
  the gold standard — ask for one early.
- The libmgba C-harness suite (gcc harnessN.c -lmgba) in /home/claude gives
  deterministic frame dumps, busRead/busWrite forensics, and real key
  injection; harness10 dumps framebuffer+VRAM(true addressing)+staging+
  scroll+map at one instant in one process.

### §12b — Session 20, part 2: Star Ally's black screen (two stacked bugs)

User report "SA doesn't work at all" was accurate and was NOT the s19 wild-PC
crash — the guest ran fine behind a black screen. Two independent defects:

1. **BKEXTEN bank starvation (s19 regression).** vt_chr4_page_bank[] is
   populated in exactly one place — inside vt_chr_sync_flush() — and the s19
   IME-masking restructure made the BKEXTEN branch of
   vt_chr4_rebuild_if_dirty() return BEFORE the flush. Under BKEXTEN the
   snapshot stayed all-zero, so every slot assembled from bank
   (0<<3)|attr = PRG code/padding (slot0's VRAM matched a bank-0 reference
   assembly 512/512). Fix: run vt_chr_sync_flush() inside the BKEXTEN
   branch's IME-masked window, before vt_bk_banks_recheck() — flush feeds
   the snapshot, recheck reassembles stale slots.

2. **Slot signature blind to zero-stomps.** vt_bk_slot_sig recorded word 0
   of each slot; when tile0-row0 is legitimately blank (SA's attr-0 bank:
   first art at word 13), a 2bpp-cache zero-stomp compares 0==0 and
   frame_check never repairs. Fix: record the first NONZERO word's index +
   value at fill time (vt_bk_slot_sigoff, mirroring vt_chr4_sigoff) and
   check that word; 0xFFFF = genuinely blank page, skip.

Verification: page banks [2,3,2,3,0,5,6,7]; slots hold exactly the
reference-assembled content (425/492/408 nonzero words) stably through 1200
frames; title renders; Start transitions (10957 px change) and the game
keeps animating. LI regression check on the same core: 0.10% vs s15.

**Still open for "SA fully playable": PIX16EN sprites (§8, unimplemented) —
gameplay will be missing/garbling sprites until then — then speed, then a
long-run crash soak.**

## §13 — ARM/Thumb interworking rule for asm-called C functions (session 20)
Any C function called from assembly via `bl_long`/`b_long` (`mov lr,pc;
ldr pc,=sym`) MUST be compiled ARM: mark it
`__attribute__((target("arm")))`. On ARM7TDMI, loading PC does NOT switch
state; a Thumb-compiled callee's symbol resolves with bit0 set, and the call
executes Thumb machine code in ARM state — instant garbage execution. This
bit sat dormant in vt_bk_consume until Star Ally's timer ISR produced the
first real ring traffic (wild PC into appended ROM data at first timer IRQ).
Audit command (run after adding any new asm->C call):
  grep -rhoE "(bl_long|b_long)[[:space:]]+\w+" src/*.s | awk '{print $2}' \
    | sort -u | while read s; do
      v=$(arm-none-eabi-readelf -sW pocketvt.elf | awk -v s="$s" \
          '$8==s && $4=="FUNC"{print $2;exit}');
      [ -n "$v" ] && [ $((0x$v & 1)) = 1 ] && echo "THUMB DANGER: $s=$v";
    done
Related: the linker only inserts interworking veneers for BL relocations,
never for literal-pool `ldr pc` data references.

## 15. Session history in one page (the status files were deleted; this replaces them)

Sessions 3-5: the 4bpp render path was built and Lonely Island first rendered.
Sessions 18-19: two s18 regressions fixed; LI became the standing control.
Session 20: PIX16EN (16-pixel-wide sprites) implemented and verified against
the reference capture; a speed hack that was making Star Ally SLOWER removed,
and a regression that had knocked LI to ~22% fixed. SA has sat at ~67% of
native (~40fps) since -- that is the interpreter ceiling, not a bug (s20b3
profile: flat, 90% IWRAM, wait loop only 8%). Two standing rules from that
work: never hand-seed an SA speedhack, and never blanket-gate the speedhack
finder off for VT (it broke LI 100 -> 22%).
Session 21, in order: b1 shipped a WRONG EVA model and was reverted; b2 found
the real orange-band cause (loadcart re-applying the iNES header mirror bit
over the $4106 default); b3 closed the 2bpp-EVA sprite gap; b4 root-caused the
multicart hang as globals-layout drift (section 14); b5 fixed grey sprites;
b6 fixed invisible EVA=0 sprites and the bottom band (section 13b); b6b fixed
a symbol collision b4 had introduced (section 14).

Claims from earlier sessions that were later DISPROVED -- do not rebuild on
them: the multicart hang is not a TM0/cascade problem; BKPAGE does not
suppress the attribute bits when forming the BG EVA; the per-scanline scroll
buffer was never "garbage" (the old probe read pointers as data); $2016 is a
CHR bank register, not a relocated controller port (that came from a
disassembler that descrambled operand bytes as well as opcodes); and the
strip along the bottom of Star Ally's screen was never "the HUD rendering
correctly".

## 16. Display mapping: 256x240 -> 240x160 (measured, s21b7)

Vertical: every third NES scanline is DROPPED. The per-line scroll table
advances the displayed NES line by +1 on two of every three GBA lines and by
+2 on the third. The 2/3 ratio is right; 80 lines of detail are gone.

Horizontal: there is NO scaling at all. DISPCNT is mode 0 with BG0 and BG2 as
TEXT backgrounds, which cannot scale, so 16 of the 256 NES columns are simply
off-screen at whatever scroll the game has set. This is the "256x240 -> 240x160
scaling check" that has been pending since the 4bpp sessions.

Doing horizontal properly needs an affine background, and that is why it keeps
getting parked: GBA affine maps use ONE-BYTE tile indices (256 tiles maximum)
while the BG cache addresses 512+ tiles (slots at 0, 64, ... 960). Any affine
plan has to solve that first -- e.g. a smaller per-frame working set, or
splitting the screen across two affine layers. The stock unscaled/pannable
mode (L/R plus Up/Down) is the other lever and costs nothing.

## 17. VT timer: vblank must be SKIPPED, not compensated (fixed s21b9)

$4101 D7 (TSYNEN) = 0 selects AD12-transition counting. AD12 only toggles
while the PPU is fetching, so the hardware counter STALLS through vblank.
Scheduling plain 341-dot scanlines fires every expiry ~22 lines early; in Star
Ally that put the raster split above its HUD text and painted the HUD page
across the bottom of the screen -- the orange/yellow band.

The working implementation (sound.s, both scheduling sites -- install_now and
the handler), with r1 = base timestamp and r2 = period * 341:

    ldr_ r0,frame_timestamp
    ldr_ r12,render_end_time
    add  r0,r0,r12          @ absolute end of rendering, this frame
    cmp  r1,r0
    bhs  1f                 @ already in vblank -> resume at next line 0
    add  r1,r1,r2
    cmp  r1,r0
    bls  2f                 @ lands inside the rendered area -> done
    sub  r2,r1,r0           @ carry ONLY the leftover count
1:  ldr_ r0,frame_timestamp
    ldr_ r12,cyclesperframe
    add  r0,r0,r12
    ldr_ r12,line_zero_start_time
    add  r0,r0,r12          @ line 0 of the NEXT frame, absolute
    add  r1,r0,r2
2:

Three traps, each of which cost a build:

1. render_end_time (82181 NTSC) and line_zero_start_time (292) are OFFSETS
   WITHIN A FRAME, not absolute timestamps. The absolute base is
   frame_timestamp -- that is how timeout.s itself uses them
   (`ldr_ r0,frame_timestamp / ldr_ r2,render_end_time / add r1,r0,r2`).
   Comparing a live timestamp against the raw offsets schedules the expiry
   into the past or the far future and the game hangs with $2010 = 00.
2. The two cases must be EXCLUSIVE. Moving the base into the next frame AND
   then wrapping the overshoot pushes the expiry a whole extra frame out --
   same hang.
3. ldr_/str_ have no conditional forms; write `ldrhs r1,[globalptr,#label]`.

An earlier version (s21b6) added a FIXED vblank whenever the target passed
render end. That got the position right but wrecked the phase, because the
guest re-arms from inside its NMI handler -- i.e. from inside vblank -- where
the correction should be the distance actually spent in vblank. It shook
visibly and was reverted before this version replaced it.

VERIFY BOTH PROPERTIES, always: a raster change is only good if the split is
in the right PLACE and STABLE. Run ~400 gameplay frames, record the line where
the per-line VOFS jumps, and histogram it. Reference numbers for Star Ally:
s21b6 = lines 138-145 with 116 changes (broken); revert = line 122 with 15
changes (stable but wrong place, band present); s21b9 = line 138 on all 401
frames with 0 changes, band gone, and NES line ~207 matches where 2.png puts
the score.

## 18. Real hardware vs emulators (GBARunner2 on DS does not boot)

Reported: PocketVT runs under mGBA on 3DS but will not boot under GBARunner2
on DS. Not reproducible in this sandbox -- mGBA is not GBARunner2 -- but the
build uses two techniques GBARunner2 is known to struggle with, and they are
the first things to test:

1. CODE EXECUTING FROM VRAM. The .vram1 section (4KB at 0x06003000, copied
   from LMA at startup by main()) holds the speedhack helpers. GBARunner2 maps
   GBA VRAM onto DS VRAM banks; instruction fetch from there is the single
   most likely blocker. Test by relinking .vram1 into EWRAM (it is not on the
   hottest path) and seeing whether the DS boots.
2. HBLANK DMA. ppu.s programs DMA0 from dma0buff into REG_BG0HOFS every
   scanline for the per-line scroll. HDMA timing is a known weak spot there.
3. Startup register pokes: REG_WAITCNT (0x4000204) and the EWRAM wait-state
   register REG_WRWAITCTL (0x4000800). The latter is GBA-specific; on DS it is
   not the same register, and writing it early can be fatal.

Change ONE of these at a time and note which one moves the needle -- and keep
the change behind a build flag so GBA-native performance is not paid for a DS
workaround.
