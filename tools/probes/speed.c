/* speed PLAY.gba FRAMETOTAL_HEX NGBA [START_FROM START_TO] -- prints NES frames per 60 GBA frames, per second of GBA time */
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
	unsigned ft = strtoul(argv[2], 0, 16); int n = atoi(argv[3]);
	int kf = argc > 5 ? atoi(argv[4]) : -1, kt = argc > 5 ? atoi(argv[5]) : -1;
	unsigned prev = 0;
	for (int i = 1; i <= n; i++) {
		c->setKeys(c, (i >= kf && i <= kt) ? 8 : 0);
		c->runFrame(c);
		if (i % 60 == 0) { unsigned v = c->busRead32(c, ft); printf("%u ", v - prev); prev = v; }
	}
	printf("\n");
	return 0;
}
