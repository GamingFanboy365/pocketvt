/* memdump PLAY.gba NGBAFRAMES OUTPREFIX ADDR:LEN ... -- after N GBA frames, write each range to OUTPREFIX_ADDR.bin */
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
	if (argc < 5) { fprintf(stderr, "usage: memdump PLAY.gba N OUTPREFIX ADDR:LEN ...\n"); return 2; }
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int n = atoi(argv[2]);
	for (int i = 0; i < n; i++) { c->setKeys(c, 0); c->runFrame(c); }
	for (int a = 4; a < argc; a++) {
		unsigned ad = strtoul(argv[a], 0, 16); char *col = strchr(argv[a], ':');
		unsigned len = col ? strtoul(col + 1, 0, 16) : 4;
		char fn[512]; snprintf(fn, sizeof fn, "%s_%08X.bin", argv[3], ad);
		FILE *f = fopen(fn, "wb");
		for (unsigned k = 0; k < len; k++) fputc(c->busRead8(c, ad + k), f);
		fclose(f);
	}
	return 0;
}
