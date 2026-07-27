#include "audio/DeviceEnumerator.h"

#include <windows.h>

#include <mmsystem.h> // WAVEFORMATEX, required before dsound.h
#include <dsound.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <iterator>

#include "audio/ComPtr.h"
#include "audio/WasapiCommon.h"
#include "core/Log.h"
#include "core/Strings.h"

namespace rv::audio {
namespace {

u64 nowMs()
{
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// Rates worth probing for exclusive mode, in the order a user is most likely
/// to want them.
constexpr int kProbeRates[] = {48000, 44100, 96000, 88200, 192000, 176400, 32000, 16000};

struct DirectSoundCollector {
    std::vector<DeviceInfo>* out;
    bool                     capture;
};

BOOL CALLBACK directSoundCallback(LPGUID guid, LPCWSTR description, LPCWSTR, LPVOID context)
{
    auto* collector = static_cast<DirectSoundCollector*>(context);

    // A null GUID is the "primary sound driver" alias. It is skipped because
    // the real endpoint is always enumerated separately, and listing both
    // invites the user to pick a device that silently follows the Windows
    // default instead of the one they chose.
    if (!guid)
        return TRUE;

    LPOLESTR guidText = nullptr;
    if (FAILED(::StringFromCLSID(*guid, &guidText)) || !guidText)
        return TRUE;

    DeviceInfo info;
    info.id      = toUtf8(guidText);
    info.name    = description ? toUtf8(description) : std::string("(unnamed)");
    info.backend = BackendType::DirectSound;
    ::CoTaskMemFree(guidText);

    info.isInput  = collector->capture;
    info.isOutput = !collector->capture;
    // DirectSound does not report channel counts without opening the device;
    // stereo is the safe assumption and the stream negotiates for real.
    info.maxInputChannels  = collector->capture ? 2 : 0;
    info.maxOutputChannels = collector->capture ? 0 : 2;
    info.defaultSampleRate = 48000;
    info.isVirtualCable    = looksLikeVirtualCable(info.name);

    collector->out->push_back(std::move(info));
    return TRUE;
}

} // namespace

bool looksLikeVirtualCable(const std::string& name)
{
    static const char* kPatterns[] = {
        "cable input", "cable output", "vb-audio", "voicemeeter",
        "virtual audio cable", "virtual cable", "vb-cable",
        "line 1 (virtual", "line 2 (virtual", "line 3 (virtual",
    };

    for (const char* pattern : kPatterns) {
        if (icontains(name, pattern))
            return true;
    }
    return false;
}

void DeviceEnumerator::refresh()
{
    std::vector<DeviceInfo> collected;

    enumerateWasapi(collected);
    enumerateDirectSound(collected);
    enumerateAsio(collected);

    std::lock_guard lock(mutex_);
    devices_       = std::move(collected);
    lastRefreshMs_ = nowMs();
}

void DeviceEnumerator::refreshIfStale(int maxAgeMs)
{
    {
        std::lock_guard lock(mutex_);
        if (lastRefreshMs_ != 0 && nowMs() - lastRefreshMs_ < static_cast<u64>(maxAgeMs))
            return;
    }
    refresh();
}

void DeviceEnumerator::enumerateWasapi(std::vector<DeviceInfo>& out)
{
    auto enumerator = wasapi::createEnumerator();
    if (!enumerator) {
        RV_ERROR("WASAPI enumeration failed: no device enumerator");
        return;
    }

    for (const EDataFlow flow : {eCapture, eRender}) {
        // Default endpoint first, so it can be flagged in the list.
        std::string defaultId;
        {
            ComPtr<IMMDevice> defaultDevice;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, defaultDevice.put())))
                defaultId = wasapi::endpointId(defaultDevice.get());
        }

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())))
            continue;

        UINT count = 0;
        if (FAILED(collection->GetCount(&count)))
            continue;

        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put())))
                continue;

            DeviceInfo info;
            info.id      = wasapi::endpointId(device.get());
            info.name    = wasapi::friendlyName(device.get());
            info.backend = BackendType::Wasapi;
            if (info.id.empty())
                continue;
            if (info.name.empty())
                info.name = "(unnamed endpoint)";

            info.isInput   = (flow == eCapture);
            info.isOutput  = (flow == eRender);
            info.isDefault = (info.id == defaultId);
            info.isVirtualCable = looksLikeVirtualCable(info.name);

            // The mix format is the authoritative description of what the
            // endpoint looks like in shared mode.
            ComPtr<IAudioClient> client;
            if (SUCCEEDED(device->Activate(wasapi::kIidIAudioClient, CLSCTX_ALL, nullptr,
                                           client.putVoid()))) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                    info.defaultSampleRate = static_cast<int>(mix->nSamplesPerSec);
                    if (flow == eCapture)
                        info.maxInputChannels = mix->nChannels;
                    else
                        info.maxOutputChannels = mix->nChannels;
                    ::CoTaskMemFree(mix);
                }
            } else {
                info.unavailableReason = "the endpoint could not be activated";
            }

            out.push_back(std::move(info));
        }
    }
}

void DeviceEnumerator::enumerateDirectSound(std::vector<DeviceInfo>& out)
{
    DirectSoundCollector captureCollector{&out, true};
    if (FAILED(::DirectSoundCaptureEnumerateW(directSoundCallback, &captureCollector)))
        RV_WARN("DirectSound capture enumeration failed");

    DirectSoundCollector renderCollector{&out, false};
    if (FAILED(::DirectSoundEnumerateW(directSoundCallback, &renderCollector)))
        RV_WARN("DirectSound render enumeration failed");
}

void DeviceEnumerator::enumerateAsio(std::vector<DeviceInfo>& out)
{
    // ASIO drivers register themselves under HKLM\SOFTWARE\ASIO regardless of
    // whether this build can open them, so the list is read either way: seeing
    // the driver with an explanation attached is far more useful than an empty
    // list that looks like a bug.
    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                        KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return;

    for (DWORD index = 0;; ++index) {
        wchar_t subkeyName[256];
        DWORD   nameLength = static_cast<DWORD>(std::size(subkeyName));

        const LSTATUS status =
            ::RegEnumKeyExW(root, index, subkeyName, &nameLength,
                            nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
            break;

        HKEY subkey = nullptr;
        if (::RegOpenKeyExW(root, subkeyName, 0, KEY_READ | KEY_WOW64_64KEY, &subkey) !=
            ERROR_SUCCESS)
            continue;

        wchar_t description[256] = {};
        DWORD   descriptionBytes = sizeof(description);
        DWORD   type             = 0;
        if (::RegQueryValueExW(subkey, L"Description", nullptr, &type,
                               reinterpret_cast<LPBYTE>(description),
                               &descriptionBytes) != ERROR_SUCCESS || type != REG_SZ) {
            // Some drivers omit Description and are identified by the key name.
            wcsncpy(description, subkeyName, std::size(description) - 1);
        }

        wchar_t clsid[64] = {};
        DWORD   clsidBytes = sizeof(clsid);
        const bool haveClsid =
            ::RegQueryValueExW(subkey, L"CLSID", nullptr, nullptr,
                               reinterpret_cast<LPBYTE>(clsid), &clsidBytes) == ERROR_SUCCESS;

        ::RegCloseKey(subkey);

        if (!haveClsid)
            continue;

        DeviceInfo info;
        // The driver name is the identifier the ASIO host API works with;
        // the CLSID is only needed internally to instantiate it.
        info.id      = toUtf8(subkeyName);
        info.name    = toUtf8(description);
        info.backend = BackendType::Asio;
        info.isInput = true;
        info.isOutput = true;
        info.defaultSampleRate = 48000;
        // Real channel counts require opening the driver, which would seize it
        // from whatever else is using it. Reported once the stream opens.
        info.maxInputChannels  = 2;
        info.maxOutputChannels = 2;

#if !RV_HAS_ASIO
        info.unavailableReason =
            "this build was compiled without the ASIO SDK "
            "(configure with -DRV_ENABLE_ASIO=ON -DRV_ASIO_SDK_DIR=...)";
#endif

        out.push_back(std::move(info));
    }

    ::RegCloseKey(root);
}

std::vector<DeviceInfo> DeviceEnumerator::inputs(BackendType backend) const
{
    std::lock_guard lock(mutex_);

    std::vector<DeviceInfo> result;
    for (const auto& device : devices_) {
        if (device.backend == backend && device.isInput)
            result.push_back(device);
    }
    return result;
}

std::vector<DeviceInfo> DeviceEnumerator::outputs(BackendType backend) const
{
    std::lock_guard lock(mutex_);

    std::vector<DeviceInfo> result;
    for (const auto& device : devices_) {
        if (device.backend == backend && device.isOutput)
            result.push_back(device);
    }
    return result;
}

std::vector<DeviceInfo> DeviceEnumerator::all() const
{
    std::lock_guard lock(mutex_);
    return devices_;
}

std::optional<DeviceInfo> DeviceEnumerator::find(const DeviceId& id, BackendType backend) const
{
    std::lock_guard lock(mutex_);
    for (const auto& device : devices_) {
        if (device.backend == backend && device.id == id)
            return device;
    }
    return std::nullopt;
}

std::vector<DeviceInfo> DeviceEnumerator::virtualCableOutputs() const
{
    std::lock_guard lock(mutex_);

    std::vector<DeviceInfo> result;
    for (const auto& device : devices_) {
        if (device.isOutput && device.isVirtualCable && device.backend == BackendType::Wasapi)
            result.push_back(device);
    }
    return result;
}

bool DeviceEnumerator::anyVirtualCableInstalled() const
{
    std::lock_guard lock(mutex_);
    return std::any_of(devices_.begin(), devices_.end(),
                       [](const DeviceInfo& d) { return d.isVirtualCable; });
}

std::vector<int> DeviceEnumerator::probeExclusiveRates(const DeviceId& id, int channels)
{
    std::vector<int> supported;

    auto enumerator = wasapi::createEnumerator();
    if (!enumerator)
        return supported;

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(toWide(id).c_str(), device.put())))
        return supported;

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(wasapi::kIidIAudioClient, CLSCTX_ALL, nullptr, client.putVoid())))
        return supported;

    for (int rate : kProbeRates) {
        // Any format the device accepts at this rate makes the rate usable;
        // the stream picks the best one when it actually opens.
        for (SampleFormat format : {SampleFormat::Float32, SampleFormat::Int32,
                                    SampleFormat::Int24, SampleFormat::Int16}) {
            WAVEFORMATEXTENSIBLE probe = wasapi::makeFormat(format, channels, rate);
            WAVEFORMATEX* closest = nullptr;
            const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                         &probe.Format, &closest);
            if (closest)
                ::CoTaskMemFree(closest);

            if (hr == S_OK) {
                supported.push_back(rate);
                break;
            }
        }
    }

    std::sort(supported.begin(), supported.end());
    return supported;
}

} // namespace rv::audio
