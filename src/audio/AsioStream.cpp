// ASIO backend implementation.
//
// NOTE: this translation unit is only compiled when RV_ENABLE_ASIO=ON, which
// additionally requires RV_ASIO_SDK_DIR to point at a Steinberg ASIO SDK
// checkout. The SDK cannot be redistributed and has to be downloaded manually,
// so this file is excluded from the default build.

#include "audio/AsioStream.h"

#if RV_HAS_ASIO

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "asio.h"
#include "asiodrivers.h"

#include "core/Denormal.h"
#include "core/Log.h"

namespace rv::audio {
namespace {

/// The ASIO host API keeps driver enumeration and loading in one global object.
AsioDrivers& drivers()
{
    static AsioDrivers instance;
    return instance;
}

/// ASIO callbacks carry no user pointer. Only one driver can be loaded per
/// process, so a single static instance pointer is sufficient - and is the
/// approach every ASIO host takes for the same reason.
std::atomic<AsioStream*> g_active{nullptr};

void bufferSwitchTrampoline(long index, ASIOBool /*directProcess*/)
{
    if (AsioStream* stream = g_active.load(std::memory_order_acquire))
        stream->onBufferSwitch(index);
}

ASIOTime* bufferSwitchTimeInfoTrampoline(ASIOTime* params, long index, ASIOBool)
{
    if (AsioStream* stream = g_active.load(std::memory_order_acquire))
        stream->onBufferSwitch(index);
    return params;
}

void sampleRateChangedTrampoline(ASIOSampleRate rate)
{
    if (AsioStream* stream = g_active.load(std::memory_order_acquire))
        stream->onSampleRateChanged(static_cast<double>(rate));
}

long asioMessageTrampoline(long selector, long value, void*, double*)
{
    switch (selector) {
        case kAsioSelectorSupported:
            // Report which of the messages below we actually handle.
            switch (value) {
                case kAsioResetRequest:
                case kAsioEngineVersion:
                case kAsioResyncRequest:
                case kAsioLatenciesChanged:
                case kAsioSupportsTimeInfo:
                    return 1;
                default:
                    return 0;
            }

        case kAsioEngineVersion:
            return 2;

        case kAsioResetRequest:
            if (AsioStream* stream = g_active.load(std::memory_order_acquire))
                stream->onReset();
            return 1;

        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
            return 1;

        case kAsioSupportsTimeInfo:
            // bufferSwitchTimeInfo is provided, but the time stamps are not
            // used: this is a live processor with no transport to sync to.
            return 1;

        default:
            return 0;
    }
}

ASIOCallbacks g_callbacks = {
    bufferSwitchTrampoline,
    sampleRateChangedTrampoline,
    asioMessageTrampoline,
    bufferSwitchTimeInfoTrampoline,
};

/// Reads one ASIO channel buffer into a strided float destination.
void readChannel(const void* src, long sampleType, float* dst, int stride, int frames)
{
    switch (sampleType) {
        case ASIOSTFloat32LSB: {
            const auto* s = static_cast<const float*>(src);
            for (int i = 0; i < frames; ++i)
                dst[static_cast<size_t>(i) * stride] = s[i];
            break;
        }
        case ASIOSTFloat64LSB: {
            const auto* s = static_cast<const double*>(src);
            for (int i = 0; i < frames; ++i)
                dst[static_cast<size_t>(i) * stride] = static_cast<float>(s[i]);
            break;
        }
        case ASIOSTInt16LSB: {
            const auto* s = static_cast<const i16*>(src);
            constexpr float scale = 1.0f / 32768.0f;
            for (int i = 0; i < frames; ++i)
                dst[static_cast<size_t>(i) * stride] = static_cast<float>(s[i]) * scale;
            break;
        }
        case ASIOSTInt24LSB: {
            const auto* s = static_cast<const u8*>(src);
            constexpr float scale = 1.0f / 8388608.0f;
            for (int i = 0; i < frames; ++i) {
                const u32 raw = static_cast<u32>(s[0]) |
                                (static_cast<u32>(s[1]) << 8) |
                                (static_cast<u32>(s[2]) << 16);
                const i32 v = static_cast<i32>(raw << 8) >> 8;
                dst[static_cast<size_t>(i) * stride] = static_cast<float>(v) * scale;
                s += 3;
            }
            break;
        }
        case ASIOSTInt32LSB: {
            const auto* s = static_cast<const i32*>(src);
            constexpr float scale = 1.0f / 2147483648.0f;
            for (int i = 0; i < frames; ++i)
                dst[static_cast<size_t>(i) * stride] = static_cast<float>(s[i]) * scale;
            break;
        }
        // Int32 containers holding fewer significant bits, right-aligned.
        case ASIOSTInt32LSB16:
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24: {
            const int shift = (sampleType == ASIOSTInt32LSB16) ? 16
                            : (sampleType == ASIOSTInt32LSB18) ? 18
                            : (sampleType == ASIOSTInt32LSB20) ? 20 : 24;
            const auto* s = static_cast<const i32*>(src);
            const float scale = 1.0f / static_cast<float>(1 << (shift - 1));
            for (int i = 0; i < frames; ++i)
                dst[static_cast<size_t>(i) * stride] = static_cast<float>(s[i]) * scale;
            break;
        }
        default:
            for (int i = 0; i < frames; ++i)
                dst[static_cast<size_t>(i) * stride] = 0.0f;
            break;
    }
}

/// Writes a strided float source into one ASIO channel buffer.
void writeChannel(const float* src, int stride, void* dst, long sampleType, int frames)
{
    switch (sampleType) {
        case ASIOSTFloat32LSB: {
            auto* d = static_cast<float*>(dst);
            for (int i = 0; i < frames; ++i)
                d[i] = src[static_cast<size_t>(i) * stride];
            break;
        }
        case ASIOSTFloat64LSB: {
            auto* d = static_cast<double*>(dst);
            for (int i = 0; i < frames; ++i)
                d[i] = static_cast<double>(src[static_cast<size_t>(i) * stride]);
            break;
        }
        case ASIOSTInt16LSB: {
            auto* d = static_cast<i16*>(dst);
            for (int i = 0; i < frames; ++i) {
                const float v = std::clamp(src[static_cast<size_t>(i) * stride], -1.0f, 1.0f);
                d[i] = static_cast<i16>(v * 32767.0f);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            auto* d = static_cast<u8*>(dst);
            for (int i = 0; i < frames; ++i) {
                const float v = std::clamp(src[static_cast<size_t>(i) * stride], -1.0f, 1.0f);
                const i32   x = static_cast<i32>(v * 8388607.0f);
                d[0] = static_cast<u8>(x & 0xFF);
                d[1] = static_cast<u8>((x >> 8) & 0xFF);
                d[2] = static_cast<u8>((x >> 16) & 0xFF);
                d += 3;
            }
            break;
        }
        case ASIOSTInt32LSB: {
            auto* d = static_cast<i32*>(dst);
            for (int i = 0; i < frames; ++i) {
                const float  v = std::clamp(src[static_cast<size_t>(i) * stride], -1.0f, 1.0f);
                const double x = static_cast<double>(v) * 2147483648.0;
                d[i] = static_cast<i32>(std::clamp(x, -2147483648.0, 2147483647.0));
            }
            break;
        }
        case ASIOSTInt32LSB16:
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24: {
            const int shift = (sampleType == ASIOSTInt32LSB16) ? 16
                            : (sampleType == ASIOSTInt32LSB18) ? 18
                            : (sampleType == ASIOSTInt32LSB20) ? 20 : 24;
            auto* d = static_cast<i32*>(dst);
            const float scale = static_cast<float>((1 << (shift - 1)) - 1);
            for (int i = 0; i < frames; ++i) {
                const float v = std::clamp(src[static_cast<size_t>(i) * stride], -1.0f, 1.0f);
                d[i] = static_cast<i32>(v * scale);
            }
            break;
        }
        default:
            std::memset(dst, 0, static_cast<size_t>(frames) * 4);
            break;
    }
}

std::string describeAsioError(ASIOError code)
{
    switch (code) {
        case ASE_OK:                return "OK";
        case ASE_SUCCESS:           return "success";
        case ASE_NotPresent:        return "no input or output present, or the driver is not installed";
        case ASE_HWMalfunction:     return "hardware malfunction";
        case ASE_InvalidParameter:  return "invalid parameter";
        case ASE_InvalidMode:       return "invalid mode";
        case ASE_SPNotAdvancing:    return "the sample position is not advancing";
        case ASE_NoClock:           return "the sample rate is not supported or no clock is present";
        case ASE_NoMemory:          return "out of memory";
        default:                    return "ASIO error " + std::to_string(code);
    }
}

} // namespace

AsioStream::~AsioStream()
{
    stop();
    closeDriver();
}

bool AsioStream::isDriverLoaded()
{
    return g_active.load(std::memory_order_acquire) != nullptr;
}

bool AsioStream::runInput(AudioRing& sink)
{
    if (sink.channels() != inputChannels_) {
        error_ = "the ring buffer does not match the negotiated channel count";
        return false;
    }
    sink_ = &sink;
    return startDriver();
}

bool AsioStream::runOutput(IAudioProducer& producer)
{
    producer_ = &producer;
    return startDriver();
}

bool AsioStream::runDuplex(AudioRing& sink, IAudioProducer& producer)
{
    if (sink.channels() != inputChannels_) {
        error_ = "the ring buffer does not match the negotiated channel count";
        return false;
    }
    sink_     = &sink;
    producer_ = &producer;
    return startDriver();
}

bool AsioStream::startDriver()
{
    if (started_)
        return true;

    if (!buffersCreated_) {
        error_ = "the ASIO device was not opened";
        return false;
    }

    running_.store(true, std::memory_order_release);

    const ASIOError status = ASIOStart();
    if (status != ASE_OK) {
        running_.store(false, std::memory_order_release);
        error_ = "ASIOStart failed: " + describeAsioError(status);
        RV_ERROR("%s", error_.c_str());
        return false;
    }

    started_ = true;
    return true;
}

bool AsioStream::openDevice(const StreamConfig& config, bool wantInput, bool wantOutput)
{
    stop();
    closeDriver();
    error_.clear();

    if (g_active.load(std::memory_order_acquire) != nullptr) {
        error_ = "another ASIO stream is already open; only one ASIO driver can be "
                 "loaded per process";
        return false;
    }

    // AsioDrivers::loadDriver takes a mutable char*, and the SDK does not
    // promise not to write to it.
    std::string driverName = config.deviceId;
    std::vector<char> nameBuffer(driverName.begin(), driverName.end());
    nameBuffer.push_back('\0');

    if (!drivers().loadDriver(nameBuffer.data())) {
        error_ = "could not load the ASIO driver \"" + driverName + "\"";
        RV_ERROR("%s", error_.c_str());
        return false;
    }
    driverLoaded_ = true;

    ASIODriverInfo info{};
    info.asioVersion = 2;
    info.sysRef      = ::GetDesktopWindow();

    ASIOError status = ASIOInit(&info);
    if (status != ASE_OK) {
        error_ = "ASIOInit failed: " + describeAsioError(status);
        RV_ERROR("%s", error_.c_str());
        closeDriver();
        return false;
    }

    // The callbacks can fire from the moment buffers are created, so the
    // instance must be reachable before ASIOCreateBuffers.
    g_active.store(this, std::memory_order_release);

    long availableInputs = 0, availableOutputs = 0;
    if ((status = ASIOGetChannels(&availableInputs, &availableOutputs)) != ASE_OK) {
        error_ = "ASIOGetChannels failed: " + describeAsioError(status);
        closeDriver();
        return false;
    }

    inputChannels_  = wantInput  ? std::min<int>(config.channels, availableInputs)  : 0;
    outputChannels_ = wantOutput ? std::min<int>(config.channels, availableOutputs) : 0;

    if (wantInput && inputChannels_ <= 0) {
        error_ = "the ASIO driver reports no input channels";
        closeDriver();
        return false;
    }
    if (wantOutput && outputChannels_ <= 0) {
        error_ = "the ASIO driver reports no output channels";
        closeDriver();
        return false;
    }

    // The hardware rate is a driver-wide setting. Change it only when it does
    // not already match, and fail loudly rather than silently running at the
    // wrong rate.
    ASIOSampleRate currentRate = 0.0;
    ASIOGetSampleRate(&currentRate);
    if (std::abs(static_cast<double>(currentRate) - config.sampleRate) > 1.0) {
        if (ASIOCanSampleRate(config.sampleRate) != ASE_OK ||
            ASIOSetSampleRate(config.sampleRate) != ASE_OK) {
            error_ = "the ASIO driver does not support " +
                     std::to_string(static_cast<int>(config.sampleRate)) +
                     " Hz; set the rate in the driver's control panel and select "
                     "the matching rate here";
            RV_ERROR("%s", error_.c_str());
            closeDriver();
            return false;
        }
    }
    ASIOGetSampleRate(&currentRate);
    sampleRate_ = static_cast<double>(currentRate);

    long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;
    if ((status = ASIOGetBufferSize(&minSize, &maxSize, &preferredSize, &granularity)) != ASE_OK) {
        error_ = "ASIOGetBufferSize failed: " + describeAsioError(status);
        closeDriver();
        return false;
    }

    // Drivers are happiest at their preferred size; honour an explicit request
    // only when it is within the advertised range.
    bufferFrames_ = preferredSize;
    if (config.bufferFrames > 0 &&
        config.bufferFrames >= minSize && config.bufferFrames <= maxSize) {
        bufferFrames_ = config.bufferFrames;
    }

    bufferInfoCount_ = inputChannels_ + outputChannels_;
    auto* infos = new ASIOBufferInfo[static_cast<size_t>(bufferInfoCount_)];
    bufferInfos_ = infos;

    for (int i = 0; i < inputChannels_; ++i) {
        infos[i].isInput    = ASIOTrue;
        infos[i].channelNum = i;
        infos[i].buffers[0] = infos[i].buffers[1] = nullptr;
    }
    for (int i = 0; i < outputChannels_; ++i) {
        const int index = inputChannels_ + i;
        infos[index].isInput    = ASIOFalse;
        infos[index].channelNum = i;
        infos[index].buffers[0] = infos[index].buffers[1] = nullptr;
    }

    status = ASIOCreateBuffers(infos, bufferInfoCount_, bufferFrames_, &g_callbacks);
    if (status != ASE_OK) {
        error_ = "ASIOCreateBuffers failed: " + describeAsioError(status);
        RV_ERROR("%s", error_.c_str());
        closeDriver();
        return false;
    }
    buffersCreated_ = true;

    // Sample types are per channel in the API but uniform in every driver seen
    // in practice; the first channel of each direction defines the format.
    if (inputChannels_ > 0) {
        ASIOChannelInfo channelInfo{};
        channelInfo.channel = 0;
        channelInfo.isInput = ASIOTrue;
        if (ASIOGetChannelInfo(&channelInfo) == ASE_OK)
            inputSampleType_ = channelInfo.type;
    }
    if (outputChannels_ > 0) {
        ASIOChannelInfo channelInfo{};
        channelInfo.channel = 0;
        channelInfo.isInput = ASIOFalse;
        if (ASIOGetChannelInfo(&channelInfo) == ASE_OK)
            outputSampleType_ = channelInfo.type;
    }

    long inputLatency = 0, outputLatency = 0;
    if (ASIOGetLatencies(&inputLatency, &outputLatency) == ASE_OK) {
        inputLatency_  = static_cast<int>(inputLatency);
        outputLatency_ = static_cast<int>(outputLatency);
    }

    postOutput_ = (ASIOOutputReady() == ASE_OK);

    inputScratch_.assign(static_cast<size_t>(bufferFrames_) * std::max(1, inputChannels_), 0.0f);
    outputScratch_.assign(static_cast<size_t>(bufferFrames_) * std::max(1, outputChannels_), 0.0f);

    RV_INFO("ASIO opened: \"%s\", %d in / %d out @ %.0f Hz, buffer %d frames (%.2f ms), "
            "latency %d/%d frames",
            driverName.c_str(), inputChannels_, outputChannels_, sampleRate_,
            bufferFrames_, 1000.0 * bufferFrames_ / sampleRate_,
            inputLatency_, outputLatency_);

    return true;
}

void AsioStream::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;

    started_ = false;
    ASIOStop();
    // ASIOStop is synchronous: no callback is in flight once it returns, so
    // clearing the instance pointer here cannot race with bufferSwitch.
    g_active.store(nullptr, std::memory_order_release);
}

void AsioStream::closeDriver()
{
    if (running_.load(std::memory_order_acquire))
        stop();

    started_ = false;

    if (buffersCreated_) {
        ASIODisposeBuffers();
        buffersCreated_ = false;
    }

    if (bufferInfos_) {
        delete[] static_cast<ASIOBufferInfo*>(bufferInfos_);
        bufferInfos_ = nullptr;
        bufferInfoCount_ = 0;
    }

    if (driverLoaded_) {
        ASIOExit();
        drivers().removeCurrentDriver();
        driverLoaded_ = false;
    }

    g_active.store(nullptr, std::memory_order_release);
}

bool AsioStream::showControlPanel()
{
    if (!driverLoaded_) {
        error_ = "the ASIO driver is not open";
        return false;
    }
    return ASIOControlPanel() == ASE_OK;
}

void AsioStream::onBufferSwitch(long bufferIndex)
{
    if (!running_.load(std::memory_order_acquire))
        return;

    ScopedNoDenormals noDenormals;

    auto* infos = static_cast<ASIOBufferInfo*>(bufferInfos_);
    if (!infos)
        return;

    const int frames = bufferFrames_;

    if (inputChannels_ > 0 && sink_) {
        for (int c = 0; c < inputChannels_; ++c) {
            readChannel(infos[c].buffers[bufferIndex], inputSampleType_,
                        inputScratch_.data() + c, inputChannels_, frames);
        }
        if (sink_->write(inputScratch_.data(), frames) < frames)
            xruns_.fetch_add(1, std::memory_order_relaxed);
    }

    if (outputChannels_ > 0 && producer_) {
        producer_->produce(outputScratch_.data(), outputChannels_, frames);
        for (int c = 0; c < outputChannels_; ++c) {
            const int index = inputChannels_ + c;
            writeChannel(outputScratch_.data() + c, outputChannels_,
                         infos[index].buffers[bufferIndex], outputSampleType_, frames);
        }
    } else {
        // Output side unused (input-only ASIO): leave the driver's buffers
        // silent rather than whatever they last contained.
        for (int c = 0; c < outputChannels_; ++c) {
            const int index = inputChannels_ + c;
            std::memset(infos[index].buffers[bufferIndex], 0,
                        static_cast<size_t>(frames) * 4);
        }
    }

    if (postOutput_)
        ASIOOutputReady();
}

void AsioStream::onSampleRateChanged(double rate)
{
    // Logged rather than acted upon: the engine's format is fixed for the
    // lifetime of the stream, so the user has to restart it.
    RV_WARN("ASIO driver changed the sample rate to %.0f Hz; restart the stream to follow it",
            rate);
    sampleRate_ = rate;
}

void AsioStream::onReset()
{
    RV_WARN("ASIO driver requested a reset; the stream must be restarted");
    error_ = "the ASIO driver requested a reset (device changed or removed)";
    running_.store(false, std::memory_order_release);
}

} // namespace rv::audio

#endif // RV_HAS_ASIO
