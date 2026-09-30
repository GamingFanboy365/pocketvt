/* fgrab PLAY.gba FROM TO -- writes GBA frames FROM..TO to stdout: per frame a 4-byte frametotal
 * (FT= address, else 0) and 240x160 RGBX pixels.  FT=<frametotal addr> KEYS="first-last:mask,..."
 * presses GBA keys over NES frames, as nestrace/peek.  For frame-by-frame checks (flicker,
 * one-frame glitches) that compare_furb's NES-frame-keyed captures step over. */
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
	if (argc < 4) { fprintf(stderr, "usage: fgrab PLAY.gba FROM TO > frames.bin\n"); return 2; }
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static uint32_t buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int from = atoi(argv[2]), to = atoi(argv[3]);
	const char *ks = getenv("KEYS"); unsigned fta = getenv("FT") ? strtoul(getenv("FT"), 0, 16) : 0;
	for (int i = 1; i <= to; i++) {
		unsigned keys = 0;
		if (ks && fta) {
			unsigned ft = c->busRead32(c, fta); const char *p = ks;
			while (*p) { unsigned f0, f1, m; int used;
				if (sscanf(p, "%u-%u:%u%n", &f0, &f1, &m, &used) != 3) break;
				if (ft >= f0 && ft <= f1) keys |= m;
				p += used; if (*p == ',') p++; }
		}
		c->setKeys(c, keys); c->runFrame(c);
		if (i < from) continue;
		uint32_t ft = fta ? c->busRead32(c, fta) : 0;
		fwrite(&ft, 4, 1, stdout);
		for (unsigned y = 0; y < h; y++) fwrite(buf + y * w, 4, w, stdout);
	}
	return 0;
}
