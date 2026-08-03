#pragma once

#include <string>
#include <vector>

#include "core/Params.h"
#include "dsp/Node.h"
#include "dsp/Smoothing.h"

struct DenoiseState;

namespace rv::dsp {

/// Neural noise suppression, wrapping RNNoise.
///
/// The noise gate can only decide that a whole block is silence and turn it
/// down. This decides, band by band and frame by frame, how much of what it is
/// hearing is speech - so a fan or an air conditioner is attenuated *while*
/// someone is talking, which no gate can do.
///
/// Three properties of the underlying library shape everything here:
///
///  * It works in fixed frames of 480 samples at 48 kHz. Blocks arriving from
///    the engine are any size at all, so the two are bridged by a FIFO, and the
///    cost is 480 samples of delay - reported honestly through latencySamples.
///    The dry side of the mix is delayed by the same frame, so a partial mix
///    blends two copies of the same instant rather than comb-filtering one
///    against a 10 ms echo of itself.
///
///  * It is mono. Each channel gets its own instance, which is also the only
///    correct choice: a shared one would let one channel's noise estimate
///    decide the other's gain.
///
///  * It expects floats in the *integer* range, roughly -32768 to 32767,
///    despite the interface being float. Feeding it the -1..1 the rest of this
///    application uses does not fail, it just quietly does almost nothing,
///    because everything looks like silence to it.
class NoiseSuppressor final : public ProcessorNode {
public:
    explicit NoiseSuppressor(Params& params, Meters& meters);
    ~NoiseSuppressor() override;

    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    NodeKind kind() const override { return NodeKind::NoiseSuppressor; }
    const std::string& name() const override { return name_; }

    void prepare(double sampleRate, int maxFrames, int channels) override;
    void reset() override;
    void process(PlanarBuffer& buffer) override;

    int latencySamples() const override { return active_ ? kFrameSize : 0; }

    /// The switch on the panel and the one in the chain list are this one flag.
    bool isEnabled() const override
    {
        return params_.denoiseEnabled.load(std::memory_order_relaxed);
    }
    void setEnabled(bool on) override
    {
        params_.denoiseEnabled.store(on, std::memory_order_relaxed);
    }

    /// True when the library is compiled in and running at a rate it supports.
    /// The panel says so rather than leaving a control that does nothing.
    bool isActive() const { return active_; }

    /// Why it is not running, when it is not.
    const std::string& inactiveReason() const { return inactiveReason_; }

private:
    /// What RNNoise consumes and produces per call, and the rate it assumes.
    static constexpr int kFrameSize  = 480;
    static constexpr int kSampleRate = 48000;

    /// Per-channel state and the FIFOs that turn arbitrary block sizes into
    /// whole frames.
    struct Channel {
        DenoiseState*      state = nullptr;
        std::vector<float> input;   ///< Awaiting a full frame.
        std::vector<float> output;  ///< Denoised, awaiting collection.
        int                filled = 0;   ///< Samples gathered towards the next frame.
        int                readAt = 0;   ///< Next sample to hand back.
    };

    void releaseChannels();

    Params& params_;
    Meters& meters_;
    std::string name_ = "Noise Suppressor";
    std::string inactiveReason_;

    std::vector<Channel> channels_;

    bool   active_ = false;
    double sampleRate_ = 0.0;

    /// Ramped so moving the mix slider does not step the signal.
    Smoothed mix_;
};

} // namespace rv::dsp
