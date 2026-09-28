/* memwatch PLAY.gba ADDR FRAMETOTAL GBAFRAME NSTEPS IPB -- run to GBAFRAME, single-step, and log every change of the u32 at ADDR with ARM pc/r0/r1/lr and the first 8 instant_prg_banks entries (IPB = the table's address, read from _instant_prg_banks) */
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
	unsigned LB = strtoul(argv[2],0,16) /* the watched address */, FT = strtoul(argv[3],0,16);
	int gf = atoi(argv[4]); unsigned IPB = strtoul(argv[6],0,16); long ns = atol(argv[5]);
	struct ARMCore *cpu = c->cpu;
	for (int i = 1; i < gf; i++) { c->setKeys(c, 0); c->runFrame(c); }
	unsigned plb = c->busRead32(c, LB);
	unsigned hist[64][3]; int hn = 0;
	for (long k = 0; k < ns; k++) {
		c->step(c);
		unsigned lb = c->busRead32(c, LB);
		hist[hn & 63][0] = cpu->gprs[15]; hist[hn & 63][1] = cpu->gprs[9]; hist[hn & 63][2] = lb; hn++;
		if (lb != plb) {
			printf("r0=%08X r1=%08X lr=%08X ipb=[%08X %08X %08X %08X %08X %08X %08X %08X]\n", cpu->gprs[0], cpu->gprs[1], cpu->gprs[14], c->busRead32(c,IPB), c->busRead32(c,IPB+4), c->busRead32(c,IPB+8), c->busRead32(c,IPB+12), c->busRead32(c,IPB+16), c->busRead32(c,IPB+20), c->busRead32(c,IPB+24), c->busRead32(c,IPB+28)); printf("step %ld ft=%u watch %08X -> %08X  armpc=%08X r9=%08X  nes=%04X\n", k, c->busRead32(c, FT), plb, lb, cpu->gprs[15], cpu->gprs[9], (cpu->gprs[9]-lb)&0xFFFF);
			if (0) {
				printf(" last steps (armpc r9 lb):\n");
				for (int j = hn - 40; j < hn; j++) if (j >= 0) printf("  %08X %08X %08X nes=%04X\n", hist[j&63][0], hist[j&63][1], hist[j&63][2], (hist[j&63][1]-hist[j&63][2])&0xFFFF);
				return 0;
			}
			plb = lb;
		}
	}
	return 0;
}
