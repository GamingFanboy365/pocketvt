/* vt369snd_test -- checks src/vt369_snd.c's batched sound-CPU HLE against a
 * direct port of Furbtendulator's per-tick one (OneBus_VT369.cpp,
 * APU_VT369::Run; ADPCM_VT369.cpp; ADPCM_VT1682.cpp) on random states.
 *   gcc -O2 -I tools/probes -DVT369_SND_HOSTTEST tools/probes/vt369snd_test.c -o vt369snd_test
 * Every fill of 128 ticks must give the same per-tick sum and leave the same
 * sound RAM and decoder state.  One deliberate change from the reference:
 * a start bit acts on its rising edge, as in the cart's own program ($40AE),
 * so the model below keeps the last start mask (rprev).  Guide s.84. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt369snd_hoststubs.h"
#include "../../src/vt369_snd.c"

/* ---------------- reference (per tick) ---------------- */
static uint8_t R[8192];
static uint64_t rframe[3];
static uint8_t rcount[3];
static uint8_t rprev;
static const int16_t rstep[16][16] ={
	{0,14,28,42,56,70,84,97,-111,-97,-84,-70,-56,-42,-28,-14},{0,13,26,39,52,65,78,91,-104,-91,-78,-65,-52,-39,-26,-13},
	{0,11,21,32,43,54,64,75,-86,-75,-64,-54,-43,-32,-21,-11},{0,9,18,27,35,44,53,62,-71,-62,-53,-44,-35,-27,-18,-9},
	{0,7,13,20,27,34,40,47,-54,-47,-40,-34,-27,-20,-13,-7},{0,6,11,17,22,28,33,39,-44,-39,-33,-28,-22,-17,-11,-6},
	{0,5,9,14,18,23,27,32,-36,-32,-27,-23,-18,-14,-9,-5},{0,4,8,11,15,19,23,26,-30,-26,-23,-19,-15,-11,-8,-4},
	{0,3,6,9,12,15,17,20,-23,-20,-17,-15,-12,-9,-6,-3},{0,2,5,7,10,12,14,17,-19,-17,-14,-12,-10,-7,-5,-2},
	{0,2,4,6,8,10,12,14,-16,-14,-12,-10,-8,-6,-4,-2},{0,2,3,5,6,8,10,11,-13,-11,-10,-8,-6,-5,-3,-2},
	{0,1,2,4,5,6,7,9,-10,-9,-7,-6,-5,-4,-2,-1},{0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1},
	{0,1,2,3,3,4,5,6,-7,-6,-5,-4,-3,-3,-2,-1},{0,1,1,2,3,4,4,5,-6,-5,-4,-4,-3,-2,-1,-1}};
static int32_t rdecode(uint8_t *a) {
	a[5] %=48;
	int nibble =a[1] >>(a[5] &1? 4: 0);
	int index  =a[0] -(a[5] >=24 && a[0] &0x40? 1: 0) +(a[5] >=24 && a[0] &0x80? 2: 0);
	int step   =rstep[index &0xF][nibble &0xF];
	int8_t output =a[4];
	switch (a[0] >>4 &3) {
		case 0: output =step; break;
		case 1: output =step +(int8_t) a[4]; break;
		case 2: output =step +(int8_t) a[4]*2 - (int8_t) a[3]; break;
		case 3: output =step +(int8_t) a[4] -((int8_t) a[3] >>1); break;
	}
	a[3] =a[4]; a[4] =output; a[5]++;
	return output *(a[2] &0x7F);
}
static const uint8_t ris[4] ={0,0,3,5};
static const uint8_t rit[26] ={0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,20,20,20,20};
static const int8_t rst[4][21] ={{0,1,1,1,1,1,2,2,2,3,3,4,5,5,6,7,8,10,11,13,15},{1,3,3,3,4,4,6,6,7,9,10,12,15,16,19,22,25,30,34,40,46},
	{3,5,5,6,7,8,10,11,13,16,18,21,25,28,32,38,43,51,58,68,78},{4,7,7,8,10,11,14,15,18,22,25,29,35,39,45,53,60,71,81,95,109}};
static void rd1682(uint8_t code, int8_t *output, uint8_t *index) {
	int16_t p =*output +rst[code &3][*index] *(code &4? -1: 1);
	*output =p <-128? -128: p >127? 127: p;
	*index =rit[*index +ris[code &3]];
}
static int32_t ref_tick(const uint8_t *prg, uint32_t pmask) {
	uint16_t rv =R[0x1FF8] | R[0x1FF9] <<8;
	if (rv ==0x0293) {
		switch (R[0x1FA2]) {
			case 1: R[0x1CB0+R[0x1FA3]] =0xFF; R[0x1CB7+R[0x1FA3]*2] =0; R[0x1980+R[0x1FA3]*2] =0; R[0x1981+R[0x1FA3]*2] =0; R[0x1FA2] =0; rcount[R[0x1FA3]] =0; break;
			case 2: R[0x1FA2] =0; break;
			case 3: R[0x1CB0+R[0x1FA3]] =0; R[0x1FA2] =0; break;
		}
		int32_t out =0;
		for (int ch =0; ch <3; ch++) {
			if (R[0x1CB0+ch] ==0) continue;
			if (rcount[ch] ==0) {
				rframe[ch] =0;
				uint16_t ra =0x1800 +ch*0x80 +R[0x1CB7+ch*2];
				for (int i =0; i <8; i++) rframe[ch] |=(uint64_t) R[ra+i] <<(i*8);
				if (rframe[ch] &0x8000000000000000ull) rframe[ch] =0;
				rcount[ch] =21;
				R[0x1CB7+ch*2] +=8; R[0x1CB7+ch*2] &=0x7F;
			}
			rd1682(rframe[ch] &7, (int8_t *)&R[0x1980+ch*2], &R[0x1981+ch*2]);
			out +=(int8_t)R[0x1980+ch*2] <<7;
			rframe[ch] >>=3; rcount[ch]--;
		}
		return out;
	}
	uint16_t m =0, loop =0; uint8_t inc =1; int stream =0;
	switch (rv) {
		case 0x0203: case 0x02A0: m =0x184C; loop =0x18FC; break;
		case 0x02E0: m =0x184C; loop =0x1873; inc =4; break;
		case 0x0250: m =0x184C; loop =0x18FC; stream =1; break;
		case 0x1C4C: case 0x40AE: m =0x18A4; break;
		default: return 0;
	}
	R[0x18F6]++;
	R[m+1] =R[m+1] &~R[m+2];
	const uint8_t edge =R[m] &~rprev;
	rprev =R[m];
	int32_t out =0;
	for (int ch =0; ch <4; ch++) {
		if (edge &(1 <<ch)) { R[0x1830+ch*4] =R[0x1860+ch*4]; R[0x1831+ch*4] =R[0x1861+ch*4]; R[0x1832+ch*4] =R[0x1862+ch*4]; }
		if (!(R[m+1] &(1 <<ch))) continue;
		uint32_t wa =stream? R[0x1D21+ch*2]: (uint32_t)(R[0x1830+ch*4] | R[0x1831+ch*4] <<8 | R[0x1832+ch*4] <<16);
		if (R[0x1805+ch*8] ==48) {
			if (stream) { R[0x1800+ch*8] =R[0x1900+ch*0x100+(wa++ &0xFF)]; R[0x1801+ch*8] =R[0x1900+ch*0x100+(wa++ &0xFF)]; }
			else { R[0x1800+ch*8] =prg[wa++ &pmask]; R[0x1801+ch*8] =prg[wa++ &pmask]; }
			if (R[0x1800+ch*8] ==0xFF) {
				if (loop && R[loop +ch*inc]) {
					R[0x1830+ch*4] =R[0x1860+ch*4]; R[0x1831+ch*4] =R[0x1861+ch*4]; R[0x1832+ch*4] =R[0x1862+ch*4];
					R[0x1803+ch*8] =0; R[0x1804+ch*8] =0; R[0x1805+ch*8] =48;
				} else R[m+1] &=~(1 <<ch);
				continue;
			}
		} else if (~R[0x1805+ch*8] &1) {
			R[0x1801+ch*8] =stream? R[0x1900+ch*0x100+wa++]: prg[wa++ &pmask];
		}
		if (stream) R[0x1D21+ch*2] =wa &0xFF;
		else { R[0x1830+ch*4] =wa; R[0x1831+ch*4] =wa >>8; R[0x1832+ch*4] =wa >>16; }
		out +=rdecode(&R[0x1800+ch*8]);
	}
	R[m+1] =R[m+1] | edge;
	return out;
}

static uint32_t rng =12345;
static uint32_t rnd(void) { rng =rng *1103515245u +12345u; return rng >>8; }

int main(void) {
	static uint8_t prg[1 <<16];
	static const uint16_t vecs[] ={0x0293, 0x40AE, 0x1C4C, 0x0203, 0x02A0, 0x02E0, 0x0250};
	int fails =0, fills =0;
	vt369_snd_reset();                    /* builds vt1682_tab */
	for (int trial =0; trial <3000; trial++) {
		for (int i =0; i <(int)sizeof prg; i++) prg[i] =(rnd() %9 ==0)? 0xFF: (uint8_t)rnd();
		rombase =prg; rommask =sizeof prg -1; vt_chr_src =0;
		uint16_t rv =vecs[trial %7];
		for (int i =0x1800; i <0x2000; i++) R[i] =(uint8_t)rnd();
		R[0x1FF8] =rv; R[0x1FF9] =rv >>8;
		for (int ch =0; ch <4; ch++) if (rnd() %2) R[0x1805+ch*8] =48;   /* often at a frame start */
		if (rv ==0x0293) { R[0x1FA2] =rnd() %5; R[0x1FA3] =rnd() %3;
			/* reachable states only: the start command zeroes the read offset and
			 * it steps by 8 inside the ring ($00-$78); the index stays <= 20 */
			for (int ch =0; ch <3; ch++) { R[0x1981+ch*2] %=21; R[0x1CB7+ch*2] =(rnd() %16) *8; } }
		uint16_t m =(rv ==0x40AE || rv ==0x1C4C)? 0x18A4: 0x184C;
		if (rv !=0x0293) { R[m] =rnd() %4 ==0? (uint8_t)rnd(): 0; R[m+2] =rnd() %4 ==0? (uint8_t)rnd(): 0; }
		rprev =vt369_st_prev =rnd() %2? (uint8_t)rnd(): 0;
		memcpy(vt369_sram, &R[0x1800], 0x800);
		for (int ch =0; ch <3; ch++) { rframe[ch] =0; rcount[ch] =0; vt369_adpcm_left[ch] =0; vt369_adpcm_frame[ch][0] =vt369_adpcm_frame[ch][1] =0; }
		for (int f =0; f <6; f++) {
			int32_t want[128], got[128];
			for (int t =0; t <128; t++) want[t] =ref_tick(prg, rommask);
			static s32 mix[128]; memset(mix, 0, sizeof mix);
			vt369_render(mix, 128);
			for (int t =0; t <128; t++) got[t] =mix[t];
			fills++;
			int bad =memcmp(want, got, sizeof want) !=0;
			int badram =memcmp(&R[0x1800], vt369_sram, 0x800) !=0;
			for (int ch =0; ch <3 && rv ==0x0293; ch++)
				if (rcount[ch] !=vt369_adpcm_left[ch] || (uint32_t)rframe[ch] !=vt369_adpcm_frame[ch][0] || (uint32_t)(rframe[ch] >>32) !=vt369_adpcm_frame[ch][1]) badram =1;
			if (bad || badram) {
				if (fails++ <8) {
					int t0 =0; while (t0 <128 && want[t0] ==got[t0]) t0++;
					int a0 =0; while (a0 <0x800 && R[0x1800+a0] ==vt369_sram[a0]) a0++;
					printf("trial %d rv %04X fill %d: out %s (first tick %d: %d vs %d) ram %s (first $%04X: %02X vs %02X)\n",
					       trial, rv, f, bad? "DIFF": "ok", t0, t0 <128? want[t0]: 0, t0 <128? got[t0]: 0,
					       badram? "DIFF": "ok", 0x1800+a0, a0 <0x800? R[0x1800+a0]: 0, a0 <0x800? vt369_sram[a0]: 0);
				}
				break;
			}
			/* the game changes things between fills */
			if (rnd() %3 ==0) { uint8_t v =(uint8_t)rnd(); uint16_t a =0x1800 +rnd() %0x800;
				if (a ==0x1CB7 || a ==0x1CB9 || a ==0x1CBB) v &=0x78;
				if (a ==0x1981 || a ==0x1983 || a ==0x1985) v %=21;
				if (a !=0x1FF8 && a !=0x1FF9) { R[a] =v; vt369_sram[a -0x1800] =v; } }
		}
	}
	printf("%d fills compared, %d failing trials\n", fills, fails);
	return fails !=0;
}
