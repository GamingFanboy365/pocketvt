#include "includes.h"
#if VT_MODE
#include "vt_regs.h"
#endif

extern u16 SPEEDHACK_TEMP_BUF[48];
extern u16 SPEEDHACK_INCS[64];
extern speedhack_T speedhacks[4];

//extern u8* memmap_tbl[8];  //already defined in asmcalls.h
extern int speedhack_num_incs;
extern u32 speedhack_divider;
extern int speedhack_cycles;

#define LOOKBACK 18

#if VT_MODE
/* s.86: the opcode a PRG byte runs (encrypted on VT submappers 12-15). */
extern u8 vt_op_dec[256];
#define OP(b) (vt_active ? vt_op_dec[(u8)(b)] : (u8)(b))
#else
#define OP(b) ((u8)(b))
#endif

static __inline u8 ins_table(int addr);
static __inline bool forward_branch_is_jump_back(const u8 *pc, int branchlength, int initpc_16, const u8 *lastbank);
static __inline bool forward_branch_is_jump_back_2(const u8 *pc, int branchlength, int start_16);
static __inline int gets8(const u8 *p, int i);
static const u8 *find_first_instruction(const u8 *initpc, const u8 *lastbank, const u8 **branchpc);
static const u8 *find_hack(const u8 *start_pc, const u8 *branchpc, const u8 *lastbank, int hacknum);

const u8 capcom_speedhack[]=
{
	0xA2,0x00,0x86,0x90,0xA0,0x04,0xB5,0x80,0xC9,0x04,0xB0,0x0A,0xE8,0xE8,0xE8,0xE8,0x88,0xD0,0xF3
};

//02 = X, 22 = Y
#define _XXX 0x02
#define _YYY 0x22

const u8 konami_speedhack_0[]={ 0xE6,_XXX,0x18,0xA5,_XXX,0x65,_YYY,0x85,_XXX };  //A+=N*(B+1)   mostgames
const u8 konami_speedhack_1[]={ 0xA5,_XXX,0x18,0x65,_YYY,0x85,_YYY }; //B+=A*N                  lifeforce
const u8 konami_speedhack_2[]={ 0xA5,_XXX,0x18,0x65,_YYY,0x85,_XXX }; //A+=B*N                   cv2
const u8 konami_speedhack_3[]={ 0xA5,_XXX,0x38,0x65,_YYY,0x85,_XXX }; //A+=(B+1)*N              jarin ko chie
const u8 konami_speedhack_4[]={ 0xA5,_XXX,0x38,0x65,_YYY,0x85,_YYY }; //B+=(A+1)*N              goonies2
const u8 konami_speedhack_5[]={ 0xA5,_XXX,0x65,_YYY,0x85,_XXX,0xE6,_XXX }; //A+=B+carry , A++   ddribble
const u8 konami_speedhack_6[]={ 0xE6,_XXX,0xA5,_XXX,0x65,_YYY,0x85,_XXX }; //A++ , A+=B+carry   superc
const u8 konami_speedhack_7[]={ 0xA5,_XXX,0x65,_YYY,0x85,_YYY }; //B+=A+carry                   contra
const u8 konami_speedhack_8[]={ 0xA5,_XXX,0x65,_YYY,0x85,_XXX }; //A+=B+carry                   gradius2

const u8 konami_speedhack_sizes[]={
	sizeof(konami_speedhack_0), //9
	sizeof(konami_speedhack_1), //7
	sizeof(konami_speedhack_2), //7
	sizeof(konami_speedhack_3), //7
	sizeof(konami_speedhack_4), //7
	sizeof(konami_speedhack_5), //8
	sizeof(konami_speedhack_6), //8
	sizeof(konami_speedhack_7), //6
	sizeof(konami_speedhack_8)  //6
};

const u8 *const konami_speedhacks[]={
	konami_speedhack_0,
	konami_speedhack_1,
	konami_speedhack_2,
	konami_speedhack_3,
	konami_speedhack_4,
	konami_speedhack_5,
	konami_speedhack_6,
	konami_speedhack_7,
	konami_speedhack_8
};

const u8 konami_speedhack_cycles[]={
	19, //mostgames
	14, //lifeforce
	14, //cv2
	14, //jarin ko chie
	14, //goonies2
	17, //ddribble
	17, //superc
	12, //contra
	12  //gradius2
};


const u8 quickhackfinder_ins_table[]=
{
	//01 = zp
	//02 = imm
	//03 = abs
	//04 = branch
	//05 = jump
	//06 = math on A
	//0F = reject
	//00 = length 0
	//10 = length 1
	//20 = length 2
	//30 = length 3
	//00 = is a read
	//40 = okay to do math on A after loading this
	//80 = is a write
	//C0 = is an increment
/*  *///impl math shft ???? x/y  math shft ???? impl imme shft ???? x/y  math shft ???? brnc ???? shft ???? x/y  math shft ???? impl math shft ???? x/y  math shft ????
/*  *///  00   01   02   03   04   05   06   07   08   09   0A   0B   0C   0D   0E   0F   10   11   12   13   14   15   16   17   18   19   1A   1B   1C   1D   1E   1F
/*00*/	0xFF,0xFF,0xFF,0xFF,0xFF,0x21,0xFF,0xFF,0xFF,0x22,0x16,0xFF,0xFF,0x33,0xFF,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //ORA,ASL
/*20*/	0xFF,0xFF,0xFF,0xFF,0x21,0x21,0xFF,0xFF,0xFF,0x22,0xFF,0xFF,0x33,0x33,0xFF,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //AND,ROL
/*40*/	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x35,0xFF,0xFF,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //EOR,LSR
/*60*/	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //ADC,ROR
/*80*/	0xFF,0xFF,0xFF,0xFF,0xFF,0xA1,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xB3,0xFF,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //STA,STX
/*A0*/	0xFF,0xFF,0xFF,0xFF,0x21,0x61,0x21,0xFF,0x10,0x62,0xFF,0xFF,0x33,0x73,0x33,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //LDA,LDX
/*C0*/	0xFF,0xFF,0xFF,0xFF,0x21,0x21,0xFF,0xFF,0xFF,0x22,0xFF,0xFF,0x33,0x33,0xFF,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  //CMP,DEC
/*E0*/	0xFF,0xFF,0xFF,0xFF,0x21,0xFF,0xE1,0xFF,0xFF,0xFF,0x10,0xFF,0x33,0xFF,0xF3,0xFF,0x24,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF   //SBC,INC
};

static __inline u8 ins_table(int addr)
{
	return quickhackfinder_ins_table[addr];
}

static __inline bool forward_branch_is_jump_back(const u8 *pc, int branchlength, int initpc_16, const u8 *lastbank)
{
	const u8* branch_dest=pc+2+branchlength;
	if ((ins_table(OP(*branch_dest))&0x0F)==5)
	{
		int dest_addr=branch_dest[1]+branch_dest[2]*256;
		if (dest_addr<=initpc_16 && dest_addr>=initpc_16-LOOKBACK)
		{
			return true;
		}
	}
	return false;
}

static __inline bool forward_branch_is_jump_back_2(const u8 *pc, int branchlength, int start_16)
{
	const u8* branch_dest=pc+2+branchlength;
	if ((ins_table(OP(*branch_dest))&0x0F)==5)
	{
		int dest_addr=branch_dest[1]+branch_dest[2]*256;
		if (dest_addr==start_16)
		{
			return true;
		}
	}
	return false;
}

static __inline int gets8(const u8 *p, int i)
{
	return ((const s8*)p)[i];
}

static const u8 *find_first_instruction(const u8 *initpc, const u8 *lastbank, const u8 **branchpc)
{
	const u8 *pc_limit=initpc+16;
	const u8 *pc=initpc;
	int init_pc_16=initpc-lastbank;
	while (pc < pc_limit)
	{
		u8 ins=ins_table(OP(*pc));
		int len=(ins>>4)&3;
		int action=ins&0x0F;
		if (action==0x0F) return NULL;
		if (action==4)
		{
			int branchlength=gets8(pc,1);
			if (branchlength<-LOOKBACK) return NULL;
			if (branchlength<=-2)
			{
				*branchpc=pc;
				return pc+2+branchlength;
			}
			//check if forward branch goes to a JMP before this PC
			if (forward_branch_is_jump_back(pc,branchlength,init_pc_16,lastbank))
			{
				pc=pc+2+branchlength;
				continue;
			}
		}
		else if (action==5)
		{
			int dest_addr=pc[1]+pc[2]*256;
			//int my_pc=pc-lastbank;
			if (dest_addr>init_pc_16) return NULL;
			if (dest_addr<init_pc_16-LOOKBACK) return NULL;
			*branchpc=pc;
			return memmap_tbl[dest_addr/(PRG_BANK_SIZE*1024)]+dest_addr;
		}
		pc+=len;
	}
	return NULL;
}

static const u8 *find_hack(const u8 *start_pc, const u8 *branchpc, const u8 *lastbank, int hacknum)
{
	int start_16=start_pc-lastbank;
	const int MAX_READS=16;
	const int MAX_WRITES=16;
	const int MAX_INCS=16;
	
	u16 *const incs= &SPEEDHACK_TEMP_BUF[0];
	u16 *const reads= &SPEEDHACK_TEMP_BUF[16];
	u16 *const writes= &SPEEDHACK_TEMP_BUF[32];
	
	int num_reads=0;
	int num_writes=0;
	int num_incs=0;
	int addr;

	int total_cycles=0;

	int math_a_okay=0;
	int last_instruction_was_increment=0;
	
	const u8 *pc=start_pc;
	if (start_pc==NULL) return NULL;
	while (pc<branchpc)
	{
		//check if an instruction disqualifies
		u8 ins=ins_table(OP(*pc));
		int len=(ins>>4)&3;
		int action=ins&0x0F;
		int iswrite=(ins>>6);
		addr=-1;
		switch (action)
		{
		case 6:  //"math on A", for now just ASL A
			if (!math_a_okay) return NULL;
		case 0:  //implied addressing mode
		case 2:  //immediate addressing mode
			total_cycles+=2;
			break;
		case 1:  //zeropage addressing mode
			addr=pc[1];
			total_cycles+=3;
			break;
		case 3:  //absolute addressing mode
			addr=pc[1]+pc[2]*256;
			total_cycles+=4;
			break;
		case 4:  //branch
			total_cycles+=2;
			{
				int branchlength;
				if (last_instruction_was_increment)
				{
					return NULL;
				}
				branchlength=gets8(pc,1);
				if (forward_branch_is_jump_back_2(pc,gets8(pc,1),start_16))
				{
					total_cycles+=1;
					goto out_loop;
				}
				if (branchlength<0) return NULL;  //only the last branch can be a jump back
			}
			break;
		case 5:	//reject unconditional jumps except at end
		case 0x0F: //reject
		default:
			return NULL;
		}
		if (addr!=-1)
		{
			if (iswrite<2)  //is a read, or "Math on A is okay"
			{
				last_instruction_was_increment = 0;
				if (num_reads==MAX_READS) return NULL;
				reads[num_reads++]=addr;
			}
			else  //is a write or incrmenet
			{
				if ((addr>=0x2000 && addr<0x6000) || (addr>=0x8000)) return NULL;
				if (num_writes==MAX_WRITES) return NULL;
				writes[num_writes++]=addr;
			}
			if (iswrite==1)  //LDA zpg, imm, or abs
			{
				math_a_okay=1;
			}

			if (iswrite==3)  //is an increment
			{
				last_instruction_was_increment = 1;
				total_cycles+=2;
				if (num_incs==MAX_INCS) return NULL;
				incs[num_incs++]=addr;
			}
		}
		pc+=len;
	}
out_loop:
	if (pc!=branchpc) return NULL;
	
	if ((OP(*pc) & 0x1F)==0x10) //if it's a branch
	{
		if (last_instruction_was_increment)
		{
			return NULL;
		}
		int end_16=(pc-lastbank)+2;
		total_cycles+=3;
		if ((start_16 & 0xFF00) != (end_16 & 0xFF00))
		{
			total_cycles+=1;
		}
	}
	else	//it's a jump
	{
		total_cycles+=3;
	}
	
	
	//check read and write list for collisions
	if (num_reads!=0 && num_writes!=0)
	{
		int i,j;
		int a,b,lasta,lastb;
		lasta=-1;
		for (i=0;i<num_reads;i++)
		{
			a=reads[i];
			if (lasta==a) continue;
			lasta=a;
			lastb=-1;
			for (j=0;j<num_writes;j++)
			{
				b=writes[j];
				if (lastb==b) continue;
				lastb=b;
				if (a==b) return NULL;
			}
		}
	}
	{
		speedhack_T *hack=&speedhacks[hacknum];
		hack->hack_pc=branchpc;
		hack->num_incs=num_incs;
#if VT_MODE
		total_cycles*=vt_cpu_x3?1:3;	//s.88: dots per cycle, 1 at VT369 CPU x3
#else
		total_cycles*=3;
#endif
		hack->cycles_per_iteration=total_cycles;
		hack->divider=(u32)((u64)0x100000000LL/(u64)total_cycles)+1;
	}
	memcpy32(&SPEEDHACK_INCS[hacknum*16],&incs[0],16*sizeof(u16));
	return branchpc;
}

#if VT_MODE
/* s.88: a poll loop followed along the path the current RAM values take:
 * LDA zp/abs (RAM only), AND/CMP #imm, conditional branches and JMP abs, back
 * to the starting PC with no writes.  Sky Fighter's title waits in four
 * LDA $30 / AND #m / BEQ tests joined by forward branches, which
 * find_first_instruction cannot follow; at CPU x3 it spun ~900K cycles a
 * frame (13 NES fps).  The hack goes on the loop's backward branch or JMP. */
extern u32 vt_nes_ram_mask;
static bool find_poll_loop(const u8 *initpc, const u8 *lastbank, int hacknum)
{
	const u8 *ram=(const u8 *)NES_RAM;
	const u8 *pc=initpc;
	const u8 *back=NULL;
	const int bank=(initpc-lastbank)>>13;
	int a=-1, z=-1, n=-1, c=-1, total=0, steps;
	/* s.89: the vblank can land on the AND/CMP or the branch after the load,
	 * where A and the flags are unknown.  Back up to the load that feeds it
	 * (a straight run of LDA/AND/CMP ending at initpc) and close the loop
	 * there.  Before, only a vblank on the LDA found Sky Fighter's loop, so
	 * a timing change left its title at 14 fps. */
	if (OP(*initpc)!=0xA5 && OP(*initpc)!=0xAD)
	{
		int k;
		for (k=2;k<=7;k++)
		{
			const u8 *s=initpc-k, *q=s;
			if (OP(*s)!=0xA5 && OP(*s)!=0xAD) continue;
			while (q<initpc)
			{
				const u8 o=OP(*q);
				if (o==0xA5 || o==0x29 || o==0xC9) q+=2;
				else if (o==0xAD) q+=3;
				else break;
			}
			if (q==initpc) { initpc=s; pc=s; break; }
		}
	}
	for (steps=0; steps<48; steps++)
	{
		const u8 op=OP(*pc);
		int addr=-1, cyc=0, len=0, flag=-1;
		switch (op)
		{
		case 0xA5: addr=pc[1]; cyc=3; len=2; break;
		case 0xAD: addr=pc[1]|pc[2]<<8; if (addr>=0x2000) return false; cyc=4; len=3; break;
		case 0x29: if (a<0) return false; a&=pc[1]; z=a==0; n=a>>7; cyc=2; len=2; break;
		case 0xC9: if (a<0) return false; c=a>=pc[1]; z=a==pc[1]; n=((a-pc[1])>>7)&1; cyc=2; len=2; break;
		case 0xF0: flag=z; break;
		case 0xD0: flag=z<0?-1:!z; break;
		case 0x30: flag=n; break;
		case 0x10: flag=n<0?-1:!n; break;
		case 0xB0: flag=c; break;
		case 0x90: flag=c<0?-1:!c; break;
		case 0x4C:
		{
			int dest=pc[1]|pc[2]<<8;
			if (dest<0x8000 || (dest>>13)!=bank) return false;
			back=pc; pc=lastbank+dest; total+=3;
			goto next;
		}
		default: return false;
		}
		if (addr>=0)
		{
			a=ram[addr&vt_nes_ram_mask]; z=a==0; n=a>>7;
		}
		if ((op&0x1F)==0x10)
		{
			if (flag<0) return false;
			if (flag)
			{
				const int rel=gets8(pc,1);
				const u8 *dest=pc+2+rel;
				if (((dest-lastbank)>>13)!=bank) return false;
				total+=3+((((pc-lastbank)+2)^(dest-lastbank))>>8&1);
				if (rel<0) back=pc;
				pc=dest;
				goto next;
			}
			cyc=2; len=2;
		}
		total+=cyc; pc+=len;
	next:
		if (pc==initpc)
		{
			speedhack_T *hack=&speedhacks[hacknum];
			int i;
			if (!back || total<=0) return false;
			hack->hack_pc=back;
			hack->num_incs=0;
			total*=vt_cpu_x3?1:3;
			hack->cycles_per_iteration=total;
			hack->divider=0xFFFFFFFFu/(u32)total+1;	//32-bit: a 64-bit divide went deep into the IWRAM stack
			for (i=0;i<16;i++) SPEEDHACK_INCS[hacknum*16+i]=0;
			return true;
		}
	}
	return false;
}
#endif

#if VT_MODE
/* s.92: idle loops that call a subroutine and read the joypads.  Aero
 * Gyrodine and Hex City X wait on their titles in JSR readpad / LDA buttons /
 * AND #START / BEQ: the routine strobes $4016 and shifts 16 bits out of
 * $4016/$4017 into RAM, ~150 times a NES frame, and emulating that cost the
 * titles 40% of their speed (41-43 NES fps).  find_poll_loop handles loads,
 * compares and branches only.  This runs one turn of the loop through a small
 * 6502 simulator from the live state (registers, RAM, the pads' shift
 * registers), following JSR/RTS, then a second turn from the end of the
 * first.  If the second turn leaves registers, flags, every RAM byte written
 * and the pad state exactly as the first did, the loop is waiting (until an
 * interrupt or a button changes something), and the hack goes on its
 * outermost backward branch or JMP, charged one turn's cycles.  Anything the
 * simulator does not know (other I/O, indirect jumps, stack tricks) gives
 * up.  All state is in EWRAM: this runs on the vblank handler's IWRAM stack. */
extern u8 _joy0state, _joy1state, _joy2state, _joy3state, _nrplayers;
extern u8 *_m6502_s;
enum { IS_IMP = 1, IS_IMM, IS_ZP, IS_ZPX, IS_ZPY, IS_ABS, IS_ABX, IS_ABY, IS_IZY, IS_REL, IS_JMP, IS_JSR, IS_RTS };
#define IS_MAXW 48
typedef struct {
	int a, x, y, s, n, z, c, v;
	u32 ser0, ser1;
	int strobe;
	int nw;
	u16 wa[IS_MAXW];
	u8 wv[IS_MAXW];
} is_state;
typedef struct {
	is_state st, first;
	int pc, depth, cycles, ok, tookback, budget;
} is_sim;
EWRAM_BSS is_sim vt_isim;
EWRAM_BSS u32 vt_isim_regs[3];   /* A, X, Y << 24 at the call (speedhack_asm.s) */

static const u8 is_mode[256] = {
	[0xAA]=IS_IMP,[0x8A]=IS_IMP,[0xA8]=IS_IMP,[0x98]=IS_IMP,[0xE8]=IS_IMP,[0xCA]=IS_IMP,[0xC8]=IS_IMP,[0x88]=IS_IMP,
	[0x18]=IS_IMP,[0x38]=IS_IMP,[0xB8]=IS_IMP,[0xEA]=IS_IMP,[0x0A]=IS_IMP,[0x4A]=IS_IMP,[0x2A]=IS_IMP,[0x6A]=IS_IMP,
	[0x48]=IS_IMP,[0x68]=IS_IMP,
	[0xA9]=IS_IMM,[0xA2]=IS_IMM,[0xA0]=IS_IMM,[0x29]=IS_IMM,[0x09]=IS_IMM,[0x49]=IS_IMM,[0xC9]=IS_IMM,[0xE0]=IS_IMM,[0xC0]=IS_IMM,[0x69]=IS_IMM,[0xE9]=IS_IMM,
	[0xA5]=IS_ZP,[0xA6]=IS_ZP,[0xA4]=IS_ZP,[0x85]=IS_ZP,[0x86]=IS_ZP,[0x84]=IS_ZP,[0x25]=IS_ZP,[0x05]=IS_ZP,[0x45]=IS_ZP,[0xC5]=IS_ZP,
	[0xE4]=IS_ZP,[0xC4]=IS_ZP,[0x65]=IS_ZP,[0xE5]=IS_ZP,[0x24]=IS_ZP,[0x06]=IS_ZP,[0x46]=IS_ZP,[0x26]=IS_ZP,[0x66]=IS_ZP,[0xE6]=IS_ZP,[0xC6]=IS_ZP,
	[0xB5]=IS_ZPX,[0x95]=IS_ZPX,[0xB4]=IS_ZPX,[0x94]=IS_ZPX,[0x35]=IS_ZPX,[0x15]=IS_ZPX,[0x55]=IS_ZPX,[0xD5]=IS_ZPX,[0x75]=IS_ZPX,[0xF5]=IS_ZPX,
	[0x16]=IS_ZPX,[0x56]=IS_ZPX,[0x36]=IS_ZPX,[0x76]=IS_ZPX,[0xF6]=IS_ZPX,[0xD6]=IS_ZPX,
	[0xB6]=IS_ZPY,[0x96]=IS_ZPY,
	[0xAD]=IS_ABS,[0xAE]=IS_ABS,[0xAC]=IS_ABS,[0x8D]=IS_ABS,[0x8E]=IS_ABS,[0x8C]=IS_ABS,[0x2D]=IS_ABS,[0x0D]=IS_ABS,[0x4D]=IS_ABS,[0xCD]=IS_ABS,
	[0xEC]=IS_ABS,[0xCC]=IS_ABS,[0x6D]=IS_ABS,[0xED]=IS_ABS,[0x2C]=IS_ABS,[0x0E]=IS_ABS,[0x4E]=IS_ABS,[0x2E]=IS_ABS,[0x6E]=IS_ABS,[0xEE]=IS_ABS,[0xCE]=IS_ABS,
	[0xBD]=IS_ABX,[0xBC]=IS_ABX,[0x9D]=IS_ABX,[0x3D]=IS_ABX,[0x1D]=IS_ABX,[0x5D]=IS_ABX,[0xDD]=IS_ABX,[0x7D]=IS_ABX,[0xFD]=IS_ABX,
	[0x1E]=IS_ABX,[0x5E]=IS_ABX,[0x3E]=IS_ABX,[0x7E]=IS_ABX,[0xFE]=IS_ABX,[0xDE]=IS_ABX,
	[0xB9]=IS_ABY,[0xBE]=IS_ABY,[0x99]=IS_ABY,[0x39]=IS_ABY,[0x19]=IS_ABY,[0x59]=IS_ABY,[0xD9]=IS_ABY,[0x79]=IS_ABY,[0xF9]=IS_ABY,
	[0xB1]=IS_IZY,[0x91]=IS_IZY,[0x31]=IS_IZY,[0x11]=IS_IZY,[0x51]=IS_IZY,[0xD1]=IS_IZY,[0x71]=IS_IZY,[0xF1]=IS_IZY,
	[0x10]=IS_REL,[0x30]=IS_REL,[0x50]=IS_REL,[0x70]=IS_REL,[0x90]=IS_REL,[0xB0]=IS_REL,[0xD0]=IS_REL,[0xF0]=IS_REL,
	[0x4C]=IS_JMP,[0x20]=IS_JSR,[0x60]=IS_RTS,
};
/* base cycles (page crossings added below) */
static const u8 is_cyc[14] = { 0, 2, 2, 3, 4, 4, 4, 4, 4, 5, 2, 3, 6, 6 };

static int is_wfind(int a)
{
	is_state *t = &vt_isim.st;
	for (int i = 0; i < t->nw; i++) if (t->wa[i] == a) return i;
	return -1;
}
static int is_read(int a)
{
	is_state *t = &vt_isim.st;
	a &= 0xFFFF;
	if (a < 0x2000) {
		a &= vt_nes_ram_mask;
		int i = is_wfind(a);
		return i >= 0 ? t->wv[i] : ((const u8 *)NES_RAM)[a];
	}
	if (a == 0x4016 || a == 0x4017) {          /* io.s joy0_R / joy1_R */
		u32 *ser = a == 0x4016 ? &t->ser0 : &t->ser1;
		int v = *ser & 1;
		if (!t->strobe) *ser = (u32)((s32)*ser >> 1);
		return a == 0x4016 ? v | 0x40 : v;
	}
	if (a >= 0x8000) return memmap_tbl[a >> 13][a];
	vt_isim.ok = 0;
	return 0;
}
static void is_write(int a, int v)
{
	is_state *t = &vt_isim.st;
	a &= 0xFFFF;
	if (a < 0x2000) {
		a &= vt_nes_ram_mask;
		int i = is_wfind(a);
		if (i < 0) {
			if (t->nw >= IS_MAXW) { vt_isim.ok = 0; return; }
			i = t->nw++;
			t->wa[i] = (u16)a;
		}
		t->wv[i] = (u8)v;
		return;
	}
	if (a == 0x4016) {                        /* io.s joy0_W */
		t->strobe = v & 1;
		if (t->strobe) {
			const int four = _nrplayers >= 3;
			t->ser0 = _joy0state | _joy2state << 8 | (four ? 0x00080000u : 0xFFFFFF00u);
			t->ser1 = _joy1state | _joy3state << 8 | (four ? 0x00040000u : 0xFFFFFF00u);
		}
		return;
	}
	vt_isim.ok = 0;
}
static void is_nz(int v) { vt_isim.st.n = (v >> 7) & 1; vt_isim.st.z = (v & 0xFF) == 0; }
static int is_rmw(int op, int m)
{
	is_state *t = &vt_isim.st;
	if (((op & 0xE0) == 0x20 || (op & 0xE0) == 0x60) && t->c < 0) { vt_isim.ok = 0; return 0; }   /* ROL/ROR need C */
	switch (op & 0xE0) {
	case 0x00: t->c = m >> 7; m = (m << 1) & 0xFF; break;                 /* ASL */
	case 0x20: { int c = t->c; t->c = m >> 7; m = ((m << 1) | c) & 0xFF; break; }  /* ROL */
	case 0x40: t->c = m & 1; m >>= 1; break;                              /* LSR */
	case 0x60: { int c = t->c; t->c = m & 1; m = (m >> 1) | c << 7; break; }        /* ROR */
	case 0xC0: m = (m - 1) & 0xFF; break;                                 /* DEC */
	default:   m = (m + 1) & 0xFF; break;                                 /* INC */
	}
	is_nz(m);
	return m;
}
static void is_adc(int m)
{
	is_state *t = &vt_isim.st;
	if (t->c < 0) { vt_isim.ok = 0; return; }
	int r = t->a + m + t->c;
	t->v = (~(t->a ^ m) & (t->a ^ r) & 0x80) != 0;
	t->c = r > 0xFF;
	t->a = r & 0xFF;
	is_nz(t->a);
}
static void is_cmp(int r, int m) { vt_isim.st.c = r >= m; is_nz(r - m); }

/* one instruction at vt_isim.pc; 0 = stop (unknown or bad) */
static int is_step(void)
{
	is_sim *S = &vt_isim;
	is_state *t = &S->st;
	const int pc = S->pc;
	S->tookback = 0;
	if (pc < 0x8000 || --S->budget < 0) return 0;
	const u8 *p = memmap_tbl[pc >> 13] + pc;
	const int op = OP(p[0]), mode = is_mode[op];
	if (!mode) return 0;
	int ea = 0, len = 1, cyc = is_cyc[mode];
	switch (mode) {
	case IS_IMM: len = 2; break;
	case IS_ZP:  ea = p[1]; len = 2; break;
	case IS_ZPX: ea = (p[1] + t->x) & 0xFF; len = 2; break;
	case IS_ZPY: ea = (p[1] + t->y) & 0xFF; len = 2; break;
	case IS_ABS: ea = p[1] | p[2] << 8; len = 3; break;
	case IS_ABX: case IS_ABY: {
		const int base = p[1] | p[2] << 8;
		ea = (base + (mode == IS_ABX ? t->x : t->y)) & 0xFFFF; len = 3;
		if ((base ^ ea) & 0x100) cyc++;
		break; }
	case IS_IZY: {
		const int base = is_read(p[1]) | is_read((p[1] + 1) & 0xFF) << 8;
		ea = (base + t->y) & 0xFFFF; len = 2;
		if ((base ^ ea) & 0x100) cyc++;
		break; }
	case IS_REL: len = 2; break;
	case IS_JMP: case IS_JSR: len = 3; break;
	}
	const int st = (op & 0xE0) == 0x80 && mode != IS_IMM && mode != IS_IMP && mode != IS_REL;   /* STA/STX/STY */
	const int rmw = (op & 0x07) == 0x06 && (op & 0xE0) != 0x80 && (op & 0xE0) != 0xA0 && mode != IS_IMP;
	if (st || rmw) { if (mode == IS_ABX || mode == IS_ABY || mode == IS_IZY) cyc = mode == IS_IZY ? 6 : 5; }
	if (rmw) cyc += 2;
	int m = 0;
	if (mode == IS_IMM) m = p[1];
	else if (mode != IS_IMP && mode != IS_REL && mode != IS_JMP && mode != IS_JSR && mode != IS_RTS && !st && !(rmw)) m = is_read(ea);
	S->pc = (pc + len) & 0xFFFF;
	switch (op) {
	/* implied */
	case 0xAA: t->x = t->a; is_nz(t->x); break;
	case 0x8A: t->a = t->x; is_nz(t->a); break;
	case 0xA8: t->y = t->a; is_nz(t->y); break;
	case 0x98: t->a = t->y; is_nz(t->a); break;
	case 0xE8: t->x = (t->x + 1) & 0xFF; is_nz(t->x); break;
	case 0xCA: t->x = (t->x - 1) & 0xFF; is_nz(t->x); break;
	case 0xC8: t->y = (t->y + 1) & 0xFF; is_nz(t->y); break;
	case 0x88: t->y = (t->y - 1) & 0xFF; is_nz(t->y); break;
	case 0x18: t->c = 0; break;
	case 0x38: t->c = 1; break;
	case 0xB8: t->v = 0; break;
	case 0xEA: break;
	case 0x0A: case 0x2A: case 0x4A: case 0x6A: t->a = is_rmw(op, t->a); break;
	case 0x48: is_write(0x100 | t->s, t->a); t->s = (t->s - 1) & 0xFF; cyc = 3; break;
	case 0x68: t->s = (t->s + 1) & 0xFF; t->a = is_read(0x100 | t->s); is_nz(t->a); cyc = 4; break;
	/* loads, logic, arithmetic, compares */
	case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xB1: t->a = m; is_nz(m); break;
	case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE: t->x = m; is_nz(m); break;
	case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC: t->y = m; is_nz(m); break;
	case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x31: t->a &= m; is_nz(t->a); break;
	case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x11: t->a |= m; is_nz(t->a); break;
	case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x51: t->a ^= m; is_nz(t->a); break;
	case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x71: is_adc(m); break;
	case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xF1: is_adc(m ^ 0xFF); break;
	case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xD1: is_cmp(t->a, m); break;
	case 0xE0: case 0xE4: case 0xEC: is_cmp(t->x, m); break;
	case 0xC0: case 0xC4: case 0xCC: is_cmp(t->y, m); break;
	case 0x24: case 0x2C: t->z = (t->a & m) == 0; t->n = m >> 7; t->v = (m >> 6) & 1; break;
	/* stores */
	case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x91: is_write(ea, t->a); break;
	case 0x86: case 0x96: case 0x8E: is_write(ea, t->x); break;
	case 0x84: case 0x94: case 0x8C: is_write(ea, t->y); break;
	/* control */
	case 0x4C: {
		const int dest = p[1] | p[2] << 8;
		if (dest <= pc) S->tookback = 1;
		S->pc = dest; break; }
	case 0x20: {
		const int ret = (pc + 2) & 0xFFFF;
		is_write(0x100 | t->s, ret >> 8); t->s = (t->s - 1) & 0xFF;
		is_write(0x100 | t->s, ret & 0xFF); t->s = (t->s - 1) & 0xFF;
		S->pc = p[1] | p[2] << 8; S->depth++; break; }
	case 0x60: {
		t->s = (t->s + 1) & 0xFF; int lo = is_read(0x100 | t->s);
		t->s = (t->s + 1) & 0xFF; int hi = is_read(0x100 | t->s);
		S->pc = ((lo | hi << 8) + 1) & 0xFFFF;
		S->depth--;
		break; }
	default:
		if (mode == IS_REL) {
			int flag;
			switch (op >> 6) {
			case 0: flag = t->n; break;
			case 1: flag = t->v; break;
			case 2: flag = t->c; break;
			default: flag = t->z; break;
			}
			if (flag < 0) return 0;
			if (!(op & 0x20)) flag = !flag;
			if (flag) {
				const int dest = (S->pc + (s8)p[1]) & 0xFFFF;
				cyc += 1 + (((S->pc ^ dest) >> 8) & 1);
				if (dest <= pc) S->tookback = 1;
				S->pc = dest;
			}
		} else if (rmw) {
			is_write(ea, is_rmw(op, is_read(ea)));
		} else return 0;
		break;
	}
	S->cycles += cyc;
	return S->ok;
}

/* One turn of the loop whose head is pc 'head' at call depth 'level', from
 * the current state: run until the simulation is back at the head at that
 * depth.  The turn must leave that depth only downwards (calls) and come back
 * through the same backward jump 'back'.  Returns its cycles, or 0. */
static int is_turn(int head, int level, int back)
{
	is_sim *S = &vt_isim;
	S->cycles = 0;
	for (;;) {
		const int pc = S->pc, d = S->depth;
		if (!is_step() || S->depth < level) return 0;
		if (S->pc == head && S->depth == level) return S->tookback && d == level && pc == back ? S->cycles : 0;
	}
}

EWRAM_BSS u8 vt_isim_skip, vt_isim_fails;
EWRAM_BSS const u8 *vt_isim_hack[4];      /* the hack_pc this finder installed, per slot */
static bool find_idle_loop_run(const u8 *initpc, const u8 *lastbank, int hacknum);
static bool find_idle_loop(const u8 *initpc, const u8 *lastbank, int hacknum)
{
	/* every 4th call, backing off to every 32nd after failures in a row: in
	 * games whose sprite-0 hack slot keeps coming free (Sky Fighter) the
	 * finder runs every other frame, and the simulation is the dearest part
	 * of it.  An idle title still gets its hack within about half a second. */
	if (++vt_isim_skip < 4u << (vt_isim_fails < 3 ? vt_isim_fails : 3))   /* skipped: keep a hack of ours */
		return speedhacks[hacknum].hack_pc && speedhacks[hacknum].hack_pc == vt_isim_hack[hacknum];
	vt_isim_skip = 0;
	if (!find_idle_loop_run(initpc, lastbank, hacknum)) {
		if (vt_isim_fails < 255) vt_isim_fails++;
		return false;
	}
	vt_isim_fails = 0;
	return true;
}

static bool find_idle_loop_run(const u8 *initpc, const u8 *lastbank, int hacknum)
{
	is_sim *S = &vt_isim;
	is_state *t = &S->st;
	const int start = (initpc - lastbank) & 0xFFFF;
	S->ok = 1;
	S->budget = 640;                         /* instructions, over all the tries below */
	t->a = vt_isim_regs[0] >> 24; t->x = vt_isim_regs[1] >> 24; t->y = vt_isim_regs[2] >> 24;
	t->s = (u32)_m6502_s & 0xFF;
	t->n = t->z = t->c = t->v = -1;          /* unknown: reading one before it is set gives up */
	t->strobe = 0; t->nw = 0;
	t->ser0 = t->ser1 = 0;
	/* the pads' real shift state is not known here: strobe them first in the
	 * simulation (a loop that reads them strobes them itself), so a turn that
	 * reads without strobing gives up below unless it reaches a fixed point */
	is_write(0x4016, 1); is_write(0x4016, 0);
	/* The vblank can land anywhere in the loop, inside a subroutine too.  Run
	 * on until a backward jump is taken at the shallowest call depth seen so
	 * far; its target is a loop head.  Test two turns from it; if they differ
	 * (an inner loop, a counter) keep running, out to the enclosing loop. */
	S->pc = start; S->depth = 0;
	int mindepth = 0, back = -1, cycles = 0;
	for (;;) {
		const int pc = S->pc, d = S->depth;
		if (!is_step()) return false;
		if (S->depth < mindepth) mindepth = S->depth;
		if (!S->tookback || d != mindepth) continue;
		const int head = S->pc, level = d;
		back = pc;
		cycles = is_turn(head, level, back);
		if (!cycles) { if (S->ok && S->budget > 0 && S->depth < level) { mindepth = S->depth; continue; } return false; }
		S->first = *t;
		if (is_turn(head, level, back) != cycles) { if (S->ok && S->budget > 0) continue; return false; }
		/* the second turn must change nothing */
		is_state *f = &S->first;
		int same = t->a == f->a && t->x == f->x && t->y == f->y && t->s == f->s && t->n == f->n && t->z == f->z &&
		           t->c == f->c && t->v == f->v && t->ser0 == f->ser0 && t->ser1 == f->ser1 &&
		           t->strobe == f->strobe && t->nw == f->nw;
		for (int i = 0; same && i < t->nw; i++) same = t->wa[i] == f->wa[i] && t->wv[i] == f->wv[i];
		if (same) break;
		if (S->budget <= 0) return false;
	}
	{
		speedhack_T *hack = &speedhacks[hacknum];
		u32 total = (u32)cycles * (vt_cpu_x3 ? 1 : 3);
		if (!total) return false;
		hack->hack_pc = memmap_tbl[back >> 13] + back;
		vt_isim_hack[hacknum] = hack->hack_pc;
		hack->num_incs = 0;
		hack->cycles_per_iteration = total;
		hack->divider = 0xFFFFFFFFu / total + 1;
		for (int i = 0; i < 16; i++) SPEEDHACK_INCS[hacknum * 16 + i] = 0;
	}
	return true;
}
#endif

bool quickhackfinder(const u8 *initpc, const u8 *lastbank, int hacknum)
{
	const u8 *branchpc;
	const u8 *hackpc;
	const u8 *pc;
	
	pc=find_first_instruction(initpc,lastbank,&branchpc);
	hackpc=pc?find_hack(pc,branchpc,lastbank,hacknum):NULL;
#if VT_MODE
	if (!hackpc && vt_active) return find_poll_loop(initpc,lastbank,hacknum) || find_idle_loop(initpc,lastbank,hacknum);
#endif
	if (!hackpc) return false;
	return true;
}

int konami_match(const u8 *base_pc, int size)
{
	int hacknumber;
	int X,Y;
	if (size<6 || size>9) return -1;
	for (hacknumber=0;hacknumber<ARRSIZE(konami_speedhack_sizes);hacknumber++)
	{
		if (konami_speedhack_sizes[hacknumber]==size)
		{
			const u8 *hack = konami_speedhacks[hacknumber];
			int i;
			X=-1;
			Y=-1;
			for (i=0;i<size;i++)
			{
				u8 from_rom, from_hack;
				from_rom=base_pc[i];
				from_hack=hack[i];
				if (from_hack == _XXX)
				{
					if (X==-1) X=from_rom;
					if (from_rom!=X) break;
				}
				else if (from_hack == _YYY)
				{
					if (Y==-1) Y=from_rom;
					if (from_rom!=Y) break;
				}
				else
				{
					if (from_rom!=from_hack) break;
				}
			}
			if (i==size)
			{
				return hacknumber;	
			}
		}
	}
	return -1;
}



bool game_specific_hack(const u8 *initpc, const u8 *lastbank, int hacknum)
{
	//capcom uses a specific set of instructions which is quite hackable
	const u8 *jump;
	int jumppc;
	int jumpdest;
	int jumpsize;
	const u8 *hackbase;
	
#if VT_MODE
	//these match raw bytes: not valid under VT opcode encryption
	if (vt_active && vt.encryption_active) return false;
#endif
	jump=(const u8*)memchr(initpc,0x4C,22);
	if (!jump) return false;
	jumppc=jump-lastbank;
	jumpdest=jump[1]+jump[2]*256;
	jumpsize=jumppc-jumpdest;
	hackbase=jumpdest+lastbank;
	
	if (jumpsize==sizeof(capcom_speedhack))
	{
		if (0==memcmp(hackbase,capcom_speedhack,sizeof(capcom_speedhack)))
		{
			speedhack_T *sh= &speedhacks[hacknum];
			sh->hack_pc=jump;
			sh->num_incs=0;
			if (((jumpdest+6)&0xFF00) == ((jumpdest+19)&0xFF00))
			{
				sh->cycles_per_iteration=273;
				sh->divider=0x00F00F01;
			}
			else	//branch crosses pages
			{
				sh->cycles_per_iteration=285;
				sh->divider=0x00E5F36D;
			}
			return true;
		}
	}
	else
	{
		int konami_hack_number;
		konami_hack_number=konami_match(hackbase,jumpsize);
		if (konami_hack_number>=0)
		{
			u32 cyc;
			speedhack_T *sh =&speedhacks[hacknum];
			sh->hack_pc=jump;
			sh->num_incs=0x80+konami_hack_number;
			cyc=konami_speedhack_cycles[konami_hack_number]*3;
			sh->cycles_per_iteration=cyc;
			sh->divider=((u64)0x100000000LL/(u64)cyc)+1;
			return true;
		}
	}
	return false;
}

void speedhack_manager(const u8* initpc, const u8* lastbank, int hacknum)
{
	int hack_to_install;

	// Session-20b3: the automatic finder MUST stay enabled for VT carts.
	// A blanket `if (vt_active) return;` gate was tried (to keep the finder
	// from re-classifying Star Ally's encrypted loop byte) and it silently
	// broke Lonely Island: LI's playable speed comes from the finder
	// installing a hack on its LDA/CMP $26 vblank wait, not from its own
	// hand-seeded entry (which uses divider=1, a no-op).  Gating the finder
	// dropped LI 100% -> ~22%.  The finder is encryption-aware -- it re-homes
	// the default BNE hack to op_table[0xB0] and installs the semantically
	// correct _D0y for SA -- so it is SAFE for SA (boots, stable f2400 soak,
	// 67%).  SA no longer hand-seeds any hack (see vt_regs.c); it relies on
	// this finder entirely.  s.86: the finder and set_cpu_hack now decode
	// opcodes for every encryption mode (vt_op_dec), so SA's LDA $6816 /
	// BNE wait (raw $B0, refused before) is hacked too: 55 -> 60 fps.
	speedhack_T *sh=&speedhacks[hacknum];

#ifdef VT_NO_SPEEDHACK
	return;	//diagnostic build: never install a hack (s.87)
#endif
	hack_to_install=0;
	if (hacknum==0 && (ppustat_ & 0x40) )
	{
		hack_to_install=1;
	}

	
//	if (sh->hack_was_used)   //this check was moved to ASM
//	{
//		sh->frames_hack_not_used=0;
//		sh->hack_was_used=0;
//	}
//	else
	{
		sh->frames_hack_not_used++;
		if (sh->frames_hack_not_used==8)
		{
			sh->frames_hack_not_used=0;
			if (quickhackfinder(initpc,lastbank,hacknum))
			{
//				set_cpu_hack(hacknum2);
			}
			else
			{
				sh->hack_pc=NULL;
				set_cpu_hack(hack_to_install);	//remove hack
//				if (frametotal<24)
//				{
//					breakpoint();
					game_specific_hack(initpc,lastbank,hacknum);
//				}
			}
		}
	}
	if (speedhacks[hack_to_install].hack_pc)
	{
		set_cpu_hack(hack_to_install);
	}
}
