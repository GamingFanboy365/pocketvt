#pragma once
#include <mmsystem.h>
DECLARE_HANDLE(HACMSTREAM);
typedef struct { DWORD cbStruct, fdwStatus; DWORD_PTR dwUser; LPBYTE pbSrc; DWORD cbSrcLength, cbSrcLengthUsed; DWORD_PTR dwSrcUser;
	LPBYTE pbDst; DWORD cbDstLength, cbDstLengthUsed; DWORD_PTR dwDstUser; DWORD dwReservedDriver[10]; } ACMSTREAMHEADER;
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize;
	WORD wID; DWORD fdwFlags; WORD nBlockSize, nFramesPerBlock, nCodecDelay; } MPEGLAYER3WAVEFORMAT;
enum { ACM_STREAMSIZEF_SOURCE = 0, ACM_STREAMCONVERTF_BLOCKALIGN = 4, ACM_METRIC_MAX_SIZE_FORMAT = 50,
	ACM_FORMATSUGGESTF_WFORMATTAG = 0x10000, WAVE_FORMAT_MPEGLAYER3 = 0x55, MPEGLAYER3_ID_MPEG = 1,
	MPEGLAYER3_FLAG_PADDING_OFF = 2, MPEGLAYER3_WFX_EXTRA_BYTES = 12, ACM_STREAMCONVERTF_END = 0x20 };
