#!/usr/bin/env python3
"""gbafix.py -- make a GBA ROM header bootable on real hardware (guide s.79).

The GBA BIOS refuses to start a cartridge whose header does not carry the
Nintendo logo at 0x04-0x9F and a correct complement check byte at 0xBD.
devkitARM's `gbafix` writes both (the Docker build runs it); build_pvt.sh
called a gbafix that is not installed here and ignored the failure, so every
build_pvt.sh core had a zeroed logo -- it would not boot on a GBA, and mGBA
quietly skipped its BIOS for it ("Invalid logo, skipping BIOS").

The logo is copied from a ROM that already has a valid one (by default the
committed, Docker-built pocketvt.gba next to this repo's tools/), verified by
its CRC32, so no logo bytes live in this source.

    python3 tools/gbafix.py ROM.gba [--logo-from VALID.gba] [--check]

--check only reports (exit 1 if the header would not boot).  Otherwise the
header is fixed in place like `gbafix -cPNES -tPocketNES`: logo, title
"PocketNES", game code "PNES", fixed value 0x96 at 0xB2, complement at 0xBD.
Appended data (builder.py's games) is never touched.
"""
import os
import sys
import zlib

LOGO_CRC32 = 0xD0BEB55E          # CRC32 of the 156 logo bytes (mGBA checks the same)
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOGO_SRC = os.path.join(os.path.dirname(HERE), "pocketvt.gba")


def complement(h):
    return (-(sum(h[0xA0:0xBD]) + 0x19)) & 0xFF


def header_ok(h):
    return (zlib.crc32(bytes(h[4:0xA0])) & 0xFFFFFFFF) == LOGO_CRC32 and h[0xB2] == 0x96 \
        and h[0xBD] == complement(h)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    check = "--check" in args
    src = DEFAULT_LOGO_SRC
    if "--logo-from" in args:
        src = args[args.index("--logo-from") + 1]
    rom = [a for a in args if not a.startswith("--") and a != src][0]

    with open(rom, "rb") as f:
        h = bytearray(f.read(0xC0))
    if len(h) < 0xC0:
        sys.exit("gbafix.py: %s is too small for a GBA header" % rom)
    if check:
        ok = header_ok(h)
        print("gbafix.py: %s header %s" % (rom, "OK (boots on hardware)" if ok else
                                           "INVALID (the GBA BIOS will not boot it)"))
        sys.exit(0 if ok else 1)

    with open(src, "rb") as f:
        logo = f.read(0xA0)[4:0xA0]
    if (zlib.crc32(logo) & 0xFFFFFFFF) != LOGO_CRC32:
        sys.exit("gbafix.py: %s has no valid logo to copy (pass --logo-from a bootable ROM)" % src)
    h[4:0xA0] = logo
    h[0xA0:0xAC] = b"PocketNES".ljust(12, b"\0")
    h[0xAC:0xB0] = b"PNES"
    h[0xB2] = 0x96
    h[0xBD] = complement(h)
    with open(rom, "r+b") as f:
        f.write(h)
    print("gbafix.py: %s header fixed" % rom)


if __name__ == "__main__":
    main()
