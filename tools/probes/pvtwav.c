/* pvtwav PLAY.gba FRAMETOTAL OUT.wav T0 T1 [FROM TO KEYMASK]...
 * Records what the GBA plays (mGBA's mixed output, mono 16-bit 32768 Hz) from
 * NES frame T0 to T1, with NES-frame-keyed input like pvt_run / ftwatch.
 * Pair it with `furb_cli ROM --frames T1 --wav ref.wav` (same input) and
 * tools/probes/wavcmp.py.  Audio is in GBA time: a cart PocketVT runs below
 * 60 NES fps plays proportionally slower, as it does on hardware. */
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
static void u32le(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
int main(int argc, char **argv) {
	if (argc < 6) { fprintf(stderr, "usage: pvtwav PLAY.gba FRAMETOTAL OUT.wav T0 T1 [FROM TO KEYMASK]...\n"); return 2; }
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) return 1;
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static unsigned buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->setAudioBufferSize(c, 4096);
	const int RATE = 32768;
	blip_t *L = c->getAudioChannel(c, 0), *R = c->getAudioChannel(c, 1);
	blip_set_rates(L, c->frequency(c), RATE); blip_set_rates(R, c->frequency(c), RATE);
	c->reset(c);
	unsigned FT = strtoul(argv[2], 0, 16), t0 = atoi(argv[4]), t1 = atoi(argv[5]);
	unsigned kf[64], kt[64], km[64], nk = 0;
	for (int a = 6; a + 2 < argc && nk < 64; a += 3, nk++) { kf[nk] = atoi(argv[a]); kt[nk] = atoi(argv[a+1]); km[nk] = atoi(argv[a+2]); }
	FILE *f = fopen(argv[3], "wb");
	fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f); u32le(f, 16);
	fputc(1, f); fputc(0, f); fputc(1, f); fputc(0, f); u32le(f, RATE); u32le(f, RATE * 2); fputc(2, f); fputc(0, f); fputc(16, f); fputc(0, f);
	fwrite("data\0\0\0\0", 1, 8, f);
	unsigned bytes = 0; short l[8192], r[8192];
	for (int g = 0; g < 400000; g++) {
		unsigned ft = c->busRead32(c, FT), m = 0;
		if (ft >= t1) break;
		for (unsigned i = 0; i < nk; i++) if (ft >= kf[i] && ft <= kt[i]) m |= km[i];
		c->setKeys(c, m); c->runFrame(c);
		int n = blip_samples_avail(L); if (n > 8192) n = 8192;
		blip_read_samples(L, l, n, 0); blip_read_samples(R, r, n, 0);
		if (ft < t0) continue;
		for (int i = 0; i < n; i++) { int s = (l[i] + r[i]) / 2; fputc(s & 0xFF, f); fputc((s >> 8) & 0xFF, f); bytes += 2; }
	}
	fseek(f, 4, SEEK_SET); u32le(f, 36 + bytes); fseek(f, 40, SEEK_SET); u32le(f, bytes); fclose(f);
	fprintf(stderr, "pvtwav: %u samples (%.2f s)\n", bytes / 2, bytes / 2.0 / RATE);
	return 0;
}
