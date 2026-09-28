/* ramat PLAY.gba FRAMETOTAL OUT F1,F2,... -- when frametotal first reaches F, dump NES RAM (0x03000000, 2K) + VRAM 96K to OUT_F.ram/.vram */
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
	unsigned FT = strtoul(argv[2],0,16); unsigned t[256]; int nt = 0;
	for (char *p = argv[4]; *p && nt < 256;) { t[nt++] = strtoul(p, &p, 10); if (*p == ',') p++; }
	int k = 0;
	for (int i = 0; i < 20000 && k < nt; i++) {
		c->setKeys(c, 0); c->runFrame(c);
		while (k < nt && c->busRead32(c, FT) >= t[k]) {
			char pth[512]; FILE *f;
			sprintf(pth, "%s_%u.ram", argv[3], t[k]); f = fopen(pth, "wb");
			for (unsigned a = 0x03000000; a < 0x03000800; a++) { unsigned char v = c->busRead8(c, a); fwrite(&v,1,1,f);} fclose(f);
			sprintf(pth, "%s_%u.vram", argv[3], t[k]); f = fopen(pth, "wb");
			for (unsigned a = 0x06000000; a < 0x06018000; a += 4) { unsigned v = c->busRead32(c, a); fwrite(&v,4,1,f);} fclose(f);
			k++;
		}
	}
	return 0;
}
