/* gbaplay PLAY.gba NFRAMES -- runs a play ROM with input keyed to GBA frames (not NES frames:
 * those stop while PocketVT's menu is open), for driving the L+R menu, savestates and exit.
 *   KEYS="first-last:mask,..."  GBA keys by GBA frame (A 1, B 2, Select 4, Start 8, Right 16,
 *                               Left 32, Up 64, Down 128, R 256, L 512)
 *   KEYSN="first-last:mask,..." (with FT) the same keyed to NES frames (frametotal): the game
 *                               then plays the same on a faster or slower core (speed A/B)
 *   SHOTS="f,f,..." OUT=prefix  writes prefix_fNNNNN.ppm (240x160) after those GBA frames
 *   PEEK="addr[:n],..."         at every shot prints words (or n bytes) at those addresses
 *   FT=<frametotal addr>        prints frametotal with every shot (NES frames run so far)
 *   WATCH="addr,..." STEPFROM=f single-steps from GBA frame f on and reports each first arrival
 *                               of the ARM pc at a watched address, with REG_WAITCNT and IME
 *   BIOS=gba_bios.bin           boots through the real BIOS (reference/gba_bios.bin)
 *   PEEK6502="addr:n,..." MM=<_memmap_tbl>  at every shot also prints n bytes of 6502 address
 *                               space through PocketVT's memory map (8 entries of 8K)
 *   SRAMIN=file SRAMOUT=file    loads cart SRAM (64K) from a file at power-on / writes it at the
 *                               end: a savestate made in one run, loaded in another after power-on
 *   FTDUMP="n,n,..." DUMP="addr:len,..."  (with FT) each time frametotal arrives at n, writes
 *                               prefix_ft<n>_<k>.bin (the DUMP ranges, at the instruction that
 *                               moved frametotal) and .ppm (that GBA frame), k counting the
 *                               arrivals: a loaded savestate takes frametotal back, so k=1 is the
 *                               replay of k=0 and the two files should be identical
 * Every 60 frames (and at the end) it prints the ARM pc and frametotal, so a crash or a hang
 * shows as a pc outside the core or a frametotal that stops. */
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
#include <string.h>
#include <stdint.h>
static void nolog(struct mLogger *l, int cat, enum mLogLevel lv, const char *fmt, va_list ap) {(void)l;(void)cat;(void)lv;(void)fmt;(void)ap;}
static struct mLogger q = { .log = nolog };
static int in_list(const char *s, int f) {
	if (!s) return 0;
	while (*s) { char *e; long v = strtol(s, &e, 0); if (e == s) break; if (v == f) return 1; s = e; if (*s == ',') s++; }
	return 0;
}
static unsigned keys_at(const char *ks, unsigned f) {
	unsigned keys = 0; if (!ks) return 0;
	while (*ks) { unsigned f0, f1, m; int used;
		if (sscanf(ks, "%u-%u:%u%n", &f0, &f1, &m, &used) != 3) break;
		if (f >= f0 && f <= f1) keys |= m;
		ks += used; if (*ks == ',') ks++; }
	return keys;
}
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: gbaplay PLAY.gba NFRAMES (env: KEYS SHOTS OUT PEEK FT WATCH STEPFROM BIOS)\n"); return 2; }
	mLogSetDefaultLogger(&q);
	struct mCore *c = GBACoreCreate(); c->init(c); mCoreInitConfig(c, NULL);
	const char *bios = getenv("BIOS");
	if (bios) { mCoreConfigSetValue(&c->config, "useBios", "1"); mCoreConfigSetValue(&c->config, "skipBios", "0"); mCoreLoadConfig(c); }
	if (!mCoreLoadFile(c, argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
	if (bios) {
		struct VFile *vf = VFileOpen(bios, O_RDONLY);
		if (!vf || !c->loadBIOS(c, vf, 0)) { fprintf(stderr, "cannot load BIOS\n"); return 1; }
		c->opts.useBios = true; c->opts.skipBios = false;
	}
	unsigned w, h; c->desiredVideoDimensions(c, &w, &h);
	static uint32_t buf[256*256]; c->setVideoBuffer(c, (void*)buf, w);
	c->reset(c);
	struct ARMCore *cpu = c->cpu;
	int n = atoi(argv[2]);
	if (getenv("SRAMIN")) {
		FILE *si = fopen(getenv("SRAMIN"), "rb");
		if (!si) { fprintf(stderr, "cannot read %s\n", getenv("SRAMIN")); return 1; }
		for (unsigned i = 0; i < 0x10000; i++) { int b = fgetc(si); if (b < 0) break; c->busWrite8(c, 0x0E000000 + i, (uint8_t)b); }
		fclose(si);
	}
	const char *ks = getenv("KEYS"), *shots = getenv("SHOTS"), *out = getenv("OUT") ? getenv("OUT") : "shot";
	const char *peek = getenv("PEEK"), *watch = getenv("WATCH");
	unsigned fta = getenv("FT") ? strtoul(getenv("FT"), 0, 16) : 0;
	int stepfrom = getenv("STEPFROM") ? atoi(getenv("STEPFROM")) : -1;
	unsigned wa[16]; int nw = 0, seen[16] = {0};
	const char *ftdump = getenv("FTDUMP"), *dumps = getenv("DUMP");
	int arrivals[64] = {0};
	for (const char *p = watch; p && *p && nw < 16; ) { char *e; wa[nw++] = strtoul(p, &e, 16) & ~1u; p = e; if (*p == ',') p++; }
	for (int f = 1; f <= n; f++) {
		c->setKeys(c, keys_at(ks, f) | (fta ? keys_at(getenv("KEYSN"), c->busRead32(c, fta)) : 0));
		/* FTDUMP: the state is dumped at the instruction that moves frametotal (single-
		 * stepping this GBA frame), so both passes are sampled at the same point of the
		 * NES frame; a GBA frame boundary falls anywhere in it, and a raster-split game's
		 * registers change within the frame.  The picture follows at the frame's end.
		 * Up to 3 NES frames ahead: frame pacing can run two in one GBA frame. */
		int dumped_k = -1; unsigned dumped_ft = 0;
		unsigned ft0 = (fta && ftdump) ? c->busRead32(c, fta) : 0;
		if (fta && ftdump && (in_list(ftdump, (int)ft0 + 1) || in_list(ftdump, (int)ft0 + 2) || in_list(ftdump, (int)ft0 + 3))) {
			uint32_t fc = c->frameCounter(c);
			for (long k = 0; c->frameCounter(c) == fc && k < 4000000; k++) {
				c->step(c);
				unsigned ft = c->busRead32(c, fta);
				if (ft == ft0) continue;
				ft0 = ft;
				if (dumped_k >= 0 || !in_list(ftdump, (int)ft)) continue;
				int slot = 0; for (const char *p = ftdump; *p; slot++) { char *e; long v = strtol(p, &e, 0); if (e == p || v == (long)ft) break; p = e; if (*p == ',') p++; }
				dumped_k = slot < 64 ? arrivals[slot]++ : 0; dumped_ft = ft;
				char name[512]; snprintf(name, sizeof name, "%s_ft%u_%d.bin", out, ft, dumped_k);
				FILE *o = fopen(name, "wb");
				for (const char *p = dumps; p && *p; ) { char *e; unsigned a = strtoul(p, &e, 16), n = 4; p = e;
					if (*p == ':') { n = strtoul(p + 1, &e, 0); p = e; }
					for (unsigned i = 0; i < n; i++) fputc(c->busRead8(c, a + i), o);
					if (*p == ',') p++; }
				fclose(o);
				printf("ftdump ft=%u k=%d at frame %d\n", ft, dumped_k, f);
			}
		} else if (nw && stepfrom >= 0 && f >= stepfrom) {
			/* single-step one frame's worth: until the frame counter moves */
			uint32_t fc = c->frameCounter(c);
			for (long k = 0; c->frameCounter(c) == fc && k < 4000000; k++) {
				c->step(c);
				unsigned pc = (cpu->gprs[15] - (cpu->cpsr.t ? 4 : 8)) & ~1u;
				for (int i = 0; i < nw; i++) if (pc == wa[i] && !seen[i]) {
					seen[i] = 1;
					printf("frame %d: pc reached %08X  WAITCNT=%04X IME=%u lr=%08X r0=%08X r1=%08X\n", f, pc,
					       c->busRead16(c, 0x04000204), c->busRead16(c, 0x04000208), cpu->gprs[14], cpu->gprs[0], cpu->gprs[1]);
				}
			}
		} else c->runFrame(c);
		if (dumped_k >= 0) {
			char name[512]; snprintf(name, sizeof name, "%s_ft%u_%d.ppm", out, dumped_ft, dumped_k);
			FILE *o = fopen(name, "wb"); fprintf(o, "P6\n%u %u\n255\n", w, h);
			for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
				uint32_t px = buf[y * w + x]; unsigned char rgb[3] = { px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF };
				fwrite(rgb, 1, 3, o); }
			fclose(o);
		}
		if (in_list(shots, f)) {
			char name[512]; snprintf(name, sizeof name, "%s_f%05d.ppm", out, f);
			FILE *o = fopen(name, "wb"); fprintf(o, "P6\n%u %u\n255\n", w, h);
			for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
				uint32_t p = buf[y * w + x]; unsigned char rgb[3] = { p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF };
				fwrite(rgb, 1, 3, o); }
			fclose(o);
			printf("shot %s pc=%08X", name, cpu->gprs[15]);
			if (fta) printf(" frametotal=%u", c->busRead32(c, fta));
			for (const char *p = peek; p && *p; ) { char *e; unsigned a = strtoul(p, &e, 16), cnt = 0; p = e;
				if (*p == ':') { cnt = strtoul(p + 1, &e, 0); p = e; }
				if (cnt) { printf(" [%08X]=", a); for (unsigned i = 0; i < cnt; i++) printf("%02X", c->busRead8(c, a + i)); }
				else printf(" [%08X]=%08X", a, c->busRead32(c, a));
				if (*p == ',') p++; }
			if (getenv("PEEK6502") && getenv("MM")) {
				unsigned mm = strtoul(getenv("MM"), 0, 16);
				for (const char *p = getenv("PEEK6502"); *p; ) { char *e; unsigned a = strtoul(p, &e, 16), cnt = 16; p = e;
					if (*p == ':') { cnt = strtoul(p + 1, &e, 0); p = e; }
					printf("\n  6502 %04X:", a);
					for (unsigned i = 0; i < cnt; i++) { unsigned x = (a + i) & 0xFFFF; printf(" %02X", c->busRead8(c, c->busRead32(c, mm + 4 * (x >> 13)) + x)); }
					if (*p == ',') p++; }
			}
			printf("\n");
		}
		if (f % 60 == 0 || f == n) {
			printf("frame %d pc=%08X", f, cpu->gprs[15]);
			if (fta) printf(" frametotal=%u", c->busRead32(c, fta));
			printf("\n");
		}
	}
	if (getenv("SRAMOUT")) {
		FILE *so = fopen(getenv("SRAMOUT"), "wb");
		for (unsigned i = 0; i < 0x10000; i++) fputc(c->busRead8(c, 0x0E000000 + i), so);
		fclose(so);
	}
	return 0;
}
