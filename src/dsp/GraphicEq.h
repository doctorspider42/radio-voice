#pragma once

#include <array>
#include <vector>

#include "core/Params.h"
#include "dsp/Biquad.h"
#include "dsp/Node.h"

namespace rv::dsp {

/// Ten-band graphic equaliser plus switchable 24 dB/oct high- and low-cut.
///
/// Coefficients are recomputed only when `Params::revision` changes, so a
/// static setting costs nothing beyond the biquad arithmetic itself.
class GraphicEq final : public ProcessorNode {
public:
    explicit GraphicEq(Params& params);

    NodeKind kind() const override { return NodeKind::Equalizer; }
    const std::string& name() const override { return name_; }

    void prepare(double sampleRate, int maxFrames, int channels) override;
    void reset() override;
    void process(PlanarBuffer& buffer) override;

    /// Combined magnitude response in dB at `hz`, for the curve in the UI.
    /// Safe to call from the UI thread: it reads the parameter block directly
    /// and designs throwaway filters rather than touching live state.
    static float responseDb(const Params& params, float hz, float sampleRate);

private:
    void updateCoefficients();

    Params&     params_;
    std::string name_ = "Equalizer";

    double sampleRate_ = 48000.0;
    int    channels_   = 2;
    u32    revision_   = 0;

    // Band filters, plus two cascaded sections each for the high- and low-cut.
    std::array<Biquad, kEqBands> bands_{};
    std::array<Biquad, 2>        highPass_{};
    std::array<Biquad, 2>        lowPass_{};

    bool hpfActive_ = false;
    bool lpfActive_ = false;

    // State is [channel][section].
    std::vector<std::array<Biquad::State, kEqBands>> bandState_;
    std::vector<std::array<Biquad::State, 2>>        highPassState_;
    std::vector<std::array<Biquad::State, 2>>        lowPassState_;
};

} // namespace rv::dsp
