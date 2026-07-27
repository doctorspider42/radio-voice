#pragma once

#include <algorithm>
#include <cmath>

#include "dsp/AudioBuffer.h"
#include "dsp/Smoothing.h"

namespace rv::dsp {

/// Peak and RMS metering with broadcast-style ballistics.
///
/// The raw block peak is useless on screen - it flickers far faster than the
/// eye integrates. Peak therefore rises instantly and falls at a fixed dB per
/// second, while RMS is a 300 ms exponential average of the squared signal,
/// which is close to how loudness is perceived for speech.
class LevelMeter {
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = std::max(1.0, sampleRate);

        // 20 dB/s decay, evaluated once per block rather than per sample.
        rmsCoeffPerSample_ = std::exp(-1.0 / (0.300 * sampleRate_));

        peak_ = 0.0f;
        meanSquare_ = 0.0f;
    }

    void reset()
    {
        peak_ = 0.0f;
        meanSquare_ = 0.0f;
    }

    void process(const PlanarBuffer& buffer)
    {
        const int frames   = buffer.frames();
        const int channels = buffer.channels();
        if (frames <= 0 || channels <= 0)
            return;

        float blockPeak = 0.0f;
        double sumSquares = 0.0;

        for (int c = 0; c < channels; ++c) {
            const float* s = buffer.channel(c);
            for (int i = 0; i < frames; ++i) {
                const float v = std::abs(s[i]);
                blockPeak = std::max(blockPeak, v);
                sumSquares += static_cast<double>(s[i]) * s[i];
            }
        }

        const double blockMeanSquare =
            sumSquares / (static_cast<double>(frames) * channels);

        // Applying the per-sample coefficient `frames` times in one step.
        const double decay = std::pow(rmsCoeffPerSample_, frames);
        meanSquare_ = blockMeanSquare + (meanSquare_ - blockMeanSquare) * decay;

        if (blockPeak >= peak_) {
            peak_ = blockPeak;
        } else {
            const double seconds = static_cast<double>(frames) / sampleRate_;
            peak_ = std::max(blockPeak,
                             peak_ * static_cast<float>(std::pow(10.0, -20.0 * seconds / 20.0)));
        }
    }

    float peak() const noexcept { return peak_; }
    float rms()  const noexcept { return static_cast<float>(std::sqrt(std::max(0.0, meanSquare_))); }

    float peakDb() const noexcept { return gainToDb(peak_); }
    float rmsDb()  const noexcept { return gainToDb(rms()); }

private:
    double sampleRate_        = 48000.0;
    double rmsCoeffPerSample_ = 0.0;
    double meanSquare_        = 0.0;
    float  peak_              = 0.0f;
};

} // namespace rv::dsp
