/* nestrace PLAY.gba OP_TABLE LASTBANK NSTEPS [OUT] -- 6502 instruction trace of PocketVT from power-on.
 * Single-steps the GBA and logs "PC A X Y" each time the ARM enters an opcode handler (any address
 * op_table has held; re-read every 4096 steps, so encryption re-permutations are followed).
 * PC = r9 - lastbank - 1 (r9 is past the opcode byte).  Compare with furb_cli --trace to find the
 * first instruction where the two CPUs part (guide s.88).  Symbols: op_table, _lastbank.
 * With FT=<frametotal addr>: KEYS="first-last:mask,..." presses GBA keys (A=1 B=2 Select=4 Start=8
 * Right=16 Left=32 Up=64 Down=128) over NES frames, and FROMFT=<n> logs only from NES frame n on (whole
 * GBA frames are run up to NES frame n-1, so NSTEPS counts from about there). */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
#define HS (1 << 14)
static unsigned hset[HS];
static void hadd(unsigned a) { unsigned h = (a * 2654435761u) >> 18; while (hset[h] && hset[h] != a) h = (h + 1) & (HS - 1); hset[h] = a; }
static int hhas(unsigned a) { unsigned h = (a * 2654435761u) >> 18; while (hset[h]) { if (hset[h] == a) return 1; h = (h + 1) & (HS - 1); } return 0; }
int main(int argc, char **argv) {
	if (argc < 5) { fprintf(stderr, "usage: nestrace PLAY.gba OP_TABLE LASTBANK NSTEPS [OUT]\n"); return 2; }
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	unsigned OT = strtoul(argv[2], 0, 16), LB = strtoul(argv[3], 0, 16);
	long ns = atol(argv[4]);
	FILE *out = argc > 5 ? fopen(argv[5], "w") : stdout;
	struct ARMCore *cpu = c->cpu;
	unsigned FTA = getenv("FT") ? strtoul(getenv("FT"), 0, 16) : 0, from = getenv("FROMFT") ? atoi(getenv("FROMFT")) : 0;
	unsigned kf[64], kt[64], km[64], nk = 0, on = !FTA || !from;
	for (const char *p = getenv("KEYS"); p && *p && nk < 64; nk++) {
		char *e; kf[nk] = strtoul(p, &e, 10); kt[nk] = strtoul(e + 1, &e, 10); km[nk] = strtoul(e + 1, &e, 10);
		p = *e ? e + 1 : e;
	}
	/* fast-forward whole GBA frames until NES frame FROMFT-1, then single-step (NSTEPS counts from there) */
	while (FTA && from > 1 && c->busRead32(c, FTA) + 1 < from) {
		unsigned ft = c->busRead32(c, FTA), m = 0;
		for (unsigned j = 0; j < nk; j++) if (ft >= kf[j] && ft <= kt[j]) m |= km[j];
		c->setKeys(c, m); c->runFrame(c);
	}
	for (long k = 0; k < ns; k++) {
		if ((k & 4095) == 0) {
			for (int i = 0; i < 256; i++) { unsigned a = c->busRead32(c, OT + 4 * i); if (a) hadd(a & ~1u); }
			if (FTA) {
				unsigned ft = c->busRead32(c, FTA), m = 0;
				for (unsigned j = 0; j < nk; j++) if (ft >= kf[j] && ft <= kt[j]) m |= km[j];
				c->setKeys(c, m);
				if (ft >= from) on = 1;
			}
		}
		if (!on) { c->step(c); continue; }
		c->step(c);
		unsigned pc = cpu->gprs[15] - (cpu->executionMode ? 4 : 8);
		if (hhas(pc)) {
			unsigned nes = (cpu->gprs[9] - c->busRead32(c, LB) - 1) & 0xFFFF;
			fprintf(out, "%04X A:%02X X:%02X Y:%02X\n", nes, ((unsigned)cpu->gprs[5] >> 24) & 0xFF, ((unsigned)cpu->gprs[6] >> 24) & 0xFF, ((unsigned)cpu->gprs[7] >> 24) & 0xFF);
		}
	}
	return 0;
}
