#!/bin/bash
# restage.sh -- build_pvt.sh rm -rf's the build dir, wiping builder.py and every
# .nes.  Run this after EVERY build or the harness silently packages a
# ROM-less core ("Successfully compiled 0 game(s)").
#
#   tools/restage.sh [builddir]
#
# Copies into the build dir (default: same default as build_pvt.sh):
#   * builder.py from this tree
#   * testroms/*.nes from this tree (local only -- gitignored)
#   * every .nes in $PVT_ROMS, if set (controls that are not in testroms/:
#     Star Ally, Lonely Island, Lucky Lawn Mower, VG Pocket, ...)
#   * every harness .c in $PVT_HARNESS, if set, compiled against libmgba
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-${BUILD:-$(dirname "$HERE")/pvt_build}}"
cd "$BUILD" || { echo "restage: no build dir $BUILD" >&2; exit 1; }

cp "$HERE/builder.py" .
cp "$HERE"/testroms/*.nes . 2>/dev/null
[ -n "$PVT_ROMS" ] && cp "$PVT_ROMS"/*.nes . 2>/dev/null
if [ -n "$PVT_HARNESS" ]; then
  for c in "$PVT_HARNESS"/*.c; do
    [ -e "$c" ] || continue
    cp "$c" . && gcc -O2 "$(basename "$c")" -o "$(basename "${c%.c}")" -lmgba 2>/dev/null
  done
fi
n=$(ls *.nes 2>/dev/null | wc -l)
echo "restaged $BUILD: $n ROMs"
[ "$n" -gt 0 ] || { echo "restage: WARNING no .nes staged" >&2; exit 1; }
