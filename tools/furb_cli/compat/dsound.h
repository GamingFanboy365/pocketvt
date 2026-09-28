#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <vector>
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; LPWAVEFORMATEX lpwfxFormat; GUID guid3DAlgorithm; } DSBUFFERDESC;
enum { DSBCAPS_PRIMARYBUFFER = 1, DSBCAPS_GLOBALFOCUS = 0x8000, DSBCAPS_GETCURRENTPOSITION2 = 0x10000,
	DSBCAPS_CTRLVOLUME = 0x80, DSBCAPS_LOCSOFTWARE = 8, DSSCL_PRIORITY = 2, DSSCL_NORMAL = 1,
	DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBLOCK_ENTIREBUFFER = 2, DSBLOCK_FROMWRITECURSOR = 1 };
#define DSERR_BUFFERLOST ((HRESULT)0x88780096L)

// A capture-only DirectSound (compat.cpp).  DirectSoundCreate hands one out
// only when furb_cli is recording audio (--wav/--avi); Sound.cpp then mixes
// exactly as on Windows, and every frame's Lock/Unlock delivers the samples
// the speakers would have played to the WAV/AVI writer.  The play cursor
// always reads "far ahead", so Sound::Run never waits.
struct FurbDSBuffer : WinCOM {
	bool primary = false, clearing = false;
	std::vector<unsigned char> mem;
	HRESULT SetFormat(const WAVEFORMATEX *) { return S_OK; }
	HRESULT Play(DWORD, DWORD, DWORD) { return S_OK; }
	HRESULT Stop(void) { return S_OK; }
	HRESULT GetCurrentPosition(DWORD *play, DWORD *write);
	HRESULT Lock(DWORD offset, DWORD bytes, void **ptr1, DWORD *len1, void **ptr2, DWORD *len2, DWORD flags);
	HRESULT Unlock(void *ptr1, DWORD len1, void *ptr2, DWORD len2);
	HRESULT GetStatus(DWORD *s) { *s = DSBSTATUS_PLAYING; return S_OK; }
	ULONG Release(void) { delete this; return 0; }
};
struct FurbDS : WinCOM {
	HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
	HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, FurbDSBuffer **out, void *);
	ULONG Release(void) { delete this; return 0; }
};
typedef FurbDS IDirectSound, *LPDIRECTSOUND, *LPDIRECTSOUND8;
typedef FurbDSBuffer IDirectSoundBuffer, *LPDIRECTSOUNDBUFFER, *LPDIRECTSOUNDBUFFER8;
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *);
#define DirectSoundCreate8 DirectSoundCreate
