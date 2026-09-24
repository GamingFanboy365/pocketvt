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

## 16. Display mapping: 256x240 -> 240x160 (measured, s21b7; CORRECTED s21b40)

Vertical: one NES scanline in FOUR is dropped, not one in three. Re-measured
in s21b40 by reading _dma0buff back out of a live frame (Scramble, f150):
the visible window is NES rows 16..228 (212 rows) and the per-line step
histogram is exactly {+1: 106, +2: 53} -- 159 steps, 53 of them doubled. That
is the ppu.s `scale75` path (SCALED mode), which emits 3 lines for every 4
source lines; the earlier "every third scanline" wording in this section was
wrong and made the loss look worse than it is.

Do not re-derive this from the ratio 160/240 = 2/3. The renderer does not show
all 240 rows -- it crops 16 at the top and 11 at the bottom first, so the
decimation actually applied is 160/212 = 0.755, i.e. scale75's 3-in-4. Section
45's "drops one scanline in four" is the correct statement.

`scale75` also rotates WHICH line it drops from frame to frame (the twitch /
adjustblend / flicker self-modifying block at the top of the routine), so a
single captured frame is not a stable sample of the decimation phase. Any
offline scoring that compares one of our frames against one reference frame
inherits that jitter.

Horizontal: there is NO scaling at all. DISPCNT is mode 0 with BG0 and BG2 as
TEXT backgrounds, which cannot scale, so 16 of the 256 NES columns are simply
off-screen at whatever scroll the game has set. This is the "256x240 -> 240x160
scaling check" that has been pending since the 4bpp sessions.

### The vertical blend is CHEAPER than this section has been claiming (s21b40)

Measured on the live VG Pocket build, category menu:

    DISPCNT  = 0x1540   mode 0, BG0 on, BG1 OFF, BG2 on, BG3 OFF, OBJ on
    BG0CNT   = 0x4c02   prio 2, charbase 0, screenbase 12, 16-col, 512x256
    BLDCNT   = 0x0110   targets set but effect bits 6-7 = 0, i.e. blending OFF
    BLDALPHA = 0x1000

So BG1 and BG3 are both free, and the alpha-blend unit is idle. That makes a
two-layer vertical resample possible WITHOUT a new DMA channel:

* BG1 points at the SAME charbase and SAME screenbase as BG0, so it costs zero
  extra VRAM -- it is literally BG0 drawn again at a different VOFS.
* BLDCNT = 0x0241 (1st target BG0, 2nd target BG1, effect = alpha),
  BLDALPHA = 0x0808 (8/8). Both are set once per frame, no HDMA needed.
* DMA1 ALREADY writes DISPCNT every scanline, so BG1's enable bit can be
  toggled per line for free: enable it only on the lines that straddle a
  dropped source row, leave it off elsewhere so kept rows stay sharp.
* BG1HOFS/BG1VOFS sit at 0x04000014/16, immediately after BG0HOFS/VOFS, so
  widening DMA0 from 1 word to 2 words per line covers all four registers in
  one transfer.
* BG1CNT sits at 0x0400000A, immediately after BG0CNT, so widening DMA3 from a
  16-bit to a 32-bit transfer covers both.

Costs: dma0buff doubles and dmabg0cntbuff widens to u32 (both live in the
globals block -- see section 14, and run check_globals after), one extra BG
layer's fetch bandwidth on roughly the 53 blend lines per frame, and scale75
has to emit the second scroll word and the enable bit.

NOT YET IMPLEMENTED. Do not start it without Star Ally and Lonely Island on
disk: this touches the display path for EVERY cart, and those two are the only
controls that will catch a regression.

### Cropping is not an alternative on the screen that needs it

Measured which NES rows actually carry content, per screen (a line counts if it
has more than one colour):

    VG title      rows  38 - 228
    VG category   rows  16 - 228   <- every single visible row
    VG game list  rows  24 - 139

The category menu, which is the screen section 45 identifies as limited by the
downscale, has content on every row the renderer shows. Tightening the window
to reduce decimation would throw away real pixels there, so "crop more, drop
less" is not a fix for it.

Doing horizontal properly needs an affine background, and that is why it keeps
getting parked: GBA affine maps use ONE-BYTE tile indices (256 tiles maximum)
while the BG cache addresses 512+ tiles (slots at 0, 64, ... 960). Any affine
plan has to solve that first -- e.g. a smaller per-frame working set, or
splitting the screen across two affine layers. The stock unscaled/pannable
mode (L/R plus Up/Down) is the other lever and costs nothing.

## 46. VT09 is VT03 plus 4 KiB of CPU RAM -- and that was the whole bug (s21b41)

Lucky Lawn Mower (VT09) booted to a BLACK SCREEN forever while its CPU ran
(it kept writing $2010). The cause is one number.

NintendulatorNRS builds the console from the NES 2.0 EXTENDED CONSOLE TYPE in
header byte 13 (valid only when byte 7 bits 0-1 == 3). From MapperInterface.h:

    0x07 VT03    0x08 VT09    0x09 VT32    0x0A VT369

and NES.cpp wires VT09 as `CPU_VT09(CPU_RAM)` = `CPU_OneBus(0, 4096, RAM)`,
`APU_OneBus`, `PPU_VT03`. The APU and PPU are IDENTICAL to VT03. The ONLY
difference is 4096 bytes of CPU RAM instead of 2048 (NES.cpp's own comment:
"2 KiB for normal NES ... 4 KiB for VT09/VT32/VT369"). Confirmed in the game:
its reset routine at $FFE0 does `LDX #$0F / STA ($00),Y / DEX / BPL` -- it
clears $0000-$0FFF, four kilobytes.

PocketVT masked every $0000-$1FFF access to 11 bits (`bic addy,addy,#0x1f800`
in memory.s), so the game's entire upper 2 KiB of variables aliased onto the
lower 2 KiB and corrupted itself. Widening the mask to `#0x1f000` makes it
render immediately.

**Where the extra 2 KiB lives.** NOT in a bigger NES_RAM. IWRAM has 508 free
bytes between .bss (ends 0x03007B64) and the usr stack (0x03007D60), so a
2 KiB bump would land in the stacks -- the exact fault section 4 records. The
upper half instead OVERLAYS the first 2 KiB of NES_SRAM, which is safe here
because these carts declare no PRG-RAM (Lucky Lawn Mower's header byte 10 is
0, against 0x07 for Star Ally and Lonely Island). CAVEAT: a VT09/VT32/VT369
cart that uses BOTH 4 KiB of RAM and $6000-$67FF work RAM would corrupt one
with the other. None known; check header byte 10 before assuming.

**How it is switched.** The mask is two BIC instructions in memory.s labelled
`ram_R_mask` and `ram_W_mask`, patched at cart load by `set_nes_ram_4k()` in
loadcart.c, which copies a pre-assembled template word (`ram_mask_2k` /
`ram_mask_4k`) rather than hand-encoding an ARM immediate. The hot RAM path
costs nothing extra, and ARM7TDMI has no instruction cache so the write takes
effect immediately. The gate is console type 0x08-0x0A ONLY, so NES, VT02 and
VT03 keep 2 KiB mirrored 4x.

RESULT: Lucky Lawn Mower (VT09) renders a real playing field -- mower sprite,
fence, grass -- animating across frames, 10244 lit px / 26-39 colours,
$2010=$4E. Controls unaffected: Star Ally ($1F menu, $1A gameplay) and Lonely
Island (31309 px) produce numbers IDENTICAL to the pre-change build, as does
Scramble.

ITS PALETTE IS WRONG, and expect that. $2010 D6 is SET, so we select
`vt_compat_rgb555_vg` -- the VG POCKET's DAC, calibrated against VG captures.
This is a different console. Section 25's rule applies: do NOT touch the table
without a hardware capture of THIS machine.

## 47. VT369 needs four things we do not have; the RAM fix alone is not enough

With the section 46 gate applied (console type 0x0A also gets 4 KiB), the
VT369 Lucky Lawn Mower still renders nothing at 600 frames while its CPU runs
($2010 = $86 = COLCOMP|SP16EN|BK16EN). Its reset at $F85B likewise clears
$0000-$0EFF, so 4 KiB is right but insufficient. From NES.cpp's VT369 block,
the platform additionally remaps FOUR regions we currently point elsewhere:

    $1000-$1FFF  the 4 KiB EMBEDDED ROM, supplied as NES 2.0 Misc ROM
                 (this file: 135184 = 16 + 128 KiB PRG + exactly 4096)
    $3000-$3FFF  VRAM mapped into CPU address space -- we route this to the
                 PPU registers as a $2000 mirror
    $5000-$5FFF  palette writes (writeVT369Palette) -- we route this to IO
    $2000-$2FFF  a SECOND CPU (sound) and PPU_VT369, unless sound is HLE'd

The game uses at least two of those heavily. Scanning its PRG for absolute
loads/stores: 52 sites touch $3000-$3FFF and 24 touch $5000-$5FFF (plus 32 in
$1000-$1FFF). Static counts include some false positives from data bytes, but
the distribution matches the reference wiring exactly. Every one of those
writes is currently landing in a PPU register or the IO handler.

So VT369 is a PLATFORM port, not a patch: separate address-space routing, a
second CPU core, and its own PPU class. VT09 was one constant; this is not.
Reference sources are now in reference/nrs/ (h_OneBus.*, OneBus.*, the VT32
and VT369 headers, and every OneBus mapper: 256, 270, 296, 407, 408, 419,
423-427, 436 -- note 419 is Table Soccer, open item 6).

## 48. The reference emulator CONFIRMS our palette model -- and cannot help with the display (s21b42)

NintendulatorNRS is a PC emulator that renders 256x240 natively. It has no
downscale, no scanline budget and no DMA channels, so it has NOTHING to say
about sections 16/45. That work is a GBA hardware problem and stays ours.

What it IS authoritative for is emulation SEMANTICS, and it just settled open
item 3. From reference/nrs/OneBus.cpp:

    PPU_VT03::GetPalIndex(TC):  if (!(TC & 0x63)) TC = 0;
    BG plane 0 -> CHRLoBit[..]        (bit 0)
    BG plane 1 -> CHRHiBit[..]        (bit 1)
    BG plane 2 -> CHRLoBit[..] << 5   (bit 5)   gated by BK16EN
    BG plane 3 -> CHRHiBit[..] << 5   (bit 6)   gated by BK16EN
    sprite pixel: DisplayedTC = SprDat | 0x10   (bit 4)
    attribute:    if (reg2000[0x10] & BKEXTEN) BG = 0;

That is our shipped plane-scatter model line for line:
`p0 | p1<<1 | (BKEXTEN?0:attr)<<2 | bgspr<<4 | p2<<5 | p3<<6`, with
`(idx & 0x63) == 0 -> backdrop`. Item 3 is no longer "best of three models
tested"; it matches the reference implementation. Models B and J stay dead.

Use reference/nrs/ the same way for open item 2 (h_OneBus.cpp `syncCHR` is the
authoritative CHR bank composition) and item 6 (mapper419.cpp IS Table Soccer).

## 49. Measuring the blend WITHOUT any reference capture (s21b42, reusable)

Before writing a line of the section 16 blend, measure what it can buy. The
trick: `emuflags` and `windowtop` are re-read by the display path EVERY vblank,
so a libmgba harness can poke them mid-run and switch to UNSCALED 1:1 mode with
no rebuild.

    EMUFLAGS  = GLOBALS + 0x50c    windowtop = GLOBALS + 0x477
    GLOBALS   = _dma0buff - 0x6c0  (re-derive per link)
    busWrite32(EMUFLAGS, ef & ~0x0000FF00)   -> scaling mode 0
    busWrite8 (WINDOWTOP, top); run 3 frames; capture

windowtop clamps at 80 (240-160), so two strips at 16 and 96 reassemble NES
rows 16..239 at full vertical resolution. CONTROL FIRST: every scaled output
line must equal the reconstructed row it claims -- measured 102/38400 pixels
differ (0.27%), the residue being sprite-mode differences on the sign text.

RESULT on the VG category menu, the screen section 45 blames on the downscale:

* 53 of 160 output lines take a +2 step, i.e. they are the lines that drop a
  source row.
* A +2 line covers EXACTLY two source rows, and BLDALPHA with EVA=EVB=8 gives
  floor((A+B)/2). So the BG1 blend is not an approximation of the right filter
  -- it IS the exact box downscale for a 1-in-4 drop.
* Against that ideal, our current output differs on **2257 of 38400 pixels
  (5.9%)**, mean absolute error 0.954 in RGB555 units. 2155 of those sit on
  the 53 blend lines (16.9% of blend-line pixels).

**So the blend is worth about 6% of the frame on the WORST screen we have, not
the 26 points section 45's 3x3-tolerance experiment might suggest.** Those two
numbers measure different things: the tolerance absorbs both our row
misplacement AND the reference capture's own 0.875 resampling. This measurement
isolates ours. The larger half of that 26 is in the YARDSTICK.

Adjust expectations accordingly before spending a session on it. The blend is
still correct and still worth doing -- on real hardware 53 lines currently show
one source row where two should be averaged, which is exactly what makes dense
foliage look wrong -- but it is a polish item, not a 26-point win.

## 50. The blend does NOT fit in the scanline buffer block as laid out (s21b42)

Measured before implementing, because this would otherwise be discovered
halfway through the asm. The non-fourscreen scanline buffers are packed
downward from `extra_nametables + 0x1000` = VRAM+0x8000, a 4096-byte block:

    dma0buff    164*4 = 656      scrollbuff     240*4 = 960
    dma1buff    164*2 = 328      dmascrollbuff  240*4 = 960
    dma3buff    164*2 = 328      dispcntbuff    240*2 = 480
                                 ------------------------------
                                 total 3712, leaving 384 free

The blend needs dma0buff at 2 words per line (+656, for BG1HOFS/BG1VOFS) and
dma3buff at 1 word per line (+328, for BG1CNT) = **984 bytes against 384
free**. The in-code comment claiming "880 bytes free in that vram block" is
wrong; count it yourself.

Three ways out, cheapest first:
1. Drop the dma3buff widening. Set BG1CNT ONCE per frame instead of per line.
   Costs correctness only on lines where a game changes BG0CNT mid-frame; the
   VG category menu holds 0x4c02 on all 160 lines. Saves 328, leaving 656
   needed against 384.
2. Move dma1buff (328 B, DISPCNT) to EWRAM. Combined with (1) that gives 712
   free against 656 needed -- it fits. One halfword per hblank from EWRAM is
   affordable inside 68 dots, but it IS a timing change on every cart.
3. Relayout the whole block.

Also budget for: scale75_even AND scale75_odd both emit the copy block, and
`scale75_skip` returns via `add pc,lr,#7*4` -- that literal counts the
instructions it skips in the CALLER, so every instruction added to the copy
block must bump it. Getting this wrong lands mid-instruction. The unscaled
path (`vblunscaled`) writes the same buffers with a 4-word `ldmia/stmia` copy
and has to widen too.

## 51. The failing VG game is NOT a banking fault -- the reference composes the SAME address (s21b43)

Open item 2 has been chasing "code in the upper 2 MB, graphics fetched from the
lower" for several sessions. Worked the reference formula by hand against the
live registers and that line is dead.

FIRST, fix the entry index. Navigating category 0 and stepping Down gives:

    entry 0  lit 13359  9 col  $2010=$4E     <- $4E is the GAME LIST value:
    entry 1  lit 13359  9 col  $2010=$4E        this one never launched
    entry 2  lit 24498 12 col  $2010=$56
    entry 3  lit     0  1 col  $2010=$06     <- THE FAILING GAME
    entry 4  lit 28245 15 col  $2010=$16

Entries 0 and 1 are byte-identical, which is the dropped-tap signature from
section 36. The catalogue's "category 0 entry 2" is index 3 under this
navigation. Its registers at failure:

    $2012-15 = 00 00 00 00     $2016 = $A0   $2017 = $A2
    $2018 = $00   $201A = $00   $4100 low nibble = $00   $2010 = $06

$2010 = $06 is BK16EN|SP16EN with BKEXTEN and V16BEN CLEAR: 4bpp, 8-bit bus,
NON-extended path.

Now run reference/nrs/h_OneBus.cpp `setCHR` by hand on those values. Its
non-extended branch is

    addr1k = (reg2000[0x16] & chrAND | chrOR | VA18<<8 | VA21<<11)
    chrAND = 0xFF >> VB0STable[$201A & 7]   VB0STable = {0,1,2,0,3,4,5,0}
    chrOR  = ($201A & 0xF8) & ~chrAND       VA18 = $2018>>4 & 7
    VA21   = $4100 & 0x0F                   byte = addr1k << 10, base = chrLow

With $201A = 0: chrAND = 0xFF, chrOR = 0, VA18 = 0, VA21 = 0, so addr1k = $A0
= 160 and the byte offset is 160 << 10 = 0x28000 into `chrLow`. chrLow is the
de-interleaved 4bpp low-plane array built as
`shifted = i&0xF | i>>1 & ~0xF` for bytes with bit 4 clear, so index 0x28000
inverts to ORIGINAL ROM 0x50000.

**That is exactly the address our engine already fetches** -- the same
ROM 0x50000 recorded in item 2, where 63 of 64 tiles are blank. The reference
implementation would fetch the same blank tiles. Bank composition is CORRECT
and is not why the screen is empty. Stop looking there.

Also checked and ruled out: $4242, the CHR-RAM enable from the mapper 270
article ("1: use 8 KiB of unbanked CHR-RAM"), which would have explained blank
ROM tiles perfectly. All seven $4242 byte matches in the image sit inside data
(preceding bytes 4a42428a / 42429769 / 00ff00ff) -- none is real code, and in
any case NRS only implements $4242 in mapper 270's own handler, not in the
shared OneBus code. Our image is mapper 256.

What remains for item 2: the game fetches a blank ROM region and is not being
mis-banked into it, so either it is genuinely waiting on something we do not
provide, or its tiles arrive by a route we are not modelling. A hardware
capture is still the way to settle it.

## 52. What the reference has that we do not (s21b43, audited)

Audited reference/nrs against our implementation. CONFIRMED IDENTICAL, do not
re-derive: the $2012-$2017 and $4107-$410F power-on defaults; `setCHR`
composition in both branches, including the fact that VA18 ($2018 bits 4-6)
applies ONLY on the non-extended path (our extension composer already excludes
it -- see the comments at ppu_vt.c ~1132/1246); the 4bpp de-interleave layouts,
where bit 4 of the CHR address selects the plane pair on the 8-bit bus and bit
0 does on the 16-bit bus.

GENUINE GAPS:

**1. Our "$2010 D6, datasheet says UNUSED" gate has a name: V16BEN.**

    #define V16BEN !!(reg2000[0x10] &0x40 || reg4100[0x2B] ==0x61)

The empirical discovery of section 24 is confirmed and named. But there is a
SECOND trigger we do not implement: **$412B == $61 forces the 16-bit bus even
with $2010 D6 clear**. No cart we have does this (the VG image writes $412B
once, with $01), so it is latent, not live.

**2. We shadow only $4100-$411F.** `vt.reg` is `u8[0x20]` and vt_reg_write
gates on `addr_lo < 0x20`, so every write in $4120-$41FF is silently dropped.
NRS stores all 256 bytes (`reg4100[addr &0xFF] = val`) and re-runs `sync()` on
every one of them. In this image $412C alone has 90 access sites.

**3. $412B and $412C are the UIO direction and data registers** -- documented
in the NES 2.0 Mapper 270 article that is ALREADY in our in-tree XML dump:
"any $412C bit can only be written to if the corresponding bit in $412B is set
to 1 (output)", and mapper 270 uses $412C bits as PRG/CHR A24/A25. The VG boot
code at ROM 0x07C263 sets $412B = $01, making UIO bit 0 an output. For a 4 MB
image A24 is out of range so this is not the VG failure, but it is exactly the
mechanism any >4 MB OneBus multicart needs, and mapper 270 support starts here.

**4. Section 27's 4bpp outer-bank narrowing disagrees with the reference.**
We mask the outer field to 3 bits when 4bpp. NRS keeps VA21 = `$4100 & 0x0F`
at `<<11` unconditionally and handles 4bpp by halving chrMask and switching the
base pointer to the de-interleaved array. s21b18 recorded that the narrowing
"changed nothing visible on any cart we have", which is consistent with both
being equivalent on <= 4 MB images -- but they are not the same rule, and the
difference shows up exactly on the >4 MB carts s21b18 was written for. Resolve
before trusting either on a big cart.

## 53. OPEN ITEM 2 SOLVED (diagnosis): the game is HUNG WAITING FOR AN INTERRUPT (s21b44)

Everything sections 34/51 said about CHR banks was downstream of a game that
never turns rendering on. Traced properly this time.

**Step 1 -- the PPU is OFF.** Read the shadows BY SYMBOL (`_ppuctrl0` and
`_ppuctrl1`; re-derive per link) over 500 frames after launch:

    failing entry:  $2000=00 $2001=00  NT 832/960  lit=0      -- static, forever
    working entry:  $2000=a0 $2001=1e  NT 819/960  lit=28245

`$2001 = 0` means BOTH the background and sprite enable bits are clear. The
screen is blank because RENDERING IS DISABLED, not because the tiles are
blank. No CHR investigation can explain a PPU that is switched off.

**Step 2 -- it is hung, not slow.** Dump NES_RAM (0x03000000, 2 KiB) 120
frames apart: **0 of 2048 bytes change**. A loop that touches no RAM at all is
spinning on a register or waiting on an interrupt.

**Step 3 -- get the guest PC.** `_m6502_pc` (0x030076b4) holds a POINTER, not
a CPU address; subtract `_lastbank` (0x030076c4) to get the 6502 PC. Sampled
once per frame it pins at **$E096 / $E098**.

**Step 4 -- WORK OUT THE RIGHT BANK BEFORE DISASSEMBLING.** $E000's bank is
NOT `0xFF & prgAND | PQ3`. PA21 (`$4100 >> 4`) contributes bank bits 8-11, and
this game runs $4100 = $10, so PA21 = 1 and the bank is 239 + 256 = **495**.
Disassembling bank 239 gives a page of $FF filler and pure nonsense, which is
exactly what it looked like until the PA21 term was added. In bank 495:

    E094: LDA $26
    E096: CMP $26        <- compares $26 against ITSELF
    E098: BEQ $E096      <- so Z is always set unless an INTERRUPT
    E09A: RTS               changes $26 between the two reads

That is the classic wait-for-interrupt idiom. `$26` is a 16-bit frame counter
bumped at $FA31-$FA3A (`LDA $26 / CLC / ADC #$01 / STA $26 / LDA $27 / ADC
#$00`), inside the routine the NMI handler at $F9E8 calls -- and the IRQ
handler at $FA88 reaches the same shared code via `JSR $FA98`.

**Step 5 -- which interrupt?** $2000 = $00, so NMI is DISABLED. The game must
therefore be waiting on the VT TIMER IRQ. It arms the timer at $E3EC-$E3F1
(`LDA $1F / STA $4101 / STA $4102`) and acknowledges it at $FAA3 and $FAE0
(`LDA #$00 / STA $4103`) inside the IRQ path. Rescanning its live banks -- the
CORRECT ones, 492/492/494/495 -- shows exactly the register set of a
timer-driven game: writes to $4101, $4102, $4103 (x4), $4106-$410B.

**CONCLUSION: item 2 is a TIMER/IRQ fault, not a graphics fault.** The game
sets up the VT timer, disables NMI, and spins until an interrupt ticks its
frame counter. We never deliver that interrupt, so it never reaches the code
that would enable rendering. Look at guide section 17 (the vblank-skip timer),
`vt_timer_armed` / `vt.want_timer_irq` in vt_regs.c, and NRS's $4101 =
reloadValue / $4102 = counter / $4103 = disable+ack / $4104 = enable.

This also predicts the pattern in section 34: if a family of VG titles shares
one timer-driven engine, they fail together. **Category 3 (Sports), where all
four entries are blank or flat, is the first place to check that.**

### Two traps this cost, both worth remembering

* **Scan the banks the game is ACTUALLY running.** My first register scan used
  banks 236/236/238/239 (PA21 dropped) and reported "only $412C is read, six
  sites". The correct banks show $4101/$4102/$4103 writes and a completely
  different story. A register scan on the wrong bank is worse than no scan --
  it produces a confident wrong answer.
* **$412C collides with our ADPCM status range.** `vt_reg_read` has
  `if (addr_lo >= 0x20 && addr_lo < 0x30) return vt.adpcm[(addr_lo-0x20)>>3].playing ? 1 : 0;`
  so a read of $412C returns ADPCM channel 1's play flag. $412C is the UIO
  data register (section 52). Forcing bit 7 did NOT unhang this game, so it is
  not the bug here, but the collision is real and should be fixed when UIO
  goes in.

## 54. THE VG POCKET IS A VT09 MACHINE. It has 4 KiB of CPU RAM. (s21b45)

This is the fix for open item 2, and it is a HEADER problem, not a code
problem. The section 46 machinery built for Lucky Lawn Mower already handles
it -- the VG image was just never declared as VT09.

**How it was found.** Traced (section 53) to the game spinning at $E096
waiting for $26 to change. Then watched its zero page frame by frame:

    f5-f26   $18 = $88   <- the game's PPUCTRL shadow, NMI bit set
    f27      $18 = $00   <- spontaneously wiped
    f30+     hung, $2000 = $00 forever

The NMI handler at $FA1D does `LDA $18 / AND #$7F / STA $2000`, so once $18 is
zero every NMI writes $2000 = 0 and NMI can never fire again. $26 is bumped
only inside that handler ($FA31), so the wait at $E096 never ends.

Nothing in the ROM can zero $18. All six writes to it are read-modify-writes
that preserve the upper bits: `LDA $18 / AND #$FC / ORA $0F / STA $18` at
$C2A8 and $C2DE, `LDA $18 / EOR #$01 / STA $18` at $E52A/$E542/$E55B/$E574.
A byte that only ever has its low bits touched cannot become $00 -- so the
write came from somewhere else in the address space, aliased on top of it.

**It was $0818 landing on $0018.** ram_R/ram_W mask $0000-$1FFF to 11 bits for
anything that is not console type 0x08-0x0A. Declare the image VT09 (header
byte 7 |= 0x03 to switch on the Extended Console Type field, byte 13 = 0x08)
and the section 46 gate widens the mask to 12 bits. Result, same core:

    C0 G3   lit 0 / 1 colour  ->  lit 9667 / 43 colours    (was DEAD)
    C0 G4   lit 28245 / $2010=$16 -> 34160 / $56           (was CORRUPT)

C0 G3 is Lucky Lawn Mower -- the same title we have as a standalone VT09 cart,
which is independent confirmation of the console class. C0 G4 was not
"working" before: at 2 KiB it renders as horizontal tearing across the whole
frame; at 4 KiB it is a clean scene. C0 G0/G1/G2 are unchanged.

**This was predictable from evidence we already had.** NRS defines
`V16BEN !!(reg2000[0x10] &0x40 || reg4100[0x2B] ==0x61)` with a commented-out
`&& ROM->ConsoleType ==CONSOLE_VT09`, and it allocates the 16-bit-bus arrays
chrLow16/chrHigh16 ONLY for VT09 and VT369. The VG Pocket sets $2010 D6. We
discovered that bit empirically in s21b15 and called it "datasheet says
UNUSED"; it was telling us the console class the whole time.

### The wrapper recipe is now WRONG for VG-class images

Old (still correct for a VT03 board such as Star Ally or Lonely Island):

    4E 45 53 1A 00 00 00 08 01 01 00 00 00 00 00 00

For a console that sets $2010 D6, use instead:

    4E 45 53 1A 00 00 00 0B 01 01 00 00 00 08 00 00
                            ^^                ^^
                            |                 +-- byte 13 = 0x08 = VT09
                            +-- byte 7 bits 0-1 = 3: Extended Console Type

`tools/rewrap_onebus.py in.nes out.nes vt09` patches an existing image in
place-ish. Byte 7's low two bits MUST be 3 or byte 13 is not an extended
console type at all and the gate in loadcart.c ignores it.

### What to re-measure

Every VG number in sections 34 and 37-45 was taken at 2 KiB. The catalogue
census (~13 blank, 4 flat, 5 sparse) and the palette scores are all suspect
now -- some of those entries were starved of RAM, and a corrupted frame
calibrates colour badly. Re-run the catalogue and re-score the four reference
screens against a VT09-wrapped image before drawing any further conclusions.
Star Ally and Lonely Island are NOT affected: they are separate ROMs with
console type 0x07 and the gate leaves them at 2 KiB.

## 55. THE MENU HARNESS HAS BEEN PRESSING **SELECT**, NOT DOWN (s21b46)

Every VG catalogue number in sections 34 and 40, and every per-entry label in
sections 51-54, was produced by a harness that navigated with key bit `0x04`.

GBA KEYINPUT bit order is A, B, Select, Start, Right, Left, Up, Down, so:

    A = 0x01   B = 0x02   Select = 0x04   Start = 0x08
    Right = 0x10  Left = 0x20  Up = 0x40   DOWN = 0x80

`0x04` is SELECT. Verified by pressing each bit on the category menu and
hashing the live nametable (NES_VRAM2, 960 bytes): only A (0x01), Up (0x40)
and Down (0x80) change it. Select, Start, Left and Right do nothing there.

**What this invalidates.** With Down never registering, every "category C entry
G" run actually sat on the SAME menu row and differed only in how many frames
elapsed before the launch press. That is why sections 51/53 saw "entries 0 and
1 byte-identical" and blamed a dropped tap -- there was no dropped tap, the
cursor never moved. The catalogue census (54 entries, ~13 blank, 4 flat, 5
sparse) and every per-entry identification are VOID. Re-derive them; do not
cite the old numbers.

With the correct key the four fixed-size categories enumerate cleanly and the
category-menu hash differs per category (d3b81362 / 01d29362 / 4ded1362 /
b8079362), which it never did before.

**What this does NOT invalidate.** The section 54 finding stands on its own
evidence, none of which came through the menu: the zero-page trace showing $18
going from $88 to $00 with no ROM instruction able to do that, the aliasing
argument ($0818 onto $0018), and the standalone Lucky Lawn Mower VT09 cart --
a separate file, no menu involved -- which needs the same 4 KiB to run at all.
The 2 KiB vs 4 KiB comparison also held every navigation input constant, so
whatever entry it was launching, that entry went from dead to rendering.

Section 54's re-measurement instruction still applies, and now has a second
reason: the old numbers were taken at 2 KiB **and** through a broken cursor.

**Harness rule going forward:** prove navigation before trusting a per-entry
result. Hash the nametable after each input and assert it CHANGED; assert the
category hash differs per category; and compare the PRG bank shadows
(vt.reg[0x07..0x0B]) before and after the launch press so "did it launch" is
measured, not assumed. tools/vg_census.c does all three.

### Still unresolved in the census

With correct navigation, entry G0 of every category comes back with the game
list's own pixel count and G1 reports MENU (bank shadows unchanged by the
launch press). So the first two rows of each list are still not launching
cleanly. That is a harness timing question, not a game fault -- do not record
those entries as broken until the navigation is nailed down.

## 56. ALL 50 VG POCKET GAMES RUN. The catalogue is 50, not 54. (s21b47)

Re-derived from scratch with the correct Down key (section 55) and a settle
rule that waits for the screen to stop changing. No code change -- the core is
still md5 b5f9e477bf611c9709bedee996b88e5f. The gain is entirely section 54's
4 KiB wrapping plus a harness that finally navigates.

**The last harness bug: the VG menus take ~45 frames to finish DRAWING.**
Idling on a freshly-entered game list with no input at all, the nametable hash
changes 24 times over roughly 45 frames and is then byte-stable for 450+. The
old 40-frame settle sampled mid-redraw, so the launch press landed while the
menu was still initialising -- that, not a game fault, is why entries 0 and 1
of every list "never launched". Never use a fixed settle on these menus:

    settle(): run frames with no keys until the nametable hash is
              IDENTICAL for 20 consecutive frames (cap 240)
    press(k): hold k for 8 frames, then settle()

With that, every entry launches and every entry has a DISTINCT PRG bank
signature. tools/vg_census.c and tools/vg_listlen.c implement it.

**Counts (each found by stepping Down until the nametable hash repeats):**

    C0 Action 5 · C1 Racing 4 · C2 Shooting 6 · C3 Sports 4 · C4 Wits 31
    = 50 entries

Which is exactly what the console advertises. The old 54 was an artefact of
the Select-instead-of-Down bug; treat "50-in-1" as a CHECKSUM on any future
census -- if a re-count does not land on 50, the navigation is wrong again.

**Status at ~420 frames after launch: 47 OK, 2 sparse, 1 flat, ZERO blank.**
Against the old census (~30 of 54 rendering, ~13 blank, 4 flat, 5 sparse).
Category 3 (Sports), previously "all four entries blank or flat", is 4/4.

**And the three outliers are not faults either.** Rendered and inspected:
C2 G2 is a high-score table, C4 G5 a "STAGE: 01" card, C4 G27 the "Sky
Mission" title. All three are legitimately sparse SCREENS, and all three
progress into full gameplay when sampled later (C2 G2 1796 -> 8694 px, C4 G5
254 -> 38018, C4 G27 2428 -> 13990). **So all 50 games run.**

LESSON, third time in this project: a "broken game" list is a claim about the
HARNESS until proven otherwise. Two harness bugs (wrong key, fixed settle)
accounted for the entire difference between "30 of 54 work" and "50 of 50
work". Before recording any game as broken, (a) prove the input registered,
(b) prove the launch happened via the bank shadows, and (c) look at the frame.

Controls re-verified this session: SA f150 12305 px / 17 col / $2010=$1F, LI
31309 px / 28 col / $0E, Scramble f150 1987 px / 14 col / $06 -- all matching
their recorded baselines.

## 57. SPEED: one function was 13.4% of the CPU. And $2010 D6 is NOT a DAC id. (s21b48)

Two separate fixes, both driven by Lucky Lawn Mower (VT09).

### 57a. vt_build_16color_palette was rebuilding from scratch every frame

Profiled by stepping the ARM core and histogramming PC against the symbol
table (tools/arm_profile.c). Lucky Lawn Mower's breakdown:

    6502 opcode handlers (_XX)  50.6%   <- the interpreter, unavoidable
    vt_build_16color_palette    13.4%   <- ONE function
    scale75_even/odd_loop        6.0%
    vt_spr_eva_update            3.3%
    vt_pal_dma_fast              2.4%

It recomputed the plane-scatter index and re-selected the DAC table 128 times
per frame. Both are frame-invariant. Hoisting the scatter into a table rebuilt
only when BKEXTEN flips, and selecting the DAC pointer once:

    Lucky Lawn Mower (VT09)   38.7%  ->  67.5%
    Star Ally                 79.0%  ->  85.0%
    Scramble                  95.0%  ->  95.3%
    Lonely Island            100.0%  -> 100.0%   (already at ceiling)

Verified as a PURE speedup: GBA palette RAM is byte-identical on Star Ally,
Lonely Island, Lucky Lawn Mower and Scramble.

**DO NOT skip the rebuild when vt_palette_ram is unchanged.** Tried it first
and it regressed 98% of Star Ally's pixels. run_palette and the legacy sprite
path also write GBA palette RAM, so this rebuild is partly a REPAIR of their
writes, not a projection of vt_palette_ram. It is not a pure function of its
inputs and must run every frame.

**EWRAM_BSS the statics.** Function statics are globals: putting these in
IWRAM .bss overflowed the ~508 free bytes into the usr stack and the GBA died
on an illegal opcode. s9's rule covers function statics.

### 57b. The DAC table was selected from a bus-width bit

`nes_index_to_bgr555` chose vt_compat_rgb555_vg when `$2010 & 0x40`. That bit
is V16BEN -- the 16-bit CHR BUS width (section 52). It is not a DAC id. It is
set on the VG Pocket, so it worked there by coincidence, but EVERY VT09-class
board sets it, and they do not share the VG Pocket's LCD. Lucky Lawn Mower was
being rendered through another console's fitted table: its sky is NES colour
$21, which the VG table holds as PINK (255,189,238) against the neutral
table's blue (0,139,255).

Carts now name their DAC in the NES 2.0 header: byte 13 bits 4-7, which are
RESERVED when byte 7 bits 0-1 == 3. 0 = auto (the old D6 inference, so every
image already wrapped is unchanged), 1 = neutral table, 2 = VG Pocket.
`tools/rewrap_onebus.py in.nes out.nes vt09 default`. There is no in-band
signal that distinguishes two VT09 boards, so this has to be declared.

### 57c. A diagnostic for suspect palette entries (use it, don't automate it)

The NES palette is structured: $0x/$1x/$2x/$3x are the same hue at rising
brightness. tools/palette_sanity.py flags entries whose hue is >60 deg off
their own column. On vt_compat_rgb555_vg it flags NINE: $04 $0C $11 $12 $1A
$21 $28 $35 $37.

**Do NOT auto-correct from this.** $1A is a KNOWN TRUE per-console difference
(blue-violet on the VG Pocket, green on Lonely Island's console -- already
recorded as a real finding, not a bug). The test cannot separate a genuine DAC
difference from a calibration error, so it only nominates entries for human
review against a capture. Six of the nine ($04 $0C $11 $12 $1A $21) ARE used
by the VG menus, so they were fitted from real data; the remaining three
($28 $35 $37) appear on no menu screen and are unconstrained junk.

### 57d. Every PNG this project has produced had RED AND BLUE SWAPPED

The libmgba framebuffer word decodes as `R = p&0xFF, G = (p>>8)&0xFF,
B = (p>>16)&0xFF`. Tooling had been using the reverse. Proof: the VG table's
$21 is (255,189,238) and the framebuffer word is 0x88efbdff -- only the
low-byte-first reading reproduces it. This made Lucky Lawn Mower's corrected
BLUE sky render as ORANGE in a comparison image and nearly sent the
investigation the wrong way. tools/raw_to_png.py now has the verified order;
use it rather than hand-rolling the conversion.

## 58. A THIRD console DAC, calibrated from a GAME capture (s21b49)

[stated] Michael supplied `lawn.png` -- Lucky Lawn Mower GAMEPLAY, native
256x240, a clean 20-colour framebuffer dump. First non-menu reference we have
had, and it fixed the game outright.

**THE VALIDITY CHECK THAT MAKES PER-PIXEL VOTING LEGAL.** Our frame and the
reference must be showing the SAME scene or the vote is confident nonsense.
Measure it: compare a palette-independent row signature (does each pixel equal
its right neighbour) cell by cell at the SAME position. We scored **94.7%** --
same maze. Had it come out near 50%, position voting would have been invalid
and the whole calibration void. RUN THIS FIRST, EVERY TIME. It is cheap and it
is the difference between calibration and fiction.

**Method** (guide section 31, extended): build with `-DVALUE_SENTINEL`
(`EXTRA_CFLAGS=-DVALUE_SENTINEL BUILD=... bash build_pvt.sh`). That makes
vt_build_16color_palette emit `gba_bg[i] = (i & 31) | ((i >> 5) << 5)`, so
every BG pixel's colour names the exact GBA palette SLOT that drew it -- no
inversion ambiguity. OBJ slots get `g5 = 3`, which the BG encoding cannot
produce, so sprite pixels are masked out of the vote (831 of them here).
slot -> idx_bg_tab -> vt_palette_ram gives the NES colour index; the reference
at the mapped position gives the true colour.

**The index model was re-validated in passing.** Scored seven candidate
slot->palette-RAM mappings by how close their voted colours land to the
canonical 2C02 palette: scatter (current) 132.7, datasheet `attr<<5|v` 172.5,
linear 172.5, `a<<2|v<<4` 178.6. **Model A wins by a wide margin** -- the
plane-scatter is right, third independent confirmation.

**Result: 16 indices measured, 15 at >=95% vote confidence** ($21 is the
exception at 61% / 256 px). They are NOT close to the canonical palette --
mean distance 132.7 -- so this board has a genuinely different DAC, distinct
from BOTH Star Ally/Lonely Island's and the VG Pocket's. Shipped as
`vt_compat_rgb555_llm`, selected by `VT_DAC_LLM` (=3) in the header nibble:
`tools/rewrap_onebus.py in.nes out.nes vt09 llm`.

The play area now renders correctly -- green grass, grey boulders, orange
brick, pale stone border, blue gems -- against an orange-and-brown mess before.

### What is still wrong, and it is NOT the palette

**The HUD strip renders black where hardware shows green.** Measured cause:
in the sentinel capture **every single BG pixel is attribute group 0** --
37569 of 37569, including the HUD rows. $2010 is $4E so BKEXTEN is CLEAR,
meaning the attribute SHOULD be live at palette-index bits 2-3. It is not
reaching the palette at all, so the HUD reads group 0's entries (black)
instead of its own.

That is the next bug, and it is in the tile path, not the table: the 4bpp
assembler is not propagating the nametable attribute into the GBA tile's
palette-bank field. Note the comment at ppu_vt.c ~1488 claims "attr =
palette-index bits 5-6 = GBA bank", which CONTRADICTS the scatter that puts
p2/p3 at bits 5-6 and attr at 2-3. Resolve that contradiction first.

Because everything lands in bank 0, the long-running "palette banks 1-3 are
never selected" observation is now explained: it is not that the games only
use attr 0, it is that WE never select anything else.

### Scores in context

39.6% exact against lawn.png, and the 3x3 tolerance adds almost nothing
(39.9%). Two known causes swamp it: the HUD (about a sixth of the frame,
entirely wrong colour) and the vertical decimation (section 16 drops one row
in four, so most rows simply are not in the output). Judge this screen by the
play area, not by the whole-frame number, until both are fixed.

## 59. In GAMEPLAY the bottleneck is vt_assemble_page_to, not the interpreter

Re-profiled Lucky Lawn Mower during play (tools/arm_profile.c):

    vt_assemble_page_to      39.2%   <- 4bpp CHR assembly
    scale75_even/odd loops    7.9%
    vt_build_16color_palette  5.7%   (was 13.4% before section 57a)
    vt_spr_eva_update         4.1%
    6502 opcode handlers     23.0%

Speed is 66-67% in both the opening and later play, so [stated] Michael's
report that the opening is still slow is consistent: section 57a raised the
floor but the CHR assembler is now the ceiling. The interpreter is NOT the
limit here (23%), which is the opposite of Star Ally.

`vt_build_16color_palette` dropping 13.4% -> 5.7% is section 57a working as
intended.

NEXT: vt_assemble_page_to is a change-detection candidate -- s13 already did
this for the BG copy. Assembling only pages whose CHR bank or source bytes
changed should be a large win. Unlike the palette builder (section 57a, which
must run every frame because it REPAIRS other writers) nothing else writes the
assembled tile cache, so caching there is sound -- but verify that claim
before relying on it.

## 60. ⚠ ALL EXACT-PIXEL SCORES WERE WRONG: 5-bit vs 8-bit expansion (s21b50)

[stated] Michael supplied gg.png -- the Lucky Lawn Mower OPENING, native
256x240, clean 18-colour PNG.

Scoring our opening against it gave **7.5% exact**. The top mismatch was the
sky: ours (0,173,255), reference (0,168,255) -- 28525 pixels. Those are the
SAME COLOUR. 5-bit green 21 expands to 8-bit two different ways:

    mgba's framebuffer:  v * 255/31   -> 21 becomes 173
    the reference:       v * 8        -> 21 becomes 168

**Comparing in 5-bit space (shift both sides >>3) the same frame scores
82.3% exact, not 7.5%.** Every exact-pixel number this project has ever
produced against a hardware-style reference is depressed by this, including
section 58's "39.6%" for the gameplay screen. ALWAYS quantise both sides to
5 bits before comparing; tools/score_vs_reference.py must do this.

This does NOT affect the CALIBRATION itself: deriving a table value with
`c >> 3` is correct in both conventions (168>>3 = 21 = 21). Only the
comparison was wrong.

## 61. The opening's remaining colour errors are a genuine index conflict

With the section 60 correction the opening is 82.3% right -- and that 82.3%
is mostly the large sky. Visually the sky is CORRECT; the fence renders pale
green where hardware shows tan, and the ground renders red where hardware
shows grey cobbles and green grass.

**Ruled out: the attribute bug.** Dumped the opening's palette RAM by
attribute group: group 0 holds $0E/$21/$08/$27, and **groups 1, 2 and 3 are
entirely $0E (black)**. The opening is a group-0 screen, so section 58's
attr-never-reaches-the-bank fault is not what breaks it.

**What it actually uses** -- the 4bpp 16-colour set, read through the
plane-scatter (values 4-7 come from pram[32..35], 8-11 from pram[64..67],
12-15 from pram[96..99]):

    $0E $21 $08 $27 | $18 $39 $09 $07 | $19 $0B $00 $3D | $16 $29 $2A $20

Fourteen of those sixteen are indices section 58 already measured from the
GAMEPLAY screen. Two are NOT: **$0B and $2A**, which are still inherited from
vt_compat_rgb555 and are not evidence-backed.

But the visible errors are on indices we DID measure -- the ground reads $16,
which gameplay voted red (187,38,0) at 100% confidence over 1541 px, while
the opening shows grey there. **A single per-console DAC cannot give one index
two colours**, so one of these is wrong:

1. our scatter picks a different index than the hardware does on one of the
   two screens (the index model is right per section 58's model bake-off, but
   that test only ran on the gameplay frame);
2. the gameplay vote mis-attributed $16 (sprite overlap is masked, but a
   scrolled or animated element could still land wrong);
3. the two screens differ in a register we are not tracking, so the same
   scatter index legitimately reaches different palette RAM.

NEXT SESSION: run the section 58 value-sentinel against gg.png (the layout
check first -- the opening is a static scene so agreement should be very
high), and compare the resulting index->colour map with the gameplay one.
Indices that AGREE across both screens are settled; the ones that DISAGREE
localise the fault to a specific element and will discriminate between the
three hypotheses above. Do NOT hand-edit the table to make the opening look
right -- that would silently break the gameplay screen, which is currently
correct.

## 62. The opening is a TILE-DATA fault, not a palette fault (s21b51)

[stated] Michael's screenshot of our opening on his machine matches the
headless render exactly: sky right, fence pale green where hardware shows tan,
ground red where hardware shows grey cobbles and green grass.

**Decisive test: compare the COLOUR SETS, not the pixel positions.** Quantise
both sides to 5 bits (section 60) and take the distinct-colour sets:

    our frame   22 colours
    gg.png      18 colours
    IN BOTH     14
    shared colours cover 95.8% of our area and 96.8% of the reference's

**If the DAC table were wrong, the sets would diverge. They do not.** We are
emitting almost exactly the right palette of colours -- we are putting them in
the wrong PLACES. The fence is pale green because those pixels carry the wrong
4bpp VALUE, not because value 7 maps to the wrong colour.

**So section 61's "index conflict" framing was wrong.** There is no conflict
between the opening and the gameplay screen over what colour an index is; the
gameplay calibration stands. The fault is upstream, in tile assembly: the
opening's tiles are being composed with wrong plane bits, so values land in
the wrong quarter of the 16-colour set (e.g. a value-7 pixel coming out as
value 4).

That points at `vt_assemble_page_to` / the 4bpp plane composition for this
screen's CHR pages -- the SAME function section 59 measured at 39.2% of
gameplay CPU. The gameplay screen assembles correctly and the opening does
not, so start by diffing what differs between the two: which CHR pages each
uses, the bus-width gate ($2010 D6 / V16BEN), and $201A's mask nibble.

**Do not spend more time on the DAC table for this game.** Sections 58 and 61
already measured it from a validated reference; this screen's remaining error
is not in it. `$0B` and `$2A` are still uncalibrated, but they are a minor
residue next to a plane-composition fault.

DIAGNOSTIC WORTH KEEPING: when a screen looks miscoloured, compare the colour
SETS and their area share before assuming the palette. High set overlap with
wrong placement means the tile data or the index path; low overlap means the
table. This test costs one command and would have redirected sections 61 and
most of b50 immediately.

## 63. CORRECTION to section 62, and the sharper evidence (s21b52)

Section 62 claimed the colour sets matched at 95.8/96.8% of area and concluded
the palette was fine. **That area figure was inflated and I over-read it**:
sky plus black alone are 85.1% of the reference frame. **Excluding those two,
the shared area is only 78.5%**, so roughly a fifth of the meaningful content
does use colours we never emit. Recompute shared-area figures with the
dominant flat colours excluded, or they will flatter any comparison.

The conclusion survives anyway, on better evidence. The four reference colours
absent from our output are:

    5-bit (10,10,10)  ~ (80,80,80)   1594 px   the cobble ground
    5-bit (0,6,2)     ~ (0,48,16)     197 px
    5-bit (0,27,2)    ~ (0,216,16)    176 px
    5-bit (18,8,0)    ~ (144,64,0)     11 px

**The decisive one is the cobble grey.** Our calibrated table holds
`$00 = (87,87,87)`, and 87 >> 3 = 10, so `$00` IS exactly 5-bit (10,10,10).
The colour the reference needs is already in our palette and we simply never
emit it. In the opening's 16-colour set, value 10 selects pram[66] = `$00`.

**So value 10 is never produced by our tile assembly on this screen.** That is
a plane-composition fault, not a DAC fault -- no table edit can fix a value
the assembler never generates. Section 62's direction was right even though
its supporting number was wrong.

Value 10 is binary 1010 = p1 | p3, i.e. it needs the HIGH plane pair. Values
8-11 all come from pram[64..67] (bit 6 of the scatter = p3). Start there:
check whether p3 is reaching the assembled tile at all on this screen.

**Registers are NOT the difference.** Dumped both screens:

    OPENING   $2010=4E  $2012-17 = 01 00 00 00 00 02  $2018=00 $201A=00 outer=00
    GAMEPLAY  $2010=4E  $2012-17 = 01 00 00 00 04 06  $2018=00 $201A=00 outer=00

Same mode, same bus-width gate, same mask nibble, same outer bank. **Only the
CHR page banks differ** ($2016/$2017: 00/02 vs 04/06). So the bus-width and
de-interleave paths are identical for both screens, and the fault has to be
either in how those particular pages' bytes are fetched or in the plane
composition for them. The gameplay screen assembling correctly is the control.

## 64. The value-confusion analysis is CIRCULAR -- do not repeat it (s21b53)

Chasing section 63's "value 10 is never emitted", I mapped both our frame and
the reference back to 4bpp values and built a confusion matrix. It is not
usable, and the reason is worth recording so nobody rebuilds it.

Facts established first (both sound):

* The frames ARE the same moment. Row-signature agreement on the opening is
  **95.2%** (gameplay was 94.7%), so per-pixel comparison is legitimate here.
* Exactly one value, **10**, is absent from our output; the other fifteen all
  appear.

Then the matrix came out as this, which is NOT a permutation:

    ref 15 -> ours  4  (1221 px)     ref  5 -> ours 13  (565)
    ref 10 -> ours 12  (1114)        ref  6 -> ours  5  (542)
    ref 13 -> ours  2   (701)        ref  3 -> ours 1 AND 6

`ref 3` maps to both 1 and 6, and `ref 6` to both 5 and 1, so it is not even a
function. **The flaw: the reference's pixels were mapped to values using OUR
table.** If a table entry is wrong, the reference's colours resolve to the
wrong values, and the matrix then mixes "we emitted the wrong value" with "we
have the wrong colour for that value" with no way to separate them. Any
analysis that decodes the REFERENCE through our own calibration is circular.

**The only non-circular instrument is the value sentinel** (section 58): build
with `-DVALUE_SENTINEL`, and our side's value is read directly from the
framebuffer encoding rather than inferred through the table. Do that against
gg.png and the question resolves in one run -- our value at each pixel is then
ground truth, and the reference colour at that pixel is independent evidence.

That run is the next step and was not completed. Section 63's registers dump
(same mode, only CHR pages differ) and the single missing value 10 both stand
and are the starting evidence for it.

### Honest status after four sessions on this game

Fixed and verified: the 4 KiB RAM fault, the DAC selector, the gameplay
palette, and a 13.4%-of-CPU hot spot. **Not fixed: the opening's colours and
the lag.** The lag is `vt_assemble_page_to` at 39.2% (section 59) and the fix
-- change detection -- was not attempted this session because it could not
have been verified in the budget left, and an unverified change to the tile
cache is exactly the kind of thing that regresses every cart silently.

## 65. Two corrections and a characterised palette fault (s21b54)

### 65a. Section 59's 39.2% was a SAMPLING ARTIFACT

tools/arm_profile.c was run for 300,000 steps. That covers roughly **two
frames**, and `vt_assemble_page_to` is called about once per frame, so a
single call dominated the sample. Re-profiled over **4,000,000 steps (~25
frames)**:

    6502 opcode handlers     37.4%
    vt_assemble_page_to      27.8%
    scale75_even_loop         4.4%
    vt_build_16color_palette  4.2%
    vt_spr_eva_update         3.2%

**Profile for at least 20 frames.** A short window on a function that fires
once per frame produces a number that is off by a third and points work in the
wrong direction.

### 65b. The assembler is expensive per CALL, not called often

Added counters (`vt_asm_calls`, `vt_asm_from_framecheck`, left in the tree):

    OPENING    0.4 calls/frame     frame_check repairs: 0
    GAMEPLAY   1.0 calls/frame     frame_check repairs: 0

So this is not a repair storm and s13's frame_check is not firing. One call
assembles a whole 2 KB page -- 64 tiles x 8 rows, four masked ROM reads and
four `vt_spread` lookups each, ~20-45k instructions. One of those per frame
is ~28% of the CPU on its own.

Hoisting the bus-width test and the `rombase`/`vt_spread` base pointers out of
the inner loop (they were re-read twice per row, 1024 redundant tests per
call) is output-identical but bought only **66.2% -> 66.8%** on the opening --
GCC had already done most of it. **Micro-optimisation is not the answer
here.** The fix has to be structural: assemble fewer tiles (most of a page's
64 may be unreferenced), or cache assembled pages by bank so an animation that
cycles between a few banks does not re-derive them. Neither was attempted; both
need storage and careful verification.

Current speeds: LLM opening 66.8%, LLM gameplay 45.8%, Star Ally 85.0%,
Lonely Island 100.0%. Controls verified byte-identical (SA 12295 lit px,
LI 31309).

### 65c. The opening palette fault, characterised (not fixed)

Ran the value sentinel against gg.png -- the non-circular instrument section
64 called for. It works: clean per-index votes on both screens, 13 indices
from the opening and 16 from gameplay, most at 85-100% confidence.

**Result: 4 indices agree across the two screens, 7 CONFLICT.** A single
per-console DAC cannot give one index two colours, so something upstream
differs. Ruled out, each by measurement:

* **Not the attribute path.** Both screens are 100% attribute group 0
  (37818/37818 and 37569/37569 sentinel pixels).
* **Not another palette bank.** Checked all four 128-byte banks; only bank 0
  holds data, and it is what we already read.
* **Not the scatter.** Searched seven candidate slot->palette-RAM mappings
  scored against the gameplay-calibrated table; the best alternative managed
  5/13 and the current scatter 4/13. No permutation reconciles them.
* **Not a stale capture.** Row-signature agreement on the opening is 95.2%
  (gameplay 94.7%), so the frames are the same moment.

**What IS true:** the opening's palette RAM ANIMATES -- entries for values
1-7 alternate between their real index and `$0E`, 26 byte-changes in 120
frames. Gameplay's palette is completely static (every entry single-valued
over 180 frames), which is why that calibration is sound.

And the decisive observation: **our palette RAM for the opening contains
exactly the right SET of colour indices, at the wrong SLOTS.** Ground truth
(sentinel + reference) says the opening's 16-colour set should be
`0E 21 29 3D 20 09 27 07 19 ? ? ? 00 39 ? ?`; we hold
`0E 21 08 27 18 39 09 07 19 0B 00 3D 16 29 2A 20`. Every truth value we can
resolve is present in our array, just at a different offset. A read-side bug
would not preserve the set; a WRITE-ADDRESS or write-ORDER bug does exactly
this.

NEXT: instrument the palette write path during the opening -- log every
($3F00-$3F7F) write address and value the game issues, and compare the
resulting array against the ground-truth set above. The scramble is
deterministic and the ground truth is known, so the offending transform
should fall straight out. Do not touch the DAC table; it is not implicated.

## 66. What the in-tree .md files settled -- and the WRONG FUNCTION (s21b55)

[stated] Michael asked whether the in-tree docs could locate the blockage.
They could; this should have been the first step, per the standing rule to
read DATASHEET_DIGEST*.md before instrumenting.

**66a. The palette model is confirmed a FOURTH time, from the datasheet.**
DATASHEET_DIGEST p.19: a 16-colour pixel address is 7 bits, NUMBERED FROM 1 --
bits 1,2 and 6,7 from pattern data, bits 3,4 from the attribute, bit 5 selects
sprite vs background. In 0-indexed terms that is exactly
`p0|p1<<1|attr<<2|bgspr<<4|p2<<5|p3<<6`. p.22's map agrees: four 32-colour
palettes at $3F00/$3F20/$3F40/$3F60, each BG +0..15 then sprite +16..31.

**This retracts the s21b55 "groups never read" lead.** The palette dump shows
eight 4-colour groups at stride 16, and our BG scatter only reads offsets
0/32/64/96. Groups 16/48/80/112 are the SPRITE halves -- read by the sprite
path, exactly as designed. Not a bug. Always number datasheet bits carefully:
the digest is 1-indexed and our code is 0-indexed.

**66b. COMR7 -- flagged by the digest, admitted unimplemented, ruled out.**
The digest says $4105 bit 7 (COMR7) swaps the $0000-$0FFF and $1000-$1FFF
pattern banks and that "PocketVT must honor this". ppu_vt.c ~524 confirms it
is not handled ("LI and SA both leave COMR7=0; add if needed"). MEASURED on
Lucky Lawn Mower: **$4105 = $00 on both the opening and gameplay.** Not the
cause here -- but still a real gap for any cart that sets it.

**66c. The only register difference between the two screens.**
$2000 = $88 on both (same BG pattern half, $0000). $2010, $2018, $201A,
$4100, $4105 identical. **Only $2016/$2017 differ: 00/02 on the opening,
04/06 in gameplay.**

**66d. I had been suspecting the WRONG FUNCTION.** Section 59/65 treated
`vt_assemble_page_to` as the BG tile builder. It is not, for this game: it is
only reached from EXTENSION paths (BKEXTEN BG slots, SPEVA/PIX16 sprites), and
Lucky Lawn Mower runs **BKEXTEN clear** ($2010=$4E, bit 4 = 0). Reading
`vt_bk_slot_bank[]` confirms every extension slot holds bank 0 on BOTH screens.
So:

* the opening's BACKGROUND is built by the NON-extension 4bpp path,
  `vt_chr4_assemble` -- that is where a bank-0-3-specific fault would live,
  and it has not been examined yet;
* the 27.8% CPU attributed to `vt_assemble_page_to` is SPRITE work, which
  retargets the speed investigation at the sprite path.

**66e. A flawed test, recorded so nobody reuses it.** I mapped 16-bit-layout
tile neighbour-coherence across the ROM to find where graphics live. It is
unreliable: DITHERED textures (this game's grass, cobbles, brick) have
naturally LOW neighbour-coherence, so detailed art scores like code. Its
reading that "gameplay's banks are not graphics" is not evidence of anything.

NEXT: read `vt_chr4_assemble`'s bank computation for the non-extension 4bpp
case and compare the address it produces for $2016=$00/$2017=$02 against the
datasheet's NORMAL-mode formula
`($4100&0x0F)<<21 + ($2018&0x70)<<14 + VBANK<<10`, shifted one bit left for
4bpp (p.11). Then look at the sprite path for the speed work.

## 67. The VT video-DMA paths hardcoded 2 KiB of RAM (FIXED), and the opening narrowed to tile data (s21b56)

### 67a. FIXED: both video-DMA paths masked their source to 2 KiB

`vt_pal_dma_fast` (ppu.s) and the generic DMA loop (Mappers/mapVT.s) fetch
their SOURCE bytes straight out of NES_RAM, and both had

    mov rN,#0x800 / sub rN,rN,#1        @ 0x7FF

hardcoded. On a 4 KiB cart (VT09/VT32/VT369, section 46) any DMA staged in
$0800-$0FFF read the wrong bytes -- the SAME 11-bit-vs-12-bit fault as section
46 itself, in two paths section 46 never touched. Both now load
`vt_nes_ram_mask`, set alongside `ram_R_mask`/`ram_W_mask` in
`set_nes_ram_4k()` (0x7FF or 0xFFF).

Effect: Lucky Lawn Mower gameplay 96.9% -> 97.2% (5-bit). Star Ally, Lonely
Island and Scramble are BYTE-IDENTICAL to the previous core, palette RAM and
framebuffer both -- they are 2 KiB carts, so their mask is unchanged. Lonely
Island DMAs its whole palette every frame, so this is the control that
matters.

This did NOT fix the opening -- LLM stages its palette at $0400, inside the
low 2 KiB -- but it is a real bug for any 4 KiB cart that stages higher.

### 67b. Everything in the palette pipeline is now PROVEN correct for the opening

`tools/score_5bit.sh <builddir>` scores the LLM opening and gameplay against
gg.png / lawn.png in 5-bit space (section 60). Baseline 82.3% / 96.9%.

* **Palette RAM is faithful.** The game stages its palette at NES RAM $0400,
  and that buffer is byte-for-byte what vt_palette_ram holds (all four BG
  groups). Our DMA copies it exactly. There is no palette scramble -- which
  retracts sections 61, 63 and 65c's write-address hypothesis.
* **$4119 is not involved.** Returning $00, $08, $10 and $18 all give the
  opening 82.3%: LLM never reads it. (Lonely Island's $4119 = $18 fix, which
  picks which of its two palette-upload routines runs, stays correct for LI.)
* **The decode is correct.** Applying alternative plane decodes to banks 0-3
  ONLY -- swapping p0p1 with p2p3, or using the 8-bit layout -- drops the
  opening from 82.3% to 7.6% and 5.7% while gameplay stays at 97%. The
  current 16-bit decode is decisively best for these banks too.
* **The attribute is 0.** Read the live attribute table: every byte is $00,
  written by the game. Section 58's attribute bug is not involved here.
* **The band is background**, not sprites (sprite pixels are 1-14% of two
  rows -- the mower).
* **It is not animation phase.** Scored 200 frames f100-f896 against gg.png:
  best 84.9%, no frame near 100%.

### 67c. What is left

The error is sharply localised: NES rows 16-143 (the sky) are **0% wrong**;
**every wrong pixel is in rows 144-223**, the fence-and-ground band.

**Every colour the reference shows in that band already exists in our LLM
table** -- (1,5,0)=$20, (10,10,10)=$00, (10,13,0)=$29, (22,22,22)=$39,
(31,19,0)=$09, (0,15,0)=$19, (0,7,0)=$27 -- just at indices the band's pixels
do not select. With the palette RAM, scatter, decode and attribute all proven,
the only thing that can differ is the pixel VALUES the band's tiles produce,
i.e. **which CHR bytes those tiles are fetched from.** The sky and the band
may come from different CHR pages; check whether the band's pages resolve to a
different ROM region than hardware uses. Start from the nametable indices of
the band's tiles and the page each selects.

Tool traps recorded this session: ROM-coherence mapping cannot locate graphics
(section 66e); the vt_bk extension slots are unused on this game (all bank 0)
because BKEXTEN is clear, so the BG comes from vt_chr4_assemble.

## 68. ON THE 16-BIT BUS, PLANES 1 AND 2 WERE SWAPPED -- the opening is FIXED (s21b57)

[stated] Michael re-supplied the opening reference as 1.png (256x208 = NES rows
16-223 of the same scene as gg.png, same 18 colours) and said the palette was
still wrong. It was -- and the cause had been hiding under every "confirmation"
this project made.

### 68a. How it was found (reusable)

1. **Palette-independent tile matching.** Reduce every 8x8 tile to its
   PARTITION (which pixels share a colour, relabelled in first-seen order) and
   search every 32-byte tile in the ROM for the same partition. The reference
   band aligns at (0,0); **138 of 205 band cells use EXACTLY the tile we fetch**
   (the rest are animated grass frames and the mower -- a moment difference).
   So we fetch the right tiles from the right place: section 67c's guess was
   wrong.
2. **Value -> colour on provably identical tiles** is then non-circular: our
   value comes straight from the ROM, the colour straight from the reference.
   Values 0,1,7,8 matched; 2-6,12,13 did not; every value was 100% consistent.
3. **The trap that hid it for sessions:** "gameplay scores 100%" was treated as
   proof the index scatter was right. It proves nothing -- the LLM table was
   FITTED to gameplay, so it absorbs any scatter error. The same applies to
   the VG table and the VG menus.
4. **The table-free test.** For each candidate plane order (24 orders x 16
   inversion masks = 384), ask whether the opening and gameplay can SHARE one
   palette: collect index -> colour demands from both screens and count indices
   asked for two colours. Result:

       p0->bit0 p1->bit5 p2->bit1 p3->bit6    0 conflicts
       next best                               6
       CURRENT (p0,p1,p2,p3 -> 0,1,5,6)       7

   **Planes 1 and 2 are the other way round on the 16-bit bus.** The byte pairs
   are {p0,p2} at +2r/+2r+1 and {p1,p3} at +16+2r/+17+2r -- address bit 0 (byte
   within the 16-bit word) is plane bit 1, address bit 4 is plane bit 0.
5. **Confirmed offline:** one table refitted to BOTH screens scores 100.0% /
   100.0% under the swap; the old order cannot exceed 89.6% on the opening
   however the table is fitted.

**Why nothing caught it before:** s21b15 derived the 16-bit layout by
minimising STRIPING. Exchanging p1 and p2 preserves every edge, so striping is
blind to it. Every table fitted afterwards absorbed the error, so each fitted
screen looked right and every OTHER screen came out wrong.

### 68b. What changed

* `vt_chr4_assemble` (the NON-extension 4bpp BG path): 16-bit plane offsets
  are now {+0, +16, +1, +17}, row stride 2. The 8-bit bus is unchanged
  ({+0,+8,+16,+24}). Offsets are frame constants hoisted out of the loop so the
  8-bit path's cost is unchanged.
* `vt_assemble_page_to` (the EXTENSION path) is deliberately **NOT** swapped --
  see 68c. Offsets are the original decode, hoisted.
* `vt_compat_rgb555_llm` refitted from lawn.png + gg.png together: 18 indices,
  all 18 unanimous across both screens.
* `vt_compat_rgb555_vg` refitted: every pixel of the three calibrated menus
  re-voted with target colour = the previous core's calibrated render and index
  = the new decode's palette entry. 28 indices, 23 unanimous.

**Result (5-bit, tools/score_5bit.sh):** Lucky Lawn Mower opening
**82.3% -> 96.5%**, gameplay 97.2% -> 96.8% (the residue on both is the mower,
animated tiles and the HUD at different moments than the references). The
opening's fence is tan, its ground grey cobbles and green grass, as on
hardware. Controls: Lonely Island and Scramble BYTE-IDENTICAL (palette RAM and
framebuffer); Star Ally renders exactly the same 4 distinct frames over 800
(all present in the old run) with a blink phase shifted by a frame or two.

### 68c. The extension path is left unswapped -- and why that is a judgement

Applying the swap to BOTH paths scrambled the VG Pocket title logo. The title is
the only calibrated screen on the extension path (BKEXTEN set), and its
reference, vg.png, is a clean native framebuffer dump the old decode matched at
95% -- the strongest reference in the project. The swap is proven only on the
non-extension path (Lucky Lawn Mower runs BKEXTEN clear). So: proven path
changed, unproven path left alone. Menus after the fix, versus the previous
core: title 99.6% unchanged, list 99.9%, **category 91.4%** -- the category's
sign text and paw icons changed colour. That screen is on the proven path, but
its old calibration came from an INTERPOLATED capture (section 44) and it was
always the weakest-scoring screen. **Re-score it against the real category
capture before trusting either version.**

Do not "fix" the VG mismatch by swapping the extension path too without a
reference for an extension-path screen OTHER than the title. The circular check
("does the old render reproduce?") always favours the old decode, because the
old render IS the old decode's output -- do not use it as evidence.

### 68d. Independent evidence for the VG refit

tools/palette_sanity.py on the VG table: suspect entries **9 -> 7**, and
**$1A is no longer flagged.** Section 57c recorded $1A as a "known TRUE
per-console difference" (blue-violet on the VG Pocket, green on Lonely Island's
console). It now sits correctly in its hue column, so that "difference" was
very likely this plane swap all along. $28/$35/$37 remain flagged and remain
unused by any menu.

## 69. The swap belongs on EVERY 16-bit path; the title "scramble" was a cache stomp (s21b58)

[stated] Michael: the opening was "almost there -- most of the problem is only
the lawnmower sprite colour", and supplied vgp.png (category) and vgp2.png
(game list) as the correct VG palettes. vgp.png is an interpolated window grab
(3514 colours); vgp2.png is close to clean and fits our frame at 97.3%.

### 69a. Sprites need the swap -- the mower proves it

Lucky Lawn Mower runs $2010=$4E, which sets SPEXTEN, so its sprites go through
the EXTENSION assembler that section 68 had deliberately left unswapped.
vt_assemble_page_to gained a `swap16` parameter (callers pass VT_SPR_SWAP16 /
VT_BG_SWAP16). With the swap, the mower matches the reference exactly --
black tyres with white rims, red seat, orange body -- where the old build drew
blue wheels. LLM gameplay 96.8% -> 97.3%.

### 69b. The VG title "scramble" was a STOMP, not the decode

Section 68 left the BKEXTEN background path unswapped because swapping it
garbled the VG title's "Pocket" logo. Decisive test: build with
`-DFORCE_BK_REPAIR` (re-assemble every extension slot every frame). **With
forced repair the logo is perfect.** So the swap was right; PocketNES's 2bpp
tile cache was overwriting some logo tiles and the extension slots' stomp
detector did not notice.

Why: vt_bk_slot_fill used "first nonzero word" as the signature (s20, chosen to
catch zero-stomps). A 2bpp stomp can coincidentally write the SAME value at
that one word. Under the old plane order it happened not to for the logo;
under the new one it did. **Fix (in the tree): prefer the first word with
high-plane bits (`x & 0xCCCCCCCC`)** -- the 2bpp converter only ever writes
values 0-3, so it can never reproduce such a word and every stomp is caught.
Pages with no high-plane bits fall back to first-nonzero (still catches
zero-stomps). vt_chr4_assemble has always chosen its signature this way.

The FORCE_BK_REPAIR hook stays in the tree as a diagnostic: if a BKEXTEN
screen ever looks "scrambled", build with it first -- a clean result means a
stomp, not a decode fault.

### 69c. The VG table, refit against real captures

Votes per palette index from: the title (target = the previous core's render,
which matched clean vg.png at 95%; weight 2), the game list (target = vgp2.png;
weight 2), and the category (target = vgp.png, interpolated; weight 1). Only
indices with a >=50% winner change. Changed: $00 -> grey, $0C -> teal,
$10 -> grey (the chains), $11, $12 <-> $1A (they SWAP: $1A green, $12
blue-violet, as on canonical NES hardware), $13 -> purple (the selected row),
$21, $27 -> orange (the selected paw).

Result: the game list now matches vgp2.png -- full purple row, orange paw on
the selected entry, green paws, grey bars -- and the category matches vgp.png:
blue "Action" paw, white paws, white text, grey chains. The title is visually
unchanged: 20 pixels move by more than a colour distance of 6 (the small TM
mark); the rest are one-unit shade settles toward the new captures.

**palette_sanity.py: VG suspects 9 -> 4 ($04 $28 $35 $37).** $12 and $1A --
recorded in section 57c as a "true per-console difference" -- are both
cleared. That difference was this plane swap all along.

### 69d. Evidence discipline that decided it (reuse)

* The table-free cross-screen test with a HARD threshold said "title
  unswapped" 5-0; re-run with COLOUR DISTANCE it said "swapped" (0.68 vs 0.91)
  and resolved the one real hue conflict ($27 grey vs orange). The threshold
  had excluded exactly the discriminating index. **Prefer distance over
  exact-match thresholds when votes come from interpolated captures.**
* Never use "does the old render reproduce?" as evidence for the old decode --
  the old render IS the old decode's output, so it always wins (circular).

### 69e. Speed is unchanged by all of this

Same harness, both builds, opening with no input, four windows:
previous 92.7/92.7/93.3/93.7%, final 92.0/92.7/93.0/92.7% -- within one frame
per 300. Gameplay readings (which tap Start/A) alternate between 100% and
~95% on the final build in some windows, but assembler call counts are
IDENTICAL (400 per 400 frames, zero repairs) in both builds: the runs drift
into different game states once timing differs by a frame. Do not compare
builds on the tapping gameplay harness; use the no-input opening.

Controls: Lonely Island and Scramble byte-identical to the previous core;
Star Ally renders exactly its same four frames (blink phase shift only).

## 70. Three more VT03 carts fixed; Scramble's shot diagnosed (s21b59)

[stated] Michael supplied Aero Gyrodine, Hex City X, Add 'em Up (graphical
glitches) and Scramble (works, but fired shots are invisible). All four are
NES 2.0 mapper 256, extended console type $07 (VT03).

### 70a. Add 'em Up: a separate CHR ROM, plus submapper 1 -- both unimplemented

Header: mapper 256 **submapper 1**, PRG 64 KB, **CHR 216 KB** -- OneBus carts
normally keep CHR inside PRG. Two faults, both documented as open work:

* **CHR source.** NintendulatorNRS (h_OneBus.cpp load()) reads CHR from the
  CHR ROM whenever one is present, rounded up to a power of two for masking,
  and from PRG-ROM otherwise. PocketNES's loader already set vrombase to the
  CHR ROM, but EVERY VT CHR fetch in ppu_vt.c used rombase/rommask -- so the
  tiles were built from the game's own program code (pure noise on screen).
  Fix: `vt_chr_src`/`vt_chr_mask` (ppu_vt.h), set in loadcart.c for internal
  mapper 253; all six CHR fetch functions use `vt_chr_base()`/`vt_chr_mask_get()`.
  PocketNES's own path is given the CHR-RAM-style setup every other VT cart
  already runs with. For CHR-in-PRG carts the source is rombase/rommask as
  before -- byte-identical.
* **Submapper byte-mangling** (IMPLEMENTATION_NOTES.md listed it as "the
  remaining work"). From reference/nrs/mapper256.cpp, all three tables are now
  implemented: ppuMangle on $2012-$2017 writes (ppu_vt.c), mmc3Mangle on the
  $8000 bank-select value (vt_mmc3_forward, applied before use exactly as NRS
  does), cpuMangle on $4107/$4108 (vt_reg_write; only submapper 2 differs).
  Submapper 1 = {1,0,5,4,3,2} for PPU and {5,4,3,2,1,0,6,7} for MMC3.

Result: Add 'em Up renders its title scene and a clean puzzle board (numbered
tiles, numbers/tiles-left panel, restart/music/pause/quit).

### 70b. Aero Gyrodine and Hex City X: COLCOMP in 16-colour mode

Both run `$2010=$86` = COLCOMP | SP16EN | BK16EN. COLCOMP (12-bit colour:
`Palette[TC|0x80]<<6 | Palette[TC]`) was only implemented for the 2bpp layout;
vt_build_16color_palette bailed on it, so pixel values 4-15 read palette
entries nobody filled and the screen collapsed to the backdrop. Fixed:

* vt_build_16color_palette composes hi<<6|lo through vt03_palette_lut in
  COLCOMP mode (same index scatter);
* VRAM_pal_hi / vt_palette_write_hi keep all 128 high-bank entries (were
  masked to 32, aliasing $3FA0-$3FFF onto $3F80), with NRS's mirroring rule;
* vt_obj4_overlay no longer bails on COLCOMP -- it is a colour mode, not a
  tile format;
* the palette-DMA fast path REFUSES high-bank destinations ($3F80+), which now
  go byte-by-byte through VRAM_pal_hi. (First attempt widened the fast path's
  window to 256 bytes instead; that changed Star Ally, because DMAs that used
  to take the per-byte path -- with its side effects, e.g. vt_palette_active
  -- took the fast path. Bisected to ppu.s and replaced with the narrow bail.)

Result: both play in full colour (Aero Gyrodine's round card and gyrodyne over
a starfield; Hex City X's "PLAYER 1 READY" and rainbow pseudo-3D ground). Both
wait at a near-blank screen for **a Start TAP** (press and release); a held
Start does nothing. Their pre-Start screens are genuinely near-empty in the
nametable with sprites off -- believed correct, unverified without a capture.

### 70c. Scramble's invisible shot -- diagnosed, NOT fixed

Traced end to end in ONE frame (earlier cross-frame comparisons misled me
twice -- the GBA OAM is not index-aligned with NES OAM: here GBA index =
(NES index + 44) mod 64; match sprites by POSITION, in the SAME frame):

* the shot is NES OAM #63, tile $5B, level with the ship; it IS converted, to a
  GBA sprite with the same attributes as the ship's parts (8x16 double-size
  affine, palette 0, priority 2);
* its GBA tile is empty except ONE pixel on texture row 7 (colour $0252, a
  visible olive -- not a palette problem);
* sprite affine matrix: pa=256, **pd=336**. For an 8x16 double-size OBJ the
  texture centre is row 8, and texY = floor(pd*iy/256)+8 samples rows
  0,1,2,4,5,6,8,9,10,11,13,14,15 -- **rows 3, 7 and 12 are never drawn.**
  Because floor rounds negatives down, the row just above centre (row 7) is
  skipped for EVERY pd > 256, so no matrix value can recover it.

So the shot can never appear in SCALED_SPRITES mode -- Michael's guess
("could be a scaling issue") is right. An attempt to confirm the sampling
model pixel-by-pixel against a ship sprite was inconclusive because OAM was
read after the frame rendered (it describes the next frame). Redo it by
reading OAM BEFORE runFrame.

The background already avoids permanent loss: scale75 rotates which row it
drops each frame. Sprites use one fixed matrix, so the same rows vanish
forever. FIX DIRECTION (a cross-cart change -- verify on every cart): rotate
the dropped sprite rows per frame, e.g. by offsetting the NES tile's rows
inside the 8x16 texture per frame and compensating the screen Y. Display modes
1/2 were not tested successfully (the emuflags write did not read back).

### 70d. Controls

Star Ally, Lonely Island, Scramble, Lucky Lawn Mower and the VG Pocket are all
BYTE-IDENTICAL (palette RAM + framebuffer) to s21b58 at f700. Star Ally, Lonely
Island and the VG Pocket render only frames the old core rendered; Scramble
and Lucky Lawn Mower show a few new frames over long runs -- timing drift from
the larger core, same as before.

### 70e. The ROM-less-harness trap bit again -- use tools/restage.sh

After a rebuild I re-copied only two ROMs; builder.py then "Successfully
compiled 0 game(s)" for the controls and every regression check looked like a
catastrophic failure (a frozen single frame). tools/restage.sh re-copies
builder.py, every .nes and the harness sources: run it after EVERY build.

## 71. Video DMA from ROM (FIXED); the raster-split CHR gap (s21b60)

[stated] Michael supplied reference captures: aero.png, hex.png (the real title
screens), add.png and add2.png (Add 'em Up title and puzzle screen), bug.png
(our puzzle screen: grid fine, top strip garbage) and shot.png (Scramble's shot
is a small dot in front of the ship).

### 71a. FIXED: VT video DMA read its source from RAM only

A DMA_LOG build logged every video DMA. Both title screens copy their ENTIRE
nametable straight out of PRG-ROM: Aero Gyrodine $A000-$A3FF, Hex City X
$8400-$87FF, four 256-byte DMAs into $2000-$23FF. Our DMA loop read
`NES_RAM[src & mask]` for every source, so the nametable stayed empty.
NintendulatorNRS runs VT DMA through the CPU core's DMA engine, which reads
over the CPU bus. Fix (Mappers/mapVT.s): sources >= $8000 read through
memmap_tbl -- the per-8K biased pointers the 6502 core fetches opcodes with;
below $8000 is the old RAM path unchanged. vt_pal_dma_fast declines non-RAM
sources so they take that path.

Result: **Hex City X's title is correct** (logo, planet, starfield, ship).
Add 'em Up's title now appears too (it was a white frame). All five controls
byte-identical to s21b58; frame-set drift numbers identical to s21b59.

### 71b. NOT FIXED: raster-split CHR banking in 4bpp mode

**Add 'em Up** writes $2016/$2017 twice per frame ($08/$0A then $04/$06).
**Aero Gyrodine's title** is a THREE-band split done by its IRQ handler at $A500
(title state: $FF == $E0; split index in $50): band 1 uses the NMI's
$08/$0A; IRQ 1 acks ($4103), sets $0C/$0E and re-arms the VT timer for 72
lines ($4101/$4102 = $48, $4104); IRQ 2 sets $10/$12. The reference cells come
from banks 8-11 AND 12-15 at once -- impossible from one bank set.

Two gaps, one per game:
* **4bpp CHR is assembled once per frame** from the frame's final banks
  (vt_chr4_assemble), so every band but the last is drawn with the wrong
  graphics. Needed: record the bank set active per scanline, assemble one 4bpp
  set per distinct bank set in the frame, and switch BG0CNT's char base at the
  split lines through the existing per-scanline dmabg0cntbuff. Check the VRAM
  budget first (one 256-tile 4bpp BG set = 8 KB).
* **Aero's timer IRQ does not fire on schedule.** Logged over single frames,
  neither $2012-$2017 nor the MMC3 $8001 path sees any write mid-frame; the
  split's writes only land every ~10 frames. Investigate the VT timer (the
  $4101-$4104 path, section 17's vblank-skip timer) before the banking work
  can help Aero.

TRAP recorded: I first compared our frame 200 against aero.png and concluded
the bank-to-address rate was wrong. It was not -- the title animates, and
frame 200 was a different band's banks. Two full theories (a 2x rate, an
AND/OR mask) were tested and disproved before the IRQ disassembly showed the
split. **When a screen's banks differ from the reference's, check whether the
game changes banks over time or mid-frame before touching the address math.**

### 71c. Scramble's shot -- confirmed by the reference

shot.png shows the shot as a small dot in front of the ship, consistent with
section 70c: a one-pixel sprite on texture row 7, which the fixed sprite
affine matrix never samples. Still open.

## 72. ROOT CAUSE of the flickering titles and slow Add 'em Up: a 4bpp rebuild storm (s21b61)

[stated] Michael: the Aero Gyrodine and Hex City X titles now render but are a
"flickering mess"; Add 'em Up still has rendering problems.

### 72a. The measurement that cracked it: NMIs per 60 frames

timeout.s's nmi_handler increments a DEBUG byte at 0x020007DF. Delivered NMIs
per 60 GBA frames: Aero Gyrodine **2**, Hex City X **3**, Add 'em Up **23**,
Star Ally 51, Lucky Lawn Mower 56, Lonely Island / Scramble / VG Pocket 60.

**This number IS the emulation speed** -- one NMI per completed NES frame.
Aero's title runs at ~3% speed, Hex's at ~5%, Add 'em Up's puzzle screen at
~38%. (Star Ally's 85% and LLM's 93% match the speeds measured earlier; those
were genuine timeline speed all along. An earlier claim this session that they
were "missed NMIs, not slowness" was wrong -- the NMIs are missed BECAUSE the
timeline is slow.)

### 72b. Hypotheses tested and DISPROVED (do not repeat)

1. **$2002-read NMI suppression windows** (stat_R_suppressvbl and
   vblank_handler_2's VBL-flag recheck). Narrowed both for VT carts: NMI counts
   unchanged, to the frame. Reverted.
2. **Timeout-queue corruption / r8 (`cycles`) clobbered by C.** vt_reg_write is
   Thumb code that never touches r8; the dispatcher detaches nodes correctly.
   The queue walked at frame end is intact -- just FROZEN (Aero: identical
   events AND timestamps across five frames; Lonely Island advances 89,342 per
   frame).
3. **The speed hack.** `speedhack_pc` is 0 on Aero, Hex and Star Ally.
4. (s21b60) a wrong bank->address rate and an AND/OR mask -- see section 71b.

Instrumenting the vblank pipeline stage by stage showed vblank_handler itself
runs 2/60 on Aero: nothing suppresses anything; the NES timeline simply is not
advancing. Disabling VT-timer scheduling raised vblank installs from 2 to 41
per 60 -- the timer's splits are involved, but as the TRIGGER, not the bug.

### 72c. The actual cause (tools/arm_profile.c on Aero's title)

    vt_chr4_assemble        47.8%
    vt_chr_sync_flush       28.7%
    vt_chr4_copy_to_vram    2.9%
    6502 opcode handlers    8.0%

The split games change CHR banks 2-3 times per NES frame. vt_chr4_rebuild_if_dirty
runs from the GBA vblank hook, which is NOT phase-locked to the NES timeline,
so the bank state it samples differs from one GBA frame to the next -> dirty
every frame -> a FULL re-decode of all eight 4bpp pages from ROM, plus the 2bpp
deinterleave and a 16 KB VRAM copy, every frame. That starves the 6502 (8% of
the ARM), so the NES timeline crawls; NMIs and the timer IRQs land every ten-
plus frames; each frame shows whichever band's banks happened to be live ->
the flicker. The source even anticipated it (ppu_vt.c, vt_chr_sync_from_prg):
"mid-frame CHR bank raster tricks lose sub-frame granularity -- acceptable
until something needs it." Aero Gyrodine, Hex City X and Add 'em Up need it.

### 72d. The fix -- a VRAM redesign, planned, not started

Correctness AND speed need the same two pieces:

1. **Cache assembled 4bpp tile sets per bank configuration** so switching
   between a game's known band settings costs a lookup, not a re-decode.
2. **Switch BG0CNT's character base per scanline** at the split lines (record
   the NES scanline of each bank write; map through dma0buff; write the
   existing per-scanline dmabg0cntbuff).

Constraint: BG VRAM is essentially full. Lonely Island uses every 2 KB block but
14-15; blocks 16-31 are PocketNES's 2bpp BG tile cache. One 256-tile 4bpp BG
set is 8 KB and must start on a 16 KB char-block boundary. The likely route is
to repurpose the 2bpp BG cache region for 4bpp VT carts, where the 4bpp
overlay already overwrites what it produces -- which means taming its
stomp/repair cycle (sections 13, 69). A cross-cart change: verify on EVERY
control with tools/restage.sh and the frame-set test.

A cheaper interim that would only fix SPEED (not the split visuals): sample
the bank state at a fixed NES-timeline point (e.g. render_end) instead of the
asynchronous GBA vblank, so a split game's snapshot is identical every frame
and never marked dirty. That would show one band's graphics everywhere --
stable instead of flickering, full speed instead of 3-38%.

## 73. Rebuild-storm FIXED; raster-split slots WIP (s21b62) -- see also CLAUDE.md

[stated] Michael asked for the full fix, then asked to wrap up so the project
can move to Claude Code. What shipped, and what is left half-done:

### 73a. SHIPPED: the rebuild storm is gone
vt_chr_sync_flush (run from the GBA vblank hook, not phase-locked to the NES)
now builds the primary tile set from the registers at the END of the last NES
frame (`vt_frame_reg`) whenever that frame was a raster split
(`vt_split_frame`); non-split frames use the live registers exactly as
before. A stable split therefore never marks the set dirty.

Speed, NMIs per 60 frames: **Add 'em Up 23 -> 60** (full speed), Aero Gyrodine
title 2 -> 39, Hex City X title 3 -> 35. Controls: Lonely Island, Scramble and
VG Pocket byte-identical; Star Ally renders only previously-seen frames; Lucky
Lawn Mower scores unchanged (96.5% / 97.1%). Visually each split screen now
shows ONE band's graphics across the whole screen, stably -- not yet correct.

### 73b. SHIPPED (always on): band recording
* ppu.s vt_ppu_extended_W -> `vt_ppu_write_marked` (ROM wrapper, bl_long:
  the IWRAM section cannot reach ROM with a plain bl) -> vt_ppu_reg_write,
  then for $2012-$2017 `get_scanline_2` + `vt_band_mark(line)`.
  get_scanline_2 reads r8 (cycles); vt_ppu_reg_write is Thumb and preserves it.
* `vt_bands_frame_end()` is called from the top of vt_palette_rebuild_gba,
  which newframe_nes_vblank calls first at NES line 242 -- bg0cntbuff is
  complete (catch-ups ran in turn_screen_off at line 240) and not yet swapped.
  It is called from there because the VRAM code section (`.vram1`, 4 KB at
  0x06003000) that holds newframe_nes_vblank was full: adding a bl_long there
  overflowed it by 8 bytes.
* Writes at line >= 240 set the NEXT frame's band 0 (vt_band_fresh).

### 73c. WIP, OFF by default: `-DVT_SPLIT_SLOTS`
Assigns non-primary bands to BG char blocks 2/3 (two-entry LRU,
vt_split_slot_get, decoded by vt_chr4_assemble_page_vram), rewrites bits 2-3
of bg0cntbuff for their lines, and re-decodes a slot if its signature word
loses its high-plane bits (vt_split_repair). Basis: on VT carts PocketNES's
own BG group cache (agb_bg_map) only ever occupies slot 0, and the VT pipeline
puts the $1000 half in slot 1.

With it on, Aero Gyrodine reached 59/60. **Add 'em Up hangs** (0 NMIs, blank
screen after ~f200) right after its first two slot assemblies; with the slot
writes skipped it runs at 60/60. So char blocks 2/3 are not free on that cart
-- it is the only one with a separate CHR ROM, so suspect a path that uses
that VRAM when vrompages was set (PocketNES's 1K bank cache / bank_search,
or loadcart's buffer placement). Find the occupant before enabling.

## 15b. The sprite path is NOT the Scramble bug -- the horizontal crop is (s21b40)

Open item 4 said "Scramble's BG pipeline is verified complete, so look at the
SPRITE path". The sprite path checks out. Two things that LOOK wrong and are
not, so nobody re-opens them:

**Sprite X is `nes_x - 12`, the BG is `nes_x - 8`, and that is correct.**
In SCALED_SPRITES mode `update_sprites` emits DOUBLE-SIZE affine OBJs
(attr0 rot/scale=1 + double=1; matrix 0 is PA=0x0100, PB=PC=0, PD=0x0150).
A double-size OBJ's content sits 4 px inside its 2x box, which is exactly what
the `ands r1,r6,#0x100 / add r1,r1,#SCREEN_LEFT<<6` pair compensates for --
r1 becomes 0x300, and `subs r1,r3,r1,lsl#18` subtracts 12 from the X byte.
Net visible left edge = nes_x - 8, same as the BG. SCREEN_LEFT is 8 for both.

**The `@FIXME` on `windowtop` in update_sprites is benign in SCALED mode.**
Sprites index YSCALE_LOOKUP with `windowtop` (measured 0) while scale75 uses
`windowtop_scaled6_8` (measured 16). The two disagree, but the lookup table
already bakes the offset in -- measured `yscale[y] = 0.75*y - 17`. Verified
empirically instead of by arithmetic: the 8x16 sprite at NES y=56 gets an OBJ
box covering GBA lines 31..43, and the BG puts NES rows 57..72 at GBA lines
31..42. Aligned to within PD rounding. (Do NOT measure this by comparing the
sprite's LIT ROWS to the box -- the glyph does not fill its 8x16 slot, and
that mismatch reads as a 3-line offset that is not there.)

**What is actually wrong: 16 NES columns are deleted unconditionally.**
12 of Scramble's 18 on-screen sprites are one object -- tile $83 at NES x=248,
at y = 15, 31, 47 ... 191, i.e. a continuous 8 px wide strip down NES rows
16..207. It is a right-edge border. We never draw it: the horizontal window is
hard-wired to NES columns 8..247 (SCREEN_LEFT is a compile-time 8 and BG0HOFS
reads back a constant 8 on every line), so those OBJs land at GBA x=240..247,
one pixel past the screen. There is NO horizontal pan -- the L/R handling in
update_sprites moves `windowtop`, which is VERTICAL.

So section 16's horizontal note is understated. It is not "16 columns happen to
be off-screen at whatever scroll the game set"; it is a fixed 8-left/8-right
crop, and on Scramble it removes a real game element rather than overscan.
Item 4 and the horizontal half of item 1 are the same bug.

## 16b. WHAT THE SHIPPED TARBALL DOES *NOT* CONTAIN (s21b40)

The session tarball carries src/, tools/, reference/*.xml, the DATASHEET
digests, the docs and testroms/ (Add 'em Up, Push the Ball, Scramble, Table
Soccer, Time Pilot). It does NOT carry, and cannot carry:

* the VG Pocket 50-in-1 4 MB image
* Star Ally and Lonely Island (the two regression CONTROLS)
* every reference capture -- vg.png, 1.png, 2.png, 3.png, 11.png

Without those, none of the VG scoring in sections 36-45 can be reproduced and
neither control can be checked for byte-identity. A session that only extracts
the tarball can build, link, run check_globals and exercise the five testroms,
and nothing more. Ask for the images up front rather than discovering this
after the toolchain is up.

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

## 19. COLCOMP=1 with 16 colours, and the palette hi-byte width (fixed s21b10)

Two separate defects kept the new colour mapping from working with 16-colour
planes. Add 'em Up runs $2010 = $86 -- COLCOMP=1, BK16EN=1, SP16EN=1 -- and hit
both.

**Defect 1: the 16-colour builder refused to run.** vt_build_16color_palette
bailed on COLCOMP=1 with the comment "handled elsewhere".
vt_palette_rebuild_gba does handle COLCOMP=1, but only in the 2bpp layout --
entries 0..3 of each GBA bank. A 4bpp game needs all 16, so pixel values 4..15
indexed slots nothing ever wrote and rendered black. Measured before the fix:
BG bank 0 held 0000 7ee7 75c0 0539 followed by twelve zeros.

The scatter index is the same in both colour modes; only the lookup differs, so
the builder now calls vt_pal_entry_colour(idx), which branches on COLCOMP:

* COLCOMP=0 -> nes_index_to_bgr555(lo). Do NOT route this through
  vt03_palette_lut: its first 64 entries are all 0x0000 (the SAT=0/LUM=0 row),
  which is precisely the 0.4 "every game boots black" bug.
* COLCOMP=1 -> vt03_palette_lut[(hi << 6) | lo], hi taken from idx | 0x80.

**Defect 2: the hi byte was masked to five bits.** A colour is the pair
($3F00+k lo, $3F80+k hi) for k = 0..$7F. The lo path in ppu.s deliberately
stores the full 7-bit offset ("games upload the whole 128-byte palette before
enabling BK16EN"), but the hi path did `and r0,addy,#0x1f` and
vt_palette_write_hi re-masked with `(offset & 0x1F) | 0x80`. Every hi byte for
entries $20-$7F was therefore discarded AND aliased onto entries 0-$1F -- and
those upper entries are exactly where the scattered 16-colour indices live.
Both sides now keep 7 bits, and the backdrop mirroring in the hi bank is
confined to its low 32 entries, matching the lo half.

Result: all 16 entries per bank carry real colour and Add 'em Up went from
~3.2K lit pixels to ~18K.

Verification notes worth repeating: Lonely Island stayed bit-identical and the
Star Ally split histogram stayed at line 138 x 401 with zero changes. Star
Ally's TITLE does differ frame-for-frame, but that screen fades in and the two
builds are a few frames apart in the fade (b9 settles at ~11951 lit pixels by
f79; b10 climbs 9289 -> 11975 by f90). The settled menu differs by 138 bytes of
153600 and scores marginally closer to the 11.png reference than before. Judge
animated screens by a settled frame or a sweep, never by one frame.

## 20. CHR ROM vs CHR-in-PRG: which image VT tile fetches address (fixed s21b11)

Every VT CHR fetch in ppu_vt.c read from `rombase` -- the PRG image. That is
right for a OneBus cart, which keeps character data inside PRG (vrompages == 0,
so loadcart points vrombase at the 8 KB CHR-RAM in EWRAM): Star Ally, Lonely
Island, Time Pilot, Scramble, Push the Ball. It is wrong for a dump that has a
REAL CHR ROM. Add 'em Up is 64 KB PRG + 216 KB CHR, so loadcart sets
vrombase = rombase + romsize -- a second image in cart space -- and the game's
bank numbers index that one. Reading it from rombase fetched every tile 64 KB
too early: "just garbled graphics".

Fix: vt_chr_src_base() / vt_chr_src_mask() pick vrombase+vrommask when
vrombase points into cart space (>= 0x08000000) and rombase+rommask otherwise,
and every fetch site uses them (both sync-copy paths and all four assemblers).

Evidence it worked: with rombase the screen was a uniform solid fill and not
one drawn tile could be found anywhere in the ROM; with the CHR ROM the screen
is a structured grid (it is a puzzle game) and the drawn tiles match CHR data.
Lonely Island stayed bit-identical, Star Ally's menu identical and its split
histogram still line 138 x 401 with zero changes -- as expected, since for
CHR-in-PRG carts the helper returns exactly what the code used before.

## 21. Scramble: the "empty palette banks" lead was a RED HERRING (s21b26)

The old note here said Scramble's GBA BG palette banks 1-3 were entirely zero
and hunted for whoever zeroed them. They ARE zero -- and it does not matter:
ALL 960 of its cells use ATTRIBUTE 0, so banks 1-3 are never selected. Dump the
attribute table before theorising about attribute handling (the same mistake
was made on the VG category menu, section 32).

Its palette RAM is coherent, just laid out differently from the VG Pocket's:
Scramble writes $3F00-03, $3F10-13, $3F20-23, $3F30-33 -- two 32-colour palette
banks, each with a BG half and a sprite half (bit 4), four colours per group.
The VG Pocket instead uses $3F00/$3F20/$3F40/$3F60. Both fit our model.

BG pipeline checked end to end on the current core: of the 114 cells with a
non-zero name, 114 have a non-zero GBA map entry and 114 have real pixel data
in the tile they point at. There is no missing BG content.

STATUS: Michael reported a glitchy title screen, but that was BEFORE the
s21b15/b17 bus-layout fixes and the s21b22 geometry correction, and there is no
capture of Scramble to score against. Ask him to re-check it on a current build,
and for a native-resolution capture if it is still wrong -- without one this is
guesswork, exactly as it was for the VG menus.

## 22. VG Pocket 50-in-1, and 4 MB carts (booting since s21b13)

The dump is a 4 MB raw OneBus image: no iNES header, 6502 vectors in the last
six bytes (NMI $F9E0, RESET $F862, IRQ $FA80), and the reset code reads as
clean 6502 RAW -- `CLD / SEI / LDX #$FF / TXS / LDA #$00 / STA $2000 ...` -- so
this cart is NOT opcode-scrambled, unlike Star Ally. Valid vectors also sit at
the 2 MB, 1 MB and 512 KB boundaries, which is what a 50-in-1 assembled from
sub-images looks like, each keeping its own fixed bank on top. It is a mixed
bag of VT03 and possibly other OneBus variants, so expect per-title differences
in $2010 mode once individual games are launched.

Wrap a raw image like this in a NES 2.0 header -- mapper 256, submapper 0, no
CHR, PRG = 256 x 16 KB pages (byte 4 = 0x00 LSB, byte 9 low nibble = 1):

    4E 45 53 1A 00 00 00 08 01 01 00 00 00 00 00 00

WHAT HAD TO CHANGE: loadcart.c parsed the NES 2.0 size correctly (256 pages)
and then capped page counts at 255, because `rompages` was a u8. 255 pages is
4080 KB -- 16 KB SHORT of a true 4 MB image -- so rommask became 0x3FBFFF and
the top bank, where the vectors live, was masked off; the CPU never started.

`rompages` is now 16-bit. The extra byte came from the filler that used to sit
after `fourscreen` in the globals block, so rompages(2) + vrompages(1) +
fourscreen(1) still totals four bytes and NOTHING after it shifts -- the one
way to widen a global in that block without a layout migration (section 14).
No assembly reads rompages; only C does, via asmcalls.h, where both `extern u8
_rompages` declarations became u16. The PRG cap is now 2048 pages (32 MB, the
GBA cart ceiling); the CHR page count is still 8-bit, so that cap stays.

Result: the cart boots and draws its menu ($2010 = $5E -- BKEXTEN, SPEXTEN and
both 16-colour planes). Regression set after the change: Star Ally title, menu
and Lonely Island f400 all BIT-IDENTICAL, split histogram still line 138 x 401
with zero changes, Push the Ball and Time Pilot unchanged.

Still unverified: whether the menu is pixel-correct, and whether individual
titles launch and run. Those need a capture to compare against.

## 23. The COLCOMP=0 compat palette is CALIBRATED, not derived (partly fixed s21b14)

`vt_compat_rgb555[64]` is not from a datasheet -- it was fitted in an early
session against reference captures, and it is wrong wherever no capture pinned
it down. Michael's vg.png (VG Pocket menu) exposed that: 16 distinct colours on
each side and only BLACK in common, while the SHAPES lined up (82% ink recall,
77% precision), i.e. right pixels, wrong colours.

HOW TO CALIBRATE AGAINST A CAPTURE (this method works, use it again):
1. Render the same screen headless and transform the capture into our geometry
   -- crop x+8, and take NES row (y*3)/2 for GBA row y (we drop every third
   line). Verify the offset by search; for vg.png dy=0 dx=8 is optimal.
2. Vote OUR colour -> REFERENCE colour, counting only pixels whose 3x3
   neighbourhood is uniform on BOTH sides. Edge pixels are noise; interiors
   give 94-100% confidence where the raw vote gives 20-57%.
3. Map colour->colour, NEVER colour->index: the table contains duplicate
   values (0x0120, 0x0200, 0x7F1F all appear twice), so inverting a rendered
   colour back to an index is ambiguous and will mis-attribute.
4. Discard any pair whose target is BLACK. Those are pixels we draw where the
   reference has none -- a structural fault, not a palette one. Folding them
   into the table would blank that colour everywhere it is legitimately used.
5. SCORE EVERY REFERENCE BEFORE KEEPING: exact-pixel match against vg.png,
   11.png (SA menu) and 2.png (SA gameplay). Entries are shared between games.

s21b14 applied five pairs -- 35AD->06BE, 1320->53A8, 0200->7CAB, 7C83->53A8,
00B2->06BE (six table slots, since 0200 is duplicated). Result: VG Pocket menu
38.8% -> 51.1% exact-pixel match, Star Ally menu 64.9% and gameplay 87.7% both
UNCHANGED. A second calibration pass then found no further colour mismatches,
so the palette is converged for this cart.

WHAT REMAINS on that screen is structural, not colour: we draw ~3.6K ink pixels
the reference lacks (including 312 in rows 0-19 where it is pure black) and
miss ~3.3K it has. Alignment is already optimal, so it is real extra/missing
content -- chase that next, not the palette.

## 24. 8-bit vs 16-bit CHR bus: the plane/row layout (fixed s21b15)

The wiki's "VT02+ CHR-ROM Bankswitching" final-address diagrams give TWO 4bpp
layouts, and we only implemented one:

  8-bit bus : ...TTTTTTPPRRR  row = addr bits 0-2, plane = bits 3-4
              -> planes at +0, +8, +16, +24 within the 32-byte tile
  16-bit bus: ...TTTTTTPRRRp  plane D0 = bit 0, row = bits 1-3, plane D1 = bit 4
              -> row r at +2r, +2r+1, +16+2r, +16+2r+1

Reading a 16-bit-bus cart with the 8-bit layout takes each row's halves from
different rows, which renders as heavy horizontal STRIPING. Metric that catches
it (ink recall/precision does NOT -- it barely moved, 82->83%): mean count of
pixels differing between vertically adjacent rows. VG Pocket menu measured 89.6
under the 8-bit layout, 33.2 under the 16-bit one, against 31.3 for the
reference capture itself.

GATING: bus width is a board property with no documented register. The
empirical gate is $2010 D6 -- listed as UNUSED in the VT03 datasheet, but SET
on our only 16-bit-bus cart (VG Pocket, $5E) and CLEAR on both 8-bit-bus carts
(Star Ally $1F, Lonely Island $0E). If a counter-example appears, move this to
a per-cart flag in builder.py's injected header instead of widening the guess.
Verified after gating: Star Ally menu and Lonely Island f400 byte-identical,
Star Ally title byte-identical at f100/f120/f190 (its f80 differs only because
that screen is mid-fade -- always compare a SETTLED frame).

## 25. The compat palette needs per-INDEX ground truth, not per-colour

Do not repeat the s21b14 mistake. Calibrating our rendered COLOUR -> reference
colour and patching every table slot holding that value is only valid if the
image is otherwise correct. It was not: the calibration was run on a STRIPED
render, and the resulting patch was wrong (e.g. 017C calibrated to BLACK on the
striped image but to GREY 39CE once unstriped). Applied to the shared table it
recoloured Lonely Island's foliage green -> blue-violet, which Michael
confirmed is wrong, and it was reverted.

There is also a genuine conflict the per-colour method cannot resolve: the
table VALUE 0x0200 must be green for Lonely Island and blue-violet (0x7CAB, at
100% confidence) for the VG Pocket menu. Since 0x0200 occupies TWO indices
(0x1A and 0x1B) and duplicates exist elsewhere too (0x0120, 0x7F1F), the real
hardware almost certainly has DIFFERENT colours at those indices and our table
has collapsed them.

The way to settle it: build a debug core whose vt_compat_rgb555[i] = a distinct
sentinel encoding i, render each game, and read the index straight out of the
framebuffer per pixel. That gives index -> reference-colour pairs with no
ambiguity, for every game with a capture, and will show whether the two games'
index sets are disjoint. Only then patch the table, and re-score vg.png,
11.png and 2.png together.

## 26. Per-console compat palettes, calibrated per INDEX (s21b16)

The COLCOMP=0 compat palette is NOT one table for all VT carts. Proof: index
$1A must be blue-violet on the VG Pocket and GREEN on Lonely Island's console;
$12 and $27 conflict too. That is a per-console DAC difference, which is why
s21b14's attempt to satisfy both by editing one shared table wrecked Lonely
Island's foliage. There are now two tables, selected by $2010 D6 -- the same
signal that picks the 16-bit CHR bus (section 24).

HOW TO CALIBRATE A NEW CONSOLE'S TABLE (repeatable, and far better than the
colour->colour method that preceded it):
1. Build a SENTINEL core: replace vt_compat_rgb555[i] with a value that encodes
   i uniquely, e.g. r = i & 31, g = i >> 5, b = 0.
2. Render the screen headless. Every pixel's colour now decodes straight back
   to the palette INDEX that produced it -- no ambiguity, unlike inverting a
   real colour (the real tables contain duplicate values).
3. Join against the capture, transformed into our geometry (crop x+8, NES row
   (y*3)/2), voting only where the 3x3 neighbourhood is uniform on BOTH sides.
4. Print, per index, current vs derived plus which OTHER games use that index.
   That tells you immediately whether a change is safe or a conflict.
5. Apply only to the new console's table; re-score every reference.

Result for the VG Pocket title: 8 entries corrected at 87-100% confidence,
exact-pixel match against vg.png 38.8% -> 65.6%. Star Ally byte-identical.
Lonely Island's colour SET is unchanged (its 234 differing pixels are object
motion from a small timing shift, not colour) and it never sets D6 -- verified
by step-sampling, 0 hits in 300000 samples.

## 27. Outer CHR bank is 3 bits in 4bpp, 4 bits in 2bpp (s21b18)

From the wiki's final-address diagrams: 2bpp puts the outer bank at address
bits 21-24 ($4100.0-3, four bits), 4bpp at bits 22-24 ($4100.0-2, THREE bits),
because a 4bpp tile consumes one more low address bit. We masked with 0x0F
everywhere, so any 4bpp fetch with $4100 bit 3 set aimed eight outer slots too
high. vt_compute_chr_bank_n(inner, fourbpp) now takes the width, and the 4bpp
callers (vt_chr4_assemble, the extension-active composer, the pix16 composer)
pass 1 while the 2bpp sync-copy path passes 0.

Honest note on impact: this changed NOTHING visible on any cart we have. For a
4 MB image rommask is 0x3FFFFF, and the outer field contributes multiples of
4 MB, so every value masks to the same offset. Star Ally and Lonely Island
never set $4100 bit 3 at all. It is a correctness fix that will matter on carts
larger than 4 MB; it did not fix the VG Pocket game list, and I checked rather
than assuming it had.

## 28. Comparing our output to a capture: GET THE GEOMETRY FROM THE EMULATOR

This section exists because a wrong geometry assumption invalidated a whole
session of measurements. Read it before scoring anything against a capture.

Our 240x160 frame is NOT the top-left of the NES 256x240 frame decimated from
row 0. The per-line scroll table (_dma0buff, one entry per GBA line, VOFS in
the high half-word) gives the real mapping, and for the VG Pocket it is:

    GBA row 0 -> NES row 16 ... GBA row 159 -> NES row 228
    advance +1 on 106 lines, +2 on 53 lines;  HOFS -> x + 8

i.e. the first sixteen NES rows are not shown at all. Assuming NES row
(y*3)/2 from row 0 puts every comparison sixteen rows out, which no small
dx/dy search will recover. ALWAYS read the mapping:

    nes_row[y] = y + (dma0buff[y] >> 16 as signed)

Measured effect of fixing this on the VG category menu, same build, same
capture: structural match 63.5% -> 85.8%, exact-pixel 27.7% -> 50.2%. Nothing
about the emulator changed; only the yardstick.

Two more traps in the same family:
* The reference screenshots are antialiased window grabs (585x481 for a
  256x240 screen). De-scale by CENTRE-SAMPLING each destination pixel and then
  snapping to the top-N colours -- and use N=32, not 16: these menus use four
  palette banks and genuinely exceed 16 colours. Snapping to 16 destroys the
  image and produced zero ROM tile matches.
* Read register shadows by SYMBOL, never by guessing addresses adjacent to a
  known one. vt_reg_2010 is in one BSS group and vt_chr_reg_2018/201A/4100 in
  another entirely. Reading the bytes after vt_reg_2010 returned $2018=$19,
  $201A=$29, $4100=$0B -- all garbage from unrelated variables -- and sent me
  hunting a bank-composition bug that did not exist. The true values are all
  $00 on both VG screens.

## 29. Confirming a bank formula against ROM contents (method)

When a screen looks wrong and you suspect banking, do not guess formulas --
find where the correct tiles actually live:

1. Dump the live nametable and page_bank from the emulator.
2. Recover the capture at native resolution (section 28).
3. For each candidate bank B, decode the ROM tiles at B*2048 + (name&63)*32
   and compare them to the capture's 8x8 blocks using a PALETTE-INDEPENDENT
   signature -- whether each pixel equals its right and lower neighbour
   (112 booleans per tile). Sum over ~48 structurally rich cells.
4. The true bank stands out sharply: for the VG category menu, bank 204 scored
   84.5% while every other candidate, including our then-current effective
   bank, sat at the 52-59% noise floor.

Applied here it PROVED the bank composition was already correct (true bank =
inner bank = page_bank value, with $201A/$2018/$4100 all zero), and it
independently re-confirmed the 16-bit bus layout: the same search under the
8-bit layout peaked at only 61-68%.

## 30. VG Pocket status after s21b22

Title screen: structural 100.0%, exact-pixel 90.7% against vg.png.
Category menu: structural 85.8%, exact-pixel 63.8%.
Star Ally and Lonely Island byte-identical throughout.

Remaining on these screens is palette, not structure. The sentinel calibration
(section 26) now runs against the true geometry, and most indices verify as
already-correct at 100% confidence.

ONE UNRESOLVED SIGNAL WORTH CHASING: index $08 calibrates to brown (17,11,0)
on the title screen and to white (31,31,31) on the category menu, each at 100%
confidence over hundreds of pixels. A fixed DAC cannot produce two colours for
one index, so our palette-INDEX attribution must be wrong on one of those
screens -- most likely the scatter that forms the index (plane bits, the
BKEXTEN-dependent attribute contribution, or the bg/spr bit). That is a real
emulation bug, not a calibration choice, and it is the highest-value thread
left on this cart. Index $19 differs only slightly between captures
((3,18,0) vs (6,18,0)) and is consistent with capture gamma, not a bug.

## 29. Scoring a screen against a capture (tools/, use these -- do not re-derive)

tools/capture_screen.c + tools/score_vs_reference.py are the corrected-geometry
harness. Build: gcc -O2 tools/capture_screen.c -o cap -lmgba. Usage:
`cap <rom> <sa150|sa700|vgtitle|vgcat> <_dma0buff addr> <out-prefix>` writes
<prefix>_fb.raw and <prefix>_geom.txt, then
`python3 tools/score_vs_reference.py <fb> <geom> <reference.png> <label>`.

THE GEOMETRY IS NOT (y*3)/2 FROM ROW 0. Our 240x160 frame starts partway down
the NES field and drops lines unevenly. Read it from the emulator, never assume:
    nes_row[y] = y + (int16)(dma0buff[y] >> 16)        HOFS -> nes_x = x + 8
Getting this wrong cost a whole line of investigation -- it made correct screens
look structurally broken, and NO small dx/dy search recovers it (the offset is
~16 rows). Fixing only the yardstick moved the VG category menu from 63.5% to
85.8% structural with zero code change.

De-scaling a window-grab capture: resize to 256x240 nearest, then snap every
pixel to the top-32 colours. Snapping to 16 destroys these menus -- they use
four palette banks and legitimately exceed 16 colours on screen.

Baseline scores to regress against (s21b22 core):
    VG title     exact 90.7%  structural 99.6%
    VG category  exact 61.9%  structural 86.5%
    SA menu      exact 68.2%  structural 99.1%
    SA gameplay  exact 89.8%  structural 90.6%   <- NOISY, see below
SA gameplay's exact score is not a reliable discriminator: any change in
per-frame host work shifts SA's RNG and object positions. Judge model changes on
the three STATIC screens.

## 30. The BG palette index: three models tested, the plane-scatter wins

vt_build_16color_palette maps (attr, 4bpp value) -> a VT palette-RAM address.
What the hardware does is not obvious and the datasheet text alone is
misleading, so here is the evidence.

WHAT THE GAMES ACTUALLY WRITE (dump vt_palette_ram; this is the key datum):
both VG Pocket screens fill the whole 128-byte palette with $0E and then write
only FOUR entries per 32-colour palette -- at $3F00-03, $3F20-23, $3F40-43,
$3F60-63. That matches the datasheet's four 32-colour palettes at
$3F00/$3F20/$3F40/$3F60 (p.22), with four colours used in each.

MODEL A (shipped, plane-scatter):
    idx = p0 | p1<<1 | (BKEXTEN ? 0 : attr)<<2 | p2<<5 | p3<<6
With BKEXTEN=1 this reads exactly the 16 written entries -- the upper two plane
bits select the 32-colour palette. That is why the VG title scores 90.7%.

MODEL B (datasheet p.22 read literally -- "palette selected by BG7-6, colours
within by SB5 and BG4-1"):  idx = attr<<5 | bgspr<<4 | value.
    VG title 62.3% (-28.4), VG category 37.2% (-24.7), SA menu 67.7%.
    REJECTED. SA gameplay rose to 91.9% but that screen's score is noise (above).
    A sprites-only variant of B left VG untouched and made SA gameplay WORSE
    (88.1%), proving the SA movement came from the BG term, not the sprite term.

MODEL J (unify: low two planes index within a group, a 2-bit BANK selector picks
the palette -- attr normally, upper two planes when BKEXTEN steals attr):
    idx = p0 | p1<<1 | (BKEXTEN ? (p2|p3<<1) : attr)<<5
    Identical to A when BKEXTEN=1; VG category fell to 35.5%. REJECTED.

So model A stands. The remaining VG category error (61.9% exact / 86.5%
structural) is NOT explained by any of these, and the two rejected models both
made it worse -- do not re-try them.

Note on the "$08 conflict": colour number $08 calibrating to brown on the title
and white on the category menu does NOT by itself prove an index-attribution
bug. Both screens legitimately contain $08 (title $3F20, category $3F42), and a
calibration taken from a screen that is only ~62% correct is unreliable. Treat
it as a symptom of the category error, not as independent evidence.

## 31. The VALUE-sentinel: calibrating the DAC without guessing indices (s21b24)

Section 26's index sentinel answers "which palette-RAM entry did this pixel
use". The VALUE sentinel answers the more useful question directly: set
gba_bg[group*16 + v] = v (encode the 4bpp value as the colour), render, and
every pixel's colour IS its 4bpp value. Join that with the capture and you get
value -> true colour at ~100% confidence. The game's own palette RAM then gives
value -> colour index, so you can read off index -> true colour with no
inversion and no ambiguity, and simultaneously CHECK the index model: if every
value lands on an entry the game actually wrote, the model is right.

That check is what finally validated model A (section 30) on the VG category
screen: values 0,1,4,6,9,10,13 mapped to $3F00,$3F01,$3F20,$3F22,$3F41,$3F42,
$3F61 -- all written entries, all with sensible colours.

It also killed the last two "conflicts" cheaply:
* Every visible cell on that screen has ATTRIBUTE 0, so the unwritten $3F04-0B
  entries our model reaches for attr != 0 are never read. Dump the attribute
  table before theorising about attribute handling.
* ci $19 reads (3,18,0) on vg.png and (6,18,0) on the category grab. That is
  capture gamma, not a hardware conflict: vg.png is a native 256x240 capture,
  the menu shots are rescaled window grabs.

PREFER THE NATIVE CAPTURE when two disagree. Applying the title-derived values
($08 -> 0x0171, $19 -> 0x0243) moved the VG title from 90.7% to 97.1% exact.
The category screen's exact score drops (61.9 -> 45.5) purely because its own
capture's gamma no longer matches: at +/-4 per-channel tolerance the two builds
are within 2.4 points on the category (82.9 vs 80.5) while the title is 98.8 vs
94.5, and the category's STRUCTURAL score is unchanged at 86.5% -- nothing
renders differently. A native-resolution capture of the category menu would
settle the remaining few counts.

## 32. The blank VG Pocket games: vt_prg_banks was 8 bits wide (FIXED s21b26)

Both games that booted to a black screen were crashes, and both had ONE cause:
the PRG bank number is `inner | middle | (outer << 8)`, so any cart using a
non-zero OUTER bank produces a bank >= 256 -- and `vt_prg_banks[4]` was a **u8
array**, written through explicit `(u8)` casts and read back with `ldrb` in
vt_apply_prg_banks. The outer bank was silently discarded: entry 0's bank 0x11C
became 0x1C, mapping the game to the wrong 2 MB half of the image, and the CPU
ran off into fill bytes.

The two failures looked different only because the two wrong regions had
different fill: one is 0x00 (the CPU executes BRK forever, PC pinned at guest
$0000, stack filling with BRK pushes), the other is 0xFF (the CPU sprays $FF
into every register -- nametable and CHR all zero, guest palette all $3F,
$2000/$2001 = $3F, and $4100 itself ends up $FF, which is a SYMPTOM and not the
cause; do not chase it).

HOW IT WAS FOUND -- the clean discriminator: capture the bank registers at the
frame the launcher writes them, not later. Entries 0 and 1 turned out to have
IDENTICAL registers except $4100 (0x10 vs 0x00). Entry 1, outer bank 0, worked.
Entry 0, outer bank 1, crashed. Same for entry 4. "Outer bank 0 works, outer
bank 1 crashes" points straight at the outer term being lost.

The fix is the width: u16 in vt_regs.c and vt_regs.h, (u16) casts, and ldrh at
offsets 0/2/4/6 in mapVT.s. Nothing else needed -- map89_/mapAB_/mapCD_/mapEF_
already accept 9-bit banks (they mask with rommask>>13, i.e. 511 for a 4 MB
image).

RESULT: all five reachable VG Pocket games now render (entry 0: 0 -> 36186 lit
pixels, entry 4: 0 -> 37288). Star Ally menu and gameplay and Lonely Island are
BYTE-IDENTICAL, and the VG title and category scores are unchanged -- as
expected, since SA and LI never set a non-zero PRG outer bank.

This is worth remembering for any future multicart: a game that boots black
while its neighbours work is a banking-width question first, and the register
capture at the launch frame is the cheapest way to see it.

TOOLING TRAP FOUND HERE: _m6502_pc is an ARM REGISTER (r9) spilled to memory
only at certain boundaries. Reading it between core->step() calls returns
garbage -- a ring buffer of "PCs" full of values like 9a4ac4de and c83f6000 is
the tell. Frame-granular reads (after runFrame) are reliable; instruction-level
6502 tracing needs a different mechanism.

## 33. The VG Pocket has FIVE category menus, not one list (s21b28)

Navigation is three levels, not two:
  title --A--> CATEGORY menu (green bushes, 5 wooden signs: Action, Racing,
  Shooting, Sports, Wits) --Down x C, A--> GAME LIST for that category (black
  background, paw icons, 5 titles, first row highlighted purple)
  --Down x G, A--> launch.
An earlier note called the category menu "the game list"; it is not. Use
tools/capture_screen.c screens `vgcat` and `vglist`.

PALETTES (calibrated with the section 31 value sentinel against Michael's two
menu captures):
  VG title     97.1% exact / 99.6% structural   (95.0% with the $19 choice below)
  VG category  61.9% exact / 86.5% structural
  VG game list 79.2% exact / 92.1% structural   <- first ever measurement
The list screen is BLACK in hardware. Entries $0E/$1D/$23 must be black and
$2D is the purple highlight; $23 was coloured here, which is exactly why that
screen rendered green.

THE $19 TRADE-OFF, measured across all three screens (it is used by both the
title and the category background, and the two captures disagree by 3/31 in
red):
  $19 = 0x0243 (title/vg.png):  title 97.1, category 45.5, list 79.2
  $19 = 0x0246 (category grab): title 95.0, category 61.9, list 79.2
Shipped 0x0246 -- the category screen is a large flat area where the error is
obvious, and it gains 16.4 points for 2.1 lost on the title.

SPRITE CAVEAT for the value sentinel: it instruments only gba_bg, so any pixel
covered by a SPRITE in the reference mis-attributes. That is the whole story
behind the "$08 conflict" -- the category menu's sign text is sprite-drawn, so
45 pixels claimed $08 was white while the title's 766 pixels said brown. Weigh
by vote count and prefer the native capture.

## 34. OPEN: the solid-colour VG Pocket game (category 0, entry 2)

Mechanism located precisely; the fix is NOT yet decidable. Symptoms next to its
working neighbour (category 0, entry 3):

  broken  $2010=$06  832/960 nametable names   CHR words in VRAM:   1/1024
  working $2010=$16  819/960 names             CHR words in VRAM: 773/1024

So the game loads a full screen of names and we assemble no tiles for it -- the
screen fills with one colour. Its CHR page banks are 160-163 with $4100=$10,
$201A=$00, $2018=$00. In 4bpp a bank is 2 KiB, so bank 160 is ROM 0x50000 --
and that region is 63 of 64 tiles BLANK, which is exactly the 1/1024 we see.

Our CHR banking matches the wiki article: outer = $4100 bits 0-3 (VA21-24) with
`OuterBank << 11`, intermediate = $2018 bits 4-6, the $201A mask table, and
"1 KiB (2 KiB in 4bpp modes)" units. $4100=$10 therefore gives CHR outer 0 even
though the PRG outer (bits 4-7) is 1 -- this game's CODE is in the upper 2 MB
while we fetch its GRAPHICS from the lower 2 MB.

Candidate offsets that DO contain data: bank160 x 2 KiB + 2 MB, and
bank160 x 1 KiB (with or without +2 MB). DO NOT guess between them. An offline
tile-coherence test was tried to pick a winner and FAILED ITS CONTROL -- a
known-good game's CHR scored 1.01/8 on the same metric, i.e. the metric does not
separate art from noise for these carts. (Same failure mode as the offline BG
renderer in section 28: always run the control first.)

What would settle it: a capture of that game running on hardware. That is
exactly how vg.png settled the title screen. Until then, changing CHR outer
handling risks the games that currently work, which is the b10/b14 mistake.

Worth noting for future work: the VG Pocket GAMES run with $2010 D6 CLEAR
($06, $16, $46, $56 seen), unlike its MENUS ($4E, $5E). So the games use the
8-bit CHR bus layout and the SA/LI palette table, while the menus use the 16-bit
layout and the VG table. Decode with the right layout when analysing them -- a
coherence test run with the wrong one is meaningless.

## 35. The first game ("Right Spot"): structure CONFIRMED, palette DISPUTED

Reference 1.png is the first game, reached by three A taps (title -> category ->
game list -> launch; category 0, entry 0). Michael notes it is a VARIANT capture
-- it carries "Right Spot" and "PRESS START" logos that the VG Pocket's copy
does not -- so treat it as authoritative for SHAPE and provisional for COLOUR.

GOOD NEWS, and the point worth keeping: our render scores **94.8% structural**
against it. So for a game in PRG outer bank 1 with $2010 = $56 (D6 set), the CHR
banking, geometry and tile assembly are all essentially correct. Whatever ails
the other games, it is not a general failure of the game path.

THE PALETTE CONFLICT (unresolved -- do not "fix" it by picking a side):
The value sentinel calibrated all 16 of that screen's values at 88-100%
confidence. Applying them:
    Right Spot game   5.4% -> 89.8% exact   (+84.4)
    VG title         95.0% -> 81.6%         (-13.4)
    VG category      61.9% -> 42.8%         (-19.1)
    VG game list     79.2% -> 78.7%         (-0.5)
Fifteen of sixteen entries disagree with the menu-derived values, and not by a
little: $20 is white on the title and dark green in the game, $1A is
blue-violet on the title and white in the game. That is not capture gamma.

Reverted -- the menus were calibrated from captures of Michael's actual console,
1.png is a different release. But note what this implies: on real silicon ONE
DAC serves both, so a single table must satisfy both screens. It does not, which
means our value -> palette-index attribution differs from hardware on one of
them. The screens differ in $2010: game $56 (SPEXTEN=0, BKEXTEN=1), title $5E
(SPEXTEN=1, BKEXTEN=1), menus $4E (SPEXTEN=1, BKEXTEN=0). Both game and title
have BKEXTEN=1 and identical four-palettes-of-four palette RAM layouts, so model
A should apply identically -- yet they disagree. Worth checking whether the chip
has an old (25-colour) versus new (121-colour) palette mapping select, which
would translate indices differently between modes.

CORRECTION to an earlier note: it is NOT true that VG Pocket games all run with
$2010 D6 clear. $06 and $16 have it clear; $46 and $56 have it SET. Check the
bit per screen -- it selects both the CHR bus layout and which compat palette
table is used.

## 36. Four native captures reconcile the DAC (s21b30)

Michael supplied native captures of all three VG Pocket menu levels plus the
first game: vg.png (title), 1.png (category), 2.png (game list), 3.png ("Get it
Right"). 3.png is his own console's copy -- it LACKS the "Right Spot"/"PRESS
START" logos that the earlier variant capture carried, which is how you tell
them apart.

With four native references the DAC becomes consistent: calibrating each screen
independently, **20 colour indices AGREE across screens and only 3 conflict**
($14 and $19 differ by ~3/31, i.e. capture noise; $29 is a real disagreement
between the category and game screens). That is the single-DAC coherence the
variant capture could not give -- and it retires guide section 35's worry that
our index attribution differed per screen. It does not; the earlier conflict was
an artifact of comparing against a different release.

RESULT (scores below): Get it Right 0.6% -> 43.6% exact with every other screen
UNCHANGED, and Star Ally + Lonely Island byte-identical. Strict improvement.

TWO MEASUREMENT TRAPS THIS TURN, both worth keeping:
1. THE MENU CAPTURES ARE CROPPED, NOT FULL FRAMES -- 256x210 and 256x209, not
   256x240. The reference row for our GBA row y is (y + vofs - R0), and R0 must
   be found, not assumed: 0 for the title, ~7-10 for the menus, ~20 for the
   game. Assuming R0=0 silently mis-scores everything.
2. ALIGN ON A PALETTE-INDEPENDENT SIGNATURE, NOT ON COLOUR. Choosing R0 by
   maximising exact-colour agreement picks spurious offsets (it chose R0=26 for
   the category screen against R0=7 from the signature). tools/score_vs_reference.py
   now searches R0 and a vertical scale by the neighbour-equality signature,
   then reports exact match at that alignment.

## 37. OPEN: the category menu is STRUCTURALLY wrong, not just mis-coloured

Measured against the native 1.png with correct alignment: **structural 59.4%,
exact 27.5%** -- while the title is 100.0/95.0, the game list 93.0/80.7 and the
game 80.3/43.6. So screens 1, 3 and 4 are structurally sound and screen 2 is
not. Its earlier 85.8% structural was measured against a 585x481 window grab
resized to 256x240, which STRETCHED 210 rows to 240 and flattered the result.

This is the screen that also wanted a vertical scale of 1.067 in the alignment
search, so re-check the capture's own geometry before assuming a rendering bug.
Then use the section 29 ROM-search (palette-independent tile signature) to find
which bank its cells should come from, exactly as that search proved bank 204
correct in s21b22.

## 38. Category menu: the BUSHES are right, the SIGNS are not (s21b31)

Narrowed considerably. The screen is completely STATIC (0 pixels change over 240
frames), so no phase effect, and it uses ZERO sprites -- the wooden signs are
background, not objects. Rendering it beside the capture shows the bush pattern
matching along both edges while the five signs come out as flat bars where the
reference has textured planks with text.

The nametable explains the split: bush cells carry names $08-$33, i.e. CHR PAGE
0, while every sign row carries $51-$D4 -- pages 1, 2 and 3. Page 0's bank was
proven correct back in s21b22; pages 1-3 have never been verified.

ROM SEARCH RESULT (section 29 method, palette-independent tile signature, 100
cells per page):
    page 0  best bank  204 = 0.655   2nd 0.564   floor 0.504   <- we use 204, CORRECT
    page 1  best bank  256 = 0.584   2nd 0.577   floor 0.482   <- we use 205
    page 2  best bank  256 = 0.577   2nd 0.576   floor 0.477   <- we use 206
    page 3  best bank  955 = 0.594   2nd 0.583   floor 0.491   <- we use 207
Page 0 separates cleanly; pages 1-3 have NO winner -- the top candidate is
within noise of the second. Since the same search does find page 0, the sign
tiles are evidently NOT plain 4bpp tiles sitting in a 2 KiB bank anywhere in the
image. Something about how those pages are fetched differs; that is the thread
to pull, not the bank number.

ALIGNMENT, and a correction to section 36: for 1.png the true crop offset is
**R0 = 2**, not the 7 that the neighbour-signature search reported. Pin it by
ANCHORING ON KNOWN-GOOD DATA instead -- score page-0 cells against the proven
bank 204 across candidate R0 and take the peak (R0=2 gives 0.666 under the
16-bit layout, 0.637 under 8-bit, which also re-confirms the 16-bit bus for this
screen). With R0=7 the search preferred the 8-bit layout and produced misleading
bank rankings, so a wrong alignment does not merely lower scores, it flips
conclusions.

## 39. Why the VG palettes CANNOT be finished by calibration (s21b33)

Michael's verdict after b30 was blunt and correct: none of the palettes are
fixed. This section records why, so nobody burns another session calibrating.

Two colour indices give IRRECONCILABLE readings from two native captures of the
SAME console:
    $19  title (3,18,0)      vs  category bushes (6,18,0)
    $29  Get it Right (31,26,19 cream)  vs  category bushes (16,28,0 green)
$29 is not capture noise -- green versus cream, 795 and 989 votes, both at 100%
within-screen confidence.

Everything that could explain it away has been RULED OUT by measurement:
* Not structure. The category votes were restricted to CHR page 0 (the bushes),
  whose bank was proven correct in s21b22; only the sign cells are wrong.
* Not sprites. In a sentinel build a background pixel satisfies r==g and b==0,
  so sprite-covered pixels can be excluded exactly. Zero were skipped on any of
  the four screens -- these screens use no sprites at all.
* Not animation. The category screen changes 0 pixels over 240 frames, and the
  game's palette RAM is byte-identical from +80 to +560 frames.
* Not alignment. R0 is pinned by anchoring on known-good data (section 38).
* Not the index model. Category value 1 -> palette RAM index 1 ($29) and game
  value 3 -> index 3 ($29) under our model, under the datasheet-literal model B
  and under model J alike; all three agree here, so no choice among them helps.

A single DAC cannot render colour number $29 as both green and cream. Therefore
our value -> palette-RAM-index mapping is wrong on one of these screens in a way
none of the three tested models captures, and NO table can satisfy both. Chasing
better numbers by adjusting entries just moves the error between screens:

     $19          $29           title  categ  glist  game   TOTAL
     (3,18,0)     (31,26,19)    97.1    7.4   80.6   47.1   232.2
     (3,18,0)     (16,28,0)     97.1   15.8   80.7   43.7   237.4
     (6,18,0)     (31,26,19)    95.0   14.7   80.6   47.1   237.4
     (6,18,0)     (16,28,0)     95.0   23.1   80.7   43.7   242.5   <- shipped

Shipped the best total. The real work is finding what differs between a
BKEXTEN=0 screen (category, $2010=$4E) and a BKEXTEN=1 screen (game, $56) in
how a pixel value reaches palette RAM. Note the category ALSO has the flat-sign
fault (section 38) whose tiles match no bank in the image -- one mechanism may
well explain both, so treat them as one investigation, not two.

## 40. The VG Pocket catalogue: 54 menu entries, not 50 (s21b34)

Counted two independent ways that agree. Method that works and is fast: from a
game list, hash the live nametable (NES_VRAM2, 960 bytes) after each Down tap
and find where the hash cycle repeats -- no launching required. Cross-checked
by launching each entry and comparing the PRG bank signature (vt.reg[7..0x0B]),
which repeats on exactly the same period.

    CATEGORY menu: 5 categories (entry 5 repeats entry 0)
      category 0 (Action)    5 entries
      category 1 (Racing)    4
      category 2 (Shooting)  6
      category 3 (Sports)    4
      category 4 (Wits)     35   <- this list SCROLLS; the others fit on one page
      TOTAL                 54

So the unit advertises 50 and its menu actually offers 54 entries. Entry 35 of
category 4 repeats entry 1, and no PRG bank signature recurs across categories,
so these look like 54 distinct titles rather than duplicates.

STATUS OF EVERY ENTRY (px = lit pixels, cols = distinct colours, after launch
and ~420 frames):
  RENDERS SOMETHING PLAUSIBLE  ~30 entries
  BLANK (0 px)                 ~13: C2 G2; C3 G1, G3; C4 G4, G7, G9, G10, G16,
                                    G19, G22, G29 (+C4 G5 at 254 px)
  FLAT (one colour, 38400 px)    4: C0 G1; C2 G5; C3 G0, G2
  VERY SPARSE (<6000 px)         5: C2 G4; C4 G12, G21, G27, G30
  NOT MEASURED                   5: C4 G0, G8, G17, G26, G34 -- the harness
                                    dropped the launch tap, NOT a game fault
Category 3 (Sports) is the worst: all four of its entries are blank or flat.

CAVEAT ON THE HARNESS: a "no-launch" row means the menu tap was missed, which
happens every dozen or so entries with tap(8 frames on, 22 off). Confirm with
the bank registers before recording a game as broken, and re-run those indices.

## 41. Hunting unused games in the VG Pocket image (s21b35 — none confirmed)

Worth doing, and the tooling is reusable, but the honest result is negative.

STEP 1 -- which banks does the menu actually use? Compute each entry's $E000
bank from its launch registers: eff = (0xFF & A) | ((pq3 | outer<<8) & ~A) with
A = 0x3F>>ps, ps = $410B bits 0-2, pq3 = $410A, outer = $4100 bits 4-7. The 54
entries resolve to only 39 DISTINCT banks (several share one; nine entries map
to bank 31), so entries are not one-to-one with fixed banks.

STEP 2 -- which banks look like a game? Scan all 512: NMI/RESET/IRQ all within
$8000-$FFFF, all three distinct, RESET >= $E000 (it must live in its own fixed
bank), and the byte at RESET a plausible opening opcode (SEI/CLD/LDX/JMP/...).
43 banks qualify. Five are not claimed by any menu entry -- 143, 255, 319, 359,
391 -- and each has unique 8 KiB content (its md5 matches no other bank), so
they are not duplicate copies.

STEP 3 -- BOOT THEM. Build a probe ROM that overwrites the image's BOOT bank
with a launcher stub. The boot bank is **63 (ROM 0x7E000)**, NOT the last bank
of the image -- putting the stub at the end does nothing and you just get the
normal title screen, which is exactly how this first went wrong. The stub must
copy its register writes to RAM and run them from there, because setting the
banks pulls the fixed bank out from under itself:

    $E000: SEI, CLD, LDX #$FF, TXS, LDX #len-1,
           LDA $E100,X / STA $0200,X / DEX / BPL, JMP $0200
    $E100: LDA #ps  STA $410B ; LDA #pq3 STA $410A ; LDA #outer<<4 STA $4100
           LDA #0   STA $4107 ; LDA #1   STA $4108 ; JMP ($FFFC)
    vectors at $FFFA all point to $E000

Choose ps as the smallest value with (target & A) == A, then pq3 = target & ~A
and outer = target >> 8. ALWAYS include a control: bank 207 is a reachable
game and boots to ~36.8K lit pixels / 16 colours, which proves the stub works.

RESULT: 143, 255 and 319 boot BLANK; 391 fills the screen with one colour; 359
produces a real screen (36947 px, 26 colours, $2010 = $5E) -- but that closely
matches menu entry C4 G35 (37294 px, 26 colours, $5E), so bank 359 is very
likely already reachable and step 1 simply missed it. Step 1's bank numbers come
from registers read AFTER launch, which the game itself may have rewritten, so
treat that "used" set as approximate.

NO hidden game is confirmed. Two caveats before anyone concludes there are none:
the stub sets only ps/$410A/$4100/$4107/$4108, while the real launcher may also
set $4109, mirroring or $2010 -- so a blank probe does not prove "not a game";
and the launcher's per-game table was NOT found (searching all 22 known
$4107..$410B signatures as contiguous bytes gives zero hits, so it is
column-major or encoded). Finding that table is the clean way to settle this.

## 42. The reference captures are SCALED, not cropped — and that changes conclusions (s21b36)

Michael's menu/game captures are 256x210, 256x209 and 256x210. Sections 36 and
38 treated the missing 30 rows as a CROP and searched for an offset R0. That was
wrong: they are a vertical SCALE of the full 240-row frame. The correct mapping
is

    reference_row = int( (y + vofs) * H / 240 )        vofs from _dma0buff
    reference_col = x + 8

PROOF, and it is decisive. Take the three CHR pages the category menu uses and
score each against the bank our emulator assigns it, sweeping scale and offset:
at scale 0.875 (= 210/240) and offset 0, ALL THREE pages agree with their
assigned banks -- page 0 vs bank 204 = 0.704, page 1 vs 205 = 0.682, page 2 vs
206 = 0.648, against a ~0.50 noise floor. Under the crop model no single offset
did that; page 0 wanted offset 2 and page 1 wanted 10, an 8-pixel disagreement
that looked like a one-cell displacement of the signs.

WHAT THIS RETRACTS:
* Section 38's headline -- "the sign tiles are NOT plain 4bpp tiles in any 2 KiB
  bank" -- is WRONG. Banks 204/205/206 are correct for pages 0/1/2. There is no
  missing fetch mechanism to find.
* Every category-menu score taken with the crop model is void. With the scale
  model the same build scores 46.3% exact / 71.8% structural, not 23.1 / 55.8.
* The palette calibration for that screen sampled reference pixels through the
  wrong mapping, so the $19 and $29 "irreconcilable" readings in section 39 are
  suspect and must be re-derived before anyone concludes the index model is
  broken.

The lesson is the one from section 38 restated more strongly: a wrong geometry
does not merely lower scores, it manufactures phantom bugs. Fit scale AND offset
by the palette-independent signature, and sanity-check by scoring known-good
banks -- if a bank you have already proven correct does not come out on top, the
geometry is wrong, not the emulator.

TOOL NOTE: tools/capture_screen.c had lost its `vglist` and `vggame` routes (they
were only ever patched into a /tmp copy), so those names silently fell through to
the vgcat branch and produced identical captures. Both routes and the palette-RAM
dump are now in the tree copy. If two screens score identically, check you are
actually capturing two screens.

## 43. Palettes recalibrated on the corrected geometry (s21b37)

Redoing section 31's value-sentinel calibration with the SCALE mapping from
section 42 instead of the crop mapping. Two rules made the difference:

1. **A screen may only vote on colour once its STRUCTURE is right.** Fit each
   capture's scale and offset first, then rank the screens by that structural
   fit and let the best-fitting screen own each index:
       title 1.000 > game list 0.961 > Get it Right 0.885 > category 0.707
   The title supplies 12 indices, the list 3, the game 9, the category 2.
2. **Settle genuine disputes by measuring, not by vote count.** $19 and $29 are
   claimed by both the title/game and the category. Scoring all four
   resolutions across all four screens offline (synthesise the frame from the
   sentinel VALUE render plus the game's own palette RAM -- no rebuild needed):
       priority, game $29        268.1 total
       category $29              278.2
       category $29 + $19        290.3   <- shipped
       category $19 only         280.2

RESULT, verified on a real build against the four native captures:
       title       95.0% exact / 100.0% structural
       category    46.3 / 71.8
       game list   86.5 / 95.8      (was 80.7 under the crop mapping)
       Get it Right 62.0 / 86.8     (was 43.6)
Star Ally's menu is 68.6% vs 11.png (baseline 68.2, i.e. unchanged within
noise) and Lonely Island is identical on every measure -- 32278 lit pixels,
19 colours, dominant 0x8400 -- as expected, since neither ever sets $2010 D6
and so neither reads the VG table.

The category menu remains the weakest at 71.8% structural. That is now known
NOT to be a banking fault (section 42 proved banks 204/205/206 correct), so the
residue is either its own capture's geometry -- it is the only screen whose
offset search does not reach a sharp optimum -- or something specific to
BKEXTEN=0 rendering. Chase the geometry first; a screen that will not align
cleanly is usually telling you about the capture, not the emulator.

## 44. The captures are INTERPOLATED — quantise before comparing (s21b38)

vg.png is a clean framebuffer dump: 256x240, exactly **16 distinct colours**.
The menu and game captures are not. Scaling them introduced blending:

    1.png (category)    4873 distinct colours, top-16 cover only 49% of pixels
    3.png (Get it Right) 4826 distinct colours, top-16 cover 76%
    2.png (game list)   1055 distinct colours, top-16 cover 92%

Half the category capture is blend pixels, which no emulator can ever match.
That was depressing both metrics uniformly and, worse, diluting the calibration
votes. ALWAYS snap a reference to its dominant colours before scoring or
calibrating: take the top 16 by area and map every pixel to the nearest.

Effect of quantising alone, same build, no code change:
    category    46.3 exact / 71.8 structural  ->  52.4 / 80.1
    Get it Right 62.0 / 86.8                  ->  68.8 / 93.1
    title and game list unchanged (they were barely blended)

Recalibrating on the quantised references then lifted the usable index count
from 26 to 30 and improved the table again. Final, verified on a real build:

    title        95.0% exact / 100.0% structural
    category     52.4 / 80.1
    game list    86.7 / 96.1
    Get it Right 70.7 / 93.3      (was 43.6 exact three builds ago)
    SA menu      68.6 / 99.5      (unchanged -- control)
    Lonely Island identical: 32278 lit pixels, 19 colours (control)

DIAGNOSTIC WORTH REUSING: when a screen scores badly, split the comparison by
CHR page using the live nametable. On the category menu all four pages scored
alike (66-76% structural) INCLUDING page 0, whose bank is proven correct -- a
uniform deficit across a screen means the yardstick, not the renderer. A
page-specific deficit would have meant the opposite. That is what pointed at the
capture rather than at another phantom banking bug.

## 45. The VG palette is at its measurable limit — the residue is the DOWNSCALE (s21b39)

Michael: "the palettes still aren't fixed... the second screen is almost there,
but still not precisely." He is right that it is not precise, and here is why,
with the evidence, so nobody spends another session recalibrating.

FIRST, two hypotheses killed cheaply:
* The VG DAC is NOT a standard NES palette. Substituting the 2C02 table scores
  130.1 total against the fitted table's 305.5, and the captures' own colours
  sit 10-24 units (RGB555) away from their nearest NES colour. Empirical
  fitting is necessary.
* Every remaining disputed index was tested INDIVIDUALLY against all four
  references. Only $0C improved anything (+2.1). $27, $20, $11 and $08 each
  made the total WORSE by 1.4-7.2. The title-derived values are right; the
  category's disagreements are attribution noise, not colour errors.

SECOND, and this is the actual answer: measure exact match against a 3x3
tolerance -- "does our colour appear anywhere in the reference's neighbourhood".

    title        95.0% exact -> 95.0% within-3x3   (+0.0)
    category     54.5       -> 80.5               (+26.0)
    game list    86.7       -> 91.9               (+5.2)
    Get it Right 70.7       -> 75.0               (+4.3)

The title gains NOTHING from the tolerance because it is large flat shapes, and
it is already at 95%. The category gains 26 POINTS, because it is dense foliage
and our 240x160 output drops one scanline in four (section 16). Its colours are
substantially correct; its pixels simply land on different rows than the
console's. No palette entry can fix that -- only reducing the vertical
decimation can, which is the section 16 scaling work.

So: the category menu's palette is close to correct and further calibration will
not move it. If it still looks off on hardware, the thing to fix is the
256x240 -> 240x160 mapping, not the table.
