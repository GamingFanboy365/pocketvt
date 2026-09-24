#!/usr/bin/env python3
"""Re-declare a wrapped OneBus image's NES 2.0 Extended Console Type.

The VG Pocket (and any OneBus console that uses V16BEN, i.e. sets $2010 D6)
is VT09-class and has 4 KiB of CPU RAM, not 2.  Images wrapped with the old
recipe declare byte 13 = 0x00 ("regular NES/Famicom"), so PocketVT gives them
2 KiB and their zero page gets aliased over from $0800-$0FFF.

Also sets the PocketVT DAC-variant nibble (byte 13 bits 4-7, reserved by
NES 2.0 when the console type is Extended).  $2010 D6 is the 16-bit CHR BUS
bit, not a DAC id, so it cannot tell one VT09 board from another -- name the
DAC explicitly instead.

Usage:  python3 rewrap_onebus.py in.nes out.nes [vt03|vt09|vt32|vt369] [auto|default|vg]
"""
import sys

TYPES = {'vt03': 0x07, 'vt09': 0x08, 'vt32': 0x09, 'vt369': 0x0A}
DACS  = {'auto': 0, 'default': 1, 'vg': 2, 'llm': 3}

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    kind = TYPES[(sys.argv[3] if len(sys.argv) > 3 else 'vt09').lower()]
    dac  = DACS[(sys.argv[4] if len(sys.argv) > 4 else 'auto').lower()]
    d = bytearray(open(sys.argv[1], 'rb').read())
    if d[:4] != b'NES\x1a':
        print('not an iNES/NES 2.0 file')
        return 1
    before = ' '.join('%02x' % b for b in d[:16])
    d[7] = (d[7] & 0xFC) | 0x0B   # keep mapper mid-nibble, force NES 2.0 + ext console type
    d[13] = (dac << 4) | kind
    open(sys.argv[2], 'wb').write(bytes(d))
    print('before: %s\nafter : %s' % (before, ' '.join('%02x' % b for b in d[:16])))
    return 0

sys.exit(main())
