#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rv {

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;

/// Everything downstream of the input backend is float32.
using Sample = float;

/// Hard ceiling on channel count anywhere in the signal path. The processor is
/// mono-or-stereo by design; the extra headroom exists so that multi-channel
/// interfaces can be opened and down-mixed without a separate code path.
inline constexpr int kMaxChannels = 8;

/// Bounds for the fixed-size block the DSP chain and plugins are driven with.
inline constexpr int kMinBlockSize = 32;
inline constexpr int kMaxBlockSize = 2048;

inline constexpr int kEqBands = 10;

/// ISO 266 preferred centre frequencies, one octave apart.
inline constexpr float kEqCenters[kEqBands] = {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

enum class BackendType : int {
    Wasapi = 0,     ///< Shared or exclusive mode, event driven.
    DirectSound,    ///< Legacy polled backend, widest device compatibility.
    Asio,           ///< Lowest latency; requires an ASIO driver.
};

enum class WasapiMode : int {
    Shared = 0,     ///< Coexists with other applications, resampled by the mixer.
    Exclusive,      ///< Bypasses the mixer: lower latency, device locked to us.
};

const char* toString(BackendType b);
const char* toString(WasapiMode m);

/// Stable identifier for a device, used to reconnect across restarts.
/// For WASAPI this is the endpoint id string, for ASIO the driver name,
/// for DirectSound the GUID rendered as text.
using DeviceId = std::string;

} // namespace rv
