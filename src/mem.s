#include "equates.h"

@	.include "equates.h"

	.global NES_RAM
	.global NES_SRAM
	.global CHR_DECODE
	.global YSCALE_EXRTA
	.global YSCALE_LOOKUP
	
	.global IWRAM_CANARY_0
	.global IWRAM_CANARY_1
	.global IWRAM_CANARY_2

	.global TEXTMEM
	.global MULTIBOOT_LIMIT	

 .align
 .pool
 .section .bss_prefix, "ax", %progbits
 .subsection 0
 .align
 .pool
NES_RAM: .skip 0x800
NES_SRAM: .skip 0x2000

 .align
 .pool
 .section .bss.end
 .subsection 9999
 .align
 .pool

IWRAM_CANARY_0: .skip 4
IWRAM_CANARY_1: .skip 4
IWRAM_CANARY_2: .skip 4

@ --------------------------------------------------------------------------
@ CHR_DECODE and the YSCALE tables used to live here at the "end of IWRAM".
@ The VT fork's IWRAM growth pushed this block THROUGH the stacks (usr sp
@ 0x03007D60, irq sp 0x03007E00) and past the 32KiB boundary: YSCALE_LOOKUP
@ linked at 0x03008040, which MIRRORS to 0x03000040 = guest NES zero page
@ $40.  Consequences: update_sprites read the game's zero page as its Y
@ scale table (all sprites clamped to GBA y=0 -> the "player character
@ doesn't move / parked top-left" symptom), and spriteinit sprayed its ramp
@ over guest zp on every cart load.  Relocated to EWRAM (.sbss): the extra
@ EWRAM latency is a few cycles per sprite lookup / per CHR_DECODE fetch,
@ irrelevant next to the bug.  YSCALE_EXRTA must stay IMMEDIATELY before
@ YSCALE_LOOKUP -- spriteinit's unscaled fill starts at LOOKUP-80.
@ --------------------------------------------------------------------------
 .align
 .pool
 .section .sbss, "aw", %nobits
 .align

CHR_DECODE: .skip 0x400
YSCALE_EXRTA: .skip 0x50
YSCALE_LOOKUP: .skip 0x108

@Data END: 0x0300FC50
@Stack END: 0x0300FD80
@Breathing room: 304 bytes

