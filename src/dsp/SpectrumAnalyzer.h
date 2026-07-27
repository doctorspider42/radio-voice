#pragma once

#include <vector>

#include "core/RingBuffer.h"
#include "dsp/AudioBuffer.h"
#include "dsp/Fft.h"

namespace rv::dsp {

/// Feeds the spectrum display behind the EQ curve.
///
/// The audio thread only ever mono-sums a block and pushes it into a ring - no
/// transform, no allocation, and a full ring is simply dropped because a missed
/// display frame is not worth a single missed audio deadline. All the actual
/// work happens on the UI thread in `update()`.
class SpectrumAnalyzer {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kHopSize = 512;

    void prepare(double sampleRate);

    /// Audio thread. Never blocks; drops data when the UI has fallen behind.
    void push(const PlanarBuffer& buffer);

    /// UI thread. Consumes whatever is queued and recomputes the spectrum if a
    /// whole hop arrived. Returns true when the displayed data changed.
    bool update();

    int   binCount() const noexcept { return static_cast<int>(displayDb_.size()); }
    float binDb(int index) const noexcept { return displayDb_[static_cast<size_t>(index)]; }
    float binFrequency(int index) const noexcept
    {
        return static_cast<float>(index) * static_cast<float>(sampleRate_) / kFftSize;
    }

    const std::vector<float>& binsDb() const noexcept { return displayDb_; }

    void clear();

private:
    double sampleRate_ = 48000.0;

    AudioRing feed_;
    Fft       fft_;

    std::vector<float> history_;   ///< Sliding kFftSize window.
    std::vector<float> hopBuffer_;
    std::vector<float> magnitudes_;
    std::vector<float> displayDb_; ///< Temporally smoothed, in dBFS.

    std::vector<float> monoScratch_; ///< Audio-thread mono sum, preallocated.
};

} // namespace rv::dsp
