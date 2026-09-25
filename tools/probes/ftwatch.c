/* ftwatch PLAY.gba FRAMETOTAL ADDR T0 T1 [KEYFROM KEYTO KEYMASK]... -- NES-frame keyed input (like pvt_run;
 * any number of key spans, OR-ed);
 * from NES frame T0 to T1 single-step and log every change of the u32 at ADDR with the writer's pc and lr. */
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
	unsigned FT = strtoul(argv[2],0,16), A = strtoul(argv[3],0,16), t0 = atoi(argv[4]), t1 = atoi(argv[5]);
	unsigned kf[64], kt[64], km[64], nk = 0;
	for (int a = 6; a + 2 < argc && nk < 64; a += 3, nk++) { kf[nk] = atoi(argv[a]); kt[nk] = atoi(argv[a+1]); km[nk] = atoi(argv[a+2]); }
#define KEYS(ft) ({ unsigned _m = 0; for (unsigned _i = 0; _i < nk; _i++) if ((ft) >= kf[_i] && (ft) <= kt[_i]) _m |= km[_i]; _m; })
	struct ARMCore *cpu = c->cpu;
	for (int i = 0; i < 20000 && c->busRead32(c, FT) < t0; i++) { unsigned ft = c->busRead32(c, FT); c->setKeys(c, KEYS(ft)); c->runFrame(c); }
	unsigned pv = c->busRead32(c, A);
	printf("start ft=%u [%08X]=%08X\n", c->busRead32(c, FT), A, pv);
	for (long k = 0; k < 400000000L; k++) {
		unsigned ft = c->busRead32(c, FT);
		if (ft >= t1) break;
		c->setKeys(c, KEYS(ft));
		c->step(c);
		unsigned v = c->busRead32(c, A);
		if (v != pv) { printf("ft=%u [%08X] %08X -> %08X pc=%08X lr=%08X\n", ft, A, pv, v, cpu->gprs[15], cpu->gprs[14]); pv = v; }
	}
	return 0;
}
