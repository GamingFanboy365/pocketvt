# VT02 Datasheet Digest

Distilled engineering reference from the VRT VT02 datasheet scan
(`VT02 Data Sheet RevisionA5_ENG`, 48pp, rev A5 dated Sep 13 2005).
Printed page numbers match scan image numbers 1:1 (cover = page 1).

Companion to DATASHEET_DIGEST.md (VT03). VT02 is the EARLIER, lower-color
sibling of VT03 (VT02 = 2005/rev A5; VT03 = 2008/rev A6). The two chips
share almost the entire architecture; this file documents VT02 in full,
and notes both VT02-vs-VT03 and VT02-vs-NES differences inline.

Status legend:
  [MATCH-VT03]  identical to the VT03 datasheet (cross-ref that file)
  [VT02!=VT03]  VT02 differs from VT03
  [VT02 vs NES] difference from the NES (2A03 CPU / 2C02 PPU)
  [GAP]         documented behaviour PocketVT may not implement
  [?]           ambiguous / needs a second read or another source

NES comparisons are from general knowledge of the 2A03/2C02, NOT read off
the VT02 page; they are marked [VT02 vs NES] and should be treated as
informed context, not datasheet-sourced fact.

---

## 0. Quick reference: VT02 vs VT03 deltas (the whole list)

Everything in VT02 matches VT03 EXCEPT the following. These are all
downstream of one fact: VT02 is 4-color (2bpp) only; VT03 adds 16-color
(4bpp) and a richer palette.

| Area | VT02 | VT03 |
|------|------|------|
| Marketing | "Real 4 / Virtual 16 colors" | "Real 16 / Virtual 64 colors" |
| Revision/date | A5, Sep 2005 | A6, Oct 2008 |
| Background color | 4-color (4 sets) only | 4-color OR 16-color |
| Sprite color | 4-color only | 4-color OR 16-color |
| Sprite sizes | 8x8, 8x16 | 8x8, 8x16, 16x8, 16x16 |
| Palette colors | 25 | 25 or 121 |
| Palette format | 1 byte, 6-bit (LUM[1:0]+PHA[3:0]), $3F00-$3F1F only | 2 bytes, 12-bit (SAT+LUM+PHA), $3F00 + $3F80 |
| $2010 register | BKEXTEN(D4), SPEXTEN(D3) only | + COLCOMP(D7), SP16EN(D2), BK16EN(D1), PIX16EN(D0) |
| Character types | Type1, Type2 (both 4-color) | Type1-4 (3,4 are 16-color) |
| VA34 address bit | absent | present (OA4 in 16-color types) |
| Per-pixel color bits | 5 bits -> 25x6 SRAM | 5 bits (4-col) or 7 bits (16-col) -> 25x6 / 121x12 |
| $410D 16-bit-flash note | absent (added later) | present |
| Programming guide | 12 notes (no 16-color setup) | 15 notes (incl. $2010 16-color values) |

IDENTICAL between the two (do not re-implement separately): 6502 CPU/ISA,
program & video bank mapping (formulas, C1/C2/A4/A6/B1/B2/B3 tables, except
VT02's Table A3 has only Type1/Type2), sound (XOP1/XOP2 + DWS/PCM, all
registers), DMA ($4014/$4034), timer/IRQ, RS232, scrolling, sprite-pool
format, nametable/attribute indirection, pin set & multiplexing, the
$2000 NMI-inversion-vs-NES trap, and the "$410F is GPIO not encryption" /
"no VT extra opcodes" findings.

*** PocketVT impact of the deltas: a VT02 ROM must NOT use the VT03
4bpp/COLCOMP path or the 4096-entry HSL LUT. VT02 needs (a) a 64-entry
6-bit LUM/PHA palette path keyed on single $3F00-$3F1F writes (no $3F80),
(b) $2010 treated as only BKEXTEN/SPEXTEN (D7/D2/D1/D0 unused), (c) sprites
limited to 4-color 8x8/8x16. A chip-type flag (VT02 vs VT03) should gate
these. See sections 10, 3 ($2010), 7 (Table A2), and 9 for specifics. ***

---

## 1. Architecture overview (VT02 pp.1,3)

VT02 = "Real 4 colors or Virtual 16 colors" (cover). VT03 = "Real 16 /
Virtual 64". This color-depth gap is the headline difference.

System block (p.3) -- IDENTICAL to VT03:
- 6502 CPU; 2 KB internal program RAM; 2 KB internal video RAM (pattern
  vectors for 2 background pages).
- DMA (sprite + background); programmable timer; multiple IRQ control.
- Bank decoder for external memory up to 32 MB.
- One Bus Mode: 8-bit data bus (16 aux I/O pins) or 16-bit (8 aux I/O).
- TV NTSC/PAL; joystick; built-in RS232.
[MATCH-VT03] CPU, RAM, DMA, banking, OneBus, timer, IRQ, peripherals.

Graphics (p.3) -- where VT02 DIFFERS from VT03:
- 256x240, 64 sprites/frame. [MATCH-VT03]
- Background: 4 colors (4 color sets) ONLY.
  [VT02!=VT03] VT03 also has a 16-color background mode; VT02 does not.
- Sprites: 4 colors (4 color sets), sizes 8x8 or 8x16 ONLY.
  [VT02!=VT03] VT03 adds 16-color sprites and 16x8 / 16x16 sizes; VT02 has
  neither -- no 4bpp, no large sprites.
- Color palette: 25 colors.
  [VT02!=VT03] VT03 has 25 OR 121; VT02 caps at 25.
NET: VT02 is the 2bpp-only chip -- 4 colors/tile everywhere, 25-color
palette, no 16-color (4bpp) modes, no 16x8/16x16 sprites. The entire
"16-color / extension / 4bpp" machinery documented for VT03 (COLCOMP=1,
BK16EN/SP16EN, the "shift one bit left" rule, character Types 3/4) is
absent or unused on VT02.

Sound (p.3) -- IDENTICAL to VT03:
- 4 rhythm, 2 low-frequency, 2 noise channels; PCM or DWS DMA built in.
[MATCH-VT03] Same XOP1/XOP2 + DWS/PCM model expected (confirm on p.33).

### VT02 vs NES (2A03 CPU / 2C02 PPU) -- informed context
[VT02 vs NES] Similarities: both are 6502-class CPUs with a 2bpp,
4-color-per-tile PPU at 256x240, 64 sprites/frame (8/scanline limit),
and ~25 displayed colors. A VT02 in "old compatible mode" behaves much
like an NES.
[VT02 vs NES] Differences VT02 adds over NES:
  - OneBus banked addressing to 32 MB (NES relies on cartridge mappers
    with far smaller, mapper-specific spaces).
  - A PROGRAMMABLE palette (SAT/LUM/PHA-style writes) vs the NES's fixed
    analog-derived 54-entry master palette. VT02's 25 colors are chosen
    from a programmable space, not a hardwired NTSC palette.
  - Extra/different sound: NES APU = 2 pulse + triangle + noise + DMC.
    VT02 = 4 rhythm + 2 low-frequency + 2 noise + PCM/DWS DMA -- a
    different channel mix, not a superset of the NES APU.
  - A programmable timer + IRQ controller and a built-in RS232 port, which
    the stock NES lacks.
  - Indirect pattern-vector nametables (internal VRAM stores POINTERS to
    external pattern data) vs the NES's fixed pattern tables indexed by
    nametable byte.
  - Two-bank-deep video banking ($4100/$2018/$2012-7) reaching far more
    CHR than NES CHR-ROM/RAM banking.
These are the same VT-family extensions noted for VT03; VT02 has them too,
just without VT03's added color depth. (All [VT02 vs NES] points are from
general 2A03/2C02 knowledge, not the VT02 page.)

---

## 2. Register map -- Program unit ports (VT02 pp.23-28)

### $4100-$4104 (p.23) -- [MATCH-VT03], identical bit layout
    $4100 W : Program Bank1 / Video Bank2.
              D7..D4 = PA24..PA21 (program high); D3..D0 = VA24..VA21 (video high)
    $4101 W : Preload times of timer interrupt.
              D7 = TSYNEN (0 = count AD12 transitions, 1 = count HSYNC); D6..D0 = count
    $4102 W : write any value -> load $4101 into timer, start counting
    $4103 W : write any value -> disable timer interrupt
    $4104 W : write any value -> enable timer interrupt
(See VT03 digest S2 for full discussion; VT02 is byte-for-byte the same here.)

### $4105 W -- V/P Bank0 decode type, Internal Char VRAM (p.24) -- [MATCH-VT03]
    D7 = COMR7 (Video Bank0 decoder row set, Table C2)
    D6 = COMR6 (Program Bank0 decoder, with PQ2EN, Table C1)
    D5 = IVRCH (internal VRAM as char RAM: 0=disable, 1=enable as vector+char)
    D4..D0 = unused
Tables C1 (program decode, PQ2EN+COMR6+A[14:13] -> TPA20..TPA13) and
C2 (video decode, COMR7+AD[12:10] -> TVA17..TVA10) are CELL-FOR-CELL
IDENTICAL to VT03 p.24. See the VT03 digest (S2 Table C1, and S7 Table A6
which is the same matrix as C2) for the full grids -- not re-transcribed.
[MATCH-VT03] The entire silicon-level bank-decode is shared between the chips.

### $4106-$410A (p.25) -- [MATCH-VT03]
    $4106 W : H/V scroll selector. D0 = HV (0=Horizontal, 1=Vertical); D7..D1 unused
    $4107 W : Program Bank0 register0 = PQ07..PQ00
    $4108 W : Program Bank0 register1 = PQ17..PQ10
    $4109 W : Program Bank0 register2 = PQ27..PQ20
    $410A W : Program Bank0 register3 = PQ37..PQ30
Identical to VT03 p.25.

### $410B W -- Timer clock / PQ2EN / RS232 / bus tristate / Program Bank0 selector (p.26) -- [MATCH-VT03]
    D7 = TSYNEN (timer clock: 0=AD12, 1=HSYNC)
    D6 = PQ2EN  (Program Bank0 reg2 enable; the PQ2EN input to Table C1)
    D5 = RS232EN
    D4 = BUSTRI (0=bus normal, 1=tristate)
    D3 = FWEN   (0: $8000-$FFFF/$6000-$7FFF writes don't assert XRWB;
                 1: they do; FWEN=1 disables the "old program method")
    D2..D0 = PS2,PS1,PS0 (Program Bank0 selector)
### $410D W -- I/O port control (p.26) -- [MATCH-VT03]
    D7=IOP3EN D6=IOP3OEN D5=IOP2EN D4=IOP2OEN D3=IOP1EN D2=IOP1OEN D1=IOP0EN D0=IOP0OEN
    per port n: IOPnOEN = direction (0=in,1=out); IOPnEN = enable (0/1)
[VT02!=VT03] VT02 p.26 does NOT carry the "set $410D D3-D0 = $A for 16-bit
flash / external SRAM unavailable in 16-bit flash" footnotes that VT03 p.26
has. Those constraints were added in a later revision (VT03 is A6, VT02 is
A5). The register bits themselves are identical; only the documented
16-bit-flash guidance is absent here.

### $410E/$410F I/O data, RS232 timer, $4119 (p.27) -- [MATCH-VT03]
    $410E W : I/O port 0,1 output data = XVD7..XVD0 (D3-0 port0, D7-4 port1)
    $410F W : I/O port 2,3 output data
              D0=XRA10 D1=XAD10 D2=XAD11 D3=XAD12 (port2);
              D4=XRC D5=XRCB D6=XVOEB D7=XVRW (port3)
    $410E R : I/O port 0,1 input data (same XVD layout)
    $410F R : I/O port 2,3 input data (same XRA/XAD/XRC/XRCB/XVOEB/XVRW layout)
    $4114 W : Low byte of RS232 timer
    $4115 W : High byte of RS232 timer
              RS232T = ($4115<<8)|$4114; CK21M = 26.601712 MHz (PAL) /
              21.47727 MHz (NTSC); baud = CK21M/((RS232T+2)*2);
              PAL 9600 baud -> RS232T = 0x0567
    $4119 W : RS232 register. D5=B8EN (0=10-bit,1=11-bit frame), D0=T8B (TX bit8)

*** [MATCH-VT03 -> confirms encryption finding] $410F on VT02 is ALSO I/O
port 2,3 output data (GPIO), NOT an encryption control. The PocketVT
README's "$410F encryption gate" association is wrong on BOTH chips. ***
Note: RS232 register is already at $4119 in VT02 (rev A5, 2005). VT03's
revision history says RS232 moved from $4109 to $4119 in VT03 rev A2 -- so
both chips' current docs agree on $4119; only very old VT03 pre-A2 docs
used $4109.

## 3. Register map -- Graphic unit ports (VT02 pp.28-32)

### $4119 R / $411A / $411B (p.28) -- [MATCH-VT03]
    $4119 R : RS232 flags. D7=RIFLAG(rx done) D6=TIFLAG(tx done) D5=RINGF
              D4=XF5OR6(TV system selector) D3=XPORN D1=RERRF D0=R8B(rx bit8)
    $411A W : TX data of RS232
    $411B R : RX data of RS232

### $2000 W -- PPU control (p.28) -- mostly [MATCH-VT03], one sprite delta
    D7 = NMI EN  : 0 = ENABLE NMI, 1 = DISABLE NMI
    D6 = UNUSED  : (TEST pin control I/O)
    D5 = SP SIZE : 1 = BIG sprite (8x16), 0 = SMALL sprite (8x8)
    D4 = BK AD12 : background character ROM address XAD12
    D3 = SP AD12 : sprite character ROM address XAD12
    D2 = V W SEQ : video data update sequence (0=Horizontal, 1=Vertical)
    D1 = VCOOR6  : page select in vertical mode   (0=page0, 1=page1)
    D0 = HCOOR6  : page select in horizontal mode (0=page0, 1=page1)
[VT02!=VT03] D5 SP SIZE: VT02 = "BIG=8x16 / SMALL=8x8" only. VT03 said
"8x16 or 16x16 / 8x8 or 16x8" -- VT02 has no 16-wide sprites.
*** [VT02 vs NES + MATCH-VT03] D7 NMI EN is INVERTED vs NES: 0=enable,
1=disable (NES $2000 bit7=1 enables NMI). Same trap as VT03. PocketVT must
not forward VT $2000 D7 straight into NES PPUCTRL NMI logic. ***

### $2001-$2006 (p.29) -- [MATCH-VT03]
    $2001 W : display control.
              D4=SP EN(sprite en) D3=BK EN(bg en) D2=SP INI(0=righter,1=lefter)
              D1=BK INI(0=righter,1=lefter) D0=B/W(0=Color,1=B/W); D7-5 unused
    $2002 R : status. D7=VSYN(blanking/vblank) D6=B,S 0V(priority/sprite0)
              D5=OVLOAD(sprite overload); D4-0 unused. Reading resets the
              $2005/$2006 write-toggle without changing stored contents.
    $2003 W : sprite pool counter initial address (OAMADDR analog)
    $2004 W : sprite pool data; writes DRAM, increments counter (OAMDATA analog)
    $2005 W : H/V display origin coordinate, 2-byte (1st=H, 2nd=V; PPUSCROLL analog)
    $2006 W : initial address of Video RAM/ROM, 2-byte (PPUADDR analog; detail p.30)
All identical to VT03 (S3, pp.29-30 there). [VT02 vs NES] $2000-$2007 are
the NES PPU register block, same addresses/roles, with VT extensions noted.

### $2006 (2-byte) / $2007 (p.30) -- [MATCH-VT03]
    $2006 high (1st) byte: D5=XRC D4=AD12 D3=AD11 D2=AD10 D1=AD9 D0=AD8 (D7,D6 unused*)
    $2006 low  (2nd) byte: AD7..AD0
    $2007 R/W: video RAM/ROM data port; first read after addr set is stale
               (buffered), then auto-increment. Same access example as VT03.
  *NOTE: VT02's $2006 high byte shows XRC at D5 and does NOT label a VA34
   bit at D6 (VT03 had D6=VA34). Consistent with VT02's smaller address
   needs. XRC = internal($3F/VRAM) vs external(ROM) selector (same as VT03).

### $2010 W -- Background/Sprite address EXTENSION enable (p.30)  *** MAJOR VT02!=VT03 ***
    D7,D6,D5 = UNUSED
    D4 = BKEXTEN : background address extension enable (0=disable, 1=enable)
    D3 = SPEXTEN : sprite address extension enable (0=disable, 1=enable)
    D2,D1,D0 = UNUSED
*** [VT02!=VT03] VT02's $2010 has ONLY BKEXTEN (D4) and SPEXTEN (D3).
It has NO COLCOMP (D7), NO SP16EN (D2), NO BK16EN (D1), NO PIX16EN (D0).
VT03's $2010 had all six of those. This is the register-level proof that
VT02 has no 16-color / 4bpp / "new color mapping" modes at all. ***
Consequences for VT02:
  - No COLCOMP -> no 121-color / full-12-bit SAT/LUM/PHA mode. VT02 is
    always the 25-color (compatible) palette.
  - No SP16EN/BK16EN/PIX16EN -> no 16-color tiles, no 4bpp doubling, no
    "shift one bit left" rule, no character Types 3/4. Only Type1 (4-color,
    no ext) and Type2 (4-color, ext enabled via BKEXTEN/SPEXTEN) exist.
  - BKEXTEN/SPEXTEN still select extension addressing (EVA insertion), but
    always in 4-color mode.
*** [FOR PocketVT -- ACTIONABLE] PocketVT's COLCOMP gate (vt_reg_2010 &
0x80) and the entire 4bpp path are VT03-ONLY. For a VT02 ROM, $2010 D7 is
UNUSED -- it must never trigger 16-color mode. If PocketVT keys 4bpp off
$2010 D7 without checking the chip type, a VT02 ROM that happens to set D7
(or where D7 floats) could wrongly flip into 16-color rendering. The
emulator needs a VT02-vs-VT03 mode flag, or to treat $2010 D7/D2/D1/D0 as
unused when running VT02 content. ***

### $2011 W -- LCD/video options (p.31) -- [MATCH-VT03]
    D7,D6 = UNUSED
    D5=VLS1 D4=VLS0 : LCD line count (00=240, 01=160, 10=120, 11=80)
    D3 = EVRAMEN : enable internal VRAM (0=Enable, 1=Disable/don't use)
    D2 = PIX2EN  : B/W 2-color mode (0=Disable, 1=Enable)
    D1 = VDAEN   : composited video DA enable (0=Enable, 1=Disable)
    D0 = EVA12S  : EVA12 selector (0=reg.BKPAGE, 1=HV)

### $2012-$2017 W -- Video Bank0 registers 0..5 (p.31) -- [MATCH-VT03]
    $2012=RV07..RV00  $2013=RV17..RV10  $2014=RV27..RV20
    $2015=RV37..RV30  $2016=RV47..RV40  $2017=RV57..RV50
CHR bank latches feeding the C2/A6 video decode. Same as VT03 $2012-$2017.
(VT02 reaches these on p.31 vs VT03 p.32 -- layout shifted because VT02's
$2010 is smaller.)

### $2018 / $2019 / $201A / gun ports (p.32) -- [MATCH-VT03]
    $2018 W : Video Bank1 reg / BKPAGE / Video RW Bank.
              D6=VA20 D5=VA19 D4=VA18; D3=BKPAGE (=EVA12 when EVA12S=0);
              D2=VRWB2 D1=VRWB1 D0=VRWB0 (video RW bank for $2007); D7 unused
    $2019 W : reset gun port -- write any value clears gun1/gun2 X,Y
    $201A W : Video Bank0 reg6 / selector. D7=RV67..D3=RV63; D2=VB0S2 D1=VB0S1 D0=VB0S0
    $201C R : X coord of gun port 1     $201D R : Y coord of gun port 1
    $201E R : X coord of gun port 2     $201F R : Y coord of gun port 2
All identical to VT03 (S3, p.33 there).
[GAP] Light-gun ports ($201C-$201F): no GBA equivalent; PocketVT likely
doesn't implement. Harmless for non-gun games.

## 4. Sound generator (VT02 p.33)

### Full sound register map (p.33) -- [MATCH-VT03], identical
XOP1 ($4000-$4013) and XOP2 ($4020-$402F) banks, plus $4030/$4031, are
CELL-FOR-CELL IDENTICAL to VT03 (VT03 spread this over pp.34-35; VT02 fits
it on one page). Summary:
    XOP1: $4000-$4003 RHYTHM A, $4004-$4007 RHYTHM B, $4008/$400A/$400B
          ENVELOP, $400C/$400E/$400F NOISE,
          $4010-$4013 DWS DMA (Amplitude DIRQ/DREP/SD3-0; Initial Amplitude
          IA6-0; Start addr SA13-6; Length DL11-4).
    XOP2: $4020-$402F = same channels at +$20 (no DWS DMA).
    $4030 W: DWS/PCM selector + DA control (D3=DP, D2=DA2, D1=DA1=~A15, D0=~A14)
    $4031 W: PCM data (PCM7-0)
See VT03 digest S4 for the full per-bit field names -- not re-transcribed.
[MATCH-VT03] VT02 and VT03 share the ENTIRE sound architecture, including
DWS/PCM (NOT ADPCM).
[VT02 vs NES] This channel set (4 rhythm + 2 low-freq + 2 noise + DWS/PCM
DMA across two XOP banks) is NOT the NES APU (2 pulse + triangle + noise +
DMC). Different synthesis model, same $4000-$4017 address neighborhood.

## 5. Parameter description (VT02 pp.34-35)

### Sound parameter definitions (p.34) -- [MATCH-VT03]
Identical to VT03 (S5, p.35 there): duty table (xDY2,xDY1 -> 1/8,1/4,1/2,
3/4); xSC (one-shot/continuous); xIW + xWI[3:0] (decay 4.16ms*xWI or hold
level); xAT/xST[2:0] (pitch-band, mod time 8.33ms*xST); xSG/xAD[2:0]
(F(n+1)=Fn*(1+-2^-m)); xFT[A:0] (freq=111860/xFT, min 0x08); 3EN/3EL[6:0]
(beat length 1 = BLCK1*3EL); 4NS (noise band).
The xSL[4:0] sound-duration table (BCLK2=120Hz/100Hz, idx 00-1F) is
IDENTICAL to VT03 -- same ms values. BCLK2 set by $4017. Not re-transcribed;
see VT03 digest S5.

### DWS/PCM parameters + SD rate table (p.35) -- [MATCH-VT03]
DIRQ (DWS IRQ en), DREP (repeat/loop), DP (0=DWS,1=PCM), DA2 (XOP2 DA,
default off), DA1 (XOP1 DA, default on), ~A15/~A14 (DMA addr complements),
IA[6:0] (initial amplitude), SA[13:6] (start addr, #11xxxxxxxx000000,
64-byte aligned), DL[11:4] (length, 16-byte units), PCM[7:0] (CPU PCM).
SD[3:0] slope-decoder SAMPLE RATE table -- identical to VT03:

| SD | F | E | D | C | B | A | 9 | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|----|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Hz | 33K | 25K | 21K | 17K | 14K | 13K | 11K | 9K | 8.4K | 7.9K | 7K | 6.2K | 5.5K | 5.3K | 4.7K | 4.2K |

*** [MATCH-VT03 -> confirms finding] VT02 sample audio is also DWS (slope
decoder) / PCM, rate-selected by SD[3:0] -- NOT IMA-ADPCM. The VT369 ADPCM
step table is for neither VT02 nor VT03. ***
Two PCM paths (same as VT03): CPU writes $4031, or DMA via $4010/$4012/$4013.

## 6. Miscellaneous address ports (VT02 pp.36-38)

### $4014 / $4034 / $4015 (p.36) -- [MATCH-VT03]
    $4014 W : high byte of DMA source addr; writing it STARTS the DMA (OAMDMA analog)
    $4034 W : DMA settings. D7-4 = source addr[7:4]; D3-1 = max length
              (000=256, 100=16, 101=32, 110=64, 111=128 bytes); D0 = SEL47
              (0=sprite DMA -> updates $2004, 1=video DMA -> updates $2007).
              64-byte mode: source low byte must be 00/40/80/C0.
    $4015 W : enable XOP1 channels + DWS IRQ. D4=DWS/PCM en, D3=XOP1 Noise,
              D2=XOP1 Envelope, D1=XOP1 Rhythm B, D0=XOP1 Rhythm A (0=stop,1=start)
Identical to VT03 p.37.

### $4015 R / $4035 W / $4035 R (p.37) -- [MATCH-VT03]
    $4015 R : XOP1 flags. D7=DWS/PCM IRQ flag, D6=Clock IRQ flag (timer),
              D4=DWS/PCM status, D3=Noise, D2=Envelope, D1=RhythmB, D0=RhythmA
              (status bits: 0=end, 1=during)
    $4035 W : enable XOP2 channels. D3=Noise D2=Envelope D1=RhythmB D0=RhythmA
    $4035 R : XOP2 flags. D3=Noise D2=Envelope D1=RhythmB D0=RhythmA status
Identical to VT03 p.38. ($4015=XOP1, $4035=XOP2; D6 of $4015 R = timer IRQ.)

### $4016 / $4017 (p.38) -- [MATCH-VT03]
    $4016 W : set output pins XQ2,XQ1,XQ0 (controller strobe); D7-3 unused
    $4016 R : peripheral data. D2=Microphone D1=Floppy D0=Major joystick (joy1)
    $4017 W : beat-clock + clock IRQ. D7=BLCK1/BLCK2 select
              (0->250/120Hz, 1->200/100Hz); D6=Clock IRQ enable (0=enable 60Hz,1=disable)
    $4017 R : peripheral data. D4-1=Floppy disk lines, D0=Second joystick (joy2)
Identical to VT03 p.39. [VT02 vs NES] joystick read at $4016 D0 / $4017 D0
is NES-style controller layout.

## 7. Video memory bank mapping (VT02 pp.10-14)

### Banking model & address formulas (pp.10-11) -- [MATCH-VT03]
The 3-level video banking (Bank2 VA[24:21]=$4100; Bank1 VA[20:18]=$2018;
Bank0 VA[17:10]=$2012-7/$201A), the PPU address map ($0000=$2016&0xFE,
$0800=$2017&0xFE, $1400-1FFF=$2012-$2015, $2000+ nametables, $3F00 palette),
the normal/extension-mode address formulas, the $4105&0x80 (COMR7) pattern
exchange, and the EVA table are ALL identical to VT03 pp.10-11. Extension
mode collapses Bank1 (same as VT03). See VT03 digest S7 for the full
formulas and EVA table -- not re-transcribed.

### Table A2 -- character types (p.13)  *** VT02!=VT03: only TWO types ***
    Type1 : extension addr DISABLE, 4 colors/pixel (2bpp)
    Type2 : extension addr ENABLE,  4 colors/pixel (2bpp)
[VT02!=VT03] VT03 had Type1-4, where Type3/Type4 were the 16-color (4bpp)
variants. VT02 has NO Type3/Type4 -- no 16-color mode (matches the $2010
finding: no SP16EN/BK16EN/PIX16EN).

### Table A3 -- OA[24:0] pin assignment, VT02 (p.13)
Only Type1/Type2 columns, and NO VA34 (VT02 lacks the VA34 bit; OA4 = VA4
in both types, unlike VT03 where Type3/4 put VA34 at OA4):

| OA | Type1 | Type2 |
|----|-------|-------|
| 24 | VA24 | VA24 |
| 23 | VA23 | VA23 |
| 22 | VA22 | VA22 |
| 21 | VA21 | VA21 |
| 20 | VA20 | VA17 |
| 19 | VA19 | VA16 |
| 18 | VA18 | VA15 |
| 17 | VA17 | VA14 |
| 16 | VA16 | VA13 |
| 15 | VA15 | VA12 |
| 14 | VA14 | VA11 |
| 13 | VA13 | VA10 |
| 12 | VA12 | EVA12 |
| 11 | VA11 | EVA11 |
| 10 | VA10 | EVA10 |
| 9 | VA9 | VA9 |
| 8 | VA8 | VA8 |
| 7 | VA7 | VA7 |
| 6 | VA6 | VA6 |
| 5 | VA5 | VA5 |
| 4 | VA4 | VA4 |
| 3 | VA3 | VA3 |
| 2 | VA2 | VA2 |
| 1 | VA1 | VA1 |
| 0 | VA0 | VA0 |

Type1 = straight VA24..VA0. Type2 = extension: high VA bits shift down,
EVA12..10 inserted at OA12..OA10. (VT02's Type1/Type2 == VT03's Type1/Type2
exactly; VT02 simply omits VT03's Type3/Type4.)
Sources (p.13 text): VA[9:0] <- $2006; VA[17:10] <- Table A4; VA[20:18] <-
$2018(D6:4); VA[24:21] <- $4100(D3:0); EVA[12:10] <- Table A5. (No VA34.)
[GAP] PocketVT models Type1 (and Type2 if extension handled). With no
Type3/Type4, the VT03 4bpp address machinery is simply unused for VT02.

### Tables A4/A5/A6 (p.14) -- [MATCH-VT03]
VA[17:10] vs VB0S (A4), EVA[12:10] sources (A5), and TVA[17:10] by
COMR7+AD[12:10] (A6 = the C2 decode) are identical to VT03 p.14. See VT03
digest S7 for the full grids.

## 8. Program memory bank mapping (VT02 pp.15-19)

### [MATCH-VT03] -- entire PRG addressing identical
The 2-level program banking (Bank1 PA[24:21]=$4100 D7-4; Bank0 PA[20:13]=
$4107-$410A via C1/B3 decode), the CPU memory map ($0000-1FFF RAM, $2000-
$5FFF IO, $6000-$7FFF ext RAM, $8000+ banked, $E000 fixed 0xFF), the
$410B/PS[2:0] bank formula, the $4105&0x40 (COMR6) $8000/$C000 exchange,
Table B1 (OA=PA24-13 + A12-0), Table B2 (PA[20:13] by PS[2:0]), and Table
B3 (TPA decode, p.19 -- identical to the C1 matrix) are ALL identical to
VT03 pp.15-19. PRG addressing is not color-depth dependent, so VT02 == VT03
here. See VT03 digest S8 for full formulas/tables -- not re-transcribed.

## 9. Background pattern & internal video RAM (VT02 pp.19-21)

### Tile/nametable model (p.19) -- [MATCH-VT03 structure]
256x240 = 32x30 8x8 tiles/page; internal VRAM stores pattern VECTORS
(pointers to external pattern data); 960 vector bytes + 64 attribute bytes
per 1KB page; four adjacent patterns share the same 3rd/4th color address
(NES-attribute-style). Same indirect-pointer model as VT03/divergent from
NES fixed pattern tables.

### Per-pixel color model (p.19)  *** VT02!=VT03: 4-color ONLY ***
VT02: each pixel color = **5 bits (4-color mode only)** indexing the
**25x6 SRAM**:
    bits 1,2 = internal color of pattern (3 describable colors;
               (1,2)=(0,0) => TRANSPARENT)
    bits 3,4 = recolor whole pattern (4 selectable color sets)
    bit  5   = 1 -> sprite colors, 0 -> background colors
[VT02!=VT03] VT03 also had a 7-bit/16-color path (bits 6,7) pointing to a
121x12 SRAM. VT02 has ONLY the 5-bit/4-color path and only the 25x6 SRAM.
No 6,7 color bits, no 16-color SRAM.
[VT02 vs NES] This 2-bit-pattern + 2-bit-attribute + sprite/bg-select model
is essentially the NES color-index scheme (2bpp pattern + attribute palette
select), with the same transparent-on-(0,0) rule. The difference is the
final palette entry is a programmable LUM/PHA value (p.22), not a fixed
NES master-palette index.

### Sprite pool & scrolling (p.21)
Two-page scrolling ($4106 D0 axis select; H: left $2000-$23BF + right
$2400; V: top $2000-$23FF + bottom $2800) -- [MATCH-VT03].
Sprite pool: 256 bytes, 64 sprites, <=8/row, 4 bytes each:
    byte0 = vertical coordinate (Y)
    byte1 = 8-bit vector (pointer to sprite pattern in external video mem)
    byte2 = status; byte3 = horizontal coordinate (X)
Status byte (byte2) -- [MATCH-VT03]:
    D7 = mirror X axis (vertical flip),  0=normal
    D6 = mirror Y axis (horizontal flip),0=normal
    D5 = 1: background covers sprite, 0: sprite covers background
    D4..D2 = SPEVA2..SPEVA0 (sprite extension vector addr)
    D1..D0 = SP4, SP3 (color set of sprite; like BG[4:3])

### Sprite color & size (p.21)  *** VT02!=VT03: only 2 options ***
VT02 allows ONLY: 8x16 in 4-color, or 8x8 in 4-color.
[VT02!=VT03] VT03 had six combos (incl. 16-color and 16x8/16x16). VT02 =
4-color only, 8x8 or 8x16 only. No 16-color sprites, no 16-wide sprites.
[?] The p.21 figure labels "32 bytes for a 8x8 4color sprite" with two
color-address bit planes. For 2bpp an 8x8 tile is normally 16 bytes (2
planes x 8); the "32 bytes" label is unusual (possibly a doc carryover
from VT03's 8x8-16color=32B figure, or counting an 8x16). Treat the exact
byte count as [?]; the firm fact is VT02 sprites are 4-color, 8x8/8x16 only.
Bit5 = bg/sprite selector, Bit4/Bit3 = SP color set (same as color model).

## 10. Colour palette (VT02 p.22)

### Palette format (p.22)  *** MAJOR VT02!=VT03 -- much simpler ***
Address $3F00-$3F1F. Each colour = **6 bits (D5-D0)**, in a SINGLE byte.
There is NO $3F80 high bank on VT02. The 6 bits split as:
    D5-D4 = Luminance[1:0]   (2 bits)
    D3-D0 = Phase[3:0]       (4 bits)
So a VT02 colour = Luminance[1:0] + Phase[3:0] = 6 bits. NO saturation.
Layout:
    $3F00-$3F0F : Background 4 colours
    $3F10-$3F1F : Sprite 4 colours ("colors' palette")

*** [VT02!=VT03] VT03's palette is two bytes (SAT[3:0]+LUM[3:0]+PHA[3:0] =
12 bits, $3F00 low + $3F80 high banks, 4096-entry LUT, up to 121 colours).
VT02 is one byte (LUM[1:0]+PHA[3:0] = 6 bits, $3F00-$3F1F only, 25 colours,
no saturation, no high bank). These are DIFFERENT palette formats. ***

*** [FOR PocketVT -- ACTIONABLE] The VT03 4096-entry HSL LUT
(vt03_palette_lut.h) and the lo/hi ($3F00 / $3F80) palette-write split do
NOT apply to VT02. A VT02 palette write is a single 6-bit value at
$3F00-$3F1F; there is no $3F80 write to pair it with. PocketVT needs a
VT02 palette path: index a 64-entry (6-bit) LUM/PHA table, not the 4096
SAT/LUM/PHA table. If PocketVT routes VT02 palette writes through the VT03
two-byte assembler, colours will be wrong (it would wait for a $3F80 half
that never comes, or mis-decode the single byte). ***

[VT02 vs NES] Like NES, VT02 uses $3F00-$3F1F with 4-color background +
4-color sprite sub-palettes and ~25 displayed colours. But the byte
ENCODING differs: NES palette entries are indices into a fixed ~54-entry
analog master palette; VT02 entries are a programmable 6-bit LUM/PHA value.
Same address range and 25-color feel, different colour derivation.

## 11. Timing waveforms (VT02 pp.39-41)

### Program-unit timing (p.39) -- [MATCH-VT03]
Master clock CK21M: Fpal = 26.601712 MHz, Fntsc = 21.47727 MHz.
AC characteristics: TA = 0..70 C, VCC = 3.0..3.6 V, GND = 0 V.

| Symbol | Parameter | Min | Max | Unit |
|--------|-----------|-----|-----|------|
| Tcyc | Program cycle time | 70 | 450 | ns |
| Tph | Cycle high pulse width | 240 | 300 | ns |
| Tpl | Cycle low pulse width | 100 | 150 | ns |
| Tah | Program address hold | 10 | - | ns |
| Tdh | Program data hold | 10 | - | ns |
| Trds | Program read data setup | 10 | - | ns |
| Twds | Program write data setup | 10 | - | ns |

Identical values to VT03 (S11, p.40 there).

### Graphic-unit timing (p.40) -- [MATCH-VT03]
Diagram only (no numeric table). Signals: XVA12~0, XRC, XVD7~0, VOE(NTSC),
VRW; Fosc = CK21M period. Read params Tvph/Tvpl/Tvrcy/Tvrad/Tvrah/Tvrds/
Tvrdh; write params Tvwpl/Tvwas/Tvwah/Tvwds/Tvwdh. Same as VT03 p.41.
(Note: VT02 fits timing into pp.39-40; the TOC range 39-41 is generous.)
[Reference only -- electrical timing, not actionable for the GBA port.]

## 12. Pin description & multiplexing (VT02 pp.4,6,7)

### [MATCH-VT03] -- pin set and multiplexing identical
The VT02 pin description (p.6) and pin-optional/multiplexing table (p.7)
are identical to VT03 (pp.6-7). Same signals: XA/XD program buses, XVA/XVD/
XAD video buses, XRW/XROMCS/XFCSB/X67CSB chip selects, XVRW/XVOEB/XRC/XRCB
(the $410F GPIO pins), XQ[2:0]/XCUP46-47 (I/O + video-extension address),
XCK21M/XCK21B crystal, X4016/X4017 joystick inputs, XONEBUS (one-bus
enable), XD16BUSB (16-bit data bus selector), XPORN/XF5OR6 (NTSC/PAL
select). Same pull-up (PH) conventions. See VT03 digest S12 for the full
pin table and the multiplexing tables (IOPnEN / XONEBUS / XJOYSELB /
RS232EN dependent) -- not re-transcribed.

Confirmations this gives VT02:
- $410F GPIO names (XVRW/XVOEB/XRC/XRCB) are physical pins on VT02 too ->
  reinforces that $410F is GPIO, not encryption, on BOTH chips.
- XD16BUSB is the 16-bit-video-bus hardware pin (V16BEN), present on VT02.
- XQ0/XQ1/XQ2/XCUP47 act as VIDEO ROM A10-A13 (the EVA extension address)
  when not in OneBus/RS232 mode -- same as VT03.
[VT02 vs NES] The dedicated joystick lines (X4016/X4017 -> JOYAM/JOYBM/
JOYUPA/JOYST/JOYSE/JOYDNA/JOYLFA/JOYRTA when XJOYSELB=0) give VT02 a
built-in controller decode the bare NES lacks (NES does serial shift-
register controllers externally).

## 13. Programming guide (VT02 p.42)

Twelve notes, nearly identical to VT03's guide (VT03 p.43) but WITHOUT the
two 16-color-setup notes (VT03 #5/#7), since VT02 has no 16-color modes.

1. Avoid spurious IRQ: first set $4017 = $C0 or $40 (disable clock IRQ).
2. If the program does NOT set the new address ports, OLD COMPATIBLE
   (NES-like) mode is the default.
3. Single-bus boot: program initial address A24-A0 = $007FFFC; video
   initial = $0000XXX. $4102-$410A control decoder address ports. $4109
   grows the program bank register from 2 to 3 banks (must specify $4109).
4. Background extension addr enable: BG4/BG3 become a 10-bit character
   vector; char size 16x16; BG color-set function DISABLED, (BG4,BG3)
   fixed 00. (This is VT02's ONLY $2010-extension note -- no 16-color
   variants, unlike VT03's #5/#7.)
5. Gun ports: read in NMI ISR. $201C/$201D = gun1 X/Y, $201E/$201F =
   gun2 X/Y; write $2019 to reset.
6. PCM only outputs to XOP2. Set $4030 = $18 (enable channel-2 DA + PCM).
   $4031 -> XOP2 DA directly. PCM DMA via $4010/$4012/$4013. Max 4081 bytes;
   if longer, enable PCM IRQ, set unit-waveform $4010, update $4012+bank,
   restart via $4015 = $10 in ISR. PCM DMA exclusive with DWS.
7. Video DMA examples: $4034=$58,$4014=$02 -> $2004 from $0250-$025F (16B
   sprite); $4034=$AD,$4014=$03 -> $2007 from $03A0-$03BF (32B video);
   $4034=$0D,$4014=$03 -> $2007 from $0300-$033F (64B video).
8. Extra chip + XRWB: configure $410B; FWEN high disables old program method.
9. Don't DMA-copy to color palette in NTSC (PAL is fine). Read $4119(D3,D4)
   to detect NTSC/PAL; if NTSC, shift palette data one byte.
10. Old gun games access $3F20: initialise $3F20 = $2D in the menu program.
11. PCM data must be a multiple of 64 bytes. $4013=$FF -> plays 4081 bytes
    ($000-$FF0); $FF1-$FFF not fetchable by PCM DMA.
12. RS232: set $410B(D5)=1 first; TXDP outputs via XCUP47 -- set XCUP47 as
    TXDP first to avoid $4017-pin conflicts.

[VT02!=VT03] The ONLY substantive difference from VT03's guide is the
absence of VT03's notes #5 ("Background 16 colors: $2010=$82/$92") and #7
("Sprite 16 colors: $2010=$84/$8C/$85/$8D") -- VT02 has no 16-color modes
to configure. The renumbering above reflects VT02's 12 notes; VT03 had 15.
Everything else (IRQ, default-compatible-mode, boot addresses, DMA combos,
NTSC palette caveat, PCM/RS232) matches VT03.

## 14. CPU instruction table (VT02 pp.43-48)

### Opcode matrix (p.48) -- [MATCH-VT03] stock 6502, same blanks
VT02's opcode matrix is the stock NMOS 6502 set, identical fill pattern to
VT03. Columns 3/7/B/F and the cells 0x5A/0x7A/0xC2/0xD2/0x3C/0xA7/0xBF are
ALL BLANK -- no TAD/TDA/PHX/PHY/PLX/PLY/ADX/LDAXD/LDAD.
*** [MATCH-VT03 -> confirms FINDINGS_vt_opcodes.md on BOTH chips] Neither
VT02 nor VT03 has the "VT extra opcodes." They belong to the VT369-era
sound coprocessor only. PocketVT must not patch them onto the main CPU. ***
[VT02 vs NES] Same 6502 ISA as the NES's 2A03 (minus the 2A03's missing
decimal mode -- not separately checked here). Addressing-mode footnotes
list imm/abs/zpg/acc/imp/abx/aby/zpx/zpy/abi/rla/inx/iny and also "ina"
(abs indirect) and "inz" (zero-page indirect), but no matrix cell uses the
indirect-extra modes -- the grid is plain 6502.
