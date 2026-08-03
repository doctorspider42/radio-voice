#pragma once

#include <vector>

#include "core/Params.h"
#include "dsp/Biquad.h"
#include "dsp/Node.h"
#include "dsp/Smoothing.h"

namespace rv::dsp {

/// Feed-forward compressor working in the decibel domain.
///
/// The signal path is the textbook one: measure the level, run it through a
/// static curve to find how much gain reduction it calls for, smooth that
/// reduction with attack and release, then apply it. Doing the smoothing on the
/// *gain* rather than on the level is what makes attack and release mean what
/// their names say - smoothing the level first would let a loud transient set a
/// reduction the release time then has no say over.
///
/// Everything happens in dB rather than in linear gain. A linear-domain
/// envelope decays exponentially in a way that sounds fast at the top and slow
/// at the bottom; in dB the release is perceptually even, which is why every
/// analogue design that people like behaves this way.
///
/// The features that matter for a voice, in order:
///
///   * **Soft knee** - a hard threshold makes the compressor audibly switch on
///     and off around it, which on speech is a constant tell. The knee blends
///     the two slopes over a range centred on the threshold.
///   * **Side-chain high-pass** - the same reason as the gate: low-frequency
///     energy is disproportionate, and without it every plosive ducks the whole
///     voice.
///   * **Look-ahead** - lets the reduction be fully applied by the time the
///     transient it reacted to arrives, so a fast attack does not have to mean
///     a clipped consonant.
class Compressor final : public ProcessorNode {
public:
    Compressor(Params& params, Meters& meters);

    NodeKind kind() const override { return NodeKind::Compressor; }
    const std::string& name() const override { return name_; }

    void prepare(double sampleRate, int maxFrames, int channels) override;
    void reset() override;
    void process(PlanarBuffer& buffer) override;

    /// Nothing while it is switched off, for the reason given on NoiseGate:
    /// the disabled branch does not delay the signal, so neither should the
    /// figure reported for it.
    int latencySamples() const override { return isEnabled() ? delaySamples_ : 0; }

    /// The switch on the panel and the one in the chain list are this one flag.
    bool isEnabled() const override
    {
        return params_.compEnabled.load(std::memory_order_relaxed);
    }
    void setEnabled(bool on) override
    {
        params_.compEnabled.store(on, std::memory_order_relaxed);
        params_.touch();
    }

    /// The static curve, in dB in and dB out, for the transfer plot in the UI.
    /// Reads the parameter block directly so it can be called from the UI
    /// thread without touching live state.
    static float curveOutputDb(const Params& params, float inputDb);

    /// Make-up gain the "auto" setting computes for the current settings.
    static float autoMakeupDb(const Params& params);

private:
    void updateSettings();

    /// Gain reduction the static curve calls for at `inputDb`, as a positive
    /// number of decibels.
    float reductionForDb(float inputDb) const;

    Params&     params_;
    Meters&     meters_;
    std::string name_ = "Compressor";

    double sampleRate_ = 48000.0;
    int    channels_   = 2;
    u32    revision_   = 0;

    static constexpr float kMaxLookaheadMs = 20.0f;

    std::vector<std::vector<float>> delay_;
    int delayCapacity_ = 0;
    int delaySamples_  = 0;
    int writePos_      = 0;

    Biquad        sidechainHpf_;
    Biquad::State sidechainState_;

    // Cached parameters, refreshed when the revision changes.
    float thresholdDb_ = -18.0f;
    float ratio_       = 3.0f;
    float kneeDb_      = 6.0f;
    float makeupDb_    = 0.0f;
    float attackCoeff_  = 0.0f;
    float releaseCoeff_ = 0.0f;
    bool  rmsDetection_ = false;

    /// Coefficient of the mean-square follower used in RMS mode.
    float rmsCoeff_    = 0.0f;
    float meanSquare_  = 0.0f;

    /// Current gain reduction in dB, always >= 0.
    float reductionDb_ = 0.0f;
};

} // namespace rv::dsp
