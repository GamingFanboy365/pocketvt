// furb_cli -- headless Furbtendulator: run a .nes for N frames with a scripted
// pad, dump chosen frames as PPM (exactly what Furbtendulator's own screenshot
// would contain) plus the VT register file / palette RAM / CPU RAM, and print
// per-frame hashes.  The reference side of tools/compare_furb.py.
//
//   furb_cli ROM.nes [--frames N] [--dump F,F,...] [--every K] [--out PREFIX]
//            [--input "200-205:Start;400-620:Up;..."] [--port 1|2]
//            [--set Setting=Value ...] [--hashes] [--quiet]
//
// Input script: ';'-separated "FIRST-LAST:BUTTONS" (or "FRAME:BUTTONS"),
// BUTTONS = '+'-joined A B Select Start Up Down Left Right, optionally
// prefixed "p1:" / "p2:" (default port from --port, default 1).  Frames are
// 0-based: frame n is the n-th frame completed after power-on, so
// "--dump 700" matches a PocketVT harness that dumps after 700 runFrame calls.
#include "StdAfx.h"
#include "Nintendulator.h"
#include "Settings.h"
#include "MapperInterface.h"
#include "NES.h"
#include "Sound.h"
#include "APU.h"
#include "CPU.h"
#include "PPU.h"
#include "GFX.h"
#include "Controllers.h"
#include "OneBus.h"
#include <locale.h>
#include <sys/types.h>
#include <limits.h>
#include <set>
#include <map>
#include <string>
#include <vector>

namespace GFX { extern int OFFSETX, OFFSETY; }
extern "C" ssize_t readlink(const char *, char *, size_t);	// <unistd.h> clashes with Furb names

static bool quiet = false;

// ---------------------------------------------------------------- input script
struct PadSpan { int first, last, port; unsigned char bits; };
static std::vector<PadSpan> script;

static unsigned char button_bit(const std::string &b) {
	static const char *names[8] = {"a", "b", "select", "start", "up", "down", "left", "right"};
	std::string l;
	for (char c : b) l += (char)tolower((unsigned char)c);
	for (int i = 0; i < 8; i++) if (l == names[i]) return (unsigned char)(1 << i);
	if (l == "sel") return 1 << 2;
	if (l == "st") return 1 << 3;
	if (l == "u") return 1 << 4;
	if (l == "d") return 1 << 5;
	if (l == "l") return 1 << 6;
	if (l == "r") return 1 << 7;
	if (l.empty() || l == "none") return 0;
	fprintf(stderr, "furb_cli: unknown button '%s'\n", b.c_str());
	exit(2);
}

static void parse_script(const std::string &s, int default_port) {
	size_t pos = 0;
	while (pos < s.size()) {
		size_t end = s.find(';', pos);
		std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? s.size() : end + 1;
		if (item.empty()) continue;
		size_t colon = item.find(':');
		if (colon == std::string::npos) { fprintf(stderr, "furb_cli: bad input item '%s'\n", item.c_str()); exit(2); }
		std::string range = item.substr(0, colon), rest = item.substr(colon + 1);
		PadSpan sp;
		sp.port = default_port;
		if (rest.size() > 3 && (rest[0] == 'p' || rest[0] == 'P') && (rest[1] == '1' || rest[1] == '2') && rest[2] == ':') {
			sp.port = rest[1] - '0';
			rest = rest.substr(3);
		}
		size_t dash = range.find('-');
		sp.first = atoi(range.c_str());
		sp.last = dash == std::string::npos ? sp.first : atoi(range.c_str() + dash + 1);
		sp.bits = 0;
		size_t b = 0;
		while (b <= rest.size()) {
			size_t plus = rest.find('+', b);
			sp.bits |= button_bit(rest.substr(b, plus == std::string::npos ? std::string::npos : plus - b));
			if (plus == std::string::npos) break;
			b = plus + 1;
		}
		script.push_back(sp);
	}
}

// Port1 buttons -> DirectInput key codes 0x10-0x17, Port2 -> 0x18-0x1F; the
// CLI owns Controllers::KeyState (no DirectInput device ever overwrites it).
namespace Controllers { extern BYTE KeyState[256]; }
static void map_buttons(void) {
	for (int i = 0; i < 8; i++) {
		Controllers::Port1_Buttons[i] = 0x10 + i;
		Controllers::Port2_Buttons[i] = 0x18 + i;
	}
	for (int i = 8; i < CONTROLLERS_MAXBUTTONS; i++)
		Controllers::Port1_Buttons[i] = Controllers::Port2_Buttons[i] = 0xFF;	// turbo etc: never pressed
	Controllers::Port1->SetMasks();
	Controllers::Port2->SetMasks();
}
static void apply_input(int frame) {
	unsigned char bits[3] = {0, 0, 0};
	for (const PadSpan &sp : script)
		if (frame >= sp.first && frame <= sp.last) bits[sp.port] |= sp.bits;
	for (int i = 0; i < 8; i++) {
		Controllers::KeyState[0x10 + i] = (bits[1] >> i & 1) ? 0x80 : 0;
		Controllers::KeyState[0x18 + i] = (bits[2] >> i & 1) ? 0x80 : 0;
	}
}

// ---------------------------------------------------------------- settings
struct SettingRef { const char *name; int *p; };
static SettingRef settings[] = {
	{"VT03Palette", &Settings::VT03Palette},
	{"NoSpriteLimit", &Settings::NoSpriteLimit},
	{"RAMInitialization", &Settings::RAMInitialization},
	{"DefaultRegion", (int *)&Settings::DefaultRegion},
	{"VT369SoundHLE", &Settings::VT369SoundHLE},
	{"VT32RemoveBlueishCast", &Settings::VT32RemoveBlueishCast},
	{"DisableEmphasis", &Settings::DisableEmphasis},
	{"PPUNeverClip", &Settings::PPUNeverClip},
	{"ScrollGlitch", &Settings::ScrollGlitch},
	{"Xstart", &Settings::Xstart}, {"Xend", &Settings::Xend},
	{"Ystart", &Settings::Ystart}, {"Yend", &Settings::Yend},
	{"NTSCto709", &Settings::NTSCto709}, {"NTSCSetup", &Settings::NTSCSetup},
	{"NTSCHue", &Settings::NTSCHue}, {"NTSCSaturation", &Settings::NTSCSaturation},
	{"RGBsRGB", &Settings::RGBsRGB},
};

// ---------------------------------------------------------------- output
static unsigned long frame_hash(void) {
	// FNV-1a over the visible palette-index frame (same region the dump uses)
	uint32_t h = 2166136261u;
	const uint16_t *src = PPU::PPU[0]->DrawArray;
	for (int y = 0; y < GFX::SIZEY; y++)
		for (int x = 0; x < GFX::SIZEX; x++) {
			uint32_t rgb = GFX::Palette32[src[(y + GFX::OFFSETY) * 341 + x + GFX::OFFSETX]];
			for (int k = 0; k < 3; k++) { h ^= (rgb >> (8 * k)) & 0xFF; h *= 16777619u; }
		}
	return h;
}

static void dump_frame(const std::string &prefix, int frame) {
	char name[PATH_MAX];
	int w = GFX::SIZEX, h = GFX::SIZEY;
	const uint16_t *src = PPU::PPU[0]->DrawArray;
	snprintf(name, sizeof name, "%s_f%04d.ppm", prefix.c_str(), frame);
	FILE *f = fopen(name, "wb");
	if (!f) { perror(name); exit(1); }
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t rgb = GFX::Palette32[src[(y + GFX::OFFSETY) * 341 + x + GFX::OFFSETX]];
			unsigned char px[3] = {(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb};
			fwrite(px, 1, 3, f);
		}
	fclose(f);

	// raw palette-index frame (uint16 LE, w*h): the PPU's own colour indices
	snprintf(name, sizeof name, "%s_f%04d.idx", prefix.c_str(), frame);
	f = fopen(name, "wb");
	for (int y = 0; y < h; y++) fwrite(src + (y + GFX::OFFSETY) * 341 + GFX::OFFSETX, 2, w, f);
	fclose(f);

	// registers, palette RAM, CPU RAM
	snprintf(name, sizeof name, "%s_f%04d.txt", prefix.c_str(), frame);
	f = fopen(name, "w");
	fprintf(f, "frame %d  size %dx%d  offset %d,%d  console %d  mapper %d.%d\n", frame, w, h,
		GFX::OFFSETX, GFX::OFFSETY, (int)RI.ConsoleType, (int)RI.INES_MapperNum, (int)RI.INES2_SubMapper);
	fprintf(f, "reg2000 ($2000-$20FF):\n");
	for (int i = 0; i < 0x100; i++) fprintf(f, "%02X%s", reg2000[i], (i & 15) == 15 ? "\n" : " ");
	fprintf(f, "reg4100 ($4100-$41FF):\n");
	for (int i = 0; i < 0x100; i++) fprintf(f, "%02X%s", reg4100[i], (i & 15) == 15 ? "\n" : " ");
	fprintf(f, "palette RAM (PPU::Palette[0..255]):\n");
	for (int i = 0; i < 0x100; i++) fprintf(f, "%02X%s", PPU::PPU[0]->Palette[i], (i & 15) == 15 ? "\n" : " ");
	fclose(f);
	snprintf(name, sizeof name, "%s_f%04d.ram", prefix.c_str(), frame);
	f = fopen(name, "wb");
	fwrite(NES::CPU_RAM, 1, sizeof(NES::CPU_RAM), f);
	fclose(f);
	if (!quiet) fprintf(stderr, "furb_cli: dumped frame %d -> %s_f%04d.{ppm,idx,txt,ram}\n", frame, prefix.c_str(), frame);
}

static void usage(void) {
	fprintf(stderr,
		"usage: furb_cli ROM.nes [--frames N] [--dump F,F,..] [--every K] [--out PREFIX]\n"
		"                [--input \"200-205:Start;400-620:p2:Up\"] [--port 1|2]\n"
		"                [--set Setting=Value]... [--hashes] [--quiet]\n"
		"settings:");
	for (auto &s : settings) fprintf(stderr, " %s", s.name);
	fprintf(stderr, "\n");
	exit(2);
}

int main(int argc, char **argv) {
	setlocale(LC_ALL, "C.UTF-8");
	if (argc < 2) usage();
	std::string rom, prefix = "furb";
	int frames = 600, every = 0, port = 1;
	bool hashes = false;
	std::set<int> dumps;
	std::string input;
	std::vector<std::pair<std::string, int>> sets;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char * { if (i + 1 >= argc) usage(); return argv[++i]; };
		if (a == "--frames") frames = atoi(next());
		else if (a == "--every") every = atoi(next());
		else if (a == "--out") prefix = next();
		else if (a == "--input") input += std::string(next()) + ";";
		else if (a == "--port") port = atoi(next());
		else if (a == "--hashes") hashes = true;
		else if (a == "--quiet") quiet = true;
		else if (a == "--dump") {
			std::string l = next();
			for (size_t p = 0; p < l.size();) {
				dumps.insert(atoi(l.c_str() + p));
				size_t c = l.find(',', p);
				if (c == std::string::npos) break;
				p = c + 1;
			}
		} else if (a == "--set") {
			std::string kv = next();
			size_t eq = kv.find('=');
			if (eq == std::string::npos) usage();
			sets.push_back({kv.substr(0, eq), atoi(kv.c_str() + eq + 1)});
		} else if (a[0] == '-') usage();
		else rom = a;
	}
	if (rom.empty()) usage();
	if (port != 1 && port != 2) usage();
	parse_script(input, port);

	// ProgPath = directory holding Mappers/iNES.so (next to this binary)
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	exe[n > 0 ? n : 0] = 0;
	std::string dir = exe;
	dir = dir.substr(0, dir.find_last_of('/') + 1);
	wcscpy(ProgPath, furb_widen(dir.c_str()).c_str());
	wcscpy(DataPath, furb_widen("/tmp/").c_str());

	NES::Init();
	Settings::AutoRun = FALSE;	// the CLI drives frames itself; no emulation thread
	Settings::SoundEnabled = FALSE;
	Settings::FSkip = 0;		// render every frame (FSkip>0 makes the PPU skip drawing); presenting is a no-op without DirectDraw
	Settings::aFSkip = FALSE;
	for (auto &kv : sets) {
		bool found = false;
		for (auto &s : settings)
			if (kv.first == s.name) { *s.p = kv.second; found = true; }
		if (!found) { fprintf(stderr, "furb_cli: unknown setting %s\n", kv.first.c_str()); usage(); }
	}

	std::wstring wrom = furb_widen(rom.c_str());
	NES::OpenFile(&wrom[0]);
	if (!NES::ROMLoaded || !PPU::PPU[0]) {
		fprintf(stderr, "furb_cli: failed to load %s\n", rom.c_str());
		return 1;
	}
	// OpenFile's GFX::Start() computed the visible window from Settings
	if (!quiet)
		fprintf(stderr, "furb_cli: %s  mapper %d.%d  console %d  PRG %u KB  CHR %u KB  view %dx%d\n",
			rom.c_str(), (int)RI.INES_MapperNum, (int)RI.INES2_SubMapper, (int)RI.ConsoleType,
			(unsigned)(RI.INES_PRGSize * 16), (unsigned)(RI.INES_CHRSize * 8), GFX::SIZEX, GFX::SIZEY);
	map_buttons();

	// Same loop as NES::Thread, minus the debugger/frame-step/thread handling.
	for (int frame = 0; frame < frames; frame++) {
		apply_input(frame);
		for (;;) {
			if (CPU::CPU[1]) {
				if (CPU::CPU[0]->CycleCount <= CPU::CPU[1]->CycleCount) CPU::CPU[0]->ExecOp();
				else CPU::CPU[1]->ExecOp();
			} else
				CPU::CPU[0]->ExecOp();
			if (!NES::Scanline) continue;
			NES::Scanline = FALSE;
			if (PPU::PPU[0]->SLnum == 240) break;
			if (PPU::PPU[0]->SLnum == (RI.InputType == INPUT_FOURSCORE ? 10 : PPU::PPU[0]->SLStartNMI))
				Controllers::UpdateInput();
		}
		if (hashes) printf("%d %08lx\n", frame, frame_hash());
		if (dumps.count(frame) || (every && frame % every == 0)) dump_frame(prefix, frame);
	}
	return 0;
}
