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

In a git checkout (Claude Code) the short path is: `LIBGBA=<libgba clone>
bash build_pvt.sh`, then `tools/restage.sh` (copies builder.py, testroms/ and
anything in $PVT_ROMS into the build dir). The devkitARM Makefile also builds
unmodified: `sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm
make` -> ./pocketvt.gba (a different GCC, so NOT byte-comparable with a
build_pvt.sh core -- compare like with like). testroms/ and reference/ are
local-only and gitignored. The recipe below is the original sandbox one.

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

The Python mini-reference, tools/vtref.py (from the retired ROADMAP.md): a
py65 CPU + descrambling fetch + PRG banking + timer IRQ + NMI + controllers +
$6000 WRAM, no video. It runs Lonely Island's logic at full fidelity, supports
per-address read/write tracing by monkeypatching Mem, and answers "what does
the game want" questions in seconds -- it found the P2-input and WRAM
discoveries. It reads the LI PRG from $VTREF_PRG (default ./li_prg.bin, i.e.
the .nes minus its 16-byte header). tools/vtview.py adds a PPU write model and
a 16-colour COLCOMP=0 BG renderer on top; it also needs the session scratch
files vtchart.py / vtcompat.py (dir in $VTVIEW_AUX), which are not in the tree.

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

## 10. Reference: Lonely Island internals (for regression work)

Pad bits as the game sees them: A=$80 B=$40 Sel=$20 St=$10 U=$08 D=$04
L=$02 R=$01; current state at zp $00 (P1) / $02 (P2), pressed-edge $34/$36.
Map position at 0x030006F4 as YX nibbles, start $72; the sidescrolling
stage is house node $0B; route Up 220f, Right 60f, Up 60f. The game's idle
loop C5 26 A5 26 F0 FC sits at guest $E094 (VRAM 0x0600E094) and is
speedhacked. LI's iNES header lives at offset 0x184B0 inside any single-LI
.gba. Autoplay route frames: {400,0x10},{620,0},{630,0x80},{690,0},
{700,0x10},{760,0} -- starting before ~frame 400 is too early.

More LI structure (from the retired ROADMAP.md, session 10): the island map
is a node graph in famiclone WRAM at $6500+, indexed by position packed as YX
nibbles (current $06F4, target $06F7/$0602): type $01 = path, $0A-$0F = the
six house/level nodes (Y2XD=$0F, Y3X9=$0A, Y4X3=$0B, Y7X8=$0C, Y9XD=$0D,
YBX6=$0E). The direction handler ($CE15, fixed $C000 bank) validates and
queues; the walk executor ($B824, bank $0B at $A000) animates and commits; its
single caller $D073 also queues footstep sound $12 to the mailbox $05CD.
Walking INTO a house node enters its scene (position resets to 0, banks flip
to [7,6]); Select opens a level menu that scans all six special nodes. The NMI
uploads nametable data with a stack-blast ($E221: SP -> $01FF, then PLA/STA
$2007 pairs; the producer fills $0100 and raises $87 bit 4). $2010 stays $0E
on the map and in the first house scene.

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

## 12. Session 20: the Lonely Island "dashes" and the unaligned-buffer class

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

## 13. ARM/Thumb interworking rule for asm-called C functions (session 20)
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

## 29b. Scoring a screen against a capture (tools/, use these -- do not re-derive)

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

## 30b. The BG palette index: three models tested, the plane-scatter wins

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

That check is what finally validated model A (section 30b) on the VG category
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
* **Submapper byte-mangling** (the retired IMPLEMENTATION_NOTES.md listed
  it as "the remaining work"). From reference/nrs/mapper256.cpp, all three tables are now
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

## 74. Housekeeping: retired docs, and the open items they carried (repo cleanup after s21b62)

No emulator change. The tree was tidied for git: reference/ (NESdev XML
exports, NintendulatorNRS sources) and testroms/ are local-only and
gitignored, as are ROMs, PNG captures and build output. The shipping core is
byte-identical before and after this cleanup (checked with both build_pvt.sh
and the devkitARM Docker build). builder.py now exits non-zero when it
injected no ROMs (the section 2 / CLAUDE.md trap), and tools/restage.sh,
score_5bit.sh, vtref.py and vtview.py take paths from the environment
instead of the old /home/claude and /mnt/user-data layout.

Docs removed, and where their still-true content went:

* ROADMAP.md (session-10 Lonely Island plan; it already deferred to this
  guide) -> LI facts into section 10, vtref.py notes into section 3. Its
  milestones M1-M2.5 are done (sections 6-8b); M3 (audio by ear) and M4 (full
  playthrough) were never formally closed.
* FIX_4BPP_DESIGN.md (the session 3-5 4bpp design) -> its verified results
  are in sections 13 and 66a. The one lesson not recorded elsewhere: the
  first per-bit 4bpp assembler was too slow for the vblank IRQ and crashed
  with LR=0 in its inner loop. Enlarging the IRQ stack did NOT help; the fix
  was the 256-entry `vt_spread` table (plane byte -> GBA nibble order,
  0-mismatch vs the per-bit method). Adding its EWRAM globals then shifted
  the fixed-address layout and hung boot. Hence "time, not stack" and the
  EWRAM_BSS rule in section 5.
* CODE_AUDIT.md: the first half (the "garbled from boot" hunt that led to
  the 4bpp path) is obsolete. Its four register verdicts moved to
  DATASHEET_DIGEST.md appendix B.
* FINDINGS_vt_opcodes.md -> DATASHEET_DIGEST.md appendix A.
* README_pocketnes.md -> README.md (Heritage).
* IMPLEMENTATION_NOTES.md (v0.2-era) -> the durable parts are below. Its
  "submapper byte-mangling" item is DONE (section 70a).

Durable facts from IMPLEMENTATION_NOTES.md:

* `vt_active` (vt_regs.c) gates the PPU $2008+ divert: cart.s loadcart_asm
  clears it before any mapper init and mapVTinit sets it, so non-VT mappers
  keep NES $2000-$2007 mirroring.
* loadcart.c remaps NES 2.0 mapper 256/405 to internal 253 and stores the
  submapper in `vt.submapper`; bit 7 of it marks "mapper 405".
* 6502_vt.s hardcodes VTState offsets 0x20-0x22 (encryption fields); the
  `_Static_assert`s at the top of vt_regs.c fail the build if they drift.

Open items those docs carried, still unimplemented:

* **Mapper 405 zero-vector boot.** VT168 carts such as Space_War have
  all-zero vectors; the real chip's boot ROM reads a jump table at PRG
  offset $10 (Space_War: $8057,$80AE,$8008,$805F,$80B6,$8017,$806E,$80C5,
  stored high byte first) and jumps to one entry. The first ($8057) is the
  safe default. Suggested shape: a 4 KB EWRAM copy of the last PRG 4 KB with
  a `JMP $8057` stub and a patched reset vector, installed as memmap_F.
  Loadcart already sets the vt.submapper bit-7 marker.
* **VT369 ADPCM.** vt_regs.c uses the standard 89-step IMA table. Real VT369
  uses a 16x16 step table with four prediction modes chosen by lead-byte
  bits 5:4 (Furbtendulator ADPCM_VT369.cpp):
  `index = lead - (pos>=24 && lead&0x40) + 2*(pos>=24 && lead&0x80);
  step = stepTable[index&15][nibble&15]`. It plays, but not bit-exact.
* **OAM-extension 16x8 sprites (vt_oam_ext).** ppu_vt.c stores the
  extension table but nothing reads it. Design: with `vt_ppumode & 0x04`,
  each OAM entry's tile is the left half and `vt_oam_ext[i*2]` the right
  half, so each VT sprite emits two GBA OBJs. This is separate from the
  PIX16EN path (sections 8/8b), which is done. No test cart needs it yet.
* **$2000 D7 NMI polarity** and the **VT02 low-luminance palette corner**
  are spec-vs-code notes, not bugs. See DATASHEET_DIGEST.md appendix B
  before "fixing" either.

## 75. furb_cli: the reference emulator, headless, and compare_furb.py (after s21b62)

No emulator change. Michael supplied the full Furbtendulator source
(Furbtendulator-main.zip, unpacked to the gitignored reference/). It now builds
as a 32-bit Linux CLI (tools/furb_cli, README there), and tools/compare_furb.py
runs one .nes through it and through PocketVT (builder.py + mGBA) with the same
input and scores every requested frame. This replaces reference screenshots for
anything the testroms cover; ask for captures only for carts not on disk.

How it is built: compat/ shims Win32/DirectX (every GUI/DirectX/registry call
inert, all settings at compiled-in defaults: NTSC palette, VT03Palette=0); the
iNES mapper pack becomes Mappers/iNES.so loaded through a dlopen-backed
LoadLibrary, exactly like iNES.dll; furb_cli.cpp replaces WinMain and runs
NES::Thread's CPU loop, owning the keyboard state the standard controller reads.
Traps met on the way, recorded so nobody re-debugs them: (1) -m32 is required,
the savestate macros and DWORD assume a 32-bit long; (2) registry stubs must
return a NONZERO code -- Controllers::LoadSettings tests == ERROR_SUCCESS and a
zero stub made it parse garbage; (3) Settings::FSkip must be 0, a large value
makes the PPU take its frame-skip path and draw nothing (one-colour frames);
(4) __forceinline must expand to nothing -- MSVC silently emits an out-of-line
copy that other files call; (5) -msse2 -mfpmath=sse matches MSVC's float model.
Deterministic: identical runs give identical frame hashes, across clean rebuilds.

Alignment (the part that makes the scores mean something):
* Frames are NES frames on both sides. PocketVT's `frame` word (symbol
  `frametotal`, the globals-block counter timeout.s bumps once per emulated
  frame) keys the harness's input and captures. At full speed GBA frame 700 is
  only NES frame ~563 (about 140 GBA frames of boot), so GBA-frame alignment is
  wrong even for 60/60 carts.
* PocketVT's screen trails its counter by up to ~5 NES frames when it runs
  below 60 (Scramble gameplay, 47/60), so each capture is matched against
  +-8 reference frames (ties go to the nearest).
* Screen geometry: column = x + 8 always (no horizontal scaling). Row: dma0buff
  vofs = NES scroll + (source row - line), so line-to-line steps are scale75's
  decimation (1 or 2) except at a raster split; the tool rebuilds the row map
  from the steps, substitutes the 3-line rhythm at splits, and searches each
  split segment's offset separately (Scramble: playfield -1, HUD -2).
  DO NOT use y + vofs directly as the row -- that is tilemap space, and was
  only right in earlier tools because their screens were unscrolled.

First results (repo-root core, testroms): Time Pilot 100.00% struct at f200 and
f600; Scramble title 99.97% (0 mismatched px), gameplay f700 99.0%; Push the
Ball f600 97.5% (moving robot/paddles only); Add 'em Up f600 81.8% with the top
strip flagged -- the raster-split CHR gap, open item 1. On Scramble gameplay
every reference colour has exactly one PocketVT counterpart (same palette
indices, different DAC: e.g. (189,192,0) -> (148,148,0)), so there is no colour
fault; the remaining ~1% is the 1-pixel terrain outline. Per column, PocketVT's
terrain top maps one NES row ABOVE the reference's on 160 columns and to the
same row on 63. That is consistent with scale75 dropping the edge row on some
lines, but a real 1-row vertical offset in gameplay (NES scroll Y = 2 there) is
not ruled out -- worth one look with the VALUE_SENTINEL build. Scramble also diverges in game state by ~f850 (the reference's ship dies, ours
does not) -- expected over long runs, so compare early frames.

## 76. furb_cli: the rest of the GUI's features, headless (after s21b62)

No emulator change. furb_cli now covers what the GUI does, not just "run a
.nes and dump frames": the FDS, NSF and VS mapper packs; settings through
Furbtendulator's own registry loader (.reg exports or Name=value files,
--set, --save-config); every controller type on every port including Four
Score, the Famicom 4-player adapters, Zapper, keyboards, mice and the
microphones; WAV and AVI capture of the real mixer output; savestates,
movies (through the GUI's own movie dialogs), cheats, DIP switches, custom
palettes, header patching, CPU trace, battery saves; FDS disk and tape
commands. tools/furb_cli/README.md lists the options;
tools/furb_cli/selftest.py checks each one (0 failures at this commit).

How: the shim is no longer all-inert. The registry is a map; a dialog script
registered for a template ID runs the real dialog procedure against fake
controls; file pickers are answered from a queue; DirectSound captures the
samples Sound.cpp writes. The packs reach those host functions through one
exported symbol (furb_host_lookup, -Wl,--dynamic-list), because each .so
links its own static copy of compat.cpp built with -DFURB_PACK.

Traps, so nobody re-debugs them:
(1) GetModuleFileName must be real. A stub made ProgPath empty, which
    silently broke BIOS/, cheats.cfg, dip.cfg and samples/ loading.
(2) MSVC's "rt,ccs=UTF-16LE" fopen mode. The cfg files are UTF-16 and read
    with fgetwc; glibc ignores ccs=, the text came out garbled and a
    syntax-error message overflowed a buffer and crashed. open_ccs_read
    decodes the file (BOM detection) into a real temporary UTF-8 file and
    reopens it. Writing into tmpfile() does not work (the stream is already
    byte-oriented, fgetwc returns WEOF) and fmemopen crashes in getwc.
(3) NSF does not play on load, in the GUI either: CPU::Reset clears the INIT
    IRQ after the mapper reset, so the player waits for Play. --nsf-song N
    moves the song slider, sends WM_HSCROLL, and clicks Play.
(4) NES::OpenFile sets NES::Running for NSFs regardless of AutoRun. There is
    no emulation thread here, so furb_cli clears it after loading.
(5) Pad buttons must not share code space with keyboards: keyboard devices
    read KeyState by DIK code, so pads are mapped to virtual joysticks
    (device 2+port, code (dev<<16)|button).
(6) Frame hashes changed at this commit (RGB byte order), so compare hashes
    only within one furb_cli build. The frames themselves are unchanged: 48
    dumped frames over 3 ROMs are byte-identical to the previous build.

Unverified: real FDS disks (dummy BIOS only), the keyboards, mice, Arkanoid,
tablet and data recorder (plumbing exercised, no test ROM reads them), and
the VT369 hi-res dump path -- Lucky Lawn Mower VT369 (supplied this session,
in the gitignored testroms/) runs as console VT369 but never sets $201C bit 2.

## 77. Raster-split slots FIXED and on by default: four bugs, not one (after s21b62)

Open item 1 of s.73. `VT_SPLIT_SLOTS` is now defined in config.h and is on
by default; `-DVT_SPLIT_SLOTS=0` builds a core byte-identical to the one
before this section. Every number below is from `compare_furb.py` (s.75),
built with build_pvt.sh.

| cart (Start at NES 320) | before t200/t400/t700 | after |
|---|---|---|
| Add 'em Up | 60.65 / 60.36 / 81.83 % | 98.46 / 98.17 / 99.88 % |
| Aero Gyrodine | 64.42 / 100 / 100 % | 99.99 / 100 / 100 % |
| Hex City X | 68.69 / 99.53 / 99.53 % | 100 / 99.98 / 99.98 % |

Scramble, Time Pilot, Push the Ball, Table Soccer (VT03 and VT369), Lucky Lawn
Mower VT369 and VG Pocket have identical scores. Without input, Aero's title
scores 100.00% from t100 to t900 and Hex's 100.00%. Speed (NES frames per GBA
second) is unchanged on every cart that does not split. Aero's and Hex's titles
run at 32-33, down from 39 and 35 when they showed the wrong tiles. They still
need speed work (open item 2).

### 77a. Blocks 2/3 were never free: the 32K PRG copy lives there

s.73 assumed BG char blocks 2/3 were free on VT carts. They are not, on ANY VT
cart. loadcart.c's `USE_ACCELERATION` copies the last 32K of PRG to
`novrom_bank` = 0x06008000-0x0600FFFF (the first 16K to 0x06010000), and the
6502 executes from there. `vram_dump` confirmed it on Add 'em Up, Aero, Hex and
Scramble: each 8K there is byte-identical to a PRG bank. Aero survived the old
WIP only because its bank at 0x06008000 is mostly empty.

Moving PRG out of VRAM for good works but costs about 10% on every VT cart
(Aero 39->34, LLM VT369 38->34, Table Soccer VT369 35->31, Scramble 46->42).
Leaving one bank in the 8K gap between the slots does not work either. PocketNES
executes a branch as host-pointer arithmetic, so Add 'em Up's `$E011: BPL $DF98`,
run from a copy at 0x0600E000, lands in slot tiles. (PocketNES's old answer, the
256-byte prefix copy of the Arkista's Ring fix, has no room here.)

The fix moves PRG only when a cart first needs a slot, `vt_prg_evict`
(ppu_vt.c). loadcart.c records an identical EWRAM twin of the VRAM copy
(`vt_prg_shadow`: the whole-PRG EWRAM copy when PRG <= 128K, the decompressed
image, or a fresh 32K copy at the cache start). On first use every
`instant_prg_banks` entry and every speed-hack PC pointing into
0x06008000-0x0600FFFF is repointed to the twin. timeout.s `vblank_handler_0`
then calls `vt_apply_prg_banks` right after `newframe_nes_vblank` returns.
`map*_` end in `flush`, which re-encodes the 6502 PC through the new memmap
before another instruction runs. Every JMP/JSR/RTS/RTI/vector goes through
`encodePC` too, so nothing can re-enter the VRAM copy. Carts that never split
keep the fast layout.

### 77b. The IWRAM user stack is only ~470 bytes, and split slots overflowed it

The user stack runs from `__sp_usr` (0x03007D60) down to `__bss_end__`
(~0x03007B84). The vblank IRQ runs `vt_chr4_rebuild_if_dirty` in System mode on
whatever stack it interrupted. On the base core the deepest point is 0x03007C0C,
136 bytes of headroom. With slots, an IRQ landing during the frame-end chain
(`vblank_handler_0` saves 13 registers, then `newframe_nes_vblank` ->
`vt_palette_rebuild_gba` -> `vt_bands_frame_end` -> `vt_split_build` -> decoder)
went down to 0x03007B50 and overwrote `vt_prg_banks` and the IWRAM canaries.
Add 'em Up then mapped PRG bank 0 at $E000 and jumped into NES RAM. That is
the "hang after two slot assemblies" of s.73.

Found with the probes: `lbwatch` caught the jump into RAM, `memwatch` on
`_memmap_E` caught `mapEF_` loading bank 0 from `vt_apply_prg_banks`, and `spmin`
showed the System-mode SP below `__bss_end__`. Once a cart has taken slots, both
entry points now run on a 3K EWRAM stack (`vt_ewram_stack`): the frame-end call
in timeout.s, and the vblank call through `vt_chr4_rebuild_stacked` in mapVT.s.
The latter is kept in ROM because IWRAM code comes out of the same 470 bytes; an
inline version cost 40 bytes of `.iwram`. The first frame that needs a slot only
moves the PRG and builds nothing, because that call entered on the IWRAM stack.
Building even one slot in that frame, as a first version did, corrupted Aero's
game state (90% after Start). Deepest IWRAM point now: 0x03007C50, 204 bytes of
headroom, more than before.

(Superseded by s.78e: the EWRAM stack is now used unconditionally, not only after
a cart first takes slots, and there is no build-free first frame any more.)

### 77c. Stale slot lines, and an IRQ race inside vt_split_build

The two remaining faults were on Aero's title. `bg0cntbuff` persists across
frames (double-buffered), so a line that stopped being a split line kept its
slot char base. Band lines now store their original char base in BGCNT bits
4-5 (unused by the hardware), and each frame end restores every tagged line
before applying the new bands.

`vt_split_repair` runs from the vblank IRQ and could land in the middle of a
`vt_split_build`. It took the half-written page for a stomp and rebuilt it with
the OLD key; the outer build then resumed, leaving slot 3's page 0 with 49
tiles from the other bank set (the GYRODINE logo tiles over the top star strip).
`vram_dump` of both slots before and after the LRU swap located it: seven pages
exact, one mixed. The build now clears `vt_split_valid[s]` (volatile) for its
whole duration.

### 77d. Regression, and what is not verified

Frame 700 (framebuffer + palette RAM) is byte-identical to the previous core on
Scramble, Time Pilot, Push the Ball, both Table Soccers, LLM VT369 and VG Pocket.
The frame-SET test is not a strict subset: Time Pilot renders 3 frames and Push
the Ball 6 that the old core did not, and Scramble and LLM VT369 1 each. All are
in the first 110 GBA frames (boot fade and first scroll steps), and after frame
120 every frame matches. It is a phase shift from the few instructions added per
vblank, not a rendering change.

NOT verified: Star Ally, Lonely Island and Lucky Lawn Mower VT09 (not on disk
this session). They do not split, so they should behave like VG Pocket and
Scramble, but nobody has run them. `score_5bit.sh` (LLM VT09) was not run for
the same reason.

compare_furb.py now masks NES 2.0 header byte 13 to its low nibble for
furb_cli. Furbtendulator reads the whole byte as the extended console type,
and the VG Pocket 50-in-1 dump has 0x28 there, which crashed its PPU.
`--furb-arg` passes anything else through.

## 78. Title speed, a stale-decode bug behind a split, and a redundant palette build (after s.77)

Aero Gyrodine's and Hex City X's titles went from 32-33 to 34-35 NES frames per
GBA second (base core before s.77: 39 and 35, with the wrong tiles), and Lucky
Lawn Mower VT369 from 38 to 41. Every testrom's compare_furb scores are
unchanged on the build_pvt.sh path. Frame 700 is identical to the s.77 core on
the seven carts that do not split. The only new frames are boot and scroll
phase steps: boot fade (GBA frames 55, 108-109), VG Pocket's splash transition
(93; neither core matches the reference there, which switches at NES frame 23),
Time Pilot 87, and Scramble's scroll positions (87-88, 694-696; Scramble runs
below 60, so which positions get rendered depends on timing).

### 78a. Where the time goes (tools/probes/cycprof)

`cycprof` charges every step its real GBA cycles (EWRAM and ROM wait states
included) and reports per function. With `FT=<frametotal> ABS=1` it reports per
NES frame, and with `RANGE=lo-hi` per 16-byte bin, which addr2line maps to
source lines. On Aero's title, s.77 cost about 111k cycles per NES frame over
the old core: `vt_bands_frame_end` 42.6k, palette 14k, and the 6502 handlers
2-4k each because PRG now comes from EWRAM. Almost all of the frame-end cost
was the two per-line passes (restore, then tag) over up to 240 EWRAM lines,
in Thumb code from ROM. They are now branchless, two lines per 32-bit word
(`vt_lines_restore`, `vt_lines_tag`), and the restore covers only the range
tagged when that buffer was last current. A first version picked the buffer
side wrongly and rescanned all 240 lines every frame; the word loops also
need `vt_line_orig2` forced inline under -Os.

### 78b. vt_build_16color_palette ran twice per GBA vblank

The vblank IRQ built it in `vt_chr4_rebuild_if_dirty`, then `run_palette`
overwrote it, then `vt_16c_palette_fixup` built it again as the final writer.
That was about 12% of Aero's title frame. The first build is now skipped when
the fixup is certain to follow: `firstframeready` is set and the vblank is top
level. The handler stores the outer `inside_gba_vblank` in `vt_vbl_outer`
(8 bytes of IWRAM code), because a nested vblank exits before run_palette and
the fixup. In a nested vblank it builds only if its inputs (palette RAM +
$2010, snapshot `vt_pal_built`) changed since the last build. This keeps the
s21b48 rule that the palette is rebuilt every frame: the fixup still is.

### 78c. Hex City X's menu: garbage metasprite, missing cursor

A 16x16 sprite showed as noise in the middle of the menu and the cursor
vanished (99.53% vs 99.98%). The old core had it; s.77 hid it only by timing.
The cause is not the sprite path. `vt_chr_sync_flush` decodes from
`vt_frame_reg` on split frames and from the live registers otherwise, and it
only runs when a bank write asks for a sync. The title's last split frame
consumed the pending sync with the title's banks (12-15). When the menu stopped
splitting, nothing asked again, so all eight pages stayed decoded from the
title's banks indefinitely. `vt_bands_frame_end` now calls
`vt_chr_sync_from_prg` whenever the source changes: a split starts or ends, or
the primary band's registers change. A stable split still never marks
anything dirty (the s.72 storm stays fixed).

Found by: `objdump`-style OAM/palette/OBJ-VRAM dumps keyed to NES frames (same
OAM and palette, different tile data). Then `ftwatch` on the sprite tile
(`vt_obj4_overlay` wrote it in the good build and never in the bad one). Then
the page-bank dump (12-15 instead of 0-3). A first guess was hardening
`vt_obj4_overlay`'s one-word probe, which changed nothing, so it was reverted.

This fix applies with `VT_SPLIT_SLOTS=0` too, so that build is no longer
byte-identical to the pre-s.77 core.

### 78d. Still open

60/60 on these titles needs the frame roughly halved (~500k -> ~280k cycles per
NES frame). The biggest items are now the 6502 core, read_vt4xxx ($41xx polling,
~8%), joypad reads (~5%) and the remaining palette build (~12%, now once per
vblank). None of it is split-specific.

### 78e. The IWRAM stack again: unconditional EWRAM stack, and libgba's IntrTable

The shipped core (devkitARM Docker build) broke Aero Gyrodine with 78b: title
64%, game stuck on the title. The build_pvt.sh core was fine. `spmin` from
power-on showed why: the user stack reached 0x03007B80, 68 bytes below
`__bss_end__`, inside `vt_chr_sync_flush` run from the vblank IRQ, and
overwrote `vt_prg_banks` with 0 and $3F. This was during boot, BEFORE any PRG
move, while s.77 still switched to the EWRAM stack only after one. 78b only
changed when the IRQ landed; bisecting the Docker build (78a, 78b, 78c
reverted one at a time) pinned it to 78b's timing change, not to any logic in
78b.

Two fixes:
1. Every heavy VT C entry now runs on the 3K EWRAM stack unconditionally. The
   vblank IRQ's `vt_chr4_rebuild_if_dirty` and `vt_16c_palette_fixup` go
   through ROM trampolines (`vt_ewram_trampoline` in mapVT.s; IWRAM code
   would come out of the same budget), and timeout.s switches for
   `newframe_nes_vblank`. If the interrupted code is already on the EWRAM
   stack, it keeps going down it. The slot-free first frame after the PRG
   move is gone; that frame now runs on the EWRAM stack too.
2. The Docker link pulled in `libgba.a(interrupt.o)`. PocketVT's
   `IntFn IntrTable[14];` was a -fcommon tentative definition, and GNU ld
   extracts an archive member that defines a still-COMMON symbol. libgba's
   120-byte `IntrTable` then won the merge: 64 bytes of IWRAM .bss, taken
   straight out of the stack. build_pvt.sh never links libgba, which is why
   only the shipped core had 64 bytes less stack. `IntrTable` is now
   initialised (a real definition); the Docker core's `__bss_end__` equals
   build_pvt's (0x03007B84) and its ROM is 968 bytes smaller.

Measured on the shipped core from power-on (12M steps each, Aero, Add 'em Up,
Hex, Scramble, VG Pocket, LLM VT369): the deepest IWRAM point is 0x03007C1C,
152 bytes of headroom, in non-VT code (`__clzdi2`, `spriteinit`). The EWRAM
stack peaks at about 380 of 3072 bytes.

Docker-vs-Docker scores (84f17d3 core -> this one, Start at 320): Aero title
100 -> 99.99, Hex 100 -> 99.99, Add 'em Up t700 99.88 -> 99.85, Scramble
99.97/99.00 -> 99.96/98.98, Push the Ball 97.49 -> 97.44. The Aero, Hex, Add
'em Up and Scramble deltas are 2-9 px from capturing at a different GBA frame
(faster core). Push the Ball's is a stable 20 px (one pixel column of the text
box border). NES RAM and CHR RAM are identical between the two cores at NES
frame 200, but ~9 bytes of the GBA tile cache differ. So PocketNES's CHR-RAM
tile conversion misses an update under some timings: pre-existing, and not
chased yet.

## 79. Table Soccer runs, VG Pocket's colours, the real BIOS, and wait states (after s.78)

Everything here was measured with tools/compare_furb.py against Furbtendulator,
keyed to NES frames, on the Docker-built core unless a build_pvt.sh core is named.

### 79a. Table Soccer VT03 (mapper 419): four independent faults

Mapper 419 (Taikee TK-8007 MCU) used to fall through to mapper 163 and showed
nothing. It is OneBus with the same PPU/MMC3 mangle tables and no CPU mangle,
plus a 3-bit ADPCM chip that the game handshakes with: $4016 bit 2 is a data
clock (rising edge latches the high nibble, falling edge the low one), $410F
bits 0-3 are the nibble, and $4017 reads return READY in bit 4 and NOT-clock in
bit 3 (reference/nrs/mapper419.cpp and s_ADPCM3Bit.cpp). The game spins on
AND #$18 of $4017 at $F5FC before every byte, so without the status bits it
never leaves its first command. loadcart.c routes 419 to OneBus with submapper
tag 0x40; vt_regs.c models the command protocol and the 12-frame buffer
(drained from frametotal) so READY behaves; mapVT.s installs read_tk4xxx and
write_tk4xxx only for this mapper, and the $410F nibble is stored straight from
asm because the game streams it 6000+ times a second. The decoded samples are
not played.

With the CPU running, the title had four 64-line bands of distinct CHR banks
but only two split slots, so the LRU rebuilt a slot for every band every frame
(65% of the CPU) and the first band showed the third band's tiles. Three
changes fix that without new VRAM. A slot used by an earlier band of the same
frame is never evicted by a later one. When a frame needs a third extra bank
set and no line of the frame shows the primary's other pattern half, that
half's char block is lent as slot 2 (vt_blk_lent; vt_chr4_copy_to_vram* skip
it while lent, and it is handed back the first frame that does not need it).
And slots go to the tallest bands first, so a frame with more bands than slots
(the formation screen has six) loses only its thinnest strips. Because slot 2
can now be block 0 or 1, the bg0cntbuff tag mark moved from "char base bit 3"
to BGCNT bit 6 (mosaic, which has no effect while REG_MOSAIC is 0; PocketVT
never writes it).

The menu text is PIX16EN sprites without SPEXTEN ($2010 = $87). The PIX16 path
existed but required SPEXTEN and refused COLCOMP, so these fell to the 8-wide
4bpp overlay: every letter an opaque block. vt_spr_eva_update now takes this
case with EVA forced to 0, plain page addressing (vt_assemble_page_pix16_plain)
and the slot keyed on the page's current bank, publishing all eight EVA
entries of spr_cache_map because update_sprites adds OAM byte-2 bits
regardless.

The match pitch came out as bare grass. The nametables proved it: every grass
tile was 0. The game DMAs the pitch from ROM $B800 (PRG offset $1800, 8K bank
0) with $4034 = $01, and loadcart.c's USE_ACCELERATION keeps PRG 16K page 0 in
OBJ VRAM 0x06010000-0x06013FFF, exactly where the extended sprite slots are
written. Every read of page 0 after the first slot write returned sprite
pixels. This was latent for every VT cart since the EVA path went in. Before
the first slot write, vt_prg_evict_obj now repoints the page-0 banks at an
identical twin (cart ROM, or EWRAM when the PRG was decompressed or cached;
vt_prg_page0, set by loadcart.c) and raises vt_prg_evict_pending. The
timeout.s hook that rebuilds the memmap is now VT-wide rather than split-slot
only, and slot writes wait until it has run.

Finally the split lines drifted. Furbtendulator's OneBus counter
(h_OneBus.cpp clockScanlineCounter) is clocked once per scanline at dot 256
when BK16EN or TSYNEN is set, reloads on the clock after it reaches zero and
fires at zero. A free-running period of N is therefore N+1 lines, and a $4102
rephase inside the picture takes effect on the next line's clock. PocketVT
counted plain N. Calibrated against Furbtendulator's traces (writes to $2016 at
lines 39/47/55/79/223 on the formation screen, 60/126/189 on the title),
sound.s now uses N for an arm from vblank (its first band was already exact),
N+2 for a rephase inside the picture, and N+1 for the free-running reload.
Both screens' band lines now match Furbtendulator exactly
(VT_TIMER_NPLUS1, default 1). Aero, Hex, Add 'em Up and Star Ally are
unchanged by it.

Results: title 96-99%, team select 99%, match 97% (the COM team is a random
pick and differs), formation screen 92.8% (the two six-line shirt strips would
need a sixth char block). Speed rose from 5 NES fps at first boot to 43-44 with
79c.

### 79b. VG Pocket: the DAC table was a fit, and 2bpp mode ignored it

Palette RAM in the VG games matched Furbtendulator byte for byte; the colours
did not. vt_compat_rgb555_vg had been fitted to four menu captures under index
models later disproved, and entries those menus never used were guesses:
$04, $29 and $37 all mapped to one green, $28 and $2C to one light blue, which
is why Get it Right's peach face was green. furb_cli --dump-palette now writes
Furbtendulator's active colour tables, and the VG table is its VT09 colours
(RGB888 >> 3); the fitted table is kept under #if 0. Games that run with
$2010 = 0 (plain 2bpp) went through run_palette and PocketNES's stock nes_rgb,
not the console's DAC. A cart whose header names its DAC now gets it there
too: vt_mapped_rgb_fixup rewrites MAPPED_RGB whenever it differs, called from
vt_palette_rebuild_gba once per frame. A bl_long placed inside paletteinit
was tried first and a Scramble core stopped booting. At the same time new
globals had landed in IWRAM .bss, so which of the two broke it is not
separated. Both were undone, and all new globals are EWRAM_BSS
(__bss_end__ back to 0x03007B84).

One racer lost its dashboard: it switches BG CHR at line 174 through MMC3
$8000/$8001. The s.73 comment said mapVT.s marks bands after MMC3 bank writes;
it never did. vt_mmc3_forward now flags CHR changes and write_vt_rom records
the band at get_scanline_2. The split slots also work for 2bpp backgrounds now
(vt_chr2_assemble_page_vram; lending stays 4bpp-only because PocketNES's 2bpp
cache keeps rewriting the other half). That game went from 81.3% to 99.4%.

Across all 50 games at NES frame 900 the mean exact-colour match went from
22% to 98.3% on the shipped core. A few games show a band of missing BG tiles whose size moves
with timing (88-100% between builds that differ only in timing). Their
nametables match Furbtendulator exactly, so this is the tile-cache staleness
of s.78e / open item 21, not a palette or banking fault. Lucky Lawn Mower VT09's
header names no DAC, so it inherits the VG table through V16BEN and now matches
Furbtendulator exactly as well; its earlier colours were fitted to the gg.png
and lawn.png photos, which are not in the tree, so that comparison is still
owed.

### 79c. Real hardware: the header, the BIOS, and cart wait states

mGBA 0.10 skips the BIOS without a word when a cart's Nintendo logo is invalid
("Invalid logo, skipping BIOS"); a real GBA's BIOS refuses to start such a
cart. build_pvt.sh called /opt/devkitpro/tools/bin/gbafix, which is not
installed on that path, and ignored the failure, so every build_pvt.sh core had
a zeroed logo and would not have booted on hardware. The Docker core was always
fine (the Makefile runs gbafix). tools/gbafix.py now does the same job, copying
the logo from a ROM that already carries it (the committed pocketvt.gba,
checked by CRC32 so no logo bytes are in the tree), and builder.py warns if the
play ROM's header would not boot.

With the user's BIOS dump (reference/gba_bios.bin, never committed) pvt_run
and compare_furb.py --bios boot through the real BIOS: the full intro, the
header check and the real SWI routines. Every test ROM boots and renders the
same as under mGBA's HLE BIOS; the few differences are frame-phase
(Aero's title 100 vs 99.5 at one frame, Star Ally's BKEXTEN repair cycle).
tools/probes/biosboot reports when a ROM leaves the BIOS.

Nothing ever wrote WAITCNT, so the cart ran at the power-on 4/2 wait states
without the prefetch buffer. crt0 now sets 0x4317 (3/1 plus prefetch, what
retail games use; VT_FAST_WAITCNT, default 1). PocketVT runs much of its VT
code as Thumb C from ROM, so this is the largest speed change in a while:
Time Pilot t700 34 -> 58 NES fps, Scramble 48 -> 59, Star Ally 36-38 -> 44-46,
Aero and Hex titles 34 -> 41-42, Table Soccer 28 -> 43, LLM VT09 54 -> 60.
mGBA models both the wait states and the prefetch buffer, so these numbers
should carry to hardware; that is the one claim here not checked on a GBA.

### 79d. Sound: what is and is not emulated

Measured with tools/probes/pvtwav (PocketVT audio between two NES frames) and
wavcmp.py against furb_cli --wav. Plain 2A03 music is right where the cart runs
at full speed: Lonely Island's loudness envelope correlates 0.85 with
Furbtendulator's, against 0.1-0.5 for unrelated music. Scramble scores 0.54.
Star Ally cannot be judged this way while it runs below full speed, because its
music plays proportionally slower. VT-specific PCM is weaker. The VT02+ ADPCM
channels ($4120-$412F) are decoded but mixed only inside timer1interrupt, which
is PocketNES's DMC refill and stops when the DMC is idle, so on their own they
are effectively silent, and the IMA table is not VT369's. The TK-8007 voice in
Table Soccer is handshaked but not played. The second APU ($4020-$402F) and the
VT369 sound CPU are not emulated. Among the test carts only Lucky Lawn Mower
VT369 (which does not boot yet) and Table Soccer use any of these. No test cart
played NES DMC samples in the windows sampled, including all 50 VG games,
although the VG ROM contains DMC code. One real fault was removed on the way:
vt_adpcm_mix_gba carried a second copy of the VT timer that subtracted 114 CPU
cycles per call from a period counted in scanlines. That raised IRQ_MAPPER on
every DMC refill for any game with its timer IRQ enabled.

### 79e. Speed work that is not in 79c

The ADPCM nibble stores skip vt_reg_write. vt_spr_eva_update handles each
(page, EVA) key once per call instead of once per sprite.
vt_palette_rebuild_gba no longer re-derives all 512 vt_palette_to_gba[]
entries every frame: nothing reads that table, and it was 5.5% of Table
Soccer's match frame on every VT cart.

### 79f. Tools added

furb_cli dumps the nametables (.nt) and its colour tables (--dump-palette).
pvt_run takes PVT_DUMP (memory ranges at each target) and PVT_BIOS. New probes:
peek, memdump, biosboot, pvtwav and wavcmp.py. ftwatch takes several key spans,
and cycprof can warm up to an NES frame with input (WARMFT, KEYS). The probes
README lists them.

### 79g. Star Ally paid for the speed, and the Docker build can be stale

On the Docker core, Star Ally averaged about one point lower over NES frames
500-900 than the previous Docker core (97.8 against 98.9), with parts of the
planet missing on some frames. The build_pvt.sh core showed no drop, and
neither the timer change nor any logic change moved it. A Docker build with
WAITCNT left at power-on brought it back to 98.9, at 39 NES fps instead of 48.
The cause is rate, not logic. Star Ally's BKEXTEN map is kept current partly
by vt_bk_scrub, a safety-net sweep of 60 cells per GBA vblank (full coverage
every 32 GBA frames), sized when ROM code ran at 4/2 wait states. At 48 NES fps
more of the map changes between sweeps. The sweep size is now
VT_BK_SCRUB_SPLIT chunks per screen. 8 (120 cells, full coverage every 16
frames) brought gameplay back to 98.65 at 44 NES fps, but left the title
screen permanently half-drawn (84% from NES frame 130 on). The likely cause is
that wider sweeps touch more (page, attr) pairs per vblank and thrash the
10-slot BKEXTEN cache; this is not verified. The default stays 16 (60 cells).
Star Ally therefore ships about one point lower in gameplay (97.8 over frames
500-900, some frames missing planet chunks) at 48 NES fps instead of 36. A real
fix is to budget the sweep per NES frame, or to grow the slot cache, not
simply to widen the sweep. Open.

That test first seemed to show WAITCNT was not the cause, because the "off"
core still had it. The devkitARM Makefile builds incrementally in build/, and
its asm rules do not generate header dependencies (the -MMD lines are
commented out), so changing a #define in config.h does NOT rebuild the .s files
that test it. Clean first (`rm -rf build`) whenever a config.h default or an
asm-visible define changes. The shipping core here was built both clean and
incrementally, and the two are byte-identical.

### 79h. Two corrections after s.79

Table Soccer VT369 does NOT need VT369-00.BIN to run in Furbtendulator. The
emulator prints "Plug-through device: File ...VT369-00.BIN not found" and
carries on. An earlier s.79 run looked like a failure only because furb_cli's
output was piped into `head -2`, and the SIGPIPE killed it after two lines.
Both VT369 carts run in furb_cli from power-on. Table Soccer VT369 reaches its
team select with the Chilean club teams. The user's standalone furb_cli
repository (GUI plus native 64-bit builds) gives byte-identical frames to
tools/furb_cli, because its Furbtendulator source is the same. Lesson: never
pipe furb_cli (or any run whose output files you then check) into `head`.

The user retired the LLM photo references (gg.png, lawn.png): Furbtendulator,
through compare_furb, is the colour reference from now on. The VG table
change in 79b therefore stands for Lucky Lawn Mower VT09 as well (exact5
99.7%).

## 80. VT369, part 1: the CPU side runs; the enhanced renderer is next

Both VT369 carts ran NES frames but sat in polling loops forever. Lucky Lawn
Mower VT369 waited at $E882 for $41B7 bit 2. NintendulatorNRS answers four
$41xx reads with constants on every OneBus console ("various games, unknown
purpose"): $415C = $10, $418A = $04, $41B7 = $04, $41B9 = $80. read_vt4xxx in
mapVT.s now returns them. Next it waited at $C1BD for $4136 to reach 0. That
is the busy count of the VT32/VT369 multiply/divide unit, which PocketVT did
not have (vt_regs.h described an incorrect register layout for it). Writing
$4135 multiplies the low 16 bits of $4130-$4133 by $4134-$4135. Writing $4137
divides $4130-$4133 by $4136-$4137, leaving the quotient in $4130-$4133 and the
remainder in $4134-$4135. $4130-$413D read the results, $4138-$413D mirroring
$4130-$4135. vt_reg_write computes the result at write time and fills
vt_alu_rd[], which the asm read hook returns without a C call, and $4136 reads
0 (ready at once). It is enabled for console types VT32 and VT369 only
(vt_alu_on, from the NES 2.0 header in loadcart.c). $4119 now reads 0 on
VT369, as in APU_VT369::IntRead. After this both carts run game logic.
Lucky Lawn Mower VT369 executes its gameplay code, including code in PRG page
0, but both screens are still black.

The black screen is the renderer, not the CPU. Everything below is from
OneBus_VT369.cpp and NES.cpp. With $201E != 0 (Lucky Lawn Mower VT369 has
$0F) the PPU runs RunNoSkipEnhanced. BG tiles are packed 4bpp, eight
four-byte rows with the LOW nibble the left pixel, which is the GBA's own
4bpp tile format, so they can go from PRG to VRAM with no conversion. The
pattern address is (tile << 4 | fineY*2) << ($201C & 3), plus
vt369bgData = ($2020 | $2021 << 8) << 13 when $201E bit 0 is set. $201C bit 3
clear adds the attribute bits as tile-number bits 8-9. $201C & 3 == 2 selects
8bpp. Colours are 16-bit pairs from a 1024-byte palette,
Palette[TC << 1] | Palette[TC << 1 | 1] << 8, into the PALETTE_VT369 colour
table. The CPU writes that palette through $5000-$5FFF
(writeVT369Palette: Palette[addr & ($201E ? 0x3FF : 0xFF)]). Sprites have an
enhanced OAM layout when $201E bit 2 is set, and $201C bit 2 is a hi-res
mode. The platform also maps the 4 KB embedded ROM (NES 2.0 misc ROM) at
$1000-$1FFF and VRAM into CPU space at $3000-$3FFF.

Plan, in order, each step checkable with compare_furb:
1. $5000-$5FFF palette writes and the 16-bit colour conversion (dump
   PALETTE_VT369 with furb_cli --dump-palette, extended for it).
2. Enhanced BG: map entries get 10-bit tile numbers from the attribute bits;
   tiles are DMA'd from PRG into a BG char block as they are (4bpp). Reuse the
   BKEXTEN slot machinery (vt_bk_*) for tile-bank caching.
3. Enhanced sprites and OAM.
4. $1000-$1FFF embedded ROM and $3000-$3FFF VRAM access, if the carts use
   them (trace first).
5. The sound CPU: HLE only, later (Settings::VT369SoundHLE is the reference's
   own shortcut).

Cost check. The first version put the new compares in front of every
$4000-$41FF read, joypad included, and Aero and Hex titles fell from 41-42 to
35-36 NES fps. read_vt4xxx now sends $40xx straight to IO_R with a single
unsigned compare, which made those titles faster than before (43/44). Every
other test ROM scores the same (Star Ally passes the frame-set test: all 32
distinct frames over 800 were rendered by the old core). Aero's title drops
from 100% to 99.17% at frames 140-200, because its small planet sprite rests
at another spot, and 6 of its 28 distinct frames are new. Aero reads no $41xx
register at all (traced over frames 0-150). Its NES RAM differs from the old
core only in the interrupted stack bytes and $0050, and the new core is
slightly CLOSER to Furbtendulator's RAM there (5 differing bytes against 6).
So this is display timing from the speed change, not a state fault. It is
recorded here rather than left silent.

## 81. VT369, part 2: Table Soccer VT369 runs; the palette keeps 15-bit colours

Table Soccer VT369 was black, and the renderer was not the reason. Tracing
furb_cli from reset showed that the cart's boot code calls $603D in its first
instructions, before any RAM copy could have put code there. On VT369,
$411C bit 6 maps PRG ROM at $6000-$7FFF, bank $4112 through the usual
prgAND/prgOR (h_OneBus.cpp syncPRG, `ROM->ConsoleType == CONSOLE_VT369 &&
reg4100[0x1C] & 0x40`). The cart writes $411C = $C0 and $4112 = $3D at boot.
PocketVT left SRAM there, so the 6502 ran zeros from $603D, never uploaded its
palette, and the game went on in a broken state. vt_recompute_prg_banks now
computes vt_prg6_rom and vt_prg_bank6 (both EWRAM), a $4112 or $411C write
recomputes them on VT369, and vt_apply_prg_banks in mapVT.s maps the bank with
map67_ and installs rom_R60 and empty_W, as mapper 40 does. With the bit clear
it restores sram_R, sram_W and the default memmap_6. Other consoles never
reach that code.

The second fault was in vt_reg_write. It treats $4140-$417F as a palette
window and $4180-$41FF as extended OAM, a leftover from s21b55. On VT369 those
addresses belong to the sound CPU and other hardware: Table Soccer VT369
writes $4144, $4148, $414C, $4165 and $4189 at boot. Those writes landed in
vt_palette_ram as colours $30, $32 and $3F. vt_reg_write now returns before
both windows on VT369. VT03 and VT09 are unchanged.

The palette step of the s.80 plan is done for the non-enhanced renderer. A
VT369 COLCOMP colour is 15 bits, 0RRRRRGGGGGBBBBB, from the palette byte
pair (PPU_VT369::GetPalIndex, `Palette[TC|0x80] << 8 | Palette[TC]`, into
PALETTE_VT369), so all eight bits of both bytes matter. vt_palette_ram now
keeps the full byte on every write path: the low-bank path in ppu.s
VRAM_pal, the high-bank path VRAM_pal_hi with vt_palette_write_hi, and the
palette DMA fast path vt_pal_dma_fast. nes_palette, the 32-byte view the 2bpp
renderer reads, is still masked to six bits. Every VT03/VT09 reader of
vt_palette_ram already masked with & 0x3F. vt369_col() converts a pair to
the GBA's BGR555. vt_palette_rebuild_gba uses it through vt_colcomp_col(),
and vt_build_16color_palette has its own VT369 loop, so the VT03 loop is
the same code as before. An earlier draft checked the console for each of
the 128 entries, which cost Table Soccer VT03 about one NES frame per
second (43-44 down to 42). The split loop measures 43.1 against the old
core's 43.2 over 18 seconds, which is noise.

Results against Furbtendulator (the Docker core, also booted through the real
BIOS): Table Soccer VT369 was 15.6% struct (black). The team-select screen
now scores 99.8-99.9% with exact colours equal to struct, and the match
96.7% at frame 850. Past about frame 1100 the two emulators play different
matches and the score means nothing. The select screen's remaining
differences are a few flag tiles (Italy, France) and the P1 cursor sprite.
Speed is 43 NES fps on the menus and 40 in the match. The cart sets $411C
bit 7 (CPU times three), which PocketVT does not model. Whether the game
needs the extra CPU time is not yet known.

Regression against the core before this change: Lonely Island and VG Pocket
are byte-identical for all 800 frames. Star Ally's 32 distinct frames all
appeared in the old core's set. Scramble, LLM VT09 and Table Soccer VT03
each have one new frame. Each is a transition (Scramble's title appearing
one frame earlier, one raster line in LLM's intro, a half-drawn loading
screen in Table Soccer), and their frame-700 pictures are identical.
compare_furb struct and exact5 scores are unchanged on every test ROM at
150/400/700, and Star Ally's frame 700 is higher (99.10% against 98.61%).
Lucky Lawn Mower VT369, Fire Fighter and Jewel Master are unchanged and still
black. They run the enhanced renderer ($201E = $0F), which is the next step.
Fire Fighter and Jewel Master use its 8bpp mode ($201C = $12), and Lucky Lawn
Mower VT369 its 4bpp mode ($201C = $11).

## 82. VT369, part 3: the enhanced picture ($201E != 0)

Lucky Lawn Mower VT369, Fire Fighter and Jewel Master all run the VT369
enhanced renderer: $201E = $0F (pattern bases from $2020-$2023, the new OAM
layout), $201D = $0F (128 sprites of 16x16 pixels) and attribute bits used as
tile-number bits 8-9. Lucky Lawn Mower VT369 has 4bpp backgrounds
($201C = $11), and the other two have 8bpp ($201C = $12). All three were
black. The new file src/ppu_vt369.c draws them. It follows
OneBus_VT369.cpp (RunNoSkipEnhanced, ProcessSpritesEnhanced, GetPalIndex),
and the carts' own traces showed which parts they use.

Backgrounds. An enhanced BG tile comes straight out of PRG ROM. 4bpp tiles
are 32 bytes with the low nibble as the left pixel, and 8bpp tiles are 64
bytes with one byte per pixel. Those are exactly the GBA's 4bpp and 8bpp tile
formats, so a tile is a plain copy. The tile number of a cell is its
nametable byte, plus its two attribute bits as bits 8-9 unless $201C bit 3 is
set. A tile starts at ($2020 | $2021 << 8) << 13 + tile * 32 or 64 when
$201E bit 0 is set. With the bit clear, it starts from the $2012/$2016 bank.
Fire Fighter's screen uses 743 distinct 8bpp tiles (47 KB), more than the
free BG VRAM below the map holds, so tiles are cached. Every distinct tile
number gets a VRAM slot, and each slot is reference-counted by the map cells
that show it. Slots come first from BG VRAM that nothing else needs: tiles
1-127, 256-383 and 448-511 in 8bpp (or 2-255, 512-767 and 896-1023 in 4bpp)
from char base 0. Those ranges skip the UI layer at 0x2000-0x3FFF, whose
off-screen cells point at tiles 0 and 1, and the game's tilemap at
0x6000-0x6FFF. When those run out, an 8bpp screen gets the 32K at 0x06008000,
after vt_prg_evict has moved the PRG copy from there (s.77a).
vt369_high_slots_ok wraps it with the pending/apply handshake that
vt_prg_evict_obj already uses from the GBA vblank. A cell that finds no free
slot is retried by a full sweep on the next vblank. That happens for a few
frames while the PRG copy moves, and then Fire Fighter holds all 743 tiles
with no misses. Each vblank, the map is updated by diffing the nametable RAM
against a shadow copy. A changed name byte re-derives its cell, and a changed
attribute byte re-derives 16 cells. vt369_frame_end sets 8bpp or 4bpp, with
char base 0, in every line of PocketNES's per-line BG0CNT buffer. PocketNES
itself still does the scrolling and scaling. Its own BG writers are switched
off through vt_bkexten_live, the gates BKEXTEN already has, and
vt_chr4_rebuild_if_dirty and vt_bk_consume return at once in enhanced mode.

Palette. Enhanced colours are 15-bit pairs from a 1024-byte palette, with BG
colour c at 2c and sprite colour c at 0x200 + 2c. The CPU reaches it through
$2007 at $3C00-$3FFF: while enhanced mode is on, vram_write_tbl[15] points
at vt369_pal_W (mapVT.s). The carts load it every frame with fast DMA
($411C bit 7). $4034 selects the target ($2004 or $2007), $2006 sets the
address, and each $4014 write moves 256 bytes. vt369_dma_4014 (C, called from
write_vt4xxx) does those copies a word at a time into the 1024-byte palette
or the 512-byte OAM. Only a $2007 target below $3C00, such as the nametable
loads from ROM, still takes the old per-byte path. The GBA palette is
rewritten every vblank as the last palette writer, since run_palette changes
parts of it before that, but only palette words that changed are
re-converted. The first version skipped the aligned(4) on the converted
buffer, and its word copies rotated every odd colour, so Lucky Lawn Mower's
sky came out black.

Sprites. OAM has the new arrangement ($201E bit 2): Y at n, tile low byte at
0x80+n, attribute at 0x100+n and X at 0x180+n. With $201D bit 3 the
attribute's top bits are negative X/Y instead of flips. A 16x16 4bpp sprite
is 8 bytes per row, which are the halves of two GBA tile rows, so a sprite
becomes four GBA tiles in one of 128 OBJ VRAM slots at 0x06010000. Those
slots are cached by tile number, and a slot used in the current frame is
never evicted. Sprite n is GBA OBJ n, so lower numbers stay in front as on
the VT369. The position uses update_sprites' own transform: YSCALE_LOOKUP
minus windowtop, SCREEN_LEFT, and in SCALED_SPRITES mode an affine
double-size box centred where update_sprites centres its 8x8 box. The flip
matrices are 8, 16 and 24. A one-line correction, VT369_SPR_DY, was tried at
-2, -1 and +1. +1 helped Fire Fighter and hurt the other two, so it stays 0.
PocketNES's own OAM copy is filled with Y = $FF on every sprite DMA, so its
sprite pass hides entries 0-63 and never caches tiles over ours. That pass
still clears entries 0-63 each vblank, so all 128 entries are written again
afterwards. An entry is rebuilt only when its four OAM bytes change, which
brought the per-frame cost down from 12 NES fps to near zero. 8bpp sprites
(8x16, or $201C bit 5) are not done yet.

$3000-$3FFF. The VT369 maps nametable RAM into CPU space at $3000-$3FFF
(h_OneBus.cpp readNT/writeNT). Lucky Lawn Mower VT369 mows its lawn by
copying cut-grass tiles from $34xx to $32xx with LDA/STA ($zp),Y, and it never
touches $2007. PocketVT sent those writes to its extended-register handler, so
the lawn never got cut (about 51 cells wrong at frame 400). For VT369
carts, mapVTinit now installs vt369_ppu_R and vt369_ppu_W as readmem_2 and
writemem_2. They send $3000-$3FFF to the nametables (reads through vram_map,
writes through vram_write_direct), and everything else still goes to PPU_R
and PPU_W.

Speed. The first working version cost Lucky Lawn Mower 52 -> 38 NES fps.
cycprof put 39% in vt369_vblank and 13% in the DMA. The nametable diff had
been inlined into one large function that spilled to the EWRAM stack, where
each spill costs 3 cycles, so it ran at about 66 cycles per word. It is now
split into small non-inlined functions, the palette conversion only redoes
changed words, the DMA copies words, and sprites are rebuilt only when they
change.

Results against Furbtendulator (build_pvt.sh core, NES frames 150/400/700):
Lucky Lawn Mower VT369 goes from 77% (black) to 99.66/99.67/99.73% at
60 NES fps. Fire Fighter goes from 16% to 96.5/96.3/96.3% at 44-51 fps.
Jewel Master goes from 34% to 99.85/96.9/94.8% at 60 fps. exact5 equals
struct to within 0.1% in all three. What is left in Fire Fighter and Jewel
Master is game state rather than drawing: burning windows and falling jewels
in other places, and Jewel Master's next piece is different, so the two
emulators' random numbers have diverged. Table Soccer VT369 (non-enhanced) is
unchanged at 98.68%.

Regression against the merged s.81 core: Lonely Island, VG Pocket and
Scramble are byte-identical for 800 frames. Star Ally has 2 new frames, and
Table Soccer VT03, Lucky Lawn Mower VT09 and Table Soccer VT369 have 1, 1 and
3. Each one is a single raster line or a few pixels in a boot or loading
frame, and frame 700 is identical in every case. Not done: 8bpp sprites,
hi-res mode ($201C bit 2), sprite priority against 8bpp BG colour 0 (GBA
priority 3 behind BG0 is used), CPU x3, and the sound CPU.

Why Jewel Master drifts. Its CPU RAM matches Furbtendulator's from frame 20
on, apart from frame counters one frame apart (capture alignment) and $44,
which differs from the start (7E against 57 at frame 20) and looks like the
random-number state. From about frame 300 the attract-mode games differ,
and at frame 1500 the reference is already in its game-over fill while
PocketVT's game is still running. The seed source is not found yet. Reading
uninitialised RAM, the sound CPU's registers and the timer are the
candidates to trace first.

## 83. Correction to s.82: Jewel Master's drift is the missing CPU x3, not the RNG

s.82 said Jewel Master's random-number byte $44 differs from frame 20 and
blamed an unknown seed. That was wrong. The RNG is $44 = $44 * $15 + 1
through the multiply unit, stepped once per frame at $E082 (and on demand
through $C80B). PocketVT's 7E at frame 20 is exactly one step before
Furbtendulator's 57 (7E * 15 + 1 = A57), so it is the same one-frame capture
offset as the frame counters. With that offset removed, all 4K of CPU RAM
match through frame 255. Between frames 256 and 259, $3A (a frame counter)
moves two frames apart instead of one, which means PocketVT dropped a frame:
the game's main loop overran it. The cart sets $411C bit 7, which runs the
CPU at three times the clock (5.37 MHz, updatePrescaler in OneBus_VT369.cpp),
and PocketVT runs it at 1x. A frame that fits on the chip lags in PocketVT,
and after that the attract-mode game plays out differently. Fire Fighter's
drift is presumably the same thing, but that is not traced.

CPU x3 is not attempted yet. PocketNES charges 3 dots per CPU cycle inside
every opcode macro (6502mac.h, `count*3*CYCLE`), so a runtime x3 needs a
second opcode table. The other route, tripling the per-scanline budget,
breaks the dot-based VT timer and the raster-split timing. Either way the
6502 does up to three times the work on frames that use it. Fire Fighter's
profile (cycprof, gameplay) puts the 6502 core at about 11%, the enhanced
renderer at 19% and PRG bank switching at 15%, from 20-30 $4107/$4108
writes a frame.

Bank switching was tried here and backed out. Applying only the windows
whose bank changed saved nothing on Fire Fighter, because its writes really
do alternate banks. It also lowered Table Soccer VT369 from 98.68% to 98.51%,
so something there depends on the unconditional re-apply of the $6000
window or the readmem_6/writemem_6 handlers. That dependency is not
understood. Leave vt_apply_prg_banks as it is until it is.

## 84. VT369, part 4: the sound CPU (high-level), and a correction to s.83

The VT369 has a second 6502 for sound. The main CPU sees its RAM at
$4800-$4FFF (the sound CPU's $1800-$1FFF), writes a program address into
the timer-IRQ vector at $1FF8/$1FF9 (Furbtendulator calls it the reset
vector) and starts it with $4162 = $0D. The programs come from the cart's
4K embedded ROM, which is the NES 2.0 misc ROM after PRG and CHR in the file.
The main CPU sees that ROM at $1000-$1FFF and the sound CPU at $4000-$4FFF.
Lucky Lawn Mower, Fire Fighter and Jewel Master use $40AE, a four-channel
player of 48-nibble ADPCM frames read straight from PRG. Table Soccer VT369
copies its own program into sound RAM and uses $0293, three channels of
3-bit ADPCM that the main CPU streams through 128-byte rings in sound RAM.

src/vt369_snd.c does what Furbtendulator's default "VT369 sound HLE"
does (OneBus_VT369.cpp, APU_VT369::Run). It does not run the sound program.
It recognises the vector and performs that program's effect on the sound
RAM: channel state, addresses, the active mask and the ring read offsets.
It outputs one sample per sound-CPU timer tick, 13,983 Hz for $40AE and
8,008 Hz for Table Soccer. The rendering is batched, channel by channel,
128 ticks at a time. tools/probes/vt369snd_test.c builds the file natively
and checks it against a direct port of the reference's per-tick code on
random reachable states: 18,000 fills of 128 ticks agree on every output
sample and every byte of sound RAM. A mutation of the start logic fails 434
trials.

Around the HLE, the main CPU side needed three things. loadcart.c finds the
misc ROM, and mapVTinit installs vt369_ram_R for $1000-$1FFF. The carts copy
their sound vectors from there, so without it the vector read as zero and
nothing played. read_vt369_4xxx and write_vt369_4xxx give the main CPU the
sound RAM. Writes store in assembly and call C only for $1FA2 (the $0293
command byte) and the $40AE start and stop masks, because Table Soccer
streams about 50 bytes a frame into the rings. The last piece is the GPIO
ports $4140-$415F, from NintendulatorNRS h_OneBus_GPIO.cpp with nothing
attached. A read returns the mask at +0, the latch at +2, the inverted mask
at +3 and $FF otherwise. Lucky Lawn Mower counts $0FFA once a frame only
while $414F reads $FF, and with the ports in place its RAM matches the
reference exactly at frames 400 and 500.

### 84a. Starts are edges

The reference re-applies a held start bit on every tick. That costs nothing
there, because the games clear the bit one tick later. Jewel Master writes
the bit to $48A4, spins until the channel's bit comes up in $48A5, then
clears $48A4. PocketVT renders a fill of 128 ticks at once, so the first
version held the start for all 128. The channel restarted on every tick and
lost the 25-byte frame alignment (one lead byte and 24 data bytes, 48
nibbles). Jewel Master's effects then played PRG as noise. The diff showed
it at once: furb_cli now writes the sound RAM as `.snd` next to `.ram`, and
compare_furb takes PVT_DUMP. The reference's channel stopped at the end
marker at $7E48, 17 frames after $7C9F. PocketVT's ran on to $8E0A.

The cart's own program settles which behaviour is right. At $40BD it ORs
`$18A4 & ~(its copy of the last $18A4)` into $18A5, so a start is a rising
edge. vt369_snd.c now does the same, and vt369_snd_write applies a start
edge (copy the start address, set the active bit) and a stop at the moment
of the write. The game's spin-wait therefore ends at once instead of
stalling up to 9 ms until the next fill. The host test's reference model
keeps the last start mask for the same reason. After the fix, Jewel Master's
channel state matches the reference byte for byte at every frame compared
(400-600), and the two effects in frames 400-700 match it in level, band
spectrum (0.92) and envelope (0.77). The remaining waveform difference is
the reference's low-pass filter and the 8-bit output.

### 84b. The output path: timer 0, two buffers, gain

PocketNES plays the NES DMC on DirectSound B. DMA2 fills FIFO B, timer 0
sets the sample rate, and timer 1 counts 128 samples and then raises
timer1interrupt. That interrupt restarts DMA2 and ends in
vt_adpcm_mix_gba. While the sound CPU runs, vt_adpcm_mix_gba hands over to
vt369_snd_fill on the EWRAM stack, which writes the next 128 samples and
points DMA2's source at them. Three details in that path were wrong at
first.

The first was the DMA register. The first version wrote 0x040000D4, which
is DMA3's source register, so Table Soccer stayed silent and the sprite
palette played. DMA2's source register is 0x040000C8.

The second was a buffer race. The interrupt restarts DMA2 on entry, and the
fill then rewrote the same buffer while the DMA was already reading it.
There are now two buffers. The DMA plays the block written last time while
the fill writes the other one. Each block has a 128-byte guard that repeats
its last sample, because a late interrupt lets the DMA read past 128. The
first version had an 8-byte tail there, and the DMA ran on into other
variables and played them as full-scale clicks.

The third was a stalled clock. The idle-DMC code (pcm1 in timer1interrupt)
stops timer 0 on every interrupt, and the first version restarted it only
after rendering. Every 128-sample block was stretched by the render time,
and Table Soccer's stream played at 7,580 Hz instead of 8,008.
vt_adpcm_mix_gba now restarts the timer before anything else. Measured with
fill counts: Table Soccer runs 62.5 fills/s (8,000 Hz) and Jewel Master
13,953 Hz, against 8,008 and 13,983.

The gain is a separate matter. PocketNES plays the APU about 2.2 times
louder than Furbtendulator's mixer. The same factor holds on main, and with
this tree's HLE switched off (`-DVT369_SND_OFF`, kept as a diagnostic).
Mixed at the reference's scale, the sound CPU came out three times too quiet
next to the music. I measured this by recording Jewel Master against a copy
of the ROM whose effect volume ($CAE8 `LDA #$3F`) was patched to 0, on both
emulators, and subtracting. The gain is x3 for the four-channel programs,
which brings the effect-to-music ratio to the reference's (2.28 vs 2.27).
The 3-bit stream gets x1.5, because Table Soccer's music already peaks at
19,354 of 32,767 in the reference and x3 would clip it. These are
`VT369_SND_GAIN48` and `VT369_SND_GAIN3`, in halves.

Table Soccer's stream is checked by waveform, not by wavcmp. wavcmp's
envelope score stretches PocketVT's audio for its game speed (48 fps), but
this music is the stream, which plays in real time. It scores 0.11 there,
which means nothing. Cut into 0.5 s pieces, every piece matches the
reference waveform at 0.94-0.95 correlation. The music is a loop of about
1.67 s. A debug build counted ring laps, where the HLE reads a frame the
game had not refilled: there were 151 at start-up and none after.

### 84c. Speed: the loops are in IWRAM

With everything in cart ROM, the HLE cost Table Soccer 35K cycles a NES
frame for three channels, which took it from 52 fps to 43. ARM code in ROM
costs about 4 cycles an instruction on the 16-bit bus, and switching to
Thumb bought nothing. The four-channel carts paid 17-23K cycles a frame just
to convert 128 zero samples, because their channels are idle most of the
time. Four changes fixed it:

1. A fill in which no channel played leaves the buffer alone once it holds
   zeroes.
2. The 3-bit decoder looks up one combined table (predictor delta and next
   index, built in EWRAM at reset) and skips silent frames.
3. Its loop and the 8-bit conversion run as ARM from IWRAM.
4. The asm write path above.

The IWRAM came from apack.s. The aPLib decompressor's loop only runs while
a compressed game loads, and it now runs from ROM. That freed 368 bytes,
and the two loops fit in exactly that much, so `__bss_end__` is still
0x03007B84. (cf.s, the cheat finder's loop, was the other candidate, but it
patches its own instructions and must stay in RAM.) The four-channel loop
stays ARM in ROM, because it only runs while an effect plays.

The HLE now costs Table Soccer about 19K cycles a NES frame. The game runs
at 48 fps, against 52 on main, where the stream code did no work because
sound RAM did not exist. Jewel Master and Lucky Lawn Mower VT369 run at
their main speeds (56-60), and Fire Fighter at 44 as before.

### 84d. Correction to s.83

s.83 said Jewel Master's RAM matches the reference through frame 255 and
that a frame dropped between 256 and 259 because PocketVT runs the CPU at
1x. The first half is right, the explanation is not. On main, the game's
frame counter $3A stops at $FD from frame 258 on, for good. The main loop is
waiting for the sound CPU, which did not exist. With the HLE, $3A keeps
counting, and 2K of RAM stays within 6-42 bytes of the reference through
frame 700 (6 at frames 300 and 400), with $3A 1-4 frames behind. That slow lag may still be the missing CPU
x3, which remains open. The picture comparison moved accordingly: Jewel
Master is at 99.86% and 99.92% at frames 400 and 700, up from 96.9% and
94.8%.

### 84e. Regression and what is not done

All fifteen test ROMs were compared at frames 150, 400 and 700. Every
VT03/VT09 control scores exactly as on main, as do Lucky Lawn Mower VT369
and Table Soccer VT369. Fire Fighter differs by one pixel of exact5 at
frame 700. In the frame-set test, Lonely Island, Scramble and Lucky Lawn
Mower VT09 are byte-identical, and Star Ally and VG Pocket are subsets of
main. Table Soccer VT03 renders one frame main does not: frame 71, a boot
frame caught in the middle of a tile upload, which is garbage on both
cores. That is re-timing from the shifted ROM layout. Its settled frames
all match.

Still missing are the sound CPU's other programs ($0203, $02A0, $02E0 and
$0250 are implemented from the reference but no test cart uses them), the
per-channel rate divider the real $40AE program has ($188A+ch; the
reference ignores it too), a low-level sound CPU, and CPU x3.

## 85. The VT369 start-up stall, and Jewel Master VT03

### 85a. VT369 carts stalled for seconds at start-up

The user saw the VT369 games "glitch up at the start, with a delay". The
speed probe had shown it all along. Jewel Master completed 3 NES frames in its
first second and then almost none for about five more; Fire Fighter stalled
for about two. During the stall the ARM sat in vt369_alloc (ppu_vt369.c,
armprobe), not in the 6502 core. At boot the enhanced background briefly
holds more distinct tiles than there are VRAM slots. Every cell that found
no free slot scanned all ~830 slots, and the failure forced a full rebuild
of both screens on the next frame, which failed the same way: about a second
of GBA time per NES frame. The half-updated screen during those frames is
the "glitch".

vt369_alloc now remembers that nothing is free. It fails at once until a
slot's count drops to 0 (vt369_free_gen) or more slots open up (the high 8bpp
slots after vt_prg_evict). The retry sweep waits for the same event. Jewel
Master now runs at 52 fps in its second second, and Fire Fighter at 42. Boot
costs three full sweeps and about 1,200 tile copies, spread over about 20
GBA frames. The compare_furb boot frames (NES 3-40) match the reference
exactly as before. Every cart, VT03 included, spends its first ~45 GBA frames
in PocketVT's own start-up (bytecopy and friends in cycprof), before the
game's first frame.

### 85b. Jewel Master VT03: blank sprites, then stomped slots

Jewel Master VT03 (mapper 256 submapper 15, Jungletac opcode encryption,
which PocketVT already had) runs $2010 = $16: BK16EN, SP16EN and BKEXTEN,
without SPEXTEN or PIX16EN. No earlier test cart combines 16-colour sprites
with BKEXTEN. Star Ally has BKEXTEN, but its sprites are 2bpp. On the title
the high-score digits were missing and in gameplay the playfield showed
garbage (struct 86%).

The first fault was the sprites. 16-colour sprites without the extended
sprite path are copied into their OBJ slots from vt_chr4_buf
(vt_obj4_overlay). Only the non-BKEXTEN branch of vt_chr4_rebuild_if_dirty
ever filled that buffer; the BKEXTEN branch returns early. So under BKEXTEN
the buffer stayed zero and every sprite tile was blank. The BKEXTEN branch
now runs vt_chr4_assemble when the buffer is dirty, SP16EN is set and the
extended sprite path is off. It fills the buffer only, since BKEXTEN owns BG
VRAM, and runs after IME is restored because it costs about 80K cycles.

The second fault was the title's background: a band of wrong tiles under
"JEWEL", and wrong tiles by the "M", over "HI SCORE" and around the left moai.
A -DFORCE_BK_REPAIR build drew every tile right, so by s.69b this was a stomp.
The slots, the map and the nametable were all correct at frame 150: a new
-DBK_STOMP_PROBE build compares each slot with a fresh assembly every frame,
and a cell check against the dumped map showed 0 bad cells. But slot tiles
100-124 and 533-573 differed from the probe build's. ftwatch found the
writer: render_recent_tiles (ppu.s, render_tiles_2 in cart.s), PocketNES's
CHR-RAM path, which converts tiles the game writes through $2007. Jewel
Master writes to pattern space once at frame 20, and the conversion landed
on two BKEXTEN slots. The one-word signature (s.69b) did not notice, because
the stomp missed that word. The probe build caught the same stomps, but its
different timing let the signature see them.

Each slot now also keeps a checksum of its whole 2 KB page, taken when it is
filled (vt_bk_page_sum). vt_bk_frame_check re-verifies one slot a frame
against it, round robin, so any stomp is repaired within VT_BK_SLOTS (10)
frames at about 1.5K cycles a frame. The title went from 93.0% to 99.88%.
Gameplay went from 86% to 98.4-98.9%, and what is left there is the jewels:
the game generates them at random, so the reference's differ.

Colours are a separate matter. exact5 is 4% on the title and 22% in
gameplay, because the default VT03 table (vt_compat_rgb555) was fitted to
real-hardware photos of Star Ally and Lonely Island (s.57), while
Furbtendulator uses a standard NES-style palette for VT03 (its yellow $28 is
#BDC000, ours #909000). The photos are still the reference for that table.
Whether VT03 follows Furbtendulator, as VG Pocket did in s.79b, is the
user's call.

### 85c. Regression

Star Ally is the one control that moved. Its boot runs about 4 GBA frames
later (the new fill-time checksum), so the partly built title frames at GBA
56-73 differ from main's. From frame 74 every frame is one main renders.
Measured per NES frame against the reference, the boot is as good or better:
frame 10 is 100% against main's 99.38%. Lonely Island and Scramble are
byte-identical over 800 frames. VG Pocket is a subset of main. Lucky Lawn
Mower VT09 and Table Soccer VT03 each add one boot-transition frame. Speed is
unchanged: Star Ally 55-56, Table Soccer VT03 43-44, the rest 60.

The slot checksum also fixes most of open item 3 in VG Pocket. The band of
missing background tiles "that moves with timing" (s.79b) was this same kind
of stomp. Across all 50 games at NES frames 900 and 1100:

| Game (category/game) | Before | After |
|---|---|---|
| 0/3 | 91.06% | 100% |
| 1/3 and 1/9 | 95.88% | 100% |
| 2/1 and 2/2 | 94.14% | 100% |
| 3/1, 3/2, 3/8 and 3/9 | 97.3% | 99.4% |
| 3/4 and 3/5 | 96.8% | 98.4% |
| 4/3 and 4/4 (frame 1100) | 98.2% | 98.8% |

Game 2/5 at frame 900 went from 99.68% to 99.05%, and the difference is where
the enemy planes are: gameplay timing, not drawing, and frame 1100 is
identical. (One pass also showed game 0/0 at 34.9%. It was measured while
play ROMs were being rebuilt in the same build directory, and a clean re-run
gives 100% at every frame from 700 to 1000 on both builds.) Every other game
is unchanged. The picture regression over all 16 test ROMs at frames 150,
400 and 700 is the same as PR #8 or better (Star Ally frame 400: 99.59% to
99.64%).

## 86. Zuma (VT369), and speed hacks under every opcode encryption

Zuma is mapper 256 submapper 13: VT369, with Cube Tech opcode encryption
(bits 1 and 4 of each opcode byte swapped). It sets $411C = $C0, so it asks
for the triple-speed CPU, which PocketVT still does not emulate (open item
1). It first showed a screen of garbage (struct 7%) and ran at 20-25 NES fps.

### 86a. The video DMA source low byte

Zuma builds its palette and its tile map with the fast video DMA ($4014 with
$411C bit 7). PocketVT took the source as `$4014 << 8 | ($4034 & $F0)`, and
only from NES RAM. Furbtendulator (APU_VT369::IntWrite) has two more rules.
On VT369 in enhanced mode ($201E != 0) the low byte comes from $4024, and a
$4034 write sets it only while $201D bit 0 is clear; a $412D write of 0
clears it. The source can also be any CPU address, PRG included. Zuma DMAs
its palette from $8080 and its map from $8160; with $4024 ignored the map
came from $8100 and showed palette words as tiles.

The low byte now lives in vt_dma_lo (vt_regs.c). mapVT.s intercepts $4024
and fills vt_dma_lo from $4034 & $F0 at the $4034 write, which for VT03 and
VT09 is the same value the DMA used to mask out at $4014 time, so those carts
are unchanged. On VT03 a $4024 write still goes to IO_W. vt369_dma_4014 reads
sources below $2000 from NES RAM with word copies as before, and anything
from $6000 up through the memory map a byte at a time (vt369_dma_byte).

### 86b. 16-row sprites from $2000 bit 5

Zuma's balls came out half height. In enhanced mode the NES 8x16 bit ($2000
bit 5) also makes sprites 16 rows tall, as $201D bit 2 does. The tile number
loses bit 0 and the stride is 64 bytes for a 16-wide 4bpp sprite, as on the
NES (Furbtendulator PPU_VT369). vt369_sprites and vt369_sprite_build now take
the height from either bit. The title is 99.92% and gameplay 95-99% against
the reference. The ball colours are random (the user confirmed this, as with
Jewel Master's jewels), so most of the gameplay residue is different balls,
not drawing. Some may be chain position drift from the missing CPU x3.

### 86c. The sound loop

Zuma plays 8003 Hz ADPCM through the sound CPU's vector $02A0 (s.84) on one
or two channels, which made vt369_run_48 its biggest single cost (124K
cycles per NES frame). The common case (no stream, no pending start, a
sample that does not wrap inside the fill) now runs in vt369_play48, a
pointer-based loop that keeps one row pointer and switches it at position
24; anything unusual falls back to the old loop, renamed vt369_run_48_gen.
Cost fell to 88K cycles. It runs from ROM because IWRAM has no room. The
host test (tools/probes/vt369snd_test.c) compares 18,000 fills with 0
failing, and a planted fault in play48's mode-2 predictor fails 1,775.
Against Furbtendulator's WAV over NES frames 400-700 the envelope scores
0.77 and the long-term spectrum 0.96. The level is 2.2x, the same
over-loud APU as the other carts (open item 2).

### 86d. Speed: idle loops under opcode encryption

A whole-frame profile put most of the rest in the 6502 core: JMP, CMP #,
LDA zp and untaken branches. Furbtendulator's trace shows why. Zuma's main
loop waits for its NMI in

    C4D9  A5 5C     LDA $5C
    C4DB  C9 01     CMP #$01
    C4DD  F0 07     BEQ $C4E6
    C4DF  C9 02     CMP #$02
    C4E1  F0 06     BEQ $C4E9
    C4E3  4C D9 C4  JMP $C4D9

about 5,800 times a frame. PocketNES's speed-hack finder skips exactly this
kind of loop, but it read opcode bytes straight from PRG. In Zuma's PRG the
BEQs are stored as $E2, which the finder's table rejects. set_cpu_hack had a
second limit: it knew only submapper 15's bit 5<->6 swap. It accepted only
the branch rows that swap leaves alone ($10, $70, $90, $F0), refused JMP,
and put the default BNE hack at raw $B0, which on submapper 13 is LDX #.

Both now decode. vt_rebuild_optable fills vt_op_dec (raw byte to the opcode
it runs) for the current mode, plus vt_sh_raw[n], the raw byte that runs
branch n or JMP, and vt_sh_norm[n], the handler that byte runs with no
hack. The finder classifies OP(byte) instead of the byte. set_cpu_hack takes
the hack type from the decoded opcode and patches op_table at the raw byte,
which is correct under any mode because op_table[raw] already holds the
handler for the decoded opcode. It restores a removed hack from vt_sh_norm
(so op_vt_JMP_abs, the VT09 encryption commit, survives), and puts the
default BNE hack at vt_sh_raw[6]. dobnehack decodes the opcode before the
BNE too. On submapper 15 the old code took raw $CA (TAX there) for DEX.
A JMP hack uses the new op_vt_JMP_absy, and dobranchhackjmp ends in
op_vt_JMP_abs, so the encryption commit still happens. game_specific_hack
matches raw Capcom and Konami byte patterns, so it is skipped while
encryption is on. A rebuild of op_table (a $4169 mode change) removes any
installed hack, so it now also clears _speedhack_pc and _speedhack_pc2,
and set_cpu_hack installs the hack again.

get_instruction_number and set_cpu_hack moved from .vram1 to ROM. They run
at most once a frame, and .vram1 was full: it now ends at 0x06003ED0 of
0x06004000.

Zuma now runs at 52-60 NES fps instead of 20-25, and the picture is
unchanged. Star Ally gains too, from 55-56 to 60. Its LDA $6816 / BNE *-5
wait stores BNE as raw $B0, a row the old set_cpu_hack refused. The finder
found that loop at the same address in both builds; only the install is
new. (Session 20b3 measured a hand-seeded hack on that loop as slower, but
that one used a divider of 1 and was re-armed every frame. This is the
finder's own hack with its real cycle count.)

### 86e. Regression

Over all 17 test ROMs at NES frames 150, 400 and 700, no struct or exact5
score drops by more than 0.05 points (Fire Fighter frame 700 and Star Ally
frame 400; both are gameplay timing, and Star Ally frame 700 rises from
99.10% to 99.61%). Speed:

| Cart | Before | After |
|---|---|---|
| Zuma | 21-25 | 60 |
| Star Ally (frames 400, 700) | 42-44 | 60 |
| Table Soccer VT369 (submapper 13 too) | 48 | 60 |
| Aero, Hex City X, Table Soccer VT03 | 43-44 | 42-43 |

The one-frame drops are within the probe's noise: the GBA speed probe gives
43-44 for Table Soccer VT03 on both builds.

In the frame-set test over 800 GBA frames, Lonely Island is byte-identical,
and Star Ally, Scramble and VG Pocket are subsets of main. Five carts add
frames, all in the boot transition: LLM VT09 one (GBA frame 72), Jewel
Master VT03 one (83), Aero four (63-69), Hex City X four (59-65) and Table
Soccer VT03 seven (56-72). Aero spends GBA frames 2 to ~70 inside NES frame
2 while it builds its title, so the screen shows whatever that frame has
drawn so far. Main shows a half-built title there, and this build shows a
half-built screen of other tiles. The NES frames themselves match the
reference 100% on both builds. Aero and Hex City X are unencrypted, and
neither build installs a speed hack on them, so their shifts come from code
moving (set_cpu_hack to ROM, the new tables), not from a hack.

VT369 sound: the host test passes (0 failing). The Docker build links
(`__bss_end__` 0x03007B64, `.vram1` to 0x06003ED0), and its Zuma, Star Ally,
Lonely Island and Jewel Master VT369 play ROMs boot through the real BIOS
and reach 60 NES fps.

VG Pocket, all 50 games at NES frames 900 and 1100: mean struct 99.46%
against main's 99.48% (main's bad 0/0 pass from s.85c left out; 0/0 is 100%
here). The one real move is the racing game at 1/1, 1/2, 1/7 and 1/8, from
99.58% to 99.40%. The black line under its HUD split sits one row off the
reference. On main it was one row early (96 pixels of the reference's green
missing), and here it is one row late (144 pixels of green where the
reference is black). That is the ±1-row split residue, and it follows timing:
the core before s.85 also scored 99.40%. The other changes (0/5-0/6 +0.03,
2/0 frame 900 -0.15, 4/3-4/4 frame 1100 -0.05) are gameplay positions.

## 87. Star Ally and Zuma "sorta buggy", Fire Fighter below full speed

The user tested the PR #10 ROMs and reported Star Ally and Zuma as "sorta
buggy" and Fire Fighter VT369 as not running at full speed. Nothing more
specific came with the report, so each game was measured against the
reference over long gameplay runs (NES frames 100-2000 with Start and
periodic A presses and moves), with three cores side by side: main before
PR #10, the PR #10 core, and a PR #10 core built with `-DVT_NO_SPEEDHACK`
(a new diagnostic switch in speedhack_manager that never installs a hack).

### 87a. Fire Fighter: the vblank wait, not the emulation

Fire Fighter ran at 44 NES fps. Its profile showed 25% of all GBA time in
the BIOS halt at 0x1F0, and a `-DVT_DIAG_NOVSYNC` build (the frame end never
waits) ran it at 66-69 fps. The emulator was fast enough; the pacing lost
the time. Fire Fighter's NES frames cost very different amounts: without the
wait, the NES frames finished per GBA frame went 2 0 3 0 1 2 0 2. The frame
end waited with IntrWait(0, VBlank), which remembers only one pending
vblank, so after a frame that ran past two vblanks each of the cheap frames
behind it still waited a whole GBA frame: 1 1 1 0, 45 fps.

The frame end now paces with credits (vt_vsync_ahead, vt_regs.c). Every
top-level GBA vblank adds one (vt_gba_vbl, counted in vt_timer_tick_frame),
every NES frame spends one, and the frame end waits only while it has none.
A backlog above 3 (after the menu, or a stall) is dropped instead of raced
through, and two waits without a new credit fall back to the old behaviour,
in case the vblank IRQ ever stops counting. NoVSync, slow motion and 50 Hz
keep the old wait. Fire Fighter now runs 58-61 (60 on average). The catch-up
shows as 59/61 pairs in the speed probe; the average never exceeds 60.

### 87b. Zuma: register writes from cart ROM

The one thing measurably wrong with Zuma was gameplay speed: 60 fps on the
title, then 44-51 once the chain filled the screen. The picture scored the
same with and without the speed hack at every frame, and a frame that looked
broken (frame 677: green specks at the top, balls missing) turned out to be
PocketVT's own game popping three balls, which the reference's game (random
colours, s.86b) had not done. A snapshot of the VT369 sprite list at NES line
242, tried against that frame, changed nothing and was reverted.

Zuma's gameplay cost about 350K cycles per NES frame, and 91K of it was
register access: per frame it multiplies 29 times on the VT32/VT369
multiplier (116 writes to $4130/$4131/$4134/$4135, then reads of $4136,
$4138 and $4139) and writes $4107/$4108 58 times. Each write went through
write_vt4xxx and vt_reg_write in C, about 250 cycles from cart ROM. Two
changes:

vt_recompute_prg_banks now works out the four windows inline and marks only
the windows that changed in vt_prg_dirty (bit 4 is the VT369 $6000 window).
vt_apply_prg_dirty (mapVT.s) re-maps just those; vt_apply_prg_banks keeps its
all-windows meaning for init and eviction, and vt_reset invalidates the
shadow so the first recompute marks everything.

write_vt4xxx_v, in .vram1, is now writemem_4 on every VT cart but mapper
419 and passes anything it does not handle to the ROM handler in
vt_w4_next. It returns at once for a $4107/$4108 write of the value already
there (Fire Fighter and Zuma rewrite them constantly; not on submapper 2,
which swaps the two) and does the multiplier writes itself, exactly as
vt_reg_write's ALU case does (the divide, $4136/$4137, stays in C). Both are
enabled by vt_w41_fast once the first $41xx write has armed the VT timer.
A version of the same code in ROM still cost about 300 cycles a write; in
VRAM it is about 110. Zuma gameplay is now about 281K cycles per NES frame
and runs 60 fps to frame 2000 in the long run. `.vram1` ends at 0x06003FF0.

### 87c. Star Ally: the vblank handler ran into the next frame

In the PR #10 core Star Ally drew about a hundred consecutive frames (NES
640-740) with the picture shifted down, a grey line across the screen and
the score bar cut off. The same two kinds of damage appear on main and on
the no-hack core, just scattered: frame by frame over NES 700-1000, main
showed 23 frames with a grey line and 15 isolated shifted frames.

The VT timer was the first suspect, since Star Ally's score bar is a raster
split. A `-DVT_TIMER_LOG` build (a ring of expiries and re-arms in EWRAM)
showed the timer firing at lines 199.9 and 237.9 and re-arming in vblank in
every frame, glitches included, so the timer was not it. It did turn up a
real difference from the reference: the IRQ handler at line 199 writes a
new reload ($4101 = $25) and the hardware uses it for the count already
running (it reloads on the clock after the zero, h_OneBus.cpp), so the
reference fires again at 237. PocketVT computed the next expiry when the
timer fired, with the old value, and carried 200 lines into the next frame.
vt_timer_reload_check (sound.s) now re-schedules from the last expiry when
$4101 is written within a line of it; the line-237 IRQ fires as in the
reference. "Within a line" needs the exact time of the write: the first
version compared against `timestamp`, which is the time of the last timeout
event and can be many lines old, and VG Pocket game 2/0 (a vertical shooter
with a score-bar split) lost its whole playfield (99.2% to 36.9%). The write
path in mapVT.s now stores the exact time (timestamp + cycles_to_run -
cycles, as get_scanline_2 computes it; r8 is only valid there, not inside
the C call) in vt_w41_now, and the check uses that.

A diagnostic that logged VCOUNT when run_palette starts found the cause.
run_palette normally runs at GBA line 194-217, so the vblank handler already
spends 35-57 of vblank's 68 lines before it; in every grey-line frame it ran
at line 14-35 of the NEXT frame, and the grey line sat exactly on that row.
The handler writes the legacy 4-entry palette (run_palette) and a moment
later the 16-colour one (vt_16c_palette_fixup), and the line the GBA drew in
between showed NES colour $00. The shifted frames are the same overrun one
step earlier: the handler stops HBlank DMA on entry and started it again
only after vt_chr4_rebuild_stacked (where Star Ally's BKEXTEN cell writes,
sweep and slot checks run), so a late restart left the top band with the
previous frame's last scroll (the score bar's) and applied the scroll table
from the wrong line down.

The BKEXTEN batch (vt_chr_sync_flush, the whole-map stepper, the bank
recheck, the slot checksums and the map sweep: Star Ally's heavy part) now
runs after the HBlank DMA set-up. vt_chr4_rebuild_if_dirty only marks it
pending (vt_bk_late_pending) and vt_bk_late does it, called on the EWRAM
stack from the top of vrom_update_tiles (ppu.s), the first call after the
set-up that lives in ROM: a call in the IWRAM handler itself would have
left `__bss_end__` exactly at the 0x03007B84 limit. Moving the whole of
vt_chr4_rebuild_stacked there was tried first and broke Aero Gyrodine's
title (a block of wrong tiles in its bottom band, 100% to 99.79% at every
title frame): for the raster-split carts something in the non-BKEXTEN
rebuild has to happen before the per-line tables are copied, so that path
keeps its place and only BKEXTEN work moved.

run_palette returns at once while VT's own builder writes every palette
entry the picture uses (vt_pal_owned: BK16EN, or VT369 enhanced), and
vt_16c_palette_fixup keeps the VCOUNT IRQ off while it does.

Frame by frame over NES 700-1000 (Start, A and moves) Star Ally now shows no
grey-line frames and no shifted frames, against 23 and 15 on main; over NES
1000-1300 with a different input pattern, none against 20 and 9. A build with
the timer log compiled in, whose different timing had moved glitches around
before, also shows none. A build that only skipped run_palette removed the
lines but still shifted 9 frames, which is what separated the two causes.
Why PR #10 showed them for a hundred frames in a row: at a locked 60 fps the
phase between the NES and GBA frames stays put, so an overrun that happens
once happens every frame until the game's load changes.

The handler is still long in Star Ally (vt_bk_write_cell alone averages
19.6K cycles per NES frame); what overruns now is work whose late arrival
does not show. VT369 enhanced carts do their picture uploads in
vt369_vblank at the end of the same handler, so an overrun there would tear
too; Zuma's frame-by-frame run showed none.

### 87d. Regression

All 17 test ROMs at NES frames 150, 400 and 700 score within 0.11 points of
PR #10. The largest move is Push the Ball frame 150, 97.49% to 97.38%, its
timing-exposed stale tile (open item 3); Aero Gyrodine's title is 100% at
every frame from 100 to 300. Speed: Fire Fighter 44-51 to 60, Jewel Master
and LLM VT369 56-58 to 60, Scramble 57-59 to 60, Table Soccer VT03 43 to 44,
Zuma 60 to NES frame 2000 in gameplay; Star Ally still falls to about 47 in
its heaviest stretch (NES ~1400, main 35-40), which is compute-bound. VG
Pocket, all 50 games at NES frames 900 and 1100: mean 99.49% against PR
#10's 99.47%, lowest game 97.74% (was 97.58%); the per-game moves are
gameplay timing in both directions (0/1 -0.25 at frame 900, +0.41 at 1100).

In the frame-set test (800 GBA frames, no input) new frames appear in the
boot transitions (GBA 50-85) and in carts whose speed changed: Zuma and Fire
Fighter now show different in-between frames at 60 fps, Scramble's title
fades a few GBA frames earlier, LLM VT09 is one animation step apart at GBA
800. The VT369 sound host test passes (0 failing). The Docker build links
with `__bss_end__` 0x03007B64 and `.vram1` to 0x06003FF0; the lowest user
stack pointer is 0x03007C88 (Zuma) and 0x03007CA8 (Star Ally), still above
.bss. Docker-built Star Ally, Zuma, Fire Fighter and VG Pocket play ROMs
boot through the real BIOS and run at 60.

## 88. Jumper and Sky Fighter (VT369), and CPU x3

The user supplied two more VT369 carts, both mapper 256 submapper 13 (Cube
Tech opcode encryption): Jumper, a Super Mario Bros. hack, which stayed on a
black screen, and Sky Fighter, a vertical shooter whose select screen matched
but whose enemies were garbled striped blocks, with a band of wrong tiles near
the top of the playfield. Both turned out to need the same missing piece, the
VT369 CPU at three times the NES clock, and several VT369 features no earlier
cart had used.

A new probe did most of the finding. tools/probes/nestrace single-steps the
GBA and logs the 6502 PC and registers at every opcode-handler entry, in a
form that lines up with `furb_cli --trace`; a small aligner (tolerant of the
speed hack, which collapses loops) then finds the first instruction where the
two CPUs part. `FT=`, `FROMFT=` and `KEYS=` start it at a NES frame and press
keys. bpcount gained `VERBOSE=1`, which prints r0-r2 and lr at every hit.

### 88a. Jumper's black screen: three missing pieces in a row

The first divergence was a wait on the sound CPU's timer-IRQ counter, $18F6
(seen by the main CPU at $48F6). The HLE advanced it by one every sample
tick, 128 a fill; the reference's real sound CPU advances it about 5.16 times
a NES frame for program $02A0, once per 25.78 ticks. Jumper waits for it to
change and copes with any step, so this was not the hang, but Zuma's music
now lines up better (wavcmp envelope 0.67 to 0.76, lag -260 ms to -40 ms) and
Zuma's gameplay picture rose from 96-97% to 98%. The rate is a 12-bit
fraction (VT369_IRQ_PER_TICK, 159/4096 per tick); the host test's model
counts the same way and passes with 0 failing trials.

The second was a $2007 read of pattern memory. Jumper copies its VRAM update
lists out of CHR at $1EC0 through $2007, and PocketVT returned zeros: in VT369
enhanced mode the NES_VRAM CHR copy is not maintained. The reference reads
CHRPointer, which h_OneBus.cpp setCHR(0x00, ...) points at the planes-0-1 half
of the 4bpp data (or chrLow16) with the extended bank formula when BKEXTEN or
SPEXTEN is set. vt_chr_read (ppu_vt.c) computes the same byte, and
vt369_ppu_R (mapVT.s, ROM, readmem_2 on VT369) calls it for a screen-off $2007
read below $2000, after vmdata_R has done the increment and buffer swap. IWRAM
is untouched.

The third was the sprite-0 hit. SMB's NMI waits for it before its status-bar
split, and PocketNES's test reads sprite and background patterns from
NES_VRAM, which enhanced mode leaves empty; worse, the fast $4014 DMA sets
PocketNES's OAM copy to Y = $FF. OneBus_VT369.cpp sets the hit on the first
opaque pixel of sprite 0 whatever the background has there, so
vt369_spr0_hit (ppu_vt369.c) finds that pixel from VT369 sprite memory in
either OAM layout, and screen_on_sprite0 (ppu.s, ROM) installs
sprite_zero_handler_3 at its time. The answer is cached while sprite 0 and
the registers stay the same (a scan from ROM cost ~20K cycles), and a row of
all-transparent bytes is skipped whole.

### 88b. VT369 sprites: the OAM half, the NES layout, 8bpp

With 64 sprites ($201D bit 0 clear) the reference's $2004 keeps the high OAM
half at 0; vt369_dma_4014 flipped halves on every 256-byte transfer, so every
other frame's sprites went where nothing read them. The enhanced sprite path
only read the new planar layout ($201E bit 2); Jumper uses the NES layout,
y/tile/attr/x per sprite, which is now read once a fast DMA has filled
vt369_oam (vt369_oam_live). Jumper's sprites are also 8bpp, 8 wide ($201C bit
5; $201D bit 2 selects 8bpp 16-row tiles too): the source rows are already
the GBA's 8bpp layout, so a slot is a straight copy, the GBA entry sets the
256-colour bit, and the OBJ palette upload grows to all 256 sprite colours
(vt369_gba_pal and vt369_pal_seen now cover 512 colours). The 16-row 8bpp
form is untested; no cart uses it.

### 88c. Sky Fighter: the "Cross River" tile rule, and tiles shared by content

Sky Fighter's broken sprites were every sprite from tile 256 up.
ProcessSpritesEnhanced has a rule commented "Needed for Cross River": with
$201E = $0F, $201D = $0B and BKEXTEN, tile bits 8-10 move down to 6-8. Sky
Fighter meets that condition; vt369_tile_fix applies it in the sprite path
and the sprite-0 search.

The band of wrong tiles was the BG slot pool running out. Sky Fighter's
forest is an 8bpp picture in which all 960 cells of the nametable have
different tile numbers; about 896 are on screen at once in the default scaled
mode, and the pool holds at most 831 8bpp slots (BG VRAM minus the tilemap,
.vram1 and the debug screen). The cells the full rebuild reached last, the
bottom rows of the nametable, found no slot and kept stale tiles, which the
0.75 vertical scaling smeared into a band. Only 603 of the 960 tiles are
different, though: vt369_canon_tile maps each tile number to the first tile
with the same pattern bytes (a hash table, reset with the cache when the BG
base or depth changes), and the cell and slot code works in those. Sky
Fighter's gameplay picture went from 83-86% to 96-98%; the rest is enemy
waves drifting from the reference, which the input harness starts a frame
apart.

### 88d. CPU x3

With the picture fixed, Jumper still died strangely: after its first death
it never left a blue screen. The trace showed why. The game turns opcode
encryption on ($4169) for a short OAM copy in its main loop, and at PocketVT's
speed the NMI arrived inside that copy and ran the NMI handler through the
opcode swap, into $2D30. The reference's trace advances one PPU dot per CPU
cycle: Jumper writes $411C = $C0 at boot, and bit 7 runs the VT369 CPU at
three times the NES rate (OneBus_VT369.cpp RunCycle: the PPU runs one dot a
cycle, the APU every third). Every VT369 test cart sets it. Before, the
games' logic simply lagged: Fire Fighter's per-frame counter $3A advanced 42
times in 60 NES frames on PR #11 against 60 on the reference, so PR #11's "60
fps" was a game running at 70% of its speed.

PocketNES's timeline is in PPU dots, and every opcode handler charges its
cycles as an immediate, `subs cycles,cycles,#n*3*CYCLE` (6502mac.h fetch,
fetch_branch, fetch_c, the page-cross penalties and 21 sites in 6502.s).
Those are now written through a `cyc` macro that records the instruction's
address in section vt_cycpatch (GNU ld provides __start_/__stop_), and
vt_cpu_x3_set (vt_regs.c), called on a $411C write, rewrites every recorded
immediate that lives in RAM between 3n and n dots. 227 sites are recorded;
the table is in ROM. The timeline, the APU, the VT timer and the sound HLE
keep counting dots, so they keep their speed, as in the reference. The NMI
entry (timeout.s) and the OAM DMA cost (ppu.s) read vt_cpu_x3; cart reset
switches it off (cart.s). What x3 does not reach: handlers in ROM, which are
the VT extra opcodes and the unofficial ones, still charge 3 dots a cycle,
and the speed hacks' per-iteration costs for loops found before a switch.
A first version switched x3 off again on the $4112 write, through a missing
case label; watch for fall-through in that switch.

With x3 Jumper's frame counter stays within one frame of the reference's
through 1200 NES frames and its death and restart play as in the reference.
Hidden costs showed up at 3x: loops spin three times as often. vt369_stat_R
(mapVT.s) ends the time slice when a $2002 read is followed by AND #m and a
BNE/BEQ back to it that will be taken (Jumper waits out vblank that way, ~570
iterations a frame), and the speed-hack finder gained find_poll_loop, which
follows a poll loop along the path the current RAM values take (LDA zp/abs,
AND/CMP #, branches, JMP; no writes, RAM only) and puts the hack on its
backward branch. Sky Fighter's title waits in four chained LDA $30 / AND #m /
BEQ tests the old finder could not follow: 13 NES fps before, 60 after.

### 88e. Encryption toggles and the speed hack

Jumper writes $4169 four times a NES frame (encryption on and off twice).
Each write rebuilt the whole op table (256 decodes), rebuilt the speed-hack
byte tables and dropped the installed idle-loop hack. The encrypted table,
its decode and the hack bytes are now built once per encryption mode and
copied in (vt_optable_load, ldm/stm), and a toggle re-installs the current
hack at its raw byte if that byte still decodes to a branch or JMP
(vt_optable_toggle). A DMA3 copy was tried first and stalled the game; not
investigated.

vt_timer_tick_frame re-armed speed hack 1 on every GBA vblank for every VT
cart. That was for Lonely Island's hand-installed hack; for the finder's
entries it fired at a random point of the NES frame, and in Jumper slot 1 is
the sprite-0 wait, so the main loop's idle JMP ran unhacked until the next
hit (~90K cycles a frame). It is now limited to Lonely Island's entry.
Table Soccer VT03 went from 44 to 60 NES fps in the same build; the re-arm is
the only change on its path, so it is the likely cause, not proven.

### 88f. Regression

Test ROMs at NES 150, 400 and 700 against PR #11: identical scores except
Zuma (98.9/96.2/96.8% to 99.6/98.3/98.0%), Table Soccer VT369 (98.68% to
98.59%), Push the Ball frame 150 (97.38% to 97.24%, its timing-exposed stale
tile) and Fire Fighter within 0.15 points. Speed: Table Soccer VT03 44 to 60,
Fire Fighter 60 to 53-57 (its logic now runs every frame), Jewel Master VT369
57-60, the others unchanged. Jumper 99.9% at the regression frames and
98.4-100% in a run to NES 1200 with input, Sky Fighter
title 100% and gameplay 96-98%. VG Pocket, all 50 games at NES 900 and 1100:
mean 99.46% against 99.49%; the largest move, game 0/4 at frame 900 (100% to
97.0%), is a one-frame palette-raster glitch that the harness catches or not
from run to run: over 100 frames sampled every third frame both cores score
identically, frame for frame.

The frame-set test (800 GBA frames, no input): Star Ally identical frame for
frame; Lonely Island, LLM VT09, Aero Gyrodine and Table Soccer VT03 show new
frames only in the boot transition (GBA frames 56-75); Scramble and VG Pocket
none. The sound host test reports 0 failing trials. The Docker build links
with `__bss_end__` 0x03007B64 and `.vram1` to 0x06003FF0; Docker-built
Jumper, Sky Fighter, Zuma, Fire Fighter and Star Ally boot through the real
BIOS. The lowest user stack pointer is 0x03007C14, in libgcc's 64-bit divide
under the speed-hack finder; PR #11 reaches 0x03007C1C on the same run.

## 89. VT369 slowdown and flicker

The user reported the VT369 carts as running but "buggy as in some
slowdown, flickering". Measured with the gameplay input used below (Start at
NES frame 320, A at 400, Start at 500, A held from 600), PR #12 ran Fire
Fighter at 38-46 NES fps, Sky Fighter's gameplay at 50-51 and Jewel Master at
57, and one missing-sprite problem turned out to be the GBA's sprite line
budget. Every fix below came from a profile (tools/probes/cycprof with
WARMFT and KEYS) or a probe; the speeds are compare_furb's NES frames per 60
GBA frames.

### 89a. Sprites: the line budget and reference-counted slots

Fire Fighter parks about 70 of its 128 enhanced sprites at (0,0) with blank
tile 0. PocketVT drew them as affine double-size objects, and on GBA lines
0-9 they cost 5254 cycles of the 1210-cycle OBJ line budget (H-blank free
off), so the hardware dropped whatever came after them in OAM: the HUD digits
and the fires in the windows. A slot whose converted tile is all zero is now
hidden (vt369_oblank), and a normal-size affine box replaces the double-size
one (2w+10 cycles a line instead of 4w+10); Lucky Lawn Mower VT369 moves from
99.66% to 99.59% on the mower row from the box's different sampling, which
was accepted. The same per-line cost sum over OAM dumps (PVT_DUMP of
0x07000000) of Sky Fighter and Jumper found no line over budget; the flicker option in the menu already
defaults to off (cart.s _flicker).

vt369_sprites stamped every sprite every frame to age its tile slots, which
cost Fire Fighter ~25K cycles a frame. Slots are now reference-counted
(vt369_orc, released by vt369_orel), a group of four sprites whose OAM words
did not change is skipped (vt369_oshadow, compared with XOR/OR), the sprite-0
hit answer is cached by its key (vt369_s0_key), and the entries are four
halfwords so the OAM copy is one word and one halfword a sprite. Together:
34K to 14.5K cycles a NES frame in Fire Fighter's heaviest stretch.

### 89b. Palette and nametables: marks instead of compares

The palette went to palette RAM in full every frame (13.7K cycles in Fire
Fighter and Jewel Master); it became incremental, and then the 256-word
compare that found the changes was itself the cost (Thumb from ROM with EWRAM
loads). The writers now mark the changed words in vt369_pal_dw: the DMA fast
path in vt369_dma_4014 compares four words at a time as it copies, and
vt369_pal_W ($2007) marks its word. vt369_pal_upload converts only marked
words.

vt369_nt_diff compared both 2K nametables every vblank (~12K cycles). The
nametable writer is writeBG in IWRAM (ppu.s), which has a spare self-modify
slot, writeBG_mapper_9_mod, used only by mappers 9 and 10. While enhanced
mode is on, vt369_nt_hook patches that slot into a B to vt369_nt_mark, a
six-instruction routine in the `.ewram` section (a B from IWRAM reaches EWRAM;
ROM is out of range and `.vram1` has no room). It marks the word in
vt369_nt_dw and the flag vt369_nt_any, and returns without logging the write
in the BG cache ring, which nothing reads in enhanced mode. Leaving enhanced
mode puts the old instruction back; the vblank re-installs the hook if the
mapper-9 set-up in ppu.s overwrote it. A write that bypasses writeBG is still
caught by a scrub that compares one eighth of the map each frame. With
nothing marked the diff costs the scrub only.

The marker costs ~50 cycles a byte from EWRAM, and Sky Fighter's title
video-DMAs about 1.5K nametable bytes a frame through $4014 -> vmdata_W ->
writeBG: 76K cycles, and its title fell from 60 to 46 fps. vt369_dma_nt now
takes a nametable DMA whose quadrants map to NES_VRAM2 (vram_write_tbl entry
VRAM_name0 or VRAM_name1): it stores straight into the nametable, marks only
bytes that change, and advances vramaddr as vmdata_W's strh does. Anything
else (the four-screen quadrants, a source in $2000-$7FFF, a range that reaches
$3C00) returns 0 and takes the old per-byte path.

### 89c. $4107/$4108 and the divider in vt_w4_bank

Fire Fighter writes $4107/$4108 about 60 times a NES frame; through
vt_reg_write, vt_recompute_prg_banks and vt_apply_prg_dirty that cost ~25K
cycles. vt_w4_bank (mapVT.s, ROM, behind write_vt4xxx_v) maps the one window
directly from the mask, OR and window order that vt_recompute_prg_banks
caches in vt_q. It must preserve r3 (m6502_nz): writemem handlers may clobber
only r0-r2 and addy, and the push/pop of r3 was added on the C paths of
write_vt4xxx and write_vt_rom too. `-DVT_NO_W4_BANK` turns it off.

Jewel Master VT369 ran at 57 although the CPU idled 44% of the time: one NES
frame in about twenty took four GBA frames, and the pacer drops a backlog of
more than three (s.87a). That frame divides 222 times through $4136/$4137,
each through write_vt4xxx, vt_reg_write and two libgcc divisions. vt_w4_bank
now stores $4136 and, on $4137, divides with the BIOS Div SWI (signed, so a
dividend with bit 31 set still goes to C). Checked on 207 divisions of that
frame against Python's; Jewel Master is 60 at every checkpoint, through the
real BIOS as well.

### 89d. The poll-loop finder and where the vblank lands

After vt_w4_bank went in, Sky Fighter's title fell to 14 fps. Nothing was
wrong with the bank switch: find_poll_loop (s.88e) starts at the pc the
vblank interrupted, and only succeeded when that pc was the LDA of a test.
Landing on the AND or the branch, where A and the flags are unknown, it gave
up, and the new timing always landed there. It now backs up to the load that
feeds the pc (a straight run of LDA/AND/CMP ending at it) and closes the loop
there. The lesson: a speed that depends on the finder can change with any
change of timing; after a core change, check the titles as well as gameplay.

### 89e. Dead end: vt369_ram_R inline

vt369_ram_R (readmem_0 with the misc ROM) jumps to ram_R_mask for $0000-$0FFF;
inlining its `bic addy,#0x1f800` looked like a free 6K cycles. It is not the
same: loadcart.c patches ram_R_mask to 0xFFF on carts with 4K of RAM, and
Fire Fighter keeps a VRAM update queue at $0C00. The inline copy read the
mirror at $0400, the queue never emptied and the game ran at 8 fps from boot.
Reverted; the comment there now says why.

### 89f. Regression

Gameplay input, PR #12 to now: Fire Fighter 38-46 to 54-60 (picture 96.4-96.6%
to 96.7-96.9%), Sky Fighter gameplay 50-51 to 60, Jewel Master 57 to 60; Jumper,
Zuma, Lucky Lawn Mower VT369 and Table Soccer VT369 stay at 60 with the same
scores within 0.2 points. The test ROMs at NES 150, 400 and 700 with
Start only, against PR #12: Fire Fighter 49-55 to 60, Lucky Lawn Mower VT369
99.66/99.61/99.67 to 99.59/99.61/99.67 (89a), Push the Ball 150 97.24 to
97.49, Aero Gyrodine and Hex City X titles 42/43 to 41/42 (present since the
first build of this round; not chased), the rest identical. The frame-set
test (800 GBA frames) shows new frames only in the boot transition (GBA
frames 55-75) on Star Ally, Lonely Island, Scramble, VG Pocket, Aero Gyrodine
and Table Soccer VT03, none on LLM VT09. VG Pocket games 0/4, 1/3, 2/2, 3/4 and
4/1 at NES 900 and 1100 match PR #12 within 0.02 points (0/4 at 900 is the
one-frame raster glitch of s.88f). The sound host test reports 0 failing
trials. The Docker build links with `__bss_end__` 0x03007B64 and `.vram1` to
0x06003FF0 (`.ewram` grows by 40 bytes); its core boots Fire Fighter, Sky
Fighter, Jewel Master, Zuma, Jumper and Star Ally through the real BIOS with
the build_pvt.sh pictures, at 60 except Fire Fighter 54 and Sky Fighter 58
at NES 800 (a different GCC; the build_pvt.sh core gives 59 and 60 there). The
lowest user stack pointer is 0x03007C14, as in s.88f.

tools/probes: `peek` and `bpcount` take `FT=<frametotal>` and
`KEYS="first-last:mask,..."` like nestrace; bpcount also `FROMFT=<n>` and
`PEEK=<addr>` (a word printed at every hit).

## 90. Sky Fighter's black line: the sound refill and the vblank DMA set-up

The user saw black lines flicker during Sky Fighter's gameplay. A frame-by-
frame capture (tools/probes/fgrab, every GBA frame from 400 to 1000 with the
gameplay input) of the shipped core found a one-frame black row about every
22 frames, stepping down the screen (row 29, 41, 46, 54, ... 155), in time
with the background scroll. The row is the nametable seam: in the per-line
scroll table the vertical offset jumps by 16 there (0xBA to 0xCA) to skip the
two unused rows of the 32-row GBA map. The table was right; it was started a
few lines late, so the pre-seam offsets landed on the lines after the seam,
and those lines showed the empty rows. The build_pvt.sh cores of PR #12 and
s.89 showed none over the same 600 frames; the Docker core, which the release
uses, did.

Probing VCOUNT at the vblank handler's entry and at its DMA set-up (vbl5)
found the cause. The handler normally sets the DMA up at line ~170. In about
one frame in ten its entry was held back to line 180-212, and in 24 of 461
frames on the Docker core the set-up slipped past line 227 into the next
picture. Both come from the VT369 sound refill. timer1interrupt masks the
vblank interrupt while it runs, and with the sound HLE on it renders 128
samples, ~55 scanlines of work, every ~218 lines. Its phase drifts about ten
lines a frame against the picture. A refill running at the start of vblank
held the handler back, and one arriving just after the handler re-enabled
IME (before its DMA set-up) ran inside it. On the build_pvt.sh core both
cases still finished before line 227, narrowly; on the slower Docker core
they did not.

The render now waits when it comes during a vblank handler: vt_adpcm_mix_gba
still restarts the sample timer at once, but if `_inside_gba_vblank` is set
(ppu.s, now exported) it sets vt369_fill_pending and returns, and
vt_16c_palette_fixup, the last call of a top-level vblank handler on VT
carts, runs the fill after the DMA set-up and vt369_vblank. The DMA is already
playing the previous block, and the next restart is ~218 lines away. A render
that starts outside a vblank handler re-enables the vblank bit of IE first,
so the vblank interrupt nests in it rather than waiting. If a pending fill is
ever left (a vblank handler that skips the fixup, as before the first frame),
the next refill renders as usual and the DMA replays one old block.

Results on the Docker core: all 461 vblanks of the Sky Fighter window set the
DMA up at lines 170-179, none late, none delayed; no transient black row in
the 600 frames except the title-to-game switch at NES 328, which the
reference shows too (100% at 327-329). Fire Fighter, Jumper and Zuma had 10-16
delayed entries in a 20M-step window and now have none; no VT369 cart sets
the DMA up late. About a fifth of the refills (106 of 481) take the deferred
path. Audio against the reference is unchanged: Sky Fighter envelope 0.26 to
0.25, level 2.74 both; Table Soccer VT369 0.31 to 0.29; Fire Fighter 0.70
both; no new large sample jumps. The EWRAM stack reaches 552 of its 3072
bytes (396 before), the IWRAM user stack 0x03007C3C. On the control carts the
frame sequences are identical except new frames in the boot transition (Star
Ally 68-70, VG Pocket 55), as in s.88f and s.89f. The test ROMs at NES 150,
400 and 700 score exactly as with the s.89 core. With the gameplay input,
Sky Fighter at NES 800 goes from 96.47% to 97.02%, and Fire Fighter reads
58-59 NES fps at 800 and 900 where the s.89 core read 59-60 (one frame in
60, within the run-to-run spread of this measurement; not chased).

tools/probes/fgrab writes every GBA frame of a span, with FT/KEYS input, for
one-frame glitches that compare_furb's NES-frame-keyed captures step over.

## 91. Booting on flash carts and reproduction carts

A user relayed that a September 24 build booted on a flash cart (a
SuperCard) and the newest did not. Nothing could be tested on hardware here;
this section records an audit of the start-up path and of everything the
core does to the cartridge bus, and the changes that came out of it.

The one change since September 24 that touches the cartridge is s.79's
WAITCNT 0x4317 (3/1 wait states with the prefetch buffer), set by crt0 at the
first instruction since September 28. Carts that run a game from their own
RAM, and some reproduction carts, can be too slow for that timing; the core
then fetches wrong instructions and crashes before its first frame. Nothing
else that touches the bus changed. The memory layout is as it was (.bss
now ends 32 bytes lower), the September 24 core performs the same writes
outside RAM, and the play ROMs are 0.6-4.3 MB.

crt0 no longer sets WAITCNT. After the sections are copied, it calls
vt_waitcnt_probe, ARM code in `.ewram` (gba_crt0_my.s), so a cart that cannot
keep up only returns wrong data to it and cannot crash it. The probe
checksums the core image (4-word bursts over 0x08000000 to __rom_end__, then
4096 scattered halfword and byte reads in the first 64K) at the power-on
timing, sets 0x4317, and checksums it four more times; any difference
restores the power-on timing (0). SELECT held at power-on skips the test and
keeps the power-on timing, for a cart that passes but still misbehaves (the
test reads data, so it cannot see trouble that only shows in instruction
fetches through the prefetch buffer). vt_waitcnt_mode records the outcome:
1 fast, 2 test failed, 3 SELECT. `-DVT_WAITCNT_TEST_FAIL` makes the test
fail, for checking the fallback in mGBA, which cannot model slow cart memory.

Checked in mGBA: the normal build ends with WAITCNT 0x4317 and mode 1; the
test-fail build with 0 and mode 2; SELECT held from power-on gives 0 and mode
3 in both; all three play. Under the real BIOS the Docker core keeps the fast
timing (Time Pilot, Scramble, Star Ally, Sky Fighter, Fire Fighter, VG Pocket
at 59-62 NES fps, the usual pictures). The slow fallback gives the same
pictures at the s.79 speeds (Time Pilot 45 at NES 700, Star Ally 49-51,
Scramble 53-56, Sky Fighter 59-60). The test adds about 17 GBA frames
(0.28 s) to start-up, mostly the five passes over the 127K image from EWRAM.
Regression against the s.90 core: the test ROMs at NES 150, 400 and 700 and
the VT369 gameplay runs score identically, speed included. On the control
carts the start-up delay shifts every frame sequence, and the only new
frames are in the boot transition (GBA frames 74-89).

The ROM now also carries `SRAM_V113` (bindata.s, word-aligned), the marker
EverDrive, EZ-Flash and other carts use to choose the save memory they give
a ROM. Without it such a cart may give none. That does not stop the core
from booting: getsram re-initialises a save area whose signature is missing.
But settings and saves are then lost.

The rest of the audit, for the record. These were all present on
September 24 and none should stop a boot. mGBA's log flags a 4-byte overrun
of the LZ77 font (font2.lz77, unchanged since July). The font ends exactly
at 0x06003000, so those bytes land on the first word of .vram1. Both
loadfont calls (C_entry and splash) run before main copies .vram1 in, so the
copy always overwrites them; a new loadfont after that copy would not be
safe. The log also flags one RTC probe write to 0x080000C8 (header padding). Scramble writes 0x0680-0x069F through a stale pointer (the BIOS
area, so the GBA ignores the writes; the September 24 core does the same; not
chased). The reset-to-menu and crash paths send the reset sequences of
several flash carts (visoly.s), so a crash on a flash cart can look like
"back to the cart's menu". The core runs code from VRAM and EWRAM and rewrites
some of it (writeBG's slot, s.89; the x3 cost immediates, s.88). That is fine
on a GBA, on a DS in GBA mode and in mGBA, but may not be on emulators with
recompilers (gpSP).


## 92. Aero Gyrodine and Hex City X at full speed; Funny Coins; Zuma's top row

Aero Gyrodine and Hex City X ran their raster-split titles at 41-43 NES fps
(open item 4 since s.78d). Both wait on the title in the same loop: JSR to a
pad routine ($A8C9 in Aero) that strobes $4016 and shifts sixteen bits out of
$4016/$4017 into RAM, then LDA $0308, AND #$10, BEQ back. That loop ran about
150 times a NES frame, and emulating it cost 40% of the frame. find_poll_loop
(s.88) cannot take it, because it accepts only loads, compares and branches,
and the JSR, the stores and the pad reads are all outside that.

new_speed_hack.c now has a second finder, find_idle_loop, called from
quickhackfinder when find_poll_loop finds nothing on a VT cart. It is a small
6502 simulator that starts from the live state at the vblank: the registers,
NES RAM, the stack pointer, and the pads' shift registers as io.s keeps them
(joy0_R/joy0_W; _joy0state.._joy3state are now global for it). Writes go into
an overlay of up to 48 bytes, never into RAM. PRG reads go through memmap_tbl.
$4016/$4017 follow the io.s shift model. Any other address gives up. So do
indirect jumps, BRK/RTI, and a branch, ADC, SBC, ROL or ROR on a flag whose
value is not known yet (flags start unknown). The simulator runs from the
vblank's pc until a backward branch or JMP is taken at the shallowest call
depth seen; that target is the loop head. A vblank that lands inside the pad
routine therefore still finds the outer loop, which the first version, taking
the first backward jump, did not (Hex was never found). It then runs two whole
turns from the head. If the second turn leaves the registers, the flags, every
overlaid byte and the pad state exactly as the first did, the loop can only
leave through an interrupt or a button, and the hack goes on the loop's
backward jump, charged one turn's cycles (times three at CPU x1, s.88's
convention). The whole run is limited to 640 instructions.

The C code cannot see the 6502 registers, which live in r5-r7. speedhack_asm.s
now calls a ROM stub, speedhack_manager_regs, which stores r5-r7 into
vt_isim_regs and jumps to speedhack_manager; this kept `.vram1` at its 16 free
bytes. All the simulator's state is in EWRAM, because it runs on the vblank
handler's IWRAM stack.

The first version cost Sky Fighter 4-7 fps (60 down to 53-56 in places): the
finder runs every eighth frame in which no hack fired, and Sky Fighter's main
loop is long enough that each attempt ran the whole budget and failed. Two
changes fixed it. The simulator now runs on every fourth call only, and after
consecutive failures on every 8th, 16th and then 32nd call. A skipped call
reports success only while the slot still holds the hack this finder put there
(vt_isim_hack). Without that last rule the skipped calls let speedhack_manager
clear Hex's hack, and Hex fell back to 42 fps every few seconds.

Result: the Aero and Hex titles run at 60 NES fps, with pictures identical to
the s.91 core (99.99%). Sky Fighter over four gameplay inputs averages 59.3,
59.6, 57.1 and 59.3 fps, against 59.6, 59.9, 57.2 and 59.6 for the s.91 core,
which is within the run-to-run spread. Whether other carts take the new
hack was not logged; their pictures and speeds are unchanged (below).

A user on GitHub reported that Funny Coins (VT369) crashes about five seconds
into a game. In PocketVT the game dropped to about 10 fps with a scrambled
picture. That turned out to be an interrupt storm, not a CPU fault. The game
writes $4010 with the IRQ bit set and enables the DMC in $4015, and its IRQ
handler acknowledges only the VT timer, so the DMC IRQ (_wantirq bit 7)
fired again after every RTI. The VT369 has no DPCM channel. Furbtendulator
(APU_VT369::IntWrite, with a comment naming "Crazy Coins") drops $4010 writes
and clears $4015 bit 4 once $2010 or $201E is non-zero. write_vt369_4xxx
(mapVT.s) now does the same, testing vt369_reg $10 and $1E and vt_reg_2010,
with r1 and r2 only. Funny Coins now plays to NES frame 1800 (as far as it
was run) at 60 fps, with a title at 100% and gameplay at 98.2%. The residue is
one-line streaks at the edges of the board's raster-split bands, around the
hand cursor and along the board's left edge. These were not chased.

The same report said Zuma (and "Pyramid", which is not among the test carts)
shows glitches at the top of the screen. Nothing was wrong at the fast timing.
In the `-DVT_WAITCNT_TEST_FAIL` build (s.91's slow fallback, which a cart that
fails the start-up test gets), vt369_vblank's picture work ran past line 227 in
13% of frames. In those frames the top of the picture showed what PocketNES's
update_sprites (ppu.s), which runs earlier in the vblank, had written to OAM
0-63: PocketNES's view of the NES sprites, not the VT369 ones, so the top row
of balls vanished for a frame. vt369_set_mode now patches update_sprites' `mov
r2,#AGB_OAM` (label vt_oam_dest_mod) into `mov r2,#0x10000000`, an unmapped
address whose stores the GBA ignores, for as long as enhanced mode is on. OAM
then holds the previous frame's VT369 sprites until vt369_sprites replaces
them. In a 2000-frame fgrab capture at the slow timing, the frames whose top
band differed from both neighbours fell from 35 to 3, and those 3 are normal
motion. This is verified only in mGBA with the simulated slow timing; it is
not known whether the reporter's cart fell back to it, and Pyramid is
untested.

nestrace (tools/probes) now runs whole GBA frames up to FROMFT-1 before
single-stepping, so a trace that starts at NES frame 1700 takes seconds
instead of an hour.

Regression against the s.91 core (build_pvt.sh, all of the above). On the
test ROMs at NES 150, 400 and 700, Aero and Hex go from 41/42 to 60 fps at
t=150. Funny Coins is new (100%, 60). LLM VT369 rises from 99.59-99.67% to
99.66-99.73%. Everything else is identical. The VT369 gameplay runs are within
their usual spread: Fire Fighter 54/57/60 against 54/58/59, Sky Fighter 800
97.52% against 97.02%, and Zuma slightly higher. In the frame-set test the
only new frames on the control carts are in the boot transition (Star Ally
GBA frames 86-88, Lonely Island 93). The sound host test reports 0 failing.
The Docker core ends .bss at 0x03007B64 and .vram1 at 0x06003FF0. Under the
real BIOS, Aero, Hex, Funny Coins, Zuma, Sky Fighter, Fire Fighter, Star Ally
and Lonely Island boot and play.

## 93. Soccer 2009: mirroring, $4119, COLCOMP sprites, the VT03 colour table

A user asked for Soccer 2009 (NES 2.0 mapper 256.0, VT03, 128K PRG and a 256K
CHR ROM). Its menus ran from the start, but a match showed a pitch with the
right third wrong, no texture, no players and no score, at 35-42 NES fps. Six
separate faults were behind that. They are recorded here in the order they
were found.

The game sets its mirroring through the MMC3-compatible $A000 (1, horizontal,
at the VS screen). vt_mmc3_forward only flagged the change (vt_mirror_dirty);
the flag was applied by the next $41xx write that reached the C path, and
Soccer 2009 makes none after its $A000. So the pitch it wrote to $2800 went
into the nametable shown at $2000, the second nametable kept the menu, and
with a horizontal scroll of 80 the right nine columns came from it.
write_vt_rom now applies the flag itself, as write_vt4xxx does.

The pitch texture was missing because the palette RAM held a different
table: 201 of 256 bytes differed from the reference. The game reads $4119 and
chooses its palette table by bits 3-4 (XPORN, XF5OR6: the TV system).
PocketVT had answered $18, PAL, to every VT03 and VT09 since session 12,
because Lonely Island's colours came out right with it. That was the wrong
reason. Furbtendulator answers from the region, $00 for NTSC and $18 for PAL
or Dendy (APU_OneBus::IntRead), and every test cart's header says NTSC. On an
NTSC VT03 a video DMA writes palette bytes one entry early, a quirk the
datasheet warns about ("if NTSC, shift palette data one byte") and that
Furbtendulator models (PPU_OneBus::IntWrite: Palette[(addr - 1) & $FF] during
a DMA, and $4000 wraps to $3F00). Lonely Island's NTSC routine DMAs to $3F01
to compensate. Answering PAL sent it to its PAL routine, which DMAs to $3F00
and gives the same palette; Soccer 2009 instead picked its PAL colour table.
loadcart.c now sets vt_4119 from NES 2.0 byte 12 ($00 NTSC or both, $18 PAL or
Dendy; $00 on VT32 and VT369, as before for VT369; $18 when the header names no
console type, so old headers keep the old answer) and vt_pal_dma_shift for a
VT03 in the NTSC region. The generic DMA loop in mapVT.s has a copy for that
case which sends palette bytes through vt_dma_pal_early; vt_pal_dma_fast
(IWRAM, unchanged: growing it pushed .bss 16 bytes past the limit) gets the
shifted start from its caller. Soccer 2009's palette RAM now matches the
reference exactly. Lonely Island, Push the Ball, Jewel Master VT03 and
Scramble, which also read $4119, differ from before in one palette byte,
entry $FF, which held $3F and now holds $00 as in the reference; their GBA
palettes are unchanged.

The first version put the shift test inside the shared per-byte loop. That
cost a few instructions per DMA byte on every cart, and Table Soccer VT369's
knockout screen lost two bracket corners (98.40% to 98.19%) with its game RAM,
nametables and palette byte-identical: 224 bytes of BG tiles were stale. That
is the timing-exposed stale tile cache of open item 3 (Push the Ball's 20
pixels), not a decode fault. With a separate loop every other cart runs
exactly the old instructions and the corners are back.

The players and the score were missing because vt_spr_eva_update refused
SPEXTEN sprites whenever COLCOMP was set ($2010 = $9E here: COLCOMP, BKEXTEN,
SPEXTEN, SP16EN, BK16EN). The refusal predates the COLCOMP 16-colour OBJ
palettes of s.70b, and no other test cart sets the two together. Without it
the players drew, but the score and the ball-holder marker showed other
tiles. A match frame uses eight (page, EVA) pairs and there were four slots,
so one page was reassembled every frame and the sprites on evicted pages drew
stale slots. The 2 KB slots (4bpp and 2bpp extension) now number eight,
filling 0x06010000-0x06013FFF, which is free once PRG page 0 has moved out
(vt_prg_evict_obj); the 4 KB PIX16 slots stay at four. Lucky Lawn Mower VT09
had the same thrash in its opening: its frames there went from 98.7-99.6% to
99.7-99.8%, and those are the 172 new frames its frame-set test shows. Jewel
Master VT03's gameplay rose from 98.9/98.6% to 99.7%.

The colours were still off: the pitch's texture came out olive with red
specks where the reference has two greens. Entry $302, for example, was
(20,17,0) in 5-bit RGB against Furbtendulator's (11,21,0). vt03_palette_lut was
EmuVT's HSL2RGB.TAB from the NESdev wiki. It is now generated by
tools/gen_vt03_lut.py from Furbtendulator's own formula (GFX.cpp, VT03Palette=0,
the NTSC model it uses by default), rounded to float32 as the C code is; all
4096 entries equal furb_cli --dump-palette. That moves every COLCOMP cart to
the reference: Aero Gyrodine 63/99% exact5 to 100%, Hex City X 70/97% to
99.98-100%, Add 'em Up 5/24% to 99.2/99.98%, Table Soccer VT03 0.02% to 96.1%,
Soccer 2009 to 99.5% in the menus and 97-98% in a match. The 64 standard
colours (COLCOMP clear) still come from the calibrated DACs of s.57; s.85 left
that choice to the user and nothing here touches it. The old table is in git
history.

compare_furb's struct score keyed reference colours at 24 bits and PocketVT's
at 15. Furbtendulator draws some whites as #FEFEFE and others as #FFFFFF, and
two near-blacks of Add 'em Up differ only below 5 bits, so once PocketVT
matched both exactly the score counted a split: Add 'em Up's title fell from
98.17% to 94.85% with exact5 at 99.19%. Both sides are now keyed in 5-bit RGB
(ref_ids5); the report still prints the reference's commonest 24-bit colour.
Scores from before this section are not comparable; the regression below
re-scored the s.92 core with the new tool.

Speed. A match runs the CPU right up to the NMI at line 240 in every frame
(8.6-9K instructions, no wait loop), so no speed hack applies, and the 6502
core alone costs about 272K GBA cycles a NES frame against a budget of 281K.
Full speed is out of reach; what could be cut was VT overhead. The game
switches $8000-$9FFF through MMC3 command 6 about 16 times a frame, and every
$8000 and $8001 write ran vt_recompute_prg_banks. write_vt_rom now sends $8001
under command 6 or 7 to vt_w4_bank (as a $4107/$4108 write, unmangled as in
NRS writeMMC3; only when vt_w41_fast allows it), and $8000 recomputes only
when COMR6 or COMR7 changes. The BKEXTEN scrub swept 60 unchanged cells a
frame for ~25K cycles; a chunk is now skipped while everything
vt_bk_write_cell reads for it is unchanged (its nametable and attribute bytes,
the slot keys, $2000 bit 4), and all chunks are forced every 256 frames. The
slot checksum (s.85) checks a quarter page a frame instead of a whole one.
vt_bk_slot_get is noinline: the compiler had inlined it and the slot fill
into vt_bk_write_cell, which lives in IWRAM, and keeping only the LUT-hit path
there frees 224 bytes (__bss_end__ 0x03007B74 to 0x03007AA0). Match speed went
from 35-39 to 42-44 NES fps; menus run at 60. The rest of the overhead is the
sprite slot update (~15K cycles a frame), the 16-colour palette rebuild (~14K)
and scale75 (~11K).

Regression against the s.92 core, both scored with the new struct. On the test
ROMs at NES 150, 400 and 700 the COLCOMP carts move to the reference's colours
(Aero Gyrodine 100%, Hex City X 99.98-100%, Add 'em Up 99.19/99.98%, Table
Soccer VT03 exact5 0.02% to 96.09% with struct unchanged at 96.23%) and Soccer
2009 scores 99.5-99.7%. Jewel Master VT03 rises from 98.94/98.59% to
99.67/99.71% at 400/700 and Time Pilot from 97.59% to 97.68% at 700. Scramble
moves from 99.41/99.00% to 99.49/98.94% (the best reference frame at 700 is 697
instead of 696). Everything else is identical. In the VT369 gameplay runs only
Zuma changes, from 97.94/97.82/97.58% to 97.67/97.55/97.33%: its game RAM
differs from NES frame 400 on (a few RNG and stack bytes, later the ball queue
at $300), so its random balls differ while the picture is sound. The change
came with the scrub and checksum changes, which alter only ARM-side timing, so
Zuma's random draws evidently depend on GBA timing, presumably through the
sound HLE that runs on GBA timers (not verified). The frame-set test shows Star
Ally (GBA frames 82, 86) and Lonely Island (93) with new boot-transition frames
only, Scramble with none, and VG Pocket's sequence identical; Lucky Lawn
Mower VT09's 172 new frames are the sprite-slot fix, and Aero Gyrodine and
Table Soccer VT03 differ everywhere through the colour table. The sound host
test reports 0 failing. The Docker core ends .bss at 0x03007AA0 and .vram1 at
0x06003FF0. Under the real BIOS, Soccer 2009 (into a match), Star Ally, Lonely
Island, Aero Gyrodine, Add 'em Up, Zuma and LLM VT09 boot and score as in mGBA.
