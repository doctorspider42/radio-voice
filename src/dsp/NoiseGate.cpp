#include "dsp/NoiseGate.h"

#include <algorithm>
#include <cmath>

namespace rv::dsp {

NoiseGate::NoiseGate(Params& params, Meters& meters)
    : params_(params)
    , meters_(meters)
{
}

void NoiseGate::prepare(double sampleRate, int /*maxFrames*/, int channels)
{
    sampleRate_ = sampleRate;
    channels_   = std::clamp(channels, 1, kMaxChannels);

    delayCapacity_ = static_cast<int>(kMaxLookaheadMs * 0.001 * sampleRate_) + 2;
    delay_.assign(static_cast<size_t>(channels_),
                  std::vector<float>(static_cast<size_t>(delayCapacity_), 0.0f));

    detector_.prepare(static_cast<float>(sampleRate_));
    // Fast enough to catch a syllable onset, slow enough that the level
    // estimate does not collapse between glottal pulses.
    detector_.setTimes(0.5f, 25.0f);

    revision_ = 0;
    updateSettings();
    reset();
}

void NoiseGate::reset()
{
    for (auto& ch : delay_)
        std::fill(ch.begin(), ch.end(), 0.0f);

    writePos_    = 0;
    sidechainState_.reset();
    detector_.reset(0.0f);
    currentGain_ = rangeGain_;
    open_        = false;
    holdCounter_ = 0;
}

void NoiseGate::updateSettings()
{
    const float fs = static_cast<float>(sampleRate_);

    const float thresholdDb  = params_.gateThresholdDb.load(std::memory_order_relaxed);
    const float hysteresisDb = std::max(0.0f, params_.gateHysteresisDb.load(std::memory_order_relaxed));

    openThreshold_  = dbToGain(thresholdDb);
    closeThreshold_ = dbToGain(thresholdDb - hysteresisDb);
    rangeGain_      = dbToGain(params_.gateRangeDb.load(std::memory_order_relaxed));

    const float attackMs  = std::max(0.05f, params_.gateAttackMs.load(std::memory_order_relaxed));
    const float releaseMs = std::max(1.0f,  params_.gateReleaseMs.load(std::memory_order_relaxed));
    attackCoeff_  = std::exp(-1.0f / (attackMs  * 0.001f * fs));
    releaseCoeff_ = std::exp(-1.0f / (releaseMs * 0.001f * fs));

    holdSamples_ = static_cast<int>(
        std::max(0.0f, params_.gateHoldMs.load(std::memory_order_relaxed)) * 0.001f * fs);

    const float lookaheadMs = std::clamp(
        params_.gateLookaheadMs.load(std::memory_order_relaxed), 0.0f, kMaxLookaheadMs);
    delaySamples_ = std::min(static_cast<int>(lookaheadMs * 0.001f * fs), delayCapacity_ - 1);

    sidechainHpf_ = Biquad::highPass(
        params_.gateSidechainHpfHz.load(std::memory_order_relaxed), 0.707f, fs);
}

void NoiseGate::process(PlanarBuffer& buffer)
{
    const bool enabled = params_.gateEnabled.load(std::memory_order_relaxed);

    const u32 rev = params_.revision.load(std::memory_order_acquire);
    if (rev != revision_) {
        revision_ = rev;
        updateSettings();
    }

    const int frames   = buffer.frames();
    const int channels = std::min(buffer.channels(), channels_);

    if (!enabled) {
        // Still push audio through the delay line so that switching the gate
        // back on does not splice in stale samples, but keep the gain at unity.
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels; ++c) {
                float* line = delay_[static_cast<size_t>(c)].data();
                const float in = buffer.channel(c)[i];
                line[writePos_] = in;
            }
            writePos_ = (writePos_ + 1) % delayCapacity_;
        }
        currentGain_ = 1.0f;
        open_        = true;
        meters_.gateReductionDb.store(0.0f, std::memory_order_relaxed);
        meters_.gateOpen.store(true, std::memory_order_relaxed);
        return;
    }

    const int readOffset = delayCapacity_ - delaySamples_;
    float minGain = 1.0f;

    for (int i = 0; i < frames; ++i) {
        // --- side-chain: mono sum, high-passed, peak-followed --------------
        float mono = 0.0f;
        for (int c = 0; c < channels; ++c)
            mono += buffer.channel(c)[i];
        mono /= static_cast<float>(channels);

        const float filtered = sidechainHpf_.process(mono, sidechainState_);
        const float level    = detector_.process(std::abs(filtered));

        // --- state machine with a hysteresis dead-band --------------------
        if (level > openThreshold_) {
            open_        = true;
            holdCounter_ = holdSamples_;
        } else if (level < closeThreshold_) {
            if (holdCounter_ > 0)
                --holdCounter_;
            else
                open_ = false;
        }
        // Between the two thresholds the current state is retained.

        const float target = open_ ? 1.0f : rangeGain_;
        const float coeff  = (target > currentGain_) ? attackCoeff_ : releaseCoeff_;
        currentGain_ = target + (currentGain_ - target) * coeff;

        minGain = std::min(minGain, currentGain_);

        // --- delay line: write current, read look-ahead-delayed -----------
        const int readPos = (writePos_ + readOffset) % delayCapacity_;
        for (int c = 0; c < channels; ++c) {
            float* line = delay_[static_cast<size_t>(c)].data();
            line[writePos_] = buffer.channel(c)[i];
            buffer.channel(c)[i] = line[readPos] * currentGain_;
        }
        writePos_ = (writePos_ + 1) % delayCapacity_;
    }

    meters_.gateReductionDb.store(gainToDb(minGain), std::memory_order_relaxed);
    meters_.gateOpen.store(open_, std::memory_order_relaxed);
}

} // namespace rv::dsp
