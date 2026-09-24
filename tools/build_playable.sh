#!/bin/bash
# Build ready-to-run .gba files for hardware/emulator testing.
# Run from a directory containing pocketvt.gba, builder.py and the .nes files.
# NOTE: build_pvt.sh rm -rf's pvt_build, so re-copy builder.py and every .nes
# AFTER building the core, or you will silently package a ROM-less cart.
set -e
mkdir -p out
mk () { local name="$1"; shift; rm -f ./*.sav
        python3 builder.py "$@" >/dev/null
        cp play_me.gba "out/$name"; echo "  out/$name  <- $*"; }

mk pvt_vgpocket_vt09.gba        vg_vt09.nes
mk pvt_lucky_lawn_mower_vt09.gba Lucky_Lawn_Mower__VT09_.nes
mk pvt_multicart.gba            Star_Ally__VT03_.nes Lonely_Island.nes \
                                Scramble.nes Lucky_Lawn_Mower__VT09_.nes \
                                Push_the_Ball.nes Time_Pilot.nes
echo "done - copy out/*.gba to a flashcart or open in mGBA"
