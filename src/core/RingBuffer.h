#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "core/Types.h"

namespace rv {

/// Single-producer / single-consumer lock-free ring of interleaved float frames.
///
/// One device thread writes, one processing thread reads (or vice versa), so no
/// mutex is needed: the producer only ever advances `write_` and the consumer
/// only ever advances `read_`, with release/acquire ordering publishing the
/// data written before the index update.
///
/// Capacity is rounded up to a power of two so wrapping is a mask rather than a
/// modulo. Indices are free-running and never wrapped themselves, which keeps
/// "full" distinguishable from "empty" without wasting a slot.
class AudioRing {
public:
    AudioRing() = default;

    /// Not callable while either side is running.
    void resize(int channels, int minFrames)
    {
        channels_ = std::max(1, channels);

        u32 cap = 1;
        while (cap < static_cast<u32>(std::max(2, minFrames)))
            cap <<= 1;

        capacity_ = cap;
        mask_     = cap - 1;
        data_.assign(static_cast<size_t>(cap) * channels_, 0.0f);
        clear();
    }

    void clear()
    {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        std::fill(data_.begin(), data_.end(), 0.0f);
    }

    int channels() const noexcept { return channels_; }
    int capacity() const noexcept { return static_cast<int>(capacity_); }

    /// Frames currently buffered. Safe to call from either side; the value is a
    /// lower bound for the consumer and an upper bound for the producer.
    int filled() const noexcept
    {
        const u32 w = write_.load(std::memory_order_acquire);
        const u32 r = read_.load(std::memory_order_acquire);
        return static_cast<int>(w - r);
    }

    int writable() const noexcept { return static_cast<int>(capacity_) - filled(); }

    /// Writes up to `frames`; returns how many were actually stored.
    int write(const float* interleaved, int frames) noexcept
    {
        const u32 w     = write_.load(std::memory_order_relaxed);
        const u32 r     = read_.load(std::memory_order_acquire);
        const int space = static_cast<int>(capacity_ - (w - r));
        const int n     = std::min(frames, space);
        if (n <= 0)
            return 0;

        const u32 start = w & mask_;
        const int first = std::min(n, static_cast<int>(capacity_ - start));

        std::memcpy(&data_[static_cast<size_t>(start) * channels_], interleaved,
                    static_cast<size_t>(first) * channels_ * sizeof(float));
        if (n > first) {
            std::memcpy(&data_[0], interleaved + static_cast<size_t>(first) * channels_,
                        static_cast<size_t>(n - first) * channels_ * sizeof(float));
        }

        write_.store(w + static_cast<u32>(n), std::memory_order_release);
        return n;
    }

    /// Writes `frames` of silence, used to paper over a starved producer.
    int writeSilence(int frames) noexcept
    {
        const u32 w     = write_.load(std::memory_order_relaxed);
        const u32 r     = read_.load(std::memory_order_acquire);
        const int space = static_cast<int>(capacity_ - (w - r));
        const int n     = std::min(frames, space);
        if (n <= 0)
            return 0;

        const u32 start = w & mask_;
        const int first = std::min(n, static_cast<int>(capacity_ - start));

        std::memset(&data_[static_cast<size_t>(start) * channels_], 0,
                    static_cast<size_t>(first) * channels_ * sizeof(float));
        if (n > first) {
            std::memset(&data_[0], 0,
                        static_cast<size_t>(n - first) * channels_ * sizeof(float));
        }

        write_.store(w + static_cast<u32>(n), std::memory_order_release);
        return n;
    }

    /// Reads up to `frames`; returns how many were actually produced.
    int read(float* interleaved, int frames) noexcept
    {
        const u32 r         = read_.load(std::memory_order_relaxed);
        const u32 w         = write_.load(std::memory_order_acquire);
        const int available = static_cast<int>(w - r);
        const int n         = std::min(frames, available);
        if (n <= 0)
            return 0;

        const u32 start = r & mask_;
        const int first = std::min(n, static_cast<int>(capacity_ - start));

        std::memcpy(interleaved, &data_[static_cast<size_t>(start) * channels_],
                    static_cast<size_t>(first) * channels_ * sizeof(float));
        if (n > first) {
            std::memcpy(interleaved + static_cast<size_t>(first) * channels_, &data_[0],
                        static_cast<size_t>(n - first) * channels_ * sizeof(float));
        }

        read_.store(r + static_cast<u32>(n), std::memory_order_release);
        return n;
    }

    /// Reads without consuming - used by the drift resampler, which needs one
    /// frame of look-ahead beyond what it will actually retire this block.
    int peek(float* interleaved, int frames, int offsetFrames = 0) const noexcept
    {
        const u32 r         = read_.load(std::memory_order_relaxed);
        const u32 w         = write_.load(std::memory_order_acquire);
        const int available = static_cast<int>(w - r) - offsetFrames;
        const int n         = std::min(frames, available);
        if (n <= 0)
            return 0;

        const u32 start = (r + static_cast<u32>(offsetFrames)) & mask_;
        const int first = std::min(n, static_cast<int>(capacity_ - start));

        std::memcpy(interleaved, &data_[static_cast<size_t>(start) * channels_],
                    static_cast<size_t>(first) * channels_ * sizeof(float));
        if (n > first) {
            std::memcpy(interleaved + static_cast<size_t>(first) * channels_, &data_[0],
                        static_cast<size_t>(n - first) * channels_ * sizeof(float));
        }
        return n;
    }

    /// Retires frames previously inspected with peek().
    void advanceRead(int frames) noexcept
    {
        const u32 r = read_.load(std::memory_order_relaxed);
        read_.store(r + static_cast<u32>(frames), std::memory_order_release);
    }

    /// Drops the oldest frames, keeping at most `keepFrames`. Called by the
    /// consumer when the producer has run far ahead (device restart, xrun);
    /// re-syncing is preferable to draining a large backlog at real time.
    void trimTo(int keepFrames) noexcept
    {
        const int excess = filled() - keepFrames;
        if (excess > 0)
            advanceRead(excess);
    }

private:
    // Producer and consumer indices sit on separate cache lines: sharing one
    // would make every write invalidate the reader's line and vice versa.
    alignas(64) std::atomic<u32> write_{0};
    alignas(64) std::atomic<u32> read_{0};

    std::vector<float> data_;
    u32 capacity_ = 0;
    u32 mask_     = 0;
    int channels_ = 1;
};

} // namespace rv
