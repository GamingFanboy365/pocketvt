#pragma once
#include <windows.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
typedef WinCOM IDirectInput8, *LPDIRECTINPUT8, IDirectInputDevice8, *LPDIRECTINPUTDEVICE8;
typedef struct { LONG lX, lY, lZ, lRx, lRy, lRz; LONG rglSlider[2]; DWORD rgdwPOV[4]; BYTE rgbButtons[128];
	LONG lVX, lVY, lVZ, lVRx, lVRy, lVRz; LONG rglVSlider[2]; LONG lAX, lAY, lAZ, lARx, lARy, lARz; LONG rglASlider[2];
	LONG lFX, lFY, lFZ, lFRx, lFRy, lFRz; LONG rglFSlider[2]; } DIJOYSTATE2;
typedef struct { LONG lX, lY, lZ; BYTE rgbButtons[8]; } DIMOUSESTATE2;
typedef struct { DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, dwFFSamplePeriod, dwFFMinTimeResolution,
	dwFirmwareRevision, dwHardwareRevision, dwFFDriverVersion; } DIDEVCAPS;
typedef struct { DWORD dwSize; GUID guidInstance, guidProduct; DWORD dwDevType; WCHAR tszInstanceName[MAX_PATH], tszProductName[MAX_PATH];
	GUID guidFFDriver; WORD wUsagePage, wUsage; } DIDEVICEINSTANCE, *LPDIDEVICEINSTANCE;
typedef const DIDEVICEINSTANCE *LPCDIDEVICEINSTANCE;
typedef struct { DWORD dwSize; GUID guidType; DWORD dwOfs, dwType, dwFlags; WCHAR tszName[MAX_PATH];
	DWORD dwFFMaxForce, dwFFForceResolution; WORD wCollectionNumber, wDesignatorIndex, wUsagePage, wUsage; DWORD dwDimension;
	WORD wExponent, wReportId; } DIDEVICEOBJECTINSTANCE, *LPDIDEVICEOBJECTINSTANCE;
typedef const DIDEVICEOBJECTINSTANCE *LPCDIDEVICEOBJECTINSTANCE;
typedef struct { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } DIPROPHEADER;
typedef struct { DIPROPHEADER diph; LONG lMin, lMax; } DIPROPRANGE;
typedef struct { DIPROPHEADER diph; DWORD dwData; } DIPROPDWORD;
typedef struct { DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs; void *rgodf; } DIDATAFORMAT;
extern const DIDATAFORMAT c_dfDIKeyboard, c_dfDIMouse2, c_dfDIJoystick2;
static const GUID GUID_SysKeyboard = {0}, GUID_SysMouse = {0}, IID_IDirectInput8 = {0}, GUID_XAxis = {0}, GUID_YAxis = {0},
	GUID_ZAxis = {0}, GUID_RxAxis = {0}, GUID_RyAxis = {0}, GUID_RzAxis = {0}, GUID_Slider = {0}, GUID_POV = {0}, GUID_Button = {0}, GUID_Key = {0};
enum { DIENUM_CONTINUE = 1, DIENUM_STOP = 0, DISCL_NONEXCLUSIVE = 2, DISCL_EXCLUSIVE = 1, DISCL_FOREGROUND = 4,
	DISCL_BACKGROUND = 8, DI8DEVCLASS_GAMECTRL = 4, DI8DEVCLASS_ALL = 0, DIEDFL_ATTACHEDONLY = 1, DIDFT_AXIS = 3,
	DIDFT_BUTTON = 0xC, DIDFT_POV = 0x10, DIDFT_ALL = 0, DIPH_DEVICE = 0, DIPH_BYID = 2, DIPH_BYOFFSET = 1,
	DI8DEVTYPE_KEYBOARD = 0x13, DI8DEVTYPE_MOUSE = 0x12, DIDC_ATTACHED = 1 };
#define DIPROP_RANGE (*(const GUID *)4)
#define DIPROP_DEADZONE (*(const GUID *)5)
#define DIPROP_AXISMODE (*(const GUID *)2)
#define DIERR_INPUTLOST ((HRESULT)0x8007001EL)
#define DIERR_NOTACQUIRED ((HRESULT)0x8007000CL)
#define DIDFT_GETTYPE(n) LOBYTE(n)
#define DIDFT_GETINSTANCE(n) LOWORD((n) >> 8)
#define GET_DIDEVICE_TYPE(dwDevType) LOBYTE(dwDevType)
typedef BOOL (*LPDIENUMDEVICESCALLBACK)(LPCDIDEVICEINSTANCE, LPVOID);
typedef BOOL (*LPDIENUMDEVICEOBJECTSCALLBACK)(LPCDIDEVICEOBJECTINSTANCE, LPVOID);
enum {
	DIK_ESCAPE = 0x01,
	DIK_1 = 0x02,
	DIK_2 = 0x03,
	DIK_3 = 0x04,
	DIK_4 = 0x05,
	DIK_5 = 0x06,
	DIK_6 = 0x07,
	DIK_7 = 0x08,
	DIK_8 = 0x09,
	DIK_9 = 0x0A,
	DIK_0 = 0x0B,
	DIK_MINUS = 0x0C,
	DIK_EQUALS = 0x0D,
	DIK_BACK = 0x0E,
	DIK_BACKSPACE = 0x0E,
	DIK_TAB = 0x0F,
	DIK_Q = 0x10,
	DIK_W = 0x11,
	DIK_E = 0x12,
	DIK_R = 0x13,
	DIK_T = 0x14,
	DIK_Y = 0x15,
	DIK_U = 0x16,
	DIK_I = 0x17,
	DIK_O = 0x18,
	DIK_P = 0x19,
	DIK_LBRACKET = 0x1A,
	DIK_RBRACKET = 0x1B,
	DIK_RETURN = 0x1C,
	DIK_LCONTROL = 0x1D,
	DIK_A = 0x1E,
	DIK_S = 0x1F,
	DIK_D = 0x20,
	DIK_F = 0x21,
	DIK_G = 0x22,
	DIK_H = 0x23,
	DIK_J = 0x24,
	DIK_K = 0x25,
	DIK_L = 0x26,
	DIK_SEMICOLON = 0x27,
	DIK_APOSTROPHE = 0x28,
	DIK_GRAVE = 0x29,
	DIK_LSHIFT = 0x2A,
	DIK_BACKSLASH = 0x2B,
	DIK_Z = 0x2C,
	DIK_X = 0x2D,
	DIK_C = 0x2E,
	DIK_V = 0x2F,
	DIK_B = 0x30,
	DIK_N = 0x31,
	DIK_M = 0x32,
	DIK_COMMA = 0x33,
	DIK_PERIOD = 0x34,
	DIK_SLASH = 0x35,
	DIK_RSHIFT = 0x36,
	DIK_MULTIPLY = 0x37,
	DIK_LMENU = 0x38,
	DIK_LALT = 0x38,
	DIK_SPACE = 0x39,
	DIK_CAPITAL = 0x3A,
	DIK_F1 = 0x3B,
	DIK_F2 = 0x3C,
	DIK_F3 = 0x3D,
	DIK_F4 = 0x3E,
	DIK_F5 = 0x3F,
	DIK_F6 = 0x40,
	DIK_F7 = 0x41,
	DIK_F8 = 0x42,
	DIK_F9 = 0x43,
	DIK_F10 = 0x44,
	DIK_NUMLOCK = 0x45,
	DIK_SCROLL = 0x46,
	DIK_NUMPAD7 = 0x47,
	DIK_NUMPAD8 = 0x48,
	DIK_NUMPAD9 = 0x49,
	DIK_SUBTRACT = 0x4A,
	DIK_NUMPAD4 = 0x4B,
	DIK_NUMPAD5 = 0x4C,
	DIK_NUMPAD6 = 0x4D,
	DIK_ADD = 0x4E,
	DIK_NUMPAD1 = 0x4F,
	DIK_NUMPAD2 = 0x50,
	DIK_NUMPAD3 = 0x51,
	DIK_NUMPAD0 = 0x52,
	DIK_DECIMAL = 0x53,
	DIK_F11 = 0x57,
	DIK_F12 = 0x58,
	DIK_NUMPADENTER = 0x9C,
	DIK_RCONTROL = 0x9D,
	DIK_DIVIDE = 0xB5,
	DIK_RMENU = 0xB8,
	DIK_RALT = 0xB8,
	DIK_PAUSE = 0xC5,
	DIK_HOME = 0xC7,
	DIK_UP = 0xC8,
	DIK_PRIOR = 0xC9,
	DIK_PGUP = 0xC9,
	DIK_LEFT = 0xCB,
	DIK_RIGHT = 0xCD,
	DIK_END = 0xCF,
	DIK_DOWN = 0xD0,
	DIK_NEXT = 0xD1,
	DIK_PGDN = 0xD1,
	DIK_INSERT = 0xD2,
	DIK_DELETE = 0xD3,
	DIK_LWIN = 0xDB,
	DIK_RWIN = 0xDC,
	DIK_APPS = 0xDD,
	DIK_YEN = 0x7D,
	DIK_CONVERT = 0x79,
	DIK_NOCONVERT = 0x7B,
	DIK_KANA = 0x70,
	DIK_CIRCUMFLEX = 0x90,
	DIK_AT = 0x91,
	DIK_COLON = 0x92,
	DIK_UNDERLINE = 0x93,
	DIK_KANJI = 0x94,
	DIK_STOP = 0x95,
	DIK_NUMPADEQUALS = 0x8D,
	DIK_NUMPADCOMMA = 0xB3,
	DIK_SYSRQ = 0xB7,
	DIK_OEM_102 = 0x56,
	DIK_F13 = 0x64,
	DIK_F14 = 0x65,
	DIK_F15 = 0x66,
	DIEDFL_ALLDEVICES = 0,
};
