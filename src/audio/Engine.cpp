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
    status_.running = false;
}

void Engine::closeStreams()
{
    // Render first: it is the side that pulls, so stopping it guarantees no
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
    starvedBlocks_   = 0;
    starvedBlocksToFlag_ =
        std::max(1, static_cast<int>(outputSampleRate / std::max(1, maxBlockFrames_)));
}

float Engine::latencyMs() const
{
    const EngineStatus snapshot = status();
    if (!snapshot.running || snapshot.outputSampleRate <= 0.0)
        return 0.0f;

    // Input-side figures are in input frames; convert them to the output clock
    // before adding, otherwise a 44.1/48 mismatch skews the total.
    const double inputFrames =
        (input_ ? input_->latencyFrames() : 0) + targetRingFill_;
    const double inputInOutputFrames =
        inputFrames / std::max(1e-9, nominalRatio_);

    const double frames = inputInOutputFrames +
                          chain_.latencySamples() +
                          limiter_.latencySamples() +
                          (output_ ? output_->latencyFrames() : 0);

    return static_cast<float>(1000.0 * frames / snapshot.outputSampleRate);
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

    updateCpuLoad((performanceCounter() - startTicks) / counterFrequency_, frames);
}

void Engine::processChunk(float* dst, int dstChannels, int frames)
{
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
