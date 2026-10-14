/* vt_state.h -- s.95: the "VTST" savestate block of a VT cart (vt_regs.c
 * vt_state_io / vt_state_apply, called from savestate2.c).  Each VT module
 * copies its machine state with VT_SV in a fixed order; save != 0 copies the
 * variables into the block at p, save == 0 out of it. */
#ifndef VT_STATE_H
#define VT_STATE_H

#define VT_STATE_MAX   8192            /* bytes; the block is about 4.4K */
#define VT_STATE_MAGIC 0x31545356u     /* "VST1": layout version */

#define VT_SV(p, save, x) do { \
        if (save) memcpy((p), &(x), sizeof(x)); else memcpy(&(x), (p), sizeof(x)); \
        (p) += sizeof(x); } while (0)

u32  vt_state_io(u8 *p, int save);     /* returns the block size, 0 = not a VT block */
void vt_state_apply(void);
u8  *vt_regs_state_io(u8 *p, int save);
u8  *ppu_vt_state_io(u8 *p, int save);
u8  *vt369_state_io(u8 *p, int save);
u8  *vt369_snd_state_io(u8 *p, int save);
void ppu_vt_state_apply(void);
void vt369_state_apply(void);
void vt369_snd_state_apply(void);

#endif
