#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/Types.h"

namespace rv::dsp {

/// Non-interleaved (planar) float buffer.
///
/// Planar is the native layout for VST3 - `IAudioProcessor::process` takes
/// `float**` - so keeping the whole internal chain planar means the only
/// interleave/deinterleave happens once at each device boundary rather than
/// around every plugin.
///
/// Channels live in one contiguous allocation with a separate pointer table,
/// so the buffer can be handed straight to a plugin.
class PlanarBuffer {
public:
    void resize(int channels, int frames)
    {
        channels_ = std::clamp(channels, 1, kMaxChannels);
        frames_   = std::max(0, frames);
        stride_   = static_cast<size_t>(frames_);

        storage_.assign(static_cast<size_t>(channels_) * stride_, 0.0f);
        pointers_.resize(static_cast<size_t>(channels_));
        for (int c = 0; c < channels_; ++c)
            pointers_[static_cast<size_t>(c)] = storage_.data() + static_cast<size_t>(c) * stride_;
    }

    /// Presents fewer frames than were allocated, without reallocating. Used
    /// for the final short block of a stream.
    void setActiveFrames(int frames) noexcept
    {
        frames_ = std::clamp(frames, 0, static_cast<int>(stride_));
    }

    /// Presents fewer channels than were allocated - e.g. driving a stereo
    /// chain from a mono microphone before the up-mix.
    void setActiveChannels(int channels) noexcept
    {
        channels_ = std::clamp(channels, 1, static_cast<int>(pointers_.size()));
    }

    int    channels() const noexcept { return channels_; }
    int    frames()   const noexcept { return frames_; }
    size_t capacityFrames() const noexcept { return stride_; }

    float*       channel(int c)       noexcept { return pointers_[static_cast<size_t>(c)]; }
    const float* channel(int c) const noexcept { return pointers_[static_cast<size_t>(c)]; }

    float**       data()       noexcept { return pointers_.data(); }
    float* const* data() const noexcept { return pointers_.data(); }

    void clear() noexcept
    {
        std::memset(storage_.data(), 0, storage_.size() * sizeof(float));
    }

    void clearActive() noexcept
    {
        for (int c = 0; c < channels_; ++c)
            std::memset(pointers_[static_cast<size_t>(c)], 0,
                        static_cast<size_t>(frames_) * sizeof(float));
    }

    void copyFrom(const PlanarBuffer& other) noexcept
    {
        const int ch = std::min(channels_, other.channels_);
        const int n  = std::min(frames_, other.frames_);
        for (int c = 0; c < ch; ++c)
            std::memcpy(pointers_[static_cast<size_t>(c)],
                        other.pointers_[static_cast<size_t>(c)],
                        static_cast<size_t>(n) * sizeof(float));
    }

    /// Interleaved -> planar, taking `srcChannels` from the source. Extra
    /// source channels are discarded; missing ones are filled by duplicating
    /// channel 0, which is what turns a mono microphone into a stereo chain.
    void readInterleaved(const float* src, int srcChannels, int frames) noexcept
    {
        const int n = std::min(frames, frames_);
        for (int c = 0; c < channels_; ++c) {
            float* dst = pointers_[static_cast<size_t>(c)];
            if (c < srcChannels) {
                const float* s = src + c;
                for (int i = 0; i < n; ++i)
                    dst[i] = s[static_cast<size_t>(i) * srcChannels];
            } else {
                std::memcpy(dst, pointers_[0], static_cast<size_t>(n) * sizeof(float));
            }
        }
    }

    /// Planar -> interleaved. Channels beyond what the buffer holds are filled
    /// from the last available channel so a stereo chain can feed a 4-channel
    /// device without silent gaps.
    void writeInterleaved(float* dst, int dstChannels, int frames) const noexcept
    {
        const int n = std::min(frames, frames_);
        for (int c = 0; c < dstChannels; ++c) {
            const int   src = std::min(c, channels_ - 1);
            const float* s  = pointers_[static_cast<size_t>(src)];
            float*       d  = dst + c;
            for (int i = 0; i < n; ++i)
                d[static_cast<size_t>(i) * dstChannels] = s[i];
        }
    }

private:
    std::vector<float>  storage_;
    std::vector<float*> pointers_;
    size_t stride_   = 0;
    int    channels_ = 0;
    int    frames_   = 0;
};

} // namespace rv::dsp
