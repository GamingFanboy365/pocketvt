#!/usr/bin/env python3
"""Build furb_cli: a headless Linux build of Furbtendulator (the reference
VT/OneBus emulator) for comparing against PocketVT.

    python3 tools/furb_cli/build.py [--furb DIR] [--build DIR] [-j N]

--furb   Furbtendulator source root (default: ./Furbtendulator-src next to this
         script if present (standalone package), else the PocketVT tree's
         gitignored reference/Furbtendulator-main/src)
--build  output dir (default: tools/furb_cli/build, gitignored)

Produces BUILD/furb_cli and BUILD/Mappers/{iNES,FDS,NSF,VS}.so (the mapper packs,
loaded at run time exactly like Furbtendulator loads Mappers\*.dll).
The source file lists come from Furbtendulator's own Visual Studio projects.
Needs g++ with 32-bit multilib (apt install g++-multilib): the code assumes
Win32's 32-bit long.  Incremental: only changed sources are recompiled.
"""
import argparse, os, re, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

ap = argparse.ArgumentParser()
_bundled = os.path.join(HERE, 'Furbtendulator-src')
ap.add_argument('--furb', default=_bundled if os.path.isdir(_bundled) else
                os.path.join(REPO, 'reference', 'Furbtendulator-main', 'src'))
ap.add_argument('--build', default=os.path.join(HERE, 'build'))
ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
args = ap.parse_args()

if not os.path.isdir(os.path.join(args.furb, 'src-main')):
    sys.exit('build.py: no Furbtendulator source at %s (pass --furb, or unzip Furbtendulator-main.zip '
             'into PocketVT\'s reference/)' % args.furb)

B = os.path.abspath(args.build)
SRC = os.path.join(B, 'src')
OBJ = os.path.join(B, 'obj')
os.makedirs(os.path.join(B, 'Mappers'), exist_ok=True)

# 1. copy + POSIX-fix the sources when the pristine tree is newer than the copy
stamp = os.path.join(B, '.prepped')
def newest(root):
    t = 0
    for d, _, fs in os.walk(root):
        for f in fs:
            t = max(t, os.path.getmtime(os.path.join(d, f)))
    return t
prep = os.path.join(HERE, 'prep_src.py')
if (not os.path.exists(stamp) or os.path.getmtime(stamp) < max(newest(args.furb), os.path.getmtime(prep))):
    print('preparing sources ...')
    subprocess.check_call([sys.executable, prep, args.furb, SRC])
    open(stamp, 'w').close()

def vcx_sources(proj, subdir):
    text = open(proj, encoding='utf-8', errors='replace').read()
    out = []
    for rel in re.findall(r'ClCompile Include="([^"]+)"', text):
        rel = rel.replace('\\', '/')
        assert rel.startswith('../src/'), rel
        p = os.path.join(SRC, subdir, 'src', rel[len('../src/'):])
        if not os.path.exists(p):		# case differences (e.g. DLL/ vs Dll/)
            d, f = os.path.split(p)
            parent, leaf = os.path.split(d)
            for n in os.listdir(parent):
                if n.lower() == leaf.lower():
                    p = os.path.join(parent, n, f)
        out.append(p)
    return out

main_srcs = vcx_sources(os.path.join(args.furb, 'src-main', 'msvc100', 'Nintendulator.vcxproj'), 'src-main')
# the four mapper packs, each its own shared library like the Windows DLLs
PACKS = [('iNES', 'INES.vcxproj', 'INES_EXPORTS'), ('FDS', 'FDS.vcxproj', 'FDS_EXPORTS'),
         ('NSF', 'NSF.vcxproj', 'NSF_EXPORTS'), ('VS', 'VS.vcxproj', 'VS_EXPORTS')]
pack_srcs = {name: vcx_sources(os.path.join(args.furb, 'src-mappers', 'msvc100', proj), 'src-mappers')
             for name, proj, _ in PACKS}

COMPAT = os.path.join(HERE, 'compat')
CXX = ['g++', '-m32', '-msse2', '-mfpmath=sse', '-O2', '-std=gnu++17', '-fpermissive', '-w', '-fno-operator-names',
       '-fwrapv', '-fno-strict-aliasing', '-I' + COMPAT, '-DUNICODE', '-D_UNICODE',
       '-DWIN32', '-D_WINDOWS', '-DNDEBUG', '-include', 'stdexcept', '-include', 'cstring',
       '-include', 'locale', '-include', 'codecvt']	# MSVC's headers pull these in implicitly
# keep-inline: MSVC emits 'inline' members that other files call (PPU IncrementH)
MAIN_INC = ['-I' + os.path.join(SRC, 'src-main', 'src'), '-fkeep-inline-functions']
DLL_FLAGS = ['-fPIC', '-fvisibility=hidden', '-D_USRDLL', '-DFURB_PACK']

def obj_for(src, tag):
    rel = os.path.relpath(src, SRC).replace('/', '__').replace(' ', '_')
    return os.path.join(OBJ, tag, os.path.splitext(rel)[0] + '.o')

jobs = []
for s in main_srcs:
    jobs.append((s, obj_for(s, 'main'), CXX + MAIN_INC))
jobs.append((os.path.join(HERE, 'furb_cli.cpp'), os.path.join(OBJ, 'main', 'furb_cli.o'), CXX + MAIN_INC))
jobs.append((os.path.join(COMPAT, 'compat.cpp'), os.path.join(OBJ, 'main', 'compat.o'), CXX))
for name, _, define in PACKS:
    flags = CXX + DLL_FLAGS + ['-D' + define]
    for s in pack_srcs[name]:
        jobs.append((s, obj_for(s, name), flags))
    jobs.append((os.path.join(COMPAT, 'compat.cpp'), os.path.join(OBJ, name, 'compat.o'), flags))

hdr_time = max(os.path.getmtime(os.path.join(COMPAT, f)) for f in os.listdir(COMPAT))
# (furb_cli.cpp also depends on the headers; handled by the same rule)

def compile_one(job):
    src, obj, flags = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= max(os.path.getmtime(src), hdr_time):
        return None
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    r = subprocess.run(flags + ['-c', src, '-o', obj], capture_output=True, text=True)
    if r.returncode:
        return '%s\n%s' % (src, r.stderr[-4000:])
    return None

print('compiling %d sources (-j%d) ...' % (len(jobs), args.j))
with ThreadPoolExecutor(args.j) as ex:
    errors = [e for e in ex.map(compile_one, jobs) if e]
if errors:
    for e in errors[:5]:
        print(e, file=sys.stderr)
    sys.exit('build.py: %d file(s) failed to compile' % len(errors))

objs_of = lambda tag: [j[1] for j in jobs if os.sep + tag + os.sep in j[1]]
exe = os.path.join(B, 'furb_cli')
def link(what, cmd):
    print('linking %s ...' % what)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
        sys.exit('build.py: linking %s failed\n%s' % (what, '\n'.join(undef) or r.stderr[-4000:]))

# -z defs: the pack must be self-contained, like a DLL (fail at build, not dlopen)
for name, _, _ in PACKS:
    link('Mappers/%s.so' % name, ['g++', '-m32', '-shared', '-static-libstdc++', '-static-libgcc',
         '-Wl,--exclude-libs,ALL', '-fvisibility=hidden', '-Wl,-z,defs',
         '-o', os.path.join(B, 'Mappers', name + '.so')] + objs_of(name) + ['-ldl'])
# export exactly one symbol, furb_host_lookup, through which the packs reach
# the executable's dialogs / cursor / file pickers (compat.cpp)
link('furb_cli', ['g++', '-m32', '-static-libstdc++', '-static-libgcc',
     '-Wl,--dynamic-list=' + os.path.join(HERE, 'exports.list'), '-o', exe] + objs_of('main') + ['-ldl'])

# Furbtendulator's data files belong next to the program, as on Windows:
# cheats.cfg (cheat database), dip.cfg (DIP switch definitions), fastload.cfg,
# and the BIOS/ and samples/ folders (their dir.txt lists what goes there).
for data in (os.path.join(os.path.dirname(args.furb), 'bin'), os.path.join(args.furb, 'bin-data')):
    if os.path.isdir(data):
        for f in os.listdir(data):
            src = os.path.join(data, f)
            if f.endswith('.cfg'):
                shutil.copy(src, os.path.join(B, f))
            elif f in ('BIOS', 'samples') and os.path.isdir(src):
                os.makedirs(os.path.join(B, f), exist_ok=True)
                for g in os.listdir(src):
                    if not os.path.exists(os.path.join(B, f, g)):
                        shutil.copy(os.path.join(src, g), os.path.join(B, f, g))
        break
print('built %s' % exe)
