#include "platform/macos/RouteProbe.h"
#include "app/App.h"
#include "core/Paths.h"
#include "core/Log.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <json.hpp>

namespace rv::audio {
RouteProbe::RouteProbe(app::App& app) : app_(app) {}
RouteProbe::~RouteProbe()
{
    receiver_.stop();
    if (active_) app_.setMuted(originalMute_);
}
bool RouteProbe::start()
{
    originalMute_ = app_.isMuted();
    if (!app_.isEngineRunning()) { finish("The main audio engine is not running."); return false; }
    DeviceEnumerator devices;
    devices.refresh();
    const auto id = app_.outputDeviceId();
    const auto outputs = devices.virtualCableOutputs();
    const auto chosen = std::find_if(outputs.begin(), outputs.end(), [&](const auto& d) {
        return d.id == id && d.name.find("BlackHole") != std::string::npos;
    });
    if (chosen == outputs.end()) { finish("Select BlackHole as the main output before verifying the route."); return false; }
    cableName_ = chosen->name;
    StreamConfig config;
    config.deviceId = id;
    if (!receiver_.openInput(config)) { finish(receiver_.error()); return false; }
    ring_.resize(receiver_.actualChannels(), static_cast<int>(receiver_.actualSampleRate()) * 2);
    scratch_.resize(static_cast<size_t>(4096) * receiver_.actualChannels());
    if (!receiver_.runInput(ring_)) { finish(receiver_.error()); return false; }
    app_.setMuted(false);
    active_ = true;
    started_ = std::chrono::steady_clock::now();
    RV_INFO("route verification started: microphone -> effects -> %s -> independent capture; normal / mute / normal, 15 seconds", cableName_.c_str());
    return true;
}
void RouteProbe::tick()
{
    if (!active_) return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    if (seconds >= 15) { finish(); return; }
    const int next = seconds < 6 ? 0 : seconds < 9 ? 1 : 2;
    const double sinceTransition = seconds - (next == 0 ? 0 : next == 1 ? 6 : 9);
    if (next != phase_) {
        phase_ = next;
        app_.setMuted(next == 1);
        RV_INFO("route verification: %s", next == 1 ? "muted" : "normal processing");
    }
    int frames = 0;
    auto& stats = levels_[next];
    while ((frames = ring_.read(scratch_.data(), 4096)) > 0) {
        // Let hardware buffers, RNNoise FIFO and limiter lookahead drain after
        // each transition before comparing the independent receiver's levels.
        if (sinceTransition < 1) continue;
        const int samples = frames * receiver_.actualChannels();
        for (int i = 0; i < samples; ++i) {
            const float value = scratch_[i];
            if (!std::isfinite(value)) { nonFinite_ = true; continue; }
            stats.peak = std::max(stats.peak, std::abs(value));
            stats.sumSquares += static_cast<double>(value) * value;
        }
        stats.samples += samples;
    }
    if (sinceTransition >= 1) {
        stats.microphonePeak = std::max(stats.microphonePeak, app_.microphoneRms());
        stats.processedPeak = std::max(stats.processedPeak, app_.processedRms());
    }
    if (!app_.isEngineRunning() || receiver_.deviceInvalidated()) finish("An audio device stopped during verification.");
}
void RouteProbe::finish(const std::string& error)
{
    receiver_.stop();
    if (active_) { app_.setMuted(originalMute_); app_.saveConfigNow(); }
    active_ = false;
    nlohmann::json phases = nlohmann::json::array();
    for (const auto& s : levels_) phases.push_back({
        {"samples", s.samples}, {"rms", s.samples ? std::sqrt(s.sumSquares / s.samples) : 0},
        {"peak", s.peak}, {"microphone_rms_max", s.microphonePeak}, {"processed_rms_max", s.processedPeak}});
    const bool callbacks = std::all_of(levels_.begin(), levels_.end(), [](const auto& s) { return s.samples > 0; });
    const bool audible = levels_[0].peak > 1.0e-6f && levels_[2].peak > 1.0e-6f;
    const bool muted = levels_[1].peak < 1.0e-7f;
    const bool passed = error.empty() && callbacks && audible && muted && !nonFinite_ && receiver_.xruns() == 0;
    const std::string status = passed ? "passed" : !error.empty() || nonFinite_ || !callbacks || !muted ? "failed" : "inconclusive";
    nlohmann::json report{{"status", status}, {"error", error}, {"virtual_device", cableName_},
        {"phases_normal_muted_normal", phases}, {"receiver_xruns", receiver_.xruns()},
        {"finite_audio", !nonFinite_}, {"signal_before_and_after_mute", audible}, {"silence_while_muted", muted}};
    const auto path = paths::dataDir() / "route-verification.json";
    std::ofstream(path) << report.dump(2) << '\n';
    RV_INFO("route verification %s: peaks %.8f / %.8f / %.8f, receiver xruns %u; report: %s",
        status.c_str(), levels_[0].peak, levels_[1].peak, levels_[2].peak, receiver_.xruns(), path.c_str());
    if (!audible && error.empty()) RV_WARN("Not enough processed audio to prove the route. Speak during the normal phases and run --verify-route again.");
}
}
