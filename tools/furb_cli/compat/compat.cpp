// Real implementations behind the Win32 shim (see windows.h): wide printf with
// MSVC semantics, UTF-8 paths, and "DLL" loading through dlopen.  Compiled
// into both furb_cli and the mapper .so (hidden there).
#include <windows.h>
#include <dirent.h>
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

FILE *furb_wfopen(const wchar_t *name, const wchar_t *mode) {
	return fopen(posix_path(name).c_str(), furb_narrow(mode).c_str());
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
