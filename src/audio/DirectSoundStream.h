#pragma once

#include <windows.h>

// WIN32_LEAN_AND_MEAN keeps windows.h from pulling in the multimedia headers,
// but dsound.h uses WAVEFORMATEX without declaring it itself.
#include <mmsystem.h>

#include <dsound.h>

#include <atomic>
#include <thread>
#include <vector>

#include "audio/ComPtr.h"
#include "audio/SampleFormat.h"
#include "audio/Stream.h"

namespace rv::audio {

/// DirectSound backend.
///
/// On Vista and later DirectSound is emulated on top of the same engine WASAPI
/// shared mode uses, so it offers no latency advantage. It is here because some
/// drivers and virtual devices are still enumerated and behave more predictably
/// through it, and it is the widest-compatibility fallback when WASAPI refuses
/// an endpoint outright.
///
/// Unlike WASAPI there is no buffer-ready event: both directions run a polling
/// thread that chases the hardware cursor. The poll interval is a quarter of
/// the buffer period, which keeps the cursor from lapping us without spinning.
class DirectSoundStreamBase {
public:
    virtual ~DirectSoundStreamBase();

    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    double actualSampleRate() const { return sampleRate_; }
    int    actualChannels() const { return channels_; }
    int    bufferFrames() const { return periodFrames_; }
    int    latencyFrames() const { return ringFrames_; }
    u32    xruns() const { return xruns_.load(std::memory_order_relaxed); }

    const std::string& error() const { return error_; }

protected:
    /// Number of periods held in the DirectSound ring. Four is the smallest
    /// figure that reliably survives a scheduling hiccup on the poll thread.
    static constexpr int kPeriodsInRing = 4;

    bool parseDeviceGuid(const DeviceId& id, GUID& out) const;
    void launchThread();

    virtual void threadBody() = 0;

    /// A periodic timer for the poll loop, and the wait that goes with it.
    ///
    /// These exist because a plain timeout does not do what the interval says.
    /// The system timer runs at about 15.6 ms by default, and
    /// WaitForSingleObject rounds up to it - so asking for 2 ms and getting
    /// 15.6 is not an overload symptom, it is the ordinary case, and it holds
    /// however short the requested period is.
    ///
    /// The alternative fix, timeBeginPeriod, raises the tick rate for the whole
    /// machine to repair one thread. A high-resolution waitable timer is local
    /// to this stream. It needs Windows 10 1803, so createPollTimer falls back
    /// to a plain timeout when the flag is refused - slow polling being better
    /// than none.
    HANDLE createPollTimer(DWORD intervalMs) const;
    /// Returns false when the stream should stop.
    bool   waitForPoll(HANDLE timer, DWORD intervalMs) const;

    WAVEFORMATEX makeWaveFormat(int channels, int sampleRate) const;

    SampleFormat sampleFormat_ = SampleFormat::Int16;
    double sampleRate_   = 0.0;
    int    channels_     = 0;
    int    periodFrames_ = 0;
    int    ringFrames_   = 0;
    int    frameBytes_   = 0;

    HANDLE stopEvent_ = nullptr;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<u32>  xruns_{0};
    std::string       error_;
};

class DirectSoundInputStream final : public DirectSoundStreamBase, public IInputStream {
public:
    bool openInput(const StreamConfig& config) override;
    bool runInput(AudioRing& sink) override;

    void stop() override { DirectSoundStreamBase::stop(); }
    bool isRunning() const override { return DirectSoundStreamBase::isRunning(); }
    double actualSampleRate() const override { return DirectSoundStreamBase::actualSampleRate(); }
    int actualChannels() const override { return DirectSoundStreamBase::actualChannels(); }
    int bufferFrames() const override { return DirectSoundStreamBase::bufferFrames(); }
    int latencyFrames() const override { return DirectSoundStreamBase::latencyFrames(); }
    u32 xruns() const override { return DirectSoundStreamBase::xruns(); }
    const std::string& error() const override { return DirectSoundStreamBase::error(); }

protected:
    void threadBody() override;

private:
    ComPtr<IDirectSoundCapture8>       capture_;
    ComPtr<IDirectSoundCaptureBuffer8> buffer_;

    AudioRing*         sink_ = nullptr;
    std::vector<float> scratch_;
    DWORD              readCursor_ = 0;
};

class DirectSoundOutputStream final : public DirectSoundStreamBase, public IOutputStream {
public:
    bool openOutput(const StreamConfig& config) override;
    bool runOutput(IAudioProducer& producer) override;

    void stop() override { DirectSoundStreamBase::stop(); }
    bool isRunning() const override { return DirectSoundStreamBase::isRunning(); }
    double actualSampleRate() const override { return DirectSoundStreamBase::actualSampleRate(); }
    int actualChannels() const override { return DirectSoundStreamBase::actualChannels(); }
    int bufferFrames() const override { return DirectSoundStreamBase::bufferFrames(); }
    int latencyFrames() const override { return DirectSoundStreamBase::latencyFrames(); }
    u32 xruns() const override { return DirectSoundStreamBase::xruns(); }
    const std::string& error() const override { return DirectSoundStreamBase::error(); }

protected:
    void threadBody() override;

private:
    ComPtr<IDirectSound8>       device_;
    ComPtr<IDirectSoundBuffer8> buffer_;

    IAudioProducer*    producer_ = nullptr;
    std::vector<float> scratch_;
    DWORD              writeCursor_ = 0;
};

} // namespace rv::audio
