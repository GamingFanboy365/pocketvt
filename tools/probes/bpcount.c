/* bpcount PLAY.gba FROMFRAME NSTEPS ADDR1 [ADDR2...] -- count hits of each pc address (thumb bit ignored), print step of first/last */
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
	for (int i = 1; i < gf; i++) { c->setKeys(c, 0); c->runFrame(c); }
	for (long k = 0; k < ns; k++) {
		c->step(c);
		unsigned pc = (cpu->gprs[15] - (cpu->cpsr.t ? 4 : 8)) & ~1u;
		for (int i = 0; i < na; i++) if (pc == a[i]) { cnt[i]++; if (first[i] < 0) first[i] = k; last[i] = k; }
	}
	for (int i = 0; i < na; i++) printf("%08X hits=%ld first=%ld last=%ld\n", a[i], cnt[i], first[i], last[i]);
	return 0;
}
