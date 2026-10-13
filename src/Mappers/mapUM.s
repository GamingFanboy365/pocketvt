#include "../equates.h"
#include "../6502mac.h"

@ ============================================================================
@ mapUM.s -- the UMC UM6578 (NES 2.0 mapper 405, console type $0B), guide
@ s.94.  The memory handlers and the timer timeout; the work is in C
@ (um6578.c).  Internal mapper number 251 (loadcart.c).
@
@   $0000-$1FFF  8K RAM: $0000-$07FF stays NES_RAM (zero page and stack go
@                there directly), $0800-$1FFF is um_ram_ptr
@   $2000-$2007  PocketNES for $2000-$2004 (and $2000 into the loopy model),
@                um_ppu_reg for $2005-$2007; $2008, $2040-$207F palette
@   $4016        PocketNES, then $4016 bit 1 (PRG bank bit 8, DMA page)
@   $4014        sprite DMA from $0800-$1FFF reads um RAM
@   $4031-$404F  um_reg_W / um_reg_R (timer, PRG banks, DMA)
@   $5000-$57FF  2K RAM
@
@ C entry points run on the EWRAM stack (um_tramp): the IWRAM user stack
@ has ~150 bytes to spare (CLAUDE.md).  Write handlers may clobber r0-r2
@ and addy only; read handlers must keep addy (read-modify-write).
@ ============================================================================

UMF_SCHED	= 1
UMF_CANCEL	= 2
UMF_IRQ		= 4
UMF_PRG		= 8

	global_func mapUMinit
	global_func um_ram_R
	global_func um_ram_W
	global_func um_ppu_R
	global_func um_ppu_W
	global_func um_io_R
	global_func um_io_W
	global_func um_apply_prg
	global_func um_hooks_off

	.text
	.align 2

@ C on the EWRAM stack, unless the code we interrupt is already on it.
@ r0-r2 pass through; r12 is scratch.
.macro um_tramp name, target
\name:
	mov r12,sp
	cmp r12,#0x03000000
	ldrhs sp,=vt_ewram_stack_top
	stmfd sp!,{r12,lr}
	bl \target
	ldmfd sp!,{r12,lr}
	mov sp,r12
	bx lr
.endm
	um_tramp um_reset_t, um_reset
	um_tramp um_ppu_reg_t, um_ppu_reg
	um_tramp um_r2007_t, um_r2007
	um_tramp um_reg_W_t, um_reg_W
	um_tramp um_reg_R_t, um_reg_R
	um_tramp um_timer_fire_t, um_timer_fire
	um_tramp um_hooks_t, um_hooks
	.global um_vblank_stacked		@ ppu.s's vblank call, via um_hooks
	um_tramp um_vblank_stacked, um_vblank_hook
	.ltorg

@----------------------------------------------------------------------------
mapUMinit:
@----------------------------------------------------------------------------
	.word void,void,void,void	@ $8000-$FFFF: no registers there

	stmfd sp!,{lr}
	ldr r0,=um_active
	mov r1,#1
	strb r1,[r0]

	@ byte 7 of an extended-console NES 2.0 header reads as VS to the
	@ iNES-era flags (mapVT.s has the long story)
	ldrb_ r1,cartflags
	bic r1,r1,#(SCREEN4+VS)
	strb_ r1,cartflags

	@ (the CPU reset after this writes $4017 = $40: 6502.s)

	bl um_reset_t
	bl um_apply_prg
	mov r0,#1			@ ppu.s's frame-end and vblank calls -> um6578.c
	bl um_hooks_t

	ldr r1,=um_ram_R
	str_ r1,readmem_0
	str_ r1,readmem_0_
	ldr r1,=um_ram_W
	str_ r1,writemem_0
	str_ r1,writemem_0_
	ldr r1,=um_ppu_R
	str_ r1,readmem_2
	ldr r1,=um_ppu_W
	str_ r1,writemem_2
	ldr r1,=um_io_R
	str_ r1,readmem_4
	ldr r1,=um_io_W
	str_ r1,writemem_4
	ldmfd sp!,{pc}

@----------------------------------------------------------------------------
um_hooks_off:	@ cart.s, every cart load: ppu.s's calls back to the VT functions
@----------------------------------------------------------------------------
	stmfd sp!,{r0-r3,lr}
	mov r0,#0
	bl um_hooks_t
	ldmfd sp!,{r0-r3,pc}

@----------------------------------------------------------------------------
um_apply_prg:	@ map the four 8K banks um6578.c computed
@----------------------------------------------------------------------------
	stmfd sp!,{lr}
	ldr r1,=um_prg8
	ldrh r0,[r1,#0]
	bl_long map89_
	ldr r1,=um_prg8
	ldrh r0,[r1,#2]
	bl_long mapAB_
	ldr r1,=um_prg8
	ldrh r0,[r1,#4]
	bl_long mapCD_
	ldr r1,=um_prg8
	ldrh r0,[r1,#6]
	bl_long mapEF_
	ldmfd sp!,{pc}

@----------------------------------------------------------------------------
um_ram_R:	@ $0000-$1FFF (addy may have run past $FFFF: readmem_0_)
@----------------------------------------------------------------------------
	bic addy,addy,#0x1e000
	cmp addy,#0x800
	ldrlob r0,[cpu_zpage,addy]
	movlo pc,lr
	ldr r1,=um_ram_ptr
	ldr r1,[r1]
	ldrb r0,[r1,addy]
	mov pc,lr
@----------------------------------------------------------------------------
um_ram_W:
@----------------------------------------------------------------------------
	bic addy,addy,#0x1e000
	cmp addy,#0x800
	strlob r0,[cpu_zpage,addy]
	movlo pc,lr
	ldr r1,=um_ram_ptr
	ldr r1,[r1]
	strb r0,[r1,addy]
	mov pc,lr

@----------------------------------------------------------------------------
um_ppu_W:	@ $2000-$3FFF.  The registers are not mirrored ($2008-$203F and
		@ $2080+ do nothing); $3xxx repeats $2xxx (Furbtendulator).
@----------------------------------------------------------------------------
	mov r1,addy,lsl#20		@ r1 = (addy & $FFF) << 20
	cmp r1,#0x008<<20
	blo 1f
	beq 8f
	sub r1,r1,#0x040<<20
	cmp r1,#0x040<<20
	movhs pc,lr
	ldr r1,=um_pal			@ $2040-$207F: the palette
	and r2,addy,#0x3F
	strb r0,[r1,r2]
	ldr r1,=um_pal_dirty
	mov r2,#1
	strb r2,[r1]
	mov pc,lr
8:	mov r1,#8			@ $2008
	b um_ppu_c
1:	and r2,addy,#7
	cmp r2,#5
	movhs r1,r2			@ $2005-$2007: ours alone
	bhs um_ppu_c
	cmp r2,#0
	beq 2f
	b_long PPU_W			@ $2001-$2004: PocketNES
2:	stmfd sp!,{r0,lr}		@ $2000: PocketNES (NMI, sprite size,
	bl_long PPU_W			@ increment), then the loopy model
	ldmfd sp!,{r0,lr}
	mov r1,#0
um_ppu_c:	@ r0 = value, r1 = register -> um_ppu_reg(value, register, now)
	stmfd sp!,{r3,lr}
	ldr_ r2,cycles_to_run
	sub r2,r2,cycles,asr#CYC_SHIFT
	ldr_ r12,timestamp
	add r2,r2,r12
	bl um_ppu_reg_t
	ldmfd sp!,{r3,pc}

@----------------------------------------------------------------------------
um_ppu_R:	@ keeps addy
@----------------------------------------------------------------------------
	mov r1,addy,lsl#20
	cmp r1,#0x008<<20
	blo 1f
	beq 8f
	sub r1,r1,#0x040<<20
	cmp r1,#0x040<<20
	movhs r0,addy,lsr#8		@ open bus
	movhs pc,lr
	ldr r1,=um_pal
	and r2,addy,#0x3F
	ldrb r0,[r1,r2]
	mov pc,lr
8:	ldr r1,=um_2008
	ldrb r0,[r1]
	mov pc,lr
1:	and r2,addy,#7
	cmp r2,#7
	beq 7f
	cmp r2,#2			@ $2002 clears the $2005/$2006 latch
	ldreq r1,=um_w
	moveq r2,#0
	streqb r2,[r1]
	b_long PPU_R
7:	stmfd sp!,{r3,r12,lr}		@ $2007
	bl um_r2007_t
	ldmfd sp!,{r3,r12,pc}

@----------------------------------------------------------------------------
um_io_W:	@ $4000-$5FFF
@----------------------------------------------------------------------------
	sub r1,addy,#0x5000
	cmp r1,#0x800
	blo 5f
	sub r1,addy,#0x4000
	cmp r1,#0x14
	beq 4f
	cmp r1,#0x16
	beq 6f
	sub r2,r1,#0x31
	cmp r2,#0x50-0x31
	blo um_io_reg			@ $4031-$404F
	b_long IO_W
5:	ldr r2,=um_ram2_ptr		@ $5000-$57FF
	ldr r2,[r2]
	strb r0,[r2,r1]
	mov pc,lr
6:	stmfd sp!,{r0,lr}		@ $4016: the pads, then bit 1
	bl_long IO_W
	ldmfd sp!,{r0,lr}
	mov r1,#0x16
	b um_io_reg
4:	and r2,r0,#0xFF			@ $4014: a page in $0800-$1FFF is um RAM;
	sub r2,r2,#0x08			@ dma_W reads the page through memmap_0
	cmp r2,#0x18
	bhs_long IO_W
	stmfd sp!,{lr}
	ldr r1,=um_ram_ptr
	ldr r1,[r1]
	str_ r1,memmap_0
	bl_long IO_W
	ldr r1,=NES_RAM
	str_ r1,memmap_0
	ldmfd sp!,{pc}

um_io_reg:	@ r0 = value, r1 = $40xx offset
	stmfd sp!,{r3,lr}
	ldr_ r2,cycles_to_run
	sub r2,r2,cycles,asr#CYC_SHIFT
	ldr_ r12,timestamp
	add r2,r2,r12
	stmfd sp!,{r2}
	bl um_reg_W_t
	ldmfd sp!,{r1}
	bl um_post
	ldmfd sp!,{r3,pc}

@ After a register write: r0 = flags (um6578.c UMF_*), r1 = now.
um_post:
	stmfd sp!,{r0,r1,lr}
	tst r0,#UMF_PRG
	blne um_apply_prg
	ldr r0,[sp]
	tst r0,#UMF_SCHED
	beq 1f
	ldr r1,=um_tsched
	ldr r1,[r1]
	ldr r0,=um_timer_handler
	adrl_ r12,mapper_timeout
	bl_long replace_timeout_2
	b 2f
1:	tst r0,#UMF_CANCEL
	beq 2f
	adrl_ r12,mapper_timeout
	bl_long remove_timeout
2:	ldr r0,[sp]
	ldrb_ r1,wantirq
	tst r0,#UMF_IRQ
	biceq r1,r1,#IRQ_MAPPER
	streqb_ r1,wantirq
	beq 9f
	tst r1,#IRQ_MAPPER
	bne 9f				@ already asserted
	orr r1,r1,#IRQ_MAPPER
	strb_ r1,wantirq
	ldr r1,[sp,#4]			@ raised by this write: take it at the
	ldr r0,=um_irq_now		@ next instruction boundary
	adrl_ r12,mapper_irq_timeout
	bl_long replace_timeout_2
9:	ldmfd sp!,{r0,r1,pc}

@----------------------------------------------------------------------------
um_io_R:	@ $4000-$5FFF, keeps addy
@----------------------------------------------------------------------------
	sub r1,addy,#0x5000
	cmp r1,#0x800
	blo 5f
	sub r1,addy,#0x4000
	sub r2,r1,#0x31
	cmp r2,#0x50-0x31
	bhs_long IO_R
	stmfd sp!,{r3,r12,lr}
	mov r0,r1
	ldr_ r2,cycles_to_run
	sub r1,r2,cycles,asr#CYC_SHIFT
	ldr_ r2,timestamp
	add r1,r1,r2
	bl um_reg_R_t
	ldmfd sp!,{r3,r12,pc}
5:	ldr r2,=um_ram2_ptr
	ldr r2,[r2]
	ldrb r0,[r2,r1]
	mov pc,lr

@----------------------------------------------------------------------------
@ Timeouts.  Entered from the timeout queue with r1 = this timestamp.
@----------------------------------------------------------------------------
um_timer_handler:		@ the planned expiry
	stmfd sp!,{r3}
	mov r0,r1
	bl um_timer_fire_t
	ldmfd sp!,{r3}
	stmfd sp!,{r0}
	tst r0,#UMF_SCHED		@ repeat mode: the next one
	beq 1f
	ldr r1,=um_tsched
	ldr r1,[r1]
	ldr r0,=um_timer_handler
	adrl_ r12,mapper_timeout
	bl_long install_timeout_2
1:	ldmfd sp!,{r0}
	ldrb_ r1,wantirq
	tst r0,#UMF_IRQ
	orrne r1,r1,#IRQ_MAPPER
	biceq r1,r1,#IRQ_MAPPER
	strb_ r1,wantirq
	movs r1,r1			@ CheckI only with something to take
	beq 2f
	b_long CheckI
2:	b_long _GO

um_irq_now:			@ a write raised the line
	ldrb_ r0,wantirq
	movs r0,r0
	beq 2f
	b_long CheckI
2:	b_long _GO
	.ltorg
