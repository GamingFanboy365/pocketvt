/* Host build of src/vt369_snd.c for tools/probes/vt369snd_test.c. */
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;
#define EWRAM_BSS
#define VT_MODE 1
u8 *rombase; u32 rommask; u8 *vt_chr_src; u32 vt_chr_mask;
u8 vt369_gpio_mask[4], vt369_gpio_latch[4];
