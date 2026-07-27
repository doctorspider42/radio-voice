#pragma once

#include <vector>

#include "core/Types.h"

namespace rv::dsp {

/// In-place iterative radix-2 complex FFT.
///
/// Deliberately small and dependency-free: the only consumer is the spectrum
/// display, which runs on the UI thread at frame rate over a 2048-point window.
/// Twiddle factors and the bit-reversal permutation are precomputed once, so
/// the transform itself is a flat triple loop with no trigonometry.
class Fft {
public:
    /// `size` must be a power of two.
    void prepare(int size);

    int size() const noexcept { return size_; }

    /// Transforms `real`/`imag` in place; both must hold `size()` elements.
    void forward(float* real, float* imag) const;

    /// Convenience: magnitude spectrum of a real signal.
    /// `input` holds `size()` samples, `magnitude` receives `size()/2 + 1` bins.
    void magnitude(const float* input, float* magnitudeOut);

    /// Periodic Hann window of `size()` points, normalised so that a full-scale
    /// sine reads back at full scale.
    const std::vector<float>& window() const noexcept { return window_; }

private:
    int size_  = 0;
    int order_ = 0;

    std::vector<u32>   bitReverse_;
    std::vector<float> cosTable_;
    std::vector<float> sinTable_;
    std::vector<float> window_;

    // Scratch for magnitude(), so the UI path does not allocate per frame.
    mutable std::vector<float> scratchReal_;
    mutable std::vector<float> scratchImag_;
};

} // namespace rv::dsp
