#include "audio/DeviceEnumerator.h"
#include "platform/macos/CoreAudioStream.h"
#include "core/Denormal.h"
#include "core/Strings.h"
#include "dsp/NoiseSuppressor.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
class Silence final : public rv::audio::IAudioProducer {
public:
    std::atomic<int> calls{0};
    void produce(float* samples, int channels, int frames) override
    {
        std::fill_n(samples, channels * frames, 0.0f);
        ++calls;
    }
};
void offline()
{
    const std::string unicode = "Zażółć / 音声 / 🎙";
    require(rv::toUtf8(rv::toWide(unicode)) == unicode, "Native wide string round-trip failed");
    require(rv::fromUtf16(u"Zażółć / 音声 / 🎙") == unicode, "VST3 UTF-16 decoding failed");
    require(rv::pathToUtf8(rv::pathFromUtf8(unicode)) == unicode, "Unicode plugin path round-trip failed");
#if defined(__aarch64__)
    std::uint64_t before, during, after;
    asm volatile("mrs %0, fpcr" : "=r"(before));
    { rv::ScopedNoDenormals scope; asm volatile("mrs %0, fpcr" : "=r"(during)); }
    asm volatile("mrs %0, fpcr" : "=r"(after));
    require((during & (1ULL << 24)) != 0 && before == after, "ARM floating-point control was not restored");
#endif
    require(rv::audio::looksLikeVirtualCable("BlackHole 2ch"), "BlackHole was not classified as a cable");
    require(!rv::audio::looksLikeVirtualCable("MacBook Pro Speakers"), "Physical speakers classified as virtual");
    rv::audio::CoreAudioInputStream input;
    rv::audio::StreamConfig missing;
    missing.deviceId = "radiovoice-test-device-that-does-not-exist";
    require(!input.openInput(missing) && !input.error().empty(), "Missing input did not fail cleanly");
    input.stop(); input.stop();
    rv::audio::CoreAudioOutputStream output;
    require(!output.openOutput(missing) && !output.error().empty(), "Missing output did not fail cleanly");
    output.stop(); output.stop();
#if RV_HAS_RNNOISE
    rv::Params params;
    rv::Meters meters;
    rv::dsp::NoiseSuppressor suppressor(params, meters);
    suppressor.prepare(48000, 256, 2);
    require(suppressor.isActive(), "RNNoise failed to initialize on the native architecture");
    rv::dsp::PlanarBuffer buffer;
    buffer.resize(2, 256);
    const auto start = std::chrono::steady_clock::now();
    for (int block = 0; block < 188; ++block) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buffer.channel(c)[i] = 0.05f * std::sin((block * 256 + i) * 0.04f);
        suppressor.process(buffer);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) require(std::isfinite(buffer.channel(c)[i]), "RNNoise emitted non-finite audio");
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("RNNoise: 1 second of stereo audio in %.3f seconds\n", seconds);
#endif
}
int playback()
{
    rv::audio::DeviceEnumerator devices;
    devices.refresh();
    auto list = devices.outputs(rv::BackendType::CoreAudio);
    const auto chosen = std::find_if(list.begin(), list.end(), [](const auto& d) { return d.isDefault && !d.isVirtualCable; });
    if (chosen == list.end()) { std::puts("No physical default output; skipping playback test"); return 77; }
    rv::audio::StreamConfig config;
    config.deviceId = chosen->id;
    // Silence exercises the real AUHAL callback without playing a test tone.
    rv::audio::CoreAudioOutputStream stream;
    Silence producer;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!stream.openOutput(config)) throw std::runtime_error(stream.error());
        require(stream.actualSampleRate() > 0 && stream.bufferFrames() > 0, "Invalid negotiated output format");
        const int previous = producer.calls.load();
        if (!stream.runOutput(producer)) throw std::runtime_error(stream.error());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stream.stop();
        require(producer.calls.load() > previous, "No CoreAudio playback callbacks arrived");
        require(stream.xruns() == 0, "CoreAudio playback layout mismatch or oversized callback");
        const int stoppedAt = producer.calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        require(producer.calls.load() == stoppedAt, "Callback continued after stop");
    }
    std::printf("CoreAudio: %d silent output callbacks; open/start/stop/reopen passed\n", producer.calls.load());
    return 0;
}
}
int main(int argc, char** argv)
{
    try {
        if (argc > 1 && std::string(argv[1]) == "--playback") return playback();
        offline();
        std::puts("macOS audio portability checks passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
