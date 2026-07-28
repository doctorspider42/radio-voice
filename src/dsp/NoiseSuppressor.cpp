#include "dsp/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>

#include "core/Log.h"

#if RV_HAS_RNNOISE
#include <rnnoise.h>
#endif

namespace rv::dsp {
namespace {

/// RNNoise works in the integer sample range, this application in -1..1.
constexpr float kToRnnoise   = 32768.0f;
constexpr float kFromRnnoise = 1.0f / 32768.0f;

} // namespace

NoiseSuppressor::NoiseSuppressor(Params& params, Meters& meters)
    : params_(params)
    , meters_(meters)
{
}

NoiseSuppressor::~NoiseSuppressor()
{
    releaseChannels();
}

void NoiseSuppressor::releaseChannels()
{
#if RV_HAS_RNNOISE
    for (auto& channel : channels_) {
        if (channel.state) {
            rnnoise_destroy(channel.state);
            channel.state = nullptr;
        }
    }
#endif
    channels_.clear();
}

void NoiseSuppressor::prepare(double sampleRate, int /*maxFrames*/, int channels)
{
    releaseChannels();

    sampleRate_ = sampleRate;
    active_     = false;
    inactiveReason_.clear();

#if !RV_HAS_RNNOISE
    inactiveReason_ = "this build was compiled without RNNoise "
                      "(configure with -DRV_ENABLE_RNNOISE=ON)";
    return;
#else
    // The model is trained at 48 kHz and the frame size is baked into it, so
    // there is no meaningful way to run it at another rate. Resampling around
    // it would add delay and cost on a path that already has both, to no
    // benefit that could not be had by running the device at 48 kHz.
    if (std::abs(sampleRate - kSampleRate) > 1.0) {
        inactiveReason_ = "the model runs at 48 kHz only; this stream is at " +
                          std::to_string(static_cast<int>(sampleRate)) + " Hz";
        RV_WARN("noise suppressor inactive: %s", inactiveReason_.c_str());
        return;
    }

    const int count = std::max(1, channels);
    channels_.resize(static_cast<size_t>(count));

    for (auto& channel : channels_) {
        channel.state = rnnoise_create(nullptr);
        if (!channel.state) {
            inactiveReason_ = "the model could not be loaded";
            RV_ERROR("noise suppressor: rnnoise_create failed");
            releaseChannels();
            return;
        }

        channel.input.assign(kFrameSize, 0.0f);
        channel.output.assign(kFrameSize, 0.0f);
        channel.filled = 0;

        // The output FIFO starts full of silence, which is what turns the
        // library's frame quantisation into a fixed, honest delay rather than
        // a gap of unpredictable length at the start of the stream.
        channel.readAt = 0;
    }

    mix_.prepare(static_cast<float>(sampleRate), 20.0f, 1.0f);

    active_ = true;
    RV_INFO("noise suppressor ready: %d channel(s), %d-sample frames", count, kFrameSize);
#endif
}

void NoiseSuppressor::reset()
{
#if RV_HAS_RNNOISE
    for (auto& channel : channels_) {
        std::fill(channel.input.begin(), channel.input.end(), 0.0f);
        std::fill(channel.output.begin(), channel.output.end(), 0.0f);
        channel.filled = 0;
        channel.readAt = 0;
    }
#endif
}

void NoiseSuppressor::process(PlanarBuffer& buffer)
{
#if RV_HAS_RNNOISE
    if (!active_ || channels_.empty())
        return;

    const int frames   = buffer.frames();
    const int channels = std::min(buffer.channels(), static_cast<int>(channels_.size()));

    // Switched off still means running, at a mix of zero. The frames keep
    // flowing through the network so its recurrent state stays current, and the
    // reported latency stays put - both of which would otherwise change under
    // the user the moment they toggled the switch, which is a click and a
    // second of the model re-learning the room.
    const float amount =
        params_.denoiseEnabled.load(std::memory_order_relaxed)
            ? std::clamp(params_.denoiseAmount.load(std::memory_order_relaxed), 0.0f, 1.0f)
            : 0.0f;
    mix_.setTarget(amount);

    float speech = 0.0f;

    for (int c = 0; c < channels; ++c) {
        Channel& channel = channels_[static_cast<size_t>(c)];
        float*   samples = buffer.channel(c);

        // The ramp has to advance identically for every channel, so each starts
        // from the same point and only the last one's end state is kept.
        Smoothed mix = mix_;

        for (int i = 0; i < frames; ++i) {
            // Read before write, and the order is not a matter of taste. The
            // input fills and the output empties in lockstep, so processing
            // first would overwrite the last sample of the outgoing frame
            // before it had been collected - one sample discarded every 480,
            // which is a click a hundred times a second.
            const float wet = channel.output[static_cast<size_t>(channel.readAt)] * kFromRnnoise;
            if (channel.readAt + 1 < kFrameSize)
                ++channel.readAt;

            channel.input[static_cast<size_t>(channel.filled++)] = samples[i] * kToRnnoise;

            if (channel.filled == kFrameSize) {
                // Straight into the output buffer: it has just been drained to
                // its last sample, so there is nothing left in it to protect.
                const float probability = rnnoise_process_frame(
                    channel.state, channel.output.data(), channel.input.data());

                channel.filled = 0;
                channel.readAt = 0;

                if (c == 0)
                    speech = probability;
            }

            const float blend = mix.next();
            samples[i] = samples[i] * (1.0f - blend) + wet * blend;
        }

        if (c == channels - 1)
            mix_ = mix;
    }

    meters_.speechProbability.store(speech, std::memory_order_relaxed);
#else
    (void)buffer;
#endif
}

} // namespace rv::dsp
