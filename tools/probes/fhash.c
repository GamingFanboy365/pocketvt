/* fhash PLAY.gba N -- prints FNV hash of the framebuffer + palette RAM for each GBA frame 1..N.
 * KEYS="first-last:mask,..." presses keys by GBA frame (A 1, B 2, Select 4, Start 8, Right 16, ...). */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
static unsigned keys_at(const char *ks, unsigned f) {
	unsigned keys = 0; if (!ks) return 0;
	while (*ks) { unsigned f0, f1, m; int used;
		if (sscanf(ks, "%u-%u:%u%n", &f0, &f1, &m, &used) != 3) break;
		if (f >= f0 && f <= f1) keys |= m;
		ks += used; if (*ks == ',') ks++; }
	return keys;
}
int main(int argc, char **argv) {
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static uint32_t buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	int n = atoi(argv[2]);
	for (int i = 1; i <= n; i++) {
		c->setKeys(c, keys_at(getenv("KEYS"), i)); c->runFrame(c);
		uint64_t hsh = 1469598103934665603ULL;
		for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) { hsh ^= buf[y*w+x] & 0xF8F8F8; hsh *= 1099511628211ULL; }
		for (unsigned a = 0x05000000; a < 0x05000400; a += 2) { hsh ^= c->busRead16(c, a); hsh *= 1099511628211ULL; }
		printf("%d %016llx\n", i, (unsigned long long)hsh);
	}
	return 0;
}
