#include "equates.h"

	.global font
	.global fontpal
	.text
 .align
 .pool

@ guide s.91: the save-type marker flash carts look for (EverDrive, EZ-Flash
@ and others pick the save memory they give a ROM by these strings).  The
@ core keeps its settings and saves in 32K/64K SRAM at 0x0E000000 (sram.c).
	.global vt_save_type_id
	.align 2
vt_save_type_id:
	.ascii "SRAM_V113"
	.align 2

@This is removed from memory after it's loaded in Multiboot builds (except for builds including Link Transfer)
#if MULTIBOOT
 .section .rdata, "ax", %progbits
#else
 .section .append, "ax", %progbits
#endif


font:
	.incbin "../src/font2.lz77"
fontpal:
	.incbin "../src/fontpal.bin"
