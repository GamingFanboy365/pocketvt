#!/usr/bin/env python3
"""lint_ram_addresses.py -- no new fixed EWRAM/IWRAM addresses in code.

s.97: nmi_handler counted NMIs at the fixed address 0x020007DF for its whole
life, and as EWRAM .sbss grew that byte became part of CHR_DECODE (or vt_isim,
depending on the link): every NMI corrupted a table.  Addresses move on every
link; code must name a symbol.  This scans src/ for numeric constants inside
EWRAM (0x02000000-0x0203FFFF) and IWRAM (0x03000000-0x03007FFF), outside
comments, and fails on any that is not listed below with its reason.

    python3 tools/ci/lint_ram_addresses.py        # exit 1 on a new address
"""
import os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Fixed by the hardware, the BIOS or the memory map: allowed in any file.
ALLOW_ANY = {
    0x02000000: 'EWRAM start (region tests, multiboot image base)',
    0x03000000: 'IWRAM start: region tests; the NES RAM mirror base',
    0x03007D60: '__sp_usr (gba_crt0_my.s / the linker script)',
    0x03007E00: 'the old IRQ stack top, in comments and the stack probe',
    0x03007F00: '__sp_irq (s.96f)',
    0x03007FF8: 'BIOS interrupt-acknowledge word',
    0x03007FFA: 'BIOS soft-reset flag',
    0x03007FFC: 'BIOS interrupt-handler pointer',
}
# PocketNES's own fixed addresses, each with a reason it cannot move.
ALLOW_FILE = {
    ('src/main.c', 0x0203F900): 'PocketNES: multiboot/flash-cart hand-over area at the top of EWRAM',
    ('src/main.c', 0x0203FBFC): 'PocketNES: as above',
    ('src/main.c', 0x0203FC08): 'PocketNES: as above',
    ('src/sram.c', 0x02000004): 'PocketNES: header word of the image copied to EWRAM',
    ('src/mbclient.c', 0x02000000): 'multiboot client',
    ('src/mbclient.c', 0x03000000): 'multiboot client',
}

NUM = re.compile(r'\b0x0*([23])([0-9A-Fa-f]{6})\b')


def strip_comments(text, asm):
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    out = []
    for line in text.split('\n'):
        line = re.sub(r'//.*', '', line)
        if asm:
            line = re.sub(r'@.*', '', line)
        out.append(line)
    return out


def main():
    bad = []
    for d, _, files in os.walk(os.path.join(REPO, 'src')):
        for fn in sorted(files):
            if not fn.endswith(('.c', '.h', '.s', '.S')):
                continue
            p = os.path.join(d, fn)
            rel = os.path.relpath(p, REPO).replace(os.sep, '/')
            lines = strip_comments(open(p, encoding='latin-1').read(), fn.endswith(('.s', '.S')))
            for n, line in enumerate(lines, 1):
                for m in NUM.finditer(line):
                    v = int(m.group(1) + m.group(2), 16)
                    if not (0x02000000 <= v < 0x02040000 or 0x03000000 <= v < 0x03008000):
                        continue
                    if v in ALLOW_ANY or (rel, v) in ALLOW_FILE:
                        continue
                    bad.append('%s:%d: 0x%08X in code: %s' % (rel, n, v, line.strip()))
    for b in bad:
        print(b)
    if bad:
        print('\n%d fixed RAM address(es) in code.  Name a symbol instead (CLAUDE.md "No fixed RAM '
              'addresses in code", guide s.97); if one is truly fixed by the hardware, add it to '
              'tools/ci/lint_ram_addresses.py with its reason.' % len(bad))
        return 1
    print('lint_ram_addresses: no fixed EWRAM/IWRAM addresses in code outside the allow-list')
    return 0


if __name__ == '__main__':
    sys.exit(main())
