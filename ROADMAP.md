# PocketVT ROADMAP — from "map playable" to "fully playable, correct graphics"
Self-contained handoff: any capable engineer or model, starting from a fresh
sandbox with only the uploads, should be able to continue from here.

## 1. Project definition
PocketVT is Michael's PocketNES fork emulating VT02/VT03/VT09 OneBus
famiclone ROMs on GBA. Primary target: Lonely Island (LI), mapper 256
submapper 15 (opcode-scrambled JungleTac cart, 128KB PRG, CHR from PRG via
$2012-$2017 sync). Success = LI fully playable start to end with correct
graphics and acceptable audio. Star Ally is deferred by Michael's request.
Authoritative hardware references live in the repo (DATASHEET_DIGEST*.md)
and Michael's NESdev wiki exports (uploads/NESdev_Wiki-*.xml: VT02+
Registers / Video Modes / CHR+PRG Bankswitching / CHR Pattern Layout /
Enhanced Palette / Sound). Read those before touching video or banking.

## 2. Current verified state (as of session 10)
The game boots, renders the island map in correct 16-colour palette, plays
music, and the penguin player character walks the map path graph under
real input, verified end to end in-sandbox (frame-differenced realtime
captures show the sprite traversing the scripted route; the game's
position variable $06F4 tracks node to node). All of the following are
FIXED and must not regress: the session-6 speedhack/op_table encrypted-BCS
clobber (set_cpu_hack gated on vt.encryption_active); the session-7
palette write path (full 7-bit offset into vt_palette_write_lo, linear
storage, vt_16c_palette_fixup as the frame's final palette writer); the
flicker default (0); the 4bpp sprite overlay (inside vt_16c_palette_fixup,
inlined copy, sigoff change detection); the session-9 IWRAM relocation
(CHR_DECODE + YSCALE tables in EWRAM .sbss, vt_optable_canonical in EWRAM,
__bss_end__ must stay under ~0x03007B00); the session-9 palette-dirty perf
fix (never set vt_chr4_dirty from palette writes); and the session-10
input fix (joycfg bit30 set: the famiclone controller is NES PLAYER 2 —
LI's movement handler reads the $4017-derived pressed-edge at zp $36).

## 3. How the game actually works (reverse-engineered, trust this)
Bit layout as the game sees the pad after its serial read: A=$80 B=$40
Sel=$20 Start=$10 U=$08 D=$04 L=$02 R=$01, current state in zp $00 (P1) /
$02 (P2), pressed-edge in $34/$36. The island map is a node graph stored
in famiclone WRAM at $6500+ indexed by position packed as YX nibbles
(current position $06F4, target $06F7/$0602): type $01 = path, $0A-$0F =
the six house/level nodes (Y2XD=$0F, Y3X9=$0A, Y4X3=$0B, Y7X8=$0C,
Y9XD=$0D, YBX6=$0E). Start is Y7X2. The direction handler ($CE15, fixed
$C000 bank) validates and queues; the walk executor ($B824, bank $0B at
$A000) animates and commits; its single caller is $D073 which also queues
footstep sound $12 to the mailbox $05CD. Walking INTO a house node enters
its scene (position resets to 0, banks flip to [7,6]); Select opens a
level-enumeration menu that scans all six special nodes. The NMI uploads
nametable data with the stack-blast trick ($E221 task: repoint SP to $01FF
then PLA/STA $2007 pairs; producer fills $0100 and raises $87 bit4).
$2010 stays $0E on the map and in the first house scene; the $1E/$1F
BKEXTEN+PIX16EN mode noted in FIX_4BPP_DESIGN.md is expected inside actual
minigames — capture it with the tools below when first reached.

## 4. The remaining milestones, in order
M1 — Traverse a house scene to gameplay. In the mini-reference (see §6),
walk U,U,R,U then U into house $0B; the scene persists with banks [7,6].
Determine what the scene wants (it is a dialog/menu: reconstruct its
nametable from the $2007 stack-blast writes in the mini-ref, or Xvfb-
capture it in PocketVT with the autoplay build) and script the inputs to
reach the minigame proper. Acceptance: $2010 leaves $0E, or the scene
demonstrably becomes a playable minigame.
M1.5 — SPRITES / SPEVA (the penguin). Root-caused in session 13 with ROM
evidence: the effective sprite CHR bank is (page_bank << 3) | EVA where EVA
= OAM byte 2 bits 2-4, and PocketVT ignores it, drawing a solid garbage
block. The full implementation spec -- register layout of update_sprites'
r3, the free spr_cache_map indices, the free OBJ slots 0..7 -- is in
SESSION13_STATUS.md. Do this before M2; it is the last obviously-wrong
thing on screen.
M2 — Implement the in-game video mode. When M1 reaches it: BKEXTEN (BG
tile number extended by the 16x16 area's attribute bits + BKPAGE, palette
forced to set 0 — GBA's 10-bit tile field fits attr<<8|name exactly, so
extend vt_chr4 assembly/nametable conversion accordingly; effective CHR
bank = (reg<<3)|EVA and games load $2012-17 pre-shifted), SPEVA for
sprites (OAM byte 2 bits 2-4 are three extra tile bits — LI's penguin
already uses EVA 2 and 4; vt_obj4_overlay must honor them), and PIX16EN
(16-pixel-wide 2bpp sprites: low 2 bits left half, high 2 bits right).
Acceptance: minigame screenshot matches plausible art, no garble.
M2.5 — DONE (session 13): 60 fps reached. write_vt4xxx now dispatches on
the address high byte before building a call frame, so plain APU writes
tail-jump into IO_W. Frame cost 1.00; verified via the guest NMI tick. The wait-loop speedhack and the
inlined palette store took 20->31fps (frame cost 3.0 -> 1.92 GBA frames);
the post-fix profile shows the remainder spread across the core: opcode
handlers, the game's $2007 stack-blast uploads, and write_vt4xxx dispatch.
Candidates in order: an asm-side shadow-compare for the per-frame
$2012-17/$4106/$2010 rewrites before entering C; audit
vmdata_W/vramaddrinc; only then core fetch/dispatch work. Re-profile after
every change (frame-pause r15 sampling -> nm symbolization, recipe in §6).
Session 12 landed the $4119 read hook (see §7) and vt_pal_dma_fast, taking
31 -> 39 fps and fixing the palette entirely; the grass-colour item is CLOSED.
The top profile entry is now write_vt4xxx (~8%), which every $4000-$40FF APU
write pays before reaching IO_W -- dispatch earlier on the high byte. Then
the core opcode handlers.
M3 — Audio correctness. The engine runs (channel flags $0555/$0594
toggle; footstep $12 and ding $13 queue via $05CD). Compare XOP register
writes against the wiki Sound page by ear and by trace. Acceptance:
Michael says the tune sounds right.
M4 — Full playthrough. Autoplay-script or hand-play each house/minigame;
fix crashes as found using the session-6 methodology (deterministic
anchor, trace, op_table/state diff). Acceptance: reach the game's ending.
M5 — Perf and polish backlog: stop feeding the 2bpp tile cache in 16c
mode, $4106 H/V handling, vt_reg_read wiring, palette shade tuning vs
lone.png, Star Ally when Michael re-supplies it.

## 5. Environment bootstrap (sandbox resets every session)
apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi
libnewlib-arm-none-eabi mgba-sdl xvfb imagemagick; pip install py65 pillow
--break-system-packages. Unzip uploads/pocketvt_full_src.zip to
~/pocketvt (or use the newest pocketvt_src_sN.zip in outputs — always the
newer). Fetch libgba HEADERS: curl codeload.github.com/devkitPro/libgba
tar.gz. Recreate build_pvt.sh per SESSION5_STATUS.md §build (compile all
.c, all .s with -x assembler-with-cpp, Mappers/*.s, plus a thumb
`swi 0x12` LZ77UnCompVram stub; link -Wl,-z,muldefs -T gba_cart_my.ld
-nostartfiles; objcopy; fix header complement byte at 0xBD; python3
builder.py "Lonely Island.nes" — carve the .nes from any play_li*.gba at
offset 0x184B0 if needed). build_pvt.sh rm -rf's the build dir: recopy
builder.py and the .nes after every build. RE-DERIVE EVERY ADDRESS WITH
nm AFTER EVERY LINK — symbols move each generation; a stale poke address
cost an hour once.

## 6. The debugging toolkit (all proven this project)
The Python mini-reference (~/vtref.py, also in this zip) is the single
most powerful tool: py65 CPU + descrambling fetch + PRG banking + timer
IRQ + NMI + controllers + $6000 WRAM, no video. It runs LI's logic at
full fidelity, supports per-address read/write tracing by monkeypatching
Mem, and answers "what does the game want" questions in seconds — it
found the P2-input and WRAM discoveries. Extend it rather than
reverse-engineer blind. In mGBA (-d, SDL dummy drivers): deterministic
frame anchors, break/watch (watch/w on a RAM byte anchors traces at
events like the NMI $26 tick), trace N FILE (guest A=r5 hi byte, X=r6 hi,
NZ mirror=r3, 6502 C=r8 bit0; opcode dispatches contain 579AF100; decode
banked PCs by PRG offset via host 0x02004D00 base, fixed bank appears at
0x0600xxxx), w/1 memory pokes for input via the vt_dbg_pad_or hook
(io.s no4scr; address per generation). Realtime visuals: all-in-one bash
call, setsid Xvfb :99, DISPLAY=:99 SDL_VIDEODRIVER=x11 mgba -C
videoSync=1, sleep, import -window root; guest runs ~1/3 speed so scale
wall-clock accordingly; captures occasionally come back black — retry;
verify motion by frame-differencing when color filters are ambiguous.
The autoplay build (-DVT_AUTOPLAY, script table in vt_16c_palette_fixup)
makes realtime runs self-playing for visual verification.

## 7. Traps that already bit us (do not repeat)
Memory READ handlers (readmem_* hooks) are entered with "adr lr,0f ; ldr pc,
[...]" and have NO usable ARM stack -- sp is repurposed by the 6502 core.
A read hook must never push and never call C; answer inline and tail-jump
(ldr pc,=IO_R) to the stock handler. Pushing there crashes instantly with a
garbage sp. Also: vt_palette_ram links to an odd address, so mGBA r/4 word
reads round down and fabricate a one-byte shift -- always dump it with r/1.
The vblank IRQ stack is tiny and canary-guarded: never add a bl_long call
chain or memcpy32 inside the vblank path — fold work into existing call
sites and inline copies; the alarm is a gray/white flashing screen
(non-fatal) or a DEADBEEE/DEAFBEEE jump (fatal), and headless frame-
stepping hides the flashing — always realtime-verify after IRQ-path
changes. IWRAM is at the cliff: every new C global gets EWRAM_BSS, and
check `nm | grep __bss_end__` stays below ~0x03007B00 after linking, or
tables silently mirror onto guest RAM (the YSCALE disaster) and the user
stack eats canaries. Never set vt_chr4_dirty from palette writes. The
op_table under encryption is sacred: nothing may write raw-indexed
entries while vt.encryption_active (the speedhack lesson). r10-relative
equate symbols (joy0state, windowtop, timeout nodes) resolve as
0x03006F3C + offset. mGBA r/4 of write-only or mid-frame state can
mislead; sample at breakpoints with registers in view.

## 8. Deliverable discipline
Every session ships: the injected .gba, the full source zip, the ELF for
symbols, a SESSION_STATUS.md, and updated /areas/pocketvt.md memory.
Verify in-sandbox before claiming anything; Michael field-tests but must
never be asked to debug. Honest accounting of what is fixed, what is
diagnosed, and what is unknown.

== Session 14 addendum ==
SPEVA is implemented and verified: effective sprite CHR bank =
(page_bank << 3) | EVA, EVA = OAM byte 2 bits 2-4, gated on $2010 bit 3
(SPEXTEN). update_sprites looks up spr_cache_map[64 + page*8 + EVA]; C fills
those entries with OBJ slots 0..3 (0x06010000-0x06011FFF, untouched by the
stock cache at slots 8..15), assembling at most one 1KB page per vblank.
Flag: vt_spr16_active.

Next up, in order:
 * Repair tools/vtview.py's input path (holds do nothing; taps used to work)
   so the reference renderer can walk into house $0B. Needed to decide
   whether the side-scrolling stage's black background is authentic. Evidence
   so far says it is: the GAME stages $0E into palette entries $00-$3F itself
   and PocketVT copies it faithfully.
 * M2 in-game video mode ($2010 = $1E/$1F): BKEXTEN, PIX16EN.
 * M3 audio-by-ear.

== Session 15 addendum ==
Nametable mirroring is now real. $4106 bit 0 (and MMC3-compat $A000 bit 0)
select the arrangement; vt_regs.c flags the change, write_vt4xxx applies it
via vt_set_mirroring (cart.s). CRITICAL: mapVTinit must clear SCREEN4+VS from
cartflags -- PocketNES's loader mis-reads NES 2.0 flags7 ($0B) as VS
Unisystem, and mirrorchange force-overrides to four-screen (m0123) whenever
SCREEN4 or VS is set. That forced four independent nametables on a 2KB-CIRAM
VT board and was the side-scroller's black background.

The session-14 note claiming that black was "the game's own staged palette"
is retracted. It was blank nametable tiles.

Remaining: M2 in-game video mode ($2010 = $1E/$1F, BKEXTEN/PIX16EN) if any
stage reaches it; M3 audio-by-ear (channel correctness never examined);
M4 full playthrough; M5-opt horizontal affine BG scaling for the 8px/side crop.
