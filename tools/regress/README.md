# tools/regress: the cart regression check

One command checks a change against every test cart, on both toolchains,
against an older core (MAINTAINERS_GUIDE s.98):

```
python3 tools/regress/regress.py                 # working tree against origin/main, ~10 min
python3 tools/regress/regress.py --quick         # 8 carts, build_pvt only, state + speed
python3 tools/regress/regress.py --old 828cc9c --carts Zuma,Jumper_SMB_hack --toolchains docker
python3 tools/regress/regress.py --stage         # only put the ROMs in testroms/
```

The ROMs are never in git. `carts.json` lists every test cart with its md5,
and the runner finds the files by md5 wherever they are: `testroms/`, the
session's uploads folder (`~/.claude/uploads`), or any zip or folder named
with `--bundle`. The user keeps all of them in one file,
`pocketvt_testroms.zip`; a session that uploads only that zip has every cart.
A raw VG Pocket `vgpocket.bin` is wrapped with its NES 2.0 header on the way.

For each cart and toolchain (build_pvt.sh and the devkitARM Docker image, the
one CI uses) the old and the new core run three checks. **state** plays the
same input keyed to NES frames on both cores and compares NES RAM and the
picture at 15 frames; VT369 carts run on `-DVT369_SND_OFF` builds because
their sound HLE runs on GBA time. gbaplay applies the input exactly at the
NES frame (it single-steps near each key change). RAM must be identical
outside the byte ranges `carts.json` marks benign, the dead stack just below
an equal stack pointer, and RAM from a cart's `drift_from` frame on (VG
Pocket's launch); a difference is re-run on `-DVT_NO_SPEEDHACK` builds, and
one that goes away there is the idle-loop finder's race (s.97e), a WARN. A picture that differs but equals the old
core's picture of a neighbouring NES frame is display phase; any other
picture change is a WARN with an old | new | difference PNG next to it.
**ref** runs compare_furb against Furbtendulator at NES 150/400/700 (Start at
320, the frames the guide quotes) and on the state session's input at
200/450/700/1000/1400; struct may not drop. A picture change together with a
struct drop is a FAIL. **speed** measures NES frames per GBA second over
50 s with sound on; the mean may not drop, and no cart may stall.

Cores are cached in `~/.cache/pocketvt-regress` by source tree, toolchain and
defines, so only changed trees are rebuilt, and furb_cli's frames are cached
by ROM and input. The report goes to `regress_out/<time>/report.md` (and
`report.json`); the exit status is 1 when anything FAILs. Every WARN needs a
sentence in the guide section of the change that caused it.

furb_cli comes from the GamingFanboy365/furbtendulator repository
(`bin/furb_cli`) or from `python3 tools/furb_cli/build.py`; without it the
ref check is skipped and the report says so. Docker is started if it is
installed and not running.

The runner was tested against three planted faults (s.98): a 3000-iteration
loop in vt_bands_frame_end (Scramble speed 59.2 to 51.8, FAIL), INC zp
skipping a value ending in $F (state FAIL on Scramble and Table Soccer VT369,
still different with the speed hacks off) and sprites never enabled (ref FAIL
on Lucky Lawn Mower VT09 and on Scramble).
