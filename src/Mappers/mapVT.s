@ mapVT.s -- VT03 / VT09 OneBus mapper for PocketVT
@
@ The VT-series chips (VT03 / VT09 / VT32 / VT369) are "NES-on-a-chip"
@ designs that integrate PRG ROM, CHR ROM, RAM, and all peripherals onto
@ a single die.  From a software perspective the bankswitching is driven
@ by the extended $4100-$41FF register block (handled in vt_regs.c) rather
@ than by writes to cartridge address lines.
@
@ OneBus PRG layout (per Furbtendulator/src-mappers/h_OneBus.cpp):
@   $8000-$9FFF  = bank PQ0 (= $4107)
@   $A000-$BFFF  = bank PQ1 (= $4108)
@   $C000-$DFFF  = bank PQ2 (= $4109) IF PQ2EN ($410B bit 6), ELSE 0xFE
@   $E000-$FFFF  = bank 0xFF (FIXED last)
@ The high bank-bits OR'd from PQ3 ($410A) and PA21 ($4100>>4) are ignored
@ here -- our test ROMs (64-256 KB) fit in a 6-bit bank space and never
@ touch them.  Extend prg_bank_compose if you hit a larger ROM.
@
@ This mapper file provides the GBA-side glue:
@   mapVTinit     -- called by loadcart_asm to initialise the VT mapper
@   write_vt4xxx  -- writemem_4 hook: dispatches APU vs VT-register vs
@                    bank-switch writes inline in asm.
@
@ Implementation notes
@ --------------------
@ 1. RESET VECTOR ANCHOR
@    Reset vectors for all known VT mapper-256 ROMs sit in the topmost
@    16KB.  We set up $C000-$FFFF as banks 0xFE/0xFF (last 16KB) and
@    $8000-$BFFF as bank 0 to match real chip default after power-on.
@
@ 2. PPU $2008+ DIVERT GATE
@    The PPU register divert at ppu.s only activates when vt_active != 0.
@    Without that gate, every standard NES PPU mirror access would be
@    redirected to the VT extension handler.  We set vt_active here and
@    loadcart_asm clears it for non-VT cartridges (see cart.s).
@
@ 3. WRITEMEM_4 CALLING CONVENTION
@    On entry to write_vt4xxx the NES bus address is in addy (r12), NOT
@    in r1.  r1 holds the negated bank index from writememabs's dispatch.
@
@ 4. ABI SAFE BANK SWITCHING
@    Calling `map89_` directly from C code corrupts the GCC ABI because `map89_`
@    clobbers `r4` (m6502_mmap) and `r9` (m6502_pc) which C treats as entirely
@    callee-saved! Returning from C restores their *old* values, instantly
@    undoing the bank switch and sending execution to garbage! Instead, C 
@    code simply updates `vt_prg_banks` and `vt_prg_dirty`. This assembly
@    checks the dirty flag, carefully avoids overwriting `r4`-`r11`, and 
@    performs the actual switch safely in the outer wrapper.

#include "../equates.h"
#include "../6502mac.h"

    .global mapVTinit
    .global write_vt4xxx
    .global write_vt_rom
    .global vt_apply_prg_banks

    .extern vt_reg_write
    .extern vt_reg_read
    .extern vt_reset
    .extern vt_recompute_prg_banks
    .extern vt_mmc3_forward
    .extern vt_active
    .extern vt_prg_banks
    .extern vt_prg_dirty
    .extern vt_mirror_dirty
    .extern vt_mirror_value
    .extern vt_set_mirroring
    .extern vt_dma_settings

    .extern IO_W
    .extern map89_
    .extern mapAB_
    .extern mapCD_
    .extern mapEF_
    .extern void
    .extern dma_W           @ stock PocketNES sprite DMA (ppu.s::_4014w)
    .extern vmdata_W        @ $2007 write path (ppu.s, increments vramaddr)
    .extern NES_RAM         @ for CPU-source byte reads ($0000-$07FF)

@ ============================================================================
@ mapVTinit
@ Called by loadcart_asm when mapper_number == MAPPER_VT.
@
@ ** CALLING CONVENTION QUIRK **
@ PocketNES's mapper-dispatch loop in cart.s does this BEFORE jumping to
@ a mapperinit function:
@
@     ldr r0, [r1, #-4]            ; r0 = mappertbl2[index] = mapVTinit
@     ldmia r0!, {r1-r4}           ; r1-r4 = first FOUR words at mapVTinit;
@                                  ; r0 now advanced by 16 bytes
@     str r1, writemem_8           ; writemem_8 := first  .word
@     str r2, writemem_A           ; writemem_A := second .word
@     str r3, writemem_C           ; writemem_C := third  .word
@     str r4, writemem_E           ; writemem_E := fourth .word
@     mov pc, r0                   ; jump to mapVTinit + 16  (PAST the header)
@
@ So **every** mapperinit must start with a 4-word table giving the
@ writemem handlers for $8000-$9FFF / $A000-$BFFF / $C000-$DFFF /
@ $E000-$FFFF.  PocketVT's first cut omitted this header, with the
@ result that:
@   (1) the first four ARM instructions of mapVTinit got installed as
@       bogus writemem handlers, so any $8000+ write jumped into the
@       middle of an instruction byte sequence
@   (2) mapVTinit started executing 16 bytes in, skipping `stmfd sp!,{lr}`
@       and the vt_active=1 store; the eventual `ldmfd sp!,{pc}` then
@       popped garbage off the stack and returned to garbage land
@
@ THIS WAS THE PRIMARY REASON ALL VT ROMS BLACK-SCREENED in 0.3.0.
@ The reset code in $E000-$FFFF (which is fixed-bank for OneBus) never
@ even got a chance to execute -- loadcart_asm crashed before issuing
@ the JMP to the reset vector.
@
@ VT cartridges don't write to $8000-$FFFF (the OneBus bus is bank-locked
@ ROM in that range; bank-switch registers are at $4100+).  Every entry
@ in the header is therefore `void` (the empty-function handler in
@ memory.s).  Submapper-1 Waixing VT03 and a handful of others use the
@ MMC3 protocol over $8000-$9FFF and would need a different first word,
@ but none of the test ROMs do.
@ ============================================================================
mapVTinit:
    .word   write_vt_rom, write_vt_rom, write_vt_rom, write_vt_rom
    @
    @ The four entries above are writemem_8 / writemem_A / writemem_C /
    @ writemem_E -- they get installed by the cart.s dispatcher when it
    @ jumps to this function.  Earlier PocketVT versions used `void` here,
    @ which silently dropped every write to $8000-$FFFF.  Per the NESdev
    @ wiki article "VT02+ MMC3 Compatibility Registers", real VT silicon
    @ FORWARDS those writes to the corresponding VT-native registers by
    @ default; dropping them caused every game's "STA $8000 / STA $8001"
    @ bank-switch sequence to disappear, which is why the user saw an
    @ infinite reset loop (cart crashes -> PocketNES crash handler
    @ returns to menu -> menu auto-launches the same cart on single-ROM
    @ multicarts).  See vt_mmc3_forward() in vt_regs.c for the exact
    @ MMC3-to-VT translation table.
    @
    stmfd   sp!, {lr}

    @ --- Mark this cartridge as a VT cartridge -----------------------
    @ The PPU $2008+ extended-register divert in ppu.s is gated on
    @ vt_active so non-VT mappers still behave as standard NES.
    ldr     r0, =vt_active
    mov     r1, #1
    strb    r1, [r0]

    @ --- Install VT extra opcode handlers BEFORE vt_reset ------------
    @ Patches PHX/PLX/PHY/PLY/TAD/TDA/ADX/JMP-VT09 into op_table at
    @ their canonical (decrypted) positions.  This MUST happen before
    @ vt_reset() because vt_reset() calls vt_rebuild_optable(), which
    @ takes a one-time snapshot of op_table[] on its first invocation
    @ and uses that snapshot as the source for every subsequent
    @ encryption-state rebuild.  If the VT extras weren't already
    @ installed at snapshot time, encryption-mode permutations would
    @ route encrypted bytes whose decryption is a VT extra (e.g.
    @ 0x3A -> 0x5A=TAD under submapper 15) to the *stock* slot's
    @ canonical handler (NOP/illegal) instead of the VT handler.
    @ vt_patch_optable touches only op_table[], not VTState, so the
    @ "VTState must be zeroed first" concern from earlier comments
    @ was incorrect.
#if VT_EXTRA_OPCODES
    bl_long vt_patch_optable
#endif

    @ --- Initialise the VT C-side state block (zeros vt.reg[] etc) ---
    @ Must come BEFORE vt_resync_prg_banks so the register shadow it
    @ reads is in a defined state.  Also calls vt_rebuild_optable(),
    @ which snapshots op_table NOW (with VT extras already in place)
    @ and permutes it per current encryption state.
    bl      vt_reset

    @ Set up the default PRG layouts that C computed
    bl      vt_apply_prg_banks

    @ ---------------------------------------------------------------------
    @ Clear the VS-Unisystem / four-screen cart flags (session 15).
    @
    @ PocketNES's loader reads iNES flags7 bit 0 as "VS Unisystem" using the
    @ ORIGINAL iNES semantics.  Lonely Island's header is NES 2.0 (flags7 =
    @ $0B: bits 2-3 = %10 mark NES 2.0, bits 0-1 = %11 are an EXTENDED console
    @ type), so that bit does not mean VS at all -- yet it left cartflags with
    @ VS set.  mirrorchange force-overrides the arrangement to m0123 whenever
    @ SCREEN4 or VS is set, so PocketVT ran the whole game on four INDEPENDENT
    @ nametables.  A VT board only has 2KB of CIRAM: the game relies on $2800
    @ mirroring $2000, and with four real nametables the mirrored halves were
    @ simply never written -- the black background that only "filled in" once
    @ the camera scrolled far enough to re-enter the one nametable the game
    @ had actually drawn into.
    @
    @ No VT/OneBus board is a VS Unisystem or has four-screen VRAM, so clear
    @ both bits here and let $4106 (and the MMC3-compat $A000) drive mirroring.
    ldrb_   r1, cartflags
    bic     r1, r1, #(SCREEN4+VS)
    strb_   r1, cartflags

    @ Start from the side-by-side arrangement (horizontal scrolling), which is
    @ what $4106 bit 0 = 0 selects; the game overrides this as scenes load.
    mov     r0, #0
    bl_long vt_set_mirroring

    @ Install the $4100-$41FF write hook ------------------------------
    adr     r1, write_vt4xxx
    str_    r1, writemem_4

    @ Install the $4000-$40FF / $4100-$41FF READ hook -----------------
    @ Until session 12 there was NO read hook: vt_reg_read() existed but
    @ nothing ever called it, so every $41xx read returned stock open bus.
    @ That silently broke Lonely Island's palette upload -- see the $4119
    @ comment in vt_reg_read() -- and would break any VT title that probes
    @ its hardware.
    adr     r1, read_vt4xxx
    str_    r1, readmem_4

    @ --- VT extra opcode handlers were installed at the top of this
    @ function (before vt_reset).  See the long comment there.

    ldmfd   sp!, {pc}

@ ============================================================================
@ vt_apply_prg_banks
@ Reads the computed PRG banks from C and applies them safely in assembly.
@ This prevents GCC from corrupting the m6502_pc (r9) register across the ABI.
@ ============================================================================
vt_apply_prg_banks:
    stmfd   sp!, {lr}
    
    ldr     r1, =vt_prg_banks
    ldrb    r0, [r1, #0]
    bl_long map89_
    
    ldr     r1, =vt_prg_banks
    ldrb    r0, [r1, #1]
    bl_long mapAB_
    
    ldr     r1, =vt_prg_banks
    ldrb    r0, [r1, #2]
    bl_long mapCD_
    
    ldr     r1, =vt_prg_banks
    ldrb    r0, [r1, #3]
    bl_long mapEF_
    
    ldmfd   sp!, {pc}

@ ============================================================================
@ write_vt4xxx  (writemem_4 hook)
@
@ Calling convention (PocketNES writemem path):
@   r0   = byte value being written
@   addy (r12) = NES bus address ($4000-$5FFF range)
@   lr   = return address (set up by writememabs's adr lr,0f)
@
@ IMPORTANT: We must avoid clobbering r4 (m6502_mmap) and r9 (m6502_pc)
@ across the ASM to C boundary! We use the stack to hold any precious state.
@ ============================================================================
@ ============================================================================
@ read_vt4xxx -- readmem_4 hook ($4000-$41FF reads).
@
@ CRITICAL CALLING CONVENTION: memory READ handlers are entered from the 6502
@ core with "adr lr,0f ; ldr pc,[r10,r1,lsl#2]".  There is NO usable ARM
@ stack here -- sp is repurposed by the core -- so this routine must not push
@ and must not call C (a C callee would need a frame).  It returns the value
@ in r0 via "mov pc,lr", and must preserve r12 (addy) for RMW instructions.
@
@ Only $4119 needs VT-specific handling for the games we support:
@   $4119 read = RS232 Flags; bit 3 XPORN (1 = PAL), bit 4 XF5OR6 (1 = 50Hz).
@ Lonely Island's NMI palette task at $E0B1 reads it and tests AND #$18 to
@ decide WHERE to DMA its 128-byte palette: $3F00 (bits set) or $3F01 (clear).
@ With no read hook at all, the stock empty_R returned open bus (addy>>8 =
@ $41), whose bits 3-4 are clear, so every palette entry landed one slot late
@ and backgrounds sampled the wrong colours -- the "glitched palette".
@ These carts are PAL/50Hz VT03 boards (the reference capture matches the
@ $3F00 upload), so report XPORN|XF5OR6.  See vt_reg_read() for the same note.
@ Everything else falls through to the stock IO_R, preserving old behaviour.
@ ============================================================================
    .global read_vt4xxx
read_vt4xxx:
    sub     r1, r12, #0x4100        @ r1 = low byte for $41xx, huge otherwise
    cmp     r1, #0x19
    moveq   r0, #0x18               @ XPORN | XF5OR6
    moveq   pc, lr
    ldr     pc, =IO_R               @ tail-jump; lr still points at the core


write_vt4xxx:
    @ Session-13 perf: dispatch BEFORE building a frame.  This hook sees every
    @ write in $4000-$4FFF, and the overwhelming majority are plain APU
    @ register writes from the music engine ($4000-$4013, $4015, $4017).  The
    @ old prologue pushed {r12,lr} and later did bl_long IO_W + ldmfd for all
    @ of them -- write_vt4xxx was the top entry in the session-12 profile at
    @ ~8%.  Now the common cases tail-jump straight into the stock handler
    @ with lr still pointing at the 6502 core, so they cost three extra
    @ instructions instead of a call frame.  Only $4034, $4014 and $41xx --
    @ the VT-specific registers -- take the slow path and set up a frame.
    lsr     r1, r12, #8
    cmp     r1, #0x40
    bne     .Lvt_w_maybe41

    and     r1, r12, #0xFF
    cmp     r1, #0x34
    cmpne   r1, #0x14
    ldrne   pc, =IO_W                @ plain APU write: no frame, no call

    stmfd   sp!, {r12, lr}
    b       .Lvt_w_40_special

.Lvt_w_maybe41:
    cmp     r1, #0x41
    ldrne   pc, =IO_W                @ $42xx-$4Fxx: stock, no frame
    stmfd   sp!, {r12, lr}
    b       .Lvt41xx

.Lvt_w_40_special:

    @ === $4000-$40FF range ===========================================
    @ Two VT-specific special cases (per VT03 datasheet p.37) need to
    @ be intercepted before falling through to stock IO_W:
    @
    @   $4034 W: DMA settings (bit0 = target $2007 vs $2004,
    @            bits 1-3 = byte count, bits 4-7 = source addr low nibble).
    @            Stock IO_W has no handler -- writes silently dropped.
    @
    @   $4014 W: DMA trigger.  Stock _4014w always does 256-byte sprite
    @            DMA to $2004, ignoring $4034.  If $4034 bit 0 is set we
    @            need to do video DMA to $2007 instead (e.g. palette
    @            uploads like Lonely Island's $E0C1 routine).
    @
    @ Everything else in $4000-$40FF still goes to stock IO_W.

    and     r2, r12, #0xFF       @ r2 = low addr byte

    cmp     r2, #0x34
    beq     .Lvt_w_4034
    cmp     r2, #0x14
    beq     .Lvt_w_4014

    @ (Standard APU writes never reach here any more -- they tail-jumped to
    @ IO_W in the prologue above.  Kept as a safety net.)
    bl_long IO_W
    b       .Lvt_write_done

@ ------------------------------------------------------------------
@ $4034 W -- store DMA settings shadow, no actual DMA yet.
@ ------------------------------------------------------------------
.Lvt_w_4034:
    ldr     r1, =vt_dma_settings
    strb    r0, [r1]
    b       .Lvt_write_done

@ ------------------------------------------------------------------
@ $4014 W -- DMA trigger.  Decide sprite vs video based on $4034 bit 0.
@ ------------------------------------------------------------------
.Lvt_w_4014:
    ldr     r1, =vt_dma_settings
    ldrb    r1, [r1]
    tst     r1, #0x01
    beq     .Lvt_w_4014_sprite       @ bit0=0 -> stock sprite DMA

    @ --- Video DMA path ($4034 bit 0 = 1, target = $2007) -----------
    @
    @ Layout:
    @   r0 = byte just written to $4014 (= source addr high)
    @   r1 = vt_dma_settings (= $4034 value)
    @
    @ Compute length from bits 1-3 of $4034:
    @   000 -> 256, otherwise -> 1<<(bits) (so 100=16, 101=32, 110=64, 111=128).
    @ Source addr = (r0<<8) | (r1 & 0xF0), masked to NES RAM range (2KB).
    @
    @ For each byte: load from NES_RAM[src & 0x7FF], call vmdata_W
    @ (which auto-advances vramaddr).
    
    stmfd   sp!, {r4, r5, r6, r7}     @ save callee-saved regs we'll use
    
    @ r6 = source byte counter (running offset within source addr space)
    @ r7 = length remaining
    
    @ Length: 1 << bits, with bits==0 -> 256
    mov     r2, r1, lsr #1
    and     r2, r2, #0x07            @ r2 = bits 1-3 of $4034
    cmp     r2, #0
    moveq   r2, #8                   @ shift==0 means length=256
    mov     r7, #1
    mov     r7, r7, lsl r2           @ r7 = length

    @ Source address: (r0 << 8) | (r1 & 0xF0)
    @ NES_RAM only goes to $07FF, so we'll mask each fetch.
    and     r4, r1, #0xF0            @ low nibble of mid byte
    orr     r6, r4, r0, lsl #8       @ r6 = full 16-bit source addr

    @ Fast path: LI (and other VT titles) blast the whole palette here every
    @ frame.  vt_pal_dma_fast handles the case where the transfer lands
    @ entirely inside the $3F00-$3F7F window with stride 1, doing the same
    @ stores in one tight loop instead of 128 x (vmdata_W + VRAM_pal).
    @ It returns 0 when the transfer doesn't qualify, and we fall through.
    mov     r0, r6                   @ r0 = source addr
    mov     r1, r7                   @ r1 = length
    bl_long vt_pal_dma_fast
    cmp     r0, #0
    bne     .Lvt_w_4014_dma_done

    ldr     r5, =NES_RAM             @ r5 = NES RAM base (0x03000000)
    mov     r4, #0x800
    sub     r4, r4, #1               @ r4 = 0x7FF (NES RAM addr mask)

.Lvt_w_4014_loop:
    @ Load source byte (mask to 2KB NES RAM range)
    and     r0, r6, r4
    ldrb    r0, [r5, r0]

    @ Call $2007 write path -- writes r0 to current vramaddr, increments.
    @ vmdata_W clobbers r1, r2, r12, and uses addy (r12) internally.
    @ r4-r7 are preserved across the call.
    bl_long vmdata_W

    add     r6, r6, #1
    subs    r7, r7, #1
    bne     .Lvt_w_4014_loop

.Lvt_w_4014_dma_done:
    ldmfd   sp!, {r4, r5, r6, r7}
    b       .Lvt_write_done

.Lvt_w_4014_sprite:
    @ Stock sprite DMA path (the normal _4014w in ppu.s).
    @ IO_W reads addy and dispatches to dma_W.
    bl_long IO_W
    b       .Lvt_write_done

.Lvt41xx:
    cmp     r1, #0x41
    bne     .Lvt_write_done

    stmfd   sp!, {r0}           @ Save value just in case
    
    and     r0, r12, #0xFF
    mov     r1, r0              @ arg2 = val
    ldr     r0, [sp]            @ arg1 = addy
    
    @ We swap around so r0=addr, r1=val, then call our C handler
    mov     r1, r0
    and     r0, r12, #0xFF
    bl      vt_reg_write
    
    ldmfd   sp!, {r0}           @ Pop value
    
    @ Did vt_reg_write change the banking configuration?
    ldr     r1, =vt_prg_dirty
    ldrb    r2, [r1]
    cmp     r2, #0
    beq     .Lvt_no_prg
    mov     r2, #0
    strb    r2, [r1]
    bl      vt_apply_prg_banks

.Lvt_no_prg:
    @ Did vt_reg_write change the nametable arrangement ($4106 / $A000)?
    @ Applying it needs ARM code and a real stack, which we have here.
    ldr     r1, =vt_mirror_dirty
    ldrb    r2, [r1]
    cmp     r2, #0
    beq     .Lvt_write_done
    mov     r2, #0
    strb    r2, [r1]
    ldr     r1, =vt_mirror_value
    ldrb    r0, [r1]
    bl_long vt_set_mirroring

.Lvt_write_done:
    ldmfd   sp!, {r12, pc}


@ ============================================================================
@ write_vt_rom  (writemem_8 / writemem_A / writemem_C / writemem_E hook)
@
@ Forwards $8000-$FFFF writes to the MMC3-compatibility translator in C.
@ Real VT silicon does this by default (per the NESdev wiki "VT02+ MMC3
@ Compatibility Registers" article); the only way to disable it is to set
@ the FWEN bit in $410B, which vt_mmc3_forward checks for us.
@
@ Calling convention (same writememabs path as write_vt4xxx):
@   r0 = byte value being written
@   addy (r12) = NES bus address ($8000-$FFFF)
@   lr = return address (set by writememabs's `adr lr, 0f`)
@
@ vt_mmc3_forward is C, so its args are (r0=addr, r1=val) per AAPCS.
@ ============================================================================
    .global write_vt_rom
write_vt_rom:
    stmfd   sp!, {r12, lr}
    stmfd   sp!, {r0}
    
    mov     r1, r0              @ r1 = val (C arg 2)
    mov     r0, addy            @ r0 = addr (C arg 1)
    bl      vt_mmc3_forward
    
    ldmfd   sp!, {r0}
    
    @ Did vt_mmc3_forward change the banking configuration?
    ldr     r1, =vt_prg_dirty
    ldrb    r2, [r1]
    cmp     r2, #0
    beq     .Lvt_rom_write_done
    
    mov     r2, #0
    strb    r2, [r1]
    bl      vt_apply_prg_banks

.Lvt_rom_write_done:
    ldmfd   sp!, {r12, pc}


@ ============================================================================
@ Mapper number registration
@ ---------------------------------------------------------------------------
@ PocketNES cart.s includes a mappertbl byte array and a corresponding
@ jump table (mapperinit_tbl).  We declare MAPPER_VT = 253 here; add it
@ to both tables in cart.s using the existing .byte / .word pattern.
@ ============================================================================

    MAPPER_VT = 253

    .end