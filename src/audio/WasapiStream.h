#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "audio/SampleFormat.h"
#include "audio/Stream.h"
#include "audio/WasapiCommon.h"

namespace rv::audio {

/// Shared plumbing for the capture and render sides of WASAPI.
///
/// Both directions negotiate a format, initialise an `IAudioClient` in
/// event-driven mode and run a dedicated MMCSS "Pro Audio" thread. What differs
/// is only which service interface is fetched and which way the samples move.
class WasapiStreamBase {
public:
    virtual ~WasapiStreamBase();

    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    double actualSampleRate() const { return sampleRate_; }
    int    actualChannels() const { return channels_; }
    int    bufferFrames() const { return bufferFrames_; }
    int    latencyFrames() const { return latencyFrames_; }
    u32    xruns() const { return xruns_.load(std::memory_order_relaxed); }

    const std::string& error() const { return error_; }
    bool deviceInvalidated() const
    {
        return deviceInvalidated_.load(std::memory_order_acquire);
    }

    SampleFormat wireFormat() const { return sampleFormat_; }
    WasapiMode   mode() const { return mode_; }

protected:
    /// Opens the endpoint and initialises the client. `capture` selects the
    /// data-flow direction and therefore which negotiation rules apply.
    bool openClient(const StreamConfig& config, bool capture);
    void closeClient();

    /// Runs on the device thread; implemented by the two subclasses.
    virtual void threadBody() = 0;

    void launchThread();
    void fail(const char* what, HRESULT hr);
    void markDeviceInvalidated(const char* direction);

    ComPtr<IMMDevice>    device_;
    ComPtr<IAudioClient> client_;

    HANDLE bufferEvent_ = nullptr;
    HANDLE stopEvent_   = nullptr;

    SampleFormat sampleFormat_ = SampleFormat::Unknown;
    WasapiMode   mode_         = WasapiMode::Shared;

    double sampleRate_    = 0.0;
    int    channels_      = 0;
    int    bufferFrames_  = 0;
    int    frameBytes_    = 0;
    int    latencyFrames_ = 0;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> deviceInvalidated_{false};
    std::atomic<u32>  xruns_{0};
    std::string       error_;

private:
    /// Exclusive mode rejects buffer durations that are not an exact multiple
    /// of the device period. The prescribed recovery is to ask the client what
    /// size it would have used, discard it and re-create it with that duration.
    HRESULT initialiseExclusive(const StreamConfig& config, WAVEFORMATEXTENSIBLE& format);
    HRESULT initialiseShared(const StreamConfig& config, WAVEFORMATEXTENSIBLE& format);
};

/// Microphone capture. Writes interleaved float frames into the engine's ring.
class WasapiInputStream final : public WasapiStreamBase, public IInputStream {
public:
    bool openInput(const StreamConfig& config) override;
    bool runInput(AudioRing& sink) override;

    void stop() override { WasapiStreamBase::stop(); }
    bool isRunning() const override { return WasapiStreamBase::isRunning(); }
    double actualSampleRate() const override { return WasapiStreamBase::actualSampleRate(); }
    int actualChannels() const override { return WasapiStreamBase::actualChannels(); }
    int bufferFrames() const override { return WasapiStreamBase::bufferFrames(); }
    int latencyFrames() const override { return WasapiStreamBase::latencyFrames(); }
    u32 xruns() const override { return WasapiStreamBase::xruns(); }
    const std::string& error() const override { return WasapiStreamBase::error(); }
    bool deviceInvalidated() const override
    {
        return WasapiStreamBase::deviceInvalidated();
    }

protected:
    void threadBody() override;

private:
    AudioRing*         sink_ = nullptr;
    std::vector<float> scratch_;
};

/// Render to the selected output endpoint - in the intended setup, a virtual
/// cable's input side. Pulls from the engine on its own device thread.
class WasapiOutputStream final : public WasapiStreamBase, public IOutputStream {
public:
    bool openOutput(const StreamConfig& config) override;
    bool runOutput(IAudioProducer& producer) override;

    void stop() override { WasapiStreamBase::stop(); }
    bool isRunning() const override { return WasapiStreamBase::isRunning(); }
    double actualSampleRate() const override { return WasapiStreamBase::actualSampleRate(); }
    int actualChannels() const override { return WasapiStreamBase::actualChannels(); }
    int bufferFrames() const override { return WasapiStreamBase::bufferFrames(); }
    int latencyFrames() const override { return WasapiStreamBase::latencyFrames(); }
    u32 xruns() const override { return WasapiStreamBase::xruns(); }
    const std::string& error() const override { return WasapiStreamBase::error(); }
    bool deviceInvalidated() const override
    {
        return WasapiStreamBase::deviceInvalidated();
    }

protected:
    void threadBody() override;

private:
    IAudioProducer*    producer_ = nullptr;
    std::vector<float> scratch_;
};

} // namespace rv::audio
