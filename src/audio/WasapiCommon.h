#pragma once

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <string>

#include "audio/ComPtr.h"
#include "audio/SampleFormat.h"
#include "core/Types.h"

namespace rv::audio::wasapi {

// The GUIDs below are spelled out rather than pulled from ksmedia.h /
// functiondiscoverykeys_devpkey.h because those headers only define the
// symbols when INITGUID is set, and which import library actually carries them
// differs between the MSVC and MinGW toolchains. Writing them here makes the
// build independent of both.

extern const CLSID kClsidMMDeviceEnumerator;
extern const IID   kIidIMMDeviceEnumerator;
extern const IID   kIidIAudioClient;
extern const IID   kIidIAudioCaptureClient;
extern const IID   kIidIAudioRenderClient;
extern const IID   kIidIAudioClock;

extern const GUID  kSubtypeIeeeFloat;
extern const GUID  kSubtypePcm;

extern const PROPERTYKEY kPkeyDeviceFriendlyName;

/// Creates the process-wide device enumerator. The caller must already be on a
/// thread with COM initialised.
ComPtr<IMMDeviceEnumerator> createEnumerator();

/// Reads the endpoint's friendly name; returns an empty string on failure.
std::string friendlyName(IMMDevice* device);

/// Endpoint id string, as used for `DeviceInfo::id`.
std::string endpointId(IMMDevice* device);

/// Maps a negotiated WAVEFORMATEX onto our internal sample format enum.
/// Returns Unknown for formats we cannot convert.
SampleFormat formatOf(const WAVEFORMATEX* format);

/// Builds a WAVEFORMATEXTENSIBLE for the given layout. `format` must be one of
/// Float32, Int32, Int24 or Int16.
WAVEFORMATEXTENSIBLE makeFormat(SampleFormat format, int channels, int sampleRate);

/// Best-effort human-readable rendering of a WASAPI HRESULT, including the
/// AUDCLNT_* codes that the system message table does not cover.
std::string describeHresult(HRESULT hr);

/// Marks the calling thread as latency-critical audio work so the scheduler
/// stops treating it like a background task. Returns a handle to be passed to
/// `revertProAudio`, or nullptr when MMCSS is unavailable.
HANDLE enterProAudio();
void   revertProAudio(HANDLE handle);

} // namespace rv::audio::wasapi
