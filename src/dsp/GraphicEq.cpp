#include "dsp/GraphicEq.h"

#include <cmath>

namespace rv::dsp {

GraphicEq::GraphicEq(Params& params)
    : params_(params)
{
}

void GraphicEq::prepare(double sampleRate, int /*maxFrames*/, int channels)
{
    sampleRate_ = sampleRate;
    channels_   = std::clamp(channels, 1, kMaxChannels);

    bandState_.assign(static_cast<size_t>(channels_), {});
    highPassState_.assign(static_cast<size_t>(channels_), {});
    lowPassState_.assign(static_cast<size_t>(channels_), {});

    revision_ = 0; // force a recompute on the first block
    updateCoefficients();
}

void GraphicEq::reset()
{
    for (auto& ch : bandState_)
        for (auto& s : ch)
            s.reset();
    for (auto& ch : highPassState_)
        for (auto& s : ch)
            s.reset();
    for (auto& ch : lowPassState_)
        for (auto& s : ch)
            s.reset();
}

void GraphicEq::updateCoefficients()
{
    const float fs = static_cast<float>(sampleRate_);

    for (int i = 0; i < kEqBands; ++i) {
        const float gain = params_.eqGainDb[i].load(std::memory_order_relaxed);
        const float q    = params_.eqQ[i].load(std::memory_order_relaxed);
        const float hz   = kEqCenters[i];

        // Shelves at the extremes: a peaking filter at 31 Hz or 16 kHz leaves
        // the very bottom and top of the spectrum untouched, which is never
        // what someone dragging the end slider expects.
        if (i == 0)
            bands_[i] = Biquad::lowShelf(hz, q, gain, fs);
        else if (i == kEqBands - 1)
            bands_[i] = Biquad::highShelf(hz, q, gain, fs);
        else
            bands_[i] = Biquad::peaking(hz, q, gain, fs);
    }

    hpfActive_ = params_.hpfEnabled.load(std::memory_order_relaxed);
    if (hpfActive_) {
        const float hz = params_.hpfHz.load(std::memory_order_relaxed);
        highPass_[0] = Biquad::highPass(hz, Biquad::kButterworthQ1, fs);
        highPass_[1] = Biquad::highPass(hz, Biquad::kButterworthQ2, fs);
    }

    lpfActive_ = params_.lpfEnabled.load(std::memory_order_relaxed);
    if (lpfActive_) {
        const float hz = params_.lpfHz.load(std::memory_order_relaxed);
        lowPass_[0] = Biquad::lowPass(hz, Biquad::kButterworthQ1, fs);
        lowPass_[1] = Biquad::lowPass(hz, Biquad::kButterworthQ2, fs);
    }
}

void GraphicEq::process(PlanarBuffer& buffer)
{
    if (!params_.eqEnabled.load(std::memory_order_relaxed))
        return;

    const u32 rev = params_.revision.load(std::memory_order_acquire);
    if (rev != revision_) {
        revision_ = rev;
        updateCoefficients();
    }

    const int frames   = buffer.frames();
    const int channels = std::min(buffer.channels(), channels_);

    for (int c = 0; c < channels; ++c) {
        float* samples = buffer.channel(c);
        auto&  bs      = bandState_[static_cast<size_t>(c)];
        auto&  hs      = highPassState_[static_cast<size_t>(c)];
        auto&  ls      = lowPassState_[static_cast<size_t>(c)];

        // One pass over the block per channel, all sections applied to a value
        // held in a register - far friendlier to the cache than one pass per
        // filter section over the whole block.
        for (int i = 0; i < frames; ++i) {
            float x = samples[i];

            if (hpfActive_) {
                x = highPass_[0].process(x, hs[0]);
                x = highPass_[1].process(x, hs[1]);
            }

            for (int b = 0; b < kEqBands; ++b)
                x = bands_[b].process(x, bs[static_cast<size_t>(b)]);

            if (lpfActive_) {
                x = lowPass_[0].process(x, ls[0]);
                x = lowPass_[1].process(x, ls[1]);
            }

            samples[i] = x;
        }
    }
}

float GraphicEq::responseDb(const Params& params, float hz, float sampleRate)
{
    if (!params.eqEnabled.load(std::memory_order_relaxed))
        return 0.0f;

    float magnitude = 1.0f;

    for (int i = 0; i < kEqBands; ++i) {
        const float gain = params.eqGainDb[i].load(std::memory_order_relaxed);
        if (gain == 0.0f)
            continue;

        const float q = params.eqQ[i].load(std::memory_order_relaxed);
        Biquad f;
        if (i == 0)
            f = Biquad::lowShelf(kEqCenters[i], q, gain, sampleRate);
        else if (i == kEqBands - 1)
            f = Biquad::highShelf(kEqCenters[i], q, gain, sampleRate);
        else
            f = Biquad::peaking(kEqCenters[i], q, gain, sampleRate);

        magnitude *= f.magnitudeAt(hz, sampleRate);
    }

    if (params.hpfEnabled.load(std::memory_order_relaxed)) {
        const float f0 = params.hpfHz.load(std::memory_order_relaxed);
        magnitude *= Biquad::highPass(f0, Biquad::kButterworthQ1, sampleRate)
                         .magnitudeAt(hz, sampleRate);
        magnitude *= Biquad::highPass(f0, Biquad::kButterworthQ2, sampleRate)
                         .magnitudeAt(hz, sampleRate);
    }

    if (params.lpfEnabled.load(std::memory_order_relaxed)) {
        const float f0 = params.lpfHz.load(std::memory_order_relaxed);
        magnitude *= Biquad::lowPass(f0, Biquad::kButterworthQ1, sampleRate)
                         .magnitudeAt(hz, sampleRate);
        magnitude *= Biquad::lowPass(f0, Biquad::kButterworthQ2, sampleRate)
                         .magnitudeAt(hz, sampleRate);
    }

    return 20.0f * std::log10(std::max(magnitude, 1.0e-6f));
}

} // namespace rv::dsp
