#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "audio/DriftResampler.h"
#include "audio/Stream.h"
#include "core/Params.h"
#include "core/RingBuffer.h"
#include "dsp/Chain.h"
#include "dsp/Limiter.h"
#include "dsp/Meter.h"
#include "dsp/Smoothing.h"
#include "dsp/SpectrumAnalyzer.h"

namespace rv::audio {

class AsioStream;

struct EngineConfig {
    StreamConfig input;
    StreamConfig output;

    /// Width of the internal signal path. Two is the default even for a mono
    /// microphone: a large share of VST3 plugins are stereo-only, and the cost
    /// of processing a duplicated channel is far smaller than the cost of
    /// plugins refusing to instantiate.
    int internalChannels = 2;

    /// Largest block handed to the chain in one call. The device buffer is
    /// split into chunks of at most this size, which bounds what plugins have
    /// to be prepared for.
    int maxBlockFrames = 512;
};

/// Live status, refreshed when the streams open and polled by the UI.
struct EngineStatus {
    bool running = false;

    std::string inputError;
    std::string outputError;

    double inputSampleRate  = 0.0;
    double outputSampleRate = 0.0;
    int    inputChannels    = 0;
    int    outputChannels   = 0;
    int    inputBufferFrames  = 0;
    int    outputBufferFrames = 0;

    std::string inputFormat;
    std::string outputFormat;
};

/// Owns the streams, the signal path and the clock bridge between them.
///
/// The whole DSP chain runs on the output device thread, inside `produce`.
/// A separate worker thread would decouple a slow plugin from the device, but
/// at the cost of another buffer of latency in a path that is being monitored
/// live - so the chain runs where the deadline is, exactly as a DAW does it.
class Engine final : public IAudioProducer {
public:
    Engine(Params& params, Meters& meters);
    ~Engine() override;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Opens both devices and starts processing. Returns false and populates
    /// the status errors if either side fails.
    bool start(const EngineConfig& config);
    void stop();
    bool isRunning() const;

    /// Restarts with the current configuration - used after a device change.
    bool restart();

    const EngineConfig& config() const { return config_; }
    EngineStatus        status() const;

    dsp::Chain& chain() { return chain_; }

    /// Spectra either side of the chain, for the display behind the EQ curve.
    dsp::SpectrumAnalyzer& inputSpectrum() { return inputSpectrum_; }
    dsp::SpectrumAnalyzer& outputSpectrum() { return outputSpectrum_; }

    /// Where the end-to-end delay actually goes, in milliseconds on the output
    /// clock. A single total answers "how much" but never "why", and the
    /// answer is almost always one term dominating the rest.
    struct LatencyBreakdown {
        float inputDevice  = 0.0f; ///< Driver-reported capture latency.
        float bridge       = 0.0f; ///< The ring between the two device clocks.
        float chain        = 0.0f; ///< Look-ahead in the gate, compressor and plugins.
        float limiter      = 0.0f; ///< The output limiter's look-ahead.
        float outputDevice = 0.0f; ///< Driver-reported render latency.

        float total() const
        {
            return inputDevice + bridge + chain + limiter + outputDevice;
        }
    };

    /// UI thread only.
    LatencyBreakdown latencyBreakdown() const;

    /// Total round-trip latency estimate in milliseconds. UI thread only.
    float latencyMs() const;

    /// Refreshes the UI-thread-derived meter fields. Call once per frame.
    void updateSlowMeters();

    /// The ASIO stream, when one is open, so the UI can offer its control
    /// panel. Null for every other backend.
    AsioStream* asioStream() const { return asioShared_.get(); }

    // --- audio thread -----------------------------------------------------
    void produce(float* interleaved, int channels, int frames) override;

private:
    bool createStreams();
    bool openStreams();
    void closeStreams();
    void allocateBuffers(double outputSampleRate);
    void processChunk(float* dst, int dstChannels, int frames);
    void updateCpuLoad(double elapsedSeconds, int frames);

    Params& params_;
    Meters& meters_;

    EngineConfig config_;

    mutable std::mutex statusMutex_;
    EngineStatus       status_;

    // Ownership is split because a duplex ASIO device is one object serving
    // both interfaces; `input_`/`output_` are the borrowed views the rest of
    // the engine works through.
    std::unique_ptr<IInputStream>  ownedInput_;
    std::unique_ptr<IOutputStream> ownedOutput_;
    std::shared_ptr<AsioStream>    asioShared_;
    IInputStream*  input_  = nullptr;
    IOutputStream* output_ = nullptr;

    AudioRing       inputRing_;
    DriftResampler  resampler_;
    DriftController drift_;

    dsp::Chain            chain_;
    dsp::Limiter          limiter_;
    dsp::LevelMeter       inputMeter_;
    dsp::LevelMeter       outputMeter_;
    dsp::SpectrumAnalyzer inputSpectrum_;
    dsp::SpectrumAnalyzer outputSpectrum_;

    dsp::Smoothed inputGain_;
    dsp::Smoothed outputGain_;
    dsp::Smoothed muteGain_;

    // Preallocated audio-thread scratch.
    std::vector<float> staging_; ///< Interleaved frames peeked from the ring.
    dsp::PlanarBuffer  inputPlanar_;
    dsp::PlanarBuffer  work_;

    int    internalChannels_ = 2;
    int    maxBlockFrames_   = 512;
    int    targetRingFill_   = 0;
    double nominalRatio_     = 1.0;

    /// Read by `produce` without a lock. Written only while the audio thread
    /// is stopped, so no synchronisation is required.
    double audioOutputRate_ = 0.0;

    std::atomic<bool> running_{false};

    double counterFrequency_ = 1.0;
    double cpuLoadSmoothed_  = 0.0;

    /// False until the capture ring has first reached its working level. See
    /// the priming block at the top of processChunk.
    bool primed_ = false;

    /// Consecutive blocks that could not be fed from the capture ring, and the
    /// threshold at which that is reported as a dead input (about one second).
    int starvedBlocks_       = 0;
    int starvedBlocksToFlag_ = 200;
};

} // namespace rv::audio
