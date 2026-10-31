#pragma once
#include "platform/macos/CoreAudioStream.h"
#include <array>
#include <chrono>
#include <memory>

namespace rv::app { class App; }
namespace rv::audio {
/// Opt-in, in-process end-to-end check. Only aggregate levels are written to
/// disk; captured microphone/loopback samples are consumed and discarded.
class RouteProbe {
public:
    explicit RouteProbe(app::App& app);
    ~RouteProbe();
    bool start();
    void tick();
private:
    struct Levels {
        unsigned long long samples = 0;
        double sumSquares = 0;
        float peak = 0, microphonePeak = 0, processedPeak = 0;
    };
    void finish(const std::string& error = {});
    app::App& app_;
    CoreAudioInputStream receiver_;
    AudioRing ring_;
    std::vector<float> scratch_;
    std::array<Levels, 3> levels_{};
    std::chrono::steady_clock::time_point started_;
    bool originalMute_ = false, active_ = false, nonFinite_ = false;
    int phase_ = -1;
    std::string cableName_;
};
}
