#pragma once

#include <string>

#include "core/RingBuffer.h"
#include "core/Types.h"

namespace rv::audio {

struct StreamConfig {
    DeviceId    deviceId;
    BackendType backend    = BackendType::Wasapi;
    WasapiMode  wasapiMode = WasapiMode::Shared;

    double sampleRate = 48000.0;
    int    channels   = 2;

    /// Requested device period in frames. 0 lets the driver pick, which is the
    /// right default for shared mode.
    int bufferFrames = 0;
};

/// Implemented by the engine. Called on the output device thread once per
/// device period, and must behave like an audio callback: no allocation, no
/// locks, no blocking.
class IAudioProducer {
public:
    virtual ~IAudioProducer() = default;

    /// Fills `interleaved` with exactly `frames` frames of `channels` channels.
    virtual void produce(float* interleaved, int channels, int frames) = 0;
};

/// Common surface for every backend, so the engine can treat ASIO, WASAPI and
/// DirectSound identically.
class IStream {
public:
    virtual ~IStream() = default;

    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    /// Format actually negotiated with the driver, which may differ from what
    /// was requested.
    virtual double actualSampleRate() const = 0;
    virtual int    actualChannels() const = 0;
    virtual int    bufferFrames() const = 0;

    /// Driver-reported latency in frames, when the backend can supply it.
    virtual int latencyFrames() const { return 0; }

    virtual u32 xruns() const = 0;

    /// Empty while healthy; set when the stream failed to open or died.
    virtual const std::string& error() const = 0;

    /// True when Windows invalidated an already-running endpoint. The caller
    /// must create a new stream; an IAudioClient cannot be revived in place.
    virtual bool deviceInvalidated() const { return false; }
};

/// Capture side.
///
/// Opening is deliberately split from running. The format a driver actually
/// grants can differ from what was asked for - WASAPI shared mode falls back to
/// the mixer's channel count and rate when it rejects a request - and the
/// engine has to size its ring buffer and allocate its scratch from the *real*
/// format. Negotiating first and starting the device thread second is what
/// makes that possible without a restart dance.
///
/// `IStream` is a virtual base so a single ASIO object can implement both
/// directions: an ASIO driver is duplex and exclusive, so capture and render on
/// the same device must share one instance rather than fight over the driver.
class IInputStream : public virtual IStream {
public:
    /// Reserves the device and negotiates the format. No audio flows yet, but
    /// `actualSampleRate`, `actualChannels` and `bufferFrames` are valid after
    /// this returns true.
    ///
    /// Named per direction rather than plain `open` because `AsioStream`
    /// implements both interfaces, and two identical signatures in sibling
    /// bases cannot be overridden separately.
    virtual bool openInput(const StreamConfig& config) = 0;

    /// Starts the device thread. `sink` must already be sized for
    /// `actualChannels`.
    virtual bool runInput(AudioRing& sink) = 0;
};

/// Render side. The stream pulls from `producer` on its own device thread.
class IOutputStream : public virtual IStream {
public:
    virtual bool openOutput(const StreamConfig& config) = 0;
    virtual bool runOutput(IAudioProducer& producer) = 0;
};

} // namespace rv::audio
