@ 6502_vt.s -- VT03/VT09 extra opcode handlers for PocketVT
@
@ Register map (from equates.h):
@   m6502_a   = r5   value in bits 31-24; bits 0-23 = 0
@   m6502_x   = r6   value in bits 31-24; bits 0-23 = 0
@   m6502_y   = r7   value in bits 31-24; bits 0-23 = 0
@   cycles    = r8   NZ/carry/overflow flags packed with cycle counter
@   m6502_pc  = r9   16-bit NES program counter
@   m6502_nz  = r3   N=bit31, Z=0 when bits 0-7 are 0
@   cpu_zpage = r11  pointer to 256-byte zero-page mirror in GBA RAM
@   addy      = r12  address register (scratch per APCS)
@
@ Opcode handlers do NOT return -- they end with "fetch N" which
@ deducts N cycles and dispatches the next opcode directly.
@
@ VT03/VT369 extra opcodes (cross-verified against
@ Furbtendulator/src-main/src/OneBus_VT369.cpp dispatch table):
@   0x34  PLY        (impl, 1 byte)   -- Pull Y
@   0x3C  PLX        (impl, 1 byte)   -- Pull X
@   0x5A  TAD        (impl, 1 byte)   -- Transfer A to D
@   0x62  ADX  $nnnn (abs,  3 bytes)  -- ADC absolute affecting X
@   0x7A  TDA        (impl, 1 byte)   -- Transfer D to A
@   0xC2  PHX        (impl, 1 byte)   -- Push X
@   0xD2  PHY        (impl, 1 byte)   -- Push Y
@
@ The previous opcode assignments (0x03, 0x4B, 0x6B, 0x89, 0x8B, 0xAB, 0xBB)
@ from the original implementation notes were wrong; real hardware uses the
@ positions above, which is why ROMs like Lonely_Island hit the C2/D2 bytes
@ in their reset code and crashed when those decoded as illegal _xx.
@
@ VT09 extra:
@   JMP/JMPI wrappers that commit pending opcode encryption changes.

#include "equates.h"
#include "6502mac.h"

@ ============================================================================
@ vt_patch_optable
@ Called from mapVTinit (assembly) BEFORE vt_reset().  This ordering is
@ required so that vt_rebuild_optable's one-time snapshot of op_table[]
@ (taken inside vt_reset) captures the VT extras at their canonical
@ (decrypted) opcode positions.  Without that, the encryption-mode
@ permutation in vt_rebuild_optable would route encrypted bytes whose
@ intended decryption is a VT extra to the stock canonical handler
@ (typically illegal/NOP), and games like Lonely Island would fail to
@ execute their TAD / TDA / PHX / PHY opcodes after boot.
@ Writes handler addresses into op_table[] for VT extra opcodes.
@ Follows AAPCS: saves r4, returns via pc (from ldmfd).
@ ============================================================================
    .global vt_patch_optable
    .extern op_table

    .text
    .align 2

vt_patch_optable:
    stmfd   sp!, {r4, lr}
    ldr     r4, =op_table           @ base of 256-entry word table

    @ 0x34  PLY  (impl)
    ldr     r0, =op_vt_PLY
    str     r0, [r4, #(0x34 * 4)]

    @ 0x3C  PLX  (impl)
    ldr     r0, =op_vt_PLX
    str     r0, [r4, #(0x3C * 4)]

    @ 0x5A  TAD  (impl)
    ldr     r0, =op_vt_TAD
    str     r0, [r4, #(0x5A * 4)]

    @ 0x62  ADX  abs  -- ADC absolute with D-register interaction
    ldr     r0, =op_vt_ADX
    str     r0, [r4, #(0x62 * 4)]

    @ 0x7A  TDA  (impl)
    ldr     r0, =op_vt_TDA
    str     r0, [r4, #(0x7A * 4)]

    @ 0xC2  PHX  (impl)
    ldr     r0, =op_vt_PHX
    str     r0, [r4, #(0xC2 * 4)]

    @ 0xD2  PHY  (impl)
    ldr     r0, =op_vt_PHY
    str     r0, [r4, #(0xD2 * 4)]

    @ 0xA7  LDAXD  and  0xBF  LDAD  -- DELIBERATELY NOT PATCHED.
    @ The VT02 and VT03 datasheets (CPU Instruction Table, VT02 p47 /
    @ VT03 p48, rev A6) show the main CPU is plain 6502: cells 0xA7 and
    @ 0xBF are blank, as are 0x3C/0x34/0x5A/0x7A/0xC2/0xD2/0x62.  These
    @ opcodes exist only on the VT369-era *sound coprocessor*
    @ (Furbtendulator CPU_VT369_Sound, gated on reg4100[0x62]==0x0D),
    @ which PocketVT does not emulate -- on the GBA the sample streaming
    @ these perform is done by vt_adpcm_mix_gba reading PRG directly.
    @ See FINDINGS_vt_opcodes.md.  Do not enable these on the main CPU.

#if VT09_ENCRYPTION
    @ Replace JMP abs and JMP ind with encryption-commit wrappers
    ldr     r0, =op_vt_JMP_abs
    str     r0, [r4, #(0x4C * 4)]

    ldr     r0, =op_vt_JMP_ind
    str     r0, [r4, #(0x6C * 4)]
#endif

    ldmfd   sp!, {r4, pc}


@ ============================================================================
@ D-register storage (VT03 extended accumulator)
@ Kept in IWRAM for fast access.
@ ============================================================================
    .section .iwram, "ax", %progbits
    .align 2
vt_dreg:    .byte 0
    .align 2

@ ============================================================================
@ Opcode handlers (placed in .text like the rest of PocketNES handlers)
@ ============================================================================
    .text
    .align 2

@ ----------------------------------------------------------------------------
@ op_vt_PHX -- Push X  (opcode 0xC2)  3 cycles
@ Equivalent to PHA but for X: extract X byte, push to stack.
@ ----------------------------------------------------------------------------
op_vt_PHX:
    mov     r0, m6502_x, lsr#24     @ X value in bits 7-0
    push8   r0
    fetch   3

@ ----------------------------------------------------------------------------
@ op_vt_PHY -- Push Y  (opcode 0xD2)  3 cycles
@ ----------------------------------------------------------------------------
op_vt_PHY:
    mov     r0, m6502_y, lsr#24     @ Y value in bits 7-0
    push8   r0
    fetch   3

@ ----------------------------------------------------------------------------
@ op_vt_PLX -- Pull X  (opcode 0x3C)  4 cycles
@ pop8 delivers signed value into m6502_nz; we store it in m6502_x shifted.
@ ----------------------------------------------------------------------------
op_vt_PLX:
    pop8    m6502_nz                @ pull signed byte; also sets NZ
    mov     m6502_x, m6502_nz, lsl#24
    fetch   4

@ ----------------------------------------------------------------------------
@ op_vt_PLY -- Pull Y  (opcode 0x34)  4 cycles
@ ----------------------------------------------------------------------------
op_vt_PLY:
    pop8    m6502_nz
    mov     m6502_y, m6502_nz, lsl#24
    fetch   4

@ ----------------------------------------------------------------------------
@ op_vt_TAD -- Transfer A to D-register  (opcode 0x5A)  2 cycles
@ ----------------------------------------------------------------------------
op_vt_TAD:
    mov     r0, m6502_a, lsr#24     @ A value in bits 7-0
    ldr     r1, =vt_dreg
    strb    r0, [r1]
    fetch   2

@ ----------------------------------------------------------------------------
@ op_vt_TDA -- Transfer D-register to A  (opcode 0x7A)  2 cycles
@ Uses ldrsb to get a signed value for correct NZ flag behaviour, matching
@ PLA's pattern: pop8 -> m6502_nz -> lsl#24 -> m6502_a.
@ ----------------------------------------------------------------------------
op_vt_TDA:
    ldr     r1, =vt_dreg
    ldrsb   m6502_nz, [r1]          @ signed byte -> m6502_nz
    mov     m6502_a, m6502_nz, lsl#24
    fetch   2

@ ----------------------------------------------------------------------------
@ op_vt_ADX -- Add Memory (abs) to A with Carry  (opcode 0x62)  4 cycles
@
@ Functionally identical to ADC abs.  The D-register interaction documented
@ in the original notes is omitted until hardware traces confirm the exact
@ behaviour; for ROMs we've tested, treating this as plain ADC abs lets the
@ reset code progress without crashing on illegal-opcode garbage.
@
@ Pattern mirrors _6D (ADC abs): doABS to fetch the 16-bit operand into
@ addy, then perform readmem + ADC.
@ ----------------------------------------------------------------------------
op_vt_ADX:
    doABS
    readmemabs
    movs    r1, cycles, lsr#1
    subcs   r0, r0, #0x00000100
    adcs    m6502_a, m6502_a, r0, ror#8
    mov     m6502_nz, m6502_a, asr#24
    orr     cycles, cycles, #CYC_C+CYC_V
    bicvc   cycles, cycles, #CYC_V
    fetch_c 4


@ ============================================================================
@ VT09 JMP wrappers -- commit pending encryption state on JMP/JMPI
@ ============================================================================
#if VT09_ENCRYPTION

@ Byte offsets into VTState for the bool encryption fields.
@ Must match the GCC layout of VTState in vt_regs.h.
@ Verify with: arm-none-eabi-nm pocketvt_VT09.elf | grep vt_encryption
@ and check IMPLEMENTATION_NOTES.md for the static_assert recipe.
vt_state_enc_active  = 0x20
vt_state_enc_pending = 0x21
vt_state_enc_next    = 0x22

    .extern vt            @ VTState global from vt_regs.c
    .extern _JMP_abs      @ original JMP abs handler in 6502.s (label _4C)
    .extern _JMP_ind      @ original JMP ind handler in 6502.s (label _6C)

op_vt_JMP_abs:
    ldr     r0, =vt
    ldrb    r1, [r0, #vt_state_enc_pending]
    cmp     r1, #0
    beq     op_vt_jmp_abs_go
    ldrb    r1, [r0, #vt_state_enc_next]
    strb    r1, [r0, #vt_state_enc_active]
    mov     r1, #0
    strb    r1, [r0, #vt_state_enc_pending]
op_vt_jmp_abs_go:
    b_long  _4C                 @ tail-call original JMP abs

op_vt_JMP_ind:
    ldr     r0, =vt
    ldrb    r1, [r0, #vt_state_enc_pending]
    cmp     r1, #0
    beq     op_vt_jmp_ind_go
    ldrb    r1, [r0, #vt_state_enc_next]
    strb    r1, [r0, #vt_state_enc_active]
    mov     r1, #0
    strb    r1, [r0, #vt_state_enc_pending]
op_vt_jmp_ind_go:
    b_long  _6C                 @ tail-call original JMP ind

#endif
