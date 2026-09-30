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
    ldr     r1, =write_vt4xxx
    ldr     r2, =vt_tk8007          @ mapper 419: $4016 also clocks the ADPCM MCU
    ldrb    r2, [r2]
    cmp     r2, #0
    ldrne   r1, =write_tk4xxx
    str_    r1, writemem_4
    ldreq   r2, =vt_w4_slow         @ s.87: the VRAM fast entry goes first,
    streq   r1, [r2]                @ s.89: then vt_w4_bank, then this
    ldreq   r2, =vt_w4_next
    ldreq   r1, =vt_w4_bank
    streq   r1, [r2]
    ldreq   r1, =write_vt4xxx_v
    streq_  r1, writemem_4

    @ Install the $4000-$40FF / $4100-$41FF READ hook -----------------
    @ Until session 12 there was NO read hook: vt_reg_read() existed but
    @ nothing ever called it, so every $41xx read returned stock open bus.
    @ That silently broke Lonely Island's palette upload -- see the $4119
    @ comment in vt_reg_read() -- and would break any VT title that probes
    @ its hardware.
    adr     r1, read_vt4xxx
    ldr     r2, =vt_tk8007
    ldrb    r2, [r2]
    cmp     r2, #0
    adrne   r1, read_tk4xxx
    str_    r1, readmem_4

    @ guide s.82: the VT369 maps the nametables into CPU space at
    @ $3000-$3FFF (h_OneBus.cpp readNT/writeNT).  Lucky Lawn Mower VT369
    @ mows its lawn by copying tiles there with LDA/STA ($zp),Y.
    ldr     r2, =vt_console
    ldrb    r2, [r2]
    cmp     r2, #0x0A
    bne     1f
    ldr     r1, =vt369_ppu_R
    str_    r1, readmem_2
    ldr     r1, =vt369_ppu_W
    str_    r1, writemem_2
    @ guide s.84: the embedded ROM at $1000-$1FFF, when the file has one
    ldr     r1, =vt369_misc
    ldr     r1, [r1]
    cmp     r1, #0
    ldrne   r1, =vt369_ram_R
    strne_  r1, readmem_0
    @ guide s.84: and the sound CPU's RAM at $4800-$4FFF
    ldr     r1, =read_vt369_4xxx
    str_    r1, readmem_4
    ldr     r1, =write_vt369_4xxx
    ldr     r2, =vt_w4_slow         @ s.87: behind the VRAM fast entry
    str     r1, [r2]                @ s.89: and vt_w4_bank
    ldr     r2, =vt_w4_next
    ldr     r1, =vt_w4_bank
    str     r1, [r2]
    ldr     r1, =write_vt4xxx_v
    str_    r1, writemem_4
1:

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
    mov     r0, #0x1F                @ every window
    b       .Lvt_apply_mask

@ s.87: re-map only the windows vt_recompute_prg_banks marked in
@ vt_prg_dirty (bit n = window n, bit 4 = VT369 $6000), and clear it.
    .global vt_apply_prg_dirty
vt_apply_prg_dirty:
    stmfd   sp!, {lr}
    ldr     r1, =vt_prg_dirty
    ldrb    r0, [r1]
    mov     r2, #0
    strb    r2, [r1]
.Lvt_apply_mask:
    ldr     r1, =vt_prg_apply_mask
    strb    r0, [r1]

    @ s21b26: vt_prg_banks is u16 -- outer bank pushes the number past 255.
    tst     r0, #1
    beq     1f
    ldr     r1, =vt_prg_banks
    ldrh    r0, [r1, #0]
    bl_long map89_
1:
    ldr     r1, =vt_prg_apply_mask
    ldrb    r0, [r1]
    tst     r0, #2
    beq     1f
    ldr     r1, =vt_prg_banks
    ldrh    r0, [r1, #2]
    bl_long mapAB_
1:
    ldr     r1, =vt_prg_apply_mask
    ldrb    r0, [r1]
    tst     r0, #4
    beq     1f
    ldr     r1, =vt_prg_banks
    ldrh    r0, [r1, #4]
    bl_long mapCD_
1:
    ldr     r1, =vt_prg_apply_mask
    ldrb    r0, [r1]
    tst     r0, #8
    beq     1f
    ldr     r1, =vt_prg_banks
    ldrh    r0, [r1, #6]
    bl_long mapEF_
1:

    @ s.81: VT369 $411C bit 6 maps PRG ROM bank $4112 at $6000-$7FFF (like
    @ mapper 40's ROM there); clear, it is the usual SRAM again.  Other
    @ consoles never touch $6000 here.
    ldr     r1, =vt_prg_apply_mask
    ldrb    r0, [r1]
    tst     r0, #0x10
    ldmeqfd sp!, {pc}
    ldr     r1, =vt_console
    ldrb    r0, [r1]
    cmp     r0, #0x0A
    ldmnefd sp!, {pc}
    ldr     r1, =vt_prg6_rom
    ldrb    r0, [r1]
    cmp     r0, #0
    beq     1f
    ldr     r0, =rom_R60
    str_    r0, readmem_6
    ldr     r0, =empty_W
    str_    r0, writemem_6
    ldr     r1, =vt_prg_bank6
    ldrh    r0, [r1]
    bl_long map67_
    ldmfd   sp!, {pc}
1:
    ldr     r0, =sram_R
    str_    r0, readmem_6
    ldr     r0, =sram_W
    str_    r0, writemem_6
    ldr     r0, =NES_RAM-0x5800
    str_    r0, memmap_6
    ldmfd   sp!, {pc}

@ ============================================================================
@ EWRAM-stack trampolines for the heavy VT C work the vblank IRQ runs
@ (guide s.77b, s.78e).  The IRQ runs in System mode on whatever user stack it
@ interrupted, and IWRAM leaves ~410-470 bytes of it.  Nested on the frame-end
@ chain, vt_chr_sync_flush overran into .bss (vt_prg_banks sits at its top):
@ Add 'em Up with split slots (s.77b), then Aero Gyrodine's boot on the
@ devkitARM build, before any split (s.78e).  If sp is still in IWRAM, switch
@ to vt_ewram_stack; if the interrupted code is already on it, keep going down
@ it.  In ROM: vblankinterrupt is IWRAM code, and IWRAM code comes out of the
@ same budget.
@ ============================================================================
.macro vt_ewram_trampoline name, target
    .global \name
\name:
    mov     r1, sp
    cmp     r1, #0x03000000
    ldrhs   sp, =vt_ewram_stack_top
    stmfd   sp!, {r1, lr}
    bl      \target
    ldmfd   sp!, {r1, lr}
    mov     sp, r1
    bx      lr
.endm
    vt_ewram_trampoline vt_chr4_rebuild_stacked, vt_chr4_rebuild_if_dirty
    vt_ewram_trampoline vt_16c_palette_fixup_stacked, vt_16c_palette_fixup
    vt_ewram_trampoline vt369_snd_fill_stacked, vt369_snd_fill    @ guide s.84
    vt_ewram_trampoline vt_bk_late_stacked, vt_bk_late            @ s.87
    .ltorg

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
    cmp     r1, #0x100              @ $40xx (joypad, $4015): straight to the
    ldrhs   pc, =IO_R               @ stock handler, no extra compares (s.80)
    cmp     r1, #0x19
    bne     2f
    ldr     r0, =vt_console         @ VT369 reports no TV-system bits (NRS
    ldrb    r0, [r0]                @ APU_VT369::IntRead $4119 = 0)
    cmp     r0, #0x0A
    moveq   r0, #0
    movne   r0, #0x18               @ XPORN | XF5OR6
    mov     pc, lr
2:
    @ guide s.80: VT32/VT369 multiply/divide results, $4130-$413D
    sub     r2, r1, #0x30
    cmp     r2, #0x0E
    bhs     3f
    ldr     r0, =vt_alu_on
    ldrb    r0, [r0]
    cmp     r0, #0
    ldrne   r0, =vt_alu_rd
    ldrneb  r0, [r0, r2]
    movne   pc, lr
3:
    @ guide s.80: fixed answers NintendulatorNRS h_OneBus.cpp readAPU gives
    @ every OneBus console ("various games, unknown purpose").  Lucky Lawn
    @ Mower VT369 spins at $E882 until $41B7 bit 2 is set.
    cmp     r1, #0x5C
    moveq   r0, #0x10
    moveq   pc, lr
    cmp     r1, #0x8A
    cmpne   r1, #0xB7
    moveq   r0, #0x04
    moveq   pc, lr
    cmp     r1, #0xB9
    moveq   r0, #0x80
    moveq   pc, lr
    @ guide s.84: VT369 GPIO ports $4140-$415F (OneBus h_OneBus_GPIO.cpp,
    @ no device attached): +0 mask, +2 latch, +3 ~mask, else $FF.  Lucky
    @ Lawn Mower counts $0FFA once a frame only while $414F reads $FF.
    sub     r2, r1, #0x40
    cmp     r2, #0x20
    ldrhs   pc, =IO_R               @ tail-jump; lr still points at the core
    ldr     r0, =vt_console
    ldrb    r0, [r0]
    cmp     r0, #0x0A
    ldrne   pc, =IO_R
    mov     r2, r2, lsr #3          @ port 0-3
    and     r1, r1, #7
    cmp     r1, #2
    ldreq   r0, =vt369_gpio_latch
    ldreqb  r0, [r0, r2]
    moveq   pc, lr
    cmp     r1, #3
    cmpne   r1, #0
    movne   r0, #0xFF
    movne   pc, lr
    ldr     r0, =vt369_gpio_mask
    ldrb    r0, [r0, r2]
    cmp     r1, #3
    mvneq   r0, r0
    andeq   r0, r0, #0xFF
    mov     pc, lr


@ ============================================================================
@ read_tk4xxx / write_tk4xxx -- mapper 419 (Taikee TK-8007 MCU) only.
@ $4017 reads carry the ADPCM chip's READY (bit 4) and NOT-clock (bit 3), and
@ $4016 bit 2 clocks the chip (vt_tk_write4016 in vt_regs.c).  Installed in
@ place of read_vt4xxx / write_vt4xxx so no other cart pays for them.
@ The read side is joy1_R (io.s) without the VS dip bits: no stack, no C.
@ ============================================================================
read_tk4xxx:
    ldr     r1, =0x4017
    cmp     r12, r1
    bne     read_vt4xxx
    ldr_    r0, joy1serial
    mov     r1, r0, asr #1
    and     r0, r0, #1
    ldrb_   r2, joystrobe
    movs    r2, r2
    streq_  r1, joy1serial
    ldr     r1, =vt_tk_4017
    ldrb    r1, [r1]
    orr     r0, r0, r1
    mov     pc, lr

write_tk4xxx:
    ldr     r1, =0x410F              @ ADPCM data nibble: the game streams it
    cmp     r12, r1                  @ twice per byte, so skip vt_reg_write
    andeq   r0, r0, #0x0F
    ldreq   r1, =vt_tk_data
    streqb  r0, [r1]
    moveq   pc, lr
    ldr     r1, =0x4016
    cmp     r12, r1
    bne     write_vt4xxx
    stmfd   sp!, {r0, r12, lr}
    bl      vt_tk_write4016          @ r0 = value
    ldmfd   sp!, {r0, r12, lr}
    ldr     pc, =IO_W                @ then the stock joypad strobe
    .ltorg

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
    cmpne   r1, #0x24                @ s.86: VT369 DMA source low byte
    ldrne   pc, =IO_W                @ plain APU write: no frame, no call

    stmfd   sp!, {r3, r12, lr}
    b       .Lvt_w_40_special

.Lvt_w_maybe41:
    cmp     r1, #0x41
    ldrne   pc, =IO_W                @ $42xx-$4Fxx: stock, no frame
    stmfd   sp!, {r3, r12, lr}
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
    cmp     r2, #0x24
    beq     .Lvt_w_4024

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
    @ s.86: bits 4-7 are the DMA source's low byte, except on VT369 while
    @ $201D bit 0 is set (Furbtendulator APU_VT369::IntWrite $4034)
    ldr     r1, =vt_console
    ldrb    r1, [r1]
    cmp     r1, #0x0A
    bne     1f
    ldr     r1, =vt369_reg
    ldrb    r1, [r1, #0x1D]
    tst     r1, #1
    bne     .Lvt_write_done
1:
    and     r1, r0, #0xF0
    ldr     r2, =vt_dma_lo
    strb    r1, [r2]
    b       .Lvt_write_done

@ ------------------------------------------------------------------
@ $4024 W -- s.86: on VT369 in enhanced mode ($201E != 0) the DMA source's
@ low byte (Zuma DMAs its palette from $8080 and its map from $8160; with
@ the low byte ignored the map came from $8100 and showed palette words).
@ Elsewhere it goes to the stock handler as before.
@ ------------------------------------------------------------------
.Lvt_w_4024:
    ldr     r1, =vt_console
    ldrb    r1, [r1]
    cmp     r1, #0x0A
    bne     1f
    ldr     r1, =vt369_reg
    ldrb    r1, [r1, #0x1E]
    cmp     r1, #0
    ldrne   r1, =vt_dma_lo
    strneb  r0, [r1]
    bne     .Lvt_write_done
1:
    bl_long IO_W
    b       .Lvt_write_done

@ ------------------------------------------------------------------
@ $4014 W -- DMA trigger.  Decide sprite vs video based on $4034 bit 0.
@ ------------------------------------------------------------------
.Lvt_w_4014:
    @ guide s.82: VT369 enhanced mode -- 256-byte DMA into the 512-byte OAM
    @ or the 1024-byte palette ($3C00-$3FFF), in C.  0 = not handled (a
    @ $2007 target below $3C00): fall through to the paths below.
    ldr     r1, =vt369_enh
    ldrb    r1, [r1]
    cmp     r1, #0
    beq     1f
    stmfd   sp!, {r0}
    bl      vt369_dma_4014
    cmp     r0, #0
    ldmfd   sp!, {r0}
    bne     .Lvt_write_done
1:
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

    @ Source address: (r0 << 8) | vt_dma_lo (s.86: $4034 & 0xF0, or $4024
    @ on VT369).  NES_RAM only goes to $07FF, so we'll mask each fetch.
    ldr     r4, =vt_dma_lo
    ldrb    r4, [r4]
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
    ldr     r4, =vt_nes_ram_mask     @ s21b56: 0x7FF or 0xFFF by cart RAM
    ldr     r4, [r4]                 @ size (was hardcoded 0x7FF)

.Lvt_w_4014_loop:
    @ s21b60: a VT video DMA reads its SOURCE over the CPU bus, so a source in
    @ PRG-ROM ($8000+) reads ROM (NintendulatorNRS runs these through the CPU
    @ core's DMA engine).  We read NES_RAM for every source, so Aero Gyrodine
    @ and Hex City X -- which DMA their whole title nametable from ROM
    @ ($A000-$A3FF, $8400-$87FF) -- got an empty screen.  $8000+ now reads
    @ through memmap_tbl (the per-8K biased pointers the 6502 core fetches
    @ opcodes with); below $8000 is the existing RAM path, unchanged.
    cmp     r6, #0x8000
    blo     1f
    mov     r0, r6, lsl #16
    mov     r0, r0, lsr #16          @ 16-bit CPU address (a DMA may wrap $FFFF)
    adr_    r1, memmap_tbl
    mov     r2, r0, lsr #13
    ldr     r1, [r1, r2, lsl #2]
    ldrb    r0, [r1, r0]
    b       2f
1:
    @ Load source byte (mask to NES RAM range)
    and     r0, r6, r4
    ldrb    r0, [r5, r0]
2:

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
    
    @ s.87: the exact 6502 time of this write, for vt_timer_reload_check
    @ ($4101 right after a timer expiry).  `timestamp` alone is the time of
    @ the last timeout event, which can be many lines back; the core's
    @ cycles register (r8) is only valid here, not inside the C call.
    ldr_    r1, cycles_to_run
    sub     r1, r1, cycles, asr #CYC_SHIFT
    ldr_    r2, timestamp
    add     r1, r1, r2
    ldr     r2, =vt_w41_now
    str     r1, [r2]

    @ We swap around so r0=addr, r1=val, then call our C handler
    ldr     r1, [sp]
    and     r0, r12, #0xFF
    bl      vt_reg_write
    
    ldmfd   sp!, {r0}           @ Pop value
    
    @ Did vt_reg_write change the banking configuration?
    ldr     r1, =vt_prg_dirty
    ldrb    r2, [r1]
    cmp     r2, #0
    beq     .Lvt_no_prg
    bl      vt_apply_prg_dirty

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
    ldmfd   sp!, {r3, r12, pc}


@ ============================================================================
@ vt_w4_bank -- s.89: $4107/$4108 with a new value (write_vt4xxx_v returns at
@ once for the old value) maps its one window here, from the mask, OR and
@ window order vt_recompute_prg_banks caches in vt_q.  Fire Fighter VT369
@ writes them ~60 times a NES frame; through vt_reg_write, the recompute and
@ vt_apply_prg_dirty that cost ~25K cycles.  Anything else, or before the
@ timer has armed the fast paths, goes to vt_w4_slow.  r0 = value,
@ r12 = address.  r3 is the 6502's N/Z (m6502_nz): writemem handlers may only
@ clobber r0-r2 and addy, so it is saved here and on the C paths below.
@ ============================================================================
    .global vt_w4_bank
vt_w4_bank:
#ifdef VT_NO_W4_BANK
    b       8f
#endif
    and     r1, r12, #0xFF00
    cmp     r1, #0x4100
    bne     8f
    and     r1, r12, #0xFF
    sub     r2, r1, #0x36
    cmp     r2, #1
    bls     5f                       @ $4136/$4137: the divider
    sub     r1, r1, #0x07
    cmp     r1, #1
    bhi     8f
    ldr     r2, =vt_w41_fast
    ldrb    r2, [r2]
    tst     r2, #2
    beq     8f
    ldr     r2, =vt
    add     r2, r2, r1
    strb    r0, [r2, #0x07]          @ vt.reg[7 + r1]
    stmfd   sp!, {r3, r12, lr}
    ldr     r2, =vt_q
    ldr     r3, [r2]                 @ mask
    and     r0, r0, r3
    ldr     r3, [r2, #4]             @ OR
    orr     r0, r0, r3
    add     r3, r2, #8
    ldrb    r1, [r3, r1]             @ window: 0/1, or 2 for $4107 under COMR6
    ldr     r2, =vt_prg_banks
    add     r2, r2, r1, lsl #1
    ldrh    r3, [r2]
    cmp     r3, r0
    ldmeqfd sp!, {r3, r12, pc}
    strh    r0, [r2]
    cmp     r1, #1
    bhi     3f
    beq     2f
    bl_long map89_
    ldmfd   sp!, {r3, r12, pc}
2:  bl_long mapAB_
    ldmfd   sp!, {r3, r12, pc}
3:  bl_long mapCD_
    ldmfd   sp!, {r3, r12, pc}
@ s.89: $4136/$4137, the divisor (VT32/VT369 ALU, as vt_reg_write).  Jewel
@ Master VT369 divides ~220 times in some frames; through vt_reg_write and
@ two libgcc divisions one such frame took four GBA frames.  $4136 only
@ stores its byte ($4136 reads 0: the result is ready at once); $4137 divides
@ with the BIOS Div (signed, so a dividend with bit 31 set goes to C).
5:  ldr     r1, =vt_w41_fast
    ldrb    r1, [r1]
    tst     r1, #1
    beq     8f
    ldr     r1, =vt_alu67
    strb    r0, [r1, r2]
    cmp     r2, #0
    moveq   pc, lr
    ldrh    r1, [r1]                 @ divisor
    ldr     r0, =vt_alu14
    ldr     r0, [r0]                 @ dividend
    cmp     r1, #0
    moveq   pc, lr                   @ / 0: nothing changes (vt_reg_write)
    cmp     r0, #0
    blt     8f
    stmfd   sp!, {r3, r12, lr}
    swi     0x060000                 @ Div: r0 = quotient, r1 = remainder
    ldr     r2, =vt_alu14
    str     r0, [r2]
    ldr     r2, =vt_alu56
    strh    r1, [r2]
    ldr     r2, =vt_alu_rd           @ read-back: +0 and +8 mirror
    str     r0, [r2]
    str     r0, [r2, #8]
    strb    r1, [r2, #4]
    strb    r1, [r2, #12]
    mov     r1, r1, lsr #8
    strb    r1, [r2, #5]
    strb    r1, [r2, #13]
    ldmfd   sp!, {r3, r12, pc}
8:  ldr     r1, =vt_w4_slow
    ldr     pc, [r1]
    .ltorg

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
    stmfd   sp!, {r3, r12, lr}
    stmfd   sp!, {r0}
    
    mov     r1, r0              @ r1 = val (C arg 2)
    mov     r0, addy            @ r0 = addr (C arg 1)
    bl      vt_mmc3_forward
    
    ldmfd   sp!, {r0}

    @ guide s.79: a CHR bank changed -> note the raster band (vt_band_mark),
    @ like ppu.s vt_band_hook does for $2012-$2017.  get_scanline_2 returns
    @ the NES scanline in addy (r12, restored from the stack below).
    ldr     r1, =vt_mmc3_chr_touched
    ldrb    r2, [r1]
    cmp     r2, #0
    beq     1f
    mov     r2, #0
    strb    r2, [r1]
    stmfd   sp!, {r0}
    bl_long get_scanline_2
    mov     r0, addy
    bl      vt_band_mark
    ldmfd   sp!, {r0}
1:
    @ Did vt_mmc3_forward change the banking configuration?
    ldr     r1, =vt_prg_dirty
    ldrb    r2, [r1]
    cmp     r2, #0
    beq     .Lvt_rom_write_done
    bl      vt_apply_prg_dirty

.Lvt_rom_write_done:
    ldmfd   sp!, {r3, r12, pc}


@ ============================================================================
@ write_vt4xxx_v -- writemem_4 on every VT cart but mapper 419 (s.87)
@
@ Two writes that need no C, in .vram1 because from cart ROM every
@ instruction and literal waits on the bus (~300 cycles a write):
@   $4107/$4108 with the value already there (Fire Fighter and Zuma rewrite
@   them 30-60 times a frame): nothing changes, return.
@   $4130-$4135, the VT32/VT369 multiplier (Zuma: ~29 multiplies, 116
@   writes a frame): as vt_reg_write's ALU case, the bytes go to vt_alu14 /
@   vt_alu56 and their read-back copies in vt_alu_rd (+0 and +8), and a
@   $4135 write multiplies.  $4136/$4137 (divide) stay in C.
@ vt_w41_fast (vt_regs.c) enables them: bit 0 the multiplier (vt_alu_on),
@ bit 1 the bank registers (not submapper 2, which swaps them); both only
@ once the first $41xx write has armed the VT timer.  Everything else goes
@ to the ROM handler in vt_w4_next.  r0 = value, r12 = address; only r1 and
@ r2 are used, so r0 and r12 come back as they were.
@ ============================================================================
    .pushsection .vram1, "ax", %progbits
    .align 2
    .global write_vt4xxx_v
write_vt4xxx_v:
    and     r1, r12, #0xFF00
    cmp     r1, #0x4100
    bne     9f
    and     r1, r12, #0xFF
    sub     r1, r1, #0x30
    cmp     r1, #5
    bls     2f
    add     r1, r1, #0x30-0x07      @ $4107 -> 0, $4108 -> 1
    cmp     r1, #1
    bhi     9f
    ldr     r2, =vt_w41_fast
    ldrb    r2, [r2]
    tst     r2, #2
    beq     9f
    ldr     r2, =vt
    add     r2, r2, r1
    ldrb    r2, [r2, #0x07]         @ vt.reg[] is at offset 0
    cmp     r2, r0
    bxeq    lr
9:
    ldr     r1, =vt_w4_next
    ldr     pc, [r1]
2:
    ldr     r2, =vt_w41_fast
    ldrb    r2, [r2]
    tst     r2, #1
    beq     9b
    ldr     r2, =vt_alu_rd
    add     r2, r2, r1
    strb    r0, [r2]
    strb    r0, [r2, #8]
    cmp     r1, #4
    ldrlo   r2, =vt_alu14
    ldrhs   r2, =vt_alu56-4
    strb    r0, [r2, r1]
    cmp     r1, #5
    bxne    lr
    ldr     r2, =vt_alu56
    ldrh    r1, [r2]
    ldr     r2, =vt_alu14
    ldrh    r2, [r2]
    mul     r1, r2, r1
    ldr     r2, =vt_alu14
    str     r1, [r2]
    ldr     r2, =vt_alu_rd
    str     r1, [r2]
    str     r1, [r2, #8]
    bx      lr
    .ltorg
    .popsection

@ ============================================================================
@ Mapper number registration
@ ---------------------------------------------------------------------------
@ PocketNES cart.s includes a mappertbl byte array and a corresponding
@ jump table (mapperinit_tbl).  We declare MAPPER_VT = 253 here; add it
@ to both tables in cart.s using the existing .byte / .word pattern.
@ ============================================================================

@ ============================================================================
@ read_vt369_4xxx / write_vt369_4xxx -- readmem_4 / writemem_4 on VT369
@ (s.84): $4800-$4FFF is the sound CPU's RAM ($1800-$1FFF, vt369_snd.c);
@ everything else goes on to the VT handlers.  Reads: no stack.
@ ============================================================================
@ vt369_ram_R -- readmem_0 on VT369 carts with an embedded ROM (s.84):
@ $1000-$1FFF reads the 4K misc ROM, $0000-$0FFF stays RAM (ram_R_mask).
    .global vt369_ram_R
vt369_ram_R:
    tst     r12, #0x1000
    ldreq   pc, =ram_R_mask          @ patched for 2K or 4K RAM (loadcart.c)
    ldr     r0, =vt369_misc
    ldr     r0, [r0]
    mov     r1, r12, lsl #20
    ldrb    r0, [r0, r1, lsr #20]
    mov     pc, lr

    .global read_vt369_4xxx
read_vt369_4xxx:
    tst     r12, #0x0800
    beq     read_vt4xxx
    ldr     r1, =vt369_sram
    mov     r0, r12, lsl #21
    ldrb    r0, [r1, r0, lsr #21]
    mov     pc, lr

    .global write_vt369_4xxx
write_vt369_4xxx:
    tst     r12, #0x0800
    beq     write_vt4xxx
    ldr     r1, =vt369_sram         @ store here; C only for the registers
    mov     r2, r12, lsl #21        @ vt369_snd_write acts on: $1FA2 and
    mov     r2, r2, lsr #21         @ $1840-$184F / $18A0-$18AF (start and
    strb    r0, [r1, r2]            @ stop masks).  Table Soccer streams
    bic     r2, r2, #0x0F           @ ~50 bytes a frame into $1800-$197F.
    cmp     r2, #0x040
    cmpne   r2, #0x0A0
    cmpne   r2, #0x7A0
    movne   pc, lr
    stmfd   sp!, {r0, r12, lr}
    mov     r1, r0                  @ val
    mov     r0, r12                 @ addr
    bl      vt369_snd_write
    ldmfd   sp!, {r0, r12, pc}

@ ============================================================================
@ vt369_ppu_R / vt369_ppu_W -- readmem_2 / writemem_2 on VT369 (s.82).
@ $3000-$3FFF is nametable RAM ($2000 | addr & $FFF, normal mirroring);
@ $2000-$2FFF stays the PPU register window.  The read side has no stack
@ and must keep addy (read-modify-write instructions reuse it).
@ ============================================================================
    .global vt369_ppu_R
vt369_ppu_R:
    tst     addy, #0x1000
    beq     vt369_ppu_R_reg
    mov     r0, addy, lsr #10
    and     r0, r0, #3
    add     r0, r0, #8               @ vram_map[8..11] = $2000-$2FFF pages
    adr_    r1, vram_map
    ldr     r1, [r1, r0, lsl #2]
    mov     r0, addy, lsl #22
    ldrb    r0, [r1, r0, lsr #22]
    mov     pc, lr

@ s.88: $2007 with the screen off and the address in $0000-$1FFF reads CHR
@ through the VT banks (vt_chr_read): vmdata_R runs first for the address
@ increment and returns the old read buffer, then the buffer gets the byte.
vt369_ppu_R_reg:
    and     r0, addy, #7
    cmp     r0, #2
    beq     vt369_stat_R
    cmp     r0, #7
    ldrne   pc, =PPU_R
    ldrb_   r0, screen_off
    cmp     r0, #0
    ldreq   pc, =PPU_R
    ldr_    r0, vramaddr
    mov     r0, r0, lsl #18
    cmp     r0, #0x80000000          @ $2000 << 18
    ldrhs   pc, =PPU_R
    mov     r0, r0, lsr #18
    stmfd   sp!, {r0, r3, r12, lr}
    adr     lr, 1f
    ldr     pc, =PPU_R
1:  ldr     r1, [sp]
    str     r0, [sp]                 @ the value this read returns
    mov     r0, r1
    ldr     r1, =vt_chr_read
    mov     lr, pc
    bx      r1
    strb_   r0, readtemp
    ldmfd   sp!, {r0, r3, r12, pc}

@ s.88: a $2002 read that a poll loop will repeat (LDA $2002 / AND #m /
@ BNE or BEQ back to it, the branch taken) ends the time slice: the value
@ can only change at a PPU event.  Jumper's NMI waits out vblank this way
@ (sprite-0 flag clear, from line 246 to the pre-render line), which at CPU
@ x3 was ~570 iterations a frame.  Opcodes decode through vt_op_dec.
vt369_stat_R:
    stmfd   sp!, {lr}
    adr     lr, 1f
    ldr     pc, =PPU_R               @ r0 = $2002, side effects as usual
1:  ldr     r2, =vt_op_dec
    ldrb    r1, [m6502_pc]
    ldrb    r1, [r2, r1]
    cmp     r1, #0x29                @ AND #imm
    ldmnefd sp!, {pc}
    ldrb    r1, [m6502_pc, #3]
    cmp     r1, #0xF9                @ back to the LDA abs
    ldmnefd sp!, {pc}
    ldrb    r1, [m6502_pc, #2]
    ldrb    r1, [r2, r1]
    ldrb    r2, [m6502_pc, #1]
    ands    r2, r2, r0
    movne   r2, #0xD0                @ the branch that is taken
    moveq   r2, #0xF0
    cmp     r1, r2
    andeq   cycles, cycles, #CYC_MASK
    ldmfd   sp!, {pc}

    .global vt369_ppu_W
vt369_ppu_W:
    tst     addy, #0x1000
    ldreq   pc, =PPU_W
    stmfd   sp!, {addy, lr}
    mov     addy, addy, lsl #20
    mov     addy, addy, lsr #20
    orr     addy, addy, #0x2000
    bl_long vram_write_direct
    ldmfd   sp!, {addy, pc}

@ ============================================================================
@ vt369_pal_W -- vram_write_tbl[15] while VT369 enhanced mode is on (s.82):
@ a $2007 write to $3C00-$3FFF lands in the 1024-byte VT369 palette.
@ In: r0 = data, addy = PPU address.  May clobber r1 and addy, like VRAM_pal.
@ ============================================================================
    .global vt369_pal_W
vt369_pal_W:
    ldr     r1, =vt369_pal
    mov     addy, addy, lsl #22
    strb    r0, [r1, addy, lsr #22]
    str     r2, [sp, #-4]!
    mov     r2, #1
    ldr     r1, =vt369_pal_dw       @ s.89: this palette word changed
    strb    r2, [r1, addy, lsr #24]
    ldr     r1, =vt369_pal_dirty
    strb    r2, [r1]
    ldr     r2, [sp], #4
    mov     pc, lr
    .pool

@ ============================================================================
@ vt369_nt_mark -- s.89: while VT369 enhanced mode is on, writeBG's spare
@ slot (writeBG_mapper_9_mod, ppu.s; only mapper 9/10 use it) branches here
@ right after the nametable byte is stored (vt369_nt_hook).  It marks the
@ nametable word for vt369_nt_diff instead of logging the write in the
@ PocketNES BG cache ring, which nothing reads in enhanced mode.  In EWRAM
@ because a B from IWRAM reaches it (ROM is out of range).
@ In: addy = offset in the 1K screen, r2 = screen base (0/0x400); returns
@ to writeBG's caller.  Clobbers r1, r2 and addy, like writeBG.
@ ============================================================================
    .section .ewram, "ax", %progbits
    .arm
    .align 2
    .global vt369_nt_mark
vt369_nt_mark:
    add     addy, addy, r2
    bic     addy, addy, #0x800      @ 4-screen: a spare mark, harmless
    ldr     r1, =vt369_nt_dw
    mov     r2, #1
    strb    r2, [r1, addy, lsr #2]
    ldr     r1, =vt369_nt_any
    strb    r2, [r1]
    bx      lr
    .pool
    .previous

    MAPPER_VT = 253

    .end