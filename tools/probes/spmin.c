/* spmin PLAY.gba FROMFRAME NSTEPS ADDR -- lowest sp per CPU mode (0x1F = System, the user stack) over NSTEPS steps, and every change of the u32 at ADDR */
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
	int gf = atoi(argv[2]); long ns = atol(argv[3]); unsigned A = strtoul(argv[4],0,16);
	struct ARMCore *cpu = c->cpu;
	for (int i = 1; i < gf; i++) { c->setKeys(c, 0); c->runFrame(c); }
	unsigned mnm[32]; unsigned mnpcm[32]; for (int m=0;m<32;m++){mnm[m]=0xFFFFFFFF;mnpcm[m]=0;}
	unsigned pv = c->busRead32(c, A);
	for (long k = 0; k < ns; k++) {
		c->step(c);
		unsigned sp = cpu->gprs[13];
		{ int m = cpu->cpsr.priv & 31; if ((sp>>24)==3 && sp < mnm[m]) { mnm[m] = sp; mnpcm[m] = cpu->gprs[15]; } }
		unsigned v = c->busRead32(c, A);
		if (v != pv) { printf("step %ld: [%08X] %08X -> %08X  pc=%08X sp=%08X\n", k, A, pv, v, cpu->gprs[15], sp); pv = v; }
	}
	for (int m=0;m<32;m++) if (mnm[m]!=0xFFFFFFFF) printf("mode %02X: min sp %08X at pc %08X\n", m, mnm[m], mnpcm[m]);
	return 0;
}
