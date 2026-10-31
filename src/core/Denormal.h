#pragma once

#if defined(__aarch64__)
#include <cstdint>
#else
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

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
#if defined(__aarch64__)
    ScopedNoDenormals() noexcept
    {
        asm volatile("mrs %0, fpcr" : "=r"(previous_));
        const std::uint64_t next = previous_ | (1ULL << 24);
        asm volatile("msr fpcr, %0" : : "r"(next));
    }
    ~ScopedNoDenormals() noexcept { asm volatile("msr fpcr, %0" : : "r"(previous_)); }
#else
    ScopedNoDenormals() noexcept
        : previous_(_mm_getcsr()) { _mm_setcsr(previous_ | 0x8040); }
    ~ScopedNoDenormals() noexcept { _mm_setcsr(previous_); }
#endif
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;
private:
#if defined(__aarch64__)
    std::uint64_t previous_;
#else
    unsigned int previous_;
#endif
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
