"""asm6502.py -- a small 6502 assembler as a Python API, for the CI smoke ROMs.

    a = Asm(0xE000)
    a.label('reset')
    a.op('LDA', '#', 0x3F)          # immediate
    a.op('STA', 'abs', 0x2006)      # zp, zpx, zpy, abs, absx, absy, ind, indx, indy
    a.op('BNE', 'rel', 'reset')     # branches take a label (or an address)
    a.op('ASL')                     # implied / accumulator
    a.byte(1, 2, 3); a.word('reset')
    code = a.assemble()             # bytes from org to the last byte emitted

Operands may be numbers or label names (resolved when assembling); ('lo',
name) and ('hi', name) give a label's low or high byte for immediates.
Only the official opcodes: the smoke ROMs test what every game uses.
"""

OPS = {
    'ADC': {'#': 0x69, 'zp': 0x65, 'zpx': 0x75, 'abs': 0x6D, 'absx': 0x7D, 'absy': 0x79, 'indx': 0x61, 'indy': 0x71},
    'AND': {'#': 0x29, 'zp': 0x25, 'zpx': 0x35, 'abs': 0x2D, 'absx': 0x3D, 'absy': 0x39, 'indx': 0x21, 'indy': 0x31},
    'ASL': {'acc': 0x0A, 'zp': 0x06, 'zpx': 0x16, 'abs': 0x0E, 'absx': 0x1E},
    'BCC': {'rel': 0x90}, 'BCS': {'rel': 0xB0}, 'BEQ': {'rel': 0xF0}, 'BMI': {'rel': 0x30},
    'BNE': {'rel': 0xD0}, 'BPL': {'rel': 0x10}, 'BVC': {'rel': 0x50}, 'BVS': {'rel': 0x70},
    'BIT': {'zp': 0x24, 'abs': 0x2C},
    'BRK': {'imp': 0x00},
    'CLC': {'imp': 0x18}, 'CLD': {'imp': 0xD8}, 'CLI': {'imp': 0x58}, 'CLV': {'imp': 0xB8},
    'CMP': {'#': 0xC9, 'zp': 0xC5, 'zpx': 0xD5, 'abs': 0xCD, 'absx': 0xDD, 'absy': 0xD9, 'indx': 0xC1, 'indy': 0xD1},
    'CPX': {'#': 0xE0, 'zp': 0xE4, 'abs': 0xEC},
    'CPY': {'#': 0xC0, 'zp': 0xC4, 'abs': 0xCC},
    'DEC': {'zp': 0xC6, 'zpx': 0xD6, 'abs': 0xCE, 'absx': 0xDE},
    'DEX': {'imp': 0xCA}, 'DEY': {'imp': 0x88},
    'EOR': {'#': 0x49, 'zp': 0x45, 'zpx': 0x55, 'abs': 0x4D, 'absx': 0x5D, 'absy': 0x59, 'indx': 0x41, 'indy': 0x51},
    'INC': {'zp': 0xE6, 'zpx': 0xF6, 'abs': 0xEE, 'absx': 0xFE},
    'INX': {'imp': 0xE8}, 'INY': {'imp': 0xC8},
    'JMP': {'abs': 0x4C, 'ind': 0x6C},
    'JSR': {'abs': 0x20},
    'LDA': {'#': 0xA9, 'zp': 0xA5, 'zpx': 0xB5, 'abs': 0xAD, 'absx': 0xBD, 'absy': 0xB9, 'indx': 0xA1, 'indy': 0xB1},
    'LDX': {'#': 0xA2, 'zp': 0xA6, 'zpy': 0xB6, 'abs': 0xAE, 'absy': 0xBE},
    'LDY': {'#': 0xA0, 'zp': 0xA4, 'zpx': 0xB4, 'abs': 0xAC, 'absx': 0xBC},
    'LSR': {'acc': 0x4A, 'zp': 0x46, 'zpx': 0x56, 'abs': 0x4E, 'absx': 0x5E},
    'NOP': {'imp': 0xEA},
    'ORA': {'#': 0x09, 'zp': 0x05, 'zpx': 0x15, 'abs': 0x0D, 'absx': 0x1D, 'absy': 0x19, 'indx': 0x01, 'indy': 0x11},
    'PHA': {'imp': 0x48}, 'PHP': {'imp': 0x08}, 'PLA': {'imp': 0x68}, 'PLP': {'imp': 0x28},
    'ROL': {'acc': 0x2A, 'zp': 0x26, 'zpx': 0x36, 'abs': 0x2E, 'absx': 0x3E},
    'ROR': {'acc': 0x6A, 'zp': 0x66, 'zpx': 0x76, 'abs': 0x6E, 'absx': 0x7E},
    'RTI': {'imp': 0x40}, 'RTS': {'imp': 0x60},
    'SBC': {'#': 0xE9, 'zp': 0xE5, 'zpx': 0xF5, 'abs': 0xED, 'absx': 0xFD, 'absy': 0xF9, 'indx': 0xE1, 'indy': 0xF1},
    'SEC': {'imp': 0x38}, 'SED': {'imp': 0xF8}, 'SEI': {'imp': 0x78},
    'STA': {'zp': 0x85, 'zpx': 0x95, 'abs': 0x8D, 'absx': 0x9D, 'absy': 0x99, 'indx': 0x81, 'indy': 0x91},
    'STX': {'zp': 0x86, 'zpy': 0x96, 'abs': 0x8E},
    'STY': {'zp': 0x84, 'zpx': 0x94, 'abs': 0x8C},
    'TAX': {'imp': 0xAA}, 'TAY': {'imp': 0xA8}, 'TSX': {'imp': 0xBA}, 'TXA': {'imp': 0x8A},
    'TXS': {'imp': 0x9A}, 'TYA': {'imp': 0x98},
}
SIZE = {'imp': 1, 'acc': 1, '#': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'indx': 2, 'indy': 2, 'rel': 2,
        'abs': 3, 'absx': 3, 'absy': 3, 'ind': 3}


class Asm:
    def __init__(self, org):
        self.org = org
        self.items = []         # (addr, kind, payload)
        self.pc = org
        self.labels = {}

    def label(self, name):
        if name in self.labels:
            raise ValueError('label %s defined twice' % name)
        self.labels[name] = self.pc
        return self.pc

    def op(self, mn, mode=None, arg=None):
        if mode is None:
            mode = 'acc' if 'acc' in OPS[mn] else 'imp'
        if mode not in OPS[mn]:
            raise ValueError('%s has no %s mode' % (mn, mode))
        self.items.append((self.pc, 'op', (OPS[mn][mode], mode, arg)))
        self.pc += SIZE[mode]

    def byte(self, *vals):
        for v in vals:
            self.items.append((self.pc, 'byte', v))
            self.pc += 1

    def word(self, *vals):
        for v in vals:
            self.items.append((self.pc, 'word', v))
            self.pc += 2

    def fill(self, addr, value=0xFF):
        """Pad with value up to addr."""
        if addr < self.pc:
            raise ValueError('fill to $%04X: already at $%04X' % (addr, self.pc))
        while self.pc < addr:
            self.byte(value)

    def value(self, v):
        if isinstance(v, tuple):
            part, name = v
            x = self.value(name)
            return x & 0xFF if part == 'lo' else x >> 8 & 0xFF
        if isinstance(v, str):
            if v not in self.labels:
                raise ValueError('undefined label %s' % v)
            return self.labels[v]
        return v

    def assemble(self):
        out = bytearray(self.pc - self.org)
        for addr, kind, p in self.items:
            o = addr - self.org
            if kind == 'byte':
                out[o] = self.value(p) & 0xFF
            elif kind == 'word':
                v = self.value(p)
                out[o:o + 2] = bytes((v & 0xFF, v >> 8 & 0xFF))
            else:
                code, mode, arg = p
                out[o] = code
                n = SIZE[mode]
                if n == 1:
                    continue
                v = self.value(arg)
                if mode == 'rel':
                    d = v - (addr + 2)
                    if not -128 <= d <= 127:
                        raise ValueError('branch at $%04X out of range (%d)' % (addr, d))
                    out[o + 1] = d & 0xFF
                elif n == 2:
                    if not 0 <= v <= 0xFF and mode != '#':
                        raise ValueError('zero-page operand $%X at $%04X' % (v, addr))
                    out[o + 1] = v & 0xFF
                else:
                    out[o + 1:o + 3] = bytes((v & 0xFF, v >> 8 & 0xFF))
        return bytes(out)
