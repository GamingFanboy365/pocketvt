#include "equates.h"

	.section	".init"
	global_func _start
	.align
	.arm
@---------------------------------------------------------------------------------
_start:
@---------------------------------------------------------------------------------
	b	rom_header_end

	.fill   156,1,0			@ Nintendo Logo Character Data (8000004h)
	.fill	16,1,0			@ Game Title
	.byte   0x30,0x31		@ Maker Code (80000B0h)
	.byte   0x96			@ Fixed Value (80000B2h)
	.byte   0x00			@ Main Unit Code (80000B3h)
	.byte   0x00			@ Device Type (80000B4h)
	.fill	7,1,0			@ unused
	.byte	0x00			@ Software Version No (80000BCh)
	.byte	0xf0			@ Complement Check (80000BDh)
	.byte	0x00,0x00    		@ Checksum (80000BEh)

@---------------------------------------------------------------------------------
rom_header_end:
@---------------------------------------------------------------------------------
	b	start_vector			@ This branch must be here for proper
						@ positioning of the following header.

	.GLOBAL	__boot_method, __slave_number
@---------------------------------------------------------------------------------
__boot_method:
@---------------------------------------------------------------------------------
	.byte   0				@ boot method (0=ROM boot, 3=Multiplay boot)
@---------------------------------------------------------------------------------
__slave_number:
@---------------------------------------------------------------------------------
	.byte   0				@ slave # (1=slave#1, 2=slave#2, 3=slave#3)

	.byte   0 				@ reserved
	.byte   0 				@ reserved
	.word   0    				@ reserved
	.word   0				@ reserved
	.word   0    				@ reserved
	.word   0    				@ reserved
	.word   0    				@ reserved
	.word   0    				@ reserved

    global_func start_vector
    .align
@---------------------------------------------------------------------------------
start_vector:
@---------------------------------------------------------------------------------
	mov	r0, #0x4000000			@ REG_BASE
	str	r0, [r0, #0x208]
	@ guide s.91: the faster cart timing (WAITCNT 0x4317) is no longer set
	@ here, blind, but by vt_waitcnt_probe below, once it has checked that the
	@ cart returns the same data at that timing.

	mov	r0, #0x12			@ Switch to IRQ Mode
	msr	cpsr, r0
	ldr	sp, =__sp_irq			@ Set IRQ stack
	mov	r0, #0x1f			@ Switch to System Mode
	msr	cpsr, r0
	ldr	sp, =__sp_usr			@ Set user stack

@---------------------------------------------------------------------------------
@ Enter Thumb mode
@---------------------------------------------------------------------------------
	add	r0, pc, #1
	bx	r0

	.thumb
	
	mov r7,#0
goback:
	ldr	r0, =__text_start
	lsl	r0, #5				@ Was code compiled at 0x08000000 or higher?
@	bcs     DoEWRAMClear			@ yes, you can not run it in external WRAM
	bcs     SkipEWRAMClear			@ yes, you can not run it in external WRAM

	mov     r0, pc
	lsl     r0, #5				@ Are we running from ROM (0x8000000 or higher) ?
	bcc     SkipEWRAMClear			@ No, so no need to do a copy.

@---------------------------------------------------------------------------------
@ We were started in ROM, silly emulators. :P
@ So we need to copy to ExWRAM.
@---------------------------------------------------------------------------------
	mov r7,#1
	
	mov	r2, #2
	lsl	r2, r2, #24			@ r2= 0x02000000
	ldr	r3, =__end__			@ last ewram address
	sub	r3, r2				@ r3= actual binary size
	mov	r6, r2				@ r6= 0x02000000
	lsl	r1, r2, #2			@ r1= 0x08000000

	bl	CopyMem
	ldr r0,=goback+1
	mov pc,r0

@	bx	r6				@ Jump to the code to execute

@---------------------------------------------------------------------------------
@DoEWRAMClear:					@ Clear External WRAM to 0x00
@---------------------------------------------------------------------------------
@	mov	r1, #0x40
@	lsl	r1, #12				@ r1 = 0x40000
@	lsl	r0, r1, #7			@ r0 = 0x2000000
@	bl	ClearMem

@---------------------------------------------------------------------------------
SkipEWRAMClear:					@ Clear Internal WRAM to 0x00
@---------------------------------------------------------------------------------

@---------------------------------------------------------------------------------
@ Copy initialized data (data section) from LMA to VMA (ROM to RAM)
@---------------------------------------------------------------------------------
	ldr	r1, =__data_lma
	ldr	r2, =__data_start__
	ldr	r4, =__data_end__
	bl	CopyMemChk

@---------------------------------------------------------------------------------
@ Copy internal work ram (iwram section) from LMA to VMA (ROM to RAM)
@---------------------------------------------------------------------------------
	ldr	r1,= __iwram_lma
	ldr	r2,= __iwram_start__
	ldr	r4,= __iwram_end__
	bl	CopyMemChk

@---------------------------------------------------------------------------------
@ Copy internal work ram overlay 0 (iwram0 section) from LMA to VMA (ROM to RAM)
@---------------------------------------------------------------------------------
	ldr	r2,= __load_stop_iwram0
	ldr	r1,= __load_start_iwram0
	sub	r3, r2, r1			@ Is there any data to copy?
	beq	CIW0Skip			@ no

	ldr	r2,= __iwram_overlay_start
	bl	CopyMem
@---------------------------------------------------------------------------------
CIW0Skip:
@---------------------------------------------------------------------------------
@ Copy external work ram (ewram section) from LMA to VMA (ROM to RAM)
@---------------------------------------------------------------------------------
	ldr	r1, =__ewram_lma
	ldr	r2, =__ewram_start
	ldr	r4, =__ewram_end
	bl	CopyMemChk

@---------------------------------------------------------------------------------
@ Clear BSS section to 0x00
@---------------------------------------------------------------------------------
	ldr	r0, =__bss_start__
	ldr	r1, =__bss_end__
	sub	r1, r0
	bl	ClearMem

@---------------------------------------------------------------------------------
@ Clear SBSS section to 0x00
@---------------------------------------------------------------------------------
	ldr	r0, =__sbss_start__
	ldr	r1, =__sbss_end__
	sub	r1, r0
	bl	ClearMem

@---------------------------------------------------------------------------------
CEW0Skip:
@---------------------------------------------------------------------------------
@ set heap end
@---------------------------------------------------------------------------------
	ldr	r1, =fake_heap_end
	ldr	r0, =__eheap_end
	str	r0, [r1]
@---------------------------------------------------------------------------------
@ global constructors
@---------------------------------------------------------------------------------
	ldr	r3, =__libc_init_array
	bl	_blx_r3_stub
#if VT_FAST_WAITCNT
@---------------------------------------------------------------------------------
@ guide s.91: pick the cart timing (vt_waitcnt_probe, in EWRAM below)
@---------------------------------------------------------------------------------
	ldr	r3, =vt_waitcnt_probe
	bl	_blx_r3_stub
#endif
@---------------------------------------------------------------------------------
@ Jump to user code
@---------------------------------------------------------------------------------
	ldr r0,=copiedfromrom
	str r7,[r0]
	
	mov	r0, #0				@ int argc
	mov	r1, #0				@ char	*argv[]
	ldr	r3, =main
	bl	_blx_r3_stub
@---------------------------------------------------------------------------------
@ Clear memory to 0x00 if length != 0
@---------------------------------------------------------------------------------
@ r0 = Start Address
@ r1 = Length
@---------------------------------------------------------------------------------
ClearMem:
@---------------------------------------------------------------------------------
	mov	r2,#3				@ These	commands are used in cases where
	add	r1,r2				@ the length is	not a multiple of 4,
	bic	r1,r2				@ even though it should be.

	beq	ClearMX				@ Length is zero so exit

	mov	r2,#0
@---------------------------------------------------------------------------------
ClrLoop:
@---------------------------------------------------------------------------------
	stmia	r0!, {r2}
	sub	r1,#4
	bne	ClrLoop
@---------------------------------------------------------------------------------
ClearMX:
@---------------------------------------------------------------------------------
	bx	lr

@---------------------------------------------------------------------------------
_blx_r3_stub:
@---------------------------------------------------------------------------------
	bx	r3

@---------------------------------------------------------------------------------
@ Copy memory if length	!= 0
@---------------------------------------------------------------------------------
@ r1 = Source Address
@ r2 = Dest Address
@ r4 = Dest Address + Length
@---------------------------------------------------------------------------------
CopyMemChk:
@---------------------------------------------------------------------------------
	sub	r3, r4, r2			@ Is there any data to copy?
@---------------------------------------------------------------------------------
@ Copy memory
@---------------------------------------------------------------------------------
@ r1 = Source Address
@ r2 = Dest Address
@ r3 = Length
@---------------------------------------------------------------------------------
CopyMem:
@---------------------------------------------------------------------------------
	mov	r0, #3				@ These commands are used in cases where
	add	r3, r0				@ the length is not a multiple	of 4,
	bic	r3, r0				@ even	though it should be.
	beq	CIDExit				@ Length is zero so exit

@---------------------------------------------------------------------------------
CIDLoop:
@---------------------------------------------------------------------------------
	ldmia	r1!, {r0}
	stmia	r2!, {r0}
	sub	r3, #4
	bne	CIDLoop
@---------------------------------------------------------------------------------
CIDExit:
@---------------------------------------------------------------------------------
	bx	lr

	.align
	.pool

#if VT_FAST_WAITCNT
@---------------------------------------------------------------------------------
@ vt_waitcnt_probe -- guide s.91.  Cart ROM at 3/1 wait states with the
@ prefetch buffer (WAITCNT 0x4317, what retail games set) is worth up to 70%
@ speed (s.79), but some flash carts and reproduction carts have memory too
@ slow for it, and the core then crashed before its first frame.  This runs
@ from EWRAM, so a cart that cannot keep up only returns wrong data here.  It
@ sums the core image at the power-on timing, switches to 0x4317 and sums it
@ four more times; any difference puts the power-on timing back.  Holding
@ SELECT at power-on skips the test and keeps the power-on timing.
@ vt_waitcnt_mode: 1 fast, 2 test failed (slow), 3 SELECT (slow).
@---------------------------------------------------------------------------------
	.section .ewram, "ax", %progbits
	.arm
	.align 2
	.global vt_waitcnt_probe
vt_waitcnt_probe:
	stmfd	sp!, {r4-r11, lr}
	mov	r11, #0x04000000
	add	r10, r11, #0x200		@ [r10, #4] = REG_WAITCNT
	ldr	r9, =vt_waitcnt_mode
	add	r0, r11, #0x100
	ldrh	r0, [r0, #0x30]			@ REG_KEYINPUT: a 0 bit is a pressed key
	tst	r0, #4				@ SELECT
	moveq	r0, #3
	beq	9f
	bl	vt_waitcnt_sum			@ reference, at the power-on timing
	mov	r8, r0
#ifdef VT_WAITCNT_TEST_FAIL
	eor	r8, r8, #1			@ test build: act as a cart that fails
#endif
	ldr	r0, =0x4317
	strh	r0, [r10, #4]
	mov	r7, #4
0:	bl	vt_waitcnt_sum
	cmp	r0, r8
	bne	8f
	subs	r7, r7, #1
	bne	0b
	mov	r0, #1
	b	9f
8:	mov	r0, #0
	strh	r0, [r10, #4]			@ back to the power-on timing
	mov	r0, #2
9:	strb	r0, [r9]
	ldmfd	sp!, {r4-r11, lr}
	bx	lr

@ Checksum of the core image (0x08000000 to __rom_end__): 4-word bursts
@ (sequential accesses), then 4096 scattered halfword and byte reads in its
@ first 64K (non-sequential).  r0 = sum; clobbers r1-r6, r12.
vt_waitcnt_sum:
	mov	r0, #0
	mov	r1, #0x08000000
	ldr	r2, =__rom_end__
	bic	r2, r2, #15
1:	ldmia	r1!, {r3-r6}
	add	r0, r3, r0, ror #31
	add	r0, r4, r0, ror #31
	add	r0, r5, r0, ror #31
	add	r0, r6, r0, ror #31
	cmp	r1, r2
	blo	1b
	mov	r3, #4096
	ldr	r4, =0x2468ACE1			@ seed
	ldr	r5, =1103515245
	ldr	r12, =12345
	ldr	r6, =0xFFFE
2:	mla	r4, r5, r4, r12
	and	r1, r6, r4, lsr #8
	add	r1, r1, #0x08000000
	ldrh	r2, [r1]
	add	r0, r2, r0, ror #31
	ldrb	r2, [r1, #1]
	add	r0, r2, r0, ror #31
	subs	r3, r3, #1
	bne	2b
	bx	lr
	.ltorg

	.section .sbss, "aw", %nobits
	.global vt_waitcnt_mode
vt_waitcnt_mode:
	.space	4
#endif

	.end
