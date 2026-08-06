#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

    /// Optional second render device carrying the same processed signal.
    ///
    /// The main output usually goes to a virtual cable, and a cable is deaf by
    /// definition - whatever is sent into it cannot be heard. This is how the
    /// operator hears their own voice while it is being sent somewhere else.
    StreamConfig monitor;
    bool         monitorEnabled = false;

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

    /// Empty when monitoring is off or opened cleanly. A monitor that fails to
    /// open never stops the engine: the signal still reaches its destination,
    /// and only the operator's own headphones are affected.
    std::string monitorError;
    bool        monitorRunning    = false;
    double      monitorSampleRate = 0.0;
    int         monitorChannels   = 0;
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

    /// Starts, stops or re-points the monitor on a running engine.
    ///
    /// Deliberately not part of the restart path. The monitor hangs off the end
    /// of the output thread and shares nothing with capture, the chain or the
    /// main render device, so tearing all of that down to add a second pair of
    /// headphones would drop the audio being sent for no reason at all.
    ///
    /// Returns false only when a requested device could not be opened; the
    /// reason lands in status().monitorError. UI thread only.
    bool applyMonitor(const StreamConfig& device, bool enabled);

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

    /// Whether a main WASAPI endpoint disappeared after it was started. The
    /// application recreates the engine instead of leaving it "running" with a
    /// dead output thread.
    bool deviceRecoveryNeeded() const;

    /// The ASIO stream, when one is open, so the UI can offer its control
    /// panel. Null for every other backend.
    AsioStream* asioStream() const { return asioShared_.get(); }

    // --- audio thread -----------------------------------------------------
    void produce(float* interleaved, int channels, int frames) override;

private:
    /// Feeds the monitor device from a ring the main output thread fills.
    ///
    /// It cannot simply copy: the monitor card has its own oscillator, so it
    /// needs the same drift-corrected bridge the microphone gets. Without one,
    /// the two clocks separate by tens of parts per million and the monitor
    /// buffer creeps to empty or full over a few minutes, then clicks.
    ///
    /// Deliberately its own ring rather than a tee into the main path: a
    /// monitor that stalls must not be able to hold up the signal that is
    /// actually being sent somewhere.
    class MonitorTap final : public IAudioProducer {
    public:
        void prepare(AudioRing& ring, double sourceRate, double deviceRate,
                     int devicePeriod, int targetFill,
                     const std::atomic<float>& gainDb);
        void produce(float* interleaved, int channels, int frames) override;

    private:
        AudioRing*      ring_ = nullptr;
        DriftResampler  resampler_;
        DriftController drift_;
        dsp::PlanarBuffer  planar_;
        dsp::PlanarBuffer  work_;
        std::vector<float> staging_;

        double nominalRatio_ = 1.0;
        int    targetFill_   = 0;
        int    maxFrames_    = 0;
        bool   primed_       = false;

        /// Read straight from Params on the audio thread, like every other
        /// gain in the engine.
        const std::atomic<float>* gainDb_ = nullptr;
        dsp::Smoothed             gain_;
    };

    bool createStreams();
    bool openStreams();
    bool openMonitor(double sourceRate, int sourceChannels);
    void closeMonitor();
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

    std::unique_ptr<IOutputStream> ownedMonitor_;
    IOutputStream*                 monitor_ = nullptr;
    AudioRing                      monitorRing_;
    MonitorTap                     monitorTap_;

    /// Read by the main output thread every block. Set only while that thread
    /// is stopped, so a plain flag would do - it is atomic to say plainly that
    /// two threads look at it.
    std::atomic<bool> monitorActive_{false};

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
