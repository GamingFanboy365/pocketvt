/* pvt_run -- the PocketVT side of compare_furb.py (libmgba, headless).
 *
 *   pvt_run PLAY.gba FRAMETOTAL DMA0BUFF OUTPREFIX TARGETS SCRIPT [MAXGBA]
 *
 * FRAMETOTAL / DMA0BUFF: hex addresses of the `frametotal` and `_dma0buff`
 * symbols (read them from pocketvt.elf with nm -- they move every link).
 * frametotal is PocketVT's count of emulated NES frames, so input and
 * captures are keyed to NES frames, not GBA frames: a cart PocketVT runs at
 * 39/60 still sees Start on the same NES frame furb_cli does.
 *
 * TARGETS: comma-separated NES frame numbers.  For target t the GBA frame is
 * captured as soon as frametotal >= t+1 (NES frame t has completed):
 *   OUTPREFIX_tNNNN.rgb   240x160 32-bit pixels as mGBA renders them
 *   OUTPREFIX_tNNNN.geom  160 lines "hofs vofs" from dma0buff (row mapping)
 * SCRIPT: file of lines "FIRST LAST KEYMASK" (NES frames, GBA key bits:
 *   A=1 B=2 Select=4 Start=8 Right=16 Left=32 Up=64 Down=128).
 * Also writes OUTPREFIX_timeline.txt: "gba_frame nes_frames" per GBA frame. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct span { unsigned first, last, keys; };

static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {
	(void)l; (void)cat; (void)lv; (void)fmt; (void)ap;
}
static struct mLogger quiet_logger = { .log = nolog };

int main(int argc, char **argv) {
	if (argc < 7) {
		fprintf(stderr, "usage: pvt_run PLAY.gba FRAMETOTAL DMA0BUFF OUTPREFIX TARGETS SCRIPT [MAXGBA]\n");
		return 2;
	}
	unsigned ft = strtoul(argv[2], 0, 16), d0 = strtoul(argv[3], 0, 16);
	const char *out = argv[4];
	unsigned targets[4096], nt = 0;
	for (char *p = argv[5]; *p && nt < 4096;) {
		targets[nt++] = strtoul(p, &p, 10);
		if (*p == ',') p++;
	}
	struct span spans[4096];
	unsigned ns = 0;
	FILE *s = fopen(argv[6], "r");
	if (!s) { perror(argv[6]); return 1; }
	while (ns < 4096 && fscanf(s, "%u %u %u", &spans[ns].first, &spans[ns].last, &spans[ns].keys) == 3) ns++;
	fclose(s);
	unsigned maxt = 0;
	for (unsigned i = 0; i < nt; i++) if (targets[i] > maxt) maxt = targets[i];
	unsigned maxgba = argc > 7 ? strtoul(argv[7], 0, 10) : (maxt + 2) * 4 + 600;

	mLogSetDefaultLogger(&quiet_logger);
	struct mCore *c = GBACoreCreate();
	c->init(c);
	mCoreInitConfig(c, NULL);
	if (!mCoreLoadFile(c, argv[1])) { fprintf(stderr, "pvt_run: cannot load %s\n", argv[1]); return 1; }
	unsigned w, h;
	c->desiredVideoDimensions(c, &w, &h);
	uint32_t *v = malloc(w * h * 4);
	c->setVideoBuffer(c, v, w);
	c->reset(c);

	char name[1024];
	snprintf(name, sizeof name, "%s_timeline.txt", out);
	FILE *tl = fopen(name, "w");
	unsigned done = 0;
	char captured[4096] = {0};
	for (unsigned g = 0; g < maxgba && done < nt; g++) {
		unsigned nf = c->busRead32(c, ft), keys = 0;
		for (unsigned i = 0; i < ns; i++)
			if (nf >= spans[i].first && nf <= spans[i].last) keys |= spans[i].keys;
		c->setKeys(c, keys);
		c->runFrame(c);
		nf = c->busRead32(c, ft);
		fprintf(tl, "%u %u\n", g, nf);
		for (unsigned i = 0; i < nt; i++) {
			if (captured[i] || nf < targets[i] + 1) continue;
			captured[i] = 1;
			done++;
			snprintf(name, sizeof name, "%s_t%04u.rgb", out, targets[i]);
			FILE *f = fopen(name, "wb");
			fwrite(v, 4, w * h, f);
			fclose(f);
			unsigned dp = c->busRead32(c, d0);
			snprintf(name, sizeof name, "%s_t%04u.geom", out, targets[i]);
			f = fopen(name, "w");
			for (unsigned y = 0; y < h; y++) {
				uint32_t e = c->busRead32(c, dp + y * 4);
				fprintf(f, "%d %d\n", (int)(short)(e & 0xFFFF), (int)(short)(e >> 16));
			}
			fclose(f);
		}
	}
	fclose(tl);
	if (done < nt) {
		fprintf(stderr, "pvt_run: only %u of %u targets reached (NES frame counter stuck? core-only ROM?)\n", done, nt);
		return 1;
	}
	return 0;
}
