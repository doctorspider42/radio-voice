#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Params.h"
#include "dsp/AudioBuffer.h"
#include "dsp/Smoothing.h"

namespace rv::dsp {

/// Sliding-window maximum in amortised O(1) per sample.
///
/// A monotonically decreasing deque of candidates: any sample that is smaller
/// than one arriving after it can never be the window maximum again, so it is
/// discarded immediately. The front of the deque is therefore always the
/// maximum of the current window. The alternative - rescanning the window every
/// sample - costs the window length per sample, which at 48 kHz is millions of
/// redundant comparisons per second.
class SlidingMax {
public:
    void prepare(int windowSize)
    {
        window_ = std::max(1, windowSize);
        values_.assign(static_cast<size_t>(window_ + 1), 0.0f);
        indices_.assign(static_cast<size_t>(window_ + 1), 0);
        reset();
    }

    void reset()
    {
        head_ = 0;
        count_ = 0;
        position_ = 0;
    }

    /// Feeds one sample and returns the maximum over the trailing window.
    inline float push(float v) noexcept
    {
        const size_t cap = values_.size();

        // Drop candidates that this sample dominates.
        while (count_ > 0) {
            const size_t back = (head_ + count_ - 1) % cap;
            if (values_[back] > v)
                break;
            --count_;
        }

        const size_t slot = (head_ + count_) % cap;
        values_[slot]  = v;
        indices_[slot] = position_;
        ++count_;

        // Drop the front once it has fallen out of the window.
        if (indices_[head_] + window_ <= position_) {
            head_ = (head_ + 1) % cap;
            --count_;
        }

        ++position_;
        return values_[head_];
    }

private:
    std::vector<float> values_;
    std::vector<i64>   indices_;
    size_t head_   = 0;
    size_t count_  = 0;
    i64    position_ = 0;
    int    window_ = 1;
};

/// Look-ahead brick-wall limiter on the output.
///
/// This is not a creative dynamics stage - it exists so that a hot microphone,
/// an over-enthusiastic EQ boost or a plugin with an unexpected output level
/// cannot send clipped samples into the virtual cable, where the receiving
/// application has no way to recover them.
///
/// Gain reduction is derived from the peak over the look-ahead window and
/// applied to the delayed signal, so every overshoot is caught before it is
/// audible. Channels are linked: per-channel gain would shift the stereo image
/// whenever one side peaked.
class Limiter {
public:
    void prepare(double sampleRate, int channels)
    {
        sampleRate_ = sampleRate;
        channels_   = std::clamp(channels, 1, kMaxChannels);

        // 2 ms of look-ahead: long enough to catch a single-sample transient,
        // short enough that the added latency is irrelevant for live monitoring.
        lookahead_ = std::max(1, static_cast<int>(0.002 * sampleRate_));
        capacity_  = lookahead_ + 1;

        delay_.assign(static_cast<size_t>(channels_),
                      std::vector<float>(static_cast<size_t>(capacity_), 0.0f));
        peaks_.prepare(capacity_);

        writePos_    = 0;
        currentGain_ = 1.0f;
    }

    void reset()
    {
        for (auto& ch : delay_)
            std::fill(ch.begin(), ch.end(), 0.0f);
        peaks_.reset();
        writePos_    = 0;
        currentGain_ = 1.0f;
    }

    int latencySamples() const noexcept { return lookahead_; }

    /// Returns the largest gain reduction applied over the block, in dB
    /// (negative, or zero when the limiter never engaged).
    float process(PlanarBuffer& buffer, float ceilingLinear, float releaseMs)
    {
        const int frames   = buffer.frames();
        const int channels = std::min(buffer.channels(), channels_);

        const float releaseCoeff =
            std::exp(-1.0f / (std::max(1.0f, releaseMs) * 0.001f *
                              static_cast<float>(sampleRate_)));

        float minGain = 1.0f;

        for (int i = 0; i < frames; ++i) {
            float peak = 0.0f;
            for (int c = 0; c < channels; ++c) {
                delay_[static_cast<size_t>(c)][static_cast<size_t>(writePos_)] =
                    buffer.channel(c)[i];
                peak = std::max(peak, std::abs(buffer.channel(c)[i]));
            }

            const float windowPeak = peaks_.push(peak);

            const float required = (windowPeak > ceilingLinear && windowPeak > 1.0e-9f)
                                       ? ceilingLinear / windowPeak
                                       : 1.0f;

            // Instant attack - the look-ahead has already bought the time -
            // and an exponential release.
            currentGain_ = (required < currentGain_)
                               ? required
                               : required + (currentGain_ - required) * releaseCoeff;

            minGain = std::min(minGain, currentGain_);

            const int readPos = (writePos_ + 1) % capacity_;
            for (int c = 0; c < channels; ++c) {
                const float out =
                    delay_[static_cast<size_t>(c)][static_cast<size_t>(readPos)] * currentGain_;
                // The look-ahead peak search makes this clamp a no-op in theory;
                // it costs one instruction and removes any doubt in practice.
                buffer.channel(c)[i] = std::clamp(out, -ceilingLinear, ceilingLinear);
            }

            writePos_ = readPos;
        }

        return gainToDb(minGain);
    }

private:
    double sampleRate_  = 48000.0;
    int    channels_    = 2;
    int    lookahead_   = 96;
    int    capacity_    = 97;
    int    writePos_    = 0;
    float  currentGain_ = 1.0f;

    std::vector<std::vector<float>> delay_;
    SlidingMax                      peaks_;
};

} // namespace rv::dsp
