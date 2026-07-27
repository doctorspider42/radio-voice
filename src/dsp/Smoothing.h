#pragma once

#include <cmath>

namespace rv::dsp {

/// One-pole exponential ramp toward a target.
///
/// Gain and threshold changes are applied per sample through this so that
/// dragging a slider does not produce zipper noise. The coefficient is the
/// classic exp(-1 / (tau * fs)) so `timeMs` is the time constant, not the
/// full settling time.
class Smoothed {
public:
    void prepare(float sampleRate, float timeMs, float initial = 0.0f) noexcept
    {
        setTime(sampleRate, timeMs);
        current_ = target_ = initial;
    }

    void setTime(float sampleRate, float timeMs) noexcept
    {
        const float tau = std::max(0.01f, timeMs) * 0.001f;
        coeff_ = std::exp(-1.0f / (tau * std::max(1.0f, sampleRate)));
    }

    void setTarget(float v) noexcept { target_ = v; }
    void snapTo(float v) noexcept { current_ = target_ = v; }

    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }

    inline float next() noexcept
    {
        current_ = target_ + (current_ - target_) * coeff_;
        return current_;
    }

    /// True when the ramp has effectively finished, so callers can take a
    /// cheaper constant-gain path for the whole block.
    bool settled(float epsilon = 1.0e-5f) const noexcept
    {
        return std::abs(current_ - target_) < epsilon;
    }

private:
    float coeff_   = 0.0f;
    float current_ = 0.0f;
    float target_  = 0.0f;
};

/// Envelope follower with independent attack and release coefficients.
class AttackRelease {
public:
    void prepare(float sampleRate) noexcept
    {
        sampleRate_ = std::max(1.0f, sampleRate);
        envelope_   = 0.0f;
    }

    void setTimes(float attackMs, float releaseMs) noexcept
    {
        attackCoeff_  = coeffFor(attackMs);
        releaseCoeff_ = coeffFor(releaseMs);
    }

    void reset(float v = 0.0f) noexcept { envelope_ = v; }
    float value() const noexcept { return envelope_; }

    inline float process(float input) noexcept
    {
        const float c = (input > envelope_) ? attackCoeff_ : releaseCoeff_;
        envelope_ = input + (envelope_ - input) * c;
        return envelope_;
    }

private:
    float coeffFor(float ms) const noexcept
    {
        if (ms <= 0.0f)
            return 0.0f;
        return std::exp(-1.0f / (ms * 0.001f * sampleRate_));
    }

    float sampleRate_   = 48000.0f;
    float attackCoeff_  = 0.0f;
    float releaseCoeff_ = 0.0f;
    float envelope_     = 0.0f;
};

inline float dbToGain(float db) noexcept
{
    return db <= -100.0f ? 0.0f : std::pow(10.0f, db * 0.05f);
}

inline float gainToDb(float gain) noexcept
{
    return gain <= 1.0e-6f ? -120.0f : 20.0f * std::log10(gain);
}

} // namespace rv::dsp
