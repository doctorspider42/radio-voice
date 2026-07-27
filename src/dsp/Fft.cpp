#include "dsp/Fft.h"

#include <cmath>
#include <utility>

namespace rv::dsp {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Fft::prepare(int size)
{
    size_  = size;
    order_ = 0;
    while ((1 << order_) < size_)
        ++order_;

    bitReverse_.resize(static_cast<size_t>(size_));
    for (int i = 0; i < size_; ++i) {
        u32 r = 0;
        for (int b = 0; b < order_; ++b)
            r |= ((static_cast<u32>(i) >> b) & 1u) << (order_ - 1 - b);
        bitReverse_[static_cast<size_t>(i)] = r;
    }

    // Twiddles for the largest stage; smaller stages index this table with a
    // stride, which avoids storing one table per stage.
    cosTable_.resize(static_cast<size_t>(size_ / 2));
    sinTable_.resize(static_cast<size_t>(size_ / 2));
    for (int i = 0; i < size_ / 2; ++i) {
        const double a = -2.0 * kPi * i / size_;
        cosTable_[static_cast<size_t>(i)] = static_cast<float>(std::cos(a));
        sinTable_[static_cast<size_t>(i)] = static_cast<float>(std::sin(a));
    }

    window_.resize(static_cast<size_t>(size_));
    double sum = 0.0;
    for (int i = 0; i < size_; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / size_));
        window_[static_cast<size_t>(i)] = static_cast<float>(w);
        sum += w;
    }
    // Coherent gain compensation: a Hann window halves the amplitude of a
    // sinusoid, and without this the display would read 6 dB low.
    const float norm = static_cast<float>(size_ / sum);
    for (auto& w : window_)
        w *= norm;

    scratchReal_.resize(static_cast<size_t>(size_));
    scratchImag_.resize(static_cast<size_t>(size_));
}

void Fft::forward(float* real, float* imag) const
{
    // Permute into bit-reversed order so the butterflies can run in place.
    for (int i = 0; i < size_; ++i) {
        const u32 j = bitReverse_[static_cast<size_t>(i)];
        if (static_cast<u32>(i) < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (int stageSize = 2; stageSize <= size_; stageSize <<= 1) {
        const int half   = stageSize >> 1;
        const int stride = size_ / stageSize;

        for (int base = 0; base < size_; base += stageSize) {
            for (int k = 0; k < half; ++k) {
                const size_t t  = static_cast<size_t>(k * stride);
                const float  wr = cosTable_[t];
                const float  wi = sinTable_[t];

                const int i0 = base + k;
                const int i1 = i0 + half;

                const float xr = real[i1] * wr - imag[i1] * wi;
                const float xi = real[i1] * wi + imag[i1] * wr;

                real[i1] = real[i0] - xr;
                imag[i1] = imag[i0] - xi;
                real[i0] += xr;
                imag[i0] += xi;
            }
        }
    }
}

void Fft::magnitude(const float* input, float* magnitudeOut)
{
    for (int i = 0; i < size_; ++i) {
        scratchReal_[static_cast<size_t>(i)] = input[i] * window_[static_cast<size_t>(i)];
        scratchImag_[static_cast<size_t>(i)] = 0.0f;
    }

    forward(scratchReal_.data(), scratchImag_.data());

    const int bins  = size_ / 2 + 1;
    const float inv = 2.0f / static_cast<float>(size_);

    for (int i = 0; i < bins; ++i) {
        const float re = scratchReal_[static_cast<size_t>(i)];
        const float im = scratchImag_[static_cast<size_t>(i)];
        // DC and Nyquist are not mirrored, so they must not get the factor of 2.
        const float scale = (i == 0 || i == size_ / 2) ? inv * 0.5f : inv;
        magnitudeOut[i] = std::sqrt(re * re + im * im) * scale;
    }
}

} // namespace rv::dsp
