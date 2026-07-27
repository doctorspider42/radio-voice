#pragma once

#include <pmmintrin.h>
#include <xmmintrin.h>

namespace rv {

/// Flush-to-zero / denormals-are-zero for the lifetime of the scope.
///
/// Denormal floats cost hundreds of cycles per operation on x86 and are
/// routinely produced by IIR filter tails and reverbs decaying to silence -
/// a single stuck denormal can push an audio callback past its deadline.
/// Instantiate once at the top of every audio thread body.
///
/// The previous MXCSR state is restored on destruction because hosted VST3
/// plugins share the thread and some are sensitive to it being changed
/// underneath them.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept
        : flushZero_(_MM_GET_FLUSH_ZERO_MODE())
        , denormalsZero_(_MM_GET_DENORMALS_ZERO_MODE())
    {
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    }

    ~ScopedNoDenormals() noexcept
    {
        _MM_SET_FLUSH_ZERO_MODE(flushZero_);
        _MM_SET_DENORMALS_ZERO_MODE(denormalsZero_);
    }

    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    unsigned int flushZero_;
    unsigned int denormalsZero_;
};

/// Replaces non-finite values with silence.
///
/// A misbehaving plugin emitting NaN would otherwise poison every filter state
/// downstream and produce a permanently silent or screaming output that only a
/// restart clears. Cheap enough to run unconditionally.
inline void sanitize(float* buffer, int count) noexcept
{
    for (int i = 0; i < count; ++i) {
        const float v = buffer[i];
        // Comparison against self is false only for NaN; the magnitude test
        // catches infinities and absurd values without a branchy isfinite.
        if (!(v > -1.0e6f && v < 1.0e6f))
            buffer[i] = 0.0f;
    }
}

} // namespace rv
