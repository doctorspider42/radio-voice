#pragma once

#include <algorithm>
#include <cmath>

namespace rv::dsp {

/// Second-order IIR section in transposed direct form II.
///
/// TDF-II is used rather than DF-I because it needs only two state words per
/// channel and has better numerical behaviour with float coefficients at the
/// low centre frequencies a 31 Hz EQ band demands.
///
/// Coefficients follow the Audio EQ Cookbook (Robert Bristow-Johnson),
/// normalised by a0 at design time so the inner loop has no divide.
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

    struct State {
        float z1 = 0.0f, z2 = 0.0f;
        void reset() noexcept { z1 = z2 = 0.0f; }
    };

    inline float process(float x, State& s) const noexcept
    {
        const float y = b0 * x + s.z1;
        s.z1 = b1 * x - a1 * y + s.z2;
        s.z2 = b2 * x - a2 * y;
        return y;
    }

    void setIdentity() noexcept
    {
        b0 = 1.0f;
        b1 = b2 = a1 = a2 = 0.0f;
    }

    /// Complex magnitude response at `hz`, for drawing the EQ curve.
    float magnitudeAt(float hz, float sampleRate) const noexcept
    {
        const double w  = 2.0 * 3.14159265358979323846 * hz / sampleRate;
        const double cw = std::cos(w), sw = std::sin(w);
        const double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);

        const double numRe = b0 + b1 * cw + b2 * c2w;
        const double numIm = -(b1 * sw + b2 * s2w);
        const double denRe = 1.0 + a1 * cw + a2 * c2w;
        const double denIm = -(a1 * sw + a2 * s2w);

        const double den = denRe * denRe + denIm * denIm;
        if (den < 1e-30)
            return 1.0f;

        return static_cast<float>(std::sqrt((numRe * numRe + numIm * numIm) / den));
    }

    // -- designers ---------------------------------------------------------

    static Biquad peaking(float hz, float q, float gainDb, float sampleRate) noexcept
    {
        Biquad f;
        if (gainDb == 0.0f) {
            f.setIdentity();
            return f;
        }

        const double A     = std::pow(10.0, gainDb / 40.0);
        const double w0    = tau() * clampHz(hz, sampleRate) / sampleRate;
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, static_cast<double>(q)));
        const double cw0   = std::cos(w0);

        const double b0 = 1.0 + alpha * A;
        const double b1 = -2.0 * cw0;
        const double b2 = 1.0 - alpha * A;
        const double a0 = 1.0 + alpha / A;
        const double a1 = -2.0 * cw0;
        const double a2 = 1.0 - alpha / A;

        return normalise(b0, b1, b2, a0, a1, a2);
    }

    static Biquad lowShelf(float hz, float q, float gainDb, float sampleRate) noexcept
    {
        Biquad f;
        if (gainDb == 0.0f) {
            f.setIdentity();
            return f;
        }

        const double A   = std::pow(10.0, gainDb / 40.0);
        const double w0  = tau() * clampHz(hz, sampleRate) / sampleRate;
        const double cw0 = std::cos(w0), sw0 = std::sin(w0);
        const double alpha = sw0 / 2.0 * std::sqrt((A + 1.0 / A) *
                             (1.0 / std::max(0.05, static_cast<double>(q)) - 1.0) + 2.0);
        const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;

        const double b0 =        A * ((A + 1.0) - (A - 1.0) * cw0 + twoSqrtAalpha);
        const double b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw0);
        const double b2 =        A * ((A + 1.0) - (A - 1.0) * cw0 - twoSqrtAalpha);
        const double a0 =             (A + 1.0) + (A - 1.0) * cw0 + twoSqrtAalpha;
        const double a1 =      -2.0 * ((A - 1.0) + (A + 1.0) * cw0);
        const double a2 =             (A + 1.0) + (A - 1.0) * cw0 - twoSqrtAalpha;

        return normalise(b0, b1, b2, a0, a1, a2);
    }

    static Biquad highShelf(float hz, float q, float gainDb, float sampleRate) noexcept
    {
        Biquad f;
        if (gainDb == 0.0f) {
            f.setIdentity();
            return f;
        }

        const double A   = std::pow(10.0, gainDb / 40.0);
        const double w0  = tau() * clampHz(hz, sampleRate) / sampleRate;
        const double cw0 = std::cos(w0), sw0 = std::sin(w0);
        const double alpha = sw0 / 2.0 * std::sqrt((A + 1.0 / A) *
                             (1.0 / std::max(0.05, static_cast<double>(q)) - 1.0) + 2.0);
        const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;

        const double b0 =        A * ((A + 1.0) + (A - 1.0) * cw0 + twoSqrtAalpha);
        const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw0);
        const double b2 =        A * ((A + 1.0) + (A - 1.0) * cw0 - twoSqrtAalpha);
        const double a0 =             (A + 1.0) - (A - 1.0) * cw0 + twoSqrtAalpha;
        const double a1 =       2.0 * ((A - 1.0) - (A + 1.0) * cw0);
        const double a2 =             (A + 1.0) - (A - 1.0) * cw0 - twoSqrtAalpha;

        return normalise(b0, b1, b2, a0, a1, a2);
    }

    static Biquad highPass(float hz, float q, float sampleRate) noexcept
    {
        const double w0    = tau() * clampHz(hz, sampleRate) / sampleRate;
        const double cw0   = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, static_cast<double>(q)));

        const double b0 =  (1.0 + cw0) / 2.0;
        const double b1 = -(1.0 + cw0);
        const double b2 =  (1.0 + cw0) / 2.0;
        const double a0 =   1.0 + alpha;
        const double a1 =  -2.0 * cw0;
        const double a2 =   1.0 - alpha;

        return normalise(b0, b1, b2, a0, a1, a2);
    }

    static Biquad lowPass(float hz, float q, float sampleRate) noexcept
    {
        const double w0    = tau() * clampHz(hz, sampleRate) / sampleRate;
        const double cw0   = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, static_cast<double>(q)));

        const double b0 = (1.0 - cw0) / 2.0;
        const double b1 =  1.0 - cw0;
        const double b2 = (1.0 - cw0) / 2.0;
        const double a0 =  1.0 + alpha;
        const double a1 = -2.0 * cw0;
        const double a2 =  1.0 - alpha;

        return normalise(b0, b1, b2, a0, a1, a2);
    }

    /// Q values that make a cascade of two biquads a 4th-order Butterworth.
    static constexpr float kButterworthQ1 = 0.54119610f;
    static constexpr float kButterworthQ2 = 1.30656296f;

private:
    static constexpr double tau() { return 6.283185307179586476925; }

    /// Keeps the design frequency inside the range where the bilinear
    /// transform is well conditioned; without this a band centred above
    /// Nyquist produces NaN coefficients.
    static double clampHz(float hz, float sampleRate) noexcept
    {
        const double lo = 5.0;
        const double hi = sampleRate * 0.49;
        return std::min(std::max(static_cast<double>(hz), lo), hi);
    }

    static Biquad normalise(double b0, double b1, double b2,
                            double a0, double a1, double a2) noexcept
    {
        Biquad f;
        if (std::abs(a0) < 1e-20) {
            f.setIdentity();
            return f;
        }
        const double inv = 1.0 / a0;
        f.b0 = static_cast<float>(b0 * inv);
        f.b1 = static_cast<float>(b1 * inv);
        f.b2 = static_cast<float>(b2 * inv);
        f.a1 = static_cast<float>(a1 * inv);
        f.a2 = static_cast<float>(a2 * inv);
        return f;
    }
};

} // namespace rv::dsp
