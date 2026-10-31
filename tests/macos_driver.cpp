// Exercise the actual HAL plug-in without root installation or microphone access.
#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "Identity.h"

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static AudioServerPlugInDriverRef driver;
static AudioObjectPropertyAddress addr(AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
template<class T> static T read(AudioObjectID object, AudioObjectPropertySelector selector) {
    T value{}; UInt32 size = sizeof(value); auto a = addr(selector);
    require((*driver)->GetPropertyData(driver, object, 0, &a, 0, nullptr, size, &size, &value) == noErr, "property read failed");
    return value;
}
static void stringEquals(AudioObjectID id, AudioObjectPropertySelector property, CFStringRef expected) {
    auto value = read<CFStringRef>(id, property);
    require(value && CFEqual(value, expected), "unexpected device string");
    CFRelease(value);
}
static std::vector<AudioObjectID> list(AudioObjectID id, AudioObjectPropertySelector property,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    auto a = addr(property, scope); UInt32 size = 0;
    require((*driver)->GetPropertyDataSize(driver, id, 0, &a, 0, nullptr, &size) == noErr, "list size failed");
    if (!size) return {};
    std::vector<AudioObjectID> values(size / sizeof(AudioObjectID));
    require((*driver)->GetPropertyData(driver, id, 0, &a, 0, nullptr, size, &size, values.data()) == noErr, "list read failed");
    return values;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected driver executable path");
        void* bundle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!bundle) throw std::runtime_error(dlerror());
        using Factory = void* (*)(CFAllocatorRef, CFUUIDRef);
        auto factory = reinterpret_cast<Factory>(dlsym(bundle, "RadioVoice_Create"));
        require(factory, "RadioVoice factory missing");
        driver = static_cast<AudioServerPlugInDriverRef>(factory(nullptr, kAudioServerPlugInTypeUUID));
        require(driver, "unsupported HAL interface");
        AudioServerPlugInHostInterface host{};
        host.CopyFromStorage = [](AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef* out) -> OSStatus { *out = nullptr; return noErr; };
        require((*driver)->Initialize(driver, &host) == noErr, "initialize failed");
        auto devices = list(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
        require(devices.size() == 2, "expected exactly two devices");
        const auto input = devices[0], output = devices[1];
        auto translate = addr(kAudioPlugInPropertyTranslateUIDToDevice);
        for (auto uid : {CFSTR(RV_AUDIO_INPUT_UID), CFSTR(RV_AUDIO_OUTPUT_UID), CFSTR("unknown_UID")}) {
            AudioObjectID resolved = 999;
            UInt32 size = sizeof(resolved);
            require((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &translate,
                sizeof(uid), &uid, size, &size, &resolved) == noErr, "UID translation failed");
            const auto expected = CFEqual(uid, CFSTR(RV_AUDIO_INPUT_UID)) ? input :
                CFEqual(uid, CFSTR(RV_AUDIO_OUTPUT_UID)) ? output : kAudioObjectUnknown;
            require(resolved == expected, "UID resolved to wrong device");
            require((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0, &translate,
                0, &uid, size, &size, &resolved) == kAudioHardwareBadPropertySizeError, "malformed UID qualifier accepted");
        }
        stringEquals(input, kAudioObjectPropertyName, CFSTR(RV_AUDIO_NAME));
        stringEquals(output, kAudioObjectPropertyName, CFSTR(RV_AUDIO_SEND_NAME));
        stringEquals(input, kAudioDevicePropertyDeviceUID, CFSTR(RV_AUDIO_INPUT_UID));
        stringEquals(output, kAudioDevicePropertyDeviceUID, CFSTR(RV_AUDIO_OUTPUT_UID));
        require(read<UInt32>(input, kAudioDevicePropertyIsHidden) == 0, "microphone is hidden");
        require(read<UInt32>(output, kAudioDevicePropertyIsHidden) == 1, "sending output is visible");
        auto ins = list(input, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
        auto outs = list(output, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
        require(ins.size() == 1 && outs.size() == 1, "missing streams");
        require(read<AudioObjectID>(ins[0], kAudioObjectPropertyOwner) == input, "wrong input stream owner");
        require(read<AudioObjectID>(outs[0], kAudioObjectPropertyOwner) == output, "wrong output stream owner");
        for (auto device : devices) {
            for (auto control : list(device, kAudioObjectPropertyControlList))
                require(read<AudioObjectID>(control, kAudioObjectPropertyOwner) == device, "wrong control owner");
        }
        require(list(input, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).empty(), "visible output exposed");
        require(list(output, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput).empty(), "hidden input exposed");
        const auto format = read<AudioStreamBasicDescription>(ins[0], kAudioStreamPropertyVirtualFormat);
        require(format.mSampleRate == 48000 && format.mChannelsPerFrame == 2 && format.mBitsPerChannel == 32, "unexpected format");
        require((*driver)->StartIO(driver, input, 1) == noErr && (*driver)->StartIO(driver, output, 2) == noErr, "start failed");
        constexpr UInt32 frames = 256;
        std::vector<float> sent(frames * 2), received(frames * 2, -1);
        for (size_t i = 0; i < sent.size(); ++i) sent[i] = std::sin(i * .17f) * .3f;
        // Include a write/read spanning the driver's ring buffer boundary.
        for (double time : {1024., 16300., 32768.}) {
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mInputTime.mSampleTime = cycle.mOutputTime.mSampleTime = time;
            require((*driver)->DoIOOperation(driver, output, outs[0], 2, kAudioServerPlugInIOOperationWriteMix,
                frames, &cycle, sent.data(), nullptr) == noErr, "write failed");
            require((*driver)->DoIOOperation(driver, input, ins[0], 1, kAudioServerPlugInIOOperationReadInput,
                frames, &cycle, received.data(), nullptr) == noErr, "read failed");
            require(sent == received, "loopback changed samples");
        }
        AudioServerPlugInIOCycleInfo late{}; late.mInputTime.mSampleTime = 65536;
        require((*driver)->DoIOOperation(driver, input, ins[0], 1, kAudioServerPlugInIOOperationReadInput,
            frames, &late, received.data(), nullptr) == noErr, "idle read failed");
        for (auto value : received) require(value == 0, "stale audio after sender stopped");
        require((*driver)->StopIO(driver, output, 2) == noErr && (*driver)->StopIO(driver, input, 1) == noErr, "stop failed");
        puts("HAL: RadioVoice input visible, send hidden; stereo 48 kHz loopback and idle silence passed");
        dlclose(bundle);
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
