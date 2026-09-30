/* peek PLAY.gba EVERY TO ADDR[:n] ... -- every EVERY GBA frames up to TO, print n bytes (default 4, as a word) at each ADDR.
 * With FT=<frametotal addr>, KEYS="first-last:mask,..." presses GBA keys over NES frames (as nestrace). */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
int main(int argc, char **argv) {
	if (argc < 5) { fprintf(stderr, "usage: peek PLAY.gba EVERY TO ADDR[:n] ...\n"); return 2; }
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int every = atoi(argv[2]), to = atoi(argv[3]);
	const char *ks = getenv("KEYS"); unsigned fta = getenv("FT") ? strtoul(getenv("FT"), 0, 16) : 0;
	for (int i = 1; i <= to; i++) {
		unsigned keys = 0;
		if (ks && fta) {
			unsigned ft = c->busRead32(c, fta); const char *q = ks;
			while (*q) { unsigned f0, f1, m; int used;
				if (sscanf(q, "%u-%u:%u%n", &f0, &f1, &m, &used) != 3) break;
				if (ft >= f0 && ft <= f1) keys |= m;
				q += used; if (*q == ',') q++; }
		}
		c->setKeys(c, keys); c->runFrame(c);
		if (i % every) continue;
		printf("%d", i);
		for (int a = 4; a < argc; a++) {
			unsigned ad = strtoul(argv[a], 0, 16); char *col = strchr(argv[a], ':');
			int n = col ? atoi(col + 1) : 0;
			if (!n) printf(" %08X", c->busRead32(c, ad));
			else { printf(" "); for (int k = 0; k < n; k++) printf("%02X", c->busRead8(c, ad + k)); }
		}
		printf("\n");
	}
	return 0;
}
