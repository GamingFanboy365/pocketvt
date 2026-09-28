#!/usr/bin/env python3
"""Copy the Furbtendulator source tree into a build dir and make it build on a
case-sensitive POSIX filesystem: backslashes in #include become '/', and an
include whose path only matches case-insensitively is rewritten to the real
name.  The pristine tree (reference/Furbtendulator-main) is never modified."""
import os, re, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
if os.path.exists(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst, ignore=shutil.ignore_patterns('msvc100', '*.aps', '*.ico', '*.rc'))

def resolve(base, inc):
    """Return inc with each path component fixed to the on-disk case, or None."""
    cur = base
    out = []
    for part in inc.split('/'):
        if part in ('.', '..'):
            cur = os.path.normpath(os.path.join(cur, part)); out.append(part); continue
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = part if part in names else next((n for n in names if n.lower() == part.lower()), None)
        if hit is None:
            return None
        out.append(hit); cur = os.path.join(cur, hit)
    return '/'.join(out)

# MSVC-only constructs g++ rejects even with -fpermissive/-fms-extensions.
# Each fixup must match exactly once, so upstream changes are noticed.
FIXUPS = [
    # anonymous union of anonymous structs at file scope (Sunsoft 5B sound)
    ('src-mappers/src/Hardware/Sound/s_SUN5.cpp', r'static union\s*\{', 'static union SUN5_Regs {'),
    ('src-mappers/src/Hardware/Sound/s_SUN5.cpp', r'\};\s*\};\s*uint8_t select;',
     '};\n} SUN5_regs;\nuint8_t select;\n'
     + ''.join('#define %s SUN5_regs.%s\n' % (n, n) for n in
               ('tone', 'envelope', 'envhold', 'envaltr', 'envattk', 'envcont', 'byte7', 'byteB', 'byteC', 'byteD'))),
    # "const enum { ... };" declares no object (FDS sound)
    ('src-mappers/src/Hardware/Sound/s_FDS.cpp', r'const enum \{ TMOD=0, TWAV=1 \};', 'enum { TMOD=0, TWAV=1 };'),
    ('src-mappers/src/Hardware/Sound/s_FDS.cpp', r'const enum \{ EMOD=0, EVOL=1 \};', 'enum { EMOD=0, EVOL=1 };'),
    # variables of unnamed union type have no linkage in g++, so the extern
    # declaration in the _transfer file cannot bind; give the unions a tag
    ('src-main/src/plugThruDevice_SuperMagicCard.cpp', r'union \{(\s*uint8_t k8 \[64\]\[2\]\[4096\];)', 'union SMC_PRGRAM {\\1'),
    ('src-main/src/plugThruDevice_SuperMagicCard.cpp', r'union \{(\s*uint8_t k1 \[256\]\[1024\];)', 'union SMC_CHRRAM {\\1'),
    ('src-main/src/plugThruDevice_SuperMagicCard_transfer.cpp', r'extern union \{(\s*uint8_t k8 \[64\]\[2\]\[4096\];)', 'extern union SMC_PRGRAM {\\1'),
    ('src-main/src/plugThruDevice_SuperMagicCard_transfer.cpp', r'extern union \{(\s*uint8_t k1 \[256\]\[1024\];)', 'extern union SMC_CHRRAM {\\1'),
]
for rel, pat, rep in FIXUPS:
    p = os.path.join(dst, rel)
    text = open(p, encoding='utf-8', errors='surrogateescape').read()
    new, n = re.subn(pat, lambda m: m.expand(rep), text, count=1)
    if n != 1:
        sys.exit('prep_src: fixup did not apply to %s: %s' % (rel, pat))
    open(p, 'w', encoding='utf-8', errors='surrogateescape').write(new)

inc_re = re.compile(r'(#\s*include\s*")([^"]+)(")')
for d, _, files in os.walk(dst):
    for f in files:
        if not f.endswith(('.cpp', '.h', '.hpp', '.c')):
            continue
        p = os.path.join(d, f)
        raw = open(p, 'rb').read()
        text = raw.decode('utf-8', errors='surrogateescape')
        def fix(m):
            inc = m.group(2).replace('\\', '/')
            real = resolve(d, inc)
            return m.group(1) + (real or inc) + m.group(3)
        new = inc_re.sub(fix, text)
        if new != text:
            open(p, 'wb').write(new.encode('utf-8', errors='surrogateescape'))
