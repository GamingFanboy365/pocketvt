// Minimal Win32 / DirectX shim so Furbtendulator's emulation core builds as a
// headless 32-bit Linux program (furb_cli).  Only what the sources reference.
// Types and macros are real; every GUI / DirectX / registry / dialog call is a
// no-op returning "zero" (0, FALSE, NULL, or a failing HRESULT), which the
// emulator treats as "feature unavailable".  The emulation path never needs
// any of them.
#pragma once
#ifndef FURB_COMPAT_WINDOWS_H
#define FURB_COMPAT_WINDOWS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <strings.h>

#ifdef __cplusplus
#include <string>
#include <utility>

// ---------------------------------------------------------------- calling conv
#define WINAPI
#define CALLBACK
#define APIENTRY
#define __cdecl
#define __stdcall
#define __fastcall
#define __forceinline	// MSVC falls back to a normal call when the body is in another file
#define __declspec(x) __declspec_##x
#define __declspec_dllexport __attribute__((visibility("default")))
#define __declspec_dllimport
#define FAR
#define NEAR
#define CONST const
#define IN
#define OUT
#define _In_
#define _In_opt_
#define _Out_
#define _Inout_
#define UNREFERENCED_PARAMETER(x) (void)(x)

// ---------------------------------------------------------------- base types
typedef int BOOL;
typedef unsigned char BYTE, UCHAR, *LPBYTE, *PBYTE;
typedef unsigned short WORD, USHORT, *LPWORD;
typedef unsigned long DWORD, ULONG, *LPDWORD, *PDWORD;   // 32-bit: build with -m32
typedef long LONG, *LPLONG, HRESULT;
typedef int INT;
typedef unsigned int UINT;
typedef short SHORT;
typedef char CHAR;
typedef float FLOAT;
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, DWORD64, UINT64;
typedef int INT_PTR, LONG_PTR;
typedef unsigned int UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM, LRESULT;
typedef void VOID, *LPVOID, *PVOID;
typedef const void *LPCVOID;
typedef wchar_t WCHAR, TCHAR, *LPWSTR, *LPTSTR, *PTSTR;
typedef const wchar_t *LPCWSTR, *LPCTSTR, *PCTSTR;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WORD ATOM;
typedef DWORD COLORREF;

typedef void *HANDLE;
#define DECLARE_HANDLE(n) typedef struct n##__ { int unused; } *n
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HACCEL);
DECLARE_HANDLE(HCURSOR);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HMONITOR);
DECLARE_HANDLE(HDROP);
DECLARE_HANDLE(HGLOBAL_);
DECLARE_HANDLE(HRGN);
typedef HINSTANCE HMODULE;
typedef void *HGDIOBJ, *HGLOBAL, *HLOCAL;
typedef void *PSID;

#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL 0
#endif
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFF
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define DD_OK S_OK
#define DS_OK S_OK
#define DI_OK S_OK
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define ERROR_SUCCESS 0L

#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w) ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w) ((BYTE)((DWORD_PTR)(w) >> 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELPARAM(l, h) ((LPARAM)MAKELONG(l, h))
#define MAKEWPARAM(l, h) ((WPARAM)MAKELONG(l, h))
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb)>>16))
#define MAKEINTRESOURCE(i) ((LPTSTR)((ULONG_PTR)((WORD)(i))))
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#define ARRAYSIZE(a) _countof(a)

#define ZeroMemory(p, n) memset((p), 0, (n))
#define FillMemory(p, n, v) memset((p), (v), (n))
#define CopyMemory(d, s, n) memcpy((d), (s), (n))
#define MoveMemory(d, s, n) memmove((d), (s), (n))

// Windows' min/max macros, as functions (mixed-type safe).
template <class A, class B> inline auto min(A a, B b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <class A, class B> inline auto max(A a, B b) -> decltype(a < b ? a : b) { return a < b ? b : a; }

// ---------------------------------------------------------------- "zero" stub
// Returned by every stubbed API: converts to any type as its zero value.  A
// failing HRESULT is what COM callers check, so HRESULT converts to E_FAIL.
struct WinZero {
	operator int() const { return 0; }
	template <class T> operator T() const { return T(); }
};
template <> inline WinZero::operator long() const { return E_FAIL; }
template <class T> inline bool operator==(T *a, WinZero) { return a == nullptr; }
template <class T> inline bool operator==(WinZero, T *a) { return a == nullptr; }
template <class T> inline bool operator!=(T *a, WinZero) { return a != nullptr; }
template <class T> inline bool operator!=(WinZero, T *a) { return a != nullptr; }
#define WINSTUB(name) template <class... A> inline WinZero name(A&&...) { return WinZero(); }

// ---------------------------------------------------------------- structs
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;
typedef struct _GUID { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID, IID, CLSID;
typedef const GUID &REFGUID, &REFIID, &REFCLSID;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER, *LPBITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO, *LPBITMAPINFO;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT;
typedef struct { UINT cbSize, fMask; int nMin, nMax; UINT nPage; int nPos, nTrackPos; } SCROLLINFO;
typedef struct { DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCTSTR lpstrFilter; LPTSTR lpstrCustomFilter; DWORD nMaxCustFilter, nFilterIndex; LPTSTR lpstrFile; DWORD nMaxFile; LPTSTR lpstrFileTitle; DWORD nMaxFileTitle; LPCTSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension; LPCTSTR lpstrDefExt; LPARAM lCustData; void *lpfnHook; LPCTSTR lpTemplateName; } OPENFILENAME;
typedef struct { UINT cbSize, style; void *lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCTSTR lpszMenuName, lpszClassName; HICON hIconSm; } WNDCLASSEX;
typedef struct { UINT length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX, *LPWAVEFORMATEX;
typedef struct { void *DebugInfo; LONG LockCount, RecursionCount; HANDLE OwningThread, LockSemaphore; ULONG_PTR SpinCount; } CRITICAL_SECTION;
typedef struct { UINT mask; int cxy; LPTSTR pszText; HBITMAP hbm; int cchTextMax, fmt; LPARAM lParam; int iImage, iOrder; } HDITEM;
typedef struct tagLVITEMW { UINT mask; int iItem, iSubItem; UINT state, stateMask; LPTSTR pszText; int cchTextMax, iImage; LPARAM lParam; int iIndent; } LVITEM, LVITEMW;
typedef struct tagLVCOLUMNW { UINT mask; int fmt, cx; LPTSTR pszText; int cchTextMax, iSubItem; } LVCOLUMN, LVCOLUMNW;
typedef struct { HWND hwndFrom; UINT_PTR idFrom; UINT code; } NMHDR, *LPNMHDR;
typedef struct { DWORD dwSize, dwICC; } INITCOMMONCONTROLSEX;
typedef struct { LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight; BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily; WCHAR lfFaceName[32]; } LOGFONT;
typedef struct { DWORD dwLength; DWORD dwMemoryLoad; } MEMORYSTATUS;
typedef void *LPSECURITY_ATTRIBUTES, *LPOVERLAPPED;
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
typedef INT_PTR (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef int (*FARPROC)();

// ---------------------------------------------------------------- COM stub
// Every DirectX/COM interface is this class.  Pointers to it stay NULL in the
// CLI (their Create* calls fail), so these methods are never reached.
struct WinCOM {
	template <class... A> WinZero QueryInterface(A&&...) { return WinZero(); }
	ULONG AddRef() { return 0; }
	ULONG Release() { return 0; }
#define COMM(n) template <class... A> WinZero n(A&&...) { return WinZero(); }
	COMM(SetCooperativeLevel) COMM(Acquire) COMM(Unacquire) COMM(Unlock) COMM(Lock)
	COMM(SetDataFormat) COMM(GetDeviceState) COMM(GetCapabilities) COMM(EnumObjects)
	COMM(CreateSurface) COMM(CreateDevice) COMM(Stop) COMM(SetClipper) COMM(Restore)
	COMM(Play) COMM(IsLost) COMM(GetDeviceInfo) COMM(GetCurrentPosition)
	COMM(CreateSoundBuffer) COMM(WaitForVerticalBlank) COMM(SetHWnd) COMM(SetFormat)
	COMM(SetDisplayMode) COMM(RestoreDisplayMode) COMM(Poll) COMM(GetSurfaceDesc)
	COMM(GetAttachedSurface) COMM(Flip) COMM(EnumDevices) COMM(CreateClipper) COMM(Blt)
	COMM(GetDC) COMM(ReleaseDC) COMM(SetProperty) COMM(GetProperty) COMM(GetStatus)
	COMM(SetVolume) COMM(GetVolume) COMM(SetCurrentPosition) COMM(GetDisplayMode)
	COMM(GetDefaultAudioEndpoint) COMM(Activate) COMM(GetMasterVolumeLevelScalar)
	COMM(SetMasterVolumeLevelScalar) COMM(GetMute) COMM(SetMute) COMM(BltFast)
	COMM(GetFrequency) COMM(SetFrequency) COMM(GetCaps) COMM(Initialize) COMM(GetPeakValue)
#undef COMM
};
typedef WinCOM IUnknown, *LPUNKNOWN;

// ---------------------------------------------------------------- messages etc.
enum {
	WM_NULL = 0, WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6,
	WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_ENABLE = 0xA, WM_SETTEXT = 0xC, WM_PAINT = 0xF,
	WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18,
	WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_GETMINMAXINFO = 0x24, WM_SETFONT = 0x30,
	WM_NOTIFY = 0x4E, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102,
	WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_INITDIALOG = 0x110, WM_COMMAND = 0x111,
	WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113, WM_HSCROLL = 0x114, WM_VSCROLL = 0x115,
	WM_INITMENU = 0x116, WM_MENUSELECT = 0x11F, WM_ENTERMENULOOP = 0x211, WM_EXITMENULOOP = 0x212,
	WM_CTLCOLORSTATIC = 0x138, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
	WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
	WM_MOUSEWHEEL = 0x20A, WM_DROPFILES = 0x233, WM_ENTERSIZEMOVE = 0x231, WM_EXITSIZEMOVE = 0x232,
	WM_SIZING = 0x214, WM_MOVING = 0x216, WM_DEVICECHANGE = 0x219, WM_USER = 0x400, WM_APP = 0x8000,
	WM_NCLBUTTONDOWN = 0xA1, WM_GETDLGCODE = 0x87, WM_DRAWITEM = 0x2B, WM_MEASUREITEM = 0x2C,
	WM_CONTEXTMENU = 0x7B, WM_DISPLAYCHANGE = 0x7E, WM_NCHITTEST = 0x84, WM_POWERBROADCAST = 0x218,
};
enum {
	IDOK = 1, IDCANCEL = 2, IDABORT = 3, IDRETRY = 4, IDIGNORE = 5, IDYES = 6, IDNO = 7,
	MB_OK = 0, MB_OKCANCEL = 1, MB_YESNOCANCEL = 3, MB_YESNO = 4, MB_ICONERROR = 0x10,
	MB_ICONQUESTION = 0x20, MB_ICONWARNING = 0x30, MB_ICONEXCLAMATION = 0x30, MB_ICONINFORMATION = 0x40,
	MB_ICONSTOP = 0x10, MB_DEFBUTTON2 = 0x100,
	MF_BYCOMMAND = 0, MF_ENABLED = 0, MF_GRAYED = 1, MF_DISABLED = 2, MF_UNCHECKED = 0,
	MF_CHECKED = 8, MF_BYPOSITION = 0x400, MF_STRING = 0, MF_POPUP = 0x10, MF_SEPARATOR = 0x800,
	SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOW = 5, SW_SHOWMAXIMIZED = 3, SW_MINIMIZE = 6, SW_RESTORE = 9, SW_SHOWNA = 8,
	SWP_NOSIZE = 1, SWP_NOMOVE = 2, SWP_NOZORDER = 4, SWP_NOACTIVATE = 0x10, SWP_SHOWWINDOW = 0x40,
	SWP_NOOWNERZORDER = 0x200, SWP_FRAMECHANGED = 0x20,
	RDW_INVALIDATE = 1, RDW_ERASE = 4, RDW_UPDATENOW = 0x100, RDW_ALLCHILDREN = 0x80,
	SIF_RANGE = 1, SIF_PAGE = 2, SIF_POS = 4, SIF_ALL = 0x17, SB_HORZ = 0, SB_VERT = 1, SB_CTL = 2,
	SB_LINEUP = 0, SB_LINEDOWN = 1, SB_PAGEUP = 2, SB_PAGEDOWN = 3, SB_THUMBTRACK = 5, SB_THUMBPOSITION = 4,
	SB_TOP = 6, SB_BOTTOM = 7, SB_LINELEFT = 0, SB_LINERIGHT = 1, SB_PAGELEFT = 2, SB_PAGERIGHT = 3,
	BST_UNCHECKED = 0, BST_CHECKED = 1, BST_INDETERMINATE = 2, BM_GETCHECK = 0xF0, BM_SETCHECK = 0xF1,
	BN_CLICKED = 0, EN_CHANGE = 0x300, EN_KILLFOCUS = 0x200, CBN_SELCHANGE = 1, LBN_SELCHANGE = 1,
	LBN_DBLCLK = 2, CB_ADDSTRING = 0x143, CB_GETCURSEL = 0x147, CB_SETCURSEL = 0x14E,
	CB_RESETCONTENT = 0x14B, CB_GETCOUNT = 0x146, CB_SETITEMDATA = 0x151, CB_GETITEMDATA = 0x150,
	CB_FINDSTRINGEXACT = 0x158, CB_GETLBTEXT = 0x148, CB_ERR = -1, CB_INSERTSTRING = 0x14A, CB_SETDROPPEDWIDTH = 0x160,
	LB_ADDSTRING = 0x180, LB_RESETCONTENT = 0x184, LB_SETCURSEL = 0x186, LB_GETCURSEL = 0x188,
	LB_GETCOUNT = 0x18B, LB_SETTOPINDEX = 0x197, LB_GETTEXT = 0x189, LB_ERR = -1, LB_SETITEMDATA = 0x19A,
	LB_GETITEMDATA = 0x199, LB_DELETESTRING = 0x182, LB_INSERTSTRING = 0x181, LB_GETTOPINDEX = 0x18E,
	LB_SETSEL = 0x185, LB_GETSEL = 0x187, LB_GETSELCOUNT = 0x190, LB_GETSELITEMS = 0x191, LB_SETHORIZONTALEXTENT = 0x194,
	EM_SETSEL = 0xB1, EM_REPLACESEL = 0xC2, EM_SETLIMITTEXT = 0xC5, EM_LIMITTEXT = 0xC5, EM_GETLINECOUNT = 0xBA,
	EM_LINESCROLL = 0xB6, EM_SCROLLCARET = 0xB7, EM_SETREADONLY = 0xCF,
	WS_OVERLAPPED = 0, WS_POPUP = (int)0x80000000, WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000,
	WS_CAPTION = 0xC00000, WS_SYSMENU = 0x80000, WS_THICKFRAME = 0x40000, WS_MINIMIZEBOX = 0x20000,
	WS_MAXIMIZEBOX = 0x10000, WS_OVERLAPPEDWINDOW = 0xCF0000, WS_TABSTOP = 0x10000, WS_BORDER = 0x800000,
	WS_VSCROLL = 0x200000, WS_HSCROLL = 0x100000, WS_EX_TOPMOST = 8, WS_EX_CLIENTEDGE = 0x200, WS_EX_ACCEPTFILES = 0x10,
	WS_DISABLED = 0x8000000, WS_GROUP = 0x20000, WS_CLIPCHILDREN = 0x2000000, WS_EX_TOOLWINDOW = 0x80,
	GWL_STYLE = -16, GWL_EXSTYLE = -20, GWLP_USERDATA = -21, GWL_USERDATA = -21, GWLP_WNDPROC = -4, DWLP_USER = 8,
	CS_HREDRAW = 2, CS_VREDRAW = 1, CS_DBLCLKS = 8, CW_USEDEFAULT = (int)0x80000000,
	COLOR_BTNFACE = 15, COLOR_WINDOW = 5, COLOR_WINDOWTEXT = 8, COLOR_3DFACE = 15,
	DIB_RGB_COLORS = 0, BI_RGB = 0, SRCCOPY = 0xCC0020, TRANSPARENT = 1, OPAQUE = 2,
	FW_NORMAL = 400, FW_BOLD = 700, DEFAULT_CHARSET = 1, ANSI_CHARSET = 0,
	KEY_ALL_ACCESS = 0xF003F, KEY_READ = 0x20019, KEY_WRITE = 0x20006, KEY_QUERY_VALUE = 1,
	REG_OPTION_NON_VOLATILE = 0, REG_DWORD = 4, REG_SZ = 1, REG_BINARY = 3,
	OFN_OVERWRITEPROMPT = 2, OFN_HIDEREADONLY = 4, OFN_FILEMUSTEXIST = 0x1000, OFN_PATHMUSTEXIST = 0x800,
	OFN_NOCHANGEDIR = 8, OFN_EXPLORER = 0x80000, OFN_ALLOWMULTISELECT = 0x200, OFN_ENABLESIZING = 0x800000,
	CP_ACP = 0, CP_UTF8 = 65001, VK_ESCAPE = 0x1B, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11,
	VK_MENU = 0x12, VK_CAPITAL = 0x14, VK_SCROLL = 0x91, VK_NUMLOCK = 0x90, VK_TAB = 9, VK_F1 = 0x70,
	VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_SPACE = 0x20, VK_BACK = 8, VK_DELETE = 0x2E,
	VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_HOME = 0x24, VK_END = 0x23, VK_INSERT = 0x2D, VK_LBUTTON = 1, VK_RBUTTON = 2,
	SC_SCREENSAVE = 0xF140, SC_MONITORPOWER = 0xF170, SC_KEYMENU = 0xF100,
	MONITOR_DEFAULTTONEAREST = 2, MONITOR_DEFAULTTOPRIMARY = 1, SM_CXSCREEN = 0, SM_CYSCREEN = 1,
	SM_CXFRAME = 32, SM_CYFRAME = 33, SM_CYCAPTION = 4, SM_CYMENU = 15,
	THREAD_PRIORITY_NORMAL = 0, THREAD_PRIORITY_HIGHEST = 2, THREAD_PRIORITY_ABOVE_NORMAL = 1,
	DT_LEFT = 0, DT_CENTER = 1, DT_SINGLELINE = 0x20, DT_VCENTER = 4, DT_CALCRECT = 0x400,
	HWND_TOP_ = 0, IDC_ARROW_ = 32512, IDC_CROSS_ = 32515, IMAGE_BITMAP = 0, IMAGE_ICON = 1, LR_DEFAULTCOLOR = 0,
	PM_REMOVE = 1, PM_NOREMOVE = 0, QS_ALLINPUT = 0x4FF, WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258,
	FILE_ATTRIBUTE_DIRECTORY = 0x10, INVALID_FILE_ATTRIBUTES = -1,
	CSIDL_APPDATA = 0x1A, CSIDL_FLAG_CREATE = 0x8000, SHGFP_TYPE_CURRENT = 0,
	BIF_RETURNONLYFSDIRS = 1, BIF_NEWDIALOGSTYLE = 0x40, BIF_USENEWUI = 0x50,
	ICC_WIN95_CLASSES = 0xFF, ICC_BAR_CLASSES = 4, ICC_LISTVIEW_CLASSES = 1,
	TBM_SETRANGE = 0x406, TBM_SETPOS = 0x405, TBM_GETPOS = 0x400, TBM_SETTICFREQ = 0x414, TBM_SETPAGESIZE = 0x415,
	UDM_SETRANGE = 0x465, UDM_SETPOS = 0x467, UDM_GETPOS = 0x468, UDM_SETRANGE32 = 0x46F, UDM_SETPOS32 = 0x471,
	LVM_INSERTITEM = 0x104D, LVM_SETITEM = 0x104C, LVM_DELETEALLITEMS = 0x1009, LVM_INSERTCOLUMN = 0x1061,
	LVM_SETEXTENDEDLISTVIEWSTYLE = 0x1036, LVM_GETNEXTITEM = 0x100C, LVM_GETITEMCOUNT = 0x1004,
	LVIF_TEXT = 1, LVIF_PARAM = 4, LVCF_TEXT = 4, LVCF_WIDTH = 2, LVCF_FMT = 1, LVCF_SUBITEM = 8, LVNI_SELECTED = 2,
	LVS_EX_FULLROWSELECT = 0x20, LVS_EX_GRIDLINES = 1, LVS_EX_CHECKBOXES = 4, LVN_ITEMCHANGED = -101,
	FILE_BEGIN = 0, FILE_CURRENT = 1, FILE_END = 2, GENERIC_READ_ = 0, GMEM_MOVEABLE = 2, CF_TEXT = 1, CF_UNICODETEXT = 13,
	ES_AUTOHSCROLL = 0x80, ES_READONLY = 0x800, ES_MULTILINE = 4, SS_LEFT = 0, BS_AUTOCHECKBOX = 3, BS_PUSHBUTTON = 0,
	HWND_BROADCAST_ = 0xFFFF, SPI_GETSCREENSAVEACTIVE = 16, SPI_SETSCREENSAVEACTIVE = 17,
	SPIF_SENDWININICHANGE = 2, ES_CONTINUOUS = (int)0x80000000, ES_DISPLAY_REQUIRED = 2, ES_SYSTEM_REQUIRED = 1,
};
#define HWND_TOP ((HWND)0)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_CROSS MAKEINTRESOURCE(32515)
#define IDC_WAIT MAKEINTRESOURCE(32514)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)
#define LVM_SETITEMTEXT 0x1074
#define LVM_GETITEMTEXT 0x1073
#define LVCFMT_LEFT 0
#define LVCFMT_RIGHT 1
#define LVCFMT_CENTER 2
typedef unsigned char byte;

// ---------------------------------------------------------------- strings / TCHAR
#define __T(x) L##x
#define _T(x) __T(x)
#define TEXT(x) __T(x)
#define _TEXT(x) __T(x)
#define _tcslen wcslen
#define _tcsnlen wcsnlen
#define _tcscpy wcscpy
#define _tcsncpy wcsncpy
#define _tcscat wcscat
#define _tcsncat wcsncat
#define _tcscmp wcscmp
#define _tcsncmp wcsncmp
#define _tcsicmp wcscasecmp
#define _wcsicmp wcscasecmp
#define _tcsnicmp wcsncasecmp
#define _wcsnicmp wcsncasecmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define _tcschr wcschr
#define _tcsrchr wcsrchr
#define _tcsstr wcsstr
#define _tcstol wcstol
#define _tcstoul wcstoul
#define _tcstod wcstod
#define _tcstok wcstok
#define _tcsdup wcsdup
#define _wcsdup wcsdup
#define _tcsspn wcsspn
#define _tcscspn wcscspn
#define _totupper towupper
#define _totlower towlower
#define _istdigit iswdigit
#define _istspace iswspace
#define _istalpha iswalpha
#define _istxdigit iswxdigit
#define _istprint iswprint
#define _ttoi(s) ((int)wcstol((s), NULL, 10))
#define _wtoi(s) ((int)wcstol((s), NULL, 10))
#define _ttol(s) wcstol((s), NULL, 10)
#define _tstof(s) wcstod((s), NULL)
#define _tcsupr furb_wcsupr
#define _wcsupr furb_wcsupr
#define _tcslwr furb_wcslwr
#define _wcslwr furb_wcslwr
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _itoa_s(v, b, n, r) snprintf((b), (n), (r) == 16 ? "%x" : "%d", (v))
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _fileno fileno
#define _access furb_access
#define _unlink(p) remove(p)
int furb_access(const char *, int);
#define _strtoui64 strtoull
#define _wcstoui64 wcstoull
#define _tcstoui64 wcstoull
#define _tcstoi64 wcstoll

inline wchar_t *furb_wcsupr(wchar_t *s) { for (wchar_t *p = s; *p; p++) *p = towupper(*p); return s; }
inline wchar_t *furb_wcslwr(wchar_t *s) { for (wchar_t *p = s; *p; p++) *p = towlower(*p); return s; }

// MSVC wide printf: %s/%c mean WIDE in wide formats; glibc wants %ls/%lc.
// Also accepts the non-conforming (buffer, format, ...) swprintf form.
std::wstring furb_fixfmt(const wchar_t *fmt);
int furb_vswprintf(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap);
int furb_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...);
int furb_swprintf_nc(wchar_t *buf, const wchar_t *fmt, ...);
int furb_fwprintf(FILE *f, const wchar_t *fmt, ...);
int furb_swscanf(const wchar_t *s, const wchar_t *fmt, ...);
#define _stprintf furb_swprintf_nc
#define _stprintf_s furb_swprintf_s
#define swprintf_s furb_swprintf_s
#define _snwprintf furb_swprintf
#define _sntprintf furb_swprintf
#define _snwprintf_s(b, n, c, ...) furb_swprintf((b), (n), __VA_ARGS__)
#define _sntprintf_s(b, n, c, ...) furb_swprintf((b), (n), __VA_ARGS__)
#define _vstprintf(b, f, a) furb_vswprintf((b), 1u << 20, (f), (a))
#define _vsntprintf furb_vswprintf
#define _vsnwprintf furb_vswprintf
#define _ftprintf furb_fwprintf
#define fwprintf furb_fwprintf
#define _stscanf furb_swscanf
#define swscanf furb_swscanf
#define _tprintf(...) ((void)0)
#define wprintf(...) ((void)0)
inline int swprintf(wchar_t *buf, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, 1u << 20, fmt, ap); va_end(ap); return r;
}
template <size_t N> inline int furb_swprintf_s(wchar_t (&buf)[N], const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, N, fmt, ap); va_end(ap); return r;
}
inline int furb_swprintf_s(wchar_t *buf, size_t n, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, n, fmt, ap); va_end(ap); return r;
}
#define sprintf_s(b, ...) snprintf((b), sizeof(b), __VA_ARGS__)
#define strcpy_s(d, ...) furb_strcpy_s(d, __VA_ARGS__)
template <size_t N> inline int furb_strcpy_s(char (&d)[N], const char *s) { strncpy(d, s, N - 1); d[N - 1] = 0; return 0; }
inline int furb_strcpy_s(char *d, size_t n, const char *s) { strncpy(d, s, n - 1); d[n - 1] = 0; return 0; }
#define _tcscpy_s(d, ...) furb_wcscpy_s(d, __VA_ARGS__)
#define wcscpy_s(d, ...) furb_wcscpy_s(d, __VA_ARGS__)
template <size_t N> inline int furb_wcscpy_s(wchar_t (&d)[N], const wchar_t *s) { wcsncpy(d, s, N - 1); d[N - 1] = 0; return 0; }
inline int furb_wcscpy_s(wchar_t *d, size_t n, const wchar_t *s) { wcsncpy(d, s, n - 1); d[n - 1] = 0; return 0; }
#define _tcscat_s(d, ...) furb_wcscat_s(d, __VA_ARGS__)
#define wcscat_s(d, ...) furb_wcscat_s(d, __VA_ARGS__)
template <size_t N> inline int furb_wcscat_s(wchar_t (&d)[N], const wchar_t *s) { wcsncat(d, s, N - wcslen(d) - 1); return 0; }
inline int furb_wcscat_s(wchar_t *d, size_t n, const wchar_t *s) { wcsncat(d, s, n - wcslen(d) - 1); return 0; }
#define _tcsncpy_s(d, n, s, c) (wcsncpy((d), (s), (c)), 0)
#define _tsplitpath_s(...) ((void)0)
#define _wsplitpath_s(...) ((void)0)

// Wide-path file I/O (paths are converted to UTF-8).
std::string furb_narrow(const wchar_t *w);
std::wstring furb_widen(const char *s);
FILE *furb_wfopen(const wchar_t *name, const wchar_t *mode);
inline int furb_wfopen_s(FILE **f, const wchar_t *name, const wchar_t *mode) { *f = furb_wfopen(name, mode); return *f ? 0 : 1; }
inline int fopen_s(FILE **f, const char *name, const char *mode) { *f = fopen(name, mode); return *f ? 0 : 1; }
#define _tfopen furb_wfopen
#define _wfopen furb_wfopen
#define _tfopen_s furb_wfopen_s
#define _wfopen_s furb_wfopen_s
int furb_wremove(const wchar_t *);
#define _tremove furb_wremove
#define _wremove furb_wremove
#define _tunlink furb_wremove
#define _wunlink furb_wremove
#define DeleteFile furb_wremove
#define _tmkdir(p) ((void)0)
#define _wmkdir(p) ((void)0)
#define _tgetenv(n) ((wchar_t *)NULL)
#define _wgetenv(n) ((wchar_t *)NULL)
#define _taccess(p, m) furb_access(furb_narrow(p).c_str(), (m))
#define _waccess(p, m) furb_access(furb_narrow(p).c_str(), (m))

// ---------------------------------------------------------------- misc runtime
inline void Sleep(DWORD) {}
DWORD GetTickCount(void);
inline DWORD timeGetTime(void) { return GetTickCount(); }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER *v) { v->QuadPart = GetTickCount(); return TRUE; }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart = 1000; return TRUE; }
inline DWORD GetModuleFileName(HMODULE, wchar_t *buf, DWORD n) { if (n) buf[0] = 0; return 0; }
inline DWORD GetCurrentDirectory(DWORD n, wchar_t *buf) { if (n) buf[0] = 0; return 0; }
inline DWORD GetFileAttributes(const wchar_t *) { return (DWORD)-1; }
inline void OutputDebugString(const wchar_t *) {}
inline void OutputDebugStringA(const char *) {}
#define InterlockedIncrement(p) (++*(p))
#define InterlockedDecrement(p) (--*(p))
#define InterlockedExchange(p, v) __sync_lock_test_and_set((p), (v))
inline void InitializeCriticalSection(CRITICAL_SECTION *) {}
inline void DeleteCriticalSection(CRITICAL_SECTION *) {}
inline void EnterCriticalSection(CRITICAL_SECTION *) {}
inline void LeaveCriticalSection(CRITICAL_SECTION *) {}
inline BOOL CloseHandle(HANDLE) { return TRUE; }
inline void GetLocalTime(SYSTEMTIME *t) { memset(t, 0, sizeof *t); }
inline void GetSystemTime(SYSTEMTIME *t) { memset(t, 0, sizeof *t); }
inline int MultiByteToWideChar(UINT, DWORD, const char *s, int n, wchar_t *w, int wn) {
	std::wstring r = furb_widen(n < 0 ? std::string(s).c_str() : std::string(s, n).c_str());
	if (!wn) return (int)r.size() + (n < 0);
	int k = (int)r.size() < wn ? (int)r.size() : wn;
	wmemcpy(w, r.c_str(), k); if (k < wn) w[k] = 0; return k + (n < 0);
}
inline int WideCharToMultiByte(UINT, DWORD, const wchar_t *w, int n, char *s, int sn, const char *, BOOL *) {
	std::string r = furb_narrow(n < 0 ? w : std::wstring(w, n).c_str());
	if (!sn) return (int)r.size() + (n < 0);
	int k = (int)r.size() < sn ? (int)r.size() : sn;
	memcpy(s, r.c_str(), k); if (k < sn) s[k] = 0; return k + (n < 0);
}
#define _aligned_malloc(n, a) aligned_alloc((a), (((n) + (a) - 1) / (a)) * (a))
#define _aligned_free free
#define _alloca __builtin_alloca
#define __assume(x) ((void)0)
#define __debugbreak() ((void)0)
#define _rotl(v, s) (((unsigned)(v) << ((s) & 31)) | ((unsigned)(v) >> ((32 - ((s) & 31)) & 31)))
#define _rotr(v, s) (((unsigned)(v) >> ((s) & 31)) | ((unsigned)(v) << ((32 - ((s) & 31)) & 31)))
#define _byteswap_ushort __builtin_bswap16
#define _byteswap_ulong __builtin_bswap32

// ---------------------------------------------------------------- stubbed API
// Window / menu / dialog / GDI / registry / shell / DLL: all inert.
WINSTUB(MessageBox) WINSTUB(MessageBoxA) WINSTUB(MessageBoxW)
WINSTUB(EnableMenuItem) WINSTUB(CheckMenuItem) WINSTUB(CheckMenuRadioItem) WINSTUB(GetMenu)
WINSTUB(SetMenu) WINSTUB(GetSubMenu) WINSTUB(DrawMenuBar) WINSTUB(ModifyMenu) WINSTUB(InsertMenu)
WINSTUB(AppendMenu) WINSTUB(DeleteMenu) WINSTUB(RemoveMenu) WINSTUB(CreatePopupMenu) WINSTUB(CreateMenu)
WINSTUB(GetMenuItemCount) WINSTUB(TrackPopupMenu) WINSTUB(DestroyMenu) WINSTUB(GetMenuState) WINSTUB(LoadMenu)
WINSTUB(GetDlgItem) WINSTUB(DialogBox) WINSTUB(DialogBoxParam) WINSTUB(CreateDialog) WINSTUB(CreateDialogParam)
WINSTUB(EndDialog) WINSTUB(SetDlgItemText) WINSTUB(GetDlgItemText) WINSTUB(SetDlgItemInt) WINSTUB(GetDlgItemInt)
WINSTUB(CheckDlgButton) WINSTUB(IsDlgButtonChecked) WINSTUB(SendDlgItemMessage) WINSTUB(CheckRadioButton)
WINSTUB(SendMessage) WINSTUB(PostMessage) WINSTUB(PeekMessage) WINSTUB(GetMessage) WINSTUB(TranslateMessage)
WINSTUB(DispatchMessage) WINSTUB(TranslateAccelerator) WINSTUB(IsDialogMessage) WINSTUB(PostQuitMessage)
WINSTUB(DefWindowProc) WINSTUB(CallWindowProc) WINSTUB(RegisterClassEx) WINSTUB(RegisterClass) WINSTUB(CreateWindow)
WINSTUB(CreateWindowEx) WINSTUB(DestroyWindow) WINSTUB(ShowWindow) WINSTUB(UpdateWindow) WINSTUB(SetWindowPos)
WINSTUB(MoveWindow) WINSTUB(GetWindowRect) WINSTUB(GetClientRect) WINSTUB(ClientToScreen) WINSTUB(ScreenToClient)
WINSTUB(AdjustWindowRect) WINSTUB(AdjustWindowRectEx) WINSTUB(SetWindowText) WINSTUB(GetWindowText)
WINSTUB(SetWindowLong) WINSTUB(GetWindowLong) WINSTUB(SetWindowLongPtr) WINSTUB(GetWindowLongPtr)
WINSTUB(InvalidateRect) WINSTUB(RedrawWindow) WINSTUB(IsWindow) WINSTUB(IsWindowVisible) WINSTUB(IsIconic) WINSTUB(IsZoomed)
WINSTUB(SetFocus) WINSTUB(GetFocus) WINSTUB(SetForegroundWindow) WINSTUB(GetForegroundWindow) WINSTUB(EnableWindow)
WINSTUB(GetWindowPlacement) WINSTUB(SetWindowPlacement) WINSTUB(MonitorFromWindow) WINSTUB(GetMonitorInfo)
WINSTUB(GetSystemMetrics) WINSTUB(SystemParametersInfo) WINSTUB(SetThreadExecutionState) WINSTUB(GetDesktopWindow)
WINSTUB(SetTimer) WINSTUB(KillTimer) WINSTUB(SetScrollInfo) WINSTUB(GetScrollInfo) WINSTUB(SetScrollPos)
WINSTUB(GetScrollPos) WINSTUB(SetScrollRange) WINSTUB(ShowScrollBar) WINSTUB(EnableScrollBar)
WINSTUB(LoadCursor) WINSTUB(LoadIcon) WINSTUB(LoadImage) WINSTUB(LoadBitmap) WINSTUB(LoadAccelerators) WINSTUB(LoadString)
WINSTUB(SetCursor) WINSTUB(ShowCursor) WINSTUB(ClipCursor) WINSTUB(GetCursorPos_) WINSTUB(SetCursorPos_)
WINSTUB(GetKeyState) WINSTUB(GetAsyncKeyState) WINSTUB(MapVirtualKey) WINSTUB(GetKeyNameText) WINSTUB(GetKeyboardState)
WINSTUB(SetCapture) WINSTUB(ReleaseCapture) WINSTUB(GetDC) WINSTUB(ReleaseDC) WINSTUB(BeginPaint) WINSTUB(EndPaint)
WINSTUB(CreateCompatibleDC) WINSTUB(CreateCompatibleBitmap) WINSTUB(CreateDIBSection) WINSTUB(DeleteDC)
WINSTUB(DeleteObject) WINSTUB(SelectObject) WINSTUB(GetStockObject) WINSTUB(SetDIBits) WINSTUB(GetDIBits)
WINSTUB(StretchDIBits) WINSTUB(SetDIBitsToDevice) WINSTUB(BitBlt) WINSTUB(StretchBlt) WINSTUB(FillRect)
WINSTUB(FrameRect) WINSTUB(CreateSolidBrush) WINSTUB(CreatePen) WINSTUB(CreateFont) WINSTUB(CreateFontIndirect)
WINSTUB(GetSysColor) WINSTUB(GetSysColorBrush) WINSTUB(SetTextColor) WINSTUB(SetBkColor) WINSTUB(SetBkMode)
WINSTUB(TextOut) WINSTUB(DrawText) WINSTUB(GetTextExtentPoint32) WINSTUB(Rectangle) WINSTUB(MoveToEx) WINSTUB(LineTo)
WINSTUB(SetPixel) WINSTUB(GetPixel) WINSTUB(SetStretchBltMode) WINSTUB(GetDeviceCaps) WINSTUB(GetObject)
// Registry: always "not found", so every setting keeps its compiled-in default
// (code tests these with == ERROR_SUCCESS, which a zero stub would satisfy).
#define ERROR_FILE_NOT_FOUND 2L
#define REGSTUB(name) template <class... A> inline LONG name(A&&...) { return ERROR_FILE_NOT_FOUND; }
REGSTUB(RegOpenKeyEx) REGSTUB(RegCreateKeyEx) REGSTUB(RegCloseKey) REGSTUB(RegQueryValueEx) REGSTUB(RegSetValueEx)
REGSTUB(RegDeleteValue) REGSTUB(RegDeleteKey) REGSTUB(RegEnumValue) REGSTUB(RegEnumKeyEx)
WINSTUB(GetOpenFileName) WINSTUB(GetSaveFileName) WINSTUB(SHGetFolderPath) WINSTUB(SHGetSpecialFolderPath)
WINSTUB(SHCreateDirectoryEx) WINSTUB(SHBrowseForFolder) WINSTUB(SHGetPathFromIDList) WINSTUB(CoTaskMemFree)
WINSTUB(ShellExecute) WINSTUB(DragAcceptFiles) WINSTUB(DragQueryFile) WINSTUB(DragFinish) WINSTUB(CreateDirectory)
WINSTUB(PathRemoveFileSpec) WINSTUB(PathFileExists) WINSTUB(PathAppend) WINSTUB(PathCombine) WINSTUB(PathFindExtension)
WINSTUB(PathFindFileName) WINSTUB(PathRemoveExtension) WINSTUB(PathIsDirectory)
WINSTUB(LoadLibraryA) WINSTUB(GetModuleHandle)
WINSTUB(CreateThread) WINSTUB(SetThreadPriority) WINSTUB(GetCurrentThread) WINSTUB(WaitForSingleObject)
WINSTUB(CreateEvent) WINSTUB(SetEvent) WINSTUB(ResetEvent) WINSTUB(MsgWaitForMultipleObjects)
WINSTUB(timeBeginPeriod) WINSTUB(timeEndPeriod) WINSTUB(timeSetEvent) WINSTUB(timeKillEvent)
WINSTUB(InitCommonControls) WINSTUB(InitCommonControlsEx) WINSTUB(ImageList_Create) WINSTUB(OpenClipboard)
WINSTUB(CloseClipboard) WINSTUB(EmptyClipboard) WINSTUB(SetClipboardData) WINSTUB(GetClipboardData)
WINSTUB(GlobalAlloc) WINSTUB(GlobalLock) WINSTUB(GlobalUnlock) WINSTUB(GlobalFree) WINSTUB(Beep) WINSTUB(MessageBeep)
WINSTUB(CoInitialize) WINSTUB(CoInitializeEx) WINSTUB(CoUninitialize) WINSTUB(CoCreateInstance)
WINSTUB(DirectDrawCreateEx) WINSTUB(DirectSoundCreate) WINSTUB(DirectSoundCreate8) WINSTUB(DirectInput8Create)
WINSTUB(AVIFileInit) WINSTUB(AVIFileExit) WINSTUB(AVIFileOpen) WINSTUB(AVIFileRelease) WINSTUB(AVIFileCreateStream)
WINSTUB(AVIStreamRelease) WINSTUB(AVIStreamWrite) WINSTUB(AVIStreamSetFormat) WINSTUB(AVIMakeCompressedStream)
WINSTUB(AVISaveOptions) WINSTUB(AVISaveOptionsFree) WINSTUB(acmStreamOpen) WINSTUB(acmStreamClose)
WINSTUB(acmStreamPrepareHeader) WINSTUB(acmStreamUnprepareHeader) WINSTUB(acmStreamConvert) WINSTUB(acmStreamSize)
WINSTUB(acmFormatSuggest) WINSTUB(acmMetrics) WINSTUB(ListView_SetExtendedListViewStyle) WINSTUB(ListView_InsertColumn)
WINSTUB(ListView_InsertItem) WINSTUB(ListView_SetItemText) WINSTUB(ListView_DeleteAllItems) WINSTUB(ListView_GetNextItem)
WINSTUB(ListView_SetItemState) WINSTUB(ListView_GetItemCount) WINSTUB(ListView_SetCheckState) WINSTUB(ListView_GetCheckState)
WINSTUB(GetLastError) WINSTUB(FormatMessage) WINSTUB(LocalFree) WINSTUB(GetTempPath) WINSTUB(GetTempFileName)
WINSTUB(GetFullPathName) WINSTUB(SetCurrentDirectory)
WINSTUB(GetPrivateProfileInt) WINSTUB(GetPrivateProfileString) WINSTUB(WritePrivateProfileString)
WINSTUB(EnumDisplaySettings) WINSTUB(ChangeDisplaySettings) WINSTUB(ComboBox_AddString) WINSTUB(ComboBox_SetCurSel)
WINSTUB(ComboBox_GetCurSel) WINSTUB(Button_GetCheck) WINSTUB(Button_SetCheck)
WINSTUB(DragQueryPoint) WINSTUB(IsWindowEnabled) WINSTUB(GetParent) WINSTUB(SetParent) WINSTUB(BringWindowToTop)
WINSTUB(WindowFromPoint) WINSTUB(ChildWindowFromPoint) WINSTUB(GetWindowDC) WINSTUB(ValidateRect)
WINSTUB(SetClassLongPtr) WINSTUB(GetClassLongPtr) WINSTUB(SetActiveWindow) WINSTUB(GetActiveWindow)
WINSTUB(CreateFile) WINSTUB(ReadFile) WINSTUB(WriteFile) WINSTUB(GetFileSize) WINSTUB(SetFilePointer)
WINSTUB(MulDiv) WINSTUB(SetLayeredWindowAttributes) WINSTUB(GetCommandLine) WINSTUB(CommandLineToArgvW)
WINSTUB(MapWindowPoints) WINSTUB(GetWindowThreadProcessId) WINSTUB(EnumWindows) WINSTUB(FlashWindow)


// ---------------------------------------------------------------- more types/consts
typedef struct WIN32_FIND_DATA { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH], cAlternateFileName[14]; } WIN32_FIND_DATA_;
typedef struct { LONG bmType, bmWidth, bmHeight, bmWidthBytes; WORD bmPlanes, bmBitsPixel; LPVOID bmBits; } BITMAP_;
typedef struct { BITMAP_ dsBm; BITMAPINFOHEADER dsBmih; DWORD dsBitfields[3]; HANDLE dshSection; DWORD dsOffset; } DIBSECTION;
typedef struct { DWORD style, dwExtendedStyle; WORD cdit; short x, y, cx, cy; } DLGTEMPLATE, *LPDLGTEMPLATE;
typedef const DLGTEMPLATE *LPCDLGTEMPLATE;
typedef struct { DWORD style, dwExtendedStyle; short x, y, cx, cy; WORD id; } DLGITEMTEMPLATE;
typedef struct { UINT CtlType, CtlID, itemID, itemAction, itemState; HWND hwndItem; HDC hDC; RECT rcItem; ULONG_PTR itemData; } DRAWITEMSTRUCT, *LPDRAWITEMSTRUCT;
typedef struct { ULONG_PTR dwData; DWORD cbData; PVOID lpData; } COPYDATASTRUCT, *PCOPYDATASTRUCT;
inline bool IsEqualGUID(const GUID &a, const GUID &b) { return !memcmp(&a, &b, sizeof(GUID)); }
inline BOOL SetRect(RECT *r, int l, int t, int rr, int b) { r->left = l; r->top = t; r->right = rr; r->bottom = b; return TRUE; }
inline BOOL OffsetRect(RECT *r, int dx, int dy) { r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return TRUE; }
inline int lstrlenW(const wchar_t *s) { return (int)wcslen(s); }
#define lstrlen lstrlenW
#define lstrcpy wcscpy
#define lstrcmpi wcscasecmp
#define GetFileAttributesW GetFileAttributes
#define _tsplitpath(p, d, dir, f, e) furb_splitpath((p), (d), (dir), (f), (e))
void furb_splitpath(const wchar_t *path, wchar_t *drive, wchar_t *dir, wchar_t *fname, wchar_t *ext);
#define GET_WM_COMMAND_ID(wp, lp) LOWORD(wp)
#define GET_WM_COMMAND_CMD(wp, lp) HIWORD(wp)
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define SelectFont(hdc, f) ((HFONT)SelectObject((hdc), (HGDIOBJ)(f)))
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define REGDB_E_CLASSNOTREG ((HRESULT)0x80040154L)
enum {
	BITSPIXEL = 12, BLACKNESS = 0x42, WHITENESS = 0xFF0062, BS_DEFPUSHBUTTON = 1, CBS_DROPDOWNLIST = 3, CBS_HASSTRINGS = 0x200,
	CLIP_DEFAULT_PRECIS = 0, DEFAULT_PITCH = 0, DEFAULT_QUALITY = 0, FW_DONTCARE = 0, OUT_DEFAULT_PRECIS = 0,
	DM_SETDEFID = 0x401, DS_MODALFRAME = 0x80, DS_SETFONT = 0x40, DS_CENTER = 0x800,
	FILE_ATTRIBUTE_NORMAL = 0x80, FILE_ATTRIBUTE_READONLY = 1, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2,
	GENERIC_READ = (int)0x80000000, GENERIC_WRITE = 0x40000000, OPEN_EXISTING = 3, CREATE_ALWAYS = 2,
	HTCLIENT = 1, LBS_HASSTRINGS = 0x40, LBS_NOTIFY = 1, LB_FINDSTRINGEXACT = 0x1A2, LB_GETTEXTLEN = 0x18A,
	LB_SETTABSTOPS = 0x192, LVM_SETVIEW = 0x108E, LV_VIEW_DETAILS = 1, MAPVK_VK_TO_VSC = 0, MB_APPLMODAL = 0,
	MB_DEFBUTTON1 = 0, OBJ_FONT = 6, SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79, SM_XVIRTUALSCREEN = 76,
	SM_YVIRTUALSCREEN = 77, SWP_HIDEWINDOW = 0x80, SW_MAXIMIZE = 3, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D,
	WM_COPYDATA = 0x4A, WM_GETTEXT = 0xD, WM_HOTKEY = 0x312, WM_SETREDRAW = 0xB, WM_GETTEXTLENGTH = 0xE,
	MOD_ALT = 1, MOD_CONTROL = 2, MOD_SHIFT = 4, MOD_NOREPEAT = 0x4000,
};
// Real implementations (compat.cpp): mapper packs are .so files loaded with dlopen.
struct WIN32_FIND_DATA;
HANDLE FindFirstFile(const wchar_t *pattern, WIN32_FIND_DATA *d);
BOOL FindNextFile(HANDLE h, WIN32_FIND_DATA *d);
BOOL FindClose(HANDLE h);
HMODULE LoadLibrary(const wchar_t *name);
void *GetProcAddress(HMODULE h, const char *name);
BOOL FreeLibrary(HMODULE h);
WINSTUB(CommDlgExtendedError) WINSTUB(DestroyCursor) WINSTUB(DialogBoxIndirect) WINSTUB(DialogBoxIndirectParam)
WINSTUB(ExitThread) WINSTUB(TerminateThread) WINSTUB(GetCurrentObject) WINSTUB(GetCursorPos) WINSTUB(SetCursorPos)
WINSTUB(GetWindowTextLength) WINSTUB(RegisterHotKey) WINSTUB(UnregisterHotKey) WINSTUB(SetFileAttributes)
WINSTUB(CreateDialogIndirect) WINSTUB(CreateDialogIndirectParam)

#endif // __cplusplus
#endif // FURB_COMPAT_WINDOWS_H
