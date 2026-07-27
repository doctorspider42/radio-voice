#include "audio/Engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/DirectSoundStream.h"
#include "audio/SampleFormat.h"
#include "audio/WasapiStream.h"
#include "core/Denormal.h"
#include "core/Log.h"

#if RV_HAS_ASIO
#include "audio/AsioStream.h"
#endif

namespace rv::audio {
namespace {

/// How much audio to keep buffered between the capture and render clocks,
/// expressed in whole device periods.
///
/// One period covers the worst case where the two device threads are exactly
/// out of phase; the second is margin for scheduling jitter while the drift
/// controller pulls the fill back to target. This buffer is pure added latency,
/// so every extra period costs the user directly - at a 22 ms shared-mode
/// period, one period is 22 ms of delay on their own voice.
constexpr int kTargetPeriodsOfSlack = 2;

double performanceFrequency()
{
    LARGE_INTEGER frequency{};
    ::QueryPerformanceFrequency(&frequency);
    return static_cast<double>(frequency.QuadPart);
}

double performanceCounter()
{
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart);
}

} // namespace

Engine::Engine(Params& params, Meters& meters)
    : params_(params)
    , meters_(meters)
{
    counterFrequency_ = performanceFrequency();
}

Engine::~Engine()
{
    stop();
}

bool Engine::isRunning() const
{
    return running_.load(std::memory_order_acquire);
}

EngineStatus Engine::status() const
{
    std::lock_guard lock(statusMutex_);
    return status_;
}

bool Engine::start(const EngineConfig& config)
{
    stop();

    config_           = config;
    internalChannels_ = std::clamp(config.internalChannels, 1, kMaxChannels);
    maxBlockFrames_   = std::clamp(config.maxBlockFrames, kMinBlockSize, kMaxBlockSize);

    {
        std::lock_guard lock(statusMutex_);
        status_ = EngineStatus{};
    }

    if (!openStreams()) {
        closeStreams();
        return false;
    }

    running_.store(true, std::memory_order_release);
    chain_.setAudioRunning(true);

    const EngineStatus snapshot = status();
    RV_INFO("engine started: %.0f Hz / %d ch in -> %.0f Hz / %d ch out, "
            "%d internal channels, block %d frames, ring target %d frames",
            snapshot.inputSampleRate, snapshot.inputChannels,
            snapshot.outputSampleRate, snapshot.outputChannels,
            internalChannels_, maxBlockFrames_, targetRingFill_);

    const LatencyBreakdown latency = latencyBreakdown();
    RV_INFO("latency %.1f ms = input device %.1f + clock bridge %.1f + chain %.1f "
            "+ limiter %.1f + output device %.1f",
            latency.total(), latency.inputDevice, latency.bridge, latency.chain,
            latency.limiter, latency.outputDevice);

    return true;
}

bool Engine::restart()
{
    return start(config_);
}

void Engine::stop()
{
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);

    chain_.setAudioRunning(false);
    closeStreams();

    if (wasRunning) {
        meters_.reset();
        RV_INFO("engine stopped");
    }

    std::lock_guard lock(statusMutex_);
    status_.running        = false;
    status_.monitorRunning = false;
}

void Engine::closeStreams()
{
    // Monitor first, and the flag before the stream: the main output thread
    // reads it to decide whether to fill the monitor ring, and a stopped
    // consumer with a live producer would just fill the ring and wedge.
    monitorActive_.store(false, std::memory_order_release);
    if (monitor_)
        monitor_->stop();
    monitor_ = nullptr;
    ownedMonitor_.reset();

    // Render next: it is the side that pulls, so stopping it guarantees no
    // further calls into the chain while capture is torn down.
    if (output_)
        output_->stop();
    if (input_)
        input_->stop();

    input_  = nullptr;
    output_ = nullptr;
    ownedInput_.reset();
    ownedOutput_.reset();
    asioShared_.reset();
}

// ---------------------------------------------------------------------------
// Monitor
// ---------------------------------------------------------------------------

void Engine::MonitorTap::prepare(AudioRing& ring, double sourceRate, double deviceRate,
                                 int devicePeriod, int targetFill,
                                 const std::atomic<float>& gainDb)
{
    ring_         = &ring;
    gainDb_       = &gainDb;
    nominalRatio_ = (deviceRate > 0.0) ? sourceRate / deviceRate : 1.0;
    targetFill_   = targetFill;
    primed_       = false;

    // Room for the largest block the device can ask for, with slack for a
    // ratio that has drifted and for the interpolator's neighbourhood.
    maxFrames_ = std::max(1024, devicePeriod * 2);
    const int stagingFrames = static_cast<int>(maxFrames_ * nominalRatio_) + 64;

    staging_.assign(static_cast<size_t>(stagingFrames) * kMaxChannels, 0.0f);
    planar_.resize(ring.channels(), stagingFrames);
    work_.resize(ring.channels(), maxFrames_);

    resampler_.prepare(ring.channels());
    drift_.reset(nominalRatio_);

    gain_.prepare(static_cast<float>(deviceRate), 20.0f, 1.0f);
}

void Engine::MonitorTap::produce(float* interleaved, int channels, int frames)
{
    ScopedNoDenormals noDenormals;

    const size_t total = static_cast<size_t>(frames) * channels;

    if (!ring_ || frames <= 0) {
        std::memset(interleaved, 0, total * sizeof(float));
        return;
    }

    // Same priming rule as the capture bridge: silence until the ring has
    // reached its working level, rather than resampling against an empty
    // buffer for as long as it takes the first block to arrive.
    if (!primed_) {
        if (ring_->filled() < targetFill_) {
            std::memset(interleaved, 0, total * sizeof(float));
            return;
        }
        primed_ = true;
        resampler_.reset();
        drift_.reset(nominalRatio_);
    }

    if (targetFill_ > 0 && ring_->filled() > targetFill_ * 4)
        ring_->trimTo(targetFill_);

    const double ratio = drift_.update(ring_->filled(), targetFill_);

    const int srcChannels = ring_->channels();
    const int maxStaging  = static_cast<int>(staging_.size()) / kMaxChannels;
    const int wanted      = std::min(frames, maxFrames_);
    const int needed = std::min(resampler_.inputFramesNeeded(wanted, ratio), maxStaging);
    const int available = ring_->peek(staging_.data(), needed);

    planar_.setActiveFrames(available);
    planar_.readInterleaved(staging_.data(), srcChannels, available);

    work_.setActiveFrames(wanted);

    bool underrun = false;
    const int consumed = resampler_.process(planar_.data(), available,
                                            work_.data(), wanted, ratio, underrun);
    ring_->advanceRead(std::min(consumed, ring_->filled()));

    const float target = gainDb_ ? dsp::dbToGain(gainDb_->load(std::memory_order_relaxed)) : 1.0f;
    gain_.setTarget(target);
    for (int i = 0; i < wanted; ++i) {
        const float g = gain_.next();
        for (int c = 0; c < srcChannels; ++c)
            work_.channel(c)[i] *= g;
    }

    work_.writeInterleaved(interleaved, channels, wanted);

    // A device asking for more than the tap was sized for gets silence in the
    // tail rather than stale audio.
    if (wanted < frames) {
        std::memset(interleaved + static_cast<size_t>(wanted) * channels, 0,
                    static_cast<size_t>(frames - wanted) * channels * sizeof(float));
    }
}

bool Engine::openMonitor(double sourceRate, int sourceChannels)
{
    if (!config_.monitorEnabled || config_.monitor.deviceId.empty())
        return true;

    // ASIO is deliberately excluded. Its drivers are usually exclusive to one
    // device, and a second ASIO stream would either fail to open or seize the
    // card the main path is already using - failing the monitor is one thing,
    // taking the signal path down with it is another.
    switch (config_.monitor.backend) {
        case BackendType::Wasapi:
            ownedMonitor_ = std::make_unique<WasapiOutputStream>();
            break;
        case BackendType::DirectSound:
            ownedMonitor_ = std::make_unique<DirectSoundOutputStream>();
            break;
        case BackendType::Asio: {
            std::lock_guard lock(statusMutex_);
            status_.monitorError = "ASIO cannot be used for monitoring - its drivers "
                                   "open a device exclusively, so a second stream "
                                   "would contend with the main path";
            RV_WARN("%s", status_.monitorError.c_str());
            return false;
        }
    }

    monitor_ = ownedMonitor_.get();

    StreamConfig request = config_.monitor;
    if (!monitor_->openOutput(request)) {
        std::lock_guard lock(statusMutex_);
        status_.monitorError = monitor_->error();
        RV_WARN("monitor output could not be opened: %s", status_.monitorError.c_str());
        monitor_ = nullptr;
        ownedMonitor_.reset();
        return false;
    }

    const double monitorRate   = monitor_->actualSampleRate();
    const int    monitorPeriod = std::max(1, monitor_->bufferFrames());

    if (monitorRate <= 0.0) {
        std::lock_guard lock(statusMutex_);
        status_.monitorError = "the monitor driver reported an unusable stream format";
        monitor_ = nullptr;
        ownedMonitor_.reset();
        return false;
    }

    // Sized like the capture bridge, from whichever side moves audio in bigger
    // chunks - expressed in frames of the source, which is the main output.
    const double ratio = sourceRate / monitorRate;
    const double chunk = std::max(monitorPeriod * ratio,
                                  static_cast<double>(maxBlockFrames_));
    const int targetFill = static_cast<int>(chunk * kTargetPeriodsOfSlack);

    monitorRing_.resize(sourceChannels, targetFill * 3 + monitorPeriod * 4);

    monitorTap_.prepare(monitorRing_, sourceRate, monitorRate, monitorPeriod,
                        targetFill, params_.monitorGainDb);

    if (!monitor_->runOutput(monitorTap_)) {
        std::lock_guard lock(statusMutex_);
        status_.monitorError = monitor_->error();
        RV_WARN("monitor output could not be started: %s", status_.monitorError.c_str());
        monitor_->stop();
        monitor_ = nullptr;
        ownedMonitor_.reset();
        return false;
    }

    monitorActive_.store(true, std::memory_order_release);

    std::lock_guard lock(statusMutex_);
    status_.monitorRunning    = true;
    status_.monitorSampleRate = monitorRate;
    status_.monitorChannels   = monitor_->actualChannels();
    status_.monitorError.clear();

    RV_INFO("monitor started: %.0f Hz / %d ch, ring target %d frames",
            monitorRate, status_.monitorChannels, targetFill);
    return true;
}

bool Engine::createStreams()
{
    const bool asioInput  = (config_.input.backend == BackendType::Asio);
    const bool asioOutput = (config_.output.backend == BackendType::Asio);

    if (asioInput || asioOutput) {
#if RV_HAS_ASIO
        asioShared_ = std::make_shared<AsioStream>();
#else
        const std::string message =
            "this build was compiled without the ASIO SDK; configure with "
            "-DRV_ENABLE_ASIO=ON -DRV_ASIO_SDK_DIR=<sdk> to enable it";
        std::lock_guard lock(statusMutex_);
        if (asioInput)
            status_.inputError = message;
        if (asioOutput)
            status_.outputError = message;
        RV_ERROR("%s", message.c_str());
        return false;
#endif
    }

    switch (config_.input.backend) {
        case BackendType::Wasapi:
            ownedInput_ = std::make_unique<WasapiInputStream>();
            input_      = ownedInput_.get();
            break;
        case BackendType::DirectSound:
            ownedInput_ = std::make_unique<DirectSoundInputStream>();
            input_      = ownedInput_.get();
            break;
        case BackendType::Asio:
#if RV_HAS_ASIO
            input_ = asioShared_.get();
#endif
            break;
    }

    switch (config_.output.backend) {
        case BackendType::Wasapi:
            ownedOutput_ = std::make_unique<WasapiOutputStream>();
            output_      = ownedOutput_.get();
            break;
        case BackendType::DirectSound:
            ownedOutput_ = std::make_unique<DirectSoundOutputStream>();
            output_      = ownedOutput_.get();
            break;
        case BackendType::Asio:
#if RV_HAS_ASIO
            output_ = asioShared_.get();
#endif
            break;
    }

    return input_ != nullptr && output_ != nullptr;
}

bool Engine::openStreams()
{
    if (!createStreams())
        return false;

#if RV_HAS_ASIO
    const bool asioDuplex = asioShared_ &&
                            config_.input.backend == BackendType::Asio &&
                            config_.output.backend == BackendType::Asio &&
                            config_.input.deviceId == config_.output.deviceId;
#else
    const bool asioDuplex = false;
#endif

    // ---- negotiate formats before anything is sized ----------------------
    //
    // The driver has the last word on rate, channel count and buffer size, and
    // the ring buffer and every scratch allocation below depend on the real
    // values, not on what was requested.
#if RV_HAS_ASIO
    if (asioDuplex) {
        if (!asioShared_->openDevice(config_.input, /*wantInput=*/true, /*wantOutput=*/true)) {
            std::lock_guard lock(statusMutex_);
            status_.inputError = status_.outputError = asioShared_->error();
            return false;
        }
    } else
#endif
    {
        if (!input_->openInput(config_.input)) {
            std::lock_guard lock(statusMutex_);
            status_.inputError = input_->error();
            return false;
        }
        if (!output_->openOutput(config_.output)) {
            std::lock_guard lock(statusMutex_);
            status_.outputError = output_->error();
            return false;
        }
    }

    const double inputRate  = input_->actualSampleRate();
    const double outputRate = output_->actualSampleRate();
    const int    inputChannels  = input_->actualChannels();
    const int    outputPeriod   = std::max(1, output_->bufferFrames());

    if (inputRate <= 0.0 || outputRate <= 0.0 || inputChannels <= 0) {
        std::lock_guard lock(statusMutex_);
        status_.inputError = "the driver reported an unusable stream format";
        return false;
    }

    nominalRatio_ = inputRate / outputRate;

    // Sized from whichever side moves audio in bigger chunks, expressed in
    // input frames. Using the output period alone would under-size the ring
    // whenever the capture device has the longer period, and the input would
    // then arrive in bursts the ring could not hold.
    const double chunkFrames =
        std::max(outputPeriod * nominalRatio_, static_cast<double>(input_->bufferFrames()));
    targetRingFill_ = static_cast<int>(chunkFrames * kTargetPeriodsOfSlack);
    const int ringFrames =
        targetRingFill_ * 3 + std::max(input_->bufferFrames(), maxBlockFrames_) * 4;

    inputRing_.resize(inputChannels, ringFrames);

    allocateBuffers(outputRate);

    // ---- start the device threads ----------------------------------------
#if RV_HAS_ASIO
    if (asioDuplex) {
        if (!asioShared_->runDuplex(inputRing_, *this)) {
            std::lock_guard lock(statusMutex_);
            status_.inputError = status_.outputError = asioShared_->error();
            return false;
        }
    } else
#endif
    {
        if (!input_->runInput(inputRing_)) {
            std::lock_guard lock(statusMutex_);
            status_.inputError = input_->error();
            return false;
        }
        if (!output_->runOutput(*this)) {
            std::lock_guard lock(statusMutex_);
            status_.outputError = output_->error();
            return false;
        }
    }

    // ---- monitor ---------------------------------------------------------
    // Opened last, and its failure is not the engine's failure: the signal
    // still reaches the cable, and only the operator's headphones are missing.
    // The error is recorded for the UI to show rather than thrown upwards.
    {
        std::lock_guard lock(statusMutex_);
        status_.monitorRunning = false;
        status_.monitorError.clear();
    }
    openMonitor(outputRate, output_->actualChannels());

    // ---- publish what was actually negotiated ----------------------------
    std::lock_guard lock(statusMutex_);
    status_.running            = true;
    status_.inputSampleRate    = inputRate;
    status_.outputSampleRate   = outputRate;
    status_.inputChannels      = inputChannels;
    status_.outputChannels     = output_->actualChannels();
    status_.inputBufferFrames  = input_->bufferFrames();
    status_.outputBufferFrames = outputPeriod;

    if (auto* wasapiIn = dynamic_cast<WasapiInputStream*>(input_)) {
        status_.inputFormat = std::string(toString(wasapiIn->wireFormat())) + ", " +
                              toString(wasapiIn->mode());
    } else if (config_.input.backend == BackendType::Asio) {
        status_.inputFormat = "ASIO native";
    } else {
        status_.inputFormat = "16-bit int";
    }

    if (auto* wasapiOut = dynamic_cast<WasapiOutputStream*>(output_)) {
        status_.outputFormat = std::string(toString(wasapiOut->wireFormat())) + ", " +
                               toString(wasapiOut->mode());
    } else if (config_.output.backend == BackendType::Asio) {
        status_.outputFormat = "ASIO native";
    } else {
        status_.outputFormat = "16-bit int";
    }

    return true;
}

void Engine::allocateBuffers(double outputSampleRate)
{
    audioOutputRate_ = outputSampleRate;

    chain_.prepare(outputSampleRate, maxBlockFrames_, internalChannels_);
    limiter_.prepare(outputSampleRate, internalChannels_);
    inputMeter_.prepare(outputSampleRate);
    outputMeter_.prepare(outputSampleRate);
    inputSpectrum_.prepare(outputSampleRate);
    outputSpectrum_.prepare(outputSampleRate);

    // 20 ms ramps: fast enough to feel immediate on a fader, slow enough that
    // no step is audible as a click.
    inputGain_.prepare(static_cast<float>(outputSampleRate), 20.0f, 1.0f);
    outputGain_.prepare(static_cast<float>(outputSampleRate), 20.0f, 1.0f);
    muteGain_.prepare(static_cast<float>(outputSampleRate), 10.0f, 1.0f);

    // Worst case input demand: the largest block, at the highest ratio the
    // drift controller can ask for, plus the interpolator's neighbourhood.
    const int maxInputFrames =
        static_cast<int>(maxBlockFrames_ * nominalRatio_ * 1.01) + 16;

    staging_.assign(static_cast<size_t>(maxInputFrames) * kMaxChannels, 0.0f);
    inputPlanar_.resize(internalChannels_, maxInputFrames);
    work_.resize(internalChannels_, maxBlockFrames_);

    resampler_.prepare(internalChannels_);
    drift_.reset(nominalRatio_);

    cpuLoadSmoothed_ = 0.0;
    primed_          = false;
    starvedBlocks_   = 0;
    starvedBlocksToFlag_ =
        std::max(1, static_cast<int>(outputSampleRate / std::max(1, maxBlockFrames_)));
}

Engine::LatencyBreakdown Engine::latencyBreakdown() const
{
    LatencyBreakdown breakdown;

    const EngineStatus snapshot = status();
    if (!snapshot.running || snapshot.outputSampleRate <= 0.0)
        return breakdown;

    const double outputRate = snapshot.outputSampleRate;
    const auto toMs = [outputRate](double frames) {
        return static_cast<float>(1000.0 * frames / outputRate);
    };

    // Input-side figures are counted in input frames, so they are converted to
    // the output clock before being added to anything else - otherwise a
    // 96/48 kHz pair reports twice the delay it really has.
    const double ratio = std::max(1e-9, nominalRatio_);

    breakdown.inputDevice = toMs((input_ ? input_->latencyFrames() : 0) / ratio);
    breakdown.bridge      = toMs(targetRingFill_ / ratio);
    breakdown.chain       = toMs(chain_.latencySamples());
    breakdown.limiter     = toMs(limiter_.latencySamples());
    breakdown.outputDevice = toMs(output_ ? output_->latencyFrames() : 0);

    return breakdown;
}

float Engine::latencyMs() const
{
    return latencyBreakdown().total();
}

void Engine::updateSlowMeters()
{
    meters_.latencyMs.store(latencyMs(), std::memory_order_relaxed);

    if (input_)
        meters_.inputXruns.store(input_->xruns(), std::memory_order_relaxed);
    if (output_)
        meters_.outputXruns.store(output_->xruns(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void Engine::produce(float* interleaved, int channels, int frames)
{
    ScopedNoDenormals noDenormals;

    const double startTicks = performanceCounter();

    int done = 0;
    while (done < frames) {
        const int chunk = std::min(maxBlockFrames_, frames - done);
        processChunk(interleaved + static_cast<size_t>(done) * channels, channels, chunk);
        done += chunk;
    }

    // ---- monitor tap -----------------------------------------------------
    // Exactly the bytes going to the destination, so what the operator hears
    // is what is being sent - including the mute, the limiter and the output
    // fold-down, rather than a flattering version of it.
    //
    // A full ring is dropped on the floor rather than waited on. The monitor
    // is a convenience; the signal path is not, and this thread has a deadline
    // to meet either way.
    if (monitorActive_.load(std::memory_order_acquire)) {
        if (params_.monitorMute.load(std::memory_order_relaxed))
            monitorRing_.writeSilence(frames);
        else
            monitorRing_.write(interleaved, frames);
    }

    updateCpuLoad((performanceCounter() - startTicks) / counterFrequency_, frames);
}

void Engine::processChunk(float* dst, int dstChannels, int frames)
{
    // ---- priming ---------------------------------------------------------
    // The render device starts asking for audio before the capture device has
    // delivered any. Processing that would mean running the whole chain, and
    // the resampler, against an empty buffer for as long as it takes the input
    // to arrive. Silence until there is something real to work with costs a few
    // tens of milliseconds at start-up that nobody hears.
    if (!primed_) {
        if (inputRing_.filled() < targetRingFill_) {
            std::memset(dst, 0, static_cast<size_t>(frames) * dstChannels * sizeof(float));
            return;
        }

        // Start from a clean slate now that the buffer is at its working level.
        primed_ = true;
        resampler_.reset();
        drift_.reset(nominalRatio_);
    }

    // ---- clock bridge ----------------------------------------------------
    // A backlog far beyond target means a device stalled and then caught up in
    // one burst. Draining it gradually would leave latency permanently high, so
    // the excess is discarded in one step instead.
    if (targetRingFill_ > 0 && inputRing_.filled() > targetRingFill_ * 4)
        inputRing_.trimTo(targetRingFill_);

    const double ratio = drift_.update(inputRing_.filled(), targetRingFill_);
    meters_.driftPpm.store(static_cast<float>(drift_.correctionPpm()),
                           std::memory_order_relaxed);

    const int maxStagingFrames = static_cast<int>(staging_.size()) / kMaxChannels;
    const int needed = std::min(resampler_.inputFramesNeeded(frames, ratio), maxStagingFrames);

    const int inputChannels = std::max(1, inputRing_.channels());
    const int available     = inputRing_.peek(staging_.data(), needed);

    // Sustained emptiness means the capture device is not producing audio at
    // all, which no amount of rate correction can fix. Counting blocks rather
    // than reacting instantly avoids flagging the normal empty ring during the
    // first moments after the stream starts.
    if (available < needed) {
        if (++starvedBlocks_ == starvedBlocksToFlag_)
            meters_.inputStarved.store(true, std::memory_order_relaxed);
    } else if (starvedBlocks_ != 0) {
        starvedBlocks_ = 0;
        meters_.inputStarved.store(false, std::memory_order_relaxed);
    }

    inputPlanar_.setActiveFrames(available);
    inputPlanar_.readInterleaved(staging_.data(), inputChannels, available);

    // ---- input fold-down -------------------------------------------------
    // Applied before the resampler so everything downstream, including the
    // meters and the spectrum, sees what the chain will actually process.
    const auto mix = static_cast<InputMix>(params_.inputMix.load(std::memory_order_relaxed));
    if (mix != InputMix::Stereo && internalChannels_ > 0) {
        float* first = inputPlanar_.channel(0);

        if (mix == InputMix::MonoSum) {
            const float scale = 1.0f / static_cast<float>(internalChannels_);
            for (int i = 0; i < available; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < internalChannels_; ++c)
                    sum += inputPlanar_.channel(c)[i];
                first[i] = sum * scale;
            }
        } else if (mix == InputMix::MonoRight && internalChannels_ > 1) {
            std::memcpy(first, inputPlanar_.channel(1),
                        static_cast<size_t>(available) * sizeof(float));
        }
        // MonoLeft needs nothing: channel 0 already holds it.

        for (int c = 1; c < internalChannels_; ++c)
            std::memcpy(inputPlanar_.channel(c), first,
                        static_cast<size_t>(available) * sizeof(float));
    }

    work_.setActiveFrames(frames);

    bool underrun = false;
    const int consumed = resampler_.process(inputPlanar_.data(), available,
                                            work_.data(), frames, ratio, underrun);
    inputRing_.advanceRead(std::min(consumed, inputRing_.filled()));

    // ---- input trim ------------------------------------------------------
    const bool bypass = params_.bypassAll.load(std::memory_order_relaxed);

    inputGain_.setTarget(dsp::dbToGain(params_.inputGainDb.load(std::memory_order_relaxed)));
    for (int i = 0; i < frames; ++i) {
        const float g = inputGain_.next();
        for (int c = 0; c < internalChannels_; ++c)
            work_.channel(c)[i] *= g;
    }

    inputMeter_.process(work_);
    inputSpectrum_.push(work_);
    meters_.inputPeak.store(inputMeter_.peak(), std::memory_order_relaxed);
    meters_.inputRms.store(inputMeter_.rms(), std::memory_order_relaxed);

    // ---- processing chain ------------------------------------------------
    if (!bypass) {
        chain_.process(work_);

        // A plugin that emits NaN or a runaway value would otherwise poison
        // every filter downstream and leave the output permanently dead.
        for (int c = 0; c < internalChannels_; ++c)
            sanitize(work_.channel(c), frames);
    }

    // ---- output trim, limiter, mute --------------------------------------
    outputGain_.setTarget(dsp::dbToGain(params_.outputGainDb.load(std::memory_order_relaxed)));
    for (int i = 0; i < frames; ++i) {
        const float g = outputGain_.next();
        for (int c = 0; c < internalChannels_; ++c)
            work_.channel(c)[i] *= g;
    }

    if (params_.limiterEnabled.load(std::memory_order_relaxed) && !bypass) {
        const float ceiling =
            dsp::dbToGain(params_.limiterCeilingDb.load(std::memory_order_relaxed));
        const float reduction = limiter_.process(
            work_, ceiling, params_.limiterReleaseMs.load(std::memory_order_relaxed));
        meters_.limiterReductionDb.store(reduction, std::memory_order_relaxed);
    } else {
        meters_.limiterReductionDb.store(0.0f, std::memory_order_relaxed);
    }

    muteGain_.setTarget(params_.mute.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    if (!muteGain_.settled() || muteGain_.target() == 0.0f) {
        for (int i = 0; i < frames; ++i) {
            const float g = muteGain_.next();
            for (int c = 0; c < internalChannels_; ++c)
                work_.channel(c)[i] *= g;
        }
    }

    // ---- output fold-down ------------------------------------------------
    // After the limiter, so summing cannot push the result back over the
    // ceiling the limiter just enforced.
    if (params_.monoOutput.load(std::memory_order_relaxed) && internalChannels_ > 1) {
        const float scale = 1.0f / static_cast<float>(internalChannels_);
        float* first = work_.channel(0);

        for (int i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < internalChannels_; ++c)
                sum += work_.channel(c)[i];
            first[i] = sum * scale;
        }
        for (int c = 1; c < internalChannels_; ++c)
            std::memcpy(work_.channel(c), first, static_cast<size_t>(frames) * sizeof(float));
    }

    outputMeter_.process(work_);
    outputSpectrum_.push(work_);
    meters_.outputPeak.store(outputMeter_.peak(), std::memory_order_relaxed);
    meters_.outputRms.store(outputMeter_.rms(), std::memory_order_relaxed);

    work_.writeInterleaved(dst, dstChannels, frames);
}

void Engine::updateCpuLoad(double elapsedSeconds, int frames)
{
    // No lock here: this runs on the audio thread, and `audioOutputRate_` is
    // only written while that thread is stopped.
    if (audioOutputRate_ <= 0.0)
        return;

    const double budget = frames / audioOutputRate_;
    if (budget <= 0.0)
        return;

    const double load = elapsedSeconds / budget;

    // Rises instantly, decays slowly: a chain that occasionally overruns its
    // budget is the interesting case, and an average would hide it.
    cpuLoadSmoothed_ = (load > cpuLoadSmoothed_) ? load
                                                 : cpuLoadSmoothed_ * 0.99 + load * 0.01;

    meters_.cpuLoad.store(static_cast<float>(cpuLoadSmoothed_), std::memory_order_relaxed);
}

} // namespace rv::audio
