#!/bin/bash
# PocketVT build script (recreated per MAINTAINERS_GUIDE §2).
# Sandbox build recipe: plain arm-none-eabi-gcc + libgba HEADERS, no devkitPro.
# The Makefile in this directory is the original devkitARM build and needs
# DEVKITARM set; this script is what sessions 18-21 actually built with.
#
# Compiles src/ + src/Mappers into $BUILD (default: ../pvt_build next to the
# tree).  WARNING: it rm -rf's $BUILD first, which deletes builder.py, any
# .nes files and any play ROMs you left there -- recopy them after, and keep
# control ROMs somewhere else.
#
# Override with env vars if your layout differs:
#   LIBGBA=/path/to/libgba  BUILD=/path/to/builddir  bash build_pvt.sh
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src"
BUILD="${BUILD:-$(dirname "$HERE")/pvt_build}"
LIBGBA="${LIBGBA:-$(dirname "$HERE")/libgba-master}"
LIBGBA_INC="$LIBGBA/include"

rm -rf $BUILD
mkdir -p $BUILD
cd $BUILD

ARCH="-mthumb -mthumb-interwork"
CFLAGS="$EXTRA_CFLAGS -g -Wall -Os -mcpu=arm7tdmi -mtune=arm7tdmi -fomit-frame-pointer \
 -ffast-math -ffixed-r10 -std=gnu99 -fcommon \
 -Wno-error=incompatible-pointer-types -Wno-error=int-conversion \
 $ARCH -I$LIBGBA_INC -I$SRC"
ASFLAGS="$EXTRA_CFLAGS $ARCH -I$SRC"

for f in $SRC/*.c; do
  arm-none-eabi-gcc $CFLAGS -c "$f" -o "$(basename "${f%.c}").o"
done
for f in $SRC/*.s $SRC/Mappers/*.s; do
  b=$(basename "$f")
  [ "$b" = "gba_crt0_my.s" ] && continue
  arm-none-eabi-gcc -x assembler-with-cpp $ASFLAGS -c "$f" -o "${b%.s}.o"
done
arm-none-eabi-gcc -x assembler-with-cpp $ASFLAGS -c $SRC/gba_crt0_my.s -o gba_crt0_my.o
# LZ77UnCompVram + friends from libgba source not needed: io.s/BIOS provides.
echo "compile done: $(ls *.o | wc -l) objects"
arm-none-eabi-gcc -g -Os -mcpu=arm7tdmi -mthumb -mthumb-interwork -I$LIBGBA_INC -c $LIBGBA/src/Compression.c -o Compression.o
echo "Compression.o added"

# LINK (recovered from Makefile: gba_cart_my.ld, -nostartfiles, muldefs, libgba+libm)
OFILES=$(ls *.o | grep -v gba_crt0_my)
arm-none-eabi-gcc -g -mthumb -mthumb-interwork -Wl,-Map,pocketvt.map -Wl,-z,muldefs \
  -T $SRC/gba_cart_my.ld -nostartfiles $OFILES gba_crt0_my.o \
  -L$LIBGBA/lib -lm -o pocketvt.elf
arm-none-eabi-objcopy -O binary pocketvt.elf pocketvt.gba
/opt/devkitpro/tools/bin/gbafix pocketvt.gba 2>/dev/null || true
echo "link done: $(stat -c%s pocketvt.gba) bytes"

# Post-link: the globals block and equates.h offset table must agree.  See
# MAINTAINERS_GUIDE section 14 -- a silent drift here caused the multicart
# hang for three sessions.
python3 "$HERE/tools/check_globals.py" pocketvt.elf
