@ vt_ovl.s -- the VT IWRAM overlay (guide s.96)
@
@ IWRAM is full, but the IWRAM code of konami.o, map023771.o, map1.o, map4.o
@ and map9.o (1372 bytes, gathered at the end of .iwram by gba_cart_my.ld)
@ is dead while a VT cart runs.  loadcart.c (iwram_ovl_select) copies this
@ blob over it for a VT cart and puts the mapper code back for any other.
@ The hot loops below ran as ARM or Thumb from cart ROM at 3-6 cycles an
@ instruction; here they run at 1.
@
@ Everything is position-independent: branches and adr only, and the blob
@ keeps its own literal pool.  C calls the routines through pointers set by
@ iwram_ovl_select (NULL while the blob is not installed: C does the work).
@ The linker script asserts vto_end - vto_blob fits.

	.section .text.vt_ovl, "ax", %progbits
	.arm
	.align 2
	.global vto_blob, vto_swap, vto_dec48, vto_fend, vto_step, vto_mix, vto_end

vto_blob:

@ ----------------------------------------------------------------------------
@ vto_swap(void **t, const u16 *pairs, u32 n): exchanges t[a] and t[b] for
@ each pair (a | b << 8).  vt_regs.c vt_optable_toggle: a $4169 toggle.
@ ----------------------------------------------------------------------------
vto_swap:
	cmp r2,#0
	bxeq lr
	stmfd sp!,{r4,r5}
1:	ldrh r3,[r1],#2
	and r12,r3,#0xFF
	mov r3,r3,lsr#8
	ldr r4,[r0,r12,lsl#2]
	ldr r5,[r0,r3,lsl#2]
	str r5,[r0,r12,lsl#2]
	str r4,[r0,r3,lsl#2]
	subs r2,r2,#1
	bne 1b
	ldmfd sp!,{r4,r5}
	bx lr

@ ----------------------------------------------------------------------------
@ vto_dec48(vt369_d48 *s, u32 k), k >= 1: k ticks of a playing 48-nibble
@ channel inside one half-frame (vt369_snd.c vt369_play48; vt369_dec48_c is
@ the same in C).  s: p, mix, frame, pos, last, out, vol, row (0-15), mode
@ (lead >> 4 & 3), as words; p, mix, frame, pos, last and out are written
@ back.  An even pos takes the next byte, an odd one its high nibble.
@ ----------------------------------------------------------------------------
vto_dec48:
	stmfd sp!,{r4-r9,r11,lr}
	mov r11,r0
	ldmia r0,{r0,r2,r3,r4,r5,r6,r7,r8,r9}
	adr r12,vto_step
	add r8,r12,r8,lsl#4		@ the row
	add pc,pc,r9,lsl#2		@ by prediction mode
	nop
	b d48_m0
	b d48_m1
	b d48_m2
	b d48_m3

	.macro D48 m
d48_m\m:
1:	tst r4,#1
	ldreqb r3,[r0],#1		@ even: next byte, low nibble
	andeq r12,r3,#15
	movne r12,r3,lsr#4		@ odd: high nibble
	ldrsb r12,[r8,r12]		@ step
	.if \m == 1
	add r12,r12,r6			@ + out
	.endif
	.if \m == 2
	add r12,r12,r6,lsl#1		@ + 2 out - last
	sub r12,r12,r5
	.endif
	.if \m == 3
	add r12,r12,r6			@ + out - (last >> 1)
	sub r12,r12,r5,asr#1
	.endif
	mov r5,r6			@ last = out
	mov r12,r12,lsl#24
	mov r6,r12,asr#24		@ out = (s8)o
	ldr lr,[r2]
	mla lr,r6,r7,lr			@ *mix++ += out * vol
	str lr,[r2],#4
	add r4,r4,#1
	subs r1,r1,#1
	bne 1b
	b d48_done
	.endm

	D48 0
	D48 1
	D48 2
	D48 3

d48_done:
	stmia r11,{r0,r2,r3,r4,r5,r6}
	ldmfd sp!,{r4-r9,r11,lr}
	bx lr

@ ----------------------------------------------------------------------------
@ vto_fend(u16 *b, u32 set): the 240 BG0CNT line values (b word-aligned):
@ bits 2, 3 and 7 cleared, then set ORed in (ppu_vt369.c vt369_frame_end).
@ ----------------------------------------------------------------------------
vto_fend:
	stmfd sp!,{r4,r5}
	orr r1,r1,r1,lsl#16
	mov r12,#30
1:	ldmia r0,{r2-r5}
	bic r2,r2,#0x8C
	bic r2,r2,#0x8C0000
	orr r2,r2,r1
	bic r3,r3,#0x8C
	bic r3,r3,#0x8C0000
	orr r3,r3,r1
	bic r4,r4,#0x8C
	bic r4,r4,#0x8C0000
	orr r4,r4,r1
	bic r5,r5,#0x8C
	bic r5,r5,#0x8C0000
	orr r5,r5,r1
	stmia r0!,{r2-r5}
	subs r12,r12,#1
	bne 1b
	ldmfd sp!,{r4,r5}
	bx lr

	.ltorg
	.align 2
@ vt369_step (vt369_snd.c), copied in by iwram_ovl_select
vto_step:
	.space 256
@ s.96: the sound fill's 128 mix words (vt369_snd.c vt369_mixp)
vto_mix:
	.space 512
vto_end:
