/* bpcount PLAY.gba FROMFRAME NSTEPS ADDR1 [ADDR2...] -- count hits of each pc address (thumb bit ignored), print step of first/last.
 * VERBOSE=1 also prints r0-r2 and lr at every hit; PEEK=ADDR adds the word there.
 * FT=<frametotal addr> with KEYS="first-last:mask,..." presses keys by NES frame; FROMFT=<n> starts there. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
int main(int argc, char **argv) {
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int gf = atoi(argv[2]); long ns = atol(argv[3]);
	int na = argc - 4; unsigned a[16]; long cnt[16] = {0}, first[16], last[16];
	for (int i = 0; i < na; i++) { a[i] = strtoul(argv[4+i],0,16) & ~1u; first[i] = last[i] = -1; }
	struct ARMCore *cpu = c->cpu;
	/* FT=<frametotal addr> KEYS="first-last:mask,..." (as nestrace); with FT, FROMFT=<n> runs until
	 * frametotal reaches n instead of FROMFRAME GBA frames */
	const char *ks = getenv("KEYS"); unsigned fta = getenv("FT") ? strtoul(getenv("FT"), 0, 16) : 0;
	unsigned fromft = getenv("FROMFT") ? strtoul(getenv("FROMFT"), 0, 0) : 0;
	for (int i = 1; fromft && fta ? c->busRead32(c, fta) < fromft : i < gf; i++) {
		unsigned keys = 0;
		if (ks && fta) {
			unsigned ft = c->busRead32(c, fta); const char *q = ks;
			while (*q) { unsigned f0, f1, m; int used;
				if (sscanf(q, "%u-%u:%u%n", &f0, &f1, &m, &used) != 3) break;
				if (ft >= f0 && ft <= f1) keys |= m;
				q += used; if (*q == ',') q++; }
		}
		c->setKeys(c, keys); c->runFrame(c);
	}
	for (long k = 0; k < ns; k++) {
		c->step(c);
		unsigned pc = (cpu->gprs[15] - (cpu->cpsr.t ? 4 : 8)) & ~1u;
		for (int i = 0; i < na; i++) if (pc == a[i]) { cnt[i]++; if (first[i] < 0) first[i] = k; last[i] = k;
			if (getenv("VERBOSE")) printf("hit %08X step %ld r0=%08X r1=%08X r2=%08X lr=%08X", a[i], k, cpu->gprs[0], cpu->gprs[1], cpu->gprs[2], cpu->gprs[14]);
				if (getenv("PEEK")) { unsigned pa = strtoul(getenv("PEEK"), 0, 16); printf(" [%08X]=%08X", pa, c->busRead32(c, pa)); }
				printf("\n"); }
	}
	for (int i = 0; i < na; i++) printf("%08X hits=%ld first=%ld last=%ld\n", a[i], cnt[i], first[i], last[i]);
	return 0;
}
