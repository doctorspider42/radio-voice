#include "audio/WasapiStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Denormal.h"
#include "core/Log.h"
#include "core/Strings.h"

namespace rv::audio {
namespace {

/// REFERENCE_TIME is in 100 ns units.
constexpr double kRefTimesPerSecond = 10'000'000.0;

REFERENCE_TIME framesToReferenceTime(int frames, double sampleRate)
{
    return static_cast<REFERENCE_TIME>(kRefTimesPerSecond * frames / sampleRate + 0.5);
}

/// Formats worth trying in exclusive mode, best first. Float is preferred
/// because it needs no conversion at all; the integer formats follow in
/// descending resolution.
constexpr SampleFormat kExclusiveCandidates[] = {
    SampleFormat::Float32,
    SampleFormat::Int32,
    SampleFormat::Int32In24,
    SampleFormat::Int24,
    SampleFormat::Int16,
};

} // namespace

// ---------------------------------------------------------------------------
// WasapiStreamBase
// ---------------------------------------------------------------------------

WasapiStreamBase::~WasapiStreamBase()
{
    stop();
    closeClient();
}

void WasapiStreamBase::fail(const char* what, HRESULT hr)
{
    error_ = std::string(what) + ": " + wasapi::describeHresult(hr);
    RV_ERROR("WASAPI %s", error_.c_str());
}

void WasapiStreamBase::closeClient()
{
    client_.reset();
    device_.reset();

    if (bufferEvent_) {
        ::CloseHandle(bufferEvent_);
        bufferEvent_ = nullptr;
    }
    if (stopEvent_) {
        ::CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void WasapiStreamBase::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable())
            thread_.join();
        return;
    }

    // Waking the device thread through a second event rather than relying on
    // the buffer event's timeout keeps shutdown immediate even when the driver
    // has gone quiet.
    if (stopEvent_)
        ::SetEvent(stopEvent_);

    if (thread_.joinable())
        thread_.join();
}

void WasapiStreamBase::launchThread()
{
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
        // Each device thread needs its own COM apartment; MTA because the
        // audio client is free-threaded and we must not pump messages here.
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comOwned = SUCCEEDED(hr);

        HANDLE mmcss = wasapi::enterProAudio();
        ScopedNoDenormals noDenormals;

        threadBody();

        wasapi::revertProAudio(mmcss);
        if (comOwned)
            ::CoUninitialize();
    });
}

bool WasapiStreamBase::openClient(const StreamConfig& config, bool capture)
{
    error_.clear();
    mode_ = config.wasapiMode;

    auto enumerator = wasapi::createEnumerator();
    if (!enumerator) {
        error_ = "could not create the WASAPI device enumerator";
        return false;
    }

    HRESULT hr;
    if (config.deviceId.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender,
                                                 eConsole, device_.put());
    } else {
        hr = enumerator->GetDevice(toWide(config.deviceId).c_str(), device_.put());
    }
    if (FAILED(hr)) {
        fail("could not open the endpoint", hr);
        return false;
    }

    hr = device_->Activate(wasapi::kIidIAudioClient, CLSCTX_ALL, nullptr, client_.putVoid());
    if (FAILED(hr)) {
        fail("could not activate the audio client", hr);
        return false;
    }

    WAVEFORMATEXTENSIBLE negotiated{};
    hr = (mode_ == WasapiMode::Exclusive) ? initialiseExclusive(config, negotiated)
                                          : initialiseShared(config, negotiated);
    if (FAILED(hr))
        return false;

    sampleFormat_ = wasapi::formatOf(&negotiated.Format);
    if (sampleFormat_ == SampleFormat::Unknown) {
        error_ = "the device negotiated a sample format this build cannot convert";
        return false;
    }

    channels_   = negotiated.Format.nChannels;
    sampleRate_ = negotiated.Format.nSamplesPerSec;
    frameBytes_ = negotiated.Format.nBlockAlign;

    UINT32 bufferFrames = 0;
    if (FAILED(hr = client_->GetBufferSize(&bufferFrames))) {
        fail("could not query the buffer size", hr);
        return false;
    }
    bufferFrames_ = static_cast<int>(bufferFrames);

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(client_->GetStreamLatency(&latency)))
        latencyFrames_ = static_cast<int>(latency / kRefTimesPerSecond * sampleRate_);
    else
        latencyFrames_ = bufferFrames_;

    bufferEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_   = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!bufferEvent_ || !stopEvent_) {
        error_ = "could not create the stream synchronisation events";
        return false;
    }

    if (FAILED(hr = client_->SetEventHandle(bufferEvent_))) {
        fail("could not attach the buffer event", hr);
        return false;
    }

    RV_INFO("WASAPI %s opened: %s, %d ch @ %.0f Hz, %s, buffer %d frames (%.1f ms)",
            capture ? "capture" : "render", toString(mode_), channels_, sampleRate_,
            toString(sampleFormat_), bufferFrames_,
            1000.0 * bufferFrames_ / sampleRate_);

    return true;
}

HRESULT WasapiStreamBase::initialiseShared(const StreamConfig& config,
                                           WAVEFORMATEXTENSIBLE& negotiated)
{
    WAVEFORMATEX* mixFormat = nullptr;
    HRESULT hr = client_->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
        fail("could not query the mix format", hr);
        return hr;
    }

    const int  desiredRate     = static_cast<int>(config.sampleRate);
    const int  desiredChannels = std::clamp(config.channels, 1, kMaxChannels);
    const bool matchesMixer    = (static_cast<int>(mixFormat->nSamplesPerSec) == desiredRate &&
                                  mixFormat->nChannels == desiredChannels);

    // The audio engine will happily resample and remix for us if asked, which
    // is how a 48 kHz stereo chain runs on a 44.1 kHz mono endpoint without a
    // separate conversion path here. Requires Windows 7 or later.
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    WAVEFORMATEXTENSIBLE requested = wasapi::makeFormat(SampleFormat::Float32,
                                                        desiredChannels, desiredRate);

    if (!matchesMixer) {
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                 &requested.Format, nullptr);
        if (SUCCEEDED(hr)) {
            negotiated = requested;
            ::CoTaskMemFree(mixFormat);
            return S_OK;
        }

        // Older or unusual drivers reject the conversion flags. Fall back to
        // the mixer's own format and let the engine resample instead.
        RV_WARN("WASAPI shared mode rejected %d ch @ %d Hz (%s); using the mixer format",
                desiredChannels, desiredRate, wasapi::describeHresult(hr).c_str());

        // Initialize may not be retried on the same client instance.
        client_.reset();
        HRESULT reactivate = device_->Activate(wasapi::kIidIAudioClient, CLSCTX_ALL,
                                               nullptr, client_.putVoid());
        if (FAILED(reactivate)) {
            fail("could not re-activate the audio client", reactivate);
            ::CoTaskMemFree(mixFormat);
            return reactivate;
        }
    }

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             0, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        fail("could not initialise the shared-mode stream", hr);
        ::CoTaskMemFree(mixFormat);
        return hr;
    }

    // Copy out before freeing: mixFormat may be a plain WAVEFORMATEX, so only
    // the bytes it actually owns may be read.
    std::memset(&negotiated, 0, sizeof(negotiated));
    std::memcpy(&negotiated, mixFormat,
                std::min<size_t>(sizeof(WAVEFORMATEXTENSIBLE),
                                 sizeof(WAVEFORMATEX) + mixFormat->cbSize));
    ::CoTaskMemFree(mixFormat);
    return S_OK;
}

HRESULT WasapiStreamBase::initialiseExclusive(const StreamConfig& config,
                                              WAVEFORMATEXTENSIBLE& negotiated)
{
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    HRESULT hr = client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    if (FAILED(hr)) {
        fail("could not query the device period", hr);
        return hr;
    }

    const int desiredRate     = static_cast<int>(config.sampleRate);
    const int desiredChannels = std::clamp(config.channels, 1, kMaxChannels);

    // Exclusive mode does no conversion, so the format has to be one the
    // hardware genuinely accepts. Probe candidates rather than guessing.
    WAVEFORMATEXTENSIBLE chosen{};
    bool found = false;
    for (SampleFormat candidate : kExclusiveCandidates) {
        WAVEFORMATEXTENSIBLE probe = wasapi::makeFormat(candidate, desiredChannels, desiredRate);
        WAVEFORMATEX* closest = nullptr;
        hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &probe.Format, &closest);
        if (closest)
            ::CoTaskMemFree(closest);

        if (hr == S_OK) {
            chosen = probe;
            found  = true;
            break;
        }
    }

    if (!found) {
        error_ = "the device does not support " + std::to_string(desiredChannels) +
                 " channel(s) at " + std::to_string(desiredRate) +
                 " Hz in exclusive mode. Match the rate to the one set in "
                 "Windows sound settings for this device, or use shared mode.";
        RV_ERROR("WASAPI %s", error_.c_str());
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    REFERENCE_TIME period = (config.bufferFrames > 0)
                                ? framesToReferenceTime(config.bufferFrames, config.sampleRate)
                                : minimumPeriod;
    period = std::max(period, minimumPeriod);

    // In exclusive event-driven mode the buffer duration and the periodicity
    // must be identical, hence the same value twice.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             period, period, &chosen.Format, nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames = 0;
        if (SUCCEEDED(client_->GetBufferSize(&alignedFrames)) && alignedFrames > 0) {
            const REFERENCE_TIME aligned =
                framesToReferenceTime(static_cast<int>(alignedFrames), chosen.Format.nSamplesPerSec);

            // A client that failed Initialize cannot be reused.
            client_.reset();
            hr = device_->Activate(wasapi::kIidIAudioClient, CLSCTX_ALL, nullptr,
                                   client_.putVoid());
            if (SUCCEEDED(hr)) {
                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         aligned, aligned, &chosen.Format, nullptr);
            }
        }
    }

    if (FAILED(hr)) {
        fail("could not initialise the exclusive-mode stream", hr);
        return hr;
    }

    negotiated = chosen;
    return S_OK;
}

// ---------------------------------------------------------------------------
// WasapiInputStream
// ---------------------------------------------------------------------------

bool WasapiInputStream::openInput(const StreamConfig& config)
{
    stop();
    closeClient();
    return openClient(config, /*capture=*/true);
}

bool WasapiInputStream::runInput(AudioRing& sink)
{
    if (!client_) {
        error_ = "the capture stream was not opened";
        return false;
    }
    if (sink.channels() != channels_) {
        error_ = "the ring buffer does not match the negotiated channel count";
        return false;
    }

    sink_ = &sink;
    // Sized for a whole device buffer plus slack, so a late wake-up never
    // forces a partial conversion.
    scratch_.assign(static_cast<size_t>(bufferFrames_) * channels_ * 2, 0.0f);

    launchThread();
    return true;
}

void WasapiInputStream::threadBody()
{
    ComPtr<IAudioCaptureClient> capture;
    HRESULT hr = client_->GetService(wasapi::kIidIAudioCaptureClient, capture.putVoid());
    if (FAILED(hr)) {
        fail("could not obtain the capture service", hr);
        running_.store(false, std::memory_order_release);
        return;
    }

    if (FAILED(hr = client_->Start())) {
        fail("could not start the capture stream", hr);
        running_.store(false, std::memory_order_release);
        return;
    }

    HANDLE waitOn[2] = {bufferEvent_, stopEvent_};

    while (running_.load(std::memory_order_acquire)) {
        const DWORD wait = ::WaitForMultipleObjects(2, waitOn, FALSE, 2000);

        if (wait == WAIT_OBJECT_0 + 1)
            break;

        if (wait == WAIT_TIMEOUT) {
            // The driver stopped delivering. Treat it as an xrun and keep
            // waiting; the engine surfaces the counter in the UI.
            xruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (wait != WAIT_OBJECT_0)
            break;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE*  data  = nullptr;
            UINT32 frames = 0;
            DWORD  flags = 0;

            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY)
                break;
            if (FAILED(hr)) {
                fail("capture GetBuffer failed", hr);
                running_.store(false, std::memory_order_release);
                break;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                xruns_.fetch_add(1, std::memory_order_relaxed);

            const size_t samples = static_cast<size_t>(frames) * channels_;
            if (samples > scratch_.size())
                scratch_.resize(samples);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                std::fill_n(scratch_.begin(), samples, 0.0f);
            else
                convertToFloat(data, sampleFormat_, scratch_.data(), samples);

            const int written = sink_->write(scratch_.data(), static_cast<int>(frames));
            if (written < static_cast<int>(frames)) {
                // The processing side is not keeping up. Dropping the excess
                // is the only option that keeps latency bounded.
                xruns_.fetch_add(1, std::memory_order_relaxed);
            }

            capture->ReleaseBuffer(frames);
        }
    }

    client_->Stop();
    client_->Reset();
    running_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// WasapiOutputStream
// ---------------------------------------------------------------------------

bool WasapiOutputStream::openOutput(const StreamConfig& config)
{
    stop();
    closeClient();
    return openClient(config, /*capture=*/false);
}

bool WasapiOutputStream::runOutput(IAudioProducer& producer)
{
    if (!client_) {
        error_ = "the render stream was not opened";
        return false;
    }

    producer_ = &producer;
    scratch_.assign(static_cast<size_t>(bufferFrames_) * channels_, 0.0f);

    launchThread();
    return true;
}

void WasapiOutputStream::threadBody()
{
    ComPtr<IAudioRenderClient> render;
    HRESULT hr = client_->GetService(wasapi::kIidIAudioRenderClient, render.putVoid());
    if (FAILED(hr)) {
        fail("could not obtain the render service", hr);
        running_.store(false, std::memory_order_release);
        return;
    }

    // Prime the whole buffer with silence before starting. Without this the
    // first period plays whatever the driver's buffer happened to contain.
    BYTE* data = nullptr;
    if (SUCCEEDED(render->GetBuffer(static_cast<UINT32>(bufferFrames_), &data)))
        render->ReleaseBuffer(static_cast<UINT32>(bufferFrames_), AUDCLNT_BUFFERFLAGS_SILENT);

    if (FAILED(hr = client_->Start())) {
        fail("could not start the render stream", hr);
        running_.store(false, std::memory_order_release);
        return;
    }

    HANDLE waitOn[2] = {bufferEvent_, stopEvent_};

    while (running_.load(std::memory_order_acquire)) {
        const DWORD wait = ::WaitForMultipleObjects(2, waitOn, FALSE, 2000);

        if (wait == WAIT_OBJECT_0 + 1)
            break;

        if (wait == WAIT_TIMEOUT) {
            xruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (wait != WAIT_OBJECT_0)
            break;

        UINT32 padding = 0;
        if (mode_ == WasapiMode::Shared) {
            if (FAILED(client_->GetCurrentPadding(&padding)))
                padding = 0;
        }

        const UINT32 frames = static_cast<UINT32>(bufferFrames_) - padding;
        if (frames == 0)
            continue;

        hr = render->GetBuffer(frames, &data);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                fail("render device invalidated", hr);
                break;
            }
            xruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const size_t samples = static_cast<size_t>(frames) * channels_;
        if (samples > scratch_.size())
            scratch_.resize(samples);

        producer_->produce(scratch_.data(), channels_, static_cast<int>(frames));
        convertFromFloat(scratch_.data(), data, sampleFormat_, samples);

        render->ReleaseBuffer(frames, 0);
    }

    client_->Stop();
    client_->Reset();
    running_.store(false, std::memory_order_release);
}

} // namespace rv::audio
