/* vram_dump PLAY.gba NGBAFRAMES OUT [KEYS_FROM KEYS_TO KEYMASK] -- runs N GBA frames, writes OUT.vram (96K) and OUT.ewram (256K) */
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
	int n = atoi(argv[2]);
	int kf = argc > 6 ? atoi(argv[4]) : -1, kt = argc > 6 ? atoi(argv[5]) : -1, km = argc > 6 ? atoi(argv[6]) : 0;
	for (int i = 0; i < n; i++) { c->setKeys(c, (i >= kf && i <= kt) ? km : 0); c->runFrame(c); }
	char p[512]; FILE *f;
	sprintf(p, "%s.vram", argv[3]); f = fopen(p, "wb");
	for (unsigned a = 0x06000000; a < 0x06018000; a += 4) { unsigned v = c->busRead32(c, a); fwrite(&v, 4, 1, f); } fclose(f);
	sprintf(p, "%s.ewram", argv[3]); f = fopen(p, "wb");
	for (unsigned a = 0x02000000; a < 0x02040000; a += 4) { unsigned v = c->busRead32(c, a); fwrite(&v, 4, 1, f); } fclose(f);
	sprintf(p, "%s.iwram", argv[3]); f = fopen(p, "wb");
	for (unsigned a = 0x03000000; a < 0x03008000; a += 4) { unsigned v = c->busRead32(c, a); fwrite(&v, 4, 1, f); } fclose(f);
	return 0;
}
