#!/usr/bin/env python3
"""Flag DAC-table entries whose hue is impossible for their palette column.

The NES palette is structured: $0x/$1x/$2x/$3x are the same hue at rising
brightness.  An entry far off its column's consensus hue is either a
calibration error or a genuine per-console DAC difference -- this script
CANNOT tell which, so treat hits as nominations for review against a capture,
never as an automatic correction.  See MAINTAINERS_GUIDE section 57c.

Usage: python3 palette_sanity.py ../src/ppu_vt.c
"""
import re, sys, colorsys

def grab(src, name):
    m = re.search(r'%s\[64\]\s*=\s*\{(.*?)\};' % re.escape(name), src, re.S)
    return [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{4})', m.group(1))] if m else None

def hue(v):
    r, g, b = (v & 31) / 31, ((v >> 5) & 31) / 31, ((v >> 10) & 31) / 31
    if max(r, g, b) - min(r, g, b) < 0.12:
        return None                       # grey: hue undefined
    return colorsys.rgb_to_hsv(r, g, b)[0] * 360

def apart(a, b):
    d = abs(a - b) % 360
    return min(d, 360 - d)

src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
ref = grab(src, 'vt_compat_rgb555')
for name in ('vt_compat_rgb555', 'vt_compat_rgb555_vg'):
    tab = grab(src, name)
    if not tab:
        continue
    bad = []
    for col in range(16):
        idx = [col, 0x10 + col, 0x20 + col, 0x30 + col]
        defined = [h for h in (hue(ref[i]) for i in idx) if h is not None]
        if not defined:
            continue
        consensus = sorted(defined)[len(defined) // 2]
        for i in idx:
            h = hue(tab[i])
            if h is not None and apart(h, consensus) > 60:
                bad.append(i)
    print('%s: %d suspect entries: %s' %
          (name, len(bad), ' '.join('$%02X' % i for i in sorted(bad))))
