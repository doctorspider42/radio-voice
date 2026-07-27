#pragma once

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "audio/Stream.h"

namespace rv::audio {

/// ASIO backend.
///
/// ASIO differs from the other backends in two ways that shape this class:
///
///  * **It is duplex and exclusive.** One driver instance owns both directions
///    of the device, and only one process may hold it. Capture and render on
///    the same interface therefore have to be the same object, which is why
///    this single class implements both `IInputStream` and `IOutputStream`.
///  * **Its callbacks are free functions with no user pointer.** The API has no
///    way to pass context into `bufferSwitch`, so the active instance is
///    reached through a file-static pointer. Since the driver is exclusive,
///    there can only ever be one.
///
/// The class supports being opened for input only, output only, or duplex; the
/// engine picks based on which side the user pointed at ASIO.
class AsioStream final : public IInputStream, public IOutputStream {
public:
    AsioStream() = default;
    ~AsioStream() override;

    /// Loads and configures the driver. `config.deviceId` is the ASIO driver
    /// name as it appears under HKLM\\SOFTWARE\\ASIO.
    ///
    /// Both directions are requested in one call because ASIO negotiates the
    /// entire channel set at once, in `ASIOCreateBuffers`.
    bool openDevice(const StreamConfig& config, bool wantInput, bool wantOutput);

    bool openInput(const StreamConfig& config) override
    {
        return openDevice(config, /*wantInput=*/true, /*wantOutput=*/false);
    }

    bool openOutput(const StreamConfig& config) override
    {
        return openDevice(config, /*wantInput=*/false, /*wantOutput=*/true);
    }

    bool runInput(AudioRing& sink) override;
    bool runOutput(IAudioProducer& producer) override;

    /// Both directions through one driver instance. Used whenever the same
    /// ASIO device was selected for input and output.
    bool runDuplex(AudioRing& sink, IAudioProducer& producer);

    void stop() override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

    double actualSampleRate() const override { return sampleRate_; }
    int    actualChannels() const override { return std::max(inputChannels_, outputChannels_); }
    int    bufferFrames() const override { return bufferFrames_; }
    int    latencyFrames() const override { return inputLatency_ + outputLatency_; }
    u32    xruns() const override { return xruns_.load(std::memory_order_relaxed); }
    const std::string& error() const override { return error_; }

    int inputChannels() const { return inputChannels_; }
    int outputChannels() const { return outputChannels_; }

    /// Opens the driver's own control panel, where buffer size and hardware
    /// sample rate are configured. ASIO deliberately leaves those to the
    /// driver's UI rather than exposing a portable API for them.
    bool showControlPanel();

    /// True when some other object already holds an ASIO driver. Only one may
    /// be loaded per process.
    static bool isDriverLoaded();

    // --- called from the ASIO callbacks; public only for the trampoline ----
    void onBufferSwitch(long bufferIndex);
    void onSampleRateChanged(double rate);
    void onReset();

private:
    /// Issues ASIOStart once both directions have been wired up.
    bool startDriver();
    void closeDriver();

    AudioRing*      sink_     = nullptr;
    IAudioProducer* producer_ = nullptr;

    double sampleRate_    = 0.0;
    int    bufferFrames_  = 0;
    int    inputChannels_  = 0;
    int    outputChannels_ = 0;
    int    inputLatency_   = 0;
    int    outputLatency_  = 0;

    /// ASIOSampleType of the channels in use, one value for each direction;
    /// drivers use a single type across all channels in practice.
    long inputSampleType_  = -1;
    long outputSampleType_ = -1;

    /// Interleaved float scratch, sized at open time so the callback allocates
    /// nothing.
    std::vector<float> inputScratch_;
    std::vector<float> outputScratch_;

    /// ASIOBufferInfo array, kept opaque so the ASIO SDK headers stay out of
    /// this header and out of every translation unit that includes it.
    void* bufferInfos_ = nullptr;
    int   bufferInfoCount_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<u32>  xruns_{0};
    std::string       error_;
    bool              driverLoaded_ = false;
    bool              buffersCreated_ = false;
    bool              started_ = false;
    bool              postOutput_ = false; ///< Driver wants ASIOOutputReady().
};

} // namespace rv::audio
