#pragma once
#include <windows.h>
enum { WAVE_FORMAT_PCM = 1, WAVE_FORMAT_IEEE_FLOAT = 3, TIME_PERIODIC = 1, TIMERR_NOERROR = 0 };
typedef UINT MMRESULT;
typedef struct { WAVEFORMATEX Format; union { WORD wValidBitsPerSample; } Samples; DWORD dwChannelMask; GUID SubFormat; } WAVEFORMATEXTENSIBLE;
#define mmioFOURCC(a,b,c,d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
DECLARE_HANDLE(HWAVEIN);
typedef struct { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded; DWORD_PTR dwUser; DWORD dwFlags, dwLoops; void *lpNext; DWORD_PTR reserved; } WAVEHDR;
enum { WAVE_MAPPER = -1, CALLBACK_NULL = 0, CALLBACK_FUNCTION = 0x30000, WHDR_DONE = 1 };
WINSTUB(waveInOpen) WINSTUB(waveInClose) WINSTUB(waveInStart) WINSTUB(waveInStop) WINSTUB(waveInReset)
WINSTUB(waveInPrepareHeader) WINSTUB(waveInUnprepareHeader) WINSTUB(waveInAddBuffer)
