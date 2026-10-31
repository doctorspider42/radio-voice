#pragma once
#include "audio/Stream.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <vector>

namespace rv::audio {
/// One AUHAL per direction. The device's native rate is left untouched; the
/// engine bridges independent device clocks and rates through its audio ring.
class CoreAudioStream : public virtual IStream {
public:
    ~CoreAudioStream() override;
    void stop() override;
    bool isRunning() const override { return running_.load(); }
    double actualSampleRate() const override { return rate_; }
    int actualChannels() const override { return channels_; }
    int bufferFrames() const override { return period_; }
    int latencyFrames() const override { return latency_; }
    u32 xruns() const override { return xruns_.load(); }
    const std::string& error() const override { return error_; }
    bool deviceInvalidated() const override { return invalidated_.load(); }
protected:
    bool open(const StreamConfig&, bool input);
    bool run();
    AudioRing* sink_ = nullptr;
    IAudioProducer* producer_ = nullptr;
private:
    bool check(OSStatus, const char* action);
    static OSStatus callback(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
    static OSStatus deviceChanged(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*);
    AudioUnit unit_ = nullptr;
    AudioDeviceID device_ = kAudioObjectUnknown;
    bool input_ = false;
    bool listening_ = false;
    double rate_ = 0;
    int channels_ = 0, period_ = 0, latency_ = 0;
    UInt32 maxFrames_ = 0;
    std::vector<float> samples_;
    std::atomic<bool> running_{false}, invalidated_{false};
    std::atomic<u32> xruns_{0};
    std::string error_;
};
class CoreAudioInputStream final : public CoreAudioStream, public IInputStream {
public:
    bool openInput(const StreamConfig& config) override { return open(config, true); }
    bool runInput(AudioRing& sink) override { sink_ = &sink; return run(); }
};
class CoreAudioOutputStream final : public CoreAudioStream, public IOutputStream {
public:
    bool openOutput(const StreamConfig& config) override { return open(config, false); }
    bool runOutput(IAudioProducer& producer) override { producer_ = &producer; return run(); }
};
}
