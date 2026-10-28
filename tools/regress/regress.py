#!/usr/bin/env python3
"""regress.py -- PocketVT's regression check over every test cart, in one command.

    python3 tools/regress/regress.py                  # working tree against origin/main
    python3 tools/regress/regress.py --quick          # 8 carts, build_pvt only, no reference
    python3 tools/regress/regress.py --old 828cc9c --carts Zuma,Jumper_SMB_hack
    python3 tools/regress/regress.py --stage          # only find the ROMs and fill testroms/

The test ROMs are never in git.  --stage (run by every check) finds them by
md5 in testroms/, the session's uploads folder (~/.claude/uploads) and any
zip or folder given with --bundle (the user keeps all of them as
pocketvt_testroms.zip), and copies them into testroms/ (gitignored).

For each cart and toolchain (build_pvt.sh, the devkitARM Docker image) the
old and the new core run three checks (MAINTAINERS_GUIDE s.98):

  state  NES RAM and the picture at 15 NES frames with input keyed to NES
         frames (gbaplay KEYSN/FTDUMP), on cores built with -DVT369_SND_OFF
         for VT369 carts (their sound HLE runs on GBA time, s.96a).  RAM must
         be identical outside the cart's known benign bytes (carts.json); a
         difference is re-checked on -DVT_NO_SPEEDHACK builds, because the
         idle-loop finder samples on GBA time (s.97e).  A picture that differs
         is accepted when it is the old core's picture of the next or the
         previous NES frame (display phase); otherwise it is a WARN with a
         side-by-side PNG.
  ref    compare_furb against Furbtendulator (furb_cli): NES 150/400/700 with
         Start at 320 (the frames the guide quotes), and NES 200/450/700/
         1000/1400 on the state session's input.  struct must not drop.
  speed  NES frames per GBA second over 50 s, sound on, input keyed to NES
         frames: the mean must not drop, and no cart may get stuck.

Cores are cached by source tree, toolchain and defines (~/.cache/
pocketvt-regress), so a second run against the same --old builds only the new
core.  Writes OUT/report.md and OUT/report.json; exits 1 if anything FAILs.
"""
import argparse, concurrent.futures as cf, datetime, hashlib, json, os, re, shutil
import struct as st, subprocess, sys, tempfile, threading, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MANIFEST = os.path.join(HERE, 'carts.json')

# ------------------------------------------------------------------ sessions
# Input keyed to NES frames, GBA key bits (A 1, B 2, Select 4, Start 8, Right 16).
# The state session is s.97's: Start at 100, 300 and 500, Right held from 600
# with A tapped every 50 frames.
STATE_FRAMES = [60, 120, 200, 260, 320, 400, 450, 520, 600, 700, 800, 900, 1000, 1200, 1400]
STATE_SPANS = [(100, 104, 8), (300, 304, 8), (500, 504, 8), (600, 1400, 16)] + \
              [(f, f + 5, 1) for f in range(650, 1401, 50)]
# The speed session is sbench's (s.97): Right held 60 frames, then Right+A 10.
SPEED_SPANS = [(100, 104, 8), (300, 304, 8), (500, 504, 8)] + \
              [s for f in range(600, 3000, 90) for s in ((f, f + 59, 16), (f + 60, f + 69, 17))]
SPEED_GBA = 3000
REF_SESSIONS = {
    'std': (['150', '400', '700'], '320-325:Start'),
    'play': (['200', '450', '700', '1000', '1400'], None),     # STATE_SPANS
}
FURB_KEYS = {1: 'A', 2: 'B', 4: 'Select', 8: 'Start', 16: 'Right', 32: 'Left', 64: 'Up', 128: 'Down'}

ORDER = {'PASS': 0, 'SKIP': 0, 'WARN': 1, 'FAIL': 2}
LOCK = threading.Lock()


def log(*a):
    with LOCK:
        print(*a, flush=True)


def sh(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def keys_str(spans):
    return ','.join('%d-%d:%d' % s for s in spans)


def furb_input(spans):
    out = []
    for f0, f1, m in spans:
        out.append('%d-%d:%s' % (f0, f1, '+'.join(n for b, n in sorted(FURB_KEYS.items()) if m & b)))
    return ';'.join(out)


def md5(data):
    return hashlib.md5(data).hexdigest()


def load_manifest():
    return json.load(open(MANIFEST))


# ------------------------------------------------------------------ ROM staging
def stage_roms(man, roms_dir, extra):
    """Copy every cart the manifest lists into roms_dir, found by md5 wherever it is."""
    want = {}
    for c in man['carts']:
        want[c['md5']] = (c, None)
        if c.get('raw_md5'):
            want[c['raw_md5']] = (c, bytes.fromhex(c['raw_header']))
    os.makedirs(roms_dir, exist_ok=True)
    have = {}
    for c in man['carts']:
        p = os.path.join(roms_dir, c['file'])
        if os.path.exists(p) and os.path.getsize(p) == c['size'] and md5(open(p, 'rb').read()) == c['md5']:
            have[c['id']] = p
    if len(have) == len(man['carts']):
        return have

    def offer(data, origin):
        h = md5(data)
        if h not in want:
            return
        c, hdr = want[h]
        if c['id'] in have:
            return
        if hdr:
            data = hdr + data
        p = os.path.join(roms_dir, c['file'])
        with open(p, 'wb') as f:
            f.write(data)
        have[c['id']] = p
        log('stage: %-26s <- %s' % (c['file'], origin))

    sizes = {c['size'] for c in man['carts']} | {c['size'] - 16 for c in man['carts'] if c.get('raw_md5')}
    roots = list(extra) + [os.path.expanduser('~/.claude/uploads'), REPO]
    seen = set()
    for root in roots:
        if not os.path.exists(root):
            continue
        walk = [(os.path.dirname(root), [], [os.path.basename(root)])] if os.path.isfile(root) else os.walk(root)
        for d, dirs, files in walk:
            dirs[:] = [x for x in dirs if not x.startswith('.git') and x not in ('node_modules', 'regress_out')
                       and not x.endswith('_build')]
            if d.startswith(os.path.abspath(roms_dir)):
                continue
            for fn in files:
                p = os.path.join(d, fn)
                if p in seen or not os.path.isfile(p):
                    continue
                seen.add(p)
                try:
                    sz = os.path.getsize(p)
                except OSError:
                    continue
                if fn.lower().endswith('.zip') and sz < 256 << 20:
                    try:
                        with zipfile.ZipFile(p) as z:
                            for zi in z.infolist():
                                if zi.file_size in sizes:
                                    offer(z.read(zi), '%s:%s' % (p, zi.filename))
                    except (zipfile.BadZipFile, OSError):
                        pass
                elif sz in sizes:
                    offer(open(p, 'rb').read(), p)
            if len(have) == len(man['carts']):
                return have
    return have


# ------------------------------------------------------------------ source trees and cores
def git(*a, cwd=REPO):
    return subprocess.check_output(['git'] + list(a), cwd=cwd, text=True).strip()


class Tree:
    """A source tree to build: the working tree (uncommitted changes included) or a git ref.
    The working tree is copied once, as it is hashed, so an edit made while the
    run goes on cannot reach a core built later in it (the -DVT_NO_SPEEDHACK
    confirmations are built on demand)."""

    def __init__(self, spec, cache):
        self.spec = spec
        if spec == 'WORKTREE':
            files = [f for f in git('ls-files', '-z', '-c', '-o', '--exclude-standard').split('\0') if f]
            files = sorted(f for f in files if os.path.isfile(os.path.join(REPO, f)))
            tmp = tempfile.mkdtemp(prefix='tree_', dir=cache)
            h = hashlib.sha1()
            for f in files:
                data = open(os.path.join(REPO, f), 'rb').read()
                h.update(f.encode() + b'\0' + data + b'\0')
                os.makedirs(os.path.dirname(os.path.join(tmp, f)), exist_ok=True)
                with open(os.path.join(tmp, f), 'wb') as o:
                    o.write(data)
                shutil.copymode(os.path.join(REPO, f), os.path.join(tmp, f))
            self.key = 'wt-' + h.hexdigest()[:16]
            self.label = 'working tree'
            self.snap = os.path.join(cache, 'trees', self.key)
            os.makedirs(os.path.dirname(self.snap), exist_ok=True)
            if os.path.isdir(self.snap):
                shutil.rmtree(tmp)
                os.utime(self.snap)
            else:
                os.rename(tmp, self.snap)
        else:
            self.key = git('rev-parse', spec + '^{tree}')[:16]
            self.label = '%s (%s)' % (spec, git('rev-parse', '--short', spec))

    def export(self, dest):
        os.makedirs(dest, exist_ok=True)
        if self.spec == 'WORKTREE':
            shutil.copytree(self.snap, dest, dirs_exist_ok=True)
        else:
            arch = subprocess.Popen(['git', 'archive', '--format=tar', self.spec], cwd=REPO, stdout=subprocess.PIPE)
            subprocess.check_call(['tar', '-x', '-C', dest], stdin=arch.stdout)
            if arch.wait():
                raise RuntimeError('git archive %s failed' % self.spec)


MAKEFILE_HOOK = '\n# regress.py: EXTRA_CFLAGS reaches C and the assembler, as in build_pvt.sh\nCFLAGS\t+=\t$(EXTRA_CFLAGS)\nASFLAGS\t+=\t$(EXTRA_CFLAGS)\n'


class Builder:
    def __init__(self, args):
        self.args = args
        self.cache = os.path.join(args.cache, 'cores')
        os.makedirs(self.cache, exist_ok=True)
        self.locks = {}
        self.ids = {}

    def toolchain_id(self, tc):
        if tc not in self.ids:
            if tc == 'pvt':
                v = sh(['arm-none-eabi-gcc', '--version']).stdout.split('\n')[0]
                self.ids[tc] = v + '|' + os.path.realpath(self.args.libgba)
            else:
                insp = self.args.docker + ['image', 'inspect', '-f', '{{.Id}}', self.args.image]
                if sh(insp).returncode:
                    sh(self.args.docker + ['pull', self.args.image])
                r = sh(insp)
                if r.returncode:
                    raise RuntimeError('docker image %s is not available:\n%s' % (self.args.image, r.stdout))
                self.ids[tc] = r.stdout.strip()
        return self.ids[tc]

    def core(self, tree, tc, defines=()):
        """Directory holding pocketvt.elf/.gba/builder.py for this tree, toolchain and defines."""
        defines = tuple(sorted(defines))
        key = hashlib.sha1(('%s|%s|%s|%s' % (tree.key, tc, ' '.join(defines), self.toolchain_id(tc))).encode()).hexdigest()[:20]
        out = os.path.join(self.cache, '%s-%s-%s' % (tc, tree.key[:12], key[:8]))
        with LOCK:
            lk = self.locks.setdefault(out, threading.Lock())
        with lk:
            if os.path.exists(os.path.join(out, 'pocketvt.gba')):
                os.utime(out)
                return out
            t0 = time.time()
            work = tempfile.mkdtemp(prefix='regress_build_', dir=self.args.cache)
            try:
                src = os.path.join(work, 'src_tree')
                tree.export(src)
                flags = ' '.join('-D' + d for d in defines)
                if tc == 'pvt':
                    bdir = os.path.join(work, 'build')
                    env = dict(os.environ, LIBGBA=self.args.libgba, BUILD=bdir, EXTRA_CFLAGS=flags)
                    r = sh(['bash', os.path.join(src, 'build_pvt.sh')], env=env, cwd=src)
                    elf, gba = os.path.join(bdir, 'pocketvt.elf'), os.path.join(bdir, 'pocketvt.gba')
                else:
                    for f in ('pocketvt.gba', 'pocketvt.elf'):
                        if os.path.exists(os.path.join(src, f)):
                            os.remove(os.path.join(src, f))
                    shutil.rmtree(os.path.join(src, 'build'), ignore_errors=True)
                    mk = os.path.join(src, 'Makefile')
                    text = open(mk).read()
                    if defines and 'EXTRA_CFLAGS' not in text:
                        # Older trees: the Makefile's VT= define reaches C files only.  The
                        # copy gets the hook; the hook is a no-op for a plain build.
                        text = text.replace('\nLDFLAGS\t=', MAKEFILE_HOOK + '\nLDFLAGS\t=', 1)
                        if MAKEFILE_HOOK not in text:
                            raise RuntimeError('cannot add EXTRA_CFLAGS to %s' % mk)
                        open(mk, 'w').write(text)
                    cmd = self.args.docker + ['run', '--rm', '-v', src + ':/src', '-w', '/src', self.args.image, 'make']
                    if defines:
                        cmd.append('EXTRA_CFLAGS=' + flags)
                    r = sh(cmd)
                    elf, gba = os.path.join(src, 'pocketvt.elf'), os.path.join(src, 'pocketvt.gba')
                if r.returncode or not os.path.exists(gba) or os.path.getsize(gba) < 64 << 10:
                    raise RuntimeError('%s build of %s %s failed:\n%s' % (tc, tree.label, flags, r.stdout[-3000:]))
                os.makedirs(out + '.tmp', exist_ok=True)
                for f in (elf, gba):
                    shutil.copy2(f, out + '.tmp')
                shutil.copy2(os.path.join(src, 'builder.py'), out + '.tmp')
                json.dump(dict(tree=tree.label, toolchain=tc, defines=defines), open(os.path.join(out + '.tmp', 'info.json'), 'w'))
                os.rename(out + '.tmp', out)
            finally:
                shutil.rmtree(work, ignore_errors=True)
            log('built  %-6s %-28s %-34s %4.0f s' % (tc, tree.label, flags or '(shipping)', time.time() - t0))
            return out


def symbols(core):
    syms = {}
    for line in subprocess.check_output(['arm-none-eabi-nm', os.path.join(core, 'pocketvt.elf')], text=True).splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = p[0]
    return syms


def play_rom(core, cart, rom):
    d = os.path.join(core, 'play')
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, '%s-%s.gba' % (cart['id'], cart['md5'][:8]))
    if os.path.exists(out):
        return out
    work = tempfile.mkdtemp(prefix='play_', dir=d)
    try:
        shutil.copy2(os.path.join(core, 'pocketvt.gba'), work)
        r = sh([sys.executable, os.path.join(core, 'builder.py'), os.path.abspath(rom)], cwd=work)
        p = os.path.join(work, 'play_me.gba')
        if r.returncode or not os.path.exists(p) or os.path.getsize(p) <= os.path.getsize(os.path.join(core, 'pocketvt.gba')):
            raise RuntimeError('builder.py injected nothing for %s:\n%s' % (rom, r.stdout[-1000:]))
        os.rename(p, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return out


# ------------------------------------------------------------------ harness runs
def gbaplay_bin(args):
    src = os.path.join(REPO, 'tools', 'probes', 'gbaplay.c')
    h = md5(open(src, 'rb').read())[:10]
    out = os.path.join(args.cache, 'bin', 'gbaplay-' + h)
    if not os.path.exists(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        r = sh(['gcc', '-O2', src, '-o', out + '.tmp', '-lmgba'])
        if r.returncode:
            sys.exit('regress: cannot build gbaplay (apt install libmgba-dev):\n' + r.stdout)
        os.rename(out + '.tmp', out)
    return out


def read_ppm(path):
    data = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    return data[m.end():], int(m.group(1)), int(m.group(2))


def run_state(args, core, cart, rom, outdir):
    """NES RAM and pictures at STATE_FRAMES (+-1 for the phase check)."""
    os.makedirs(outdir, exist_ok=True)
    play = play_rom(core, cart, rom)
    syms = symbols(core)
    frames = sorted({g for f in STATE_FRAMES for g in (f - 1, f, f + 1)})
    # NES RAM, then the 6502 stack pointer (its low byte), for the dead-stack rule in judge_state
    env = dict(os.environ, KEYSN=keys_str(STATE_SPANS), FT=syms['frametotal'], FTDUMP=','.join(map(str, frames)),
               DUMP='03000000:%d,%s:4' % (cart['ram'], syms['_m6502_s']), OUT=os.path.join(outdir, 'd'),
               STOPFT=str(max(frames) + 1))
    r = sh([args.gbaplay, play, '12000'], env=env, cwd=outdir)
    open(os.path.join(outdir, 'gbaplay.log'), 'w').write(r.stdout)
    res = {'frames': {}, 'last_ft': 0}
    for m in re.finditer(r'frametotal=(\d+)', r.stdout):
        res['last_ft'] = int(m.group(1))
    for f in frames:
        b, p = os.path.join(outdir, 'd_ft%d_0.bin' % f), os.path.join(outdir, 'd_ft%d_0.ppm' % f)
        if os.path.exists(b) and os.path.exists(p):
            res['frames'][f] = (b, p)
    return res


def run_speed(args, core, cart, rom, outdir):
    os.makedirs(outdir, exist_ok=True)
    play = play_rom(core, cart, rom)
    syms = symbols(core)
    env = dict(os.environ, KEYSN=keys_str(SPEED_SPANS), FT=syms['frametotal'])
    r = sh([args.gbaplay, play, str(args.speed_gba)], env=env, cwd=outdir)
    open(os.path.join(outdir, 'speed.log'), 'w').write(r.stdout)
    fts = [int(m.group(1)) for m in re.finditer(r'^frame \d+ pc=\S+ frametotal=(\d+)', r.stdout, re.M)]
    per = [b - a for a, b in zip(fts, fts[1:])]
    pcs = re.findall(r'^frame \d+ pc=(\S+)', r.stdout, re.M)
    g = per[3:] or per or [0]
    return dict(mean=round(sum(g) / len(g), 2), min=min(g), slow=sum(1 for x in g if x < 55),
                final=fts[-1] if fts else 0, per_second=per, last_pc=pcs[-1] if pcs else None)


def run_ref(args, core, cart, rom, outdir, session):
    at, inp = REF_SESSIONS[session]
    inp = inp or furb_input(STATE_SPANS)
    cmd = [sys.executable, os.path.join(REPO, 'tools', 'compare_furb.py'), rom, '--at', ','.join(at), '--input', inp,
           '--core', os.path.join(core, 'pocketvt.gba'), '--elf', os.path.join(core, 'pocketvt.elf'),
           '--furb', args.furb, '--out', outdir, '--ref-cache', os.path.join(args.cache, 'furbref')]
    r = sh(cmd)
    open(outdir + '.log', 'w').write(r.stdout)
    rep = os.path.join(outdir, 'report.json')
    if r.returncode or not os.path.exists(rep):
        return dict(error=r.stdout[-800:])
    return {str(e['target']): dict(struct=e['struct'], exact5=e['exact5'], image=e['image'])
            for e in json.load(open(rep))['results']}


# ------------------------------------------------------------------ verdicts
def parse_ranges(rs):
    out = []
    for r in rs or ():
        a, _, b = r.partition('-')
        out.append((int(a, 0), int(b or a, 0)))
    return out


def ram_diffs(a, b, benign):
    d = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    bad = [i for i in d if not any(lo <= i <= hi for lo, hi in benign)]
    return d, bad


def save_sheet(path, pics):
    """old | new | difference (red) as a PNG, if pillow is there."""
    try:
        from PIL import Image
    except ImportError:
        return None
    (a, w, h), (b, _, _) = pics
    diff = bytearray(len(a))
    for i in range(0, len(a), 3):
        if a[i:i + 3] != b[i:i + 3]:
            diff[i] = 255
        else:
            diff[i:i + 3] = bytes(x // 3 for x in b[i:i + 3])
    sheet = Image.new('RGB', (w * 3 + 8, h), (255, 255, 255))
    for k, data in enumerate((a, b, bytes(diff))):
        sheet.paste(Image.frombytes('RGB', (w, h), data), (k * (w + 4), 0))
    sheet.resize((sheet.width * 2, h * 2), Image.NEAREST).save(path)
    return path


def judge_state(cart, old, new, outdir):
    """-> (verdict, notes, ram_ok, frames whose pictures differ beyond display phase)"""
    notes = []
    benign = parse_ranges(cart.get('ram_benign'))
    want = [f for f in STATE_FRAMES if f in old['frames']]
    if len(want) < len(STATE_FRAMES):
        notes.append('old core reached only NES %d' % old['last_ft'])
    missing = [f for f in want if f not in new['frames']]
    if missing:
        return 'FAIL', ['new core never reached NES %d (old did; stopped at %d)' % (missing[0], new['last_ft'])], False, []
    ram_bad, benign_hits, stack_hits = [], 0, 0
    n = cart['ram']
    for f in want:
        a = open(old['frames'][f][0], 'rb').read()
        b = open(new['frames'][f][0], 'rb').read()
        d, bad = ram_diffs(a[:n], b[:n], benign)
        benign_hits += len(d) - len(bad)
        # Dead stack: with both stack pointers equal, bytes up to 32 below it are
        # whatever an interrupt last pushed there, which moves with timing (the
        # NMI landing at another point of an idle loop, s.97f).
        if bad and len(a) > n and len(b) > n and a[n] == b[n]:
            sp = 0x100 + a[n]
            live = [i for i in bad if not (sp - 32 <= i <= sp)]
            stack_hits += len(bad) - len(live)
            bad = live
        if bad:
            ram_bad.append((f, len(bad), ['$%04X:%02X>%02X' % (i, a[i], b[i]) for i in bad[:6]]))
    if benign_hits:
        notes.append('%d known-benign RAM byte differences' % benign_hits)
    if stack_hits:
        notes.append('%d byte differences in dead stack just below an equal stack pointer' % stack_hits)
    verdict = 'PASS'
    if ram_bad:
        f, n, ex = ram_bad[0]
        msg = 'NES RAM differs at %d of %d frames, first NES %d (%d bytes: %s)' % (len(ram_bad), len(want), f, n, ' '.join(ex))
        if cart.get('drift'):
            notes.append(msg + '; expected: this cart\'s game state follows GBA timing')
        else:
            notes.append(msg)
            verdict = 'FAIL'
    # pictures, where the RAM agrees
    bad_ram_frames = {f for f, _, _ in ram_bad}
    phase, differ = 0, []
    for f in want:
        if f in bad_ram_frames:
            continue
        pn = read_ppm(new['frames'][f][1])
        po = read_ppm(old['frames'][f][1])
        if pn[0] == po[0]:
            continue
        near = [g for g in (f + 1, f - 1, f + 2, f - 2) if g in old['frames'] and read_ppm(old['frames'][g][1])[0] == pn[0]]
        if near:
            phase += 1
            continue
        px = sum(1 for i in range(0, len(pn[0]), 3) if pn[0][i:i + 3] != po[0][i:i + 3])
        differ.append((f, px, save_sheet(os.path.join(outdir, 'pic_ft%d.png' % f), (po, pn))))
    if phase:
        notes.append('%d pictures are the old core\'s picture of a neighbouring NES frame (display phase)' % phase)
    if differ:
        notes.append('pictures differ at %s (pixels; old|new|diff PNGs in %s)' %
                     (', '.join('NES %d: %d' % (f, px) for f, px, _ in differ[:6]), outdir))
        verdict = max(verdict, 'WARN', key=ORDER.get)
    return verdict, notes, not ram_bad, [f for f, _, _ in differ]


def judge_speed(cart, old, new):
    notes = ['mean %.2f -> %.2f, worst second %d -> %d' % (old['mean'], new['mean'], old['min'], new['min'])]
    drop = old['mean'] - new['mean']
    loose = cart.get('sound_hle') or cart.get('drift')
    fail_at = 3.0 if loose else 1.5
    if new['final'] < 120 or (old['mean'] >= 55 and new['mean'] < 45):
        return 'FAIL', notes + ['new core stalls (NES frame %d after %d GBA frames)' % (new['final'], SPEED_GBA)]
    if drop > fail_at:
        return 'FAIL', notes
    if drop > 0.3 or new['min'] < old['min'] - 5:
        return 'WARN', notes + (['sound-on game state follows GBA time on this cart: re-check with a longer run'] if loose else [])
    return 'PASS', notes


def judge_ref(cart, old, new):
    """-> (verdict, notes, largest struct drop)"""
    notes, verdict, worst = [], 'PASS', 0.0
    for sess in REF_SESSIONS:
        o, n = old.get(sess, {}), new.get(sess, {})
        if 'error' in n or 'error' in o:
            return 'WARN', ['compare_furb failed (%s): %s' % (sess, (n.get('error') or o.get('error'))[-300:])], 0.0
        parts = []
        for t in REF_SESSIONS[sess][0]:
            if t not in o or t not in n:
                continue
            ds, de = n[t]['struct'] - o[t]['struct'], n[t]['exact5'] - o[t]['exact5']
            parts.append('%s %.2f%s' % (t, n[t]['struct'], '' if abs(ds) < 0.005 else ' (%+.2f)' % ds))
            worst = min(worst, ds)
            if ds < -0.5:
                verdict = max(verdict, 'WARN' if cart.get('drift') else 'FAIL', key=ORDER.get)
            elif ds < -0.05 or de < -1.0:
                verdict = max(verdict, 'WARN', key=ORDER.get)
        notes.append('%s: %s' % (sess, ', '.join(parts)))
    return verdict, notes, worst


# ------------------------------------------------------------------ main
def prune(cache, days):
    """Drop cached cores and tree copies nobody has used for `days` days."""
    limit = time.time() - days * 86400
    for sub in ('cores', 'trees'):
        d = os.path.join(cache, sub)
        for e in os.listdir(d) if os.path.isdir(d) else ():
            p = os.path.join(d, e)
            try:
                if os.path.getmtime(p) < limit:
                    shutil.rmtree(p, ignore_errors=True)
            except OSError:
                pass


def find_furb(given):
    cands = [given, os.environ.get('FURB_CLI'), os.path.join(REPO, 'tools', 'furb_cli', 'build', 'furb_cli'),
             os.path.join(os.path.dirname(REPO), 'gamingfanboy365', 'furbtendulator', 'bin', 'furb_cli'),
             os.path.join(os.path.dirname(REPO), 'furbtendulator', 'bin', 'furb_cli'),
             os.path.expanduser('~/furbtendulator/bin/furb_cli')]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def find_libgba(given):
    for c in (given, os.environ.get('LIBGBA'), os.path.join(os.path.dirname(REPO), 'libgba-master'),
              os.path.join(os.path.dirname(REPO), 'libgba'), os.path.join(os.path.dirname(REPO), 'devkitpro', 'libgba'),
              os.path.expanduser('~/libgba'), '/opt/devkitpro/libgba'):
        if c and os.path.isfile(os.path.join(c, 'include', 'gba.h')):
            return c
    return None


def docker_cmd():
    base = ['docker'] if os.geteuid() == 0 or shutil.which('sudo') is None else ['sudo', 'docker']
    if shutil.which('docker') is None:
        return None
    if sh(base + ['info']).returncode == 0:
        return base
    if os.geteuid() == 0 and shutil.which('dockerd'):
        subprocess.Popen(['dockerd'], stdout=open('/tmp/regress_dockerd.log', 'w'), stderr=subprocess.STDOUT)
        for _ in range(30):
            time.sleep(1)
            if sh(base + ['info']).returncode == 0:
                return base
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 1)[1])
    ap.add_argument('--old', default=None, help='git ref of the old core (default origin/main, else main)')
    ap.add_argument('--new', default='WORKTREE', help='git ref of the new core (default: the working tree, uncommitted changes included)')
    ap.add_argument('--carts', help='comma-separated cart ids (carts.json), or "controls"/"quick"')
    ap.add_argument('--quick', action='store_true', help='the quick carts, build_pvt only, state + speed')
    ap.add_argument('--toolchains', default='pvt,docker', help='pvt (build_pvt.sh), docker (devkitARM image) or both')
    ap.add_argument('--checks', default='state,ref,speed')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    ap.add_argument('--out', default=os.path.join(REPO, 'regress_out'))
    ap.add_argument('--cache', default=os.path.expanduser('~/.cache/pocketvt-regress'))
    ap.add_argument('--roms', default=os.path.join(REPO, 'testroms'))
    ap.add_argument('--bundle', action='append', default=[], help='zip or folder to search for the test ROMs (by md5)')
    ap.add_argument('--stage', action='store_true', help='only stage the ROMs, then exit')
    ap.add_argument('--libgba', default=None)
    ap.add_argument('--furb', default=None, help='furb_cli (default: tools/furb_cli/build, ../gamingfanboy365/furbtendulator/bin)')
    ap.add_argument('--image', default='devkitpro/devkitarm')
    ap.add_argument('--speed-gba', type=int, default=SPEED_GBA)
    ap.add_argument('--no-confirm', action='store_true', help='do not re-check RAM differences on -DVT_NO_SPEEDHACK builds')
    args = ap.parse_args()

    man = load_manifest()
    have = stage_roms(man, args.roms, args.bundle)
    missing = [c['id'] for c in man['carts'] if c['id'] not in have]
    log('ROMs: %d of %d carts staged in %s%s' % (len(have), len(man['carts']), args.roms,
                                                 ('; MISSING: ' + ', '.join(missing)) if missing else ''))
    if args.stage:
        return 0 if not missing else 3
    if args.quick:
        args.toolchains = 'pvt'
        args.checks = 'state,speed'
        args.carts = args.carts or 'quick'
    sel = args.carts.split(',') if args.carts else [c['id'] for c in man['carts']]
    if sel in (['controls'], ['quick']):
        sel = man[sel[0]]
    carts = [c for c in man['carts'] if c['id'] in sel]
    unknown = set(sel) - {c['id'] for c in carts}
    if unknown:
        sys.exit('regress: unknown cart ids: %s' % ', '.join(sorted(unknown)))
    skipped = [c['id'] for c in carts if c['id'] not in have]
    carts = [c for c in carts if c['id'] in have]
    if not carts:
        sys.exit('regress: no test ROMs found.  Upload pocketvt_testroms.zip (the user has it) and rerun, or pass --bundle')

    checks = set(args.checks.split(','))
    tcs = [t for t in args.toolchains.split(',') if t]
    notes_top = []
    if 'pvt' in tcs:
        args.libgba = find_libgba(args.libgba)
        if not args.libgba or not shutil.which('arm-none-eabi-gcc'):
            tcs.remove('pvt')
            notes_top.append('build_pvt toolchain skipped: no arm-none-eabi-gcc or libgba (git clone https://github.com/devkitPro/libgba ../libgba-master)')
    if 'docker' in tcs:
        args.docker = docker_cmd()
        if not args.docker:
            tcs.remove('docker')
            notes_top.append('devkitARM toolchain skipped: docker is not available')
    if not tcs:
        sys.exit('regress: no toolchain available\n' + '\n'.join(notes_top))
    if 'ref' in checks:
        args.furb = find_furb(args.furb)
        if not args.furb:
            checks.discard('ref')
            notes_top.append('ref check skipped: no furb_cli (add the GamingFanboy365/furbtendulator repo: bin/furb_cli, or --furb)')
    os.makedirs(args.cache, exist_ok=True)
    args.gbaplay = gbaplay_bin(args)

    if args.old is None:
        args.old = 'origin/main' if sh(['git', 'rev-parse', '--verify', '-q', 'origin/main'], cwd=REPO).returncode == 0 else 'main'
    prune(args.cache, days=14)
    old_t, new_t = Tree(args.old, args.cache), Tree(args.new, args.cache)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = os.path.join(args.out, stamp)
    os.makedirs(out, exist_ok=True)
    log('old: %s   new: %s   toolchains: %s   checks: %s   carts: %d   out: %s' %
        (old_t.label, new_t.label, ','.join(tcs), ','.join(sorted(checks)), len(carts), out))
    for n in notes_top:
        log('note: ' + n)

    b = Builder(args)
    pool = cf.ThreadPoolExecutor(max_workers=max(1, args.jobs))
    # every core this run needs, built in parallel
    need = set()
    for tc in tcs:
        for t in (old_t, new_t):
            need.add((t, tc, ()))
            if 'state' in checks and any(c.get('sound_hle') for c in carts):
                need.add((t, tc, ('VT369_SND_OFF',)))
    futs = {pool.submit(b.core, *k): k for k in need}
    cores = {}
    for f in cf.as_completed(futs):
        k = futs[f]
        try:
            cores[(k[0].key, k[1], k[2])] = f.result()
        except Exception as e:  # a build failure is the answer for that side
            sys.exit('regress: %s' % e)

    def core_of(tree, tc, defs):
        return cores.get((tree.key, tc, tuple(sorted(defs)))) or b.core(tree, tc, defs)

    results = {}

    def task(cart, tc):
        cid = cart['id']
        rom = have[cid]
        res = dict(cart=cid, toolchain=tc, checks={})
        pic_diff = []
        base = os.path.join(out, cid, tc)
        state_defs = ('VT369_SND_OFF',) if cart.get('sound_hle') else ()
        if 'state' in checks:
            o = run_state(args, core_of(old_t, tc, state_defs), cart, rom, os.path.join(base, 'state_old'))
            n = run_state(args, core_of(new_t, tc, state_defs), cart, rom, os.path.join(base, 'state_new'))
            v, notes, ram_ok, pic_diff = judge_state(cart, o, n, os.path.join(base, 'state_new'))
            if not ram_ok and not cart.get('drift') and not args.no_confirm:
                defs = state_defs + ('VT_NO_SPEEDHACK',)
                o2 = run_state(args, core_of(old_t, tc, defs), cart, rom, os.path.join(base, 'state_old_nosh'))
                n2 = run_state(args, core_of(new_t, tc, defs), cart, rom, os.path.join(base, 'state_new_nosh'))
                v2, notes2, ram_ok2, _ = judge_state(cart, o2, n2, os.path.join(base, 'state_new_nosh'))
                if ram_ok2:
                    v = 'WARN' if v == 'FAIL' else v
                    notes.append('with -DVT_NO_SPEEDHACK the RAM agrees at every frame: the difference comes '
                                 'from where the idle-loop finder installs its hacks (GBA-time race, s.97e)')
                else:
                    notes.append('with -DVT_NO_SPEEDHACK too: ' + '; '.join(notes2))
            res['checks']['state'] = dict(verdict=v, notes=notes)
        if 'speed' in checks:
            o = run_speed(args, core_of(old_t, tc, ()), cart, rom, os.path.join(base, 'speed_old'))
            n = run_speed(args, core_of(new_t, tc, ()), cart, rom, os.path.join(base, 'speed_new'))
            v, notes = judge_speed(cart, o, n)
            res['checks']['speed'] = dict(verdict=v, notes=notes, old=o, new=n)
        if 'ref' in checks:
            o = {s: run_ref(args, core_of(old_t, tc, ()), cart, rom, os.path.join(base, 'ref_old_' + s), s) for s in REF_SESSIONS}
            n = {s: run_ref(args, core_of(new_t, tc, ()), cart, rom, os.path.join(base, 'ref_new_' + s), s) for s in REF_SESSIONS}
            v, notes, worst = judge_ref(cart, o, n)
            # A picture that changed with the same game state, and a reference score
            # that dropped anywhere: two signs of one rendering fault.
            if v == 'WARN' and 'state' in res['checks'] and pic_diff and worst < -0.05:
                v = 'FAIL'
                notes.append('pictures changed at NES %s with identical RAM, and struct dropped by %.2f: '
                             'a rendering change for the worse' % (', '.join(map(str, pic_diff[:6])), -worst))
            res['checks']['ref'] = dict(verdict=v, notes=notes, old=o, new=n)
        res['verdict'] = max((c['verdict'] for c in res['checks'].values()), key=ORDER.get, default='PASS')
        log('%-4s %-26s %-6s %s' % (res['verdict'], cid, tc,
                                    '  '.join('%s %s' % (k, c['verdict']) for k, c in res['checks'].items())))
        return res

    jobs = [pool.submit(task, c, tc) for c in carts for tc in tcs]
    allres = []
    for j in jobs:
        try:
            allres.append(j.result())
        except Exception as e:
            allres.append(dict(cart='?', toolchain='?', verdict='FAIL', checks={'error': dict(verdict='FAIL', notes=[repr(e)])}))
    pool.shutdown()

    worst = max((r['verdict'] for r in allres), key=ORDER.get, default='PASS')
    write_report(out, old_t, new_t, tcs, checks, allres, skipped, notes_top, worst)
    log('\n%s -- %s' % (worst, os.path.join(out, 'report.md')))
    return 1 if worst == 'FAIL' else 0


def write_report(out, old_t, new_t, tcs, checks, allres, skipped, notes_top, worst):
    json.dump(dict(old=old_t.label, new=new_t.label, toolchains=tcs, checks=sorted(checks), verdict=worst,
                   skipped=skipped, notes=notes_top, results=allres), open(os.path.join(out, 'report.json'), 'w'), indent=1)
    L = ['# PocketVT regression: %s' % worst, '',
         'Old core: %s. New core: %s. Toolchains: %s. Checks: %s.' % (old_t.label, new_t.label, ', '.join(tcs), ', '.join(sorted(checks))), '']
    for n in notes_top:
        L.append('Note: %s.' % n)
    if skipped:
        L.append('Carts without a ROM here (not checked): %s.' % ', '.join(skipped))
    L += ['', '| Cart | Toolchain | Verdict | ' + ' | '.join(sorted(checks)) + ' |', '|---|---|---|' + '---|' * len(checks)]
    for r in sorted(allres, key=lambda r: (-ORDER[r['verdict']], r['cart'], r['toolchain'])):
        L.append('| %s | %s | %s | %s |' % (r['cart'], r['toolchain'], r['verdict'],
                                             ' | '.join(r['checks'].get(c, {}).get('verdict', '-') for c in sorted(checks))))
    L.append('')
    for r in sorted(allres, key=lambda r: (r['cart'], r['toolchain'])):
        L.append('**%s, %s: %s.** ' % (r['cart'], r['toolchain'], r['verdict']) +
                 ' '.join('%s: %s.' % (k, '; '.join(c.get('notes', [])) or 'ok') for k, c in r['checks'].items()))
        L.append('')
    open(os.path.join(out, 'report.md'), 'w').write('\n'.join(L) + '\n')


if __name__ == '__main__':
    sys.exit(main())
