#include "audio/DeviceEnumerator.h"
#include "platform/macos/CoreAudioDevice.h"
#include "core/Strings.h"
#include <algorithm>
#include <chrono>

namespace rv::audio {
namespace {
u64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
bool looksLikeVirtualCable(const std::string& name)
{
    return icontains(name, "BlackHole") || icontains(name, "Loopback") ||
        icontains(name, "VB-Cable") || icontains(name, "RadioVoice");
}
void DeviceEnumerator::refresh()
{
    using namespace coreaudio;
    auto a = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    std::vector<DeviceInfo> found;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) == noErr) {
        std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) == noErr) {
            AudioDeviceID defaultInput = 0, defaultOutput = 0;
            read(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, defaultInput);
            read(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, defaultOutput);
            for (auto id : ids) {
                const auto uid = stringProperty(id, kAudioDevicePropertyDeviceUID);
                if (uid.empty()) continue;
                const int ins = channels(id, true), outs = channels(id, false);
                Float64 rate = 48000;
                read(id, kAudioDevicePropertyNominalSampleRate, rate);
                for (bool input : {true, false}) {
                    if ((input ? ins : outs) == 0) continue;
                    DeviceInfo info;
                    info.id = uid;
                    info.name = stringProperty(id, kAudioObjectPropertyName);
                    info.backend = BackendType::CoreAudio;
                    info.isInput = input;
                    info.isOutput = !input;
                    info.isDefault = id == (input ? defaultInput : defaultOutput);
                    info.maxInputChannels = ins;
                    info.maxOutputChannels = outs;
                    info.defaultSampleRate = static_cast<int>(rate);
                    info.isVirtualCable = looksLikeVirtualCable(info.name);
                    found.push_back(std::move(info));
                }
            }
        }
    }
    std::lock_guard lock(mutex_);
    devices_ = std::move(found);
    lastRefreshMs_ = nowMs();
}
void DeviceEnumerator::refreshIfStale(int maxAgeMs)
{
    { std::lock_guard lock(mutex_);
      if (lastRefreshMs_ && nowMs() - lastRefreshMs_ < static_cast<u64>(maxAgeMs)) return; }
    refresh();
}
std::vector<DeviceInfo> DeviceEnumerator::all() const
{
    std::lock_guard lock(mutex_); return devices_;
}
std::vector<DeviceInfo> DeviceEnumerator::inputs(BackendType backend) const
{
    auto list = all();
    std::erase_if(list, [backend](const auto& d) { return !d.isInput || d.backend != backend; });
    return list;
}
std::vector<DeviceInfo> DeviceEnumerator::outputs(BackendType backend) const
{
    auto list = all();
    std::erase_if(list, [backend](const auto& d) { return !d.isOutput || d.backend != backend; });
    return list;
}
std::optional<DeviceInfo> DeviceEnumerator::find(const DeviceId& id, BackendType backend) const
{
    for (const auto& d : all()) if (d.id == id && d.backend == backend) return d;
    return std::nullopt;
}
std::vector<DeviceInfo> DeviceEnumerator::virtualCableOutputs() const
{
    auto list = all();
    std::erase_if(list, [](const auto& d) { return !d.isOutput || !d.isVirtualCable; });
    return list;
}
bool DeviceEnumerator::anyVirtualCableInstalled() const { return !virtualCableOutputs().empty(); }
std::vector<int> DeviceEnumerator::probeExclusiveRates(const DeviceId&, int) { return {}; }
}
