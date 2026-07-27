#pragma once

#include <algorithm>
#include <cstring>

#include "core/Types.h"

namespace rv::audio {

/// Wire formats a Windows endpoint can present. Shared mode is effectively
/// always Float32, but exclusive mode routinely hands back packed 24-bit or
/// 32-bit integer, and a device that only offers Int16 is not unusual.
enum class SampleFormat {
    Unknown,
    Int16,
    Int24,   ///< Three bytes per sample, packed.
    Int32,
    Int32In24, ///< 24 bits of data left-aligned in a 32-bit container.
    Float32,
};

inline int bytesPerSample(SampleFormat f)
{
    switch (f) {
        case SampleFormat::Int16:     return 2;
        case SampleFormat::Int24:     return 3;
        case SampleFormat::Int32:     return 4;
        case SampleFormat::Int32In24: return 4;
        case SampleFormat::Float32:   return 4;
        case SampleFormat::Unknown:   return 0;
    }
    return 0;
}

inline const char* toString(SampleFormat f)
{
    switch (f) {
        case SampleFormat::Int16:     return "16-bit int";
        case SampleFormat::Int24:     return "24-bit int";
        case SampleFormat::Int32:     return "32-bit int";
        case SampleFormat::Int32In24: return "24-in-32-bit int";
        case SampleFormat::Float32:   return "32-bit float";
        case SampleFormat::Unknown:   return "unknown";
    }
    return "?";
}

/// Scale factors are 1/2^(n-1) so that the most negative integer maps just
/// outside -1.0; this is the standard asymmetric-integer convention and keeps
/// full-scale sine waves from reading hot.
inline void convertToFloat(const void* src, SampleFormat format, float* dst, size_t count)
{
    switch (format) {
        case SampleFormat::Float32:
            std::memcpy(dst, src, count * sizeof(float));
            break;

        case SampleFormat::Int16: {
            const auto* s = static_cast<const i16*>(src);
            constexpr float scale = 1.0f / 32768.0f;
            for (size_t i = 0; i < count; ++i)
                dst[i] = static_cast<float>(s[i]) * scale;
            break;
        }

        case SampleFormat::Int24: {
            const auto* s = static_cast<const u8*>(src);
            constexpr float scale = 1.0f / 8388608.0f;
            for (size_t i = 0; i < count; ++i) {
                // Little-endian, sign extended from bit 23.
                const u32 raw = static_cast<u32>(s[0]) |
                                (static_cast<u32>(s[1]) << 8) |
                                (static_cast<u32>(s[2]) << 16);
                const i32 v = static_cast<i32>(raw << 8) >> 8;
                dst[i] = static_cast<float>(v) * scale;
                s += 3;
            }
            break;
        }

        case SampleFormat::Int32In24: {
            const auto* s = static_cast<const i32*>(src);
            constexpr float scale = 1.0f / 8388608.0f;
            for (size_t i = 0; i < count; ++i)
                dst[i] = static_cast<float>(s[i] >> 8) * scale;
            break;
        }

        case SampleFormat::Int32: {
            const auto* s = static_cast<const i32*>(src);
            constexpr float scale = 1.0f / 2147483648.0f;
            for (size_t i = 0; i < count; ++i)
                dst[i] = static_cast<float>(s[i]) * scale;
            break;
        }

        case SampleFormat::Unknown:
            std::memset(dst, 0, count * sizeof(float));
            break;
    }
}

inline void convertFromFloat(const float* src, void* dst, SampleFormat format, size_t count)
{
    switch (format) {
        case SampleFormat::Float32:
            std::memcpy(dst, src, count * sizeof(float));
            break;

        case SampleFormat::Int16: {
            auto* d = static_cast<i16*>(dst);
            for (size_t i = 0; i < count; ++i) {
                const float v = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<i16>(v * 32767.0f);
            }
            break;
        }

        case SampleFormat::Int24: {
            auto* d = static_cast<u8*>(dst);
            for (size_t i = 0; i < count; ++i) {
                const float v = std::clamp(src[i], -1.0f, 1.0f);
                const i32   x = static_cast<i32>(v * 8388607.0f);
                d[0] = static_cast<u8>(x & 0xFF);
                d[1] = static_cast<u8>((x >> 8) & 0xFF);
                d[2] = static_cast<u8>((x >> 16) & 0xFF);
                d += 3;
            }
            break;
        }

        case SampleFormat::Int32In24: {
            auto* d = static_cast<i32*>(dst);
            for (size_t i = 0; i < count; ++i) {
                const float v = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<i32>(v * 8388607.0f) << 8;
            }
            break;
        }

        case SampleFormat::Int32: {
            auto* d = static_cast<i32*>(dst);
            for (size_t i = 0; i < count; ++i) {
                const float v = std::clamp(src[i], -1.0f, 1.0f);
                // 2147483647.0f rounds up to 2^31 in float, so scale by 2^31
                // and clamp the one value that overflows.
                const double x = static_cast<double>(v) * 2147483648.0;
                d[i] = static_cast<i32>(std::clamp(x, -2147483648.0, 2147483647.0));
            }
            break;
        }

        case SampleFormat::Unknown:
            break;
    }
}

} // namespace rv::audio
