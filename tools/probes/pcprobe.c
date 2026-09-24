/* pcprobe PLAY.gba M6502PC LASTBANK FRAMETOTAL FROM TO -- per GBA frame: frametotal, host pc, lastbank, NES pc */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
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
	unsigned PC = strtoul(argv[2],0,16), LB = strtoul(argv[3],0,16), FT = strtoul(argv[4],0,16);
	int from = atoi(argv[5]), to = atoi(argv[6]);
	for (int i = 1; i <= to; i++) {
		c->setKeys(c, 0); c->runFrame(c);
		if (i >= from) {
			unsigned pc = c->busRead32(c, PC), lb = c->busRead32(c, LB), ft = c->busRead32(c, FT);
			printf("%d ft=%u host=%08X lb=%08X nes=%04X\n", i, ft, pc, lb, (pc - lb) & 0xFFFF);
		}
	}
	return 0;
}
