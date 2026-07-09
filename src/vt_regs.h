/*
 * vt_regs.h -- VT03/VT09 extended register map for PocketVT
 *
 * The VT-series chips augment the 2A03 register space with a bank of
 * "NOAC extension" registers starting at $4100.  This header documents
 * every known register, provides the C-side state struct, and declares
 * the read/write handler entry points called from cart assembly.
 *
 * Register map overview
 * ---------------------
 *  $4100-$411F  VT shared system control (OneBus bank, CPU speed, IRQ ...)
 *  $4120-$412F  VT03 ADPCM channels (two-channel IMA-ADPCM PCM engine)
 *  $4130-$413F  VT03/VT09 ALU helper (16-bit multiplier / divider)
 *  $4140-$417F  VT03 extended palette RAM (64 extra 6-bit entries)
 *  $4180-$41FF  VT03 hi-res sprite OAM extension
 *
 * All addresses are in NES bus space ($4100 range).  The handlers below
 * are called by the writemem_4 hook installed by the VT mapper init.
 */

#ifndef __VT_REGS_H__
#define __VT_REGS_H__

#include "includes.h"
#include "config.h"

#if VT_MODE

// ---------------------------------------------------------------------------
// $4100-$411F  OneBus / system control
// ---------------------------------------------------------------------------
#define VT_REG_BASE         0x4100

// $4100  CPU/PPU clock ratio + outer PRG/CHR bank bits
//   bit 7 = CPU*3 mode (CPU runs at 3x PPU)
//   bit 0 = half-speed PPU/APU flag
#define VT_REG_CLKCTRL      0x00   // offset from VT_REG_BASE

// VT369 timer / IRQ block.
#define VT_REG_TIMER_LO     0x01   // $4101 timer period low
#define VT_REG_TIMER_HI     0x02   // $4102 timer period high
#define VT_REG_TIMER_CTRL   0x03   // $4103 bit0=enable bit1=IRQ enable bit7=ack

// OneBus bank / mirror registers.
#define VT_REG_MIRROR       0x06   // $4106 HV mirroring select
#define VT_REG_PRGBANK0     0x07   // $4107 PQ0
#define VT_REG_PRGBANK1     0x08   // $4108 PQ1
#define VT_REG_PRGBANK2     0x09   // $4109 PQ2
#define VT_REG_PRGBANK3     0x0A   // $410A PQ3 / outer PRG bits
#define VT_REG_BANKCTRL     0x0B   // $410B PS/FWEN/PQ2EN/TSYNEN

// $410F  Security / encryption key register (VT09 only)
//   Writing 0x00 = enable decryption (XOR 0xA1)
//   Writing 0xFF = disable decryption (plain opcodes)
#define VT_REG_SECURITY     0x0F

// Legacy aliases kept so older call sites still compile.
#define VT_REG_CHRBANK0     VT_REG_PRGBANK1
#define VT_REG_CHRBANK1     VT_REG_PRGBANK2

// $411C  System control 2 (prescaler bits etc.)
#define VT_REG_SYSCTRL2     0x1C

// $411F  System control 3 (half-speed PPU/APU clock when bit 0 set)
#define VT_REG_SYSCTRL3     0x1F

// ---------------------------------------------------------------------------
// $4120-$412F  ADPCM channels (VT03 / VT09)
// ---------------------------------------------------------------------------
// Channel 0: $4120-$4127   Channel 1: $4128-$412F
//   +0  start address low
//   +1  start address high
//   +2  volume (0-127)
//   +3  flags: bit6=ADPCM mode  bit0=play/stop
//   +4  period low  (controls playback rate, derived from CPU clock)
//   +5  period high
//   +6  end address low   (playback stops when cur >= end; 0 = run forever)
//   +7  end address high
#define VT_ADPCM_BASE       0x4120
#define VT_ADPCM_CH_STRIDE  0x08
#define VT_ADPCM_ADDR_LO    0x00
#define VT_ADPCM_ADDR_HI    0x01
#define VT_ADPCM_VOLUME     0x02
#define VT_ADPCM_FLAGS      0x03   // bit6=adpcm, bit0=playing
#define VT_ADPCM_PERIOD_LO  0x04
#define VT_ADPCM_PERIOD_HI  0x05
#define VT_ADPCM_END_LO     0x06
#define VT_ADPCM_END_HI     0x07

// ---------------------------------------------------------------------------
// $4130-$413F  Hardware ALU (multiply / divide helper)
// ---------------------------------------------------------------------------
// Write operand A (16-bit) to $4132/$4133 then operand B to $4134/$4135.
// Read 32-bit result from $4136-$4139.  This mirrors the behaviour seen
// in Furbtendulator's APU_VT32 / APU_VT369 aluOperand fields.
#define VT_ALU_BASE         0x4130
#define VT_ALU_OP_A_LO      0x02   // $4132
#define VT_ALU_OP_A_HI      0x03   // $4133
#define VT_ALU_OP_B_LO      0x04   // $4134
#define VT_ALU_OP_B_HI      0x05   // $4135
#define VT_ALU_RESULT_0     0x06   // $4136  result byte 0 (LSB)
#define VT_ALU_RESULT_1     0x07   // $4137
#define VT_ALU_RESULT_2     0x08   // $4138
#define VT_ALU_RESULT_3     0x09   // $4139  result byte 3 (MSB)

// ---------------------------------------------------------------------------
// VT state block (kept in EWRAM alongside other PocketVT globals)
// ---------------------------------------------------------------------------

// Per-channel ADPCM decoder state
typedef struct {
    u32  start_addr;       // ROM address of sample data
    u32  cur_addr;         // Current read pointer
    u32  end_addr;         // Sample end address; playback stops when cur_addr >= end_addr
    u16  period;           // Reload value for playback rate counter
    u16  rate_counter;     // Decrements once per GBA sample; nibble decode triggers at 0
    s16  pcm_out;          // Current output sample (-32768..32767)
    s16  predictor;        // IMA-ADPCM running predictor
    u8   step_index;       // IMA-ADPCM step-size index (0-88)
    u8   step_size;        // Current IMA step magnitude
    u8   volume;           // 0-127
    u8   flags;            // mirrors VT_ADPCM_FLAGS
    bool playing;
    bool second_nibble;    // toggles between high/low nibble of each byte
} VTAdpcmChan;

typedef struct {
    // System control shadow registers ($4100-$411F).
    // MUST be exactly 0x20 bytes -- 6502_vt.s reads encryption_active
    // through encryption_next via hardcoded offsets 0x20, 0x21, 0x22.
    // A _Static_assert in vt_regs.c locks these offsets down.
    u8   reg[0x20];

    // VT09 encryption state -- MUST stay at offsets 0x20-0x22 in this order.
    // See vt_state_enc_{active,pending,next} in 6502_vt.s.
    bool encryption_active;       // 1 = unscramble opcodes at fetch time
    bool encryption_pending;      // (VT09 $410F) Change deferred until next JMP
    bool encryption_next;         // (VT09 $410F) Value encryption_active will become

    // Encryption family selector -- chooses which bit permutation to apply
    // in vt09_decode_opcode().  Mirrors Furb CPU_OneBus::Unscramble switch.
    //   0  = no encryption (or pre-NES2.0 cart)
    //   12 = sub-mapper 12: bit-swap 6<->7 and 1<->2
    //   13 = sub-mapper 13: bit-swap 1<->4
    //   14 = sub-mapper 14: bit-swap 6<->7
    //   15 = sub-mapper 15 (default for >=12 else branch): bit-swap 5<->6
    // Position is AFTER encryption_next so the 0x20-0x22 ABI is preserved.
    u8   encryption_mode;

    // ALU helper
    u32  alu_operand_a;
    u32  alu_operand_b;
    u32  alu_result;

    // ADPCM channels (VT_ADPCM_SOUND)
    VTAdpcmChan adpcm[2];

    // VT369 timer / IRQ
    u16  timer_period;
    u16  timer_counter;
    u8   timer_ctrl;      // bit0=enable bit1=IRQ enable bit7=ack-on-write
    u8   want_timer_irq;

    // NES 2.0 mapper-256 submapper number (0-15).
    u8   submapper;
} VTState;

// Global VT state instance (defined in vt_regs.c)
extern VTState vt;

// Runtime flag.  Set by mapVTinit so the PPU $2008+ divert in ppu.s only
// fires for VT cartridges, leaving the standard NES register-mirror path
// intact for non-VT mappers.
extern u8 vt_active;

// CHR bank shadow registers ($2012-$2017).
extern u8 vt_chr_reg[6];

// Extra CHR/PPU bank registers (VT02+ Registers wiki).
extern u8 vt_chr_reg_2018;
extern u8 vt_chr_reg_201A;
extern u8 vt_chr_outer_4100;

// Communication channel for PRG banks computed in C to be applied safely in ASM
extern u8 vt_prg_banks[4];
extern u8 vt_prg_dirty;

// ---------------------------------------------------------------------------
// Handler declarations -- called from the writemem_4 hook in mapVT.s
// ---------------------------------------------------------------------------

// Called on any write to $4100-$41FF
void vt_reg_write(u8 addr_lo, u8 val);

// Called on any read from $4100-$41FF
u8   vt_reg_read(u8 addr_lo);

// Power-on / reset state for the VT block
void vt_reset(void);

// Rebuild the 256-entry 6502 op_table so the dispatch result for a fetched
// (possibly encrypted) byte lands on the *intended* handler.
void vt_rebuild_optable(void);

// Recompute the physical PRG banks based on current register states.
// Safe C-side computation only; sets vt_prg_dirty = 1.
void vt_recompute_prg_banks(void);

// Run one APU-clock worth of ADPCM mixing into the given 16-bit sample buffer
void vt_adpcm_tick(s16 *buf, int samples);

// Called from sound.s timer1interrupt to mix ADPCM into the GBA
// DirectSound PCMWAV buffer (128 signed-8-bit samples at 0x05000280).
#if VT_ADPCM_SOUND
void vt_adpcm_mix_gba(void);
#endif

// Apply / remove VT09 encryption; called at JMP/JMPI instruction dispatch
void vt09_set_encryption(bool enable);

// Returns current decrypted opcode (or raw opcode if encryption is off).
u8   vt09_decode_opcode(u8 raw);

#endif // VT_MODE
#endif // __VT_REGS_H__