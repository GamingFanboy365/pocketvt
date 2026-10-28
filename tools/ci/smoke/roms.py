"""roms.py -- the CI smoke ROMs, generated from this source (no game ROM is
ever needed or committed).  Each builder returns (nes_bytes, info).

  cpu     mapper 0.  Runs ~650 cases of the official 6502 instructions (ADC/SBC
          over a grid of operands and carries, logic, compares, BIT, shifts and
          rotates on A and memory, INC/DEC, transfers, every addressing mode
          with zero-page and page wrap, JMP ($xxFF), stack, JSR/RTS/RTI/BRK,
          branches, flag instructions) and stores each result and P at
          $0200-$07FF.  Expected RAM: Furbtendulator's (and py65's).
  vtbank  NES 2.0 mapper 256 (VT03 OneBus), 64K PRG.  Reads back which 8K bank
          each window shows at power-on and after $4107-$410B, $4105 (COMR6)
          and the MMC3-style $8000/$8001 writes, including an outer-bank window
          (PS, PQ3).  Expected RAM: Furbtendulator's.
  ppu     mapper 0.  A still picture: four BG palettes by attribute, tiles of
          all four colours, fine X scroll, eight sprites with every flip and
          one behind the background.  Expected: PocketVT's own picture hash
          (blessed after compare_furb scored it against Furbtendulator).
"""
from asm6502 import Asm

V8 = [0x00, 0x01, 0x40, 0x7F, 0x80, 0x81, 0xC0, 0xFF]
V4 = [0x00, 0x7F, 0x80, 0xFF]
DONE = 0x00F8           # $F8/$F9 = $A5 $5A when a program has finished
OUT = 0x0200            # results $0200-$06FF; $0700-$07FF is the cpu test's scratch page


def ines(prg, chr_, mapper=0, sub=0, nes2=False, console=None):
    h = bytearray(16)
    h[0:4] = b'NES\x1a'
    h[4] = len(prg) // 16384
    h[5] = len(chr_) // 8192
    h[6] = (mapper & 0x0F) << 4
    h[7] = mapper & 0xF0
    if nes2:
        h[7] |= 0x08
        h[8] = (mapper >> 8 & 0x0F) | sub << 4
        if console is not None:
            h[7] |= 0x03
            h[13] = console
    return bytes(h) + prg + chr_


def common_reset(a, ram_clear=True):
    a.label('reset')
    a.op('SEI')
    a.op('CLD')
    a.op('LDX', '#', 0x40)
    a.op('STX', 'abs', 0x4017)      # no APU frame IRQ
    a.op('LDX', '#', 0xFF)
    a.op('TXS')
    a.op('LDA', '#', 0x00)
    a.op('STA', 'abs', 0x2000)      # no NMI yet
    a.op('STA', 'abs', 0x2001)
    if ram_clear:
        a.op('TAX')
        a.label('clr')
        for page in range(8):
            a.op('STA', 'absx', page << 8)
        a.op('INX')
        a.op('BNE', 'rel', 'clr')


def finish(a):
    a.op('LDA', '#', 0xA5)
    a.op('STA', 'zp', DONE)
    a.op('LDA', '#', 0x5A)
    a.op('STA', 'zp', DONE + 1)
    a.label('done')
    a.op('JMP', 'abs', 'done')


# ---------------------------------------------------------------- cpu
class Cases:
    def __init__(self, a):
        self.a, self.out, self.names = a, OUT, []

    def slot(self, name):
        """Address for this case's two result bytes."""
        o = self.out
        self.out += 2
        self.names.append(name)
        if self.out > 0x0700:
            raise ValueError('cpu test: results run into the scratch page $0700')
        return o

    def flags(self, p, a_val=None):
        """P = p (and A = a_val) without disturbing the other."""
        a = self.a
        a.op('LDA', '#', p)
        a.op('PHA')
        if a_val is not None:
            a.op('LDA', '#', a_val)
        a.op('PLP')

    def store_a_p(self, o):
        a = self.a
        a.op('PHP')
        a.op('STA', 'abs', o)
        a.op('PLA')
        a.op('STA', 'abs', o + 1)

    def store_m_p(self, o, addr):
        a = self.a
        a.op('PHP')
        a.op('LDA', 'zp' if addr < 0x100 else 'abs', addr)
        a.op('STA', 'abs', o)
        a.op('PLA')
        a.op('STA', 'abs', o + 1)

    def store_x_p(self, o, reg='X'):
        a = self.a
        a.op('PHP')
        a.op('ST' + reg, 'abs', o)
        a.op('PLA')
        a.op('STA', 'abs', o + 1)


def build_cpu():
    a = Asm(0x8000)
    # data the addressing-mode cases read: a table across a page boundary
    a.label('table')
    for i in range(0x120):
        a.byte((i * 7 + 3) & 0xFF)
    common_reset(a)
    c = Cases(a)
    for mn in ('ADC', 'SBC'):
        for av in V8:
            for mv in V8:
                for cin in (0, 1):
                    p = cin | (0xC2 if (av ^ mv) & 1 else 0)    # stale N/V/Z must be replaced
                    o = c.slot('%s A=%02X M=%02X C=%d' % (mn, av, mv, cin))
                    c.flags(p, av)
                    a.op(mn, '#', mv)
                    c.store_a_p(o)
    for mn in ('AND', 'ORA', 'EOR'):
        for av in V4:
            for mv in V8:
                o = c.slot('%s A=%02X M=%02X' % (mn, av, mv))
                c.flags(0x00 if mv & 1 else 0xC3, av)
                a.op(mn, '#', mv)
                c.store_a_p(o)
    for av in V8:
        for mv in V4:
            o = c.slot('CMP A=%02X M=%02X' % (av, mv))
            c.flags(0xC3 if av & 1 else 0x00, av)
            a.op('CMP', '#', mv)
            c.store_a_p(o)
    for reg in ('X', 'Y'):
        for rv in V4:
            for mv in V4:
                o = c.slot('CP%s %s=%02X M=%02X' % (reg, reg, rv, mv))
                a.op('LD' + reg, '#', rv)
                c.flags(0x00)
                a.op('CP' + reg, '#', mv)
                c.store_x_p(o, reg)
    for av in V4:
        for mv in V4:
            o = c.slot('BIT A=%02X M=%02X' % (av, mv))
            a.op('LDX', '#', mv)
            a.op('STX', 'zp', 0x10)
            c.flags(0x00 if mv & 2 else 0xC3, av)
            a.op('BIT', 'zp', 0x10)
            c.store_a_p(o)
    for mn in ('ASL', 'LSR', 'ROL', 'ROR'):
        for v in V8:
            for cin in (0, 1):
                o = c.slot('%s A=%02X C=%d' % (mn, v, cin))
                c.flags(cin | 0x82, v)
                a.op(mn)
                c.store_a_p(o)
        for v in V4:
            for cin in (0, 1):
                o = c.slot('%s $10=%02X C=%d' % (mn, v, cin))
                a.op('LDX', '#', v)
                a.op('STX', 'zp', 0x10)
                c.flags(cin)
                a.op(mn, 'zp', 0x10)
                c.store_m_p(o, 0x10)
    for mn in ('INC', 'DEC'):
        for v in V8:
            o = c.slot('%s $10=%02X' % (mn, v))
            a.op('LDX', '#', v)
            a.op('STX', 'zp', 0x10)
            c.flags(0x00)
            a.op(mn, 'zp', 0x10)
            c.store_m_p(o, 0x10)
        for v in V4:                                        # abs,X across a page
            o = c.slot('%s $06F0,X=$0710 =%02X' % (mn, v))
            a.op('LDX', '#', v)
            a.op('STX', 'abs', 0x0710)
            a.op('LDX', '#', 0x20)
            c.flags(0x00)
            a.op(mn, 'absx', 0x06F0)
            c.store_m_p(o, 0x0710)
    for mn, reg in (('INX', 'X'), ('DEX', 'X'), ('INY', 'Y'), ('DEY', 'Y')):
        for v in V4:
            o = c.slot('%s %s=%02X' % (mn, reg, v))
            a.op('LD' + reg, '#', v)
            c.flags(0x00)
            a.op(mn)
            c.store_x_p(o, reg)
    for v in (0x00, 0x7F, 0x80):
        for mn, src, dst in (('TAX', 'A', 'X'), ('TAY', 'A', 'Y'), ('TXA', 'X', 'A'), ('TYA', 'Y', 'A')):
            o = c.slot('%s %s=%02X' % (mn, src, v))
            a.op('LD' + src, '#', v)
            a.op('LD' + dst, '#', 0x55)
            c.flags(0x82 if v == 0 else 0x02)
            a.op(mn)
            if dst == 'A':
                c.store_a_p(o)
            else:
                c.store_x_p(o, dst)
        o = c.slot('TXS/TSX X=%02X' % v)
        a.op('LDX', '#', v)
        a.op('TXS')
        a.op('LDX', '#', 0x55)
        c.flags(0x00)
        a.op('TSX')
        c.store_x_p(o, 'X')
        a.op('LDX', '#', 0xFF)
        a.op('TXS')
    # addressing modes: loads
    t = a.labels['table']
    o = c.slot('LDA zp,X wraps in page 0')
    a.op('LDA', '#', 0x5A); a.op('STA', 'zp', 0x10)
    a.op('LDA', '#', 0x33); a.op('STA', 'abs', 0x0110)
    a.op('LDX', '#', 0xF0); c.flags(0x00); a.op('LDA', 'zpx', 0x20); c.store_a_p(o)
    o = c.slot('LDX zp,Y wraps in page 0')
    a.op('LDY', '#', 0xF0); c.flags(0x00); a.op('LDX', 'zpy', 0x20); c.store_x_p(o, 'X')
    o = c.slot('LDA abs,X across a page')
    a.op('LDX', '#', 0x30); c.flags(0x00); a.op('LDA', 'absx', t + 0xF0); c.store_a_p(o)
    o = c.slot('LDA abs,Y across a page')
    a.op('LDY', '#', 0x31); c.flags(0x00); a.op('LDA', 'absy', t + 0xF0); c.store_a_p(o)
    a.op('LDA', '#', (t + 0xFE) & 0xFF); a.op('STA', 'zp', 0x20)
    a.op('LDA', '#', (t + 0xFE) >> 8); a.op('STA', 'zp', 0x21)
    o = c.slot('LDA (zp,X) with X wrap')
    a.op('LDX', '#', 0x28); c.flags(0x00); a.op('LDA', 'indx', 0xF8); c.store_a_p(o)
    o = c.slot('LDA (zp),Y across a page')
    a.op('LDY', '#', 0x05); c.flags(0x00); a.op('LDA', 'indy', 0x20); c.store_a_p(o)
    a.op('LDA', '#', (t + 0x40) & 0xFF); a.op('STA', 'zp', 0xFF)
    a.op('LDA', '#', (t + 0x40) >> 8); a.op('STA', 'zp', 0x00)
    o = c.slot('LDA ($FF),Y pointer wraps to $00')
    a.op('LDY', '#', 0x02); c.flags(0x00); a.op('LDA', 'indy', 0xFF); c.store_a_p(o)
    # stores through every mode, read back
    a.op('LDA', '#', 0x40); a.op('STA', 'zp', 0x22)
    a.op('LDA', '#', 0x07); a.op('STA', 'zp', 0x23)          # ($22) = $0740
    for k, (mode, arg, x, y, where) in enumerate((
            ('zpx', 0xF8, 0x1A, 0, 0x12), ('abs', 0x0750, 0, 0, 0x0750), ('absx', 0x06F8, 0x10, 0, 0x0708),
            ('absy', 0x06F9, 0, 0x10, 0x0709), ('indx', 0xF0, 0x32, 0, 0x0740), ('indy', 0x22, 0, 0x08, 0x0748))):
        o = c.slot('STA %s' % mode)
        a.op('LDX', '#', x); a.op('LDY', '#', y)
        a.op('LDA', '#', 0x90 + k)
        a.op('STA', mode, arg)
        a.op('LDA', '#', 0)
        a.op('LDA', 'zp' if where < 0x100 else 'abs', where)
        c.store_a_p(o)
    o = c.slot('STX zp,Y / STY zp,X')
    a.op('LDX', '#', 0x66); a.op('LDY', '#', 0x04); a.op('STX', 'zpy', 0x30)
    a.op('LDY', '#', 0x77); a.op('LDX', '#', 0x05); a.op('STY', 'zpx', 0x30)
    a.op('LDA', 'zp', 0x34); a.op('CLC'); a.op('ADC', 'zp', 0x35); c.store_a_p(o)
    # JMP ($xxFF): the high byte comes from $xx00
    a.op('LDA', '#', ('lo', 'jmpi_ok')); a.op('STA', 'zp', 0xFF)
    a.op('LDA', '#', ('hi', 'jmpi_ok')); a.op('STA', 'zp', 0x00)
    a.op('LDA', '#', ('hi', 'jmpi_ok')); a.op('EOR', '#', 0x40); a.op('STA', 'abs', 0x0100)
    o = c.slot('JMP ($00FF) page wrap')
    a.op('JMP', 'ind', 0x00FF)            # a CPU without the wrap lands $4000 away
    a.label('jmpi_ok')
    a.op('LDA', '#', 0x01)
    a.op('STA', 'abs', o); a.op('LDA', '#', 0); a.op('STA', 'abs', 0x0100)
    # JSR pushes the address of its last byte; RTS returns after it
    o = c.slot('JSR pushes PC-1')
    a.op('JSR', 'abs', 'sub_peek')
    a.label('after_jsr')
    a.op('STX', 'abs', o); a.op('STY', 'abs', o + 1)
    # RTI restores P and PC exactly (no +1)
    o = c.slot('RTI')
    a.op('LDA', '#', ('hi', 'rti_to')); a.op('PHA')
    a.op('LDA', '#', ('lo', 'rti_to')); a.op('PHA')
    a.op('LDA', '#', 0xCF); a.op('PHA')
    a.op('RTI')
    a.op('LDA', '#', 0xEE)                              # skipped
    a.label('rti_to')
    a.op('PHP'); a.op('PLA'); a.op('STA', 'abs', o)
    a.op('TSX'); a.op('STX', 'abs', o + 1)
    a.op('CLD')
    # BRK: pushes PC+2 and P with B set, through $FFFE; the handler returns
    o = c.slot('BRK pushes P|B')
    c.flags(0x01)
    a.op('BRK'); a.op('NOP')
    a.op('STA', 'abs', o); a.op('STX', 'abs', o + 1)
    o = c.slot('BRK return address')
    a.op('STY', 'abs', o); a.op('LDA', 'zp', 0x40); a.op('STA', 'abs', o + 1)
    # PHP always pushes B and bit 5; PLP keeps neither
    for p in (0x00, 0xFF, 0x30, 0xCF):
        o = c.slot('PHP/PLP P=%02X' % p)
        a.op('LDA', '#', p); a.op('PHA'); a.op('PLP')
        a.op('PHP'); a.op('PLA'); a.op('STA', 'abs', o)
        a.op('CLD'); a.op('SEI')
        a.op('PHP'); a.op('PLA'); a.op('STA', 'abs', o + 1)
    # branches, both ways
    for k, (mn, bit) in enumerate((('BCC', 0x01), ('BCS', 0x01), ('BNE', 0x02), ('BEQ', 0x02),
                                   ('BVC', 0x40), ('BVS', 0x40), ('BPL', 0x80), ('BMI', 0x80))):
        for setbit in (0, 1):
            o = c.slot('%s with flag %d' % (mn, setbit))
            c.flags(bit if setbit else 0)
            a.op(mn, 'rel', 'br%d_%d' % (k, setbit))
            a.op('LDA', '#', 0x00)
            a.op('JMP', 'abs', 'brj%d_%d' % (k, setbit))
            a.label('br%d_%d' % (k, setbit))
            a.op('LDA', '#', 0x01)
            a.label('brj%d_%d' % (k, setbit))
            a.op('STA', 'abs', o)
    for mn, p0 in (('SEC', 0x00), ('SED', 0x00), ('SEI', 0x00), ('CLC', 0xFF), ('CLD', 0xFF),
                   ('CLI', 0xFF), ('CLV', 0xFF)):
        o = c.slot(mn)
        c.flags(p0)
        a.op(mn)
        a.op('PHP'); a.op('PLA'); a.op('STA', 'abs', o)
        a.op('SEI'); a.op('CLD')
    finish(a)
    # subroutines and vectors
    a.label('sub_peek')
    a.op('TSX')
    a.op('LDA', 'absx', 0x0101); a.op('TAY')            # low byte of the pushed address
    a.op('LDA', 'absx', 0x0102); a.op('TAX')            # high byte
    a.op('RTS')
    a.label('brk_handler')
    a.op('PLA'); a.op('PHA')                            # P as pushed
    a.op('TSX')
    a.op('LDY', 'absx', 0x0102)                         # return address low
    a.op('PHA'); a.op('LDA', 'absx', 0x0103); a.op('STA', 'zp', 0x40); a.op('PLA')
    a.op('LDX', '#', 0xBB)
    a.op('RTI')
    a.label('nmi')
    a.op('RTI')
    a.fill(0xFFFA)
    a.word('nmi', 'reset', 'brk_handler')
    prg = a.assemble()
    return ines(prg, bytes(8192)), dict(cases=c.names, ram=(0x0000, 0x0800))


# ---------------------------------------------------------------- vtbank
def build_vtbank():
    """Bank 7 is fixed at $E000; its code starts after its two signature bytes."""
    banks = []
    for b in range(8):
        d = bytearray([0xEA] * 0x2000)
        d[0], d[1] = b, b ^ 0xFF
        banks.append(d)
    a = Asm(0xE002)                     # $E000/$E001 hold bank 7's signature
    common_reset(a)
    names = []
    out = [OUT]

    def read_windows(tag, wins=(0x8000, 0xA000, 0xC000, 0xE000)):
        for w in wins:
            names.append('%s $%04X' % (tag, w))
            a.op('LDA', 'abs', w); a.op('STA', 'abs', out[0])
            a.op('LDA', 'abs', w + 1); a.op('STA', 'abs', out[0] + 1)
            out[0] += 2

    read_windows('power-on')
    for b in range(8):
        a.op('LDA', '#', b); a.op('STA', 'abs', 0x4107)
        read_windows('$4107=%d' % b, (0x8000,))
    for b in range(8):
        a.op('LDA', '#', b); a.op('STA', 'abs', 0x4108)
        read_windows('$4108=%d' % b, (0xA000,))
    a.op('LDA', '#', 0x40); a.op('STA', 'abs', 0x410B)       # PQ2EN
    for b in range(8):
        a.op('LDA', '#', b); a.op('STA', 'abs', 0x4109)
        read_windows('PQ2EN $4109=%d' % b, (0xC000,))
    a.op('LDA', '#', 2); a.op('STA', 'abs', 0x4107)
    a.op('LDA', '#', 5); a.op('STA', 'abs', 0x4109)
    a.op('LDA', '#', 0x40); a.op('STA', 'abs', 0x4105)       # COMR6: $8000 <-> $C000
    read_windows('COMR6')
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x4105)
    read_windows('COMR6 off')
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x410B)
    read_windows('PQ2EN off')
    for ptr, win in ((6, 0x8000), (7, 0xA000)):              # MMC3-style data writes
        for b in (3, 6):
            a.op('LDA', '#', ptr); a.op('STA', 'abs', 0x8000)
            a.op('LDA', '#', b); a.op('STA', 'abs', 0x8001)
            read_windows('MMC3 R%d=%d' % (ptr, b), (win,))
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x8000)
    # a 4-bank outer window: PS=4 (AND 3), PQ3=4 -> banks 4-7
    a.op('LDA', '#', 0x04); a.op('STA', 'abs', 0x410A)
    a.op('LDA', '#', 0x04); a.op('STA', 'abs', 0x410B)
    for b in (0, 1, 2, 3):
        a.op('LDA', '#', b); a.op('STA', 'abs', 0x4107)
        read_windows('PS=4 PQ3=4 $4107=%d' % b, (0x8000,))
    read_windows('PS=4 PQ3=4')
    # PS first: clearing PQ3 while PS=4 would move this code's own bank
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x410B); a.op('STA', 'abs', 0x410A)
    finish(a)
    a.label('nmi')
    a.op('RTI')
    a.fill(0xFFFA)
    a.word('nmi', 'reset', 'nmi')
    code = a.assemble()
    banks[7][2:] = code
    prg = b''.join(bytes(b) for b in banks)
    # like the real VT03 dumps: no CHR-ROM (the PPU reads PRG over the one bus)
    return ines(prg, b'', mapper=256, sub=0, nes2=True, console=7), dict(cases=names, ram=(0x0000, 0x0800))


# ---------------------------------------------------------------- ppu
def chr_tiles():
    """Tile 0 blank; 1-3 solid colours; 4 checkerboard 1/2; 5 a diagonal in 3;
    6 an arrow (sprites: shows flips); 7 a frame in 1 with a 2 dot; 8 stripes."""
    t = bytearray(8192)

    def put(n, rows):          # rows: 8 strings of 8 digits 0-3
        for y, r in enumerate(rows):
            lo = hi = 0
            for x, ch in enumerate(r):
                v = int(ch)
                lo |= (v & 1) << (7 - x)
                hi |= (v >> 1) << (7 - x)
            t[n * 16 + y] = lo
            t[n * 16 + 8 + y] = hi
    put(1, ['11111111'] * 8)
    put(2, ['22222222'] * 8)
    put(3, ['33333333'] * 8)
    put(4, ['12121212', '21212121'] * 4)
    put(5, ['3' + '0' * 7, '03000000', '00300000', '00030000', '00003000', '00000300', '00000030', '00000003'])
    put(6, ['00011000', '00111100', '01111110', '11111111', '00122100', '00122100', '00133100', '00333300'])
    put(7, ['11111111', '10000001', '10000001', '10022001', '10022001', '10000001', '10000001', '11111111'])
    put(8, ['11112222', '11112222', '33330000', '33330000'] * 2)
    # the same set in the sprite table ($1000)
    t[0x1000:0x1000 + 16 * 9] = t[0:16 * 9]
    return bytes(t)


def build_ppu():
    a = Asm(0xC000)
    common_reset(a)
    # wait two vblanks for the PPU
    for k in range(2):
        a.label('vbl%d' % k)
        a.op('BIT', 'abs', 0x2002)
        a.op('BPL', 'rel', 'vbl%d' % k)
    # palettes
    a.op('LDA', '#', 0x3F); a.op('STA', 'abs', 0x2006)
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x2006)
    a.op('LDX', '#', 0)
    a.label('palloop')
    a.op('LDA', 'absx', 'palette'); a.op('STA', 'abs', 0x2007)
    a.op('INX'); a.op('CPX', '#', 32); a.op('BNE', 'rel', 'palloop')
    # nametable: tile = (row + col) % 9, rows 0-29
    a.op('LDA', '#', 0x20); a.op('STA', 'abs', 0x2006)
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x2006)
    a.op('LDY', '#', 0)                   # row
    a.label('row')
    a.op('STY', 'zp', 0x10)
    a.op('LDX', '#', 0)                   # col
    a.label('col')
    a.op('TXA'); a.op('CLC'); a.op('ADC', 'zp', 0x10)
    a.label('mod9')
    a.op('CMP', '#', 9); a.op('BCC', 'rel', 'mod9_done'); a.op('SBC', '#', 9); a.op('JMP', 'abs', 'mod9')
    a.label('mod9_done')
    a.op('STA', 'abs', 0x2007)
    a.op('INX'); a.op('CPX', '#', 32); a.op('BNE', 'rel', 'col')
    a.op('INY'); a.op('CPY', '#', 30); a.op('BNE', 'rel', 'row')
    # attributes: palette = (x/2 + y/2) & 3 per 16x16 area
    a.op('LDX', '#', 0)
    a.label('attr')
    a.op('LDA', 'absx', 'attrs'); a.op('STA', 'abs', 0x2007)
    a.op('INX'); a.op('CPX', '#', 64); a.op('BNE', 'rel', 'attr')
    # sprites to $0200, then OAM DMA every frame from the NMI
    a.op('LDX', '#', 0)
    a.label('spr')
    a.op('LDA', 'absx', 'sprites'); a.op('STA', 'absx', 0x0200)
    a.op('INX'); a.op('BNE', 'rel', 'spr')
    finish_ppu(a)
    a.label('nmi')
    a.op('PHA')
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x2003)
    a.op('LDA', '#', 0x02); a.op('STA', 'abs', 0x4014)
    a.op('LDA', '#', 0x03); a.op('STA', 'abs', 0x2005)        # fine X = 3
    a.op('LDA', '#', 0x00); a.op('STA', 'abs', 0x2005)
    a.op('LDA', '#', 0x88); a.op('STA', 'abs', 0x2000)        # NMI on, sprites at $1000
    a.op('LDA', '#', 0x1E); a.op('STA', 'abs', 0x2001)
    a.op('INC', 'zp', 0x11)
    a.op('PLA')
    a.op('RTI')
    a.label('palette')
    a.byte(0x0F, 0x16, 0x27, 0x30, 0x0F, 0x1A, 0x2A, 0x3A, 0x0F, 0x12, 0x22, 0x32, 0x0F, 0x14, 0x24, 0x34,
           0x0F, 0x06, 0x17, 0x38, 0x0F, 0x09, 0x19, 0x29, 0x0F, 0x01, 0x11, 0x21, 0x0F, 0x04, 0x15, 0x26)
    a.label('attrs')
    for y in range(8):
        for x in range(8):
            q = [((2 * x + dx) + (2 * y + dy)) & 3 for dy in (0, 1) for dx in (0, 1)]
            a.byte(q[0] | q[1] << 2 | q[2] << 4 | q[3] << 6)
    a.label('sprites')
    spr = []
    for i in range(8):                     # y, tile, attr, x
        spr += [40 + i * 18, 6, (i & 3) | (i & 3) << 6 if i < 4 else (i & 3) | 0x40 * (i & 1) | 0x80 * (i >> 1 & 1), 60 + i * 20]
    spr[4 * 7 + 2] |= 0x20                 # the last one behind the background
    spr += [0xFF, 0, 0, 0] * 56            # the rest off screen
    a.byte(*spr)
    a.fill(0xFFFA)
    a.word('nmi', 'reset', 'nmi')
    prg = a.assemble()
    return ines(prg, chr_tiles()), dict(picture_frame=60)


def finish_ppu(a):
    a.op('LDA', '#', 0x88); a.op('STA', 'abs', 0x2000)
    finish(a)


def build_all():
    cpu, cpu_info = build_cpu()
    vt, vt_info = build_vtbank()
    ppu, ppu_info = build_ppu()
    return {'cpu': (cpu, cpu_info), 'vtbank': (vt, vt_info), 'ppu': (ppu, ppu_info)}
