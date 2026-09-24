// Real implementations behind the Win32 shim (see windows.h): wide printf with
// MSVC semantics, UTF-8 paths, "DLL" loading through dlopen, directory
// helpers -- compiled into both furb_cli and the mapper packs -- and, in the
// executable only, the host services (registry, headless dialogs, file
// pickers, cursor, microphone, capture-only DirectSound).  The mapper packs
// (built with -DFURB_PACK) forward the host calls to the executable.
#include <windows.h>
#include <dsound.h>
#include <endpointvolume.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <locale.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <vector>

std::string furb_narrow(const wchar_t *w) {
	std::string r;
	if (!w) return r;
	for (; *w; w++) {
		unsigned c = (unsigned)*w;
		if (c < 0x80) r += (char)c;
		else if (c < 0x800) { r += (char)(0xC0 | c >> 6); r += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { r += (char)(0xE0 | c >> 12); r += (char)(0x80 | (c >> 6 & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
		else { r += (char)(0xF0 | c >> 18); r += (char)(0x80 | (c >> 12 & 0x3F)); r += (char)(0x80 | (c >> 6 & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
	}
	return r;
}

std::wstring furb_widen(const char *s) {
	std::wstring r;
	if (!s) return r;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		unsigned c = *p++;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra) c &= 0x3F >> extra;
		while (extra-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
		r += (wchar_t)c;
	}
	return r;
}

// Windows paths -> POSIX: backslashes become '/', and "*.dll" means our ".so".
static std::string posix_path(const wchar_t *w) {
	std::string p = furb_narrow(w);
	for (auto &c : p) if (c == '\\') c = '/';
	if (p.size() > 4 && !strcasecmp(p.c_str() + p.size() - 4, ".dll")) p.replace(p.size() - 4, 4, ".so");
	return p;
}

// Windows file names are case-insensitive (BIOS\\DISKSYS.ROM may be disksys.rom
// on disk): when the exact path is missing, match each component ignoring case.
static std::string resolve_case(const std::string &p) {
	if (p.empty() || access(p.c_str(), F_OK) == 0) return p;
	std::string out = p[0] == '/' ? "/" : "";
	size_t pos = p[0] == '/' ? 1 : 0;
	while (pos <= p.size()) {
		size_t slash = p.find('/', pos);
		std::string part = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
		std::string dir = out.empty() ? "." : out;
		std::string hit = part;
		if (!part.empty() && access((out + part).c_str(), F_OK) != 0)
			if (DIR *d = opendir(dir.c_str())) {
				while (struct dirent *e = readdir(d))
					if (!strcasecmp(e->d_name, part.c_str())) { hit = e->d_name; break; }
				closedir(d);
			}
		out += hit;
		if (slash == std::string::npos) break;
		out += '/';
		pos = slash + 1;
	}
	return out;
}

// MSVC text mode with an encoding ("rt,ccs=UTF-16LE", used for cheats.cfg /
// dip.cfg): the BOM decides UTF-8 or UTF-16LE, and fgetwc returns characters.
// glibc's own ccs= would force UTF-16, so decode here and hand back a UTF-8
// stream, which fgetwc decodes under the C.UTF-8 locale furb_cli sets.
static FILE *open_ccs_read(const std::string &path, const std::string &ccs) {
	FILE *in = fopen(path.c_str(), "rb");
	if (!in) in = fopen(resolve_case(path).c_str(), "rb");
	if (!in) return NULL;
	std::string raw;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0) raw.append(buf, n);
	fclose(in);
	std::string utf8;
	bool utf16 = raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE;
	if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) utf8 = raw.substr(3);
	else if (utf16 || (strcasestr(ccs.c_str(), "UTF-16") && raw.size() >= 2 && raw[1] == 0)) {
		std::wstring w;
		for (size_t i = utf16 ? 2 : 0; i + 1 < raw.size(); i += 2) w += (wchar_t)((unsigned char)raw[i] | (unsigned char)raw[i + 1] << 8);
		utf8 = furb_narrow(w.c_str());
	} else utf8 = raw;
	std::string text;	// text mode: CRLF -> LF
	for (size_t i = 0; i < utf8.size(); i++) if (!(utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n')) text += utf8[i];
	// Hand back a freshly opened real file: a stream we wrote to is byte-
	// oriented (glibc then fails fgetwc), and fmemopen streams cannot do wide
	// reads at all.  Written, closed, reopened, unlinked.
	char name[] = "/tmp/furb_cfg_XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0) return NULL;
	FILE *w = fdopen(fd, "wb");
	fwrite(text.data(), 1, text.size(), w);
	fclose(w);
	FILE *f = fopen(name, "r");
	unlink(name);
	return f;
}

FILE *furb_wfopen(const wchar_t *name, const wchar_t *mode) {
	std::string p = posix_path(name), m = furb_narrow(mode);
	size_t ccs = m.find(",ccs=");
	if (ccs != std::string::npos) {
		std::string enc = m.substr(ccs + 5);
		m = m.substr(0, ccs);
		if (m[0] == 'r' && m.find('+') == std::string::npos) return open_ccs_read(p, enc);
	}
	std::string plain;	// 't' (text) is the default on POSIX
	for (char c : m) if (c != 't') plain += c;
	FILE *f = fopen(p.c_str(), plain.c_str());
	if (!f && plain[0] == 'r') f = fopen(resolve_case(p).c_str(), plain.c_str());
	return f;
}

DWORD GetModuleFileName(HMODULE, wchar_t *buf, DWORD n) {
	char exe[4096];
	ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (k <= 0 || !n) { if (n) buf[0] = 0; return 0; }
	exe[k] = 0;
	std::wstring w = furb_widen(exe);
	for (auto &c : w) if (c == L'/') c = L'\\';	// callers look for the last '\\'
	wcsncpy(buf, w.c_str(), n - 1);
	buf[n - 1] = 0;
	return (DWORD)wcslen(buf);
}
int furb_wremove(const wchar_t *name) { return remove(posix_path(name).c_str()); }
int furb_access(const char *p, int m) { return access(p, m); }

DWORD GetTickCount(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

void furb_splitpath(const wchar_t *path, wchar_t *drive, wchar_t *dir, wchar_t *fname, wchar_t *ext) {
	std::wstring p(path ? path : L"");
	size_t slash = p.find_last_of(L"/\\");
	std::wstring d = slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
	std::wstring f = slash == std::wstring::npos ? p : p.substr(slash + 1);
	size_t dot = f.find_last_of(L'.');
	std::wstring e = dot == std::wstring::npos ? L"" : f.substr(dot);
	if (dot != std::wstring::npos) f = f.substr(0, dot);
	if (drive) drive[0] = 0;
	if (dir) wcscpy(dir, d.c_str());
	if (fname) wcscpy(fname, f.c_str());
	if (ext) wcscpy(ext, e.c_str());
}

// MSVC wide formats: %s and %c take WIDE arguments, %S/%C narrow; %hs narrow.
std::wstring furb_fixfmt(const wchar_t *fmt) {
	std::wstring out;
	for (const wchar_t *p = fmt; *p; p++) {
		out += *p;
		if (*p != L'%') continue;
		if (p[1] == L'%') { out += *++p; continue; }
		// copy flags/width/precision/length until the conversion char
		while (p[1] && wcschr(L"-+ #0123456789.*", p[1])) out += *++p;
		bool narrow = false, wide = false;
		while (p[1] && wcschr(L"hlLqjztI", p[1])) {
			wchar_t c = *++p;
			if (c == L'h') { narrow = true; continue; }
			if (c == L'I') {	// I64 / I32 / I
				if (p[1] == L'6' && p[2] == L'4') { out += L"ll"; p += 2; }
				else if (p[1] == L'3' && p[2] == L'2') { p += 2; }
				continue;
			}
			if (c == L'l') wide = true;
			out += c;
		}
		wchar_t conv = p[1];
		if (!conv) break;
		p++;
		if (conv == L's' || conv == L'c') {
			if (narrow) out += conv;	// %hs -> narrow
			else { if (!wide) out += L'l'; out += conv; }
		} else if (conv == L'S' || conv == L'C') {
			out += (wchar_t)(conv == L'S' ? L's' : L'c');	// %S -> narrow
		} else {
			if (narrow) out += L'h';
			out += conv;
		}
	}
	return out;
}

int furb_vswprintf(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap) {
	std::wstring f = furb_fixfmt(fmt);
	int r = vswprintf(buf, n, f.c_str(), ap);
	if (r < 0 && n) buf[n - 1] = 0;
	return r;
}
int furb_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int furb_swprintf_nc(wchar_t *buf, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, 1u << 20, fmt, ap); va_end(ap); return r;
}
int furb_fwprintf(FILE *f, const wchar_t *fmt, ...) {
	wchar_t tmp[4096];
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(tmp, 4096, fmt, ap); va_end(ap);
	fputs(furb_narrow(tmp).c_str(), f);
	return r;
}
int furb_swscanf(const wchar_t *s, const wchar_t *fmt, ...) {
	std::wstring f = furb_fixfmt(fmt);
	va_list ap; va_start(ap, fmt); int r = vswscanf(s, f.c_str(), ap); va_end(ap); return r;
}

// ---------------------------------------------------------------- FindFirstFile
struct FindState { std::vector<std::string> names; size_t next; };

static bool fill(FindState *st, WIN32_FIND_DATA *d) {
	if (st->next >= st->names.size()) return false;
	std::string n = st->names[st->next++];
	// report ".so" packs under their Windows ".dll" names; LoadLibrary maps back
	if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) n.replace(n.size() - 3, 3, ".dll");
	memset(d, 0, sizeof *d);
	std::wstring w = furb_widen(n.c_str());
	wcsncpy(d->cFileName, w.c_str(), MAX_PATH - 1);
	return true;
}

HANDLE FindFirstFile(const wchar_t *pattern, WIN32_FIND_DATA *d) {
	std::string p = posix_path(pattern);
	size_t slash = p.find_last_of('/');
	std::string dir = slash == std::string::npos ? "." : p.substr(0, slash);
	std::string pat = slash == std::string::npos ? p : p.substr(slash + 1);
	DIR *dh = opendir(dir.c_str());
	if (!dh) return INVALID_HANDLE_VALUE;
	FindState *st = new FindState{{}, 0};
	while (struct dirent *e = readdir(dh))
		if (!fnmatch(pat.c_str(), e->d_name, FNM_CASEFOLD)) st->names.push_back(e->d_name);
	closedir(dh);
	if (!fill(st, d)) { delete st; return INVALID_HANDLE_VALUE; }
	return (HANDLE)st;
}
BOOL FindNextFile(HANDLE h, WIN32_FIND_DATA *d) { return fill((FindState *)h, d); }
BOOL FindClose(HANDLE h) { delete (FindState *)h; return TRUE; }

HMODULE LoadLibrary(const wchar_t *name) {
	void *h = dlopen(posix_path(name).c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!h) fprintf(stderr, "furb_cli: dlopen %s: %s\n", posix_path(name).c_str(), dlerror());
	return (HMODULE)h;
}
void *GetProcAddress(HMODULE h, const char *name) { return h ? dlsym((void *)h, name) : NULL; }
BOOL FreeLibrary(HMODULE h) { return h && !dlclose((void *)h); }

// ---------------------------------------------------------------- file system
DWORD GetFileAttributes(LPCTSTR p) {
	struct stat st;
	if (stat(posix_path(p).c_str(), &st)) return (DWORD)INVALID_FILE_ATTRIBUTES;
	return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL CreateDirectory(LPCTSTR p, void *) { return mkdir(posix_path(p).c_str(), 0777) == 0; }
BOOL PathFileExists(LPCTSTR p) { return access(posix_path(p).c_str(), F_OK) == 0; }
BOOL PathAppend(LPTSTR path, LPCTSTR more) {
	size_t n = wcslen(path);
	if (n && path[n - 1] != L'\\' && path[n - 1] != L'/') wcscat(path, L"\\");
	while (*more == L'\\' || *more == L'/') more++;
	wcscat(path, more);
	return TRUE;
}

#ifdef FURB_PACK
// ---------------------------------------------------------------- pack side
// Everything below lives in the executable; look it up once by name.
static void *host(const char *name) {
	typedef void *(*Lookup)(const char *);
	static Lookup lookup = (Lookup)dlsym(RTLD_DEFAULT, "furb_host_lookup");
	return lookup ? lookup(name) : NULL;
}
#define FWD(ret, name, params, args) ret name params { \
	typedef ret (*F) params; static F fn_ = (F)host(#name); return fn_ ? fn_ args : ret(); }
FWD(int, MessageBox, (HWND h, LPCTSTR t, LPCTSTR c, UINT y), (h, t, c, y))
FWD(INT_PTR, DialogBoxParam, (HINSTANCE i, LPCTSTR t, HWND p, DLGPROC f, LPARAM l), (i, t, p, f, l))
FWD(INT_PTR, DialogBoxIndirectParam, (HINSTANCE i, LPCDLGTEMPLATE t, HWND p, DLGPROC f, LPARAM l), (i, t, p, f, l))
FWD(HWND, CreateDialogParam, (HINSTANCE i, LPCTSTR t, HWND p, DLGPROC f, LPARAM l), (i, t, p, f, l))
FWD(HWND, CreateDialogIndirectParam, (HINSTANCE i, LPCDLGTEMPLATE t, HWND p, DLGPROC f, LPARAM l), (i, t, p, f, l))
FWD(BOOL, EndDialog, (HWND h, INT_PTR r), (h, r))
FWD(HWND, GetDlgItem, (HWND h, int id), (h, id))
FWD(BOOL, SetDlgItemText, (HWND h, int id, LPCTSTR t), (h, id, t))
FWD(UINT, GetDlgItemText, (HWND h, int id, LPTSTR t, int n), (h, id, t, n))
FWD(BOOL, SetDlgItemInt, (HWND h, int id, UINT v, BOOL s), (h, id, v, s))
FWD(UINT, GetDlgItemInt, (HWND h, int id, BOOL *ok, BOOL s), (h, id, ok, s))
FWD(BOOL, CheckDlgButton, (HWND h, int id, UINT c), (h, id, c))
FWD(UINT, IsDlgButtonChecked, (HWND h, int id), (h, id))
FWD(BOOL, CheckRadioButton, (HWND h, int a, int b, int c), (h, a, b, c))
FWD(LRESULT, SendDlgItemMessage, (HWND h, int id, UINT m, WPARAM w, LPARAM l), (h, id, m, w, l))
FWD(LRESULT, SendMessage, (HWND h, UINT m, WPARAM w, LPARAM l), (h, m, w, l))
FWD(int, GetWindowTextLength, (HWND h), (h))
FWD(BOOL, GetOpenFileName, (OPENFILENAME *o), (o))
FWD(BOOL, GetSaveFileName, (OPENFILENAME *o), (o))
FWD(BOOL, GetCursorPos, (POINT *p), (p))
FWD(BOOL, SetCursorPos, (int x, int y), (x, y))
FWD(BOOL, GetClientRect, (HWND h, RECT *r), (h, r))
FWD(BOOL, ScreenToClient, (HWND h, POINT *p), (h, p))
FWD(BOOL, ClientToScreen, (HWND h, POINT *p), (h, p))
FWD(float, furb_mic_level, (void), ())
// The packs never touch the registry or DirectSound.
LONG RegOpenKeyEx(HKEY, LPCTSTR, DWORD, DWORD, HKEY *) { return ERROR_FILE_NOT_FOUND; }
LONG RegCreateKeyEx(HKEY, LPCTSTR, DWORD, LPTSTR, DWORD, DWORD, void *, HKEY *, DWORD *) { return ERROR_FILE_NOT_FOUND; }
LONG RegCloseKey(HKEY) { return 0; }
LONG RegQueryValueEx(HKEY, LPCTSTR, DWORD *, DWORD *, BYTE *, DWORD *) { return ERROR_FILE_NOT_FOUND; }
LONG RegSetValueEx(HKEY, LPCTSTR, DWORD, DWORD, const BYTE *, DWORD) { return ERROR_FILE_NOT_FOUND; }
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *) { *out = NULL; return E_FAIL; }

#else
// ---------------------------------------------------------------- host side
#include "furb_host.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace FurbHost {
bool verbose = false, quiet = false;

// ---- registry ----
struct RegVal { DWORD type; std::vector<BYTE> data; std::wstring name; };
static std::map<std::wstring, RegVal> reg;	// key: lower-case value name
static std::wstring lower(std::wstring s) { for (auto &c : s) c = towlower(c); return s; }
static void reg_put(const std::wstring &name, DWORD type, const void *data, size_t n) {
	RegVal v; v.type = type; v.name = name;
	v.data.assign((const BYTE *)data, (const BYTE *)data + n);
	reg[lower(name)] = v;
}
void set_dword(const std::wstring &name, DWORD v) { reg_put(name, REG_DWORD, &v, 4); }
void set_string(const std::wstring &name, const std::wstring &v) {
	reg_put(name, REG_SZ, v.c_str(), (v.size() + 1) * sizeof(wchar_t));	// TCHAR is wchar_t here
}
bool get_dword(const std::wstring &name, DWORD &v) {
	auto it = reg.find(lower(name));
	if (it == reg.end() || it->second.data.size() < 4) return false;
	memcpy(&v, it->second.data.data(), 4);
	return true;
}

static std::wstring decode_file(const std::string &raw) {
	if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
		std::wstring w;
		for (size_t i = 2; i + 1 < raw.size(); i += 2) w += (wchar_t)((unsigned char)raw[i] | (unsigned char)raw[i + 1] << 8);
		return w;
	}
	size_t skip = raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
	return furb_widen(raw.c_str() + skip);
}
static std::wstring unescape(const std::wstring &s) {
	std::wstring r;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == L'\\' && i + 1 < s.size()) r += s[++i];
		else r += s[i];
	}
	return r;
}
static bool parse_value(const std::wstring &name, std::wstring val, std::string &err) {
	while (!val.empty() && iswspace(val.back())) val.pop_back();
	std::wstring lv = lower(val);
	if (!val.empty() && val[0] == L'"') {
		size_t end = val.rfind(L'"');
		set_string(name, unescape(val.substr(1, end > 0 ? end - 1 : 0)));
	} else if (lv.compare(0, 6, L"dword:") == 0) {
		set_dword(name, (DWORD)wcstoul(val.c_str() + 6, NULL, 16));
	} else if (lv.compare(0, 3, L"hex") == 0) {
		size_t colon = val.find(L':');
		DWORD type = REG_BINARY;
		if (lv.compare(0, 4, L"hex(") == 0) type = (DWORD)wcstoul(val.c_str() + 4, NULL, 16);
		std::vector<BYTE> bytes;
		for (const wchar_t *p = val.c_str() + colon + 1; *p;) {
			while (*p && !iswxdigit(*p)) p++;
			if (!*p) break;
			wchar_t *e;
			bytes.push_back((BYTE)wcstoul(p, &e, 16));
			p = e;
		}
		if (type == 1 || type == 2) {		// REG_SZ / REG_EXPAND_SZ exported as UTF-16LE bytes
			std::wstring w;
			for (size_t i = 0; i + 1 < bytes.size(); i += 2) w += (wchar_t)(bytes[i] | bytes[i + 1] << 8);
			while (!w.empty() && w.back() == 0) w.pop_back();
			set_string(name, w);
		} else if (type == 4 && bytes.size() == 4) {
			DWORD d; memcpy(&d, bytes.data(), 4); set_dword(name, d);
		} else reg_put(name, type == 1 ? REG_SZ : REG_BINARY, bytes.data(), bytes.size());
	} else if (!val.empty() && (iswdigit(val[0]) || val[0] == L'-')) {
		set_dword(name, (DWORD)wcstol(val.c_str(), NULL, 0));
	} else {
		err = "bad value for " + furb_narrow(name.c_str()) + ": " + furb_narrow(val.c_str());
		return false;
	}
	return true;
}

bool load_config(const std::string &path, std::string &err) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) { err = "cannot open " + path; return false; }
	std::string raw;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.append(buf, n);
	fclose(f);
	std::wstring text = decode_file(raw);
	// join regedit's "\" line continuations
	std::wstring joined;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == L'\\' && i + 1 < text.size() && (text[i + 1] == L'\r' || text[i + 1] == L'\n')) {
			while (i + 1 < text.size() && (text[i + 1] == L'\r' || text[i + 1] == L'\n' || text[i + 1] == L' ')) i++;
			continue;
		}
		joined += text[i];
	}
	bool in_section = true, any_section = false;
	size_t pos = 0;
	while (pos < joined.size()) {
		size_t eol = joined.find(L'\n', pos);
		std::wstring line = joined.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
		pos = eol == std::wstring::npos ? joined.size() : eol + 1;
		while (!line.empty() && (line.back() == L'\r' || iswspace(line.back()))) line.pop_back();
		size_t b = 0;
		while (b < line.size() && iswspace(line[b])) b++;
		line = line.substr(b);
		if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
		if (lower(line).compare(0, 8, L"regedit4") == 0 || lower(line).compare(0, 16, L"windows registry") == 0) continue;
		if (line[0] == L'[') {
			any_section = true;
			std::wstring sec = lower(line);
			in_section = sec.find(L"\\software\\nintendulator]") != std::wstring::npos;
			continue;
		}
		if (any_section && !in_section) continue;
		std::wstring name, val;
		if (line[0] == L'"') {
			size_t q = line.find(L'"', 1);
			if (q == std::wstring::npos) continue;
			name = line.substr(1, q - 1);
			size_t eq = line.find(L'=', q);
			if (eq == std::wstring::npos) continue;
			val = line.substr(eq + 1);
		} else {
			size_t eq = line.find(L'=');
			if (eq == std::wstring::npos || line[0] == L'@') continue;
			name = line.substr(0, eq);
			while (!name.empty() && iswspace(name.back())) name.pop_back();
			val = line.substr(eq + 1);
			while (!val.empty() && iswspace(val[0])) val.erase(0, 1);
		}
		if (!parse_value(name, val, err)) return false;
	}
	return true;
}

bool save_config(const std::string &path) {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	fprintf(f, "REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\SOFTWARE\\Nintendulator]\r\n");
	std::vector<const RegVal *> vals;
	for (auto &kv : reg) vals.push_back(&kv.second);
	std::sort(vals.begin(), vals.end(), [](const RegVal *a, const RegVal *b) { return a->name < b->name; });
	for (const RegVal *v : vals) {
		std::string name = furb_narrow(v->name.c_str());
		if (v->type == REG_DWORD && v->data.size() == 4) {
			DWORD d; memcpy(&d, v->data.data(), 4);
			fprintf(f, "\"%s\"=dword:%08lx\r\n", name.c_str(), (unsigned long)d);
		} else if (v->type == REG_SZ) {
			std::string s = furb_narrow((const wchar_t *)v->data.data()), e;
			for (char c : s) { if (c == '\\' || c == '"') e += '\\'; e += c; }
			fprintf(f, "\"%s\"=\"%s\"\r\n", name.c_str(), e.c_str());
		} else {
			fprintf(f, "\"%s\"=hex:", name.c_str());
			for (size_t i = 0; i < v->data.size(); i++) fprintf(f, "%s%02x", i ? "," : "", v->data[i]);
			fprintf(f, "\r\n");
		}
	}
	fclose(f);
	return true;
}

// ---- headless dialogs ----
struct FakeDlg;
struct FakeCtl { FakeDlg *dlg; int id; };
struct FakeDlg {
	int tmpl; DLGPROC proc;
	std::map<int, std::wstring> text;
	std::map<int, int> check, pos;
	std::map<int, FakeCtl *> ctls;
	bool ended = false; INT_PTR result = 0;
};
static std::set<void *> dialogs, controls;
static std::map<int, DialogScript> scripts;
static std::map<int, FakeDlg *> modeless_by_tmpl;
static std::deque<std::wstring> file_queue;

static FakeDlg *as_dlg(HWND h) { return dialogs.count(h) ? (FakeDlg *)h : NULL; }
static FakeCtl *as_ctl(HWND h) { return controls.count(h) ? (FakeCtl *)h : NULL; }
static int tmpl_id(LPCTSTR t) { return (uintptr_t)t < 0x10000 ? (int)(uintptr_t)t : -1; }

void on_dialog(int id, DialogScript s) { scripts[id] = s; }
HWND modeless(int id) { auto it = modeless_by_tmpl.find(id); return it == modeless_by_tmpl.end() ? NULL : (HWND)it->second; }
void click(HWND h, int id) {
	FakeDlg *d = as_dlg(h);
	if (d && !d->ended) d->proc(h, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(h, id));
}
void set_text(HWND h, int id, const std::wstring &t) { if (FakeDlg *d = as_dlg(h)) d->text[id] = t; }
void set_check(HWND h, int id, int c) { if (FakeDlg *d = as_dlg(h)) d->check[id] = c; }
void set_pos(HWND h, int id, int p) { if (FakeDlg *d = as_dlg(h)) d->pos[id] = p; }
std::wstring get_text(HWND h, int id) { FakeDlg *d = as_dlg(h); return d ? d->text[id] : L""; }
void queue_file(const std::wstring &p) { file_queue.push_back(p); }

static FakeDlg *open_dialog(int id, DLGPROC proc, LPARAM lp) {
	FakeDlg *d = new FakeDlg;
	d->tmpl = id; d->proc = proc;
	dialogs.insert(d);
	proc((HWND)d, WM_INITDIALOG, 0, lp);
	return d;
}
static void close_dialog(FakeDlg *d) {
	for (auto &c : d->ctls) { controls.erase(c.second); delete c.second; }
	dialogs.erase(d);
	delete d;
}

// ---- cursor / mic / audio ----
static int cur_x = 255, cur_y = 255, client_w = 256, client_h = 240;
static float mic = 0;
static AudioSink audio_sink;
void set_cursor(int x, int y) { cur_x = x; cur_y = y; }
void get_cursor(int &x, int &y) { x = cur_x; y = cur_y; }
void set_client(int w, int h) { client_w = w; client_h = h; }
void set_mic(float level) { mic = level; }
void set_audio_sink(AudioSink s) { audio_sink = s; }
} // namespace FurbHost

using namespace FurbHost;

// ---------------------------------------------------------------- registry API
LONG RegOpenKeyEx(HKEY, LPCTSTR, DWORD, DWORD, HKEY *out) { *out = (HKEY)1; return 0; }
LONG RegCreateKeyEx(HKEY, LPCTSTR, DWORD, LPTSTR, DWORD, DWORD, void *, HKEY *out, DWORD *) { *out = (HKEY)1; return 0; }
LONG RegCloseKey(HKEY) { return 0; }
LONG RegQueryValueEx(HKEY, LPCTSTR name, DWORD *, DWORD *type, BYTE *data, DWORD *size) {
	auto it = reg.find(lower(name));
	if (it == reg.end()) return ERROR_FILE_NOT_FOUND;
	DWORD need = (DWORD)it->second.data.size();
	if (type) *type = it->second.type;
	if (!data) { if (size) *size = need; return 0; }
	if (!size) return 87;
	if (*size < need) { *size = need; return ERROR_MORE_DATA; }
	memcpy(data, it->second.data.data(), need);
	*size = need;
	return 0;
}
LONG RegSetValueEx(HKEY, LPCTSTR name, DWORD, DWORD type, const BYTE *data, DWORD size) {
	reg_put(name, type, data, size);
	return 0;
}

// ---------------------------------------------------------------- dialogs API
int MessageBox(HWND, LPCTSTR text, LPCTSTR caption, UINT type) {
	// the missing display/sound/input devices are expected: only say so with --verbose
	bool expected = text && (wcsstr(text, L"DirectDraw") || wcsstr(text, L"DirectInput") || wcsstr(text, L"DirectSound"));
	if (!quiet && (verbose || !expected))
		fprintf(stderr, "furb_cli: [%s] %s\n", furb_narrow(caption ? caption : L"").c_str(), furb_narrow(text ? text : L"").c_str());
	UINT kind = type & 0xF;
	return (kind == MB_YESNO || kind == MB_YESNOCANCEL) ? IDNO : IDOK;	// never agree to anything unasked
}
INT_PTR DialogBoxParam(HINSTANCE, LPCTSTR t, HWND, DLGPROC proc, LPARAM lp) {
	int id = tmpl_id(t);
	auto s = scripts.find(id);
	if (s == scripts.end() || !proc) {
		if (verbose) fprintf(stderr, "furb_cli: dialog %d cancelled (no script)\n", id);
		return 0;
	}
	FakeDlg *d = open_dialog(id, proc, lp);
	if (!d->ended) s->second((HWND)d);
	INT_PTR r = d->ended ? d->result : 0;
	close_dialog(d);
	return r;
}
INT_PTR DialogBoxIndirectParam(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM) {
	if (verbose) fprintf(stderr, "furb_cli: in-memory dialog cancelled\n");
	return 0;
}
HWND CreateDialogParam(HINSTANCE, LPCTSTR t, HWND, DLGPROC proc, LPARAM lp) {
	if (!proc) return NULL;
	int id = tmpl_id(t);
	FakeDlg *d = open_dialog(id, proc, lp);
	modeless_by_tmpl[id] = d;
	auto s = scripts.find(id);
	if (s != scripts.end()) s->second((HWND)d);
	return (HWND)d;
}
HWND CreateDialogIndirectParam(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM) { return NULL; }
BOOL EndDialog(HWND h, INT_PTR r) {
	FakeDlg *d = as_dlg(h);
	if (!d) return FALSE;
	d->ended = true; d->result = r;
	return TRUE;
}
HWND GetDlgItem(HWND h, int id) {
	FakeDlg *d = as_dlg(h);
	if (!d) return NULL;
	FakeCtl *&c = d->ctls[id];
	if (!c) { c = new FakeCtl{d, id}; controls.insert(c); }
	return (HWND)c;
}
BOOL SetDlgItemText(HWND h, int id, LPCTSTR t) { if (FakeDlg *d = as_dlg(h)) { d->text[id] = t ? t : L""; return TRUE; } return FALSE; }
UINT GetDlgItemText(HWND h, int id, LPTSTR buf, int n) {
	FakeDlg *d = as_dlg(h);
	if (!buf || n <= 0) return 0;
	std::wstring t = d ? d->text[id] : L"";
	wcsncpy(buf, t.c_str(), n - 1);
	buf[n - 1] = 0;
	return (UINT)wcslen(buf);
}
BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sgn) {
	wchar_t b[32];
	swprintf(b, 32, sgn ? L"%d" : L"%u", v);
	return SetDlgItemText(h, id, b);
}
UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sgn) {
	FakeDlg *d = as_dlg(h);
	std::wstring t = d ? d->text[id] : L"";
	if (ok) *ok = !t.empty();
	return sgn ? (UINT)wcstol(t.c_str(), NULL, 10) : (UINT)wcstoul(t.c_str(), NULL, 10);
}
BOOL CheckDlgButton(HWND h, int id, UINT c) { if (FakeDlg *d = as_dlg(h)) { d->check[id] = c; return TRUE; } return FALSE; }
UINT IsDlgButtonChecked(HWND h, int id) { FakeDlg *d = as_dlg(h); return d ? d->check[id] : 0; }
BOOL CheckRadioButton(HWND h, int first, int last, int on) {
	FakeDlg *d = as_dlg(h);
	if (!d) return FALSE;
	for (int i = first; i <= last; i++) d->check[i] = (i == on);
	return TRUE;
}
LRESULT SendDlgItemMessage(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp) {
	FakeDlg *d = as_dlg(h);
	if (!d) {
		// Nintendulator's debug window (hDebug is NULL here): AddDebug text
		if (verbose && msg == EM_REPLACESEL && lp) {
			std::string s = furb_narrow((const wchar_t *)lp);
			if (s == "\r\n") fputc('\n', stderr);
			else fprintf(stderr, "furb_cli: %s", s.c_str());
		}
		return 0;
	}
	switch (msg) {
	case TBM_SETPOS: d->pos[id] = (int)lp; return 0;
	case TBM_GETPOS: return d->pos[id];
	case CB_SETCURSEL: case LB_SETCURSEL: d->pos[id] = (int)wp; return 0;
	case CB_GETCURSEL: case LB_GETCURSEL: return d->pos[id];
	case BM_SETCHECK: d->check[id] = (int)wp; return 0;
	case BM_GETCHECK: return d->check[id];
	case WM_SETTEXT: d->text[id] = lp ? (const wchar_t *)lp : L""; return TRUE;
	case WM_GETTEXT: {
		std::wstring t = d->text[id];
		if (!lp || !wp) return 0;
		wcsncpy((wchar_t *)lp, t.c_str(), wp - 1);
		((wchar_t *)lp)[wp - 1] = 0;
		return wcslen((wchar_t *)lp);
	}
	default: return 0;
	}
}
LRESULT SendMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (FakeCtl *c = as_ctl(h)) return SendDlgItemMessage((HWND)c->dlg, c->id, msg, wp, lp);
	if (FakeDlg *d = as_dlg(h)) return d->proc(h, msg, wp, lp);
	return 0;
}
int GetWindowTextLength(HWND h) {
	if (FakeCtl *c = as_ctl(h)) return (int)c->dlg->text[c->id].size();
	return 0;
}
static BOOL pick_file(OPENFILENAME *o) {
	if (file_queue.empty()) {
		if (verbose) fprintf(stderr, "furb_cli: file dialog cancelled (no file queued)\n");
		return FALSE;
	}
	std::wstring f = file_queue.front();
	file_queue.pop_front();
	if (!o->lpstrFile || o->nMaxFile == 0) return FALSE;
	wcsncpy(o->lpstrFile, f.c_str(), o->nMaxFile - 1);
	o->lpstrFile[o->nMaxFile - 1] = 0;
	size_t slash = f.find_last_of(L"/\\"), dot = f.find_last_of(L'.');
	o->nFileOffset = (WORD)(slash == std::wstring::npos ? 0 : slash + 1);
	o->nFileExtension = (WORD)(dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? f.size() : dot + 1);
	return TRUE;
}
BOOL GetOpenFileName(OPENFILENAME *o) { return pick_file(o); }
BOOL GetSaveFileName(OPENFILENAME *o) { return pick_file(o); }

// ---------------------------------------------------------------- cursor / mic
BOOL GetCursorPos(POINT *p) { p->x = cur_x; p->y = cur_y; return TRUE; }
BOOL SetCursorPos(int x, int y) { cur_x = x; cur_y = y; return TRUE; }
BOOL GetClientRect(HWND, RECT *r) { r->left = r->top = 0; r->right = client_w; r->bottom = client_h; return TRUE; }
BOOL ScreenToClient(HWND, POINT *) { return TRUE; }
BOOL ClientToScreen(HWND, POINT *) { return TRUE; }
float furb_mic_level(void) { return mic; }

// ---------------------------------------------------------------- DirectSound
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *) {
	if (!audio_sink) { *out = NULL; return E_FAIL; }
	*out = new FurbDS;
	return S_OK;
}
HRESULT FurbDS::CreateSoundBuffer(const DSBUFFERDESC *desc, FurbDSBuffer **out, void *) {
	FurbDSBuffer *b = new FurbDSBuffer;
	b->primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
	b->mem.resize(desc->dwBufferBytes);
	*out = b;
	return S_OK;
}
HRESULT FurbDSBuffer::GetCurrentPosition(DWORD *play, DWORD *write) {
	if (play) *play = 0x7FFFFFFF;	// "far ahead": Sound::Run's pacing loop never waits
	if (write) *write = 0x7FFFFFFF;
	return S_OK;
}
HRESULT FurbDSBuffer::Lock(DWORD, DWORD bytes, void **ptr1, DWORD *len1, void **ptr2, DWORD *len2, DWORD flags) {
	clearing = (flags & DSBLOCK_ENTIREBUFFER) != 0;
	if (clearing) bytes = (DWORD)mem.size();
	if (mem.size() < bytes) mem.resize(bytes);
	*ptr1 = mem.data(); *len1 = bytes;
	if (ptr2) *ptr2 = NULL;
	if (len2) *len2 = 0;
	return S_OK;
}
HRESULT FurbDSBuffer::Unlock(void *ptr1, DWORD len1, void *, DWORD) {
	if (!clearing && audio_sink && len1) audio_sink(ptr1, len1);
	clearing = false;
	return S_OK;
}

// ---------------------------------------------------------------- export
// The one symbol the executable exports (--dynamic-list): the packs find the
// host functions above through it.
extern "C" __attribute__((visibility("default"))) void *furb_host_lookup(const char *name) {
	static const std::map<std::string, void *> table = {
#define E(n) {#n, (void *)&n}
		E(MessageBox), E(DialogBoxParam), E(DialogBoxIndirectParam), E(CreateDialogParam),
		E(CreateDialogIndirectParam), E(EndDialog), E(GetDlgItem), E(SetDlgItemText), E(GetDlgItemText),
		E(SetDlgItemInt), E(GetDlgItemInt), E(CheckDlgButton), E(IsDlgButtonChecked), E(CheckRadioButton),
		E(SendDlgItemMessage), E(SendMessage), E(GetWindowTextLength), E(GetOpenFileName), E(GetSaveFileName),
		E(GetCursorPos), E(SetCursorPos), E(GetClientRect), E(ScreenToClient), E(ClientToScreen), E(furb_mic_level),
#undef E
	};
	auto it = table.find(name);
	return it == table.end() ? NULL : it->second;
}
#endif
