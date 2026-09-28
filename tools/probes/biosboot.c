/* biosboot PLAY.gba BIOS.bin NFRAMES FRAMETOTAL -- boot through the REAL BIOS (no HLE, no skip):
 * report the first GBA frame the ARM pc leaves the BIOS, and frametotal every 60 frames. */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <mgba-util/vfs.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
int main(int argc, char **argv) {
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	mCoreConfigSetValue(&c->config, "useBios", "1");
	mCoreConfigSetValue(&c->config, "skipBios", "0");
	mCoreLoadConfig(c);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	struct VFile *vf = VFileOpen(argv[2], O_RDONLY);
	if (!vf || !c->loadBIOS(c, vf, 0)) { fprintf(stderr, "cannot load BIOS\n"); return 1; }
	c->opts.useBios = true; c->opts.skipBios = false;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	struct ARMCore *cpu = c->cpu;
	printf("after reset pc=%08X\n", cpu->gprs[15]);
	int n = atoi(argv[3]), left = -1; unsigned FT = strtoul(argv[4], 0, 16);
	for (int i = 1; i <= n; i++) {
		c->runFrame(c);
		if (left < 0 && cpu->gprs[15] >= 0x02000000) { left = i; printf("left BIOS at GBA frame %d pc=%08X\n", i, cpu->gprs[15]); }
		if (i % 60 == 0) printf("frame %d pc=%08X frametotal=%u\n", i, cpu->gprs[15], c->busRead32(c, FT));
	}
	return 0;
}
