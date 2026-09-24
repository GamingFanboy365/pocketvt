#!/usr/bin/env python3
"""selftest.py -- checks furb_cli's CLI features with tiny generated ROMs (no
game ROMs needed; nothing is written outside a temp dir).

    python3 tools/furb_cli/selftest.py [--furb tools/furb_cli/build/furb_cli] [--rom real.nes]

The generated NROM program reads $4016/$4017 32 times every NMI into
$0300/$0340 and counts frames at $0380, so each input path can be checked in
the CPU RAM dump.  --rom adds a real game for the savestate / movie / AVI
round trips (they need a game that renders and reacts to input).
"""
import argparse, os, re, struct, subprocess, sys, tempfile, wave

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('--furb', default=os.path.join(HERE, 'build', 'furb_cli'))
ap.add_argument('--rom', help='a real game for the state/movie/AVI round trips')
a = ap.parse_args()
T = tempfile.mkdtemp(prefix='furb_selftest_')
DATA = os.path.join(T, 'data')
fails = []

def check(name, ok, detail=''):
    print('%-44s %s %s' % (name, 'ok  ' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)

def reader_rom(vs=False, mapper=0):
    """16 KiB NROM: NMI reads both controller ports 32x into RAM."""
    code = bytes([
        0x78, 0xD8, 0xA2, 0xFF, 0x9A,           # SEI CLD LDX #$FF TXS
        0xA9, 0x80, 0x8D, 0x00, 0x20,           # LDA #$80 STA $2000 (NMI on)
        0x4C, 0x0A, 0xC0,                       # loop: JMP loop
        # NMI ($C00D)
        0x48, 0x8A, 0x48,                       # PHA TXA PHA
        0xA9, 0x01, 0x8D, 0x16, 0x40,           # strobe
        0xA9, 0x00, 0x8D, 0x16, 0x40,
        0xA2, 0x00,                             # LDX #0
        0xAD, 0x16, 0x40, 0x9D, 0x00, 0x03,     # rd: LDA $4016 STA $0300,X
        0xAD, 0x17, 0x40, 0x9D, 0x40, 0x03,     #     LDA $4017 STA $0340,X
        0xE8, 0xE0, 0x20, 0xD0, 0xEF,           #     INX CPX #$20 BNE rd (-17)
        0xEE, 0x80, 0x03,                       # INC $0380
        0x68, 0xAA, 0x68, 0x40])                # PLA TAX PLA RTI
    prg = bytearray(16384)
    prg[:len(code)] = code
    prg[0x3FFA:0x4000] = struct.pack('<HHH', 0xC00D, 0xC000, 0xC00D)
    hdr = bytearray(b'NES\x1a\x01\x01' + bytes(10))
    hdr[6] = (mapper & 0x0F) << 4
    hdr[7] = (mapper & 0xF0) | (0x01 if vs else 0)
    p = os.path.join(T, 'reader%s.nes' % ('_vs' if vs else ''))
    open(p, 'wb').write(bytes(hdr) + bytes(prg) + bytes(8192))
    return p

def run(rom, *args, frames=30, dump=20):
    out = os.path.join(T, 'o')
    cmd = [a.furb, rom, '--frames', str(frames), '--dump', str(dump), '--out', out, '--quiet',
           '--data-dir', DATA] + list(args)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    return open('%s_f%04d.ram' % (out, dump), 'rb').read(), r.stderr

def bits(ram, base, n=24):
    return [ram[base + i] & 0x1F for i in range(n)]

rom = reader_rom()
ram, err = run(rom)
check('generated ROM runs, NMI counts frames', ram is not None and ram[0x380] >= 15, err.strip()[:200])

# standard pads on both ports: serial bit 0, order A B Sel St U D L R
ram, _ = run(rom, '--input', '0-30:p1:A+Start;0-30:p2:Right')
check('port 1 standard pad (A+Start)', ram and [b & 1 for b in bits(ram, 0x300, 8)] == [1, 0, 0, 1, 0, 0, 0, 0])
check('port 2 standard pad (Right)', ram and [b & 1 for b in bits(ram, 0x340, 8)] == [0, 0, 0, 0, 0, 0, 0, 1])

# Four Score: reads 1-8 pad 1, 9-16 pad 3, 17-24 signature ($4016: 0x10 at read 20)
ram, _ = run(rom, '--device', 'port1=fourscore', '--input', '0-30:p1:B;0-30:p3:Up')
seq = [b & 1 for b in bits(ram, 0x300, 24)] if ram else []
check('Four Score: pad 1, pad 3, signature', seq[:8] == [0, 1, 0, 0, 0, 0, 0, 0] and seq[8:16] == [0, 0, 0, 0, 1, 0, 0, 0]
      and seq[16:24] == [0, 0, 0, 1, 0, 0, 0, 0], str(seq))

# Famicom expansion 4-player adapter: pads 3/4 arrive on bit 1
ram, _ = run(rom, '--device', 'exp=fami4play', '--input', '0-30:p3:A;0-30:p4:Select')
check('Famicom 4-player: pad 3 on $4016 bit 1', ram and [b >> 1 & 1 for b in bits(ram, 0x300, 8)] == [1, 0, 0, 0, 0, 0, 0, 0])
check('Famicom 4-player: pad 4 on $4017 bit 1', ram and [b >> 1 & 1 for b in bits(ram, 0x340, 8)] == [0, 0, 1, 0, 0, 0, 0, 0])

# NES Zapper on port 2: trigger = $4017 bit 4; the screen is black, so no light (bit 3 set)
ram, _ = run(rom, '--device', 'port2=zapper', '--input', '0-30:mouse:128,120;0-30:p2:trigger')
check('Zapper trigger ($4017 bit 4) and no-light (bit 3)', ram and ram[0x340] & 0x18 == 0x18, ram and hex(ram[0x340]))
ram, _ = run(rom, '--device', 'port2=zapper', '--input', '0-30:mouse:128,120')
check('Zapper trigger released', ram and ram[0x340] & 0x10 == 0, ram and hex(ram[0x340]))

# Famicom microphone (menu "Microphone"): toggles $4016 bit 2 for a while
ram, _ = run(rom, '--input', '18:cmd:mic', dump=19)
check('Famicom mic toggles $4016 bit 2', ram and any(b & 4 for b in bits(ram, 0x300, 32)))

# Vs. System: coin 1 = $4016 bit 5 for ~7 frames
vs = reader_rom(vs=True)
ram, err = run(vs, '--input', '18:cmd:coin1', dump=19)
check('Vs. System ROM loads (VS.so), coin 1 ($4016 bit 5)', ram and ram[0x300] & 0x20, err.strip()[:200])
ram, _ = run(vs, dump=19)
check('Vs. System no coin', ram and not ram[0x300] & 0x20)

# savestate: save at frame 10, load it at frame 0 of another run -> frame counter jumps
ram_a, _ = run(rom, '--input', '10:cmd:save=%s' % os.path.join(T, 's.fns'), dump=20)
ram_b, _ = run(rom, '--load-state', os.path.join(T, 's.fns'), dump=10)
check('savestate save/load (frame counter continues)', ram_a and ram_b and ram_b[0x380] == ram_a[0x380],
      '%s vs %s' % (ram_a and ram_a[0x380], ram_b and ram_b[0x380]))

# DIP value reaches the machine (no dip.cfg entry needed to set it)
r = subprocess.run([a.furb, rom, '--dip', '0x5', '--info', '--data-dir', DATA], capture_output=True, text=True)
check('--info runs', r.returncode == 0 and 'mapper:    0.0' in r.stdout, r.stdout[:120])

# settings: --save-config writes the registry; --config reads it back
reg = os.path.join(T, 'saved.reg')
subprocess.run([a.furb, rom, '--frames', '1', '--quiet', '--set', 'NTSCHue=20', '--save-config', reg, '--data-dir', DATA])
txt = open(reg).read() if os.path.exists(reg) else ''
check('--save-config writes .reg with the --set value', '"NTSCHue"=dword:00000014' in txt)

# trace log
tr = os.path.join(T, 'trace.log')
run(rom, '--trace', tr, '--trace-frames', '5-5')
lines = open(tr).read().splitlines() if os.path.exists(tr) else []
check('CPU trace (--trace-frames 5-5)', len(lines) > 100 and 'A:' in lines[0], '%d lines' % len(lines))

# header patch: mapper number changes what loads
r = subprocess.run([a.furb, rom, '--header', 'mapper=2', '--info', '--data-dir', DATA], capture_output=True, text=True)
check('--header mapper=2', 'mapper:    2.0' in r.stdout, r.stdout.splitlines()[2] if r.stdout else r.stderr[:100])

# audio: a WAV of the right length (800 samples per NTSC frame)
w = os.path.join(T, 'a.wav')
run(rom, '--wav', w, frames=60, dump=10)
if os.path.exists(w):
    n = wave.open(w).getnframes()
    check('--wav length (60 frames)', abs(n - 60 * 48000 / 60.0988) < 1600, '%d samples' % n)
else:
    check('--wav written', False)

# NSF: song select goes through the NSF pack's own control dialog (headless)
nsf_code = bytes([0x85, 0x00,                  # init ($8000): STA $00 (song, 0-based)
                  0xA9, 0x0F, 0x8D, 0x15, 0x40,  # LDA #$0F STA $4015
                  0x60,                          # RTS
                  0xA5, 0x00, 0x0A, 0x0A, 0x0A, 0x0A, 0x8D, 0x02, 0x40,  # play ($8008): pitch from song
                  0xA9, 0xBF, 0x8D, 0x00, 0x40, 0xA9, 0x08, 0x8D, 0x03, 0x40,
                  0xE6, 0x01, 0x60])             # INC $01 RTS
nsf = bytearray(b'NESM\x1a\x01' + bytes([3, 1]) + struct.pack('<HHH', 0x8000, 0x8000, 0x8008))
nsf += b'selftest'.ljust(32, b'\0') + b'furb_cli'.ljust(32, b'\0') + b'GPL'.ljust(32, b'\0')
nsf += struct.pack('<H', 16639) + bytes(8) + struct.pack('<H', 19997) + bytes([0, 0]) + bytes(4)
nsf_path = os.path.join(T, 'selftest.nsf')
open(nsf_path, 'wb').write(bytes(nsf) + nsf_code)
ram, err = run(nsf_path, dump=30, frames=40)
check('NSF loads (NSF.so) and waits for Play, like the GUI', ram is not None and ram[1] == 0, err.strip()[:200])
ram, _ = run(nsf_path, '--nsf-song', '1', dump=30, frames=40)
check('--nsf-song 1 starts song 1', ram is not None and ram[0] == 0 and ram[1] > 20, ram and 'song %d, %d plays' % (ram[0], ram[1]))
ram, _ = run(nsf_path, '--nsf-song', '1', '--input', '40:cmd:nsf-song=3', dump=70, frames=80)
check('cmd:nsf-song=3 (slider + Play in its dialog)', ram is not None and ram[0] == 2, ram and 'song %d' % ram[0])
w = os.path.join(T, 'n.wav')
run(nsf_path, '--nsf-song', '2', '--wav', w, frames=60, dump=10)
if os.path.exists(w):
    import array
    samples = array.array('h', wave.open(w).readframes(10 ** 6))
    check('NSF audio is not silent', max(abs(x) for x in samples) > 1000, 'peak %d' % max(abs(x) for x in samples))

# battery save: the program copies $6000 to $0390, then writes $42 there;
# the second run must find $42 (written to DATA/SRAM by the first run's exit)
bat_code = bytes([0x78, 0xD8, 0xA2, 0xFF, 0x9A,
                  0xAD, 0x00, 0x60, 0x8D, 0x90, 0x03,     # LDA $6000 STA $0390
                  0xA9, 0x42, 0x8D, 0x00, 0x60,           # LDA #$42 STA $6000
                  0x4C, 0x10, 0xC0])                      # JMP *
prg = bytearray(16384)
prg[:len(bat_code)] = bat_code
prg[0x3FFA:0x4000] = struct.pack('<HHH', 0xC010, 0xC000, 0xC010)
bat = os.path.join(T, 'battery.nes')
open(bat, 'wb').write(b'NES\x1a\x01\x01\x02' + bytes(9) + bytes(prg) + bytes(8192))
ram1, _ = run(bat)
ram2, _ = run(bat)
sav = os.path.join(DATA, 'SRAM', 'battery.sav')
check('battery save written on exit (DATA/SRAM/battery.sav)', os.path.exists(sav))
check('battery save loaded by the next run', ram2 is not None and ram2[0x390] == 0x42, ram2 and hex(ram2[0x390]))
ram3, _ = run(bat, '--no-save', '--data-dir', os.path.join(T, 'nosave'))
check('--no-save writes nothing', not os.path.exists(os.path.join(T, 'nosave', 'SRAM', 'battery.sav')))

# custom palette: every colour red -> the (blank) screen is red
pal = os.path.join(T, 'red.pal')
open(pal, 'wb').write(bytes([255, 0, 0]) * 64)
out = os.path.join(T, 'pal')
subprocess.run([a.furb, rom, '--frames', '5', '--dump', '4', '--out', out, '--quiet', '--palette', pal, '--data-dir', DATA])
px = open(out + '_f0004.ppm', 'rb').read()[-3:] if os.path.exists(out + '_f0004.ppm') else b''
check('--palette FILE.pal', px == bytes([255, 0, 0]), px.hex())

# cheats.cfg / dip.cfg / BIOS are read from the program's own folder (like the
# Windows build): use a private copy of the program with test entries
import shutil, zlib
prog = os.path.join(T, 'prog')
os.makedirs(os.path.join(prog, 'BIOS'))
shutil.copy(a.furb, prog)
shutil.copytree(os.path.join(os.path.dirname(a.furb), 'Mappers'), os.path.join(prog, 'Mappers'))
pfurb = os.path.join(prog, 'furb_cli')
prg_crc = lambda path: zlib.crc32(open(path, 'rb').read()[16:16 + 16384]) & 0xFFFFFFFF
open(os.path.join(prog, 'cheats.cfg'), 'w', encoding='utf-8-sig').write(
    '; selftest\ngame\tcrc\t0x%08X\n\tcheat\t"Frame counter reads 77" address 0380 replace 77\n' % prg_crc(rom))
open(os.path.join(prog, 'dip.cfg'), 'w', encoding='utf-8-sig').write(
    'game\tname\t"selftest vs"\n\tcrc\t0x%08X\n\tsetting\tmask\t0x03\tdefault\t0x02\tname\t"Test"\n'
    '\tchoice\tvalue\t0x00\t\t\tname\t"Zero"\n\tchoice\tvalue\t0x02\t\t\tname\t"Two"\n' % prg_crc(vs))
r = subprocess.run([pfurb, rom, '--info', '--data-dir', DATA], capture_output=True, text=True)
check('cheats.cfg found next to the program (--info)', 'Frame counter reads 77' in r.stdout, r.stderr[:150])
a_furb, a.furb = a.furb, pfurb
ram, _ = run(rom, '--cheat', 'frame counter')
check('--cheat applies (read of $0380 replaced)', ram is not None and ram[0x380] == 0x78, ram and hex(ram[0x380]))
r = subprocess.run([pfurb, vs, '--info', '--data-dir', DATA], capture_output=True, text=True)
check('dip.cfg definition + default value (--info)', 'value 0x2' in r.stdout and '*0x2=Two' in r.stdout, r.stdout[-200:])
d0, _ = run(vs, '--dip', '0', dump=19)
d3, _ = run(vs, '--dip', '3', dump=19)
check('--dip reaches the Vs. DIP switches ($4016 bits 3-4)', d0 and d3 and (d0[0x300] ^ d3[0x300]) & 0x18, d0 and d3 and '%02x vs %02x' % (d0[0x300], d3[0x300]))
# FDS: needs BIOS/DISKSYS.ROM (not supplied); a missing BIOS must fail cleanly, a present
# one must load the FDS pack.  The blank disk has a disk-info block and no files.
side = bytearray(65500)
side[0:15] = b'\x01*NINTENDO-HVC*'
side[56:58] = b'\x02\x00'
fds = os.path.join(T, 'blank.fds')
open(fds, 'wb').write(b'FDS\x1a\x01' + bytes(11) + bytes(side))
r = subprocess.run([pfurb, fds, '--frames', '10', '--data-dir', DATA], capture_output=True, text=True)
check('FDS without BIOS fails cleanly', r.returncode != 0 and 'failed to load' in r.stderr, r.stderr.strip()[-120:])
open(os.path.join(prog, 'BIOS', 'disksys.rom'), 'wb').write(bytes([0x4C, 0x00, 0xE0]) + bytes(8192 - 3))  # lower case on purpose
r = subprocess.run([pfurb, fds, '--info', '--data-dir', DATA], capture_output=True, text=True)
check('FDS loads (FDS.so) with BIOS\\DISKSYS.ROM (case-insensitive)', 'type:      FDS' in r.stdout, (r.stdout + r.stderr)[-150:])
r = subprocess.run([pfurb, fds, '--frames', '30', '--quiet', '--data-dir', DATA,
                    '--input', '5:cmd:fds-eject;10:cmd:fds-insert;15:cmd:fds-next'], capture_output=True, text=True)
check('FDS disk commands run', r.returncode == 0, r.stderr[-150:])
a.furb = a_furb

if a.rom:
    g = os.path.abspath(a.rom)
    def hashes(*args, frames=400):
        r = subprocess.run([a.furb, g, '--frames', str(frames), '--hashes', '--quiet', '--data-dir', DATA] + list(args),
                           capture_output=True, text=True)
        return r.stdout.split('\n')
    script = '100-104:Start;200-400:Right;250-260:A'
    ref = hashes('--input', script)
    mv = os.path.join(T, 'm.nmv')
    rec = hashes('--input', script, '--movie-record', mv)
    play = hashes('--movie-play', mv)
    check('movie record keeps the run identical', rec == ref)
    check('movie playback reproduces the recorded run', play[50:390] == ref[50:390],
          '%d of 340 frames equal' % sum(x == y for x, y in zip(play[50:390], ref[50:390])))
    st = os.path.join(T, 'g.fns')
    hashes('--input', script + ';200:cmd:save=' + st, frames=201)
    # the state saved at the start of frame 200 continues AS frame 200: shift the rest of the script by 200
    later = []
    for x in script.split(';'):
        rng, act = x.split(':', 1)
        lo, hi = (int(v) for v in (rng.split('-') + [rng])[:2])
        if hi >= 200:
            later.append('%d-%d:%s' % (max(lo, 200) - 200, hi - 200, act))
    cont = hashes('--load-state', st, '--input', ';'.join(later), frames=100)
    same = sum(1 for i in range(100) if cont[i].split(' ')[-1] == ref[200 + i].split(' ')[-1])
    check('savestate at 200 + reload continues the same run', same >= 95, '%d of 100 frames equal' % same)

print('\n%d failure(s); temp dir %s' % (len(fails), T))
sys.exit(1 if fails else 0)
