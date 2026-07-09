# PocketVT Code Audit vs Datasheets

Checking the discrepancies flagged in DATASHEET_DIGEST.md / _VT02.md against
the actual PocketVT source. Each entry says what the digest predicted, what
the code actually does, and the verdict.

## *** DECISIVE DATA from furb_cli (reference emulator run on the test ROMs) ***

Built the user's furb_cli (the CLI Furbtendulator) and added OBSERVATION-ONLY
logging (no emulation-logic changes) to capture what the PERFECT emulator
does on Lonely Island and Star Ally. Results:

LONELY ISLAND (5-8M cycles):
  - COLCOMP stays 0 the entire time (reg2000[0x10] never gets bit7).
  - Writes 128 lo-palette ($3F00-$3F7F) + 128 hi-palette ($3F80-$3FFF)
    bytes, ALL = $3F (a boot-time palette clear). The hi ($3F80) writes
    happen but, with COLCOMP=0, GetPalIndex IGNORES them (returns
    Palette[TC], lo only). No $2010 / bank writes within 8M cycles (still
    in early init / asset loading).

STAR ALLY (30M cycles) -- the important one:
  - COLCOMP stays 0 (reg2000[0x10] bit7 never set).
  - $2010 = $1E then $1F. Decode:
        $1E = BK16EN|SP16EN|SPEXTEN|BKEXTEN  (bits 1,2,3,4)
        $1F = + PIX16EN (bit 0)
    => bit6 V16BEN=0, bit7 COLCOMP=0.
  - Bank writes: $2016=$02, $2012=$00, $2018=$00.

*** THE KEY INSIGHT (from real data, not guessing): the games run in
COLCOMP=0 (NES-style palette path) BUT with BK16EN|SP16EN = 1 (16-colour /
4bpp tiles) and extension addressing on. So the live rendering combination
is: 4bpp tile DATA + the NON-COLCOMP (NES-style) palette. ***

This reframes the bug precisely. Furb (perfect) renders 4bpp tiles whose
pixels are 4-bit (16-colour) indices, but colours them via GetPalIndex's
COLCOMP=0 branch = Palette[TC] into the 2C02-style base palette. So in
Furb, a 4bpp pixel's color-address bits index the SAME programmable
Palette[] RAM, just with more bits of pattern color (bits 6,7 added per the
digest's "7-bit / 16-colour" mode), still WITHOUT the COLCOMP saturation
composite.

POCKETVT MUST DO THE SAME, and this is the likely break:
  - PocketVT's four_bpp path (vt_chr_sync_from_prg, $2010 & 0x06) DOES
    detect 4bpp and de-interleave. Good -- matches $2010=$1E/$1F.
  - BUT the PocketNES tile cache + run_palette/MAPPED_RGB pipeline it then
    feeds is a 2bpp / 4-colour-per-tile NES path. A 4bpp (16-colour) tile
    pushed through a 2bpp NES cache renders scrambled -- THIS is the
    NES-vs-VT seam, now confirmed to be ACTIVE for these ROMs.
  - Equally: the 7-bit (16-colour) pixel -> palette index mapping (pattern
    bits 6,7 + attribute bits 3,4 + sprite/bg bit 5) must reach a 16-entry
    sub-palette. The NES path only provides 4-entry sub-palettes.

NEXT: verify how PocketVT's NES_VRAM copy + tile cache handle a 4bpp tile,
and whether the GBA tilemap/palette is set to 16-colour (256-colour GBA
mode / 4bpp GBA tiles) when $2010 BK16EN is set. If PocketVT leaves the GBA
BG in 4-colour NES-cache mode while the VT cart is in 16-colour mode, that
is the garble. This is now a CONCRETE, data-backed hypothesis (vs the
earlier guesses) because we KNOW these ROMs set BK16EN/SP16EN with COLCOMP=0.

### CONFIRMED by tracing PocketVT's tile pipeline
  - PocketVT detects four_bpp ($2010 & 0x06) ONLY in vt_chr_sync_from_prg
    (ppu_vt.c:536); four_bpp is used NOWHERE else (grep: 2 hits, both there).
  - It is NEVER used to change GBA BG colour depth, the tile-cache decode,
    or the palette sub-palette width.
  - The PocketNES tile cache (ppu.s flush_recent_tiles / CHR_DECODE) decodes
    every tile as NES 2bpp -> GBA 4bpp tile, producing only pixel values
    0-3 (2 bitplanes), using 4 of 16 GBA palette entries. GBA tiles are
    32 bytes (tilenum<<5, 4bpp) but only ever filled with 2bpp content.
  - So when the VT cart runs 4bpp (BK16EN/SP16EN, as Star Ally does with
    $2010=$1E/$1F), the 16-colour tile data is de-interleaved into NES_VRAM
    and then RE-DECODED AS 2bpp by CHR_DECODE. The upper 2 bitplanes of each
    4bpp tile are dropped / misread, and the byte stride is wrong for the
    2bpp cache -> scrambled tiles. THIS is the garble, and it is exactly an
    NES-logic-applied-to-VT-data bug.

### ROOT CAUSE (data-backed, high confidence)
PocketVT has no true 16-colour (4bpp) BG/sprite RENDER path. It only
de-interleaves 4bpp CHR into NES_VRAM, then renders it through the inherited
PocketNES 2bpp tile cache + 4-colour NES palette pipeline. The test ROMs
run in 4bpp (BK16EN/SP16EN) from early on, so their tiles are decoded with
the wrong bit-depth => garbled from boot. COLCOMP=0 means the palette path
itself (NES-style) is fine; the break is the 2bpp-vs-4bpp tile decode, not
the palette.

This is consistent with EVERYTHING: bank math/de-interleave/reset all match
Furb (the data GETS into NES_VRAM correctly as 4bpp), but the final NES
2bpp tile decode mis-renders it. Furb avoids this because its host NES PPU
renders VT 4bpp tiles through a VT-aware GetPixel/GetPalIndex path (7-bit
pixel -> programmable Palette[]), not a fixed 2bpp NES cache.

### FIX DIRECTION (large but well-defined)
PocketVT needs a real 4bpp render path when $2010 & 0x06:
  - Decode VT 4bpp tiles as 8bpp/4bpp GBA tiles using ALL 4 bitplanes
    (a 4bpp CHR_DECODE variant), not the 2bpp NES decode.
  - Use 16-entry GBA sub-palettes (or 256-colour GBA BG mode) so the
    16-colour pixels have colours to land in.
  - Drive the GBA BGxCNT colour-depth bit from $2010 BK16EN.
This is a real rendering-path addition, not a one-line fix -- but it is now
PRECISELY SCOPED and matches how the reference treats these ROMs.

### Audio loop (separate bug, noted for later)
furb_cli stderr shows the NMI/timer pattern (Reg2000 $00->$A0->$20->$A0).
Not yet traced; the looping audio is likely a timer-IRQ ($4101/$4103/$4104)
or DWS/PCM DMA restart issue, separate from the video path. Defer until
video renders.

NOTE on furb_cli edits: only added fprintf logging (writePPU, PPU_OneBus::
IntWrite, GetPalIndex) + a g_ppu_log global. No emulation logic changed;
the reference behaviour is intact. These are debug instrumentation, not
modifications to the emulation.


CORRECTION (verified by tracing callers + Furb): my earlier claim that the
COLCOMP=0 path indexes the VT03 LUT black corner was WRONG. Trace:
  - vt03_composite_to_gba() (ppu_vt.c:222) is DEAD CODE -- NO callers (grep
    finds only its definition). Its lo&0x3F black-corner branch never runs.
  - vt_palette_rebuild_gba() Gate 2 (ppu_vt.c:277): COLCOMP=0 -> returns
    immediately, does NOT touch the GBA palette.
  - COLCOMP=0 coloring is done by stock PocketNES run_palette (ppu.s:2976),
    filling MAPPED_RGB from nes_rgb (standard 2C02 NES 64-colour palette)
    via paletteinit/remap_pal, gamma-corrected.
  - Furbtendulator does the SAME for COLCOMP=0: GetPalIndex returns
    Palette[TC] into base region 0-0x1FF = a "2C02 Palette" (GFX.cpp:1601).
  => BOTH emulators colour COLCOMP=0 via the NES palette indexed by the
     6-bit value. PocketVT AGREES with the reference here; not the cause.
I over-claimed before tracing callers. Recording so it doesn't mislead.

OPEN questions this leaves:
  - Are these ROMs in COLCOMP=0 at the moment of garble, or do they flip
    COLCOMP=1 in init? (determines which palette path is even active)
  - Is nes_palette (the 6-bit source MAPPED_RGB indexes) correctly filled
    for VT carts via the $3F00 write path?
  - The COLCOMP=1 rebuild path (ppu_vt.c 318-334) is NOT disproven.

Symptom: PocketVT boots Lonely Island / Star Ally (both mapper 256,
submapper 15 = Jungletac, CHR=0KB so all graphics in PRG) into garbled
graphics + looping audio. Comparing vt_chr_sync_from_prg (ppu_vt.c) and
the bank math against Furbtendulator h_OneBus.cpp (the cited reference).

### Reference facts established from Furbtendulator
$2010 (reg2000[0x10]) bit map (h_OneBus.cpp:4-8):
    bit1 0x02 = BK16EN     bit2 0x04 = SP16EN
    bit3 0x08 = SPEXTEN    bit4 0x10 = BKEXTEN
    bit6 0x40 = V16BEN  (use chrLow16/chrHigh16 arrays)
Mapper256 submapper 15 (Jungletac): ALL three mangle tables (ppuMangle,
cpuMangle, mmc3Mangle) are IDENTITY -- "CPU opcode encryption only"
(mapper256.cpp). So for our ROMs there is NO register-address mangling;
$2012-$2017, $4107-$410A, MMC3 selects all pass through unmodified. The
garble is therefore NOT a mangle-table problem.

CHR de-interleave: Furb splits CHR at load (reset, RESET_HARD):
    chrLow/chrHigh  (every cart): shiftedAddr = (i&0xF)|((i>>1)&~0xF);
                                  (i&0x10)? chrHigh : chrLow   [BIT-4 split]
    chrLow16/chrHigh16 (VT09/VT369 only): (i&0x1)? high16:low16  [BIT-0 split]
setCHR(bit4pp, base): if !bit4pp, base=chrData (RAW, no split); if bit4pp,
base stays chrLow/chrLow16. syncCHR passes base = V16BEN?chrLow16:chrLow,
bit4pp = BK16EN||SP16EN.

For Lonely Island ($2010=$0E = BK16EN+SP16EN+SPEXTEN, V16BEN=0):
    -> bit4pp = true, V16BEN = false  -> base = chrLow (BIT-4 split).
PocketVT four_bpp path uses src_j=(j&0xF)|((j&~0xF)<<1) = the BIT-4 gather,
reading from raw PRG. This MATCHES Furb's chrLow construction. So the
de-interleave formula itself is NOT the bug for this ROM.

### [OPEN] Where it actually diverges -- bank addressing (investigating)

Bank COMPOSITION matches: PocketVT vt_compute_chr_bank() ==
Furb setCHR non-extended math, term for term:
    inner_bank & inner_mask   == reg & chrAND
    middle & ~inner_mask      == RV6 & ~chrAND   (RV6 = $201A & 0xF8)
    intermediate << 8         == VA18 << 8       (VA18 = $2018>>4 & 7)
    outer << 11               == VA21 << 11      (VA21 = $4100 & 0x0F)
with inner_mask = 0xFF >> VB0STable[VB0S], VB0STable={0,1,2,0,3,4,5,0}.
So the per-page bank NUMBER is correct.

Two architectural differences from Furb that are the real suspects:

LEAD A -- different rendering model (most likely root cause).
  Furb NEVER copies CHR; syncCHR sets 8 CHR POINTERS (SetCHR_Ptr1) into
  either raw chrData (2bpp) or the pre-split chrLow/chrLow16 arrays (4bpp),
  and the renderer reads tiles through those pointers. PocketVT instead
  COPIES 8x1KB into NES_VRAM and lets the PocketNES GBA tile cache render.
  The seam between VT banked CHR and the NES tile cache is the most likely
  source of boot garble. In particular PocketNES's cache expects NES 2bpp
  tile layout (16 bytes/tile, planes 0 then 1); whatever PocketVT writes
  into NES_VRAM must be in exactly that layout for every tile or the cache
  renders scrambled.

LEAD B -- 4bpp source stride. PocketVT four_bpp path:
      phys_1k = vt_chr_bank_byte_offset(bank) >> 10     // = bank number
      src_off = phys_1k * 2048                          // = bank << 11
      dp[j]   = src[ ((j&0xF)|((j&~0xF)<<1)) & 0x7FF ]   // bit-4 gather
  Furb 4bpp: address = bankval<<10 into the HALF-SIZE chrLow array (built
  once with the same bit-4 split). Net raw-PRG span per page = 2KB in both.
  The PER-PAGE math looks equivalent, BUT PocketVT recomputes the gather
  every sync and bounds src_j to a single 2KB window (&0x7FF) per page;
  if two CHR pages are NOT 2KB-contiguous in PRG (i.e. bankval not even),
  the &0x7FF wrap reads wrong bytes. Furb avoids this by pre-splitting the
  WHOLE rom once so cross-page addressing stays linear. NEEDS a concrete
  trace of the bank values Lonely Island writes to confirm.

LEAD C -- CHR reset defaults: RESOLVED, NOT a bug. PocketVT vt_reset()
  (vt_regs.c 163-168) sets $2012=04 $2013=05 $2014=06 $2015=07 $2016=00
  $2017=02, matching Furb h_OneBus.cpp reset (lines 325-330) EXACTLY,
  including the non-obvious $2017=0x02. PRG defaults (PQ1=01, PQ2=FE) also
  match. The reset path is faithful.

HONEST STATUS: Static analysis has RULED OUT: register mangling (identity
for submapper 15), bank composition math (matches Furb term-for-term),
de-interleave formula (matches Furb bit-4 split), and reset defaults
(match Furb exactly). What REMAINS is LEAD A: PocketVT's rendering MODEL
differs from Furb -- it copies 8x1KB CHR into NES_VRAM and reuses the
PocketNES GBA tile cache, whereas Furb sets direct CHR pointers. The
NES_VRAM that PocketVT writes must be in the exact byte layout the
PocketNES tile-decode (ppu.s update_tile / consume_bg_cache, ~line 1202+)
expects, for every tile, or the cache renders scrambled. This seam is the
leading hypothesis for "garbled from boot."

I cannot confirm the exact defect in LEAD A by static reading alone -- it
requires either (a) a runtime register/CHR trace of Lonely Island's first
frames, or (b) reading the full ppu.s tile-cache decode to verify the
expected NES_VRAM byte order against what vt_chr_sync_from_prg writes.
Recommended next step: trace what $2010 + $2012-$2017 Lonely Island writes
during boot (it must set $2010 to a 16-color value for the garble to
involve the 4bpp path at all; if it leaves $2010=0, the bug is in the
PLAIN 2bpp memcpy path / tile-cache seam, which is simpler to inspect).


---

## STATUS / honest assessment (after extensive static tracing)

Static analysis against Furb + digests has RULED OUT a lot and is now at
diminishing returns:
  RULED OUT as the garble cause: register mangling (identity for submapper
  15), CHR bank composition (matches Furb term-for-term), CHR de-interleave
  formula (matches Furb bit-4 split), CHR reset defaults (match Furb byte
  for byte), and the COLCOMP=0 palette approach (matches Furb's 2C02 path).
  Also note: vt03_composite_to_gba is dead code; my first palette "finding"
  was retracted after tracing callers.

STILL OPEN (cannot be settled by reading alone):
  1. Which palette path is live when the garble shows -- COLCOMP=0 (NES
     palette, looks correct in approach) or COLCOMP=1 (VT rebuild path,
     ppu_vt.c 318-334, not disproven)? Needs to know what $2010/$3F80 the
     ROMs actually write.
  2. Is nes_palette correctly populated for VT carts (the $3F00 lo-write
     path, ppu_vt.c vt_palette_write_lo + ppu.s 4871)?
  3. The CHR copy-into-NES_VRAM vs Furb pointer-model seam (does the
     PocketNES tile cache get the bytes in the exact layout it expects?).
  4. 16x8 hi-colour sprite path is an explicit TODO (ppu_vt.c 359) -- not
     boot-critical but incomplete.

PATTERN: each static hypothesis has been generated then disproven. This
strongly indicates the bug needs OBSERVATION, not more reading: capture
what the ROM writes to $2010 / $3F00 / $3F80 / $2012-$2017 in its first
frames, and/or look at the actual framebuffer. The mGBA build path (earlier
this session) is the right tool; the "black screen" result under the
from-scratch build is itself a data point (black != the reported garble,
so either the standalone build differs from the real devkitPro build, or
black is an additional clue). RECOMMENDATION: build via the user's
devkitPro Docker command (the supported build), run under mGBA with a
logging hook on the VT register writes, and let the observed register
trace pick between open questions 1-3 instead of guessing.

## Earlier register-correctness audit (NOT the rendering bug)
These four were checked first; none is the garble cause but recorded for
completeness.

---

## Finding 1 -- $2000 D7 NMI polarity  => SPEC-VS-CODE (do not change yet)

Digest claim (DATASHEET_DIGEST.md S3, DATASHEET_DIGEST_VT02.md S3):
  VT02 p.28 and VT03 p.29 both literally state $2000 D7 (NMI EN) is
  0 = ENABLE NMI, 1 = DISABLE -- inverted vs NES (where bit7=1 enables).

What the code does (ppu.s):
  - A VT cart's $2000 write routes through vt_ppu_w_check_extended
    (ppu.s ~3899): offsets 0-7 go straight to the stock PocketNES
    PPU_W_dispatch -> ctrl0_W. Only $2008+ goes to vt_ppu_reg_write.
  - ctrl0_W (ppu.s ~3938) interprets D7 with NES polarity:
        tst r2,#0x80     @ NMI-enable bit changed?
        tstne r0,#0x80    @ ...and the NEW value's bit7 is SET?
        -> if so, and vblank flag on, trigger NMI (ctrl0_trigger_nmi).
    So PocketVT treats bit7=1 as "enable NMI" = NES semantics, the OPPOSITE
    of the literal VT datasheet.

Verdict: SPEC-VS-CODE. The datasheet wording is clear and appears on BOTH
chips independently, so it is not a scan artifact. BUT:
  - PocketVT runs real VT ROMs today and NMI-driven games are not visibly
    broken, which is strong evidence that real VT silicon/games use NES
    polarity in practice.
  - VT chips are deliberately NES-compatible (they run ported NES code);
    a truly inverted NMI bit would break every ported NES game's vblank
    handler, which would have been caught long ago.
  - Most likely the datasheet's "0=enable" is an erratum, OR the bit is
    described from the chip's internal active-low signal while the
    programmer-visible behaviour is NES-standard.
DO NOT flip the polarity based on the datasheet alone. Changing ctrl0_W
(or adding a VT-specific inversion) would risk breaking every currently-
working VT game. This needs a real test: a VT ROM whose NMI behaviour is
known, run both ways, to see which polarity actually renders correctly.
If anything, the lesson is the datasheet bit description is unreliable
here -- trust the working NES-compat path.

ACTION: none (leave code as-is). Reclassify the digest's [DIVERGE] note to
"documented-but-NES-compat-in-practice" so nobody else tries to "fix" it.

---

## Finding 2 -- $4100 nibble usage (PRG D7-D4 vs CHR D3-D0)   => OK

Digest claim: PRG banking must use $4100 D7-D4 (PA24-21); CHR uses
$4100 D3-D0 (VA24-21). Easy to get backwards.

What the code does (vt_regs.c):
  - PRG side, vt_get_phys_bank() ln 209-217, matches VT03 p.16/17 exactly:
        ps     = $410B & 0x07                    (PS[2:0])
        prgAND = (ps==7)?0xFF:(0x3F>>ps)          (BANK mask per PS case:
                 PS0->0x3F, PS1->0x1F ... PS7->0xFF -- matches the formula table)
        pa21   = ($4100 >> 4) & 0x0F              (HIGH nibble = PA24-21  CORRECT)
        prgOR  = (pq3 | (pa21<<8)) & ~prgAND      ($410A + PA21 extension)
  - PQ2EN ($410B b6) and COMR6 ($4105 b6) handled in vt_recompute_prg_banks
    (ln 228-245), including the slot $8<->$C swap on COMR6 -- matches the
    digest's CPU-map + exchange rule.
  - CHR side (ln 505): vt_chr_outer_4100 = val & 0x0F (LOW nibble = VA24-21
    CORRECT).
  - Code cites h_OneBus.cpp::syncPRG as its reference -> ported from a
    known-good OneBus impl, not guessed.

Verdict: OK. Both nibbles used correctly; PRG=high, CHR=low, exactly as the
datasheet specifies. No action.

## Finding 3 -- VT02 vs VT03 palette path   => OK (no stall) + caveat [OPEN]

Digest worry: a VT02 game writes only $3F00-$3F1F (one 6-bit byte, LUM[1:0]
+PHA[3:0], no $3F80 saturation bank). I feared PocketVT's two-byte VT03
assembler might stall waiting for a $3F80 half that never comes, or
mis-decode.

What the code does (ppu_vt.c, ppu.s):
  - Routing (ppu.s 4845-4901): $3F00-$3F1F -> vt_palette_write_lo;
    $3F80-$3F9F -> vt_palette_write_hi. Independent; lo does NOT wait for hi.
  - Compositor vt03_composite_to_gba() (ppu_vt.c 222):
        COLCOMP=1: idx = (hi<<6)|lo, full 12-bit LUT lookup   (VT03 path, OK)
        COLCOMP=0: return vt03_palette_lut[lo & 0x3F]          (fallback)
  - A VT02 game never writes hi, never sets COLCOMP, so it always takes the
    COLCOMP=0 path: lo & 0x3F indexed into the LUT.

Does the fallback render VT02 colour correctly?
  - LUT is indexed [SAT:4][LUM:4][HUE:4] (12 bits). lo & 0x3F = the low 6
    bits = entries 0-63 = the SAT=0, LUM=0..3, HUE=0..15 corner of the cube.
  - VT02's 6-bit colour = LUM[1:0] (bits 5:4) + PHA[3:0] (bits 3:0). That
    maps lo[5:4]->LUT LUM field, lo[3:0]->LUT HUE field, SAT=0.
  - So STRUCTURALLY the mapping is plausible: VT02 hue lands in HUE, VT02
    2-bit luminance lands in the low 4 LUM rows at zero saturation.

Verdict: OK that it does not stall or wait for a hi byte (my digest worry
was unfounded -- good). CAVEAT/[OPEN]: VT02's 2-bit luminance maps only to
the LUT's LOWEST 4 luminance rows (LUM 0-3 of 16) at SAT=0, so VT02 games
may render darker / lower-contrast than intended IF VT02's 2-bit luminance
is meant to span the full brightness range rather than the bottom quarter.
Only a real VT02 ROM can confirm whether the corner-of-cube mapping looks
right or needs a VT02-specific 64-entry LUM/PHA table that spreads the 4
luminance levels across the full range.

ACTION: none required for correctness-of-mechanism (it works, won't crash).
If a VT02 game looks too dark, add a dedicated 64-entry VT02 palette table
(LUM[1:0] spread across full brightness, PHA[3:0] as hue, SAT=0) and select
it when the cart is VT02 / COLCOMP never set. Low priority until a VT02 ROM
is on hand. PocketVT's primary target is VT03 anyway.

## Finding 4 -- $410F encryption-gate association   => OK (VT09 overload, resolved)

Digest flag: README says the encryption gate is "toggled by $410F writes,"
but VT02/VT03 datasheets define $410F as I/O port 2,3 output data (GPIO).
Suspected the encryption-at-$410F association was wrong.

What the code does (vt_regs.c):
  - $410F (VT_REG_SECURITY) write -> vt09_set_encryption(val == 0x00)
    (ln 569-572), gated behind #if VT09_ENCRYPTION. vt.reg[0x0F] resets to
    0xFF ("Security register reset value", ln 160).
  - Encryption itself is NOT a flat XOR-0xA1 (as the README loosely said).
    It is a per-submapper BIT-PERMUTATION in vt09_decode_opcode() (ln 356):
        mode 12: swap bits 6<->7 and 1<->2 (mask 0xC6)
        mode 13: swap bits 1<->4
        mode 14: swap bits 6<->7
    Only the opcode byte is permuted; operands are not. Committed on JMP.
    References Furbtendulator CPU_OneBus::Unscramble / CPU_VT32::GetOpcode.
  - encryption_active is set from the submapper (>=12), NOT from $410F;
    $410F only schedules enable/disable transitions for carts that use it.

Resolution: The digest's hypothesis was right -- $410F is GPIO on VT02/VT03,
and the encryption use is a LATER VT09-class OVERLOAD of the same address.
The code handles this correctly: it is VT09-specific (#if VT09_ENCRYPTION,
submapper>=12 gate), so VT02/VT03 carts that never enable encryption just
shadow the $410F write harmlessly. This also CORRECTS my earlier flat-XOR
disassembly attempts -- the real scheme is a bit-permutation, which is why
XOR-0xA1 produced garbage. The code already implements the right algorithm.

Minor caveat (not a bug): if a real VT02/VT03 game used $410F as actual
GPIO output, the code would read a $410F=$00 write as "disable encryption"
(harmless when encryption inactive) and would not drive the physical pins
(GBA has no equivalent anyway). No practical issue.

ACTION: none. Update the README's loose "XOR-0xA1 toggled by $410F"
wording to "per-submapper bit-permutation; $410F is the VT09 security-
register overload of the VT02/VT03 GPIO address" so the description matches
the (correct) code.

---

## Summary

| # | Finding | Verdict |
|---|---------|---------|
| 1 | $2000 D7 NMI polarity | SPEC-VS-CODE -- code uses NES polarity, datasheet says inverted; code is probably right, DO NOT change without a test ROM |
| 2 | $4100 nibble (PRG hi / CHR lo) | OK -- both nibbles correct, ported from h_OneBus |
| 3 | VT02 vs VT03 palette path | OK mechanism (no stall); caveat: VT02 maps to LUT's low-luminance corner, may render dark -- needs VT02 ROM to confirm |
| 4 | $410F encryption association | OK -- correct VT09 overload + bit-permutation (not flat XOR); README wording should be updated |

Big picture: the VT register/bank/encryption code is in noticeably good
shape -- it cross-references both the datasheets AND Furbtendulator, and the
two genuinely-suspicious findings (1 and 3) turned out to be "code is right,
datasheet/worry was the problem" rather than real bugs. The audit-first
approach was the correct call: NONE of the four flagged discrepancies is a
confirmed bug requiring a code change. The only doc fixes are cosmetic
(README NMI note, README encryption wording).

This also informs the VT02-vs-VT03 question: VT02 needs NO new core work to
NOT-crash (palette doesn't stall, banks are correct, opcodes are plain
6502). The only VT02-specific polish is an optional 64-entry palette table
if VT02 games render too dark -- and that's low priority since the project
targets VT03 and there's no VT02 test ROM on hand.
