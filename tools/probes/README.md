# tools/probes: single-purpose mGBA harnesses

Small libmgba programs that each answer one question about a running
PocketVT play ROM (`builder.py` output). Build any of them with
`gcc -O2 NAME.c -o NAME -lmgba`. The mgba internal headers
(`mgba/internal/arm/arm.h`) come with `libmgba-dev`.

Addresses are arguments, never constants. Read them fresh from the ELF you
built, because they move on every link:

```
S(){ arm-none-eabi-nm pocketvt.elf | awk -v s=$1 '$3==s{print $1}'; }
./speed play_me.gba $(S frametotal) 1200
```

| Probe | Question it answers |
|-------|---------------------|
| `speed` | NES frames emulated per GBA second (`frametotal` deltas). The same number as NMIs per 60 frames for a running game. |
| `fhash` / `fdump` | A hash of framebuffer + palette RAM per GBA frame, for the frame-SET regression test. `fdump` also writes chosen frames as raw RGBX. |
| `vram_dump` | VRAM, EWRAM and IWRAM after N GBA frames: what is actually in a VRAM block, and where the PRG copies live. |
| `ramat` | NES RAM and VRAM when `frametotal` first reaches each listed NES frame. Diffing two cores this way shows where their game state diverges. |
| `pcprobe` | The 6502 PC saved at each NES frame start (`_m6502_pc` - `_lastbank`). |
| `armprobe` | Live ARM pc, r9 and `lastbank`, sampled inside frames. r9 is the 6502 host PC only while the ARM pc is in the CPU core. |
| `lbwatch` | Single-steps and reports every `lastbank` change. It stops at the first jump into NES RAM (host 0x03000000), with the preceding steps. |
| `memwatch` | Single-steps and reports every change of one word, with ARM pc, r0, r1 and lr, plus the first 8 `instant_prg_banks` entries. Use it to find who writes a memmap entry. |
| `bpcount` | How often given code addresses execute over N steps. Use it to check that a function runs at all. |
| `spmin` | The lowest SP reached in each CPU mode. Mode 0x1F is the user stack. Compare it with `__bss_end__`: below that means stack overflow into .bss. |

MAINTAINERS_GUIDE s.77 shows them used together to find the raster-split
slot crash.
