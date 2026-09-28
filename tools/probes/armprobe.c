/* armprobe PLAY.gba LASTBANK FRAMETOTAL FROM TO -- samples ARM pc, r9 (6502 host pc), lastbank every 20000 steps */
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
	unsigned LB = strtoul(argv[2],0,16), FT = strtoul(argv[3],0,16);
	int from = atoi(argv[4]), to = atoi(argv[5]);
	struct ARMCore *cpu = c->cpu;
	for (int i = 1; i <= to; i++) {
		c->setKeys(c, 0);
		if (i < from) { c->runFrame(c); continue; }
		for (int s = 0; s < 8; s++) {
			for (int k = 0; k < 20000; k++) c->step(c);
			unsigned r9 = cpu->gprs[9], lb = c->busRead32(c, LB);
			printf("%d.%d ft=%u armpc=%08X r9=%08X lb=%08X nes=%04X\n", i, s, c->busRead32(c, FT), cpu->gprs[15], r9, lb, (r9 - lb) & 0xFFFF);
		}
		c->runFrame(c);
	}
	return 0;
}
