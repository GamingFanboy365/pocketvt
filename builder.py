#!/usr/bin/env python3
"""
PocketVT multicart builder.

Usage:
    python3 builder.py game1.nes [game2.nes ...]

Concatenates the PocketVT GBA core (pocketvt.gba) with one or more iNES
ROMs, producing play_me.gba ready to flash or load in an emulator.  Each
ROM gets a 48-byte header before its data:

    Offset  Size  Field
    0       32    Title (ASCII, NUL-padded, truncated to 31 chars)
    32      4     Length in bytes (little-endian)
    36      4     Flags     (currently always 0)
    40      4     Sprite-follow address (currently always 0)
    44      4     Reserved  (currently always 0)

This format matches PocketNES's builder.py header layout, so PocketVT
inherits compatibility with existing multicart conventions.  The .nes
header byte sequence and submapper bits get preserved verbatim, so a
mapper-256 sub-15 (Jungletac) ROM still announces itself to the loader.
"""

import os
import struct
import sys

EMU_FILE = "pocketvt.gba"
OUT_FILE = "play_me.gba"
HEADER_SIZE = 48
NAME_LEN = 32


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 builder.py game1.nes [game2.nes ...]",
              file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(EMU_FILE):
        print(f"CRITICAL ERROR: {EMU_FILE} not found.  Run 'make' first.",
              file=sys.stderr)
        sys.exit(1)

    with open(OUT_FILE, "wb") as f_out:
        with open(EMU_FILE, "rb") as f_emu:
            f_out.write(f_emu.read())

        print(f"Building multicart payload into {OUT_FILE}...\n")

        success_count = 0
        for nes_file in sys.argv[1:]:
            try:
                with open(nes_file, "rb") as f_nes:
                    nes_data = f_nes.read()
            except FileNotFoundError:
                print(f"[-] ERROR: {nes_file} not found.  Skipping.",
                      file=sys.stderr)
                continue

            # iNES sanity check -- not strictly required, but warns on
            # non-NES files getting baked into the multicart.
            if len(nes_data) < 16 or nes_data[:4] != b"NES\x1a":
                print(f"[!] WARN: {nes_file} doesn't look like iNES "
                      f"(missing 'NES\\x1a' magic).  Continuing anyway.",
                      file=sys.stderr)

            game_name = os.path.basename(nes_file)
            if game_name.lower().endswith(".nes"):
                game_name = game_name[:-4]

            game_name_bytes = (game_name[:NAME_LEN - 1]
                               .encode("ascii", "ignore")
                               .ljust(NAME_LEN, b"\0"))

            # 32s = title, then four little-endian u32 fields
            header = struct.pack(
                "<32sIIII",
                game_name_bytes,
                len(nes_data),
                0,    # flags
                0,    # spritefollow
                0,    # reserved
            )
            assert len(header) == HEADER_SIZE, "header layout drift"

            f_out.write(header)
            f_out.write(nes_data)

            print(f"[+] Injected: {game_name}  ({len(nes_data):,} bytes)")
            success_count += 1

    print(f"\nSuccessfully compiled {success_count} game(s) into {OUT_FILE}!")


if __name__ == "__main__":
    main()
