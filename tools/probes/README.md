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
| `bpcount` | How often given code addresses execute over N steps. Use it to check that a function runs at all. `VERBOSE=1` prints r0-r2 and lr at every hit, which shows the caller and the argument; `PEEK=ADDR` adds the word at ADDR. `FT=<frametotal> KEYS="first-last:mask,..." FROMFT=<nes frame>` starts at that NES frame with input (guide s.89). |
| `nestrace` | A 6502 instruction trace of PocketVT (`PC A X Y` at every opcode-handler entry), for comparing with `furb_cli --trace` to find the first instruction where the two CPUs part (guide s.88). Arguments: play ROM, `op_table`, `_lastbank`, step count. `FT=<frametotal> FROMFT=<n>` starts logging at NES frame n (whole GBA frames are run up to frame n-1 first, so long runs are quick) and `KEYS="first-last:mask,..."` presses keys. Speed-hacked loops show fewer iterations than the reference's trace. |
| `cycprof` | A cycle-weighted profile per function: each step is charged its real GBA cycles, so EWRAM and ROM wait states count. `FT=<frametotal> ABS=1` reports cycles per NES frame, for diffing two cores. `RANGE=lo-hi` lists 16-byte bins for addr2line; `BINS=1` lists the top bins. `FT=... WARMFT=<nes frame> KEYS="320-330:1,800-810:1"` warms up to that NES frame with input, to profile a scene past the title. |
| `fgrab` | Every GBA frame from FROM to TO to stdout (frametotal + 240x160 RGBX each), with `FT`/`KEYS` input. For one-frame glitches that compare_furb's NES-frame-keyed captures step over; guide s.90 scanned its rows for transient black lines. |
| `ftwatch` | Input keyed to NES frames, like `pvt_run` (any number of `FROM TO MASK` key spans). From NES frame T0 to T1 it logs every change of one word with the writer's pc and lr. Use it to find who (never) updates a VRAM tile or variable. The address must be word-aligned: mGBA rotates an unaligned 32-bit read. |
| `peek` | Every K GBA frames, prints words (or `ADDR:n` byte strings) at the given addresses. The quickest way to watch a few counters. `FT=<frametotal> KEYS="first-last:mask,..."` presses GBA keys over NES frames, as nestrace. |
| `memdump` | After N GBA frames, writes each `ADDR:LEN` range to a file. For NES-frame-keyed dumps use `PVT_DUMP` with pvt_run / compare_furb instead (tools/furb_cli/README.md). |
| `biosboot` | Boots a play ROM through the REAL GBA BIOS (`reference/gba_bios.bin`, never committed): reports when the ARM leaves the BIOS and `frametotal` every 60 frames. A cart with a bad header never leaves it on hardware; mGBA silently skips the BIOS instead (guide s.79). |
| `pvtwav` | Records PocketVT's audio (mGBA's mixed output, 32768 Hz mono WAV) between two NES frames, with NES-frame-keyed input. Compare with `furb_cli --wav` using `wavcmp.py REF PVT --ref-from F0 --frames N`: envelope correlation (rhythm; unrelated music scores 0.1-0.5), per-window spectrum, level, and PocketVT's speed. Below full speed the music plays slower, so judge envelope only at 60 NES fps. |
| `vt369snd_test` | Not an mGBA harness: builds src/vt369_snd.c natively (`gcc -O2 -I tools/probes -DVT369_SND_HOSTTEST tools/probes/vt369snd_test.c`) and checks the batched VT369 sound HLE against a port of Furbtendulator's per-tick one on random states. Must print 0 failing trials after any change to vt369_snd.c (guide s.84). |
| `spmin` | The lowest SP reached in each CPU mode. Mode 0x1F is the user stack. Compare it with `__bss_end__`: below that means stack overflow into .bss. |

MAINTAINERS_GUIDE s.77 shows them used together to find the raster-split
slot crash.
