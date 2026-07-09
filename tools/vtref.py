#!/usr/bin/env python3
"""Minimal OneBus/VT03 CPU-level reference for Lonely Island boot analysis.

Not a full emulator: CPU + PRG banking + opcode descramble + timer IRQ +
NMI-per-frame + controllers + register write log.  No PPU rendering, no APU
audio -- the point is to observe the game's STATE MACHINE under
known-correct register semantics (NESdev VT02+ pages) and diff it against
PocketVT's behavior.
"""
import sys
from py65.devices.mpu6502 import MPU

PRG = open('/home/claude/li_prg.bin', 'rb').read()
PRG_8K_MASK = (len(PRG) // 0x2000) - 1        # 128KB -> 16 banks -> 0x0F

def dec56(b):
    r = b & ~0x60
    if b & 0x20: r |= 0x40
    if b & 0x40: r |= 0x20
    return r

class Sys:
    def __init__(self):
        self.ram = bytearray(0x800)
        self.pq = [0, 0]                      # $4107, $4108 inner banks
        self.reg41 = bytearray(0x100)
        self.ppu = bytearray(0x40)            # $2000-$203F shadow incl. VT $2010+
        self.vblank = False
        # Free-running from reset: PocketVT (and evidently the hardware --
        # LI never arms the timer, gate zp $47 stays 0, yet relies on the
        # $FCAF IRQ raster engine) delivers a periodic timer IRQ with the
        # power-on period (preload 0 -> 256 scanlines).  $4103 = ack.
        self.timer_enabled = True
        self.timer_counting = True
        self.timer_preload = 0
        self.timer_counter = 256
        self.irq_line = False
        self.pad = [0, 0]                     # injected pad state P1,P2
        self.shift = [0, 0]
        self.strobe = 0
        self.log = []
        self.frame = 0
        self.oamdma_src = None

    def bank_for(self, addr):
        slot = (addr >> 13) & 3
        if slot == 0:  bank = self.pq[0]
        elif slot == 1: bank = self.pq[1]
        elif slot == 2: bank = 0xFE
        else:           bank = 0xFF
        return bank & PRG_8K_MASK

    def prg_read(self, addr):
        return PRG[(self.bank_for(addr) << 13) | (addr & 0x1FFF)]

SYS = Sys()

class Mem:
    """dict-like memory for py65 with full VT semantics."""
    def __getitem__(self, addr):
        s = SYS
        if addr < 0x2000:
            return s.ram[addr & 0x7FF]
        if addr < 0x4000:
            r = addr & 7
            if r == 2:
                v = 0x80 if s.vblank else 0x00
                s.vblank = False
                return v
            return s.ppu[r]
        if addr == 0x4016 or addr == 0x4017:
            i = addr & 1
            bit = s.shift[i] & 1
            s.shift[i] = (s.shift[i] >> 1) | 0x80   # open-bus 1s after 8 reads
            return bit
        if addr < 0x4100:
            return 0
        if addr < 0x4200:
            return s.reg41[addr & 0xFF]             # experiment: readback
        if addr >= 0x8000:
            return s.prg_read(addr)
        return 0

    def __setitem__(self, addr, val):
        s = SYS
        val &= 0xFF
        if addr < 0x2000:
            s.ram[addr & 0x7FF] = val
            return
        if addr < 0x4000:
            r = addr & 0x3F if (addr & 0x3FF0) == 0x2010 else (addr & 7)
            # VT extended PPU regs $2010-$201F live at their own offsets
            if 0x10 <= (addr & 0xFF) <= 0x1F and (addr & 0x3F00) == 0x2000 >> 0:
                pass
            if (addr & 0xFFF0) == 0x2010:
                idx = addr & 0x3F
                old = s.ppu[idx]
                s.ppu[idx] = val
                if idx == 0x10 and old != val:
                    s.log.append((s.frame, 'W$2010', val))
            else:
                s.ppu[addr & 7] = val
            return
        if addr == 0x4014:                          # OAM DMA
            s.oamdma_src = val << 8
            return
        if addr == 0x4016:
            if (s.strobe & 1) and not (val & 1):    # 1->0 latches
                s.shift[0] = s.pad[0]
                s.shift[1] = s.pad[1]
            s.strobe = val
            return
        if addr < 0x4100:
            return
        if addr < 0x4200:
            r = addr & 0xFF
            s.reg41[r] = val
            if r == 0x01:
                s.timer_preload = val
                s.log.append((s.frame, 'W$4101', val))
            elif r == 0x02:
                s.timer_counter = s.timer_preload
                s.timer_counting = True
                s.timer_enabled = True   # $4102 self-enables on this variant
                s.log.append((s.frame, 'W$4102', val))
            elif r == 0x03:
                s.irq_line = False   # ack; timer keeps free-running
            elif r == 0x04:
                s.timer_enabled = True
                s.log.append((s.frame, 'W$4104', val))
            elif r in (0x07, 0x08):
                s.pq[r - 0x07] = val
            return
        # $8000+ writes: MMC3-forwarded regs on OneBus -- ignore for PRG
        return

class VTMPU(MPU):
    def step(self):
        instructCode = dec56(self.memory[self.pc])
        self.pc = (self.pc + 1) & self.addrMask
        self.excycles = 0
        self.addcycles = self.extracycles[instructCode]
        self.instruct[instructCode](self)
        self.pc &= self.addrMask
        self.processorCycles += self.cycletime[instructCode] + self.excycles
        return self

mem = Mem()
mpu = VTMPU(memory=mem)
# reset
mpu.pc = mem[0xFFFC] | (mem[0xFFFD] << 8)
mpu.p |= mpu.INTERRUPT

CYC_FRAME = 29780
CYC_SCANLINE = 114

def run_frames(n, watch_ram=(0x26, 0x38, 0x87, 0x5CD, 0x555, 0x594)):
    prev = {a: SYS.ram[a] for a in watch_ram}
    for _ in range(n):
        SYS.frame += 1
        target = mpu.processorCycles + CYC_FRAME
        next_line = mpu.processorCycles + CYC_SCANLINE
        while mpu.processorCycles < target:
            mpu.step()
            if mpu.processorCycles >= next_line:
                next_line += CYC_SCANLINE
                if SYS.timer_counting:
                    SYS.timer_counter -= 1
                    if SYS.timer_counter <= 0:
                        SYS.timer_counter = SYS.timer_preload if SYS.timer_preload else 256
                        if SYS.timer_enabled:
                            SYS.irq_line = True
            if SYS.irq_line and not (mpu.p & mpu.INTERRUPT):
                SYS.irq_line = False
                mpu.irq()
        # frame boundary: vblank + NMI
        SYS.vblank = True
        if SYS.ppu[0] & 0x80:
            mpu.nmi()
        for a in watch_ram:
            if SYS.ram[a] != prev[a]:
                v = SYS.ram[a]
                if a not in (0x26,):
                    SYS.log.append((SYS.frame, f'RAM${a:04X}', v))
                prev[a] = v

if __name__ == '__main__':
    plan = sys.argv[1] if len(sys.argv) > 1 else 'noinput'
    if plan == 'noinput':
        run_frames(900)
    elif plan == 'start':
        run_frames(300)
        SYS.pad[0] = 0x08
        run_frames(10)
        SYS.pad[0] = 0
        run_frames(590)
    for e in SYS.log[:200]:
        print(e)
    print('final $2010 =', hex(SYS.ppu[0x10]), 'frames:', SYS.frame,
          'pc:', hex(mpu.pc))
