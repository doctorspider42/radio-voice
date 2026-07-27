#include "audio/WasapiCommon.h"

#include <avrt.h>
#include <propvarutil.h>

#include <cstdio>

#include "core/Strings.h"

namespace rv::audio::wasapi {

const CLSID kClsidMMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidIAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidIAudioCaptureClient =
    {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
const IID kIidIAudioRenderClient =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIidIAudioClock =
    {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

const GUID kSubtypeIeeeFloat =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypePcm =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

const PROPERTYKEY kPkeyDeviceFriendlyName =
    {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

ComPtr<IMMDeviceEnumerator> createEnumerator()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = ::CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                          kIidIMMDeviceEnumerator, enumerator.putVoid());
    if (FAILED(hr))
        enumerator.reset();
    return enumerator;
}

std::string friendlyName(IMMDevice* device)
{
    if (!device)
        return {};

    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put())))
        return {};

    PROPVARIANT value;
    ::PropVariantInit(&value);

    std::string name;
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = toUtf8(value.pwszVal);
    }

    ::PropVariantClear(&value);
    return name;
}

std::string endpointId(IMMDevice* device)
{
    if (!device)
        return {};

    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw)
        return {};

    std::string id = toUtf8(raw);
    ::CoTaskMemFree(raw);
    return id;
}

SampleFormat formatOf(const WAVEFORMATEX* format)
{
    if (!format)
        return SampleFormat::Unknown;

    GUID subtype{};
    int  validBits = format->wBitsPerSample;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        subtype   = ext->SubFormat;
        validBits = ext->Samples.wValidBitsPerSample;
        if (validBits == 0)
            validBits = format->wBitsPerSample;
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subtype = kSubtypeIeeeFloat;
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        subtype = kSubtypePcm;
    } else {
        return SampleFormat::Unknown;
    }

    if (::IsEqualGUID(subtype, kSubtypeIeeeFloat))
        return format->wBitsPerSample == 32 ? SampleFormat::Float32 : SampleFormat::Unknown;

    if (::IsEqualGUID(subtype, kSubtypePcm)) {
        switch (format->wBitsPerSample) {
            case 16: return SampleFormat::Int16;
            case 24: return SampleFormat::Int24;
            case 32: return validBits == 24 ? SampleFormat::Int32In24 : SampleFormat::Int32;
            default: return SampleFormat::Unknown;
        }
    }

    return SampleFormat::Unknown;
}

WAVEFORMATEXTENSIBLE makeFormat(SampleFormat format, int channels, int sampleRate)
{
    int bits      = 32;
    int validBits = 32;
    switch (format) {
        case SampleFormat::Int16:     bits = 16; validBits = 16; break;
        case SampleFormat::Int24:     bits = 24; validBits = 24; break;
        case SampleFormat::Int32In24: bits = 32; validBits = 24; break;
        case SampleFormat::Int32:     bits = 32; validBits = 32; break;
        case SampleFormat::Float32:   bits = 32; validBits = 32; break;
        case SampleFormat::Unknown:   break;
    }

    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels       = static_cast<WORD>(channels);
    wf.Format.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    wf.Format.wBitsPerSample  = static_cast<WORD>(bits);
    wf.Format.nBlockAlign     = static_cast<WORD>(channels * bits / 8);
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    wf.Samples.wValidBitsPerSample = static_cast<WORD>(validBits);
    wf.SubFormat = (format == SampleFormat::Float32) ? kSubtypeIeeeFloat : kSubtypePcm;

    // Channel masks beyond stereo are guesswork without knowing the device
    // layout; leaving it at 0 lets the driver apply its own default, which is
    // what every well-behaved endpoint does.
    wf.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER
                     : (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                       : 0;

    return wf;
}

std::string describeHresult(HRESULT hr)
{
    switch (hr) {
        case S_OK:                                 return "OK";
        case AUDCLNT_E_NOT_INITIALIZED:            return "audio client not initialised";
        case AUDCLNT_E_ALREADY_INITIALIZED:        return "audio client already initialised";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE:        return "wrong endpoint type";
        case AUDCLNT_E_DEVICE_INVALIDATED:         return "device was removed or reconfigured";
        case AUDCLNT_E_NOT_STOPPED:                return "stream not stopped";
        case AUDCLNT_E_BUFFER_TOO_LARGE:           return "buffer too large";
        case AUDCLNT_E_OUT_OF_ORDER:               return "calls out of order";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:         return "device does not support this format";
        case AUDCLNT_E_INVALID_SIZE:               return "invalid buffer size";
        case AUDCLNT_E_DEVICE_IN_USE:              return "device is already in exclusive use by another application";
        case AUDCLNT_E_BUFFER_OPERATION_PENDING:   return "buffer operation pending";
        case AUDCLNT_E_THREAD_NOT_REGISTERED:      return "thread not registered with MMCSS";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "exclusive mode is disabled for this endpoint in Windows sound settings";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:     return "endpoint creation failed";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:        return "the Windows Audio service is not running";
        case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED:   return "event handle not expected";
        case AUDCLNT_E_EXCLUSIVE_MODE_ONLY:        return "endpoint supports exclusive mode only";
        case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL: return "buffer duration must equal the device period in exclusive event-driven mode";
        case AUDCLNT_E_EVENTHANDLE_NOT_SET:        return "event handle not set";
        case AUDCLNT_E_INCORRECT_BUFFER_SIZE:      return "incorrect buffer size";
        case AUDCLNT_E_BUFFER_SIZE_ERROR:          return "buffer size error";
        case AUDCLNT_E_CPUUSAGE_EXCEEDED:          return "CPU usage limit exceeded";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:    return "buffer size not aligned";
        case E_INVALIDARG:                         return "invalid argument";
        case E_OUTOFMEMORY:                        return "out of memory";
        case E_POINTER:                            return "null pointer";
        default: break;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

HANDLE enterProAudio()
{
    DWORD taskIndex = 0;
    // "Pro Audio" is the highest MMCSS class; without it the thread is subject
    // to ordinary scheduling and will miss deadlines under load.
    HANDLE handle = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (handle)
        ::AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL);
    return handle;
}

void revertProAudio(HANDLE handle)
{
    if (handle)
        ::AvRevertMmThreadCharacteristics(handle);
}

} // namespace rv::audio::wasapi
