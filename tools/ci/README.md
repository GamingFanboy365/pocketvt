# tools/ci: the checks CI runs without game ROMs

`.github/workflows/build.yml` runs these on every pull request after the core
is built (MAINTAINERS_GUIDE s.98). Each one also runs locally as shown.

| Check | Command | Catches |
|---|---|---|
| No ROMs in git | `python3 tools/ci/check_no_roms.py` | an iNES file, an archive, a play ROM (a core with a builder.py game block), any other file over 2 MB |
| No fixed RAM addresses | `python3 tools/ci/lint_ram_addresses.py` | a numeric EWRAM/IWRAM address in code (s.97's NMI counter in CHR_DECODE); hardware addresses are listed with their reasons |
| IWRAM budget | the link itself (`src/gba_cart_my.ld`) | `.bss` past 0x03007B84, into the user stack |
| VT369 sound HLE | `tools/probes/vt369snd_test.c` | the batched HLE leaving Furbtendulator's per-tick model |
| Smoke ROMs | `python3 tools/ci/smoke/smoke.py --core pocketvt.gba` | a 6502 instruction, VT PRG banking or the 2bpp picture changing |
| Diagnostic builds | `EXTRA_CFLAGS=-DVT_NO_SPEEDHACK bash build_pvt.sh`, and ten more | a diagnostic define that no longer builds |

## Smoke ROMs

`smoke/roms.py` generates three small NES programs from Python
(`smoke/asm6502.py` is the assembler); nothing is read from a game ROM and the
generated files are never committed. **cpu** (mapper 0) runs 629 cases of the
official 6502 instructions (ADC/SBC over a grid of operands and carries,
logic, compares, BIT, shifts on A and memory, INC/DEC, transfers, every
addressing mode with zero-page and page wrap, JMP ($xxFF), the stack,
JSR/RTS/RTI/BRK, branches and the flag instructions) and stores each result
and P at $0200-$06FF. **vtbank** (NES 2.0 mapper 256, VT03, 64K PRG, no
CHR-ROM like the real dumps) reads back which 8K bank each window shows at
power-on and after $4107-$410B, COMR6 and the MMC3-style $8000/$8001 writes,
including a 4-bank outer window. **ppu** (mapper 0) draws a still picture:
four BG palettes by attribute, tiles of every colour, fine X scroll and eight
sprites with every flip, one behind the background.

`smoke/expect.json` holds the expectations. For cpu and vtbank it is
Furbtendulator's NES RAM at frame 60, byte for byte (py65 must agree on cpu
before `--bless` records it). For ppu it is PocketVT's own picture hash,
recorded only after compare_furb scored it against Furbtendulator (99.82%
struct; the residue is the known sprite row offset of the 3:4 scaling). A
failing ppu check means the picture changed: look at the PNG it writes and
re-bless only if the change is intended:

```
python3 tools/ci/smoke/smoke.py --bless --furb ../gamingfanboy365/furbtendulator/bin/furb_cli
```

A change to `roms.py` needs a re-bless too (the check compares ROM hashes).
