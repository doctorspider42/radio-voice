#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/Types.h"

namespace rv::audio {

/// Variable-ratio resampler used to bridge two independent device clocks.
///
/// A microphone and a virtual cable are driven by different oscillators. Even
/// when both are nominally 48 kHz they differ by tens of parts per million, so
/// over minutes one side accumulates a surplus and the other starves. A fixed
/// ratio cannot fix that; the ratio has to be nudged continuously in response
/// to how full the input buffer is.
///
/// Interpolation is Catmull-Rom (cubic Hermite): it is C1 continuous, costs
/// four multiply-adds per sample and, at correction ratios within a few hundred
/// ppm of unity, its error sits far below the noise floor of any microphone.
/// Sinc interpolation would be measurably better only for large rate changes,
/// which is not what this is for.
class DriftResampler {
public:
    void prepare(int channels)
    {
        channels_ = std::clamp(channels, 1, kMaxChannels);
        reset();
    }

    void reset()
    {
        for (auto& channel : history_)
            channel.fill(0.0f);
        // Positions are expressed in an index space where 0..2 are the three
        // carried-over samples, so the first interpolation point that has a
        // full four-sample neighbourhood is 1.
        phase_ = 1.0;
    }

    /// Input frames needed to produce `outFrames` at `ratio`, plus the
    /// neighbourhood the interpolator reads past the last point.
    int inputFramesNeeded(int outFrames, double ratio) const
    {
        const double endPhase = phase_ + static_cast<double>(outFrames) * ratio;
        const int    highest  = static_cast<int>(std::floor(endPhase)) + 2;
        return std::max(0, highest - 2); // indices 0..2 are already in hand
    }

    /// Resamples planar input to planar output.
    ///
    /// Returns the number of input frames consumed. When `inFrames` is short of
    /// what the ratio demands, the tail is held rather than producing a gap,
    /// and `underrun` is set - a held sample is far less audible than silence.
    int process(const float* const* src, int inFrames,
                float* const* dst, int outFrames,
                double ratio, bool& underrun)
    {
        underrun = false;

        // Virtual index space: 0..2 are history, 3.. are src.
        const int virtualCount = inFrames + 3;
        const int lastIndex    = virtualCount - 1;

        for (int channel = 0; channel < channels_; ++channel) {
            const float* in  = src[channel];
            float*       out = dst[channel];
            const auto&  hist = history_[static_cast<size_t>(channel)];

            double phase = phase_;

            for (int i = 0; i < outFrames; ++i) {
                const int    index = static_cast<int>(phase);
                const double frac  = phase - index;

                const float y0 = sample(hist, in, inFrames, std::clamp(index - 1, 0, lastIndex));
                const float y1 = sample(hist, in, inFrames, std::clamp(index,     0, lastIndex));
                const float y2 = sample(hist, in, inFrames, std::clamp(index + 1, 0, lastIndex));
                const float y3 = sample(hist, in, inFrames, std::clamp(index + 2, 0, lastIndex));

                out[i] = catmullRom(y0, y1, y2, y3, static_cast<float>(frac));
                phase += ratio;
            }

            if (channel == channels_ - 1)
                phase_ = phase;
        }

        const int highestNeeded = static_cast<int>(std::floor(phase_)) + 2;
        if (highestNeeded > virtualCount - 1)
            underrun = true;

        // The new history must start one index below the next interpolation
        // point, so that the next call again has a full neighbourhood.
        const int keepFrom = std::clamp(static_cast<int>(std::floor(phase_)) - 1,
                                        0, std::max(0, virtualCount - 3));

        for (int channel = 0; channel < channels_; ++channel) {
            const float* in = src[channel];
            std::array<float, 3> next{};
            for (int k = 0; k < 3; ++k) {
                next[static_cast<size_t>(k)] =
                    sample(history_[static_cast<size_t>(channel)], in, inFrames,
                           std::clamp(keepFrom + k, 0, lastIndex));
            }
            history_[static_cast<size_t>(channel)] = next;
        }

        phase_ -= keepFrom;

        // Everything below the retained window has been fully consumed.
        return std::clamp(keepFrom, 0, inFrames);
    }

private:
    static float sample(const std::array<float, 3>& history, const float* src,
                        int srcCount, int index)
    {
        if (index < 3)
            return history[static_cast<size_t>(index)];
        const int offset = index - 3;
        return (offset < srcCount) ? src[offset] : 0.0f;
    }

    static float catmullRom(float y0, float y1, float y2, float y3, float t)
    {
        // Standard Catmull-Rom in the numerically friendly Horner arrangement.
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    std::array<std::array<float, 3>, kMaxChannels> history_{};
    double phase_    = 1.0;
    int    channels_ = 1;
};

/// Closed-loop controller that keeps the input ring at a target fill by nudging
/// the resample ratio.
///
/// Proportional term for responsiveness, integral term to remove the steady
/// state error that the constant clock difference would otherwise leave. Both
/// gains are deliberately tiny: the correction has to stay well under a
/// thousandth of the nominal rate, because anything larger is audible as pitch
/// modulation on a voice.
class DriftController {
public:
    void reset(double nominalRatio)
    {
        nominalRatio_ = nominalRatio;
        integral_     = 0.0;
    }

    double update(int currentFill, int targetFill)
    {
        if (targetFill <= 0)
            return nominalRatio_;

        const double error = static_cast<double>(currentFill - targetFill) /
                             static_cast<double>(targetFill);

        integral_ += kIntegralGain * error;
        integral_  = std::clamp(integral_, -kMaxCorrection, kMaxCorrection);

        const double correction =
            std::clamp(integral_ + kProportionalGain * error, -kMaxCorrection, kMaxCorrection);

        lastCorrection_ = correction;
        return nominalRatio_ * (1.0 + correction);
    }

    /// Accumulated clock difference, in parts per million.
    double correctionPpm() const { return lastCorrection_ * 1.0e6; }

private:
    // 0.5%, about 9 cents of pitch. Physical converters differ by tens of ppm
    // at most, but purely virtual endpoints - loopback cables, software
    // microphones - are driven by software timers and can be far worse, so the
    // clamp has to leave room for them rather than saturating and giving up.
    static constexpr double kMaxCorrection    = 0.005;
    static constexpr double kProportionalGain = 0.0005;
    static constexpr double kIntegralGain     = 0.000002;

    double nominalRatio_   = 1.0;
    double integral_       = 0.0;
    double lastCorrection_ = 0.0;
};

} // namespace rv::audio
