#pragma once
#include <mmdeviceapi.h>
// Karaoke microphone (Hardware/h_Mic.cpp): the level is scripted (mic-level).
struct FurbMeter : WinCOM {
	HRESULT GetPeakValue(float *p) { *p = furb_mic_level(); return S_OK; }
};
typedef FurbMeter IAudioMeterInformation;
static const GUID IID_IAudioEndpointVolume = {0};
typedef WinCOM IAudioEndpointVolume;
