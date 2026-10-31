#pragma once
#include <CoreAudio/CoreAudio.h>
#include <string>
#include <vector>

namespace rv::audio::coreaudio {
inline AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template<class T> bool read(AudioObjectID object, AudioObjectPropertySelector selector,
    T& value, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    auto a = address(selector, scope);
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &a, 0, nullptr, &size, &value) == noErr;
}
inline std::string stringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
    CFStringRef value = nullptr;
    if (!read(object, selector, value) || !value) return {};
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> bytes(static_cast<size_t>(size));
    const bool ok = CFStringGetCString(value, bytes.data(), size, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok ? std::string(bytes.data()) : std::string{};
}
inline int channels(AudioDeviceID device, bool input)
{
    auto a = address(kAudioDevicePropertyStreamConfiguration,
        input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &a, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList)) return 0;
    std::vector<char> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, list) != noErr) return 0;
    int count = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) count += list->mBuffers[i].mNumberChannels;
    return count;
}
inline AudioDeviceID resolve(const std::string& uid)
{
    CFStringRef value = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (!value) return kAudioObjectUnknown;
    AudioDeviceID device = kAudioObjectUnknown;
    auto a = address(kAudioHardwarePropertyTranslateUIDToDevice);
    UInt32 size = sizeof(device);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(value), &value, &size, &device);
    CFRelease(value);
    return device;
}
}
