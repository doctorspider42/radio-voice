#pragma once

#include <vector>

#include "core/Params.h"
#include "dsp/Biquad.h"
#include "dsp/Node.h"
#include "dsp/Smoothing.h"

namespace rv::dsp {

/// Downward expander / gate with look-ahead, hysteresis and a side-chain
/// high-pass.
///
/// Three details make the difference between this and a naive threshold mute:
///
///  * **Look-ahead** - the detector runs on the undelayed signal while the gain
///    is applied to a delayed copy, so the gate is already open by the time the
///    transient it reacted to arrives. Without it every plosive loses its
///    attack.
///  * **Hysteresis** - the level required to open is higher than the level
///    required to close, which stops the gate chattering on speech that sits
///    right at the threshold.
///  * **Side-chain high-pass** - low-frequency rumble (desk thumps, HVAC,
///    handling noise) carries a lot of energy and would otherwise hold the gate
///    open continuously. Filtering the detector input, not the audio, fixes it.
class NoiseGate final : public ProcessorNode {
public:
    NoiseGate(Params& params, Meters& meters);

    NodeKind kind() const override { return NodeKind::Gate; }
    const std::string& name() const override { return name_; }

    void prepare(double sampleRate, int maxFrames, int channels) override;
    void reset() override;
    void process(PlanarBuffer& buffer) override;

    /// Nothing while it is switched off: the disabled branch passes the signal
    /// through undelayed, so counting the look-ahead there would overstate the
    /// figure the user is shown. The chain used to arrive at the same answer by
    /// skipping a bypassed node altogether, which is no longer how a built-in
    /// module is switched off.
    int latencySamples() const override { return isEnabled() ? delaySamples_ : 0; }

    /// The switch on the panel and the one in the chain list are this one flag.
    bool isEnabled() const override
    {
        return params_.gateEnabled.load(std::memory_order_relaxed);
    }
    void setEnabled(bool on) override
    {
        params_.gateEnabled.store(on, std::memory_order_relaxed);
        params_.touch();
    }

private:
    void updateSettings();

    Params&     params_;
    Meters&     meters_;
    std::string name_ = "Noise Gate";

    double sampleRate_ = 48000.0;
    int    channels_   = 2;
    u32    revision_   = 0;

    /// Upper bound on look-ahead, so the delay line is allocated once.
    static constexpr float kMaxLookaheadMs = 20.0f;

    std::vector<std::vector<float>> delay_;
    int delayCapacity_ = 0;
    int delaySamples_  = 0;
    int writePos_      = 0;

    Biquad        sidechainHpf_;
    Biquad::State sidechainState_;
    AttackRelease detector_;

    // Cached parameter values, refreshed when the revision changes.
    float openThreshold_  = 0.0f;   ///< Linear.
    float closeThreshold_ = 0.0f;
    float rangeGain_      = 0.0f;
    float attackCoeff_    = 0.0f;
    float releaseCoeff_   = 0.0f;
    int   holdSamples_    = 0;

    float currentGain_ = 1.0f;
    bool  open_        = false;
    int   holdCounter_ = 0;
};

} // namespace rv::dsp
