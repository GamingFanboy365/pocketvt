# VT02 / VT03 Datasheet Digest

A distilled engineering reference built by reading the VRT datasheet scans
(`VT02 Data Sheet RevisionA5_ENG`, 48pp; `VT03 Data Sheet RevisionA6_ENG`,
51pp, rev A6 dated Oct 13 2008). Page numbers below are the *printed* page
numbers, which match the scan image numbers 1:1 (cover = page 1).

Purpose: a durable on-disk reference so the datasheets don't have to be
re-read image-by-image, and a place to record where PocketVT's current
assumptions match or diverge from documented hardware.

Status legend for discrepancy notes:
  [MATCH]  PocketVT agrees with the datasheet
  [DIVERGE] PocketVT assumes something the datasheet contradicts
  [GAP]    documented behaviour PocketVT does not implement
  [?]      datasheet ambiguous / needs a second read or another source

---

## 1. Architecture overview (VT03 pp.1,3,5)

### Revision history notes (p.2) -- inter-revision register changes
The datasheet is rev A6 (Oct 2008). Several registers MOVED or were
corrected across revisions, which explains why other emulators/older docs
may map them differently (a likely source of discrepancies):
- RS232 register address moved from $4109 to $4119 (rev A2). $4109 is now
  purely Program Bank0 register2 (PQ27-PQ20); RS232 reg is $4119.
- $410E / $410F output assignments revised to XVRW/XVOEB/XRCB (rev A2) --
  confirms the current GPIO meaning of $410F (not encryption).
- $410B D3 = FWEN added in rev A2 (flash-write enable; high => old program
  method inactive).
- $2014/$2015 RV bit assignments CORRECTED in rev A2: $2014 = RV27-RV20
  (not RV07-RV20), $2015 = RV37-RV30 (not RV07-RV30). The digest uses the
  corrected A6 values. (An emulator coded from a pre-A2 doc would have the
  wrong RV mapping for $2014/$2015.)
- $4119 read: TIFLAG->D5 changed to RINGF->D5 (rev A2 status-bit fix).
- rev A4/A5/A6: reformatted, revised CPU instruction table, added abs-max
  rating/package/pinout, and the 16-bit-flash rules ($410D=$A, no ext SRAM).
[FOR PocketVT] If any register association looks off vs another VT
reference, suspect a pre-A2 doc: check $4109-vs-$4119 and the $2014/$2015
RV bits first.

- Single **6502** CPU. One core only. The audio block ("PSG & Voice
  Processor" in the block diagram, p.5) is fixed-function, fed by the CPU
  and by DMA -- NOT a second programmable CPU.
- 2 KB internal **program RAM** (zero page, stack, CPU scratch).
- 2 KB internal **video RAM** (stores pattern vectors for 2 pages of
  background).
- DMA for sprite and background.
- Programmable timer -> IRQ controller (multiple IRQ sources).
- Bank decoder for external memory up to **32 MB**.
- TV signal output: NTSC / PAL composite.
- **One Bus Mode**: program bus and video bus combined into one external
  bus. Two sub-modes:
    - 8-bit data bus  -> 16 auxiliary I/O pins
    - 16-bit data bus -> 8 auxiliary I/O pins  (this is the `V16BEN`
      16-bit-video-bus mode PocketVT lists as unimplemented)
- Peripherals: joystick, built-in RS232 serial port.

### One Bus System (p.9)
VT03 combines the program and video address buses into ONE external bus.
A single physical external memory serves as both program and video memory;
the program-memory bank and video-memory bank are configured separately,
so the programmer must lay out external memory carefully. OA[24:0] (25
bits) address up to 32 MB. Boot defaults: program initial address
A24-A0 = $007FFFC; video initial address = $0000XXX. Enabled by XONEBUS
pin high. The two following chapters (video bank mapping pp.10-14, program
bank mapping pp.15-19) give the OA[24:0] computation in each mode.

Graphics (p.3 feature list):
- 256x240 resolution, **64 sprites per frame**.
- Background: 16 colours (4 colour-sets) OR 4 colours (4 colour-sets).
- Sprites:
    - 16-colour sprites: 8x8 or 8x16
    - 4-colour sprites: 8x8, 8x16, 16x8, or 16x16
- Colour palette: **25 or 121 colours** ("Real 16 / Virtual 64" on cover).
  NOTE: reconcile this with PocketVT's 4096-entry HSL LUT
  (`vt03_palette_lut.h`, from EmuVT) when the Colour Palette page (p.22)
  is read.

Sound (p.3 feature list):
- 4 rhythm channels, 2 low-frequency channels, 2 noise channels.
- **PCM or DWS DMA** built in. NOTE: VT03 vocabulary is "PCM / DWS", not
  "ADPCM". The VT369 IMA/16x16-ADPCM decoder Furbtendulator models appears
  to be a later-generation addition; VT03 sample playback may be plain
  PCM/DWS. Confirm on sound pages (pp.33-34).

### Discrepancy notes so far
- [DIVERGE] The "VT extra opcodes" (TAD/TDA/PHX/PHY/PLX/PLY/ADX/LDAXD/LDAD)
  are not in the VT02/VT03 CPU instruction tables (VT02 p.47-48, VT03
  p.47-48); both chips are stock 6502. These belong to the VT369-era sound
  coprocessor. Already documented in FINDINGS_vt_opcodes.md.
- [GAP] 16-bit video bus mode (`V16BEN`) documented (p.3), not implemented.
- [?] Opcode encryption (Jungletac/Cube-Tech XOR gate) has NO section in
  the VT03 table of contents -- it is not a documented VT03 feature,
  consistent with being a later vendor-specific addition. The blocker for
  the sub-15 test ROMs lives outside this datasheet.

---

## 2. Register map -- Program unit ports (VT03 pp.23-28)

### $4100 W -- Program Bank1 / Video Bank2 (p.23)
High-order external-address extension bits (the top of the 32 MB space).
    D7..D4 = PA24..PA21   (Program address bits 24-21 -> "Program Bank1")
    D3..D0 = VA24..VA21   (Video   address bits 24-21 -> "Video Bank2")
[MATCH] PocketVT treats `$4100[3:0]` as the CHR outer bank
(`vt_chr_outer_4100`); that is the VA24..VA21 field here. The PA24..PA21
half (D7..D4) is the *program* high extension -- check PocketVT uses the
correct nibble for each.

### $4101 W -- Preload Times of timer interrupt (p.23)
    D7    = TSYNEN : 0 = count AD12 high/low transitions
                     1 = count HSYNC high/low transitions
    D6..D0 = preload count value
### $4102 W -- Load timer & start (p.23): write any value -> loads $4101 into timer, starts counting.
### $4103 W -- Disable timer interrupt (p.23): write any value.
### $4104 W -- Enable timer interrupt (p.23): write any value.

### $4105 W -- V Bank0 decode type / P Bank0 decode type / Internal Char VRAM (p.24)
    D7    = COMR7 : selects Video Bank0 decoder row set (Table C2)
    D6    = COMR6 : selects Program Bank0 decoder (with PQ2EN, Table C1)
    D5    = IVRCH : Internal VRAM as Character RAM enable
                    0 = disable; 1 = internal VRAM is BOTH vector and
                    character RAM
    D4..D0 = unused

Bank-decode matrices (silicon-level address remapping; this is the
hardware origin of the "OneBus byte-mangling" the mapper-256 submapper
tables describe in software):

- **Table C1 (program decode):** inputs = PQ2EN (from $410B) + COMR6 +
  CPU A[14:13] (rows 0H-7H). Output = physical program pins TPA20..TPA13.
  Full per-cell table (TPA20..TPA13 by PQ2EN/COMR6/A[14:13] row):

| PQ2EN | A[14:13] | TPA20 | TPA19 | TPA18 | TPA17 | TPA16 | TPA15 | TPA14 | TPA13 |
|-------|----------|-------|-------|-------|-------|-------|-------|-------|-------|
| 0 | 0H | PQ07 | PQ06 | PQ05 | PQ04 | PQ03 | PQ02 | PQ01 | PQ00 |
| 0 | 1H | PQ17 | PQ16 | PQ15 | PQ14 | PQ13 | PQ12 | PQ11 | PQ10 |
| 0 | 2H | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 |
| 0 | 3H | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 0 | 4H | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 |
| 0 | 5H | PQ17 | PQ16 | PQ15 | PQ14 | PQ13 | PQ12 | PQ11 | PQ10 |
| 0 | 6H | PQ07 | PQ06 | PQ05 | PQ04 | PQ03 | PQ02 | PQ01 | PQ00 |
| 0 | 7H | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 1 | 0H | PQ07 | PQ06 | PQ05 | PQ04 | PQ03 | PQ02 | PQ01 | PQ00 |
| 1 | 1H | PQ17 | PQ16 | PQ15 | PQ14 | PQ13 | PQ12 | PQ11 | PQ10 |
| 1 | 2H | PQ27 | PQ26 | PQ25 | PQ24 | PQ23 | PQ22 | PQ21 | PQ20 |
| 1 | 3H | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 1 | 4H | PQ27 | PQ26 | PQ25 | PQ24 | PQ23 | PQ22 | PQ21 | PQ20 |
| 1 | 5H | PQ17 | PQ16 | PQ15 | PQ14 | PQ13 | PQ12 | PQ11 | PQ10 |
| 1 | 6H | PQ07 | PQ06 | PQ05 | PQ04 | PQ03 | PQ02 | PQ01 | PQ00 |
| 1 | 7H | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |

  (Cells of "1" = that pin driven high/fixed. With PQ2EN=1, rows 2H/4H
  reach the higher PQ2x program lines; PQ2EN=0 stays in PQ0x/PQ1x.)
  This is identical to Table B3 (p.19); TPA[20:13] feeds Table B2.
- **Table C2 (video decode):** inputs = COMR7 (grouped with CH/DH/EH/FH
  variants) + AD[12:10]. Output = physical video pins TVA17..TVA10,
  driven from RV0x..RV5x. This is identical to Table A6 (p.14, given as a
  markdown table there). Feeds VA[17:10] via VB0S (Table A4).

[GAP] PocketVT's CHR-from-PRG sync uses a fixed MMC3-style 1K-page map
($2012-$2017 -> 1K banks) and does not model the COMR6/COMR7/PQ2EN
decode-type selection. For submapper-0/15 test ROMs that use the
identity/default decode this is fine; the Waixing/Trump/Zechess
submappers that need byte-mangling are exactly the ones exercising
non-default rows of C1/C2.

### $4106 W -- Horizontal / Vertical scrolling selector (p.25)
    D0 = HV : 0 = Horizontal, 1 = Vertical
    D7..D1 = unused

### $4107-$410A W -- Program Bank0 registers 0..3 (p.25)
Four 8-bit latches holding the program-address line groups that Table C1
(p.24) selects among:
    $4107 = PQ07..PQ00   (Program Bank0 register0)
    $4108 = PQ17..PQ10   (Program Bank0 register1)
    $4109 = PQ27..PQ20   (Program Bank0 register2)
    $410A = PQ37..PQ30   (Program Bank0 register3)
[MATCH] PocketVT uses $4107-$410A for PRG bank switching. These are the
PQ0x/1x/2x/3x sources feeding the C1 program-decode matrix.

### $410B W -- Timer clock / PQ2EN / RS232 / bus tristate / Program Bank0 selector (p.26)
    D7 = TSYNEN  : timer interrupt clock select (0=AD12, 1=HSYNC)
                   (master copy of the same selector shown at $4101 D7)
    D6 = PQ2EN   : Program Bank0 register2 enable (0=disable,1=enable)
                   --> THIS is the PQ2EN input to Table C1 (p.24)
    D5 = RS232EN : RS232 enable (0=disable, 1=enable)
    D4 = BUSTRI  : bus output (0=normal, 1=tristate)
    D3 = FWEN    : flash-write enable.
                   0 = $8000-$FFFF and $6000-$7FFF writes do NOT assert
                       XRWB (external write strobe)
                   1 = those writes DO assert XRWB; the "old program
                       method" is then inactive.
    D2..D0 = PS2,PS1,PS0 : Program Bank0 selector (3-bit; feeds the
             A[14:13]/row selection into C1)

### $410D W -- I/O port control (p.26)
Four GPIO ports (0..3), each with an enable and a direction bit:
    D7=IOP3EN  D6=IOP3OEN  D5=IOP2EN  D4=IOP2OEN
    D3=IOP1EN  D2=IOP1OEN  D1=IOP0EN  D0=IOP0OEN
    For port n: IOPnOEN = direction (0=input, 1=output)
                IOPnEN  = enable    (0=disable, 1=enable)
IMPORTANT constraints (p.26 footnotes):
  - When using flash memory in 16-bit mode, $410D D3..D0 MUST be set to $A.
  - External SRAM is NOT available when using flash memory in 16-bit mode.
[GAP] Relevant to the unimplemented V16BEN 16-bit-bus mode: 16-bit flash
forces this IOP configuration and disables external SRAM.

### $410E W -- I/O port 0,1 output data (p.27)
    D3..D0 = XVD3..XVD0 = port0 output data
    D7..D4 = XVD7..XVD4 = port1 output data
### $410F W -- I/O port 2,3 output data (p.27)
    D0=XRA10 D1=XAD10 D2=XAD11 D3=XAD12   = port2 output data
    D4=XRC   D5=XRCB  D6=XVOEB D7=XVRW     = port3 output data
### $410E R -- I/O port 0,1 input data (p.27): same XVD layout, read.
### $410F R -- I/O port 2,3 input data (p.27): same XRA10/XAD/XRC/XRCB/XVOEB/XVRW layout, read.

*** [DIVERGE] -- ENCRYPTION GATE ADDRESS ***
PocketVT's README states the VT09/Jungletac opcode-encryption gate is
"toggled by $410F writes." But on documented VT03, $410F is plainly the
I/O port 2,3 output-data register (GPIO: XRA10/XAD10-12/XRC/XRCB/XVOEB/
XVRW). There is NO encryption function at $410F in the VT03 datasheet.
Implication: the encryption gate is a later vendor addition that either
(a) reuses/overloads this GPIO address on VT369-class parts, or (b) is at
a different address and PocketVT inherited a wrong association. This needs
verifying against Furbtendulator's VT369 register map before trusting any
$410F-based encryption logic.

### $4114 W -- Low byte of RS232 Timer (p.27)
### $4115 W -- High byte of RS232 Timer (p.27)
    RS232T = ($4115 << 8) | $4114
    Clock CK21M: PAL = 26.601712 MHz, NTSC = 21.47727 MHz
    Baud rate = CK21M / ((RS232T + 2) * 2)
    Example: PAL, 9600 baud -> RS232T = 0x0567
### $4119 W -- RS232 Register (p.28)
    D5 = B8EN : 0 = 10-bit mode (start, end, bit7-0)
                1 = 11-bit mode (start, end, Bit8, bit7-0)
    D0 = T8B  : TX bit 8
### $4119 R -- RS232 Flags (p.28)
    D7 = RIFLAG : 1 = completed receiving data
    D6 = TIFLAG : 1 = completed sending data
    D5 = RINGF  : 1 = status of receiving data
    D4 = XF5OR6 : output pins, TV system selector
    D3 = XPORN
    D1 = RERRF  : 1 = receiving error
    D0 = R8B    : RX bit 8
### $411A W -- TX data of RS232 (p.28)
### $411B R -- RX data of RS232 (p.28)

---

## 3. Register map -- Graphic unit ports (VT03 pp.28-33)

### $2000 W -- PPU control (extended PPUCTRL) (p.29)
    D7 = NMI EN  : NMI control.  0 = ENABLE NMI, 1 = DISABLE NMI
    D6 = UNUSED  : (TEST pin control I/O)
    D5 = SP SIZE : sprite size. 1 = BIG (8x16 or 16x16), 0 = SMALL (8x8 or 16x8)
    D4 = BK AD12 : background character ROM address XAD12
    D3 = SP AD12 : sprite character ROM address XAD12
    D2 = V W SEQ : video data update sequence (0=Horizontal, 1=Vertical)
    D1 = VCOOR6  : page select in vertical mode   (0=page0, 1=page1)
    D0 = HCOOR6  : page select in horizontal mode (0=page0, 1=page1)

*** [DIVERGE] -- NMI POLARITY IS INVERTED vs NES ***
On the NES, $2000 bit7 = 1 ENABLES NMI. On VT03, $2000 D7 (NMI EN) is
0 = enable, 1 = disable -- the opposite. If PocketVT forwards VT $2000
straight into its NES PPUCTRL handling, vblank-NMI generation will be
backwards (games that write D7=0 to enable NMI would have it disabled,
and vice versa). Verify how ppu.s / vt_ppu_reg_write treats $2000 D7.
NOTE: some sources treat this datasheet bit description as an erratum;
cross-check actual game behaviour before flipping anything.

### $2001 W -- display control (extended PPUMASK) (p.29)
    D7..D5 = UNUSED
    D4 = SP EN  : sprite enable      (0=disable, 1=enable)
    D3 = BK EN  : background enable  (0=disable, 1=enable)
    D2 = SP INI : sprite initial coordinate     (0=righter, 1=lefter)
    D1 = BK INI : background initial coordinate (0=righter, 1=lefter)
    D0 = B/W    : 0 = Color, 1 = B/W
Note: SP INI / BK INI (D2/D1) are the VT analog of the NES left-8-pixel
clip bits but described as a coordinate shift (righter/lefter).

### $2002 R -- status (p.30)
    D7 = VSYN   : Blanking indicator (0=Display, 1=Blanking) = vblank flag
    D6 = B,S 0V : Priority indicator (sprite-0 / background overlap)
    D5 = OVLOAD : Sprite overload indicator (more sprites than per-line limit)
    D4..D0 = UNUSED
Reading $2002 also RESETS the write-sequence (toggle) for $2005 and $2006,
WITHOUT changing their stored contents. (NES $2002 latch-clear analog.)

### $2003 W -- Sprite pool counter initial address (p.30)
Sets the initial address to store sprite data. (NES OAMADDR analog.)
### $2004 W -- Sprite pool data (p.30)
Writes data into DRAM and increments the sprite counter. (NES OAMDATA analog.)

### $2005 W -- Horizontal/Vertical display origin coordinate, 2-byte (p.30)
First write after a $2002 read = horizontal coordinate; second write =
vertical coordinate. (NES PPUSCROLL analog.)

### $2006 W -- Initial address of Video RAM or ROM, 2-byte (p.30)
Write HIGH byte FIRST, then low byte. Auto-increments by 1 after each
$2007 access. Reading $2002 resets the sequence.
    High (first) byte:  D6=VA34 D5=XRC D4=AD12 D3=AD11 D2=AD10 D1=AD9 D0=AD8
                        (D7 unused)
    Low  (second) byte: AD7..AD0
NOTE: the high byte carries VA34 and XRC beyond the NES 14-bit PPU
address, so $2006 can point into ROM space, not just 16 KB of VRAM.
(NES PPUADDR analog, extended.)

*** XRC ($2006 first byte D5) = INTERNAL vs EXTERNAL video-memory selector
(p.9, Table A1): ***
  XRC = 1 : AD[12:0] address the INTERNAL video memory (the 2K VRAM /
            nametables / palette at PPU $0000-$3FFF).
  XRC = 0 : (one-bus mode) AD[12:0] + Video Memory Bank settings decide
            OA[24:0] = address of EXTERNAL video memory (ROM/CHR).
This is the bit that decides whether a $2007 access hits internal RAM or
streams from external ROM. VA34 ($2006 D6) is an extra high address bit
(appears at OA4 in the 16-color character types, Table A3).
[FOR PocketVT] Confirm $2007 routing honors XRC: writes/reads with XRC=1
go to internal nametable/palette; XRC=0 should fetch from banked CHR ROM.

### $2007 R/W -- Video RAM/ROM data port (p.30)
Fill address via $2006, then read/write here. First read after setting
the address is stale/unknown (buffered, NES-style); the next read returns
data at the $2006 address. Address auto-increments.

Example (p.31, read $2010/$2011 of video RAM/ROM):
    LDA $2002      ; reset command sequence
    LDA #$20 / STA $2006   ; high byte
    LDA #$10 / STA $2006   ; low byte
    LDA $2007      ; dummy (stale)
    LDA $2007      ; first real byte (data of $2010)
    LDA $2007      ; second byte (data of $2011)

### $2010 W -- Extended graphics control (COLCOMP register) (p.31)
    D7 = COLCOMP : Old color compatible. 0 = compatible (legacy NES color),
                   1 = NEW color mapping (SAT/LUM/PHA model below).
    D6,D5 = UNUSED
    D4 = BKEXTEN : Background address extension enable (0/1)
    D3 = SPEXTEN : Sprite address extension enable (0/1)
    D2 = SP16EN  : Sprite 16-colors-or-16-pixels enable (0/1)
    D1 = BK16EN  : Background 16-colors enable (0/1)
    D0 = PIX16EN : Sprite selector: 0 = 16 colors, 1 = 16 pixels
[MATCH] PocketVT's v0.5 palette fix gates on COLCOMP = `vt_reg_2010 & 0x80`.
Confirmed: D7 is COLCOMP, 1 = new color mapping. PocketVT's 4bpp path keys
on COLCOMP=1 AND (BK16EN or SP16EN) = D7 & (D1|D2). Matches the datasheet
field meanings.

### New color mapping model (when COLCOMP=1) (p.31)
A colour is specified by TWO palette bytes, an HSL-like triple:
    $3F80/$3F81/$3F82... byte:  D5..D2 = SAT[3:0] (saturation)
                                D1..D0 = LUM[3:2] (top of luminance)
    $3F00/$3F01/$3F02... byte:  D5..D4 = LUM[1:0] (bottom of luminance)
                                D3..D0 = PHA[3:0] (phase / hue)
So a full colour = SAT[3:0], LUM[3:0], PHA[3:0] = 12 bits = 4096 entries.
*** [MATCH] This is exactly why vt03_palette_lut.h is a 4096-entry
HSL->BGR555 table. The LUT index is (hi<<6)|lo where the 12 bits decompose
into SAT/LUM/PHA. The legacy ($3F00-$3F1F low) writes feed LUM/PHA; the
$3F80-$3F9F (hi) writes feed SAT/LUM-high. PocketVT's split of
vt_palette_write_lo()/vt_palette_write_hi() corresponds to these two
byte groups. ***
Colour validity envelope (must hold):
    4 <= (LUM[3:0] * 2) + SAT[3:0] <= 0x1F
    e.g. LUM=F -> SAT<=1;  LUM=E -> SAT<=3;  ...;  LUM=3 -> SAT<=2;
         LUM=2 -> SAT=0.

### $2011 W -- LCD/video options (p.32)
    D7,D6 = UNUSED
    D5 = VLS1, D4 = VLS0 : LCD vertical line count
            00 = 240 lines, 01 = 160, 10 = 120, 11 = 80
    D3 = EVRAMEN : enable internal VRAM (0 = Enable, 1 = Disable/don't use)
    D2 = PIX2EN  : B/W 2-color mode (0 = Disable, 1 = Enable)
    D1 = VDAEN   : Composited Video DA enable (0 = Enable, 1 = Disable)
    D0 = EVA12S  : Video Extension Address EVA12 selector
                   (0 = reg.BKPAGE, 1 = HV)

### $2012-$2017 W -- Video Bank0 registers 0..5 (p.32)
Six 8-bit latches holding the video-address line groups that Table C2
(p.24) selects among:
    $2012 = RV07..RV00  (register0)
    $2013 = RV17..RV10  (register1)
    $2014 = RV27..RV20  (register2)
    $2015 = RV37..RV30  (register3)
    $2016 = RV47..RV40  (register4)
    $2017 = RV57..RV50  (register5)
[MATCH/simplify] PocketVT uses $2012-$2017 for CHR bank switching with a
fixed MMC3-style 1K-page map. The datasheet shows these are the RV0x-RV5x
video-address latches feeding the C2 decode (p.24); PocketVT's 1K-page
interpretation is the default-decode special case of C2.

### $2018 W -- Video Bank1 register / BKPAGE / Video RW Bank (p.33)
    D6 = VA20, D5 = VA19, D4 = VA18 : high video-address bits ("Video Bank1 register")
    D3 = BKPAGE : is address EVA12 when EVA12S = 0 (see $2011 D0)
    D2 = VRWB2, D1 = VRWB1, D0 = VRWB0 : "video bank when accessing video data"
                 (the RW bank used for $2007 reads/writes)
    D7 = UNUSED
[MATCH] PocketVT shadows this as `vt_chr_reg_2018`.

### $2019 W -- Reset the Gun port (p.33): write any value -> clears X,Y of Gun port 1 and 2.

### $201A W -- Video Bank0 register6 / Video Bank0 selector (p.33)
    D7=RV67 D6=RV66 D5=RV65 D4=RV64 D3=RV63 : "Video Bank0 register 6"
    D2=VB0S2 D1=VB0S1 D0=VB0S0 : Video Bank0 selector
[MATCH] PocketVT shadows this as `vt_chr_reg_201A`.

### $201C R -- X coordinate of Gun port 1 (p.33)
### $201D R -- Y coordinate of Gun port 1 (p.33)
### $201E R -- X coordinate of Gun port 2 (p.33)
### $201F R -- Y coordinate of Gun port 2 (p.33)
[GAP] Light-gun input ($201C-$201F, reset via $2019). No GBA equivalent;
PocketVT almost certainly does not implement these. Harmless for non-gun
games; gun games would read stale/zero coordinates.

## 4. Sound generator (VT03 pp.33-34)

VT03 audio is APU-like (NES-derived) with two parallel port banks,
"XOP1" and "XOP2", each carrying rhythm/envelope/noise channels. XOP1
additionally has the DWS DMA sample channel. R/W = all Write.

### XOP1 Address Port ($4000-$4013)
Channel A "RHYTHM A":
    $4000  Envelop Control            1DY2 1DY1 1SC 1IW 1WI3..1WI0
    $4001  Auto Tune Control          1AT 1ST2 1ST1 1ST0 1SG 1AD2..1AD0
    $4002  Fine Tune Control          1FT7..1FT0
    $4003  Coarse Tune & Single Sound 1SL4..1SL1 1FTA 1FT9 1FT8
Channel B "RHYTHM B":
    $4004  Envelop Control            2DY2 2DY1 2SC 2IW 2WI3..2WI0
    $4005  Auto Tune Control          2AT 2ST2 2ST1 2ST0 2SG 2AD2..2AD0
    $4006  Fine Tune Control          2FT7..2FT0
    $4007  Coarse Tune & Single Sound 2SL4..2SL1 2FTA 2FT9 2FT8
Channel C "ENVELOP":
    $4008  Single Sound Enable        3EN 3EL6..3EL0
    $400A  Fine Tune Value            3FT7..3FT0
    $400B  Coarse Tune & Single Sound 3SL4..3SL1 3FTA 3FT9 3FT8
Channel D "NOISE":
    $400C  Envelope Control           4SC 4IW 4WI3..4WI0   (D5,D4 = 4SC,4IW; D3..D0 = 4WI3..0)
    $400E  Control Base Frequency     4NS 4BF3..4BF0       (D7=4NS; D3..D0=4BF3..0)
    $400F  Channel Enable & Single Sound  4SL4..4SL0
Channel E "DWS DMA" (sample playback):
    $4010  Amplitude                  DIRQ DREP SD3..SD0   (D7=DIRQ, D6=DREP, D3..D0=SD3..0)
    $4011  Initial Amplitude          IA6..IA0
    $4012  Starting addr of DWS data  SA13..SA6
    $4013  Data length of DWS data    DL11..DL4

### XOP2 Address Port ($4020-$402F)
A second identical bank at +$20, channels A/B/C/D only (no DWS DMA):
    $4020-$4023  RHYTHM A   (same layout as $4000-$4003)
    $4024-$4027  RHYTHM B   (same as $4004-$4007)
    $4028,$402A,$402B  ENVELOP (same as $4008,$400A,$400B)
    $402C,$402E,$402F  NOISE   (same as $400C,$400E,$400F)

Notes / [DIVERGE from VT369 audio]:
- This is "PCM / DWS DMA", NOT the IMA/16x16 ADPCM of VT369. The DWS DMA
  channel uses Amplitude + Initial Amplitude + start addr + length
  ($4010-$4013) -- a delta/PCM-style scheme, not ADPCM step tables.
- DIRQ ($4010 D7) = DMA-complete IRQ; DREP ($4010 D6) = DMA repeat/loop.
- SA13..SA6 ($4012) = sample start address (note: bits 13..6, so the
  start address is in units of 64 bytes / shifted left 6).
- DL11..DL4 ($4013) = sample length (bits 11..4, units of 16 bytes).
[?] PocketVT's vt_adpcm_mix_gba: confirm whether the test ROMs drive DWS
DMA ($4010-$4013) or expect VT369 ADPCM. If VT03-class, the sample engine
is DWS, and ADPCM step-table work would be aimed at the wrong chip.

### $4030 W -- DWS/PCM selector, DA control (p.35)
    D3 = DP, D2 = DA2, D1 = DA1 (=~A15), D0 = (~A14)
### $4031 W -- PCM data (p.35)
    PCM7..PCM0 -- direct PCM sample byte write.

## 5. Parameter description (VT03 pp.34-36)

Square-wave duty (xDY2,xDY1): 00=1/8, 10=1/4, 01=1/2, 11=3/4.
xSC : 0 = single (one-shot), 1 = continuous.
xIW : envelope mode. 0 = decay Fh->0h at slope xWI[3:0];
                     1 = hold constant at level xWI[3:0].
xWI[3:0] : if xIW=0, decay time Fh->0h = 4.16ms * xWI[3:0];
           if xIW=1, level = full_scale * xWI[3:0] / 15.
xAT : pitch-band effect. 0=disable; 1=enable (freq smoothly slews to
      max/min, e.g. machine-gun SFX; rate set by xST).
xST[2:0] : modulation time = 8.33ms * xST[2:0] (rate inversely prop.).
xSG : sign of 2^-m term. 0 = "+", 1 = "-".
xAD[2:0] : m = xAD[2:0]. Next freq:
           xSG=0 -> F(n+1) = Fn * (1 + 2^-m)
           xSG=1 -> F(n+1) = Fn * (1 - 2^-m)
xFT[A:0] : Frequency = 111,860 Hz / xFT[A:0]. Minimum xFT[A:0] = 0x08.
xSL[4:0] : sound duration of a single sound (beat-length decoder input).
           BCLK2 is set by $4017. Duration table (ms):
   idx :  00   01   02  03   04  05   06  07   08  09  0A  0B   0C  0D   0E   0F
   120Hz: 72  2024 152   8  312  24  632  40 1272  56 472  72 104  88  112  104
   100Hz: 90  2530 190  10  390  30  790  50 1590  70 590  90 130 110  250  130
   idx :  10   11   12  13   14  15   16  17   18  19  1A  1B   1C  1D   1E   1F
   120Hz: 88  120  184 136  376 152  760 168 1528 184 568 200 120 216  248  232
   100Hz:110  150  230 170  470 190  950 210 1910 230 710 250 150 270  310  290

More parameter definitions (p.36):
3EN : 0 = Enable (Beat length 1), 1 = Disable.
3EL[6:0] : Beat length 1 = BLCK1 * 3EL[6:0]. BLCK1 (via $4017) = 250Hz or 200Hz.
4NS : channel-4 noise band. 0 = wide band, 1 = narrow band.
xBF[3:0] : noise frequency.
DIRQ : 0 = disable DWS IRQ, 1 = enable DWS IRQ.
DREP : 0 = no repeat, 1 = repeat DWS data access (loop).
DP   : speech-synth selector. 0 = DWS, 1 = PCM.
DA2  : XOP2 DA enable. 0 = disable (default), 1 = enable.
DA1  : XOP1 DA enable. 0 = enable (default), 1 = disable.
~A15 : DWS/PCM DMA address A15 complement.
~A14 : DWS/PCM DMA address A14 complement.
IA[6:0]  : DWS initial amplitude.
SA[13:6] : DWS data start address. Form #11xxxxxxxx000000 -- the 8 reg
           bits are address[13:6]; low 6 bits are 0; high bits forced to
           binary 11. (Start aligned to 64 bytes.)
DL[11:4] : DWS/PCM data length. Form #xxxxxxxx0000 -- length in 16-byte units.
PCM[7:0] : CPU-written PCM data ($4031). TWO sample-playback paths:
           (1) CPU writes PCM directly to $4031;
           (2) DMA (DWS-like): PCM DMA controlled by $4010 (slope/amplitude
               + DIRQ/DREP), $4012 (start addr), $4013 (length).

### SD[3:0] -- DWS slope-decoder SAMPLE RATE table (p.36)  *** important ***
This is the VT03 sample-rate selector (analog of an ADPCM rate table):
   SD : F    E    D    C    B    A    9    8    7    6    5    4    3    2    1    0
   Hz : 33K  25K  21K  17K  14K  13K  11K  9K  8.4K 7.9K 7K  6.2K 5.5K 5.3K 4.7K 4.2K

*** [DIVERGE -- CONFIRMED] VT03 sample audio is DWS (slope/delta decoder)
or raw PCM, rate-selected by SD[3:0] above. It is NOT IMA/16x16 ADPCM.
PocketVT's ADPCM mixer should, for VT03 content, implement the DWS slope
decoder with this rate table -- not a VT369 ADPCM step table. The
"VT369-specific ADPCM step table" TODO is for a different chip family and
does not apply to VT02/VT03 ROMs. ***

## 6. Miscellaneous address ports (VT03 pp.37-40)

### $4014 W -- High byte of DMA source address; starts DMA (p.37)
Two bytes specify the DMA source; $4014 is the high byte (source =
$[XX]X0 with XX = this reg). Writing $4014 ALSO starts the DMA. VT03 has
DMA for BOTH video and sprite data; see $4034 for which and how long.
(NES OAMDMA analog, generalised.)

### $4034 W -- DMA settings for video/sprite data (p.37)
    D7..D4 = source address bit[7:4] of DMA
    D3..D1 = max data length:
             000 = 256 bytes, 100 = 16, 101 = 32, 110 = 64, 111 = 128
    D0 = SEL47 : 0 = DMA of SPRITE data (updates $2004)
                 1 = DMA of VIDEO  data (updates $2007)
    NOTE alignment: in 64-byte mode the source low byte must be
    00/40/80/C0 (VT03 stops when the low address reaches 3F/7F/BF/FF).
    16-byte mode splits memory into 16 pieces; 128-byte into 2.

### $4015 W -- Enable/disable XOP1 channels & DWS IRQ (p.37)  (APU $4015 analog)
    D4 = DWS/PCM enable     (0 = stop, 1 = start)
    D3 = XOP1 Noise enable
    D2 = XOP1 Envelope enable
    D1 = XOP1 Rhythm B enable
    D0 = XOP1 Rhythm A enable
### $4015 R -- Read XOP1 FLAG (p.38)
    D7 = DWS/PCM IRQ flag   (0 = inactive, 1 = active)
    D6 = Clock IRQ flag     (0 = IRQ inactive, 1 = IRQ active) <- timer IRQ
    D4 = DWS/PCM status     (0 = end, 1 = during)
    D3 = XOP1 Noise status  (0 = end, 1 = during)
    D2 = XOP1 Envelope status
    D1 = XOP1 Rhythm B status
    D0 = XOP1 Rhythm A status
Note: D6 is the timer/clock IRQ status tied to the $4101-$4104 timer.

### $4035 W -- Enable/disable XOP2 channels (p.38)
    D3 = XOP2 Noise enable
    D2 = XOP2 Envelope enable
    D1 = XOP2 Rhythm B enable
    D0 = XOP2 Rhythm A enable      (0 = stop, 1 = start each)
### $4035 R -- Read XOP2 FLAG (p.38)
    D3 = XOP2 Noise status
    D2 = XOP2 Envelope status
    D1 = XOP2 Rhythm B status
    D0 = XOP2 Rhythm A status      (0 = end, 1 = during)
Symmetry: $4015 = XOP1 control/flags, $4035 = XOP2 control/flags.

### $4016 W -- Set output pins XQ[2:0] (p.39)  (NES $4016 strobe analog)
    D2 = XQ2, D1 = XQ1, D0 = XQ0  (controller strobe / output latch pins)
    D7..D3 = UNUSED
### $4016 R -- Read peripheral data (p.39)
    D2 = Microphone, D1 = Floppy, D0 = Major joystick (joystick 1)
    D7..D3 = UNUSED
[MATCH] Joystick 1 read on $4016 D0, NES-style.

### $4017 W -- Beat-length clock & Clock-IRQ control (p.39)
    D7 = BLCK1/BLCK2 select:
         0 -> BLCK1 = 250 Hz, BLCK2 = 120 Hz
         1 -> BLCK1 = 200 Hz, BLCK2 = 100 Hz
    D6 = Clock IRQ enable: 0 = ENABLE clock IRQ (60 Hz), 1 = DISABLE
Ties to sound timing: BLCK1 scales 3EL (beat length 1, p.36); BLCK2
scales the xSL duration table (p.35). The 60Hz clock IRQ here is the
frame-IRQ analog and its status appears at $4015 R D6.
### $4017 R -- Read peripheral data (p.39)
    D4..D1 = Floppy Disk lines, D0 = Second Joystick (joystick 2)
[MATCH] Joystick 2 read on $4017 D0, NES-style.

## 7. Video memory bank mapping, one-bus mode (VT03 pp.10-14)

### Banking model (p.10, Figure A1)
VT03 addresses up to 32 MB external memory via 25 address bits OA[24:0].
The VIDEO address is banked in a 3-level hierarchy:
    Video Bank 2 : VA[24:21]  (top 4 bits)  -> splits 32MB into big blocks
    Video Bank 1 : VA[20:18]               -> splits each Bank2 block
    Video Bank 0 : VA[17:10]               -> splits each Bank1 block (finest)
Register sources for each level:
    Bank2 VA24..VA21 = $4100 D3..D0 (VA24..VA21)
    Bank1 VA20..VA18 = $2018 D6..D4 (VA20,VA19,VA18)
    Bank0 VA17..VA10 = $2012-$2017 / $201A (RV0x..RV6x latches)
*** IMPORTANT (extension mode): there is NO Video Bank 1 in extension
mode -- each block divided from Video Bank 2 is banked DIRECTLY by Video
Bank 0. The middle level collapses. ***
Detailed per-mode mappings are in "Table A3" (pp.11-14, next reads).
[NOTE for PocketVT] The fixed MMC3-style 1K CHR pages correspond to the
Bank0 (VA17:10) level with default Bank1/Bank2 = 0. Games using Bank1/
Bank2 or extension mode address CHR beyond what the flat 1K map models.

### PPU internal address map $0000-$3FFF (p.11)  *** key ***
    $0000-$07FF : VBANK = $2016 & 0xFE   (pattern, 2K via even bank)
    $0800-$0FFF : VBANK = $2017 & 0xFE
    $1000-$13FF : VBANK = $2012
    $1400-$17FF : VBANK = $2013
    $1800-$1BFF : VBANK = $2014
    $1C00-$1FFF : VBANK = $2015
    $2000-$23BF : Screen 00 Pattern vector area
    $23C0-$23FF : Screen 00 Color vector area
    $2400-..    : Screen 01 Pattern, Screen 01 Color, Screen 10 Pattern,
                  Screen 10 Color, Screen 11 Pattern, Screen 11 Color
                  (four nametables, each with split pattern+color vector)
    $3000       : no use
    $3F00-$3FFF : Color Palette vector
So $2012-$2017 select the 1K/2K CHR banks for $1000-$1FFF and $0000-$0FFF.
"VBANK" below = the value from the table above for the accessed region.

### Video address computation (p.11)  *** the literal CHR address formula ***
NORMAL mode, selected by ($201A & 0x07):
  0 (default): addr = ($4100&0x0F)<<21 + ($2018&0x70)<<14 + VBANK<<10
  1: ($4100&0x0F)<<21 + ($2018&0x70)<<14 + (($201A&0x80)|(VBANK&0x7F))<<10
  2: ...                               + (($201A&0xC0)|(VBANK&0x3F))<<10
  4: ...                               + (($201A&0xE0)|(VBANK&0x1F))<<10
  5: ...                               + (($201A&0xF0)|(VBANK&0x0F))<<10
  6: ...                               + (($201A&0xF8)|(VBANK&0x07))<<10
EXTENSION mode, selected by ($201A & 0x07):
  0 (default): addr = ($4100&0x0F)<<21 + VBANK<<13 + EVA<<10
  1: ($4100&0x0F)<<21 + (($201A&0x80)|(VBANK&0x7F))<<13 + EVA<<10
  2: ...               (($201A&0xC0)|(VBANK&0x3F))<<13 + EVA<<10
  4: ...               (($201A&0xE0)|(VBANK&0x1F))<<13 + EVA<<10
  5: ...               (($201A&0xF0)|(VBANK&0x0F))<<13 + EVA<<10
  6: ...               (($201A&0xF8)|(VBANK&0x07))<<13 + EVA<<10
($201A & 0x07) = VB0S[2:0], the Video Bank0 selector. It controls how
many high bits come from the $201A register vs the per-region VBANK.

CRITICAL side rules (p.11):
- *** When $4105 & 0x80 (COMR7) != 0, the $0000-$0FFF and $1000-$1FFF
  pattern regions EXCHANGE. *** (MMC3-style CHR A12 swap / pattern-table
  flip. PocketVT must honor this when COMR7 is set.)
- *** When background or 16x8 sprite is 16 colors (4bpp), the actual
  address is shifted ONE bit LEFT vs the formulas above. *** (This is the
  4bpp byte-doubling; directly relevant to PocketVT's 4bpp CHR sync.)

### EVA table (extension address sources) (p.11)
                                              EVA2     EVA1    EVA0
  BG ext mode, $2011&0x02 = 1 :               HV       BG4     BG3
  BG ext mode, $2011&0x02 = 0 :               BKPAGE   BG4     BG3
  Sprite ext mode            :                SPEVA2   SPEVA1  SPEVA0
  R/W ext mode               :                VRWB2    VRWB1   VRWB0
(VRWB2..0 = $2018 D2..D0, confirming $2018's RW-bank role for $2007.)

### Register -> video-address-bit assignments (p.12)  *** confirmed ***
    VA24-21  <- $4100 (D3-D0)
    VA17-10  <- $2012-$2017 (D7-0) and $201A (D7-0)
    EVA12-10 <- $2018 (D2-0)
    VA20-18  <- $2018 (D6-4)
Minimum video bank granularity = 1K bytes.

### 4-color vs 16-color character counts (p.12)  *** 4bpp doubling confirmed ***
The same address range holds TWICE as many tiles in 4-color (2bpp) mode
as in 16-color (4bpp) mode, because a 16-color tile is 32 bytes vs 16:
    $0400-$07FF   : 64 chars (4-color)  / 32 chars (16-color)
    $2000-$3FFF   : 512 / 256
    $40000-$7FFFF : 16K / 8K
This is the hardware basis for the "shift one bit left for 16-color"
rule (p.11). [MATCH] PocketVT's 4bpp path doubles tile stride accordingly.

### Bank1/Bank2 address ranges (p.12)
    $00000-$3FFFF   : VA20-18 = 0, VA24-21 = 0
    $40000-$7FFFF   : VA20-18 = 1
    ... $1C0000-$1FFFFF : VA20-18 = 7
    $200000-$3FFFFF : VA24-21 = 1
    $400000-$5FFFFF : VA24-21 = 2
    $600000-$7FFFFF : VA24-21 = 3
    ... $1E00000-$1FFFFFF : VA24-21 = F
Full 32MB (25-bit, $1FFFFFF) = VA24-21 (4b, Bank2) x VA20-18 (3b, Bank1)
x VA17-10 (8b, Bank0) x 1K.

### Table A2 -- four background/sprite character types (p.13)
    Type1 : extension addr DISABLE, 4 colors/pixel  (2bpp)
    Type2 : extension addr ENABLE,  4 colors/pixel  (2bpp)
    Type3 : extension addr DISABLE, 16 colors/pixel OR 16x8 sprite (4bpp)
    Type4 : extension addr ENABLE,  16 colors/pixel OR 16x8 sprite (4bpp)
Source registers (p.13 text):
    VA[9:0]   <- AD[9:0] via $2006
    VA[17:10] <- per Table A4 (Video Bank0 selector dependent)
    VA[20:18] <- $2018 (D6:4)        [Video Bank 1]
    VA[24:21] <- $4100 (D3:0)        [Video Bank 2]
    VA34      <- $2006 first byte D6
    EVA[12:10]<- per Table A5

### Table A3 -- OA[24:0] output-pin assignment by type (p.13)  *** the CHR address mux ***

| OA | Type1 | Type2 | Type3 | Type4 |
|----|-------|-------|-------|-------|
| 24 | VA24 | VA24 | VA23 | VA23 |
| 23 | VA23 | VA23 | VA22 | VA22 |
| 22 | VA22 | VA22 | VA21 | VA21 |
| 21 | VA21 | VA21 | VA20 | VA17 |
| 20 | VA20 | VA17 | VA19 | VA16 |
| 19 | VA19 | VA16 | VA18 | VA15 |
| 18 | VA18 | VA15 | VA17 | VA14 |
| 17 | VA17 | VA14 | VA16 | VA13 |
| 16 | VA16 | VA13 | VA15 | VA12 |
| 15 | VA15 | VA12 | VA14 | VA11 |
| 14 | VA14 | VA11 | VA13 | VA10 |
| 13 | VA13 | VA10 | VA12 | EVA12 |
| 12 | VA12 | EVA12 | VA11 | EVA11 |
| 11 | VA11 | EVA11 | VA10 | EVA10 |
| 10 | VA10 | EVA10 | VA9 | VA9 |
| 9 | VA9 | VA9 | VA8 | VA8 |
| 8 | VA8 | VA8 | VA7 | VA7 |
| 7 | VA7 | VA7 | VA6 | VA6 |
| 6 | VA6 | VA6 | VA5 | VA5 |
| 5 | VA5 | VA5 | VA4 | VA4 |
| 4 | VA4 | VA4 | VA34 | VA34 |
| 3 | VA3 | VA3 | VA3 | VA3 |
| 2 | VA2 | VA2 | VA2 | VA2 |
| 1 | VA1 | VA1 | VA1 | VA1 |
| 0 | VA0 | VA0 | VA0 | VA0 |

Pattern: 16-color types (3,4) shift the high VA bits down one (the 4bpp
left-shift seen from the address side) and insert VA34 at OA4. Extension
types (2,4) substitute EVA12..10 for the mid VA bits at OA13..OA10ish.
[GAP] PocketVT models roughly Type1/Type3 (non-extension). Type2/Type4
(extension addr enable, $2010 BKEXTEN/SPEXTEN set) use EVA insertion that
the flat CHR map does not reproduce.

### Table A4 -- VA[17:10] vs VB0S[2:0] ($201A D2-0 = Video Bank0 Selector) (p.14)
VB0S sets how many top bits of VA[17:10] come from $201A's RV6x bits vs
the TVA decode (Table A6):

| VB0S | VA17 | VA16 | VA15 | VA14 | VA13 | VA12 | VA11 | VA10 |
|------|------|------|------|------|------|------|------|------|
| 000 | TVA17 | TVA16 | TVA15 | TVA14 | TVA13 | TVA12 | TVA11 | TVA10 |
| 001 | RV67 | TVA16 | TVA15 | TVA14 | TVA13 | TVA12 | TVA11 | TVA10 |
| 010 | RV67 | RV66 | TVA15 | TVA14 | TVA13 | TVA12 | TVA11 | TVA10 |
| 100 | RV67 | RV66 | RV65 | TVA14 | TVA13 | TVA12 | TVA11 | TVA10 |
| 101 | RV67 | RV66 | RV65 | RV64 | TVA13 | TVA12 | TVA11 | TVA10 |
| 110 | RV67 | RV66 | RV65 | RV64 | RV63 | TVA12 | TVA11 | TVA10 |

RV[67:63] via $201A (D7:3); TVA[17:10] from Table A6.

### Table A5 -- EVA[12:10] sources (p.14)

| Condition | EVA12 | EVA11 | EVA10 |
|-----------|-------|-------|-------|
| BKEXTEN=1 & EVAS12=1 & BG display area | HV ($4106) | BG4 | BG3 |
| BKEXTEN=1 & EVAS12=0 & BG display area | BKPAGE ($2018) | BG4 | BG3 |
| SPEXTEN=1 & horiz-sync read char area | SPEVA2 | SPEVA1 | SPEVA0 |
| CPU RW mode in vert-sync area / not display | VRWB2 | VRWB1 | VRWB0 |

### Table A6 -- TVA[17:10] by COMR7 + AD[12:10] (p.14) == the C2 decode (p.24)

| COMR7 / AD[12:10] | TVA17 | TVA16 | TVA15 | TVA14 | TVA13 | TVA12 | TVA11 | TVA10 |
|-------------------|-------|-------|-------|-------|-------|-------|-------|-------|
| 0H/1H or CH/DH | RV47 | RV46 | RV45 | RV44 | RV43 | RV42 | RV41 | AD10 |
| 2H/3H or EH/FH | RV57 | RV56 | RV55 | RV54 | RV53 | RV52 | RV51 | AD10 |
| 4H or 8H | RV07 | RV06 | RV05 | RV04 | RV03 | RV02 | RV01 | RV00 |
| 5H or 9H | RV17 | RV16 | RV15 | RV14 | RV13 | RV12 | RV11 | RV10 |
| 6H or AH | RV27 | RV26 | RV25 | RV24 | RV23 | RV22 | RV21 | RV20 |
| 7H or BH | RV37 | RV36 | RV35 | RV34 | RV33 | RV32 | RV31 | RV30 |

RV[17:10],[27:20],[37:30],[47:40],[57:50] via $2012-$2017. This is the
same matrix as "Table C2" on p.24, now in video-addressing context. Note
the lowest output (TVA10) is AD10 directly for the COMR7=0/1/C/D and
2/3/E/F rows (pass-through), but RVx0 for the 4-7/8-B rows.
[SUMMARY] Full video address chain now documented: AD[9:0] from $2006 ->
VA[9:0]; AD[12:10]+COMR7 -> TVA[17:10] (A6) -> VA[17:10] via VB0S (A4);
$2018 -> VA[20:18] + EVA[12:10]; $4100 -> VA[24:21]; then OA pins muxed
per character Type (A3). PocketVT's flat 1K map = VB0S=000, COMR7=0,
Type1/Type3, no extension.

## 8. Program memory bank mapping, one-bus mode (VT03 pp.15-19)

### Banking model (p.15, Figure B1)
PROGRAM address is banked in a 2-level hierarchy (vs video's 3 levels):
    Program Bank 1 : PA[24:21] (top 4 bits) -> splits 32MB into big blocks
    Program Bank 0 : PA[20:13] (8 bits)     -> splits each Bank1 block
Register sources:
    Bank1 PA24..PA21 = $4100 D7..D4
    Bank0 PA20..PA13 = $4107-$410A (PQ0x..PQ3x latches) via C1 decode (p.24)
PRG bank granularity = 8 KB (PA bit 13 = 8K boundary).
Detailed per-mode mappings on pp.16-19 (next reads).

### CPU address map $0000-$FFFF (p.16)  *** key ***
    $0000-$07FF : RAM (2K internal program RAM)
    $0800-$1FFF : RAM ($0000-$0800 mirror x3)
    $2000-$5FFF : Extension IO area (PPU/sound/control registers live here)
    $6000-$7FFF : Extension RAM area (cart SRAM/WRAM)
    $8000-$9FFF : BANK = $4107 if ($410B&0x40)==0, else BANK = 0xFE
    $A000-$BFFF : BANK = $4108
    $C000-$DFFF : BANK = 0xFE if ($410B&0x40)==0, else BANK = $4109
    $E000-$FFFF : BANK = 0xFF (fixed last bank; holds reset/NMI/IRQ vectors)
*** When $4105 & 0x40 (COMR6) != 0, the $8000 and $C000 regions EXCHANGE
(PRG analog of the COMR7 pattern swap). ***
Note $410B&0x40 = PQ2EN: it swaps whether $4107/$4109 or the fixed 0xFE
drive $8000/$C000.

### Program bank mapping formula (p.16)
The physical PRG address is selected by PS[2:0] = ($410B & 0x07). For each
case, addr = ($4100 & 0xF0) << 17  +  (Bank0 term) << 13, where the Bank0
term mixes the $410A latch with the per-region BANK value:

| PS[2:0] | Bank0 term (<<13) |
|---------|-------------------|
| 0 (default) | ($410A & 0xC0) \| (BANK & 0x3F) |
| 1 | ($410A & 0xE0) \| (BANK & 0x1F) |
| 2 | ($410A & 0xF0) \| (BANK & 0x0F) |
| 3 | ($410A & 0xF8) \| (BANK & 0x07) |
| 4 | ($410A & 0xFC) \| (BANK & 0x03) |
| 5 | ($410A & 0xFE) \| (BANK & 0x01) |
| 6 | $410A (full) |
| 7 | BANK (full) |

BANK = the per-region value from the CPU map above ($4107 / $4108 / $4109
/ 0xFE / 0xFF). PS[2:0] controls how many high bits come from $410A vs the
region BANK -- exactly mirroring the video VB0S mechanism (A4).
[NOTE for PocketVT] Default PRG mapping (PS=0, PQ2EN handling) gives the
familiar $8000/$A000/$C000/$E000 = $4107/$4108/$fixed/$last layout. The
$4100 high nibble (<<17 = *128KB) is the Program Bank 1 extension that
reaches the full 32MB; ensure PRG bank math uses $4100 D7-D4, not D3-D0.

### Register -> program-address-bit assignments (p.17)  *** confirmed ***
    PS2-0   <- $410B (D2-0)
    PQ07-0  <- $4107 (D7-0)
    PQ17-0  <- $4108 (D7-0)
    PQ27-0  <- $4109 (D7-0)
    PQ37-0  <- $410A (D7-0)
    PA24-21 <- $4100 (D7-4)   <-- PRG uses the HIGH nibble of $4100
Minimum program bank = 8 KB.

### PRG bank-size table (p.17)
Program Bank0 = 256 banks x 8K = 2 MB. How PS2-0 selects within Bank0:

| PS2-0 | Bank0 selection |
|-------|-----------------|
| 0 | within $0000-$7FFFF, PQ37-6 picks the 64K window (PQ37-6 = 0/1/2/3 -> $80000/$100000/$180000 ...); PQ25-0, PQ15-0, PQ05-0 select among 64 banks |
| 6 | PQ37-0 select 256 banks |
| 7 | PQ27-0, PQ17-0, PQ07-0 select 256 banks |

Program Bank1 (PA24-21) tiles 2 MB blocks across the 32 MB space:

| PA24-21 | Address range | Size |
|---------|---------------|------|
| 0 | $000000-$1FFFFF | 2 M |
| 1 | $200000-$3FFFFF | 2 M |
| 2 | $400000-$5FFFFF | 2 M |
| 3 | $600000-$7FFFFF | 2 M |
| 4-7 | $800000-$FFFFFF | 8 M |
| 8-F | $1000000-$1FFFFFF | 16 M |

Total = 16 Bank1 blocks x 2 MB = 32 MB.

### Table B1 -- OA[24:0] for program memory (p.18)
Simple concatenation (no character-type variants like video):
    OA24..OA13 = PA24..PA13   (high bits from bank registers)
    OA12..OA0  = A12..A0      (low 13 bits straight from CPU address)
Confirms 8K bank granularity (2^13). PA[24:21] from $4100; PA[20:13]
from Table B2; A[12:0] = CPU address low bits.

### Table B2 -- PA[20:13] for Program Bank0, by PS[2:0] ($410B) (p.18)
PS[2:0] selects how many high bits come from PQ3x ($410A) vs the TPA
decode (Table B3 = the C1 matrix on p.24):

| PS | PA20 | PA19 | PA18 | PA17 | PA16 | PA15 | PA14 | PA13 |
|----|------|------|------|------|------|------|------|------|
| 000 | PQ37 | PQ36 | TPA18 | TPA17 | TPA16 | TPA15 | TPA14 | TPA13 |
| 001 | PQ37 | PQ36 | PQ35 | TPA17 | TPA16 | TPA15 | TPA14 | TPA13 |
| 010 | PQ37 | PQ36 | PQ35 | PQ34 | TPA16 | TPA15 | TPA14 | TPA13 |
| 011 | PQ37 | PQ36 | PQ35 | PQ34 | PQ33 | TPA15 | TPA14 | TPA13 |
| 100 | PQ37 | PQ36 | PQ35 | PQ34 | PQ33 | PQ32 | TPA14 | TPA13 |
| 101 | PQ37 | PQ36 | PQ35 | PQ34 | PQ33 | PQ32 | PQ31 | TPA13 |
| 110 | PQ37 | PQ36 | PQ35 | PQ34 | PQ33 | PQ32 | PQ31 | PQ30 |
| 111 | TPA20 | TPA19 | TPA18 | TPA17 | TPA16 | TPA15 | TPA14 | TPA13 |

PS=000 -> top 2 bits from PQ37/36, rest from TPA decode;
PS=110 -> all 8 bits from PQ3x ($410A); PS=111 -> all from TPA decode.
PQ[07:00],[17:10],[27:20],[37:30] via $4107-$410A. TPA[20:13] per Table B3 (p.19).

### Table B3 -- TPA[20:13] by PQ2EN + COMR6 + A[14:13] (p.19)
This is IDENTICAL to "Table C1" on p.24 (PQ2EN from $410B D6, COMR6 from
$4105 D6). PQ2EN=0 keeps the matrix in the PQ0x/PQ1x range; PQ2EN=1
reaches PQ2x for A[14:13] rows 2H/4H. Confirms program decode = C1/B3.

## 9. Background pattern & internal video RAM (VT03 pp.19-21)

### Tile/nametable model (p.19)  *** NES-divergent indirection ***
- One page = 256x240 px = 32x30 background patterns (tiles), each 8x8 px.
- Background PATTERN pixel data lives in EXTERNAL video memory.
- Internal video RAM stores VECTORS (addresses) pointing to those patterns:
  each byte = one tile slot on the page; 32x30 = 960 bytes per page.
  (So the nametable holds indirect pattern POINTERS, not tile indices into
  a fixed pattern table as on NES.)
- Of each 1KB page, the low 960 bytes are the pattern vectors; the high
  64 bytes store the 3rd & 4th color-address bits for the page. VT03
  groups FOUR adjacent patterns to share the same 3rd/4th color bits
  (the attribute-table analog, cf. NES $23C0).

### Per-pixel color addressing (p.19)  *** ties to SAT/LUM/PHA palette ***
- Each pixel color = 5 bits (4-color mode) or 7 bits (16-color mode) that
  index the palette SRAM (25x6 in 4-color / 121x12 in 16-color). This is
  the "25 or 121 colors" from the feature page, and the palette SRAM holds
  chrominance+luminance -> the SAT/LUM/PHA model (p.31).
- Color-address bits 1,2 (and 6,7 in 16-color) come from the PATTERN data
  in external video memory; bits 3,4 come from the page attribute bytes
  (Figure B2); bit 5 selects sprite vs background palette.
- Decode rules:
    bits 1,2 or 6,7 = internal color of a pattern (3 describable colors);
    bit pattern (1,2)=(0,0) or (6,7,...)=(0,0,0) => TRANSPARENT pixel.
    bits 3,4 = recolor whole pattern (4 selectable color sets).
    bit 5 = 1 -> sprite colors; bit 5 = 0 -> background colors.
[NOTE for PocketVT] The indirect-pointer nametable (vectors -> external
pattern data) is a real divergence from NES fixed pattern tables. Verify
how PocketVT resolves $2000-region nametable bytes into CHR fetches; a
straight NES nametable-index assumption would be wrong for VT03 pages
that use the pointer indirection.

### Figure B1 -- screen <-> internal VRAM layout (p.20)
    $2000-$23BF : Background Page "left or top"  (960 vector bytes)
    $23C0-$23FF : attribute area (3rd/4th color bits) for that page
    $2400-$27FF : Background Page "right"
    $2800-$2BFF : Background Page "bottom"
Vector address within a page = row*0x20 + col, with 32 cols x 30 rows:
row0 = 000H..01FH, row1 = 020H..03FH, ..., row29 = 3A0H..3BFH
(= 0x3C0 = 960 bytes). Three pages (left/top, right, bottom) support
horizontal and vertical scroll arrangements.

### Figure B2 -- 4 adjacent patterns share 3rd/4th color address (p.20)
Each $23C0-region byte packs BG[4:3] for four 2x2 pattern groups (a
4x4-tile area), NES-attribute-style:
    patterns {000,001,020,021} -> bits[1:0] of $23C0
    patterns {002,003,022,023} -> bits[3:2] of $23C0
    patterns {040,041,060,061} -> bits[5:4] of $23C0
    patterns {042,043,062,063} -> bits[7:6] of $23C0
And the next 4x4 block's 16 patterns -> $23C1, etc.
[MATCH-ish] Attribute layout mirrors NES $23C0 semantics; PocketVT can
likely reuse NES attribute logic for the recolor bits, BUT the nametable
bytes themselves are pattern pointers (p.19), not fixed tile indices.

### Two-page background / scrolling (p.21)
2KB internal video RAM = 2 pages. $4106 D0 picks scroll axis:
  Horizontal: left page $2000-$23BF, right page $2400-$27FF;
              AD10(VIDEO) + A10(2K RAM) wired in game card.
  Vertical:   top page $2000-$23FF, bottom page $2800-$2BFF;
              AD11(VIDEO) + A10(2K RAM) wired in game card.

### Sprite Pool (OAM analog) (p.21)  *** sprite format ***
256-byte sprite pool = 64 sprites x 4 bytes. Write via $2003/$2004 or DMA
($4014/$4034). Max 64 sprites, <= 8 per scanline row.
Four bytes per sprite, IN THIS ORDER:
    byte0 = vertical coordinate (Y)
    byte1 = 8-bit VECTOR (pointer to sprite pattern in external video mem)
    byte2 = status (attributes, see below)
    byte3 = horizontal coordinate (X)
*** NOTE order/meaning differs from NES OAM (Y, tile, attr, X): byte1 is
a pattern POINTER, not a tile index. ***
Status byte (byte2) bits:
    D7 = 1: mirror at X axis (vertical flip),   0: normal
    D6 = 1: mirror at Y axis (horizontal flip), 0: normal
    D5 = 1: background covers sprite, 0: sprite covers background (priority)
    D4 = SPEVA2 (sprite extension vector addr bit2)
    D3 = SPEVA1
    D2 = SPEVA0
    D1 = SP4 (color set of sprite, bit4)
    D0 = SP3 (color set of sprite, bit3)
SP[4:3] works like BG[4:3] (recolor set selection).

### Sprite color & size (via $2000/$2001/$2010) (p.21)
Allowed combos: 8x16/16color, 8x16/4color, 16x16/4color, 8x8/4color,
8x8/16color, 16x8/4color.
16-color = 4 bits/pixel: an 8x8 16-color sprite pattern = 32 bytes
(vs 16 bytes for 4-color). 
[NOTE for PocketVT] This is the basis for the "16x8 hi-res sprite" TODO.
16x8 exists only in 4-COLOR mode (not 16-color), per the allowed combos.
The sprite attribute bit layout (flip D7/D6, priority D5, SPEVA D4-2,
SP D1-0) is VT-specific; confirm PocketVT's OAM decode matches, especially
that flip bits are D7(X-axis/vert) and D6(Y-axis/horiz).

## 10. Colour palette (VT03 p.22)

### Palette memory map (p.22)  *** definitive; refines the p.31 model ***
Palette lives at $3F00-$3FFF; each colour is 6 bits (compatible) or 12
bits (new mapping), specified across TWO parallel banks:
    $3F00-region (low bank),  D5-D0 -> Luminance[1:0] + Phase[3:0]
    $3F80-region (high bank), D5-D0 -> Saturation[3:0] + Luminance[3:2]
Paired structure (low / high), each a 16- or 32-colour set:
    $3F00-$3F0F / $3F80-$3F8F : Background 16 colours
    $3F10-$3F1F / $3F90-$3F9F : 1st 32-colour palette / Sprite 16 colours
    $3F20-$3F3F / $3FA0-$3FBF : 2nd 32-colour palette
    $3F40-$3F5F / $3FC0-$3FDF : 3rd 32-colour palette
    $3F60-$3F7F / $3FE0-$3FFF : 4th 32-colour palette

### Colour-mode selection (p.22)  *** COLCOMP clarified ***
    COLCOMP = 0 -> Luminance/Saturation[1:0] + Phase[3:0] available
                   (the "compatible" / fewer-bits mode)
    COLCOMP = 1 -> Saturation[3:0] + Luminance[3:0] + Phase[3:0]
                   (full 12-bit "new mapping")
    - Sprite vs background colour selected by SB5 (the bit-5 selector, p.19).
    - 1st/2nd/3rd/4th 32-colour palette selected by SP7-6 or BG7-6.
    - The 32 colours within a palette selected by SB5 and SP4-1 or BG4-1.
    - SB5 = background/sprite selector.
[MATCH -> ACTIONABLE] PocketVT's two palette-write halves map exactly:
$3F00-half = LUM/PHA, $3F80-half = SAT. The 4096-entry LUT is indexed by
the assembled 12-bit (SAT,LUM,PHA). COLCOMP=0 should mask to the reduced
bit set (LUM/SAT[1:0]) rather than using full SAT[3:0]; verify PocketVT's
compatible-mode path does this and doesn't always assume 12-bit. The four
32-colour palette banks selected by SP/BG[7:6] are the "4 color sets" from
the feature page.

### Palette mode clarification (p.8, Note2; gated by XRC=1)
- $3F00-$3F1F = OLD colour mapping, total 25 colours. $3F00 = transparent.
  $3F10/$3F04/$3F14/$3F08/$3F18/$3F0C/$3F1C are ignored (mirror/unused
  slots -- same pattern as NES palette mirrors).
- $3F00-$3FFF = NEW colour mapping, total 121 colours.
- Example: $3F00 and $3F80 combine into one colour = 4 bits Luminance +
  4 bits Saturation + 4 bits Phase.
So 25-colour mode = legacy NES-palette-like layout (with NES-style mirror
slots); 121-colour = full SAT/LUM/PHA. This is the COLCOMP/XRC axis:
COLCOMP selects bit depth, XRC selects old(25)/new(121) mapping location.
[FOR PocketVT] The 25-colour mode's ignored slots ($3F04/$3F08/...)
mirror NES behaviour -- a compatible-mode ROM may rely on those mirrors.

## 11. Timing waveforms (VT03 pp.40-41)

### Program-unit input cycle timing, application mode (p.40)
Master clock CK21M: Fpal = 26.601712 MHz, Fntsc = 21.47727 MHz
(confirms the RS232 baud-rate clock on p.27).
Bus signals: XA14~0 (program addr), RW, ROMCS, D7~D0 (data).
AC characteristics: TA = 0..70 C, VCC = 3.0..3.6 V, GND = 0 V.
    Tcyc (program cycle time)        : min 70  max 450 ns
    Tph  (cycle high pulse width)    : min 240 max 300 ns
    Tpl  (cycle low pulse width)     : min 100 max 150 ns
    Tah  (program address hold)      : min 10 ns
    Tdh  (program data hold)         : min 10 ns
    Trds (program read data setup)   : min 10 ns
    Twds (program write data setup)  : min 10 ns
(Electrical timing -- reference only; not directly actionable for the GBA port.)

### Graphic-unit input cycle timing, application mode (p.41)
Diagram only (no numeric min/max table on this page). Video-bus signals:
XVA12~0 (video addr), XRC, XVD7~D0 (video data), VOE(NTSC) (video output
enable), VRW (video read/write). Fosc = CK21M period.
    Read:  Tvph/Tvpl (VOE high/low), Tvrcy (read cycle), Tvrad (addr delay),
           Tvrah (addr hold), Tvrds (data setup), Tvrdh (data hold)
    Write: Tvwpl (VRW pulse), Tvwas (addr setup), Tvwah (addr hold),
           Tvwds (data setup), Tvwdh (data hold)

## 12. Pin description (VT03 p.6)

Type: I = input, O = output, I/O = bidirectional. (PH) = internal pull-up
20-50K, (PL) = internal pull-down 20-50K. Many pins have a OneBus-mode
alternate function (second clause in the description).

| Pin | Type | Function (normal / one-bus alternate) |
|-----|------|----------------------------------------|
| XA[14:0] | O | CPU address bus / one-bus addr OA14-OA0 |
| XD[7:0] | I/O | CPU data bus / one-bus data bus bit7-0 |
| XCK18 | O | CPU clock 1.8 MHz |
| XRW | O | CPU or one-bus read/write |
| XROMCS | O | ROM chip select / one-bus ROM OEB |
| XFCSB | O | ROM/flash CS in one-bus, low active, $8000-$FFFF |
| X67CSB | O | $6000-$7FFF chip select, low active |
| XDEVMB | I | Swap XFCSB and X67CSB when low (PH) |
| XLCDENB | I | Enable LCD signal output for testing, low active (PH) |
| XPTRIB | I | Force bus tristate, low active (PH) |
| XBISEL | I | Built-in function selector; low forces X67CSB/XFCSB high (PH) |
| XIRQB | I | CPU interrupt input (PH) |
| XVRW | O | Video read/write / I/O in one-bus |
| XVOEB | O | Video data output enable / I/O in one-bus |
| XVRA10 | I | Internal Video RAM address bit 10 (scroll page select) |
| XRC | O | External ROM CS low active / I/O in one-bus |
| XRCB | O | External ROM CS high active or power-on indicator / I/O |
| XVA[9:0] | O | Video addr bus / OA24-OA15 in one-bus |
| XAD[12:10] | O,I/O | Video addr bus A12-A10 / I/O in one-bus |
| XVD[7:0] | I/O | Video data bus / one-bus 8-bit data, or data bus bit15-8 in 16-bit |
| XTESTB | I | Wafer test pin (PH) |
| XRESTB | I | System reset, low active (PH) |
| XCK21M | I | Clock input pin for crystal |
| XCK21B | O | Clock output pin for crystal |
| X4016[1:0] | I | I/O interface input pins (PH) |
| X4017[4:0] | I | I/O interface input pins (PH) |
| XQ[2:0] | O,I/O | I/O output pins or Video extension address (XQ1,XQ0=O; XQ2=I/O) |
| XCUP46,XCUP47 | I/O | Clock of I/O; XCUP47 can be video extension address / TXDP (PH on 46) |
| XVIDEO | O | Composite video signal |
| XOP1,XOP2 | O | Audio signal (XOP2 = PCM output, p.43) |
| XJOYSELB | I | Internal joystick enable when XJOYSEL=0 (PH) |
| XONEBUS | I | One-bus mode selector, high active (PH) |
| XD16BUSB | I | 16-bit data bus selector (low active) in one-bus; A0 picks low byte (XD7-0) or high byte (XVD7-0) (PH) |
| XPORN,XF5OR6 | O | TV system selector: all 0 = NTSC, all 1 = PAL (PH) |

Clarifications this gives the register map:
- The $410F GPIO bit names (XVRW, XVOEB, XRC, XRCB) are these physical
  pins -- confirming $410F is GPIO, not encryption (see DIVERGE note in S3).
- XD16BUSB is the hardware pin behind the unimplemented 16-bit video bus
  (V16BEN) mode.
- XPORN/XF5OR6 = the NTSC/PAL select read back at $4119 D4.
- XQ[2:0] ($4016 output pins) double as the EVA video-extension address.

### Pin function multiplexing (VT03 p.7)
Each multiplexed pin's role depends on config bits. Summary:

| Pin(s) | GPIO mode | One-bus 16-bit | Native |
|--------|-----------|----------------|--------|
| XVD0-3 | IOP00-03 (IOP0EN=1,XONEBUS=1,XD16BUSB=1) | D8-D11 (IOP0EN=0,XD16BUSB=0) | XVD0-3 (XD16BUSB=X) |
| XVD4-7 | IOP10-13 | D12-D15 | XVD4-7 |
| XVRA10,XAD10-12 | IOP20-23 (IOP2EN=1) | -- | XVRA10,XAD10-12 (IOP2EN=0) |

| Pin | IOP3EN=1 | IOP3EN=0, XJOYSELB=1 | IOP3EN=0, XJOYSELB=0 |
|-----|----------|----------------------|----------------------|
| XRC | IOP30 | XRC | XRC |
| XRCB | IOP31 | XRCB | POWON |
| XVOEB | IOP32 | XVOEB | XVOEB |
| XVRW | IOP33 | XVRW | XVRW |

| Pin | XONEBUS=1 (one-bus) | XONEBUS=0 (native) |
|-----|---------------------|--------------------|
| XA[14:0] | OA[14:0] | XA[14:0] |
| XVA[9:0] | OA[24:15] | XVA[9:0] |
| XROMCS | ROM OEB | XROMCS |
| XRWB | MEMORY RWB | XRWB |

Joystick pins (X4016/X4017), XJOYSELB=0 = dedicated, =1 = generic input:

| Pin | XJOYSELB=0 | XJOYSELB=1 |
|-----|------------|------------|
| X4016D0 | JOYAM | X4016D0 |
| X4016D1 | JOYBM | X4016D1 |
| X4017D0 | JOYUPA | X4017D0 |
| X4017D1 (GUNPORT1) | JOYST | X4017D1 |
| X4017D2 (GUNPORT2) | JOYSE | X4017D2 |
| X4017D3 | JOYDNA | X4017D3 |
| X4017D4 | JOYLFA | X4017D4 |
| XCUP46 | JOYRTA | XCUP46 |

Video-extension / RS232 address pins:

| Pin | Condition A | Condition B |
|-----|-------------|-------------|
| XQ0 | VIDEO ROM A10 (XJOYSELB=0 && XONEBUS=0) | XQ0 (XJOYSELB=1 \|\| XONEBUS=1) |
| XQ1 | VIDEO ROM A11 (same) | XQ1 |
| XQ2 | RD (RS232EN=1) | VIDEO ROM A12 (RS232EN=0,XONEBUS=0,XJOYSELB=0) / XQ2 |
| XCUP47 | TD (RS232EN=1) | VIDEO ROM A13 (same) / XCUP47 |

KEY: XQ0/XQ1/XQ2/XCUP47 act as VIDEO ROM A10-A13 when not in OneBus/RS232
mode -- these carry the EVA extension address into the ROM, which is how
Type2/Type4 (extension) character addressing physically reaches memory.
Note: X4017D1/D2 = GUNPORT1/GUNPORT2 (the light-gun trigger lines).

## 13. Programming guide (VT03 p.43)  *** highly actionable ***

1. Avoid spurious IRQ: first set $4017 = $C0 or $40 (disable clock IRQ).
2. If the program does NOT set the new address ports, OLD COMPATIBLE
   (NES-like) mode is the default -- an unconfigured ROM = plain NES-like.
3. Single-bus boot: program initial address A24-A0 = $007FFFC (reset
   vector region); video initial address = $0000XXX. $4102-$410A control
   the decoder address ports. $4109 lets the program bank register grow
   from 2 to 3 banks, but must be specified by $4109.
4. Character byte sizes: 16-color or 16x8 sprite = 32 bytes/char;
   16x16 sprite = 64 bytes/char (confirms 4bpp doubling).
5. Background 16-color: $2010 = $82; with extension addr enable: $2010 = $92.
6. BG extension addr enable: BG4/BG3 become a 10-bit character vector;
   char size 16x16; BG color-set function DISABLED, (BG4,BG3) fixed 00.
7. Sprite 16-color: $2010 = $84; +ext: $2010 = $8C.
   Sprite 16-pixel: $2010 = $85; +ext: $2010 = $8D.
8. Gun ports: read in NMI ISR. $201C/$201D = gun1 X/Y, $201E/$201F =
   gun2 X/Y; write $2019 to reset all gun ports.
9. PCM only outputs to XOP2. Set $4030 = $18 to enable channel-2 DA + PCM
   mode. $4031 writes go straight to XOP2 DA. PCM DMA uses $4010/$4012/
   $4013 like DWS. PCM data length MAX 4081 bytes; if longer, enable PCM
   IRQ, set unit-waveform mode in $4010, update $4012 + bank, restart PCM
   DMA via $4015 = $10 in the ISR. PCM DMA is EXCLUSIVE with DWS.
10. Video DMA examples (start by writing $4014):

| $4034 | $4014 | Effect |
|-------|-------|--------|
| $58 | $02 | update $2004 from $0250-$025F (16 B, SPRITE) |
| $AD | $03 | update $2007 from $03A0-$03BF (32 B, VIDEO) |
| $0D | $03 | update $2007 from $0300-$033F (64 B, VIDEO) |

11. Extra chip + XRWB: configure $410B; when FWEN is high the old program
    method is inactive.
12. Don't DMA-copy to color palette in NTSC (PAL is fine). Read $4119
    (D3,D4) to detect NTSC/PAL; if NTSC, shift palette data one byte.
13. Old gun games access $3F20: initialise $3F20 = $2D in the menu program
    or old gun games won't run correctly.
14. PCM data must be a multiple of 64 bytes. With $4013=$FF the length is
    NOT 4096 -- it physically plays 4081 bytes (addr $000-$FF0); bytes
    $FF1-$FFF can't be fetched by PCM DMA.
15. RS232: set first command $410B(D5)=1. TXDP outputs through XCUP47;
    set XCUP47 as TXDP first to avoid $4017-pin conflicts.

[FOR PocketVT] Note #2: default (unconfigured) = old compatible mode, so
a ROM that never writes the $41xx/$2010 ports = plain NES-like. Notes
#5/#7 give the exact $2010 values per color/extension mode (good test
vectors for the 4bpp + COLCOMP path). Note #10 gives concrete DMA combos
to validate the $4014/$4034 engine.

## 14. Package & pinout (VT03 pp.50-51)
64-pin package. Pin assignment (p.51) and mechanical drawing (p.50) are
physical-layout only -- no relevance to the GBA emulator (PocketVT does
not model physical pins). Signals present match the pin description (S13b):
XFCSB, XD/XA program buses, XVA/XVD/XAD video buses, XOP1/XOP2 audio,
XVIDEO, crystal XCK21M/XCK21B, X4016D0/D1, XQ0/XQ1, status pins
XF5OR6/XPORN/XRESTB, and power VDD1-4 / VSS1-4. Not transcribed in full;
view p.51 if a specific pin number is ever needed.

## 15. VT02 deltas vs VT03

Full VT02 documentation lives in the companion file DATASHEET_DIGEST_VT02.md
(section 0 there has the complete delta table). Summary of how VT02 differs
from this (VT03) datasheet:

VT02 (rev A5, 2005) is the EARLIER, 4-color-only (2bpp) sibling. It shares
nearly the entire architecture documented above -- 6502 CPU, program/video
bank mapping (same formulas and C1/C2/A4/A6/B1/B2/B3 tables), sound
(XOP1/XOP2 + DWS/PCM), DMA, timer/IRQ, RS232, scrolling, sprite-pool
format, nametable indirection, pins, the $2000 NMI inversion, and the
"$410F is GPIO" / "no VT-extra-opcodes" findings.

VT02 DIFFERS from VT03 only in color depth and its consequences:
- Marketing: "Real 4 / Virtual 16 colors" (VT03 = "Real 16 / Virtual 64").
- Background & sprites: 4-color only (VT03 also has 16-color).
- Sprite sizes: 8x8 / 8x16 only (VT03 also 16x8 / 16x16).
- Palette: 25 colors, ONE byte, 6-bit (LUM[1:0]+PHA[3:0]), $3F00-$3F1F only,
  NO $3F80 saturation bank (VT03 = 25 or 121, two bytes, 12-bit SAT/LUM/PHA).
- $2010: only BKEXTEN(D4)/SPEXTEN(D3) (VT03 also COLCOMP/SP16EN/BK16EN/PIX16EN).
- Character types: Type1/Type2 only, no VA34 (VT03 adds 16-color Type3/Type4).
- Per-pixel color: 5 bits -> 25x6 SRAM (VT03 also 7-bit -> 121x12 SRAM).
- Programming guide: 12 notes, no 16-color setup (VT03 has 15).
- Some later VT03 notes absent ($410D 16-bit-flash rules).

[FOR PocketVT] PocketVT's VT03 4bpp/COLCOMP path and 4096-entry HSL LUT are
VT03-ONLY. VT02 content needs a separate, simpler palette path (64-entry
6-bit LUM/PHA, single $3F00-$3F1F writes), $2010 treated as BKEXTEN/SPEXTEN
only, and 4-color 8x8/8x16 sprites. A VT02-vs-VT03 chip-type flag should
gate the difference. Details in DATASHEET_DIGEST_VT02.md sections 0, 3, 7, 9, 10.
