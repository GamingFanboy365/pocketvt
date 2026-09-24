/* lbwatch PLAY.gba LASTBANK FRAMETOTAL GBAFRAME NSTEPS -- run to GBAFRAME, then single-step and log lastbank changes */
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
	int gf = atoi(argv[4]); long ns = atol(argv[5]);
	struct ARMCore *cpu = c->cpu;
	for (int i = 1; i < gf; i++) { c->setKeys(c, 0); c->runFrame(c); }
	unsigned plb = c->busRead32(c, LB);
	unsigned hist[64][3]; int hn = 0;
	for (long k = 0; k < ns; k++) {
		c->step(c);
		unsigned lb = c->busRead32(c, LB);
		hist[hn & 63][0] = cpu->gprs[15]; hist[hn & 63][1] = cpu->gprs[9]; hist[hn & 63][2] = lb; hn++;
		if (lb != plb) {
			printf("step %ld ft=%u lastbank %08X -> %08X  armpc=%08X r9=%08X  nes=%04X\n", k, c->busRead32(c, FT), plb, lb, cpu->gprs[15], cpu->gprs[9], (cpu->gprs[9]-lb)&0xFFFF);
			if ((lb >> 24) == 3) {
				printf(" last steps (armpc r9 lb):\n");
				for (int j = hn - 40; j < hn; j++) if (j >= 0) printf("  %08X %08X %08X nes=%04X\n", hist[j&63][0], hist[j&63][1], hist[j&63][2], (hist[j&63][1]-hist[j&63][2])&0xFFFF);
				return 0;
			}
			plb = lb;
		}
	}
	return 0;
}
