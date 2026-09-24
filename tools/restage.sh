#!/bin/bash
# restage.sh -- build_pvt.sh rm -rf's pvt_build, wiping builder.py and every
# .nes.  Run this after EVERY build or the harness silently packages a
# ROM-less core ("Successfully compiled 0 game(s)").
cd /home/claude/pvt_build || exit 1
cp /home/claude/pocketvt/builder.py .
cp /mnt/user-data/uploads/*.nes . 2>/dev/null
cp /home/claude/pocketvt/testroms/*.nes . 2>/dev/null
cp /mnt/user-data/outputs/Lucky_Lawn_Mower_VT09_calibrated.nes /mnt/user-data/outputs/vgpocket_vt09.nes . 2>/dev/null
cp /tmp/vg/b/pc.c /tmp/vg/b/seq.c /tmp/shots.c . 2>/dev/null
for f in pc seq shots; do gcc -O2 $f.c -o $f -lmgba 2>/dev/null; done
echo "restaged: $(ls *.nes | wc -l) ROMs"
