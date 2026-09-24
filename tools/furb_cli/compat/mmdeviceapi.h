#pragma once
#include <windows.h>
typedef WinCOM IMMDeviceEnumerator, IMMDevice;
static const GUID CLSID_MMDeviceEnumerator = {0};
static const GUID IID_IMMDeviceEnumerator = {0};
enum EDataFlow { eRender, eCapture, eAll };
enum ERole { eConsole, eMultimedia, eCommunications };
enum { CLSCTX_ALL = 0x17, CLSCTX_INPROC_SERVER = 1, COINIT_MULTITHREADED = 0, COINIT_APARTMENTTHREADED = 2 };
struct MMDeviceEnumerator;
#define __uuidof(x) (*(const GUID *)0)
