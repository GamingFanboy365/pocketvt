/* fdump PLAY.gba N F1 [F2...] -- fhash, plus frames F1.. written as PLAY.gba_fN.raw (240x160 RGBX) */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
int main(int argc, char **argv) {
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static uint32_t buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int n = atoi(argv[2]);
	for (int i = 1; i <= n; i++) {
		c->setKeys(c, 0); c->runFrame(c);
		uint64_t hsh = 1469598103934665603ULL;
		for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) { hsh ^= buf[y*w+x] & 0xF8F8F8; hsh *= 1099511628211ULL; }
		for (unsigned a = 0x05000000; a < 0x05000400; a += 2) { hsh ^= c->busRead16(c, a); hsh *= 1099511628211ULL; }
		printf("%d %016llx\n", i, (unsigned long long)hsh); for (int a = 3; a < argc; a++) if (atoi(argv[a]) == i) { char p[256]; sprintf(p, "%s_f%d.raw", argv[1], i); FILE *f = fopen(p, "wb"); for (unsigned y = 0; y < h; y++) fwrite(buf + y*w, 4, w, f); fclose(f); }
	}
	return 0;
}
