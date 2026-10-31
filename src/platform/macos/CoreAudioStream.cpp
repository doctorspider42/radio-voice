#include "platform/macos/CoreAudioStream.h"
#include "platform/macos/CoreAudioDevice.h"
#include "core/Denormal.h"
#include "core/Log.h"
#include <algorithm>
#include <cstring>

namespace rv::audio {
namespace {
constexpr AudioObjectPropertySelector kObserved[] = {
    kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyNominalSampleRate,
    kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyStreamConfiguration
};
}
CoreAudioStream::~CoreAudioStream() { stop(); }
bool CoreAudioStream::check(OSStatus status, const char* action)
{
    if (status == noErr) return true;
    error_ = std::string(action) + " (CoreAudio " + std::to_string(status) + ")";
    RV_ERROR("%s", error_.c_str());
    return false;
}
bool CoreAudioStream::open(const StreamConfig& config, bool input)
{
    stop();
    error_.clear();
    invalidated_ = false;
    xruns_ = 0;
    input_ = input;
    device_ = coreaudio::resolve(config.deviceId);
    if (device_ == kAudioObjectUnknown) {
        error_ = "The audio device is unavailable. Select a connected device.";
        return false;
    }
    const int available = coreaudio::channels(device_, input);
    if (!available) { error_ = "This device has no channels in the selected direction."; return false; }
    channels_ = std::clamp(config.channels, 1, std::min(available, kMaxChannels));
    if (!coreaudio::read(device_, kAudioDevicePropertyNominalSampleRate, rate_) || rate_ <= 0) {
        error_ = "Could not read the device sample rate."; return false;
    }
    // Buffer size belongs to the device, shared with other applications. Query
    // it rather than changing another app's running stream behind its back.
    UInt32 period = 0;
    if (!coreaudio::read(device_, kAudioDevicePropertyBufferFrameSize, period) || !period) {
        error_ = "Could not read the device buffer size."; return false;
    }
    period_ = static_cast<int>(period);
    UInt32 latency = 0, safety = 0;
    const auto scope = input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
    coreaudio::read(device_, kAudioDevicePropertyLatency, latency, scope);
    coreaudio::read(device_, kAudioDevicePropertySafetyOffset, safety, scope);
    latency_ = static_cast<int>(latency + safety);

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    auto component = AudioComponentFindNext(nullptr, &desc);
    if (!component) { error_ = "The macOS HAL audio unit is unavailable."; return false; }
    if (!check(AudioComponentInstanceNew(component, &unit_), "Create audio unit")) return false;
    UInt32 enabled = input ? 1 : 0, outputEnabled = input ? 0 : 1;
    if (!check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enabled, sizeof(enabled)), "Configure capture") ||
        !check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &outputEnabled, sizeof(outputEnabled)), "Configure playback") ||
        !check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device_, sizeof(device_)), "Select device")) return false;

    AudioStreamBasicDescription format{};
    format.mSampleRate = rate_;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels_;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = format.mBytesPerPacket = sizeof(float) * channels_;
    if (!check(AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
            input ? kAudioUnitScope_Output : kAudioUnitScope_Input, input ? 1 : 0,
            &format, sizeof(format)), "Set float stream format")) return false;
    // AUHAL requires an explicit channel map when the client uses fewer input
    // channels than the hardware. Take the first N, matching the UI mix modes.
    if (input && channels_ != available) {
        std::vector<SInt32> map(channels_);
        for (int c = 0; c < channels_; ++c) map[c] = c;
        if (!check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_ChannelMap,
                kAudioUnitScope_Output, 1, map.data(), static_cast<UInt32>(map.size() * sizeof(SInt32))), "Map capture channels")) return false;
    }
    maxFrames_ = std::max<UInt32>(4096, period);
    if (!check(AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames_, sizeof(maxFrames_)), "Set maximum callback size")) return false;
    AURenderCallbackStruct cb{callback, this};
    if (!check(AudioUnitSetProperty(unit_, input ? AudioUnitPropertyID(kAudioOutputUnitProperty_SetInputCallback) : AudioUnitPropertyID(kAudioUnitProperty_SetRenderCallback),
            input ? kAudioUnitScope_Global : kAudioUnitScope_Input, 0, &cb, sizeof(cb)), "Set audio callback") ||
        !check(AudioUnitInitialize(unit_), "Initialize audio unit")) return false;
    UInt32 size = sizeof(maxFrames_);
    AudioUnitGetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames_, &size);
    samples_.assign(static_cast<size_t>(maxFrames_) * channels_, 0);
    for (auto selector : kObserved) {
        auto a = coreaudio::address(selector,
            selector == kAudioDevicePropertyStreamConfiguration ? AudioObjectPropertyScope(scope) : AudioObjectPropertyScope(kAudioObjectPropertyScopeGlobal));
        AudioObjectAddPropertyListener(device_, &a, deviceChanged, this);
    }
    listening_ = true;
    RV_INFO("CoreAudio %s opened: %.0f Hz, %d ch, %d frames", input ? "input" : "output", rate_, channels_, period_);
    return true;
}
bool CoreAudioStream::run()
{
    if (!unit_ || (input_ ? sink_ == nullptr : producer_ == nullptr)) return false;
    running_ = true;
    if (!check(AudioOutputUnitStart(unit_), "Start audio device")) { running_ = false; return false; }
    return true;
}
void CoreAudioStream::stop()
{
    running_ = false;
    if (listening_) {
        for (auto selector : kObserved) {
            auto a = coreaudio::address(selector, selector == kAudioDevicePropertyStreamConfiguration
                ? AudioObjectPropertyScope(input_ ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput) : AudioObjectPropertyScope(kAudioObjectPropertyScopeGlobal));
            AudioObjectRemovePropertyListener(device_, &a, deviceChanged, this);
        }
        listening_ = false;
    }
    if (unit_) {
        AudioOutputUnitStop(unit_);
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
    }
    sink_ = nullptr;
    producer_ = nullptr;
}
OSStatus CoreAudioStream::deviceChanged(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* context)
{
    static_cast<CoreAudioStream*>(context)->invalidated_ = true;
    return noErr;
}
OSStatus CoreAudioStream::callback(void* context, AudioUnitRenderActionFlags* flags,
    const AudioTimeStamp* time, UInt32, UInt32 frames, AudioBufferList* output)
{
    auto& self = *static_cast<CoreAudioStream*>(context);
    ScopedNoDenormals noDenormals;
    const auto silence = [output] {
        if (output) for (UInt32 i = 0; i < output->mNumberBuffers; ++i)
            if (output->mBuffers[i].mData) std::memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    };
    if (!self.running_ || self.invalidated_ || frames > self.maxFrames_) {
        silence();
        if (frames > self.maxFrames_) { ++self.xruns_; self.invalidated_ = true; }
        return noErr;
    }
    if (self.input_) {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0] = {static_cast<UInt32>(self.channels_), static_cast<UInt32>(frames * self.channels_ * sizeof(float)), self.samples_.data()};
        if (AudioUnitRender(self.unit_, flags, time, 1, frames, &list) != noErr) {
            ++self.xruns_;
            return noErr;
        }
        if (self.sink_->write(self.samples_.data(), static_cast<int>(frames)) != static_cast<int>(frames)) ++self.xruns_;
    } else {
        self.producer_->produce(self.samples_.data(), self.channels_, static_cast<int>(frames));
        if (output && output->mNumberBuffers == 1 && output->mBuffers[0].mData &&
            output->mBuffers[0].mDataByteSize >= frames * self.channels_ * sizeof(float)) {
            std::memcpy(output->mBuffers[0].mData, self.samples_.data(), frames * self.channels_ * sizeof(float));
        } else { silence(); ++self.xruns_; }
    }
    return noErr;
}
}
