/* pacetrace PLAY.gba -- a timeline of what the GBA does around the NES frame end.
 * Runs with input keyed to NES frames (FT=<frametotal> KEYS="first-last:mask,...") up to NES
 * frame FROMFT, then single-steps STEPS instructions and prints one line per event, with the
 * GBA time (cycles since the start of the trace), the GBA line (VCOUNT) and frametotal:
 *   nes   frametotal moved
 *   wait  the pc reached WAIT (the frame-end IntrWait of the pacing loop, ppu.s l02)
 *   wake  the first instruction after it (how long the CPU slept)
 *   hit   the pc reached one of WATCH="addr[=name],..." (handler entries)
 * PEEK="addr,..." adds those words to every line.  The summary at the end gives, per GBA frame,
 * the NES frames that ended in it and the cycles spent asleep, which tells waiting from work
 * (MAINTAINERS_GUIDE s.98: Sky Fighter's frame end slept 44% of the time below 60 fps). */
#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
static unsigned keys_at(const char *ks, unsigned f) {
	unsigned keys = 0; if (!ks) return 0;
	while (*ks) { unsigned f0, f1, m; int used;
		if (sscanf(ks, "%u-%u:%u%n", &f0, &f1, &m, &used) != 3) break;
		if (f >= f0 && f <= f1) keys |= m;
		ks += used; if (*ks == ',') ks++; }
	return keys;
}
int main(int argc, char **argv) {
	if (argc < 2 || !getenv("FT") || !getenv("WAIT")) {
		fprintf(stderr, "usage: FT=<frametotal> WAIT=<addr> [FROMFT=n] [STEPS=n] [KEYS=...] [WATCH=a=name,...] [PEEK=a,...] pacetrace PLAY.gba\n");
		return 2;
	}
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	struct ARMCore *cpu = c->cpu;
	unsigned ft = strtoul(getenv("FT"), 0, 16), wait = strtoul(getenv("WAIT"), 0, 16) & ~1u;
	unsigned from = getenv("FROMFT") ? atoi(getenv("FROMFT")) : 1700;
	long steps = getenv("STEPS") ? atol(getenv("STEPS")) : 6000000;
	const char *ks = getenv("KEYS");
	unsigned wa[16]; char wn[16][32]; int nw = 0;
	for (const char *p = getenv("WATCH"); p && *p && nw < 16; ) {
		char *e; wa[nw] = strtoul(p, &e, 16) & ~1u; p = e; wn[nw][0] = 0;
		if (*p == '=') { int k = 0; p++; while (*p && *p != ',' && k < 31) wn[nw][k++] = *p++; wn[nw][k] = 0; }
		nw++; if (*p == ',') p++;
	}
	unsigned pa[8]; int np = 0;
	for (const char *p = getenv("PEEK"); p && *p && np < 8; ) { char *e; pa[np++] = strtoul(p, &e, 16); p = e; if (*p == ',') p++; }
	while (c->busRead32(c, ft) < from) { c->setKeys(c, keys_at(ks, c->busRead32(c, ft))); c->runFrame(c); }
	int64_t t0 = mTimingGlobalTime(c->timing);
	unsigned lastft = c->busRead32(c, ft), fc0 = c->frameCounter(c);
	int asleep = 0; int64_t sleep_from = 0;
	/* per GBA frame: NES frames ended, cycles asleep */
	static int nes_in[4096]; static long long slept[4096];
	for (long k = 0; k < steps; k++) {
		c->setKeys(c, keys_at(ks, c->busRead32(c, ft)));
		unsigned pc = (cpu->gprs[15] - (cpu->cpsr.t ? 4 : 8)) & ~1u;
		int64_t t = mTimingGlobalTime(c->timing) - t0;
		unsigned vc = c->busRead16(c, 0x04000006), f = c->busRead32(c, ft), gf = c->frameCounter(c) - fc0;
		if (asleep && pc != wait) {
			printf("%9lld  line %3u  ft %u  wake  slept %lld\n", (long long)t, vc, f, (long long)(t - sleep_from));
			if (gf < 4096) slept[gf] += t - sleep_from;
			asleep = 0;
		}
		const char *ev = NULL;
		if (pc == wait && !asleep) { ev = "wait"; asleep = 1; sleep_from = t; }
		for (int i = 0; i < nw; i++) if (pc == wa[i]) ev = wn[i][0] ? wn[i] : "hit";
		if (f != lastft) { printf("%9lld  line %3u  ft %u  nes\n", (long long)t, vc, f); if (gf < 4096) nes_in[gf]++; lastft = f; }
		if (ev) {
			printf("%9lld  line %3u  ft %u  %s", (long long)t, vc, f, ev);
			for (int i = 0; i < np; i++) printf(" [%08X]=%08X", pa[i], c->busRead32(c, pa[i]));
			printf("\n");
		}
		c->step(c);
	}
	unsigned nf = c->frameCounter(c) - fc0;
	printf("summary: GBA frame: NES frames ended / cycles asleep\n");
	for (unsigned g = 0; g < nf && g < 4096; g++) printf("%u:%d/%lld%s", g, nes_in[g], slept[g], (g + 1) % 8 ? "  " : "\n");
	printf("\n");
	return 0;
}
