#!/usr/bin/env python3
"""smoke.py -- CI's ROM-free check: plays the generated smoke ROMs (roms.py)
on a PocketVT core in mGBA and compares with the recorded expectations.

    python3 tools/ci/smoke/smoke.py --core pocketvt.gba [--elf pocketvt.elf]
    python3 tools/ci/smoke/smoke.py --bless --furb .../furb_cli     # re-record

cpu, vtbank: NES RAM at NES frame 60 must equal Furbtendulator's, byte for
byte (recorded in expect.json by --bless; py65 must agree on the cpu test).
ppu: the GBA picture at NES frame 60 must hash as recorded.  --bless records
the core's picture after compare_furb has scored it against Furbtendulator
(it refuses below 99% struct), so a changed hash means a changed picture:
look at the PNG it writes, and re-bless only if the change is intended.

Needs libmgba-dev (gbaplay), arm-none-eabi-nm, python3; --bless also needs
furb_cli, numpy, pillow and py65.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import roms  # noqa: E402

EXPECT = os.path.join(HERE, 'expect.json')
FRAME = 60


def sh(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def gbaplay(work):
    out = os.path.join(work, 'gbaplay')
    r = sh(['gcc', '-O2', os.path.join(REPO, 'tools', 'probes', 'gbaplay.c'), '-o', out, '-lmgba'])
    if r.returncode:
        sys.exit('smoke: cannot build gbaplay (apt install libmgba-dev):\n' + r.stdout)
    return out


def run_pvt(core, elf, nes, work, name, player):
    """-> (NES RAM bytes, picture bytes, picture path) at NES frame FRAME"""
    d = os.path.join(work, name)
    os.makedirs(d, exist_ok=True)
    rom = os.path.join(d, name + '.nes')
    open(rom, 'wb').write(nes)
    shutil.copy(core, os.path.join(d, 'pocketvt.gba'))
    r = sh([sys.executable, os.path.join(REPO, 'builder.py'), rom], cwd=d)
    if r.returncode:
        sys.exit('smoke: builder.py failed:\n' + r.stdout)
    ft = None
    for line in sh(['arm-none-eabi-nm', elf]).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == 'frametotal':
            ft = p[0]
    if not ft:
        sys.exit('smoke: no frametotal in %s' % elf)
    env = dict(os.environ, FT=ft, FTDUMP=str(FRAME), DUMP='03000000:2048', OUT=os.path.join(d, 'd'),
               STOPFT=str(FRAME + 1))
    r = sh([player, os.path.join(d, 'play_me.gba'), '3000'], env=env, cwd=d)
    b, p = os.path.join(d, 'd_ft%d_0.bin' % FRAME), os.path.join(d, 'd_ft%d_0.ppm' % FRAME)
    if not os.path.exists(b):
        return None, None, None, r.stdout[-600:]
    return open(b, 'rb').read(), open(p, 'rb').read(), p, r.stdout


def furb_ram(furb, nes, work, name):
    d = os.path.join(work, 'furb_' + name)
    os.makedirs(d, exist_ok=True)
    rom = os.path.join(d, name + '.nes')
    open(rom, 'wb').write(nes)
    r = sh([furb, rom, '--frames', str(FRAME + 1), '--dump', str(FRAME), '--out', os.path.join(d, 'f'), '--quiet'])
    if r.returncode:
        sys.exit('smoke: furb_cli failed on %s:\n%s' % (name, r.stdout))
    return open(os.path.join(d, 'f_f%04d.ram' % FRAME), 'rb').read()[:0x800], rom


def py65_ram(nes):
    from py65.devices.mpu6502 import MPU
    prg = nes[16:16 + 32768]
    m = MPU()
    for i, b in enumerate(prg):
        m.memory[0x8000 + i] = b
    m.pc = prg[0x7FFC] | prg[0x7FFD] << 8
    for _ in range(5_000_000):
        pc = m.pc
        m.step()
        if m.pc == pc:
            return bytes(m.memory[0:0x800])
    sys.exit('smoke: py65 never reached the end of the cpu test')


def describe(name, info, got, want):
    lines = []
    diff = [i for i in range(min(len(got), len(want))) if got[i] != want[i]]
    for i in diff:
        if 0x200 <= i < 0x700 and info.get('cases') and (i - 0x200) // 2 < len(info['cases']):
            what = '%s (%s)' % (info['cases'][(i - 0x200) // 2], 'P' if i & 1 else 'value')
        elif i in (roms.DONE, roms.DONE + 1):
            what = 'the end marker: the program did not finish'
        else:
            what = 'RAM $%04X' % i
        lines.append('  $%04X: got %02X, expected %02X  %s' % (i, got[i], want[i], what))
        if len(lines) >= 12:
            lines.append('  ... %d bytes differ in all' % len(diff))
            break
    return lines


def save_png(ppm, path):
    try:
        from PIL import Image
        Image.open(ppm).save(path)
        return path
    except Exception:
        return ppm


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--core', default=os.path.join(REPO, 'pocketvt.gba'))
    ap.add_argument('--elf', help='default: pocketvt.elf next to --core')
    ap.add_argument('--bless', action='store_true')
    ap.add_argument('--furb', help='furb_cli, for --bless')
    ap.add_argument('--out', default=None, help='keep the work files (pictures) here')
    a = ap.parse_args()
    a.core = os.path.abspath(a.core)
    elf = os.path.abspath(a.elf or os.path.join(os.path.dirname(a.core), 'pocketvt.elf'))
    work = os.path.abspath(a.out) if a.out else tempfile.mkdtemp(prefix='smoke_')
    os.makedirs(work, exist_ok=True)
    player = gbaplay(work)
    built = roms.build_all()
    expect = json.load(open(EXPECT)) if os.path.exists(EXPECT) and not a.bless else {}
    new_expect, failed = {}, []
    for name, (nes, info) in built.items():
        ram, pic, ppm, log = run_pvt(a.core, elf, nes, work, name, player)
        if ram is None:
            print('FAIL %-7s PocketVT never reached NES frame %d:\n%s' % (name, FRAME, log))
            failed.append(name)
            continue
        if a.bless:
            if not a.furb:
                sys.exit('smoke: --bless needs --furb')
            if name == 'ppu':
                rom = os.path.join(work, name, name + '.nes')
                cmp_dir = os.path.join(work, 'cmp_' + name)
                r = sh([sys.executable, os.path.join(REPO, 'tools', 'compare_furb.py'), rom, '--at', str(FRAME),
                        '--core', a.core, '--elf', elf, '--furb', a.furb, '--out', cmp_dir])
                m = re.search(r'struct\s+([\d.]+)%', r.stdout)
                score = float(m.group(1)) if m else 0.0
                print(r.stdout.strip())
                if score < 99.0:
                    sys.exit('smoke: refusing to bless ppu: struct %.2f%% against Furbtendulator' % score)
                new_expect[name] = dict(picture_md5=hashlib.md5(pic).hexdigest(), struct_at_bless=score,
                                        rom_md5=hashlib.md5(nes).hexdigest())
            else:
                want, _ = furb_ram(a.furb, nes, work, name)
                if name == 'cpu':
                    ref2 = py65_ram(nes)
                    if ref2 != want:
                        sys.exit('smoke: py65 and Furbtendulator disagree on the cpu test; not blessing')
                if want[roms.DONE:roms.DONE + 2] != b'\xA5\x5A':
                    sys.exit('smoke: %s did not finish on Furbtendulator' % name)
                new_expect[name] = dict(ram=want.hex(), rom_md5=hashlib.md5(nes).hexdigest())
                bad = describe(name, info, ram, want)
                print('%s %-7s against Furbtendulator%s' % ('ok  ' if not bad else 'DIFF', name, '' if not bad else ':'))
                for l in bad:
                    print(l)
            continue
        e = expect.get(name)
        if not e:
            print('FAIL %-7s no expectation recorded (run --bless)' % name)
            failed.append(name)
            continue
        if e.get('rom_md5') != hashlib.md5(nes).hexdigest():
            print('FAIL %-7s roms.py changed since expect.json was blessed: re-bless' % name)
            failed.append(name)
            continue
        if 'ram' in e:
            want = bytes.fromhex(e['ram'])
            bad = describe(name, info, ram, want)
            if bad:
                print('FAIL %-7s NES RAM differs from Furbtendulator\'s:' % name)
                for l in bad:
                    print(l)
                failed.append(name)
            else:
                print('ok   %-7s NES RAM equals Furbtendulator\'s (%d bytes)' % (name, len(want)))
        else:
            h = hashlib.md5(pic).hexdigest()
            png = save_png(ppm, os.path.join(work, name + '.png'))
            if h != e['picture_md5']:
                print('FAIL %-7s picture changed (md5 %s, blessed %s): look at %s; re-bless only if intended'
                      % (name, h, e['picture_md5'], png))
                failed.append(name)
            else:
                print('ok   %-7s picture as blessed (%.2f%% struct against Furbtendulator then)' % (name, e['struct_at_bless']))
    if a.bless:
        json.dump(new_expect, open(EXPECT, 'w'), indent=1)
        print('blessed %s' % EXPECT)
        return 0
    if failed:
        print('\nsmoke: %d of %d failed (%s); work files in %s' % (len(failed), len(built), ', '.join(failed), work))
        return 1
    print('smoke: all %d passed' % len(built))
    if not a.out:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
