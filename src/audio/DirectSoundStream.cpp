#include "audio/DirectSoundStream.h"

#include <algorithm>
#include <cstring>

#include "audio/WasapiCommon.h" // MMCSS helpers, shared by every backend
#include "core/Denormal.h"
#include "core/Log.h"
#include "core/Strings.h"

namespace rv::audio {
namespace {

/// DirectSound speaks 16-bit PCM everywhere. Float is accepted by some drivers
/// but silently mis-rendered by others, and this backend exists for
/// compatibility, so the safe choice is the right one.
constexpr SampleFormat kDsFormat = SampleFormat::Int16;

DWORD pollIntervalMs(int periodFrames, double sampleRate)
{
    const double periodMs = 1000.0 * periodFrames / std::max(1.0, sampleRate);
    return static_cast<DWORD>(std::clamp(periodMs * 0.25, 1.0, 20.0));
}

} // namespace

DirectSoundStreamBase::~DirectSoundStreamBase()
{
    stop();
    if (stopEvent_) {
        ::CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void DirectSoundStreamBase::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable())
            thread_.join();
        return;
    }

    if (stopEvent_)
        ::SetEvent(stopEvent_);
    if (thread_.joinable())
        thread_.join();
}

void DirectSoundStreamBase::launchThread()
{
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
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

HANDLE DirectSoundStreamBase::createPollTimer(DWORD intervalMs) const
{
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    if (!timer) {
        RV_WARN("high-resolution timer unavailable; DirectSound will poll at the "
                "system tick rate (about 15 ms) instead of every %lu ms",
                static_cast<unsigned long>(intervalMs));
        return nullptr;
    }

    // Negative is relative, in 100 ns units; the period repeats it.
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(intervalMs) * 10000LL;
    if (!::SetWaitableTimer(timer, &due, static_cast<LONG>(intervalMs), nullptr, nullptr, FALSE)) {
        ::CloseHandle(timer);
        return nullptr;
    }

    return timer;
}

bool DirectSoundStreamBase::waitForPoll(HANDLE timer, DWORD intervalMs) const
{
    if (!timer)
        return ::WaitForSingleObject(stopEvent_, intervalMs) != WAIT_OBJECT_0;

    HANDLE waitOn[2] = {stopEvent_, timer};
    return ::WaitForMultipleObjects(2, waitOn, FALSE, INFINITE) != WAIT_OBJECT_0;
}

bool DirectSoundStreamBase::parseDeviceGuid(const DeviceId& id, GUID& out) const
{
    // An empty id means "the default device", which DirectSound expresses as a
    // null GUID pointer; callers handle that case before getting here.
    if (id.empty())
        return false;

    const std::wstring wide = toWide(id);
    return SUCCEEDED(::CLSIDFromString(wide.c_str(), &out));
}

WAVEFORMATEX DirectSoundStreamBase::makeWaveFormat(int channels, int sampleRate) const
{
    WAVEFORMATEX wf{};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = static_cast<WORD>(channels);
    wf.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = static_cast<WORD>(channels * 2);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;
    return wf;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool DirectSoundInputStream::openInput(const StreamConfig& config)
{
    stop();
    error_.clear();

    sampleFormat_ = kDsFormat;
    channels_     = std::clamp(config.channels, 1, 2);
    sampleRate_   = config.sampleRate;
    periodFrames_ = config.bufferFrames > 0 ? config.bufferFrames
                                            : static_cast<int>(config.sampleRate * 0.010);
    ringFrames_   = periodFrames_ * kPeriodsInRing;
    frameBytes_   = channels_ * 2;

    GUID guid{};
    const bool haveGuid = parseDeviceGuid(config.deviceId, guid);

    HRESULT hr = ::DirectSoundCaptureCreate8(haveGuid ? &guid : nullptr,
                                             capture_.put(), nullptr);
    if (FAILED(hr)) {
        error_ = "DirectSoundCaptureCreate8 failed: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    WAVEFORMATEX wf = makeWaveFormat(channels_, static_cast<int>(sampleRate_));

    DSCBUFFERDESC desc{};
    desc.dwSize        = sizeof(desc);
    desc.dwBufferBytes = static_cast<DWORD>(ringFrames_ * frameBytes_);
    desc.lpwfxFormat   = &wf;

    ComPtr<IDirectSoundCaptureBuffer> base;
    hr = capture_->CreateCaptureBuffer(&desc, base.put(), nullptr);
    if (FAILED(hr)) {
        error_ = "could not create the DirectSound capture buffer: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    hr = base->QueryInterface(IID_IDirectSoundCaptureBuffer8, buffer_.putVoid());
    if (FAILED(hr)) {
        error_ = "IDirectSoundCaptureBuffer8 unavailable: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    RV_INFO("DirectSound capture opened: %d ch @ %.0f Hz, ring %d frames",
            channels_, sampleRate_, ringFrames_);
    return true;
}

bool DirectSoundInputStream::runInput(AudioRing& sink)
{
    if (!buffer_) {
        error_ = "the capture stream was not opened";
        return false;
    }
    if (sink.channels() != channels_) {
        error_ = "the ring buffer does not match the negotiated channel count";
        return false;
    }

    sink_ = &sink;
    scratch_.assign(static_cast<size_t>(ringFrames_) * channels_, 0.0f);
    readCursor_ = 0;

    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        error_ = "could not create the stop event";
        return false;
    }

    launchThread();
    return true;
}

void DirectSoundInputStream::threadBody()
{
    HRESULT hr = buffer_->Start(DSCBSTART_LOOPING);
    if (FAILED(hr)) {
        error_ = "could not start DirectSound capture: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    const DWORD ringBytes = static_cast<DWORD>(ringFrames_ * frameBytes_);
    const DWORD interval  = pollIntervalMs(periodFrames_, sampleRate_);

    std::vector<u8> raw(static_cast<size_t>(ringBytes));

    // A capture buffer that starts without complaining and then produces
    // nothing is indistinguishable, from the engine's side, from one that is
    // merely slow - both look like a ring that will not fill. These two count
    // enough to tell them apart in the log without any per-poll noise.
    u64  framesDelivered = 0;
    bool reportedFirst   = false;
    bool reportedSilent  = false;
    int  pollsSinceStart = 0;
    const int pollsForVerdict = static_cast<int>(2000 / std::max<DWORD>(1, interval));

    // How the polls divide up. A backend that under-delivers is either being
    // asked too rarely or is finding nothing when it asks, and the ratio of
    // these two says which - the fixes have nothing in common.
    u64   idlePolls    = 0;
    u64   lockFailures = 0;
    DWORD maxAvailable = 0;
    DWORD lastCapture  = 0;
    DWORD lastRead     = 0;

    HANDLE pollTimer = createPollTimer(interval);

    while (running_.load(std::memory_order_acquire)) {
        if (!waitForPoll(pollTimer, interval))
            break;

        DWORD capturePos = 0, readPos = 0;
        if (FAILED(buffer_->GetCurrentPosition(&capturePos, &readPos)))
            continue;

        // Reported once, either way, roughly two seconds in. Whether the
        // driver's own cursors moved is the whole question: cursors that
        // advanced while nothing reached the ring is a bug on this side, and
        // cursors frozen at zero is a device that accepted Start and then did
        // nothing - which no amount of code here can fix.
        ++pollsSinceStart;
        if (!reportedFirst && !reportedSilent && pollsSinceStart >= pollsForVerdict) {
            reportedSilent = true;
            RV_WARN("DirectSound capture has delivered nothing after 2 s "
                    "(capture cursor %lu, read cursor %lu, ours %lu, of %lu bytes). "
                    "Cursors still at zero mean the device accepted Start and is "
                    "not running; try the WASAPI backend for this device.",
                    static_cast<unsigned long>(capturePos),
                    static_cast<unsigned long>(readPos),
                    static_cast<unsigned long>(readCursor_),
                    static_cast<unsigned long>(ringBytes));
        }

        // Everything between our cursor and the driver's read cursor is valid,
        // fully-written data.
        lastCapture = capturePos;
        lastRead    = readPos;

        DWORD available = (readPos + ringBytes - readCursor_) % ringBytes;
        if (available > maxAvailable)
            maxAvailable = available;
        if (available == 0) {
            ++idlePolls;
            continue;
        }

        // Falling more than a ring behind means the driver lapped us.
        if (available > ringBytes - static_cast<DWORD>(frameBytes_)) {
            xruns_.fetch_add(1, std::memory_order_relaxed);
            readCursor_ = readPos;
            continue;
        }

        void*  ptr1 = nullptr; DWORD bytes1 = 0;
        void*  ptr2 = nullptr; DWORD bytes2 = 0;

        hr = buffer_->Lock(readCursor_, available, &ptr1, &bytes1, &ptr2, &bytes2, 0);
        if (FAILED(hr)) {
            ++lockFailures;
            xruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::memcpy(raw.data(), ptr1, bytes1);
        if (ptr2 && bytes2)
            std::memcpy(raw.data() + bytes1, ptr2, bytes2);

        buffer_->Unlock(ptr1, bytes1, ptr2, bytes2);

        const DWORD totalBytes = bytes1 + bytes2;
        const int   frames     = static_cast<int>(totalBytes / frameBytes_);
        const size_t samples   = static_cast<size_t>(frames) * channels_;

        if (samples > scratch_.size())
            scratch_.resize(samples);

        convertToFloat(raw.data(), sampleFormat_, scratch_.data(), samples);

        if (sink_->write(scratch_.data(), frames) < frames)
            xruns_.fetch_add(1, std::memory_order_relaxed);

        readCursor_ = (readCursor_ + totalBytes) % ringBytes;

        framesDelivered += static_cast<u64>(frames);
        if (!reportedFirst && framesDelivered > 0) {
            reportedFirst = true;
            RV_INFO("DirectSound capture delivering: first %llu frames after %d poll(s)",
                    static_cast<unsigned long long>(framesDelivered), pollsSinceStart);
        }
    }

    if (pollTimer)
        ::CloseHandle(pollTimer);

    RV_INFO("DirectSound capture stopped: %llu frames, %d polls (%llu idle, %llu lock "
            "failures), largest read %lu of %lu bytes, final cursors capture %lu / read %lu",
            static_cast<unsigned long long>(framesDelivered), pollsSinceStart,
            static_cast<unsigned long long>(idlePolls),
            static_cast<unsigned long long>(lockFailures),
            static_cast<unsigned long>(maxAvailable),
            static_cast<unsigned long>(ringBytes),
            static_cast<unsigned long>(lastCapture),
            static_cast<unsigned long>(lastRead));

    buffer_->Stop();
    running_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

bool DirectSoundOutputStream::openOutput(const StreamConfig& config)
{
    stop();
    error_.clear();

    sampleFormat_ = kDsFormat;
    channels_     = std::clamp(config.channels, 1, 2);
    sampleRate_   = config.sampleRate;
    periodFrames_ = config.bufferFrames > 0 ? config.bufferFrames
                                            : static_cast<int>(config.sampleRate * 0.010);
    ringFrames_   = periodFrames_ * kPeriodsInRing;
    frameBytes_   = channels_ * 2;

    GUID guid{};
    const bool haveGuid = parseDeviceGuid(config.deviceId, guid);

    HRESULT hr = ::DirectSoundCreate8(haveGuid ? &guid : nullptr, device_.put(), nullptr);
    if (FAILED(hr)) {
        error_ = "DirectSoundCreate8 failed: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    // DSSCL_PRIORITY is required before the primary buffer format can be set,
    // and it is what lets a background window keep rendering.
    hr = device_->SetCooperativeLevel(::GetDesktopWindow(), DSSCL_PRIORITY);
    if (FAILED(hr))
        RV_WARN("DirectSound SetCooperativeLevel failed: %s", wasapi::describeHresult(hr).c_str());

    WAVEFORMATEX wf = makeWaveFormat(channels_, static_cast<int>(sampleRate_));

    DSBUFFERDESC desc{};
    desc.dwSize        = sizeof(desc);
    desc.dwFlags       = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS |
                         DSBCAPS_CTRLPOSITIONNOTIFY;
    desc.dwBufferBytes = static_cast<DWORD>(ringFrames_ * frameBytes_);
    desc.lpwfxFormat   = &wf;

    ComPtr<IDirectSoundBuffer> base;
    hr = device_->CreateSoundBuffer(&desc, base.put(), nullptr);
    if (FAILED(hr)) {
        error_ = "could not create the DirectSound buffer: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    hr = base->QueryInterface(IID_IDirectSoundBuffer8, buffer_.putVoid());
    if (FAILED(hr)) {
        error_ = "IDirectSoundBuffer8 unavailable: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    RV_INFO("DirectSound render opened: %d ch @ %.0f Hz, ring %d frames",
            channels_, sampleRate_, ringFrames_);
    return true;
}

bool DirectSoundOutputStream::runOutput(IAudioProducer& producer)
{
    if (!buffer_) {
        error_ = "the render stream was not opened";
        return false;
    }

    producer_ = &producer;
    scratch_.assign(static_cast<size_t>(ringFrames_) * channels_, 0.0f);
    writeCursor_ = 0;

    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        error_ = "could not create the stop event";
        return false;
    }

    launchThread();
    return true;
}

void DirectSoundOutputStream::threadBody()
{
    const DWORD ringBytes = static_cast<DWORD>(ringFrames_ * frameBytes_);
    const DWORD interval  = pollIntervalMs(periodFrames_, sampleRate_);

    std::vector<u8> raw(static_cast<size_t>(ringBytes));

    // Fill the ring with silence and start one period ahead of the play cursor.
    {
        void* ptr1 = nullptr; DWORD bytes1 = 0;
        void* ptr2 = nullptr; DWORD bytes2 = 0;
        if (SUCCEEDED(buffer_->Lock(0, ringBytes, &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
            std::memset(ptr1, 0, bytes1);
            if (ptr2)
                std::memset(ptr2, 0, bytes2);
            buffer_->Unlock(ptr1, bytes1, ptr2, bytes2);
        }
    }

    writeCursor_ = 0;

    HRESULT hr = buffer_->Play(0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) {
        error_ = "could not start DirectSound playback: " + wasapi::describeHresult(hr);
        RV_ERROR("%s", error_.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    HANDLE pollTimer = createPollTimer(interval);

    while (running_.load(std::memory_order_acquire)) {
        if (!waitForPoll(pollTimer, interval))
            break;

        DWORD playPos = 0, safeWritePos = 0;
        if (FAILED(buffer_->GetCurrentPosition(&playPos, &safeWritePos)))
            continue;

        // Write everything between our cursor and the point the hardware is
        // about to reach, less one period of guard so a late poll does not
        // overwrite audio already being played.
        const DWORD guard = static_cast<DWORD>(periodFrames_ * frameBytes_);
        DWORD target = (playPos + ringBytes - guard) % ringBytes;
        DWORD toWrite = (target + ringBytes - writeCursor_) % ringBytes;

        if (toWrite < static_cast<DWORD>(frameBytes_))
            continue;

        // The play cursor overtook us: resynchronise instead of writing into
        // the past.
        const DWORD behind = (playPos + ringBytes - writeCursor_) % ringBytes;
        if (behind > ringBytes - guard) {
            xruns_.fetch_add(1, std::memory_order_relaxed);
            writeCursor_ = safeWritePos;
            continue;
        }

        const int    frames  = static_cast<int>(toWrite / frameBytes_);
        const size_t samples = static_cast<size_t>(frames) * channels_;
        if (samples > scratch_.size())
            scratch_.resize(samples);

        producer_->produce(scratch_.data(), channels_, frames);
        convertFromFloat(scratch_.data(), raw.data(), sampleFormat_, samples);

        void* ptr1 = nullptr; DWORD bytes1 = 0;
        void* ptr2 = nullptr; DWORD bytes2 = 0;

        hr = buffer_->Lock(writeCursor_, static_cast<DWORD>(frames * frameBytes_),
                           &ptr1, &bytes1, &ptr2, &bytes2, 0);
        if (hr == DSERR_BUFFERLOST) {
            buffer_->Restore();
            continue;
        }
        if (FAILED(hr)) {
            xruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::memcpy(ptr1, raw.data(), bytes1);
        if (ptr2 && bytes2)
            std::memcpy(ptr2, raw.data() + bytes1, bytes2);

        buffer_->Unlock(ptr1, bytes1, ptr2, bytes2);

        writeCursor_ = (writeCursor_ + bytes1 + bytes2) % ringBytes;
    }

    if (pollTimer)
        ::CloseHandle(pollTimer);

    buffer_->Stop();
    running_.store(false, std::memory_order_release);
}

} // namespace rv::audio
