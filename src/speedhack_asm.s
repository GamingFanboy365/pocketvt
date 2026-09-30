#include "equates.h"

 .align
 .pool
 .text
 .align
 .pool

@	.include "equates.h"
@	.include "mem.h"
@	.include "6502mac.h"
@	.include "6502.h"
@	.include "cart.h"
@	.include "memory.h"
@	.include "io.h"
@	.include "ppu.h"
@	.include "sound.h"

	@IMPORT speedhack_manager
	
	global_func call_quickhackfinder
	global_func set_cpu_hack
	
	.global SPEEDHACK_TEMP_BUF
	.global SPEEDHACK_INCS
	.global speedhacks
	global_func cpuhack_reset

	global_func dobnehack
	global_func dobranchhackjmp
	global_func dobranchhack2
	
	@imports from 6502.s
	@IMPORT branchhack_back
	
	@IMPORT _10
	@IMPORT _30
	@IMPORT _50
	@IMPORT _70
	@IMPORT _90
	@IMPORT _B0
	@IMPORT _D0
	@IMPORT _F0
	@IMPORT _4C

	@IMPORT _10y
	@IMPORT _30y
	@IMPORT _50y
	@IMPORT _70y
	@IMPORT _90y
	@IMPORT _B0y
	@IMPORT _D0y
	@IMPORT _F0y
	@IMPORT _4Cy

normalops:
	.word _10,_30,_50,_70,_90,_B0,_D0,_F0,_4C
	.global speedhackops	@ s.88: vt_regs.c re-installs a hack after a $4169 toggle
speedhackops:
#if VT09_ENCRYPTION
	@ s.86: JMP keeps the VT09 encryption commit (op_vt_JMP_abs)
	.word _10y,_30y,_50y,_70y,_90y,_B0y,_D0y,_F0y,op_vt_JMP_absy
#else
	.word _10y,_30y,_50y,_70y,_90y,_B0y,_D0y,_F0y,_4Cy
#endif

cpuhack_reset:
	stmfd sp!,{r3,lr}
	@erase all speedhacks
	mov r1,#0
	str_ r1,speedhack_pc
	str_ r1,speedhack_pc2
	ldr r0,=speedhacks
	mov r2,#64
	bl memset32_
	
	adr r2,normalops
	mov r0,#0x10
	ldr r1,=op_table
	mov r12,#8
0:
	ldr r3,[r2],#4
	str r3,[r1,r0,lsl#2]
	add r0,r0,#0x20
	subs r12,r12,#1
	bne 0b	
	ldr r3,[r2],#4
	mov r0,#0x4C
	str r3,[r1,r0,lsl#2]

#if VT_MODE
	@ ------------------------------------------------------------------
	@ The block above just stomped op_table[0x10/0x30/0x50/0x70/0x90/
	@ 0xB0/0xD0/0xF0/0x4C] with the *stock* (canonical) branch handlers.
	@ For VT carts that boot with opcode encryption enabled (Lonely
	@ Island et al. -- submapper >= 12), this destroys the permutation
	@ vt_rebuild_optable() installed at vt_reset time, sending fetched
	@ branch bytes to the wrong handler (e.g. encrypted 0xB0 from PRG
	@ should dispatch as _D0=BNE under sub-mapper-15, but cpuhack_reset
	@ above just wrote _B0=BCS there).  Re-run the permutation now so
	@ those nine slots get their encrypted-form handlers reinstalled.
	@
	@ Cheap: vt_rebuild_optable is O(256) IWRAM writes and runs only on
	@ cart reset.  Safe for non-VT carts: vt_active is 0, so we skip.
	@ Safe for VT carts with encryption disabled (sub < 12): the rebuild
	@ does an identity copy, leaving the stock handlers in place.
	@ ------------------------------------------------------------------
	ldr r0,=vt_active
	ldrb r0,[r0]
	cmp r0,#0
	beq 1f
	bl_long vt_rebuild_optable
1:
#endif
	
	ldmfd sp!,{r3,lr}
	bx lr

 .align
 .pool
 .section .vram1, "ax", %progbits
 .subsection 3
 .align
 .pool

call_quickhackfinder:
	@r0 = hack number
	@new code was added, TESTME
	@was hack used?
	ldr addy,=speedhacks
	add addy,addy,r0,lsl#4
	ldr   r1,[addy,#4]
	tst   r1,#0x0000FF00 @test hack was used
	beq 0f
	
	bic r1,r1,#0x00FF0000 @clear frames_hack_not_used
	bic r1,r1,#0x0000FF00 @clear hack was used
	str r1,[addy,#4]
	@check if we're installing the PPU hack
	ldr r0,=ppustat_
	ldrb r0,[r0]
	and r0,r0,#0x40
	mov r0,r0,lsr#6
	@maybe include code to make it not pick itself again
	b_long set_cpu_hack
0:
	stmfd sp!,{r3,lr}
	mov r2,r0
	mov r0,m6502_pc
	ldr_ r1,lastbank
	
	blx_long speedhack_manager
	
	ldmfd sp!,{r3,pc}

@typedef struct
@{
@	const u8 *hack_pc;
@	u8 num_incs;
@	u8 hack_was_used;
@	u8 frames_hack_not_used;
@	u8 __;
@	u32 cycles_per_iteration;
@	u32 divider;
@} speedhack_T;

	@ s.86: get_instruction_number and set_cpu_hack run at most once per
	@ frame; they live in ROM so .vram1 has room for the hot paths.
 .text
 .align

get_instruction_number:
	@addy = speedhack pc
	@output: r0=number of instruction (0-8), r1=instruction byte (the
	@ op_table index), r2 = the opcode it runs
	@returns eq if invalid
	movs addy,addy
	bxeq lr
	ldrb r1,[addy]
	mov r2,r1
#if VT_MODE
	@ s.86: under VT opcode encryption the byte in PRG is not the opcode
	@ it runs; classify the decoded opcode, index op_table by the raw byte.
	ldr r0,=vt_active
	ldrb r0,[r0]
	cmp r0,#0
	ldrne r2,=vt_op_dec
	ldrneb r2,[r2,r1]
#endif
	cmp r2,#0x4C
	moveq r0,#8
	beq 1f
	and r0,r2,#0x1F
	cmp r0,#0x10
	bne 0f
	sub r0,r2,#0x10
	mov r0,r0,lsr#5
1:
	movs addy,addy
	bx lr
0:
	movs r0,#0
	bx lr

set_cpu_hack:
	@r0 = speedhack number
	@ ------------------------------------------------------------------
	@ Encryption-aware (s.86, replacing the session-11 scheme that knew
	@ only submapper 15's bit5<->6 swap and refused every other row).
	@ op_table[raw] holds the handler for the opcode raw DECODES to, so a
	@ hack goes in at the raw byte found in PRG and its type comes from
	@ the decoded opcode (vt_op_dec, filled by vt_rebuild_optable).  The
	@ handler a removed hack gives back is vt_sh_norm[n] (the canonical
	@ table's, so op_vt_JMP_abs survives), and the default BNE hack goes
	@ to vt_sh_raw[6], the raw byte that decodes to $D0.
	@ ------------------------------------------------------------------
	strb_ r0,speedhacknumber

	@if it matches target's speedhack_pc, abort early.  (TESTME)
	ldr addy,=speedhacks
	add addy,addy,r0,lsl#4
	ldr r1,[r12]
	ldr_ addy,speedhack_pc2
	cmp r1,addy
	bxeq lr
	
	@remove old hack (r12 = speedhack_pc2)
	stmfd sp!,{lr}
	bl get_instruction_number
	beq 0f
	ldr r2,=normalops
#if VT_MODE
	ldr r12,=vt_active
	ldrb r12,[r12]
	cmp r12,#0
	ldrne r2,=vt_sh_norm
#endif
	ldr r0,[r2,r0,lsl#2]
	ldr r2,=op_table
	str r0,[r2,r1,lsl#2]
0:
	@add the default BNE hack
	ldr r0,=_D0y
	ldr r2,=op_table+0xD0*4
#if VT_MODE
	ldr r1,=vt_active
	ldrb r1,[r1]
	cmp r1,#0
	ldrne r1,=vt_sh_raw
	ldrneb r1,[r1,#6]
	ldrne r2,=op_table
	addne r2,r2,r1,lsl#2
#endif
	str r0,[r2]
	@set branch length
	mov r2,#3
	strb r2,[r0,#12]
	
	ldrb_ r0,speedhacknumber
	ldr addy,=speedhacks
	add addy,addy,r0,lsl#4
	mov r1,#0
	strb r1,[addy,#5]
	
	ldr addy,[addy]
	movs r0,addy
	streq_ r0,speedhack_pc
	str_ r0,speedhack_pc2
	
	bl get_instruction_number
	beq 0f
	cmp r0,#8
	ldr r2,=speedhackops
	ldr r0,[r2,r0,lsl#2]
	ldr r2,=op_table
	str r0,[r2,r1,lsl#2]

	addne r1,addy,#2
	addeq r1,addy,#1
	str_ r1,speedhack_pc
	
	beq 0f	@if jmp, don't set branch length
	
	@set branch length
	ldrb r1,[r12,#1]
	rsb r1,r1,#0x100
	strb r1,[r0,#12]
	
0:
	ldmfd sp!,{lr}
	bx lr

 .align
 .pool
 .section .vram1, "ax", %progbits
 .subsection 3
 .align

dobnehack:
	@cycles per iteration = (2+3)*3 or (2+4)*3 if branch crosses page
	sub r0,m6502_pc,#3
	and r0,r0,#0xFF00
	and r1,m6502_pc,#0xFF00
	cmp r0,r1
	mov r2,#5*3
	movne r2,#6*3
	
	@get number of possible iterations
	mov r0,cycles,asr#CYC_SHIFT
	ldreq r1,=0x11111112	@1/15
	ldrne r1,=0x0E38E38F	@1/18
	umull addy,r1,r0,r1
	
	@r1 = iterations possible, r2 = cost per iteration
	
	@get instruction
	ldrb r0,[m6502_pc,#-3]
#if VT_MODE
	@ s.86: the opcode it runs (VT opcode encryption)
	ldr r12,=vt_active
	ldrb r12,[r12]
	cmp r12,#0
	ldrne r12,=vt_op_dec
	ldrneb r0,[r12,r0]
#endif
	@is it a DEX CA \ DEY 88 \ INX E8 \ INY C8?
	cmp r0,#0xCA
	beq bnehack_dex
	cmp r0,#0x88
	beq bnehack_dey
	cmp r0,#0xE8
	beq bnehack_inx
	cmp r0,#0xE8
	bne 5f
bnehack_iny:  @TESTME
	@negate iterations and subtract 1
	rsbs r0,m6502_y,#0xFF000000
	beq 5f
	mov r0,r0,lsr#24
	cmp r1,r0
	@possible >= requested, run all requested
	addge m6502_y,m6502_y,r0,lsl#24
	@possible < requested, run all possible
	addlt m6502_y,m6502_y,r1,lsl#24
	b 0f
bnehack_inx:  @TESTME
	@negate iterations and subtract 1
	rsbs r0,m6502_x,#0xFF000000
	beq 5f
	mov r0,r0,lsr#24
	cmp r1,r0
	@possible >= requested, run all requested
	addge m6502_x,m6502_x,r0,lsl#24
	@possible < requested, run all possible
	addlt m6502_x,m6502_x,r1,lsl#24
	b 0f
bnehack_dey:	@TESTME
	@take away one iteration (let the last iteration happen naturally)
	subs r0,m6502_y,#0x01000000
	beq 5f
	mov r0,r0,lsr#24
	cmp r1,r0
	@possible >= requested, run all requested
	subge m6502_y,m6502_y,r0,lsl#24
	@possible < requested, run all possible
	sublt m6502_y,m6502_y,r1,lsl#24
	b 0f
bnehack_dex:	@TESTME
	@take away one iteration (let the last iteration happen naturally)
	subs r0,m6502_x,#0x01000000
	beq 5f
	mov r0,r0,lsr#24
	cmp r1,r0
	@possible >= requested, run all requested
	subge m6502_x,m6502_x,r0,lsl#24
	@possible < requested, run all possible
	sublt m6502_x,m6502_x,r1,lsl#24
0:
	mulge r0,r2,r0
	mullt r0,r2,r1
	
	@eat cycles
	sub cycles,cycles,r0,lsl#CYC_SHIFT
	b 5f

dobranchhackjmp:
	@second chance
	tst cycles,#BRANCH
	orr cycles,cycles,#BRANCH
	beq 6f
	
	bl usespeedhack
6:
#if VT09_ENCRYPTION
	b_long op_vt_JMP_abs
#else
	b_long _4C
#endif

dobranchhack2:
	@second chance
	tst cycles,#BRANCH
	orr cycles,cycles,#BRANCH
	beq 5f
	
	bl usespeedhack
5:
	ldrsb r0,[m6502_pc,#-1]
	b_long branchhack_back
	
usespeedhack:
	ldrb_ r2,speedhacknumber
	ldr r12,=speedhacks
	add r12,r12,r2,lsl#4
	stmfd sp!,{r2,r12,lr}
	
	ldr r1,[r12,#8]  @cycles_per_iteration
	
	mov r0,cycles,asr#CYC_SHIFT
	cmp r0,r1
	ldmltfd sp!,{r2,r12,pc}
	ldr r12,[r12,#12] @divider
	umull r2,r0,r12,r0
	mul r1,r0,r1	@multiply again
	sub cycles,cycles,r1,lsl#CYC_SHIFT
	
	ldmfd sp!,{r2,r12,lr}
	
	mov r1,#1
	strb r1,[r12,#5] @was used
	ldrsb r1,[r12,#4] @num incs
	movs r1,r1
	bxeq lr
	bmi 1f
	
	stmfd sp!,{r3}
	mov r3,r0
	
	ldr r12,=SPEEDHACK_INCS
	add r12,r12,r2,lsl#5
	@do the incs
0:
	ldrh r2,[r12],#2
	and r0,r2,#0xE000
	ldr r0,[m6502_mmap,r0,lsr#11]
	add r0,r0,r2
	ldrb r2,[r0]
	add r2,r2,r3
	strb r2,[r0]
	subs r1,r1,#1
	bne 0b
	ldmfd sp!,{r3}
	bx lr

1:
	@konami hack
	and r1,r1,#0x7F
	cmp r1,#8
	bxgt lr
	
	cmp r1,#5
	
	adr r2,konamihacklist
	ldr r1,[r2,r1,lsl#2]
	
	@copy 16 words
	stmfd sp!,{r4,r6,r7,lr}
	sub sp,sp,#0x40

	blt 1f  @skip copy to stack if it's a hack bypassable by multiplication

	mov r2,sp
	
	ldmia r1!,{r4,r6,r7,lr}
	stmia r2!,{r4,r6,r7,lr}
	ldmia r1!,{r4,r6,r7,lr}
	stmia r2!,{r4,r6,r7,lr}
	ldmia r1!,{r4,r6,r7,lr}
	stmia r2!,{r4,r6,r7,lr}
	ldmia r1!,{r4,r6,r7,lr}
	stmia r2!,{r4,r6,r7,lr}
	
	mov r1,sp
1:
	@get carry flag
	movs r2,cycles,lsr#1

	ldrb r6,[m6502_pc,#-2]
	ldrb r7,[m6502_pc,#-4]
	ldrb r2,[cpu_zpage,r6]
	ldrb r12,[cpu_zpage,r7]
	
	mov r2,r2,lsl#24
	mov r12,r12,lsl#24
	
	adr lr,afterkonamihack
	bx r1

konamihacklist:
	.word konamihack0,konamihack1,konamihack2,konamihack3,konamihack4,konamihack5,konamihack6,konamihack7,konamihack8
afterkonamihack:
	@out: register A = var A
	@     write var A back to memory
	@     zero,sign flags based on var A
	@     carry,overflow flags based on PSR
	mov m6502_nz,r2,asr#24
	mov m6502_a,r2
	strb m6502_nz,[cpu_zpage,r6]
	b 0f
afterkonamihack2:
	@for Double Dribble
	@out: register A = r2-1
	@     write r2-1 back to memory
	@     zero,sign flags based on r2
	@     carry,overflow flags based on PSR
	sub m6502_a,r2,#0x01000000
	mov m6502_nz,r2,asr#24
	strb m6502_nz,[cpu_zpage,r6]
0:
	bic cycles,cycles,#CYC_C | CYC_V
	orrcs cycles,cycles,#CYC_C
	orrvs cycles,cycles,#CYC_V
	
	add sp,sp,#64
	
	ldmfd sp!,{r4,r6,r7,pc}

	
	@r0 = number of iterations
	@r6 = guessed address of A @(PC-2)
	@r7 = guessed address of B @(PC-4)
	@r2 = value of guessed A
	@r12 = value of guessed B
	@carry in as current carry flag
konamihack0:   @mostgames
	@inc A
	@clc
	@lda A
	@adc B
	@sta A
	sub r0,r0,#1
	
	add r1,r12,#0x01000000
	mla r2,r0,r1,r2
	add r2,r2,#0x01000000
	adds r2,r2,r12
	bx lr

@const u8 konami_speedhack_0[]={ 0xE6,_XXX,0x18,0xA5,_XXX,0x65,_YYY,0x85,_XXX };  //A+=N*(B+1)   mostgames
@const u8 konami_speedhack_1[]={ 0xA5,_XXX,0x18,0x65,_YYY,0x85,_YYY }; //B+=A*N                  lifeforce

konamihack1:   @lifeforce
	ldrb r7,[m6502_pc,#-7]
	ldrb r12,[cpu_zpage,r7]
	mov r12,r12,lsl#24
	@lda B
	@clc
	@adc A
	@sta A
konamihack2:   @cv2
	@lda A
	@clc
	@adc B
	@sta A
	sub r0,r0,#1
	mla r2,r0,r12,r2
	adds r2,r2,r12
	bx lr

@const u8 konami_speedhack_2[]={ 0xA5,_XXX,0x18,0x65,_YYY,0x85,_XXX }; //A+=B*N                   cv2

@const u8 konami_speedhack_3[]={ 0xA5,_XXX,0x38,0x65,_YYY,0x85,_XXX }; //A+=(B+1)*N              jarin ko chie

konamihack4:   @goonies2
	@lda B
	@sec
	@adc A
	@sta A
	ldrb r7,[m6502_pc,#-7]
	ldrb r12,[cpu_zpage,r7]
	mov r12,r12,lsl#24
konamihack3:   @jarin ko chie
	@lda A
	@sec
	@adc B
	@sta A
	sub r0,r0,#1
	add r1,r12,#0x01000000
	mla r2,r0,r1,r2
	cmp r0,r0
	mov r1,r12,lsr#24
	subcs r1,r1,#0x00000100
	adcs r2,r2,r1,ror#8
	bx lr

@const u8 konami_speedhack_4[]={ 0xA5,_XXX,0x38,0x65,_YYY,0x85,_YYY }; //B+=(A+1)*N              goonies2

@const u8 konami_speedhack_5[]={ 0xA5,_XXX,0x65,_YYY,0x85,_XXX,0xE6,_XXX }; //A+=B+carry , A++   ddribble

konamihack5:   @ddribble
	ldrb r7,[m6502_pc,#-6]
	ldrb r12,[cpu_zpage,r7]
	mov r12,r12,lsl#24
	@lda A
	@adc B
	@sta A
	@inc A
0:
	mov r1,r12,lsr#24
	subcs r1,r1,#0x00000100
	adcs r2,r2,r1,ror#8
	add r2,r2,#0x01000000
	sub r0,r0,#1
	movs r0,r0  @test if r0==0 without touching carry or overflow flags
	bne 0b
	add pc,lr,#afterkonamihack2-afterkonamihack

@const u8 konami_speedhack_6[]={ 0xE6,_XXX,0xA5,_XXX,0x65,_YYY,0x85,_XXX }; //A++ , A+=B+carry   superc

konamihack6:   @superc
	@inc A
	@lda A
	@adc B
	@sta A
0:
	add r2,r2,#0x01000000
	mov r1,r12,lsr#24
	subcs r1,r1,#0x00000100
	adcs r2,r2,r1,ror#8
	sub r0,r0,#1
	movs r0,r0  @test if r0==0 without touching carry or overflow flags
	bne 0b
	@out: register A = var A
	@     write var A back to memory
	@     zero,sign flags based on var A
	@     carry,overflow flags based on PSR
	bx lr

@const u8 konami_speedhack_7[]={ 0xA5,_XXX,0x65,_YYY,0x85,_YYY }; //B+=A+carry                   contra

konamihack7:   @contra
	ldrb r7,[m6502_pc,#-6]
	ldrb r12,[cpu_zpage,r7]
	mov r12,r12,lsl#24
	@lda B
	@adc A
	@sta A
konamihack8:  @gradius2
	@lda A
	@adc B
	@sta A
0:	
	mov r1,r12,lsr#24
	subcs r1,r1,#0x00000100
	adcs r2,r2,r1,ror#8
	sub r0,r0,#1
	movs r0,r0  @test if r0==0 without touching carry or overflow flags
	bne 0b
	@out: register A = var A
	@     write var A back to memory
	@     zero,sign flags based on var A
	@     carry,overflow flags based on PSR
	bx lr

 .section .data.107, "w", %progbits

	.global _speedhack_pc, _speedhack_pc2	@ ppu_vt.c vt_prg_evict relocates them (guide s.77)
_speedhack_pc: .word 0
_speedhack_pc2: .word 0
	@ s21b6b: the storage label MUST follow the block's _name convention.
	@ Plain 'sh_encrypted' is ALSO the equates.h offset symbol (0x82C), and
	@ the equate wins, so ldr rX,=sh_encrypted loaded 0x82C and every write
	@ to this flag went to a dead address (mGBA: Bad memory Store8 0x82C).
	.global _sh_encrypted
_sh_encrypted: .byte 0
	.align
_speedhacknumber: .byte 0
 .byte 0
 .byte 0
 .byte 0
 	.global _deadbeef
_deadbeef: .word 0xDEADBEEF
 
