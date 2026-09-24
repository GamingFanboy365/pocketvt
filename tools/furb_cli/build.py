#!/usr/bin/env python3
"""Build furb_cli: a headless Linux build of Furbtendulator (the reference
VT/OneBus emulator) for comparing against PocketVT.

    python3 tools/furb_cli/build.py [--furb DIR] [--build DIR] [-j N]

--furb   Furbtendulator source root (default: reference/Furbtendulator-main/src;
         gitignored, never committed -- the user supplies it)
--build  output dir (default: tools/furb_cli/build, gitignored)

Produces BUILD/furb_cli and BUILD/Mappers/iNES.so (the iNES mapper pack,
loaded at run time exactly like Furbtendulator loads Mappers/iNES.dll).
The source file lists come from Furbtendulator's own Visual Studio projects.
Needs g++ with 32-bit multilib (apt install g++-multilib): the code assumes
Win32's 32-bit long.  Incremental: only changed sources are recompiled.
"""
import argparse, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

ap = argparse.ArgumentParser()
ap.add_argument('--furb', default=os.path.join(REPO, 'reference', 'Furbtendulator-main', 'src'))
ap.add_argument('--build', default=os.path.join(HERE, 'build'))
ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
args = ap.parse_args()

if not os.path.isdir(os.path.join(args.furb, 'src-main')):
    sys.exit('build.py: no Furbtendulator source at %s (unzip Furbtendulator-main.zip into reference/)' % args.furb)

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
ines_srcs = vcx_sources(os.path.join(args.furb, 'src-mappers', 'msvc100', 'INES.vcxproj'), 'src-mappers')

COMPAT = os.path.join(HERE, 'compat')
CXX = ['g++', '-m32', '-msse2', '-mfpmath=sse', '-O2', '-std=gnu++17', '-fpermissive', '-w', '-fno-operator-names',
       '-fwrapv', '-fno-strict-aliasing', '-I' + COMPAT, '-DUNICODE', '-D_UNICODE',
       '-DWIN32', '-D_WINDOWS', '-DNDEBUG', '-include', 'stdexcept', '-include', 'cstring']
# keep-inline: MSVC emits 'inline' members that other files call (PPU IncrementH)
MAIN_INC = ['-I' + os.path.join(SRC, 'src-main', 'src'), '-fkeep-inline-functions']
DLL_FLAGS = ['-fPIC', '-fvisibility=hidden', '-D_USRDLL', '-DINES_EXPORTS']

def obj_for(src, tag):
    rel = os.path.relpath(src, SRC).replace('/', '__').replace(' ', '_')
    return os.path.join(OBJ, tag, os.path.splitext(rel)[0] + '.o')

jobs = []
for s in main_srcs:
    jobs.append((s, obj_for(s, 'main'), CXX + MAIN_INC))
jobs.append((os.path.join(HERE, 'furb_cli.cpp'), os.path.join(OBJ, 'main', 'furb_cli.o'), CXX + MAIN_INC))
jobs.append((os.path.join(COMPAT, 'compat.cpp'), os.path.join(OBJ, 'main', 'compat.o'), CXX))
for s in ines_srcs:
    jobs.append((s, obj_for(s, 'ines'), CXX + DLL_FLAGS))
jobs.append((os.path.join(COMPAT, 'compat.cpp'), os.path.join(OBJ, 'ines', 'compat.o'), CXX + DLL_FLAGS))

hdr_time = max(os.path.getmtime(os.path.join(COMPAT, f)) for f in os.listdir(COMPAT))

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

main_objs = [j[1] for j in jobs if os.sep + 'main' + os.sep in j[1]]
ines_objs = [j[1] for j in jobs if os.sep + 'ines' + os.sep in j[1]]
so = os.path.join(B, 'Mappers', 'iNES.so')
exe = os.path.join(B, 'furb_cli')
def link(what, cmd):
    print('linking %s ...' % what)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
        sys.exit('build.py: linking %s failed\n%s' % (what, '\n'.join(undef) or r.stderr[-4000:]))

# -z defs: the pack must be self-contained, like a DLL (fail at build, not dlopen)
link('Mappers/iNES.so', ['g++', '-m32', '-shared', '-fvisibility=hidden', '-Wl,-z,defs', '-o', so] + ines_objs + ['-ldl'])
link('furb_cli', ['g++', '-m32', '-o', exe] + main_objs + ['-ldl'])
print('built %s' % exe)
