// The executable's side of the shim (compat.cpp without FURB_PACK): what
// furb_cli.cpp uses to configure the registry, script dialogs, feed the file
// pickers, move the cursor and capture audio.
#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace FurbHost {
extern bool verbose;	// print Furbtendulator's debug-window output and every MessageBox
extern bool quiet;	// suppress MessageBox text too

// ---- registry (HKCU\SOFTWARE\Nintendulator) ----
// A regedit export (.reg, UTF-16 or UTF-8, the Nintendulator key) or a plain
// "Name=value" file (numbers become DWORDs, "quoted" text becomes strings).
bool load_config(const std::string &path, std::string &err);
bool save_config(const std::string &path);	// writes a .reg file (REGEDIT4)
void set_dword(const std::wstring &name, DWORD v);
void set_string(const std::wstring &name, const std::wstring &v);
bool get_dword(const std::wstring &name, DWORD &v);

// ---- headless dialogs ----
typedef std::function<void(HWND)> DialogScript;
void on_dialog(int template_id, DialogScript script);	// run when that dialog opens
HWND modeless(int template_id);				// newest CreateDialog of that template, or NULL
void click(HWND dlg, int control);			// WM_COMMAND BN_CLICKED to the dialog procedure
void set_text(HWND dlg, int control, const std::wstring &text);
void set_check(HWND dlg, int control, int state);
void set_pos(HWND dlg, int control, int pos);		// trackbar position
std::wstring get_text(HWND dlg, int control);
void queue_file(const std::wstring &path);		// next GetOpen/SaveFileName answer

// ---- cursor / mic ----
void set_cursor(int x, int y);		// NES screen pixels
void get_cursor(int &x, int &y);
void set_client(int w, int h);		// the "window" size GFX scales the cursor by
void set_mic(float level);		// karaoke microphone peak level 0..1

// ---- audio ----
typedef std::function<void(const void *, size_t)> AudioSink;
void set_audio_sink(AudioSink sink);	// set before NES::Init to get a DirectSound
}
