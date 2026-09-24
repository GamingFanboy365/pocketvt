/* cycprof PLAY.gba ELF_NM_FILE WARMUP_FRAMES NSTEPS -- cycle-weighted profile by function.
 * ELF_NM_FILE: output of `arm-none-eabi-nm -n pocketvt.elf` (sorted).  Each step is charged
 * the GBA cycles it took (mTimingGlobalTime delta), so EWRAM/ROM wait states count. */
#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/arm/arm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
#define MAXS 20000
#define HB (1<<16)
static unsigned bk[HB]; static unsigned long long bc[HB];
static unsigned sa[MAXS]; static char sn[MAXS][48]; static unsigned long long sc[MAXS]; static int ns;
static int cmpc(const void *a, const void *b) { unsigned long long x = sc[*(int*)a], y = sc[*(int*)b]; return x < y ? 1 : x > y ? -1 : 0; }
int main(int argc, char **argv) {
	mLogSetDefaultLogger(&q);
	FILE *f = fopen(argv[2], "r"); char line[256];
	while (ns < MAXS && fgets(line, sizeof line, f)) {
		unsigned a; char t; char n[128];
		if (sscanf(line, "%x %c %127s", &a, &t, n) == 3 && (t=='T'||t=='t') && strncmp(n, "$", 1) && strncmp(n, ".L", 2)) {
			sa[ns] = a; snprintf(sn[ns], 48, "%.47s", n); ns++;
		}
	}
	fclose(f);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int warm = atoi(argv[3]); long N = atol(argv[4]);
	for (int i = 0; i < warm; i++) { c->setKeys(c, 0); c->runFrame(c); }
	struct ARMCore *cpu = c->cpu;
	unsigned FTA = getenv("FT") ? strtoul(getenv("FT"), 0, 16) : 0; unsigned ft0 = FTA ? c->busRead32(c, FTA) : 0;
	unsigned long long other = 0, total = 0; uint64_t t0 = mTimingGlobalTime(c->timing);
	for (long i = 0; i < N; i++) {
		unsigned pc = (cpu->gprs[15] - (cpu->cpsr.t ? 4 : 8)) & ~1u;
		c->step(c);
		uint64_t t1 = mTimingGlobalTime(c->timing); unsigned long long d = t1 - t0; t0 = t1; total += d;
		int lo = 0, hi = ns - 1, k = -1;
		while (lo <= hi) { int m = (lo + hi) / 2; if (sa[m] <= pc) { k = m; lo = m + 1; } else hi = m - 1; }
		if (k >= 0 && pc - sa[k] < 0x4000) sc[k] += d; else other += d;
		{ unsigned b = pc & ~0xFu, hh = (b * 2654435761u) >> 16; while (bc[hh] && bk[hh] != b) hh = (hh + 1) & (HB - 1); bk[hh] = b; bc[hh] += d; }
	}
	static int idx[MAXS]; for (int i = 0; i < ns; i++) idx[i] = i;
	qsort(idx, ns, sizeof(int), cmpc);
	unsigned nf = FTA ? c->busRead32(c, FTA) - ft0 : 0;
	printf("total cycles %llu (%.1f GBA frames) NES frames %u\n", total, total / 280896.0, nf);
	if (getenv("ABS")) { for (int i = 0; i < ns; i++) if (sc[i]) printf("abs %llu %s\n", nf ? sc[i] / nf : sc[i], sn[i]); return 0; }
	for (int i = 0; i < 25 && sc[idx[i]]; i++) printf("%6.2f%%  %08X %s\n", 100.0 * sc[idx[i]] / total, sa[idx[i]], sn[idx[i]]);
	printf("%6.2f%%  (unresolved)\n", 100.0 * other / total);
	if (getenv("RANGE")) {   /* every 16-byte bin in [lo,hi), e.g. RANGE=0800AF84-0800B300 */
		unsigned lo = strtoul(getenv("RANGE"), 0, 16), hi = strtoul(strchr(getenv("RANGE"), '-') + 1, 0, 16);
		for (int i = 0; i < HB; i++) if (bc[i] && bk[i] >= lo && bk[i] < hi) printf("rbin %08X %llu\n", bk[i], bc[i]);
	}
	if (getenv("BINS")) {   /* top 16-byte bins, for addr2line */
		for (int r = 0; r < 20; r++) { int best = -1; for (int i = 0; i < HB; i++) if (bc[i] && (best < 0 || bc[i] > bc[best])) best = i;
			if (best < 0) break;
			printf("bin %08X %6.2f%%\n", bk[best], 100.0 * bc[best] / total); bc[best] = 0; }
	}
	return 0;
}
