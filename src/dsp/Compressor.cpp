#include "dsp/Compressor.h"

#include <algorithm>
#include <cmath>

namespace rv::dsp {
namespace {

/// Below this the detector is treated as silence. Without a floor, log10 of a
/// denormal produces an enormous negative number that the knee arithmetic then
/// has to carry around.
constexpr float kFloorDb = -120.0f;

/// Static compression curve, shared by the audio path and the UI plot so the
/// drawn curve is the curve that runs.
///
/// The knee is the standard quadratic interpolation between the unity slope
/// below it and the 1/ratio slope above: continuous in value and in first
/// derivative at both ends, which is what stops the transition being audible.
float staticCurveOutputDb(float inputDb, float thresholdDb, float ratio, float kneeDb)
{
    const float over = inputDb - thresholdDb;

    if (kneeDb > 0.0f && 2.0f * over > -kneeDb && 2.0f * over < kneeDb) {
        const float x = over + kneeDb * 0.5f;
        return inputDb + (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
    }

    if (2.0f * over <= -kneeDb)
        return inputDb;

    return thresholdDb + over / ratio;
}

} // namespace

Compressor::Compressor(Params& params, Meters& meters)
    : params_(params)
    , meters_(meters)
{
}

void Compressor::prepare(double sampleRate, int /*maxFrames*/, int channels)
{
    sampleRate_ = sampleRate;
    channels_   = std::clamp(channels, 1, kMaxChannels);

    delayCapacity_ = static_cast<int>(kMaxLookaheadMs * 0.001 * sampleRate_) + 2;
    delay_.assign(static_cast<size_t>(channels_),
                  std::vector<float>(static_cast<size_t>(delayCapacity_), 0.0f));

    // 10 ms window for the RMS follower: long enough to average out the pitch
    // period of a low male voice, short enough to track syllables.
    rmsCoeff_ = std::exp(-1.0f / (0.010f * static_cast<float>(sampleRate_)));

    revision_ = 0;
    updateSettings();
    reset();
}

void Compressor::reset()
{
    for (auto& channel : delay_)
        std::fill(channel.begin(), channel.end(), 0.0f);

    writePos_    = 0;
    reductionDb_ = 0.0f;
    meanSquare_  = 0.0f;
    sidechainState_.reset();
}

void Compressor::updateSettings()
{
    const float fs = static_cast<float>(sampleRate_);

    thresholdDb_ = params_.compThresholdDb.load(std::memory_order_relaxed);
    ratio_       = std::max(1.0f, params_.compRatio.load(std::memory_order_relaxed));
    kneeDb_      = std::max(0.0f, params_.compKneeDb.load(std::memory_order_relaxed));

    makeupDb_ = params_.compAutoMakeup.load(std::memory_order_relaxed)
                    ? autoMakeupDb(params_)
                    : params_.compMakeupDb.load(std::memory_order_relaxed);

    const float attackMs  = std::max(0.05f, params_.compAttackMs.load(std::memory_order_relaxed));
    const float releaseMs = std::max(1.0f,  params_.compReleaseMs.load(std::memory_order_relaxed));
    attackCoeff_  = std::exp(-1.0f / (attackMs  * 0.001f * fs));
    releaseCoeff_ = std::exp(-1.0f / (releaseMs * 0.001f * fs));

    rmsDetection_ = params_.compRmsDetection.load(std::memory_order_relaxed);

    const float lookaheadMs = std::clamp(
        params_.compLookaheadMs.load(std::memory_order_relaxed), 0.0f, kMaxLookaheadMs);
    delaySamples_ = std::min(static_cast<int>(lookaheadMs * 0.001f * fs), delayCapacity_ - 1);

    sidechainHpf_ = Biquad::highPass(
        params_.compSidechainHpfHz.load(std::memory_order_relaxed), 0.707f, fs);
}

float Compressor::reductionForDb(float inputDb) const
{
    return inputDb - staticCurveOutputDb(inputDb, thresholdDb_, ratio_, kneeDb_);
}

float Compressor::curveOutputDb(const Params& params, float inputDb)
{
    const float threshold = params.compThresholdDb.load(std::memory_order_relaxed);
    const float ratio     = std::max(1.0f, params.compRatio.load(std::memory_order_relaxed));
    const float knee      = std::max(0.0f, params.compKneeDb.load(std::memory_order_relaxed));

    const float makeup = params.compAutoMakeup.load(std::memory_order_relaxed)
                             ? autoMakeupDb(params)
                             : params.compMakeupDb.load(std::memory_order_relaxed);

    return staticCurveOutputDb(inputDb, threshold, ratio, knee) + makeup;
}

float Compressor::autoMakeupDb(const Params& params)
{
    const float threshold = params.compThresholdDb.load(std::memory_order_relaxed);
    const float ratio     = std::max(1.0f, params.compRatio.load(std::memory_order_relaxed));

    // Half of what it would take to bring the threshold back to where it was.
    // Restoring it fully assumes the whole signal sits at the threshold, which
    // no real programme does, and consistently reads as too loud.
    return -0.5f * threshold * (1.0f - 1.0f / ratio);
}

void Compressor::process(PlanarBuffer& buffer)
{
    const bool enabled = params_.compEnabled.load(std::memory_order_relaxed);

    const u32 revision = params_.revision.load(std::memory_order_acquire);
    if (revision != revision_) {
        revision_ = revision;
        updateSettings();
    }

    const int frames   = buffer.frames();
    const int channels = std::min(buffer.channels(), channels_);

    if (!enabled) {
        // The delay line keeps running so that switching the compressor back on
        // does not splice in samples from whenever it was last active.
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels; ++c)
                delay_[static_cast<size_t>(c)][writePos_] = buffer.channel(c)[i];
            writePos_ = (writePos_ + 1) % delayCapacity_;
        }
        reductionDb_ = 0.0f;
        meters_.compressorReductionDb.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const int   readOffset = delayCapacity_ - delaySamples_;
    const float makeupGain = dbToGain(makeupDb_);
    float       maxReduction = 0.0f;

    for (int i = 0; i < frames; ++i) {
        // --- detector -----------------------------------------------------
        // Channels are linked: independent per-channel gain would move the
        // stereo image every time one side got louder.
        float peak = 0.0f;
        for (int c = 0; c < channels; ++c)
            peak = std::max(peak, std::abs(buffer.channel(c)[i]));

        const float filtered = sidechainHpf_.process(peak, sidechainState_);

        float level;
        if (rmsDetection_) {
            const float squared = filtered * filtered;
            meanSquare_ = squared + (meanSquare_ - squared) * rmsCoeff_;
            level = std::sqrt(std::max(0.0f, meanSquare_));
        } else {
            level = std::abs(filtered);
        }

        const float levelDb = (level > 1.0e-7f) ? 20.0f * std::log10(level) : kFloorDb;

        // --- static curve -------------------------------------------------
        const float targetReduction = reductionForDb(levelDb);

        // --- ballistics, in dB --------------------------------------------
        // More reduction than currently applied is an attack; less is a
        // release. Comparing the target against the current state, rather than
        // tracking the level, is what keeps the two times independent.
        const float coeff = (targetReduction > reductionDb_) ? attackCoeff_ : releaseCoeff_;
        reductionDb_ = targetReduction + (reductionDb_ - targetReduction) * coeff;

        maxReduction = std::max(maxReduction, reductionDb_);

        const float gain = dbToGain(-reductionDb_) * makeupGain;

        // --- delay line ---------------------------------------------------
        const int readPos = (writePos_ + readOffset) % delayCapacity_;
        for (int c = 0; c < channels; ++c) {
            float* line = delay_[static_cast<size_t>(c)].data();
            line[writePos_] = buffer.channel(c)[i];
            buffer.channel(c)[i] = line[readPos] * gain;
        }
        writePos_ = (writePos_ + 1) % delayCapacity_;
    }

    // Reported negative, matching every other gain-reduction meter here.
    meters_.compressorReductionDb.store(-maxReduction, std::memory_order_relaxed);
}

} // namespace rv::dsp
