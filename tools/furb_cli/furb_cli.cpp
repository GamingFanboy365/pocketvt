// furb_cli -- headless Furbtendulator.  Runs a ROM (.nes .unf .fds .nsf, Vs.
// System) for N frames with scripted input and commands, and dumps frames,
// audio (WAV), video (AVI), savestates, movies, traces and battery saves.
// Everything goes through Furbtendulator's own code: settings through its
// registry loader, pads/Zapper/keyboards/mice through its controller code,
// sound through its DirectSound mixer, movies through its movie dialogs.
// Run with no arguments for the option list; tools/furb_cli/README.md has more.
#include "StdAfx.h"
#include "Nintendulator.h"
#include "resource.h"
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
#include "OneBus_VT369.h"
#include "States.h"
#include "Movie.h"
#include "Tape.h"
#include "Debugger.h"
#include "DIPSwitch.h"
#include "Cheats.hpp"
#include "plugThruDevice.hpp"
#include "furb_host.h"
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GFX { extern int OFFSETX, OFFSETY; }
namespace Controllers { extern BYTE AxisFlags[MAX_CONTROLLERS]; extern DIJOYSTATE2 JoyState[MAX_CONTROLLERS]; }
namespace Debugger { extern BOOL Logging; extern FILE *LogFile; }
namespace Cheats { extern Game cheatDefinition; extern bool anyCheatsActive; }
namespace NES { extern DIP::Game dipswitchDefinition; }
extern "C" ssize_t readlink(const char *, char *, size_t);	// <unistd.h> clashes with Furb names
extern "C" int mkdir(const char *, mode_t);

static bool quiet = false;
static void die(const std::string &m) { fprintf(stderr, "furb_cli: %s\n", m.c_str()); exit(2); }
static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
static std::wstring W(const std::string &s) { return furb_widen(s.c_str()); }

// ============================================================== controllers
// Port table: the index is also the virtual joystick (DirectInput device
// 2+index) that the port's buttons are mapped to.  Keyboard devices read real
// key codes, so pad buttons must not live in the keyboard's key space.
static const char *port_names[7] = {"port1", "port2", "fs1", "fs2", "fs3", "fs4", "exp"};
static DWORD *port_buttons(int i) {
	using namespace Controllers;
	DWORD *t[7] = {Port1_Buttons, Port2_Buttons, FSPort1_Buttons, FSPort2_Buttons, FSPort3_Buttons, FSPort4_Buttons, PortExp_Buttons};
	return t[i];
}
static Controllers::StdPort **std_port(int i) {
	using namespace Controllers;
	Controllers::StdPort **t[6] = {&Port1, &Port2, &FSPort1, &FSPort2, &FSPort3, &FSPort4};
	return i < 6 ? t[i] : NULL;
}

static const char *std_types[] = {"unconnected", "standard", "zapper", "arkanoid", "powerpad-a", "powerpad-b",
	"fourscore", "snes", "vs-zapper", "snes-mouse", "subor-mouse", "fourscore2", "sudoku", "sudoku2"};
static const char *exp_types[] = {"unconnected", "fami4play", "zapper", "arkanoid", "family-basic-keyboard",
	"subor-keyboard", "family-trainer-a", "family-trainer-b", "tablet", "bandai-hyper-shot", "pec586-keyboard",
	"turbo-file", "bit79-keyboard", "city-patrolman", "moguraa", "konami-hyper-shot", "sharp-c1-cassette",
	"golden-nugget-casino", "golden-keyboard", "hori-4play"};

static void map_buttons(void) {
	for (int p = 0; p < 7; p++)
		for (int i = 0; i < CONTROLLERS_MAXBUTTONS; i++)
			port_buttons(p)[i] = (DWORD)((2 + p) << 16 | i);	// joystick 2+p, button i
	for (int p = 0; p < 6; p++) (*std_port(p))->SetMasks();
	Controllers::PortExp->SetMasks();
	Controllers::AxisFlags[1] = (1 << Controllers::AXIS_X) | (1 << Controllers::AXIS_Y) | (1 << Controllers::AXIS_Z);	// the scripted mouse
}

static void set_device(const std::string &spec) {
	using namespace Controllers;
	size_t eq = spec.find('=');
	if (eq == std::string::npos) die("--device wants PORT=TYPE, e.g. port2=zapper");
	std::string port = lower(spec.substr(0, eq)), type = lower(spec.substr(eq + 1));
	int p = -1;
	for (int i = 0; i < 7; i++) if (port == port_names[i]) p = i;
	if (p < 0) die("unknown port " + port + " (port1 port2 fs1..fs4 exp)");
	if (p == 6) {
		for (int t = 0; t < EXP_MAX; t++)
			if (type == exp_types[t]) {
				ExpPort_SetControllerType(PortExp, (EXPCONT_TYPE)t, PortExp_Buttons);
				// the pads behind a 4-player adapter, as SetDefaultInput connects them
				int from = t == EXP_HORI4PLAY ? 2 : 4, to = t == EXP_FAMI4PLAY || t == EXP_HORI4PLAY ? 6 : 0;
				for (int q = from; q < to; q++)
					if ((*std_port(q))->Type == STD_UNCONNECTED)
						StdPort_SetControllerType(*std_port(q), STD_STDCONTROLLER, port_buttons(q));
				return;
			}
		die("unknown expansion device " + type + " (see --list-devices)");
	}
	for (int t = 0; t < STD_MAX; t++) {
		if (type != std_types[t]) continue;
		if (t == STD_FOURSCORE) {	// as the GUI does: port1+port2 become the adapter, fs1-fs4 pads
			StdPort_SetControllerType(Port1, STD_FOURSCORE, Port1_Buttons);
			StdPort_SetControllerType(Port2, STD_FOURSCORE2, Port2_Buttons);
			StdPort_SetControllerType(FSPort1, STD_STDCONTROLLER, FSPort1_Buttons);
			StdPort_SetControllerType(FSPort2, STD_STDCONTROLLER, FSPort2_Buttons);
			StdPort_SetControllerType(FSPort3, STD_STDCONTROLLER, FSPort3_Buttons);
			StdPort_SetControllerType(FSPort4, STD_STDCONTROLLER, FSPort4_Buttons);
		} else
			StdPort_SetControllerType(*std_port(p), (STDCONT_TYPE)t, port_buttons(p));
		return;
	}
	die("unknown controller " + type + " (see --list-devices)");
}

// "p1".."p4" / "exp" -> port table index, following what is plugged in
static int resolve_pad(const std::string &name) {
	using namespace Controllers;
	if (name == "exp") return 6;
	int n = name[1] - '0';
	bool fourscore = Port1->Type == STD_FOURSCORE || PortExp->Type == EXP_HORI4PLAY || VSDUAL;
	if (fourscore) return 1 + n;			// fs1..fs4
	return n == 1 ? 0 : n == 2 ? 1 : 1 + n;		// port1, port2, fs3, fs4 (Famicom 4-player)
}

// DirectInput key names for key: actions
static const std::map<std::string, int> dik = {
	{"escape", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07}, {"7", 0x08},
	{"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"minus", 0x0C}, {"equals", 0x0D}, {"back", 0x0E}, {"backspace", 0x0E},
	{"tab", 0x0F}, {"q", 0x10}, {"w", 0x11}, {"e", 0x12}, {"r", 0x13}, {"t", 0x14}, {"y", 0x15}, {"u", 0x16},
	{"i", 0x17}, {"o", 0x18}, {"p", 0x19}, {"lbracket", 0x1A}, {"rbracket", 0x1B}, {"return", 0x1C}, {"enter", 0x1C},
	{"lcontrol", 0x1D}, {"a", 0x1E}, {"s", 0x1F}, {"d", 0x20}, {"f", 0x21}, {"g", 0x22}, {"h", 0x23}, {"j", 0x24},
	{"k", 0x25}, {"l", 0x26}, {"semicolon", 0x27}, {"apostrophe", 0x28}, {"grave", 0x29}, {"lshift", 0x2A},
	{"backslash", 0x2B}, {"z", 0x2C}, {"x", 0x2D}, {"c", 0x2E}, {"v", 0x2F}, {"b", 0x30}, {"n", 0x31}, {"m", 0x32},
	{"comma", 0x33}, {"period", 0x34}, {"slash", 0x35}, {"rshift", 0x36}, {"multiply", 0x37}, {"lmenu", 0x38},
	{"lalt", 0x38}, {"space", 0x39}, {"capital", 0x3A}, {"capslock", 0x3A}, {"f1", 0x3B}, {"f2", 0x3C}, {"f3", 0x3D},
	{"f4", 0x3E}, {"f5", 0x3F}, {"f6", 0x40}, {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44},
	{"numlock", 0x45}, {"scroll", 0x46}, {"numpad7", 0x47}, {"numpad8", 0x48}, {"numpad9", 0x49}, {"subtract", 0x4A},
	{"numpad4", 0x4B}, {"numpad5", 0x4C}, {"numpad6", 0x4D}, {"add", 0x4E}, {"numpad1", 0x4F}, {"numpad2", 0x50},
	{"numpad3", 0x51}, {"numpad0", 0x52}, {"decimal", 0x53}, {"f11", 0x57}, {"f12", 0x58}, {"kana", 0x70},
	{"yen", 0x7D}, {"numpadenter", 0x9C}, {"rcontrol", 0x9D}, {"divide", 0xB5}, {"rmenu", 0xB8}, {"ralt", 0xB8},
	{"pause", 0xC5}, {"home", 0xC7}, {"up", 0xC8}, {"prior", 0xC9}, {"pgup", 0xC9}, {"left", 0xCB}, {"right", 0xCD},
	{"end", 0xCF}, {"down", 0xD0}, {"next", 0xD1}, {"pgdn", 0xD1}, {"insert", 0xD2}, {"delete", 0xD3},
};

// ============================================================== input script
struct Action {
	int first, last;
	enum { PAD, KEY, MOUSE, MIC, CMD } kind;
	std::string pad;		// p1..p4 / exp
	uint32_t bits = 0;		// device buttons (bit i = button i)
	std::vector<int> keys;
	int mx = 0, my = 0, mbuttons = 0;
	float mic = 0;
	std::string cmd, arg;
};
static std::vector<Action> script;

static const std::map<std::string, int> pad_names = {
	{"a", 0}, {"b", 1}, {"select", 2}, {"sel", 2}, {"start", 3}, {"st", 3}, {"up", 4}, {"u", 4}, {"down", 5},
	{"d", 5}, {"left", 6}, {"l", 6}, {"right", 7}, {"r", 7}, {"turboa", 8}, {"turbob", 9},
	{"trigger", 0},		// Zapper / light guns: button 0
};

static std::vector<std::string> split(const std::string &s, char c) {
	std::vector<std::string> out;
	size_t p = 0;
	for (;;) {
		size_t q = s.find(c, p);
		out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
		if (q == std::string::npos) return out;
		p = q + 1;
	}
}

static void parse_script(const std::string &s, int default_port) {
	for (std::string item : split(s, ';')) {
		if (item.empty()) continue;
		size_t colon = item.find(':');
		if (colon == std::string::npos) die("bad input item '" + item + "' (want FRAMES:ACTION)");
		std::string range = item.substr(0, colon), rest = item.substr(colon + 1);
		Action a;
		size_t dash = range.find('-');
		a.first = atoi(range.c_str());
		a.last = dash == std::string::npos ? a.first : atoi(range.c_str() + dash + 1);
		std::string lr = lower(rest);
		auto head = [&](const char *h) { size_t n = strlen(h); return lr.compare(0, n, h) == 0 ? rest.substr(n) : std::string("\x01"); };
		std::string arg;
		if ((arg = head("key:")) != "\x01") {
			a.kind = Action::KEY;
			for (auto k : split(lower(arg), '+')) {
				auto it = dik.find(k);
				if (it == dik.end()) die("unknown key '" + k + "'");
				a.keys.push_back(it->second);
			}
		} else if ((arg = head("mouse:")) != "\x01") {
			a.kind = Action::MOUSE;
			auto parts = split(lower(arg), '+');
			if (sscanf(parts[0].c_str(), "%d,%d", &a.mx, &a.my) != 2) die("mouse wants X,Y: '" + item + "'");
			for (size_t i = 1; i < parts.size(); i++) {
				if (parts[i] == "left") a.mbuttons |= 1;
				else if (parts[i] == "right") a.mbuttons |= 2;
				else if (parts[i] == "middle") a.mbuttons |= 4;
				else die("mouse button must be left/right/middle: '" + item + "'");
			}
		} else if ((arg = head("mic:")) != "\x01") {
			a.kind = Action::MIC;
			a.mic = (float)atof(arg.c_str());
		} else if ((arg = head("cmd:")) != "\x01") {
			a.kind = Action::CMD;
			size_t eq = arg.find('=');
			a.cmd = lower(arg.substr(0, eq));
			a.arg = eq == std::string::npos ? "" : arg.substr(eq + 1);
			a.last = a.first;
		} else {
			a.kind = Action::PAD;
			a.pad = "p" + std::to_string(default_port);
			if (lr.size() > 3 && lr[0] == 'p' && lr[1] >= '1' && lr[1] <= '4' && lr[2] == ':') { a.pad = lr.substr(0, 2); rest = rest.substr(3); }
			else if (lr.compare(0, 4, "exp:") == 0) { a.pad = "exp"; rest = rest.substr(4); }
			for (auto b : split(lower(rest), '+')) {
				if (b.empty() || b == "none") continue;
				auto it = pad_names.find(b);
				if (it != pad_names.end()) a.bits |= 1u << it->second;
				else if (b[0] == 'b' && isdigit((unsigned char)b[1]) && atoi(b.c_str() + 1) < CONTROLLERS_MAXBUTTONS)
					a.bits |= 1u << atoi(b.c_str() + 1);
				else die("unknown button '" + b + "' (A B Select Start Up Down Left Right TurboA TurboB, or bN)");
			}
		}
		script.push_back(a);
	}
}

// ============================================================== WAV / AVI
struct Wav {
	FILE *f = NULL; uint32_t bytes = 0;
	void open(const std::string &path, int rate, int channels) {
		f = fopen(path.c_str(), "wb");
		if (!f) die("cannot write " + path);
		uint8_t h[44] = {0};
		memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVEfmt ", 8);
		uint32_t v; uint16_t s;
		v = 16; memcpy(h + 16, &v, 4); s = 1; memcpy(h + 20, &s, 2); s = channels; memcpy(h + 22, &s, 2);
		v = rate; memcpy(h + 24, &v, 4); v = rate * channels * 2; memcpy(h + 28, &v, 4);
		s = channels * 2; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2); memcpy(h + 36, "data", 4);
		fwrite(h, 1, 44, f);
	}
	void write(const std::vector<uint8_t> &d) { if (f && !d.empty()) { fwrite(d.data(), 1, d.size(), f); bytes += d.size(); } }
	void close() {
		if (!f) return;
		uint32_t v = 36 + bytes; fseek(f, 4, SEEK_SET); fwrite(&v, 4, 1, f);
		fseek(f, 40, SEEK_SET); fwrite(&bytes, 4, 1, f);
		fclose(f); f = NULL;
	}
};

// Uncompressed AVI 1.0 (RGB24 + 16-bit PCM), readable by ffmpeg, VLC, etc.
struct Avi {
	FILE *f = NULL; int w = 0, h = 0, channels = 1, rate = 48000;
	uint32_t fps_num = 0, fps_den = 1, frames = 0, audio_bytes = 0, movi_pos = 0;
	long avih_frames = 0, vstrh_len = 0, astrh_len = 0;
	std::vector<uint32_t> idx;	// fourcc, flags, offset, size
	bool full = false;
	void u32(uint32_t v) { fwrite(&v, 4, 1, f); }
	void u16(uint16_t v) { fwrite(&v, 2, 1, f); }
	void cc(const char *s) { fwrite(s, 1, 4, f); }
	void open(const std::string &path, int w_, int h_, uint32_t num, uint32_t den, int rate_, int ch) {
		w = w_; h = h_; fps_num = num; fps_den = den; rate = rate_; channels = ch;
		f = fopen(path.c_str(), "wb");
		if (!f) die("cannot write " + path);
		uint32_t frame_bytes = w * h * 3;
		cc("RIFF"); u32(0); cc("AVI ");
		cc("LIST"); u32(4 + 64 + 12 + 64 + 48 + 12 + 64 + 26); cc("hdrl");
		cc("avih"); u32(56);
		u32((uint32_t)(1000000.0 * den / num)); u32(frame_bytes * 61 + rate * ch * 2); u32(0); u32(0x10 | 0x100);
		avih_frames = ftell(f); u32(0); u32(0); u32(2); u32(frame_bytes); u32(w); u32(h); u32(0); u32(0); u32(0); u32(0);
		cc("LIST"); u32(4 + 64 + 48); cc("strl");
		cc("strh"); u32(56); cc("vids"); cc("DIB "); u32(0); u16(0); u16(0); u32(0); u32(den); u32(num); u32(0);
		vstrh_len = ftell(f); u32(0); u32(frame_bytes); u32(0xFFFFFFFF); u32(0); u16(0); u16(0); u16(w); u16(h);
		cc("strf"); u32(40); u32(40); u32(w); u32(h); u16(1); u16(24); u32(0); u32(frame_bytes); u32(0); u32(0); u32(0); u32(0);
		cc("LIST"); u32(4 + 64 + 26); cc("strl");
		cc("strh"); u32(56); cc("auds"); u32(0); u32(0); u16(0); u16(0); u32(0); u32(ch * 2); u32(rate * ch * 2); u32(0);
		astrh_len = ftell(f); u32(0); u32(rate * ch * 2); u32(0xFFFFFFFF); u32(ch * 2); u16(0); u16(0); u16(0); u16(0);
		cc("strf"); u32(18); u16(1); u16(ch); u32(rate); u32(rate * ch * 2); u16(ch * 2); u16(16); u16(0);
		cc("LIST"); u32(0); movi_pos = ftell(f); cc("movi");
	}
	void chunk(const char *id, const void *d, uint32_t n) {
		if (full || !f) return;
		if (ftell(f) > 0x7F000000L) { full = true; fprintf(stderr, "furb_cli: AVI reached 2 GB, recording stopped\n"); return; }
		uint32_t off = ftell(f) - movi_pos;
		cc(id); u32(n); fwrite(d, 1, n, f);
		if (n & 1) fputc(0, f);
		idx.push_back(*(const uint32_t *)id); idx.push_back(0x10); idx.push_back(off); idx.push_back(n);
	}
	void frame(const std::vector<uint8_t> &rgb, int fw, int fh) {	// rgb: top-down, fw*fh*3
		std::vector<uint8_t> dib(w * h * 3);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const uint8_t *s = &rgb[((y * fh / h) * fw + x * fw / w) * 3];
				uint8_t *d = &dib[((h - 1 - y) * w + x) * 3];
				d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
			}
		chunk("00db", dib.data(), dib.size());
		if (!full) frames++;
	}
	void audio(const std::vector<uint8_t> &pcm) {
		if (pcm.empty()) return;
		chunk("01wb", pcm.data(), pcm.size());
		if (!full) audio_bytes += pcm.size();
	}
	void close() {
		if (!f) return;
		long movi_end = ftell(f);
		cc("idx1"); u32(idx.size() * 4);
		fwrite(idx.data(), 4, idx.size(), f);
		long end = ftell(f);
		fseek(f, 4, SEEK_SET); u32(end - 8);
		fseek(f, movi_pos - 4, SEEK_SET); u32(movi_end - movi_pos);
		fseek(f, avih_frames, SEEK_SET); u32(frames);
		fseek(f, vstrh_len, SEEK_SET); u32(frames);
		fseek(f, astrh_len, SEEK_SET); u32(audio_bytes / (channels * 2));
		fclose(f); f = NULL;
	}
};

// ============================================================== frames
static bool hires(void) { return RI.ConsoleType == CONSOLE_VT369 && (reg2000[0x1C] & 0x04); }

// The visible frame as palette indices -- exactly the region (and, for
// VT369's 2x hi-res mode, the doubled even/odd buffers) that GFX::SaveScreenshot saves.
static std::vector<uint16_t> frame_indices(int &w, int &h) {
	std::vector<uint16_t> out;
	if (hires()) {
		auto *p = dynamic_cast<PPU::PPU_VT369 *>(PPU::PPU[0]);
		w = GFX::SIZEX * 2; h = GFX::SIZEY * 2;
		const uint16_t *even = p->DrawArrayEven + 341 * 2 * GFX::OFFSETY + 2 * GFX::OFFSETX;
		const uint16_t *odd = p->DrawArrayOdd + 341 * 2 * GFX::OFFSETY + 2 * GFX::OFFSETX;
		out.resize(w * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) out[y * w + x] = (y & 1 ? odd : even)[y / 2 * 341 * 2 + x];
	} else {
		w = GFX::SIZEX; h = GFX::SIZEY;
		const uint16_t *src = PPU::PPU[0]->DrawArray;
		out.resize(w * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) out[y * w + x] = src[(y + GFX::OFFSETY) * 341 + x + GFX::OFFSETX];
	}
	return out;
}
static std::vector<uint8_t> to_rgb(const std::vector<uint16_t> &idx) {
	std::vector<uint8_t> rgb(idx.size() * 3);
	for (size_t i = 0; i < idx.size(); i++) {
		uint32_t c = GFX::Palette32[idx[i]];
		rgb[i * 3] = c >> 16; rgb[i * 3 + 1] = c >> 8; rgb[i * 3 + 2] = c;
	}
	return rgb;
}
static unsigned long frame_hash(const std::vector<uint8_t> &rgb) {
	uint32_t h = 2166136261u;
	for (uint8_t b : rgb) { h ^= b; h *= 16777619u; }
	return h;
}

static void dump_frame(const std::string &prefix, int frame, const std::vector<uint16_t> &idx,
                       const std::vector<uint8_t> &rgb, int w, int h) {
	char name[PATH_MAX];
	snprintf(name, sizeof name, "%s_f%04d.ppm", prefix.c_str(), frame);
	FILE *f = fopen(name, "wb");
	if (!f) die(std::string("cannot write ") + name);
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb.data(), 1, rgb.size(), f);
	fclose(f);
	snprintf(name, sizeof name, "%s_f%04d.idx", prefix.c_str(), frame);
	f = fopen(name, "wb");
	fwrite(idx.data(), 2, idx.size(), f);
	fclose(f);
	snprintf(name, sizeof name, "%s_f%04d.txt", prefix.c_str(), frame);
	f = fopen(name, "w");
	fprintf(f, "frame %d  size %dx%d%s  offset %d,%d  console %d  mapper %d.%d\n", frame, w, h, hires() ? " (VT369 hi-res)" : "",
		GFX::OFFSETX, GFX::OFFSETY, (int)RI.ConsoleType, (int)RI.INES_MapperNum, (int)RI.INES2_SubMapper);
	fprintf(f, "reg2000 ($2000-$20FF):\n");
	for (int i = 0; i < 0x100; i++) fprintf(f, "%02X%s", reg2000[i], (i & 15) == 15 ? "\n" : " ");
	fprintf(f, "reg4100 ($4100-$41FF):\n");
	for (int i = 0; i < 0x100; i++) fprintf(f, "%02X%s", reg4100[i], (i & 15) == 15 ? "\n" : " ");
	int pal = RI.ConsoleType == CONSOLE_VT369 ? 1024 : 256;
	fprintf(f, "palette RAM (PPU::Palette[0..%d]):\n", pal - 1);
	for (int i = 0; i < pal; i++) fprintf(f, "%02X%s", PPU::PPU[0]->Palette[i], (i & 15) == 15 ? "\n" : " ");
	fclose(f);
	snprintf(name, sizeof name, "%s_f%04d.ram", prefix.c_str(), frame);
	f = fopen(name, "wb");
	fwrite(NES::CPU_RAM, 1, sizeof(NES::CPU_RAM), f);
	fclose(f);
	if (!quiet) fprintf(stderr, "furb_cli: dumped frame %d -> %s_f%04d.{ppm,idx,txt,ram}\n", frame, prefix.c_str(), frame);
}

// ============================================================== info
static const char *console_names[] = {"NES/Famicom", "Vs. System", "PlayChoice-10", "Bit Corp", "VT01 (B/W)",
	"VT01 (STN)", "VT02", "VT03", "VT09", "VT32", "VT369", "UM6578"};
static void print_info(void) {
	const char *types[] = {"undefined", "iNES", "UNIF", "FDS", "NSF"};
	printf("file:      %s\n", furb_narrow(RI.Filename).c_str());
	printf("type:      %s\n", RI.ROMType < ROM_NUMTYPES ? types[RI.ROMType] : "?");
	if (RI.ROMType == ROM_INES || RI.ROMType == ROM_UNIF)
		printf("mapper:    %d.%d (%s)\n", RI.INES_MapperNum, RI.INES2_SubMapper, MI && MI->Description ? furb_narrow(MI->Description).c_str() : "?");
	if (RI.ROMType == ROM_FDS) printf("disk:      %d side(s)\n", RI.FDS_NumSides);
	if (RI.ROMType == ROM_NSF)
		printf("NSF:       \"%s\" by %s, %d songs (start %d)\n", RI.NSF_Title, RI.NSF_Artist, RI.NSF_NumSongs, RI.NSF_InitSong);
	printf("console:   %s\n", RI.ConsoleType < 12 ? console_names[RI.ConsoleType] : "?");
	printf("region:    %s\n", NES::CurRegion == Settings::REGION_PAL ? "PAL" : NES::CurRegion == Settings::REGION_DENDY ? "Dendy" : "NTSC");
	printf("PRG ROM:   %u bytes  CRC32 %08X\n", (unsigned)RI.PRGROMSize, RI.PRGROMCRC32);
	printf("CHR ROM:   %u bytes  CRC32 %08X\n", (unsigned)RI.CHRROMSize, RI.CHRROMCRC32);
	printf("PRG RAM:   %u bytes  CHR RAM %u bytes\n", (unsigned)RI.PRGRAMSize, (unsigned)RI.CHRRAMSize);
	printf("ROM CRC32: %08X\n", RI.OverallCRC32);
	printf("input:     type %d", RI.InputType);
	for (int p = 0; p < 7; p++) {
		int t = p < 6 ? (*std_port(p))->Type : Controllers::PortExp->Type;
		if (t) printf("  %s=%s", port_names[p], p < 6 ? std_types[t] : exp_types[t]);
	}
	printf("\n");
	const DIP::Game &dip = NES::dipswitchDefinition;
	if (!dip.settings.empty()) {
		printf("DIP:       value 0x%X (\"%s\")\n", RI.dipValue, furb_narrow(dip.name.c_str()).c_str());
		for (auto &s : dip.settings) {
			printf("  %-28s mask 0x%X:", furb_narrow(s.name.c_str()).c_str(), s.mask);
			for (auto &c : s.choices)
				printf("  %s0x%X=%s", (RI.dipValue & s.mask) == c.value ? "*" : "", c.value, furb_narrow(c.name.c_str()).c_str());
			printf("\n");
		}
	}
	if (!Cheats::cheatDefinition.cheats.empty()) {
		printf("cheats (cheats.cfg):\n");
		for (auto &c : Cheats::cheatDefinition.cheats)
			printf("  %s%s\n", c.enabled ? "[on] " : "", furb_narrow(c.name.c_str()).c_str());
	}
}

static void usage(void) {
	fprintf(stderr,
"usage: furb_cli ROM [options]      (ROM: .nes .unf .fds .nsf ...)\n"
"run      --frames N  --dump F,F,..  --every K  --out PREFIX  --hashes  --info  --quiet  --verbose\n"
"input    --input \"FRAMES:ACTION;...\" (repeatable)  --port 1|2 (default pad)\n"
"         ACTION: [p1:..p4:|exp:]A+B+Select+Start+Up+Down+Left+Right+TurboA+TurboB+bN\n"
"                 key:NAME+NAME   mouse:X,Y[+left][+right][+middle]   mic:LEVEL\n"
"                 cmd:reset|hardreset|coin1|coin2|mic|button|fds-insert|fds-eject|fds-next|fds-prev\n"
"                 cmd:save=FILE|load=FILE|dip=VALUE|nsf-song=N|tape-play=FILE|tape-record=FILE|tape-stop\n"
"devices  --device PORT=TYPE (port1 port2 fs1..fs4 exp)  --list-devices\n"
"settings --config FILE (.reg export or Name=value)  --set Name=Value  --save-config FILE\n"
"         --palette FILE.pal  --data-dir DIR (default ~/.furb_cli)  --no-save\n"
"media    --wav FILE  --avi FILE\n"
"state    --load-state FILE  --movie-play FILE.nmv  --movie-record FILE.nmv\n"
"nsf      --nsf-song N (Furbtendulator waits for Play, as in its NSF window; this presses it at frame 0)\n"
"game     --dip VALUE  --cheat NAME|all (from cheats.cfg)  --header BYTE=VALUE|mapper=N|submapper=N\n"
"debug    --trace FILE [--trace-frames A-B]\n");
	exit(2);
}

// ============================================================== main
int main(int argc, char **argv) {
	setlocale(LC_ALL, "C.UTF-8");
	if (argc < 2) usage();
	std::string rom, prefix = "furb", wav_path, avi_path, save_config, load_state, movie_play, movie_record;
	std::string trace_path, palette, data_dir;
	int frames = 600, every = 0, port = 1, trace_from = 0, trace_to = INT_MAX;
	bool hashes = false, info = false, no_save = false, list_devices = false;
	std::set<int> dumps;
	std::string input;
	std::vector<std::string> configs, sets, devices, cheats, headers;
	long dip = -1;
	int nsf_song = 0;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
		if (a == "--frames") frames = atoi(next().c_str());
		else if (a == "--every") every = atoi(next().c_str());
		else if (a == "--out") prefix = next();
		else if (a == "--input") input += next() + ";";
		else if (a == "--port") port = atoi(next().c_str());
		else if (a == "--hashes") hashes = true;
		else if (a == "--quiet") quiet = FurbHost::quiet = true;
		else if (a == "--verbose") FurbHost::verbose = true;
		else if (a == "--info") info = true;
		else if (a == "--dump") for (auto &d : split(next(), ',')) { if (!d.empty()) dumps.insert(atoi(d.c_str())); }
		else if (a == "--config") configs.push_back(next());
		else if (a == "--set") sets.push_back(next());
		else if (a == "--save-config") save_config = next();
		else if (a == "--device") devices.push_back(next());
		else if (a == "--list-devices") list_devices = true;
		else if (a == "--wav") wav_path = next();
		else if (a == "--avi") avi_path = next();
		else if (a == "--load-state") load_state = next();
		else if (a == "--movie-play") movie_play = next();
		else if (a == "--movie-record") movie_record = next();
		else if (a == "--cheat") cheats.push_back(lower(next()));
		else if (a == "--dip") dip = strtol(next().c_str(), NULL, 0);
		else if (a == "--nsf-song") nsf_song = atoi(next().c_str());
		else if (a == "--header") headers.push_back(next());
		else if (a == "--palette") palette = next();
		else if (a == "--data-dir") data_dir = next();
		else if (a == "--no-save") no_save = true;
		else if (a == "--trace") trace_path = next();
		else if (a == "--trace-frames") { std::string r = next(); trace_from = atoi(r.c_str()); size_t d = r.find('-'); trace_to = d == std::string::npos ? trace_from : atoi(r.c_str() + d + 1); }
		else if (a[0] == '-') usage();
		else rom = a;
	}
	if (list_devices) {
		printf("controller ports (port1 port2 fs1..fs4):");
		for (auto t : std_types) printf(" %s", t);
		printf("\nexpansion port (exp):");
		for (auto t : exp_types) printf(" %s", t);
		printf("\n");
		return 0;
	}
	if (rom.empty()) usage();
	if (port != 1 && port != 2) usage();
	parse_script(input, port);

	// ---- settings: the registry Furbtendulator's own loader reads ----
	for (auto &c : configs) {
		std::string err;
		if (!FurbHost::load_config(c, err)) die(err);
	}
	for (auto &kv : sets) {
		size_t eq = kv.find('=');
		if (eq == std::string::npos) die("--set wants Name=Value");
		std::string name = kv.substr(0, eq), val = kv.substr(eq + 1);
		if (!val.empty() && val[0] == '"') FurbHost::set_string(W(name), W(val.substr(1, val.size() - (val.back() == '"' ? 2 : 1))));
		else if (!val.empty() && (isdigit((unsigned char)val[0]) || val[0] == '-')) FurbHost::set_dword(W(name), (DWORD)strtol(val.c_str(), NULL, 0));
		else die("--set value must be a number or \"text\": " + kv);
	}
	if (!palette.empty()) {
		char full[PATH_MAX];
		if (!realpath(palette.c_str(), full)) die("no palette file " + palette);
		for (const char *r : {"NTSC", "PAL", "Dendy", "VS", "PC10"}) {
			FurbHost::set_dword(W(std::string("Palette") + r), Settings::PALETTE_EXT);
			FurbHost::set_string(W(std::string("CustPalette") + r), W(full));
		}
	}

	// ---- capture ----
	Wav wav;
	Avi avi;
	std::vector<uint8_t> pending_audio;
	bool audio = !wav_path.empty() || !avi_path.empty();
	if (audio) FurbHost::set_audio_sink([&](const void *d, size_t n) {
		pending_audio.insert(pending_audio.end(), (const uint8_t *)d, (const uint8_t *)d + n);
	});

	// ---- paths: ProgPath = next to this binary (Mappers/, *.cfg, BIOS/) ----
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	exe[n > 0 ? n : 0] = 0;
	std::string dir = exe;
	dir = dir.substr(0, dir.find_last_of('/') + 1);
	wcscpy(ProgPath, W(dir).c_str());
	if (data_dir.empty()) data_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.furb_cli";
	mkdir(data_dir.c_str(), 0777);
	wcscpy(DataPath, W(data_dir).c_str());

	NES::Init();
	Settings::AutoRun = FALSE;	// the CLI drives frames itself; no emulation thread
	Settings::FSkip = 0;		// render every frame (FSkip>0 makes the PPU skip drawing)
	Settings::aFSkip = FALSE;
	if (no_save) Settings::SkipSRAMSave = TRUE;

	// ---- header patches go to a copy ----
	std::string load_path = rom;
	if (!headers.empty()) {
		FILE *in = fopen(rom.c_str(), "rb");
		if (!in) die("cannot read " + rom);
		std::vector<uint8_t> data;
		int c;
		while ((c = fgetc(in)) != EOF) data.push_back((uint8_t)c);
		fclose(in);
		if (data.size() < 16 || memcmp(data.data(), "NES\x1a", 4)) die("--header needs an iNES file");
		for (auto &h : headers) {
			size_t eq = h.find('=');
			if (eq == std::string::npos) die("--header wants BYTE=VALUE, mapper=N or submapper=N");
			std::string k = lower(h.substr(0, eq));
			long v = strtol(h.c_str() + eq + 1, NULL, 0);
			if (k == "mapper") {
				data[6] = (data[6] & 0x0F) | (v & 0x0F) << 4;
				data[7] = (data[7] & 0x0F) | (v & 0xF0);
				data[8] = (data[8] & 0xF0) | (v >> 8 & 0x0F);
				data[7] = (data[7] & 0xF3) | 0x08;	// NES 2.0
			} else if (k == "submapper") {
				data[8] = (data[8] & 0x0F) | (v & 0x0F) << 4;
				data[7] = (data[7] & 0xF3) | 0x08;
			} else {
				long b = strtol(k.c_str(), NULL, 0);
				if (b < 0 || b > 15) die("--header byte must be 0..15");
				data[b] = (uint8_t)v;
			}
		}
		std::string tmpdir = data_dir + "/patched";
		mkdir(tmpdir.c_str(), 0777);
		load_path = tmpdir + "/" + rom.substr(rom.find_last_of('/') + 1);
		FILE *out = fopen(load_path.c_str(), "wb");
		fwrite(data.data(), 1, data.size(), out);
		fclose(out);
	}

	std::wstring wrom = W(load_path);
	NES::OpenFile(&wrom[0]);
	if (!NES::ROMLoaded || !PPU::PPU[0]) die("failed to load " + rom);
	NES::Running = FALSE;	// OpenFile "starts" NSFs regardless of AutoRun; there is no emulation thread
	if (nsf_song > 0) {
		if (RI.ROMType != ROM_NSF) die("--nsf-song: not an NSF");
		parse_script("0:cmd:nsf-song=" + std::to_string(nsf_song), port);
	}
	for (auto &d : devices) set_device(d);
	map_buttons();
	FurbHost::set_client(GFX::SIZEX, GFX::SIZEY);
	if (dip >= 0) {
		RI.dipValue = (uint32_t)dip;
		RI.dipValueSet = TRUE;
		NES::Reset(RESET_HARD);
	}
	for (auto &want : cheats) {
		bool hit = false;
		for (auto &c : Cheats::cheatDefinition.cheats)
			if (want == "all" || lower(furb_narrow(c.name.c_str())).find(want) != std::string::npos) c.enabled = hit = true;
		if (!hit) die("no cheat matching '" + want + "' for this game (see --info)");
		Cheats::anyCheatsActive = true;
	}
	if (info) { print_info(); return 0; }
	if (!quiet)
		fprintf(stderr, "furb_cli: %s  mapper %d.%d  console %s  view %dx%d\n", rom.c_str(), (int)RI.INES_MapperNum,
			(int)RI.INES2_SubMapper, RI.ConsoleType < 12 ? console_names[RI.ConsoleType] : "?", GFX::SIZEX, GFX::SIZEY);

	if (audio) Sound::SoundON();
	if (!wav_path.empty()) wav.open(wav_path, 48000, 1);	// Sound.cpp: SAMPLING_RATE, mono, 16-bit
	if (!load_state.empty()) States::LoadState(W(load_state).c_str(), W(load_state).c_str());
	if (!movie_play.empty()) {
		FurbHost::on_dialog(IDD_MOVIE_PLAY, [&](HWND d) {
			FurbHost::queue_file(W(movie_play));
			FurbHost::click(d, IDC_MOVIE_PLAY_BROWSE);
			FurbHost::click(d, IDOK);
		});
		Movie::Play();
		if (!(Movie::Mode & MOV_PLAY)) die("could not play movie " + movie_play);
	}
	if (!movie_record.empty()) {
		FurbHost::on_dialog(IDD_MOVIE_RECORD, [&](HWND d) {
			FurbHost::queue_file(W(movie_record));
			FurbHost::click(d, IDC_MOVIE_RECORD_BROWSE);
			FurbHost::click(d, IDOK);
		});
		Movie::Record();
		if (!(Movie::Mode & MOV_RECORD)) die("could not record movie " + movie_record);
	}
	FILE *trace = NULL;
	if (!trace_path.empty()) {
		trace = fopen(trace_path.c_str(), "w");
		if (!trace) die("cannot write " + trace_path);
	}

	// ---- the frame loop (NES::Thread's CPU loop, minus threads/debugger UI) ----
	int prev_mx = 255, prev_my = 255;
	for (int frame = 0; frame < frames; frame++) {
		// input for this frame
		for (int p = 0; p < 7; p++) memset(Controllers::JoyState[2 + p].rgbButtons, 0, 128);
		memset(Controllers::KeyState, 0, sizeof(Controllers::KeyState));
		memset(Controllers::MouseState.rgbButtons, 0, sizeof(Controllers::MouseState.rgbButtons));
		float mic = 0;
		int mx, my;
		FurbHost::get_cursor(mx, my);
		for (Action &a : script) {
			if (frame < a.first || frame > a.last) continue;
			switch (a.kind) {
			case Action::PAD: {
				int p = resolve_pad(a.pad);
				for (int b = 0; b < CONTROLLERS_MAXBUTTONS; b++)
					if (a.bits >> b & 1) Controllers::JoyState[2 + p].rgbButtons[b] = 0x80;
				break;
			}
			case Action::KEY: for (int k : a.keys) Controllers::KeyState[k] = 0x80; break;
			case Action::MOUSE:
				mx = a.mx; my = a.my;
				for (int b = 0; b < 3; b++) if (a.mbuttons >> b & 1) Controllers::MouseState.rgbButtons[b] = 0x80;
				break;
			case Action::MIC: mic = a.mic; break;
			case Action::CMD: {
				std::string c = a.cmd;
				if (c == "reset" || c == "hardreset") {
					if (Movie::Mode) Movie::Stop();
					NES::Reset(c == "reset" ? RESET_SOFT : RESET_HARD);
				} else if (c == "coin1") { if (!NES::coinDelay1) { NES::coin1 = 0x20; NES::coinDelay1 = 222222; } }
				else if (c == "coin2") { if (!NES::coinDelay2) { NES::coin2 = 0x20; NES::coinDelay2 = 222222; } }
				else if (c == "mic") NES::micDelay = 222222;
				else if (c == "button") PlugThruDevice::pressButton();
				else if (c == "fds-insert") NES::insert28();
				else if (c == "fds-eject") NES::eject28();
				else if (c == "fds-next") NES::next28();
				else if (c == "fds-prev") NES::previous28();
				else if (c == "save") States::SaveState(W(a.arg).c_str(), W(a.arg).c_str());
				else if (c == "load") States::LoadState(W(a.arg).c_str(), W(a.arg).c_str());
				else if (c == "dip") {
					RI.dipValue = (uint32_t)strtol(a.arg.c_str(), NULL, 0);
					RI.dipValueSet = TRUE;
					if (Movie::Mode) Movie::Stop();
					NES::Reset(RESET_SOFT);		// as the GUI's DIP window does
				} else if (c == "nsf-song") {
					// the NSF pack's control window: move the song slider, then press Play
					HWND d = FurbHost::modeless(101);			// IDD_NSF (NSF/resource.h)
					if (!d) die("cmd:nsf-song: not an NSF");
					FurbHost::set_pos(d, 1007, atoi(a.arg.c_str()) - 1);	// IDC_NSF_SELECT
					SendMessage(d, WM_HSCROLL, 0, (LPARAM)GetDlgItem(d, 1007));
					FurbHost::click(d, 1005);				// IDC_NSF_PLAY
				} else if (c == "tape-play") { FurbHost::queue_file(W(a.arg)); Tape::Play(); }
				else if (c == "tape-record") { FurbHost::queue_file(W(a.arg)); Tape::Record(); }
				else if (c == "tape-stop") Tape::Stop();
				else die("unknown command cmd:" + c);
				break;
			}
			}
		}
		FurbHost::set_cursor(mx, my);
		Controllers::MouseState.lX = mx - prev_mx;
		Controllers::MouseState.lY = my - prev_my;
		Controllers::MouseState.lZ = 0;
		prev_mx = mx; prev_my = my;
		FurbHost::set_mic(mic);

		bool tracing = trace && frame >= trace_from && frame <= trace_to;
		if (tracing) { Debugger::LogFile = trace; Debugger::Logging = TRUE; }
		for (;;) {
			if (tracing) Debugger::AddInst();
			if (CPU::CPU[1]) {
				if (CPU::CPU[0]->CycleCount <= CPU::CPU[1]->CycleCount) CPU::CPU[0]->ExecOp();
				else CPU::CPU[1]->ExecOp();
			} else
				CPU::CPU[0]->ExecOp();
			if (!NES::Scanline) continue;
			NES::Scanline = FALSE;
			if (PPU::PPU[0]->SLnum == 240) break;
			if (PPU::PPU[0]->SLnum == (RI.InputType == INPUT_FOURSCORE ? 10 : PPU::PPU[0]->SLStartNMI)) {
				if (NES::GenericMulticart) Controllers::SwitchControllersAutomatically();
				Controllers::UpdateInput();
			}
		}
		if (tracing) { Debugger::Logging = FALSE; fflush(trace); }

		bool dump = dumps.count(frame) || (every && frame % every == 0);
		if (hashes || dump || avi_path.size()) {
			int w, h;
			std::vector<uint16_t> idx = frame_indices(w, h);
			std::vector<uint8_t> rgb = to_rgb(idx);
			if (hashes) printf("%d %08lx\n", frame, frame_hash(rgb));
			if (dump) dump_frame(prefix, frame, idx, rgb, w, h);
			if (!avi_path.empty()) {
				if (!avi.f) {
					bool pal = NES::CurRegion != Settings::REGION_NTSC;
					// NTSC 39375000/655171 fps (60.0988); PAL and Dendy 26601712/531960 (50.007)
					avi.open(avi_path, w, h, pal ? 26601712 : 39375000, pal ? 531960 : 655171, 48000, 1);
				}
				avi.frame(rgb, w, h);
			}
		}
		if (audio) {
			wav.write(pending_audio);
			avi.audio(pending_audio);
			pending_audio.clear();
		}
	}

	// ---- shutdown: movie, tape, captures, battery saves, settings ----
	if (Movie::Mode) Movie::Stop();
	Tape::Stop();
	if (trace) fclose(trace);
	wav.close();
	avi.close();
	NES::CloseFile();	// writes battery saves (SRAM) and modified FDS disks unless --no-save
	if (!save_config.empty()) {
		Settings::SaveSettings();
		if (!FurbHost::save_config(save_config)) die("cannot write " + save_config);
	}
	return 0;
}
