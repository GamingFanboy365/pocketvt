#!/usr/bin/env python3
"""check_no_roms.py -- no game ROM may ever be committed (the test carts are
copyrighted; they live in testroms/, gitignored, and in the user's
pocketvt_testroms.zip).  Fails on any tracked file that

  * starts with an iNES/NES 2.0 header ("NES\\x1a"), or has a ROM/archive extension;
  * is a GBA image other than the repo-root core (play ROMs carry games);
  * is the repo-root pocketvt.gba with a builder.py game block appended;
  * is larger than 2 MB (the core's ELF, ~1.3 MB, is the largest file).

    python3 tools/ci/check_no_roms.py      # exit 1 on a hit
"""
import os, struct, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BAD_EXT = ('.nes', '.unf', '.unif', '.fds', '.qd', '.nsf', '.nsfe', '.tnes', '.zip', '.7z', '.rar', '.sav', '.ss0', '.mb', '.agb')
CORE = {'pocketvt.gba', 'pocketvt.elf'}


def embedded_games(data):
    """builder.py appends 48-byte headers (32-byte title, u32 length, ...) each
    followed by an iNES image; look for one whose length fits."""
    hits, o = [], data.find(b'NES\x1a')
    while o >= 0:
        if o >= 48:
            ln = struct.unpack_from('<I', data, o - 16)[0]
            if 16 < ln <= len(data) - o and data[o - 48:o - 16].rstrip(b'\0').isascii():
                hits.append(o)
        o = data.find(b'NES\x1a', o + 1)
    return hits


def main():
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=REPO).decode().split('\0')
    bad = []
    for f in filter(None, files):
        p = os.path.join(REPO, f)
        if not os.path.isfile(p):
            continue
        size = os.path.getsize(p)
        with open(p, 'rb') as fh:
            head = fh.read(4)
        low = f.lower()
        if head == b'NES\x1a':
            bad.append('%s: iNES header' % f)
        elif low.endswith(BAD_EXT):
            bad.append('%s: ROM or archive extension' % f)
        elif low.endswith('.gba') and f not in CORE:
            bad.append('%s: a GBA image other than the core (play ROMs carry games)' % f)
        elif f == 'pocketvt.gba' and embedded_games(open(p, 'rb').read()):
            bad.append('%s: a builder.py game block is appended (a play ROM, not the core)' % f)
        elif size > 2 << 20 and f not in CORE:
            bad.append('%s: %d bytes' % (f, size))
    for b in bad:
        print('check_no_roms: ' + b)
    if bad:
        print('\nGame ROMs must never be committed (CLAUDE.md).  Remove the file from the commit; '
              'if it was pushed, the history must be rewritten by the repo owner.')
        return 1
    print('check_no_roms: %d tracked files, no ROMs' % len([f for f in files if f]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
