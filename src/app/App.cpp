#include "app/App.h"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "core/Log.h"
#include "core/Strings.h"
#include "dsp/GraphicEq.h"
#include "dsp/NoiseGate.h"
#include "dsp/Smoothing.h"
#include "gui/EqEditor.h"
#include "gui/Widgets.h"

namespace rv::app {
namespace {

using namespace rv::gui;

constexpr const char* kVirtualCableUrl = "https://vb-audio.com/Cable/";

/// Configuration is written at most this often while knobs are moving; the
/// pending state is always flushed on exit.
constexpr double kSaveIntervalSeconds = 3.0;

const char* backendName(BackendType backend) { return toString(backend); }

ImU32 loadColour(float load)
{
    if (load > 0.85f)
        return theme::kDanger;
    if (load > 0.6f)
        return theme::kWarning;
    return theme::kSignal;
}

} // namespace

App::App() = default;

App::~App()
{
    shutdown();
}

bool App::initialize(void* mainWindow, const gui::Fonts& fonts, Config loaded)
{
    mainWindow_ = mainWindow;
    fonts_      = fonts;

    config_ = std::move(loaded);
    config_.applyTo(params_);

    engine_ = std::make_unique<audio::Engine>(params_, meters_);

    devices_.refresh();
    scanner_.loadCache();

    // A first run has no device selection; default to the system's own
    // choices, and point the output at a virtual cable if one is installed,
    // since that is the entire purpose of the application.
    if (config_.input.deviceId.empty()) {
        const auto inputs = devices_.inputs(BackendType::Wasapi);
        for (const auto& device : inputs) {
            if (device.isDefault) {
                config_.input.deviceId   = device.id;
                config_.input.deviceName = device.name;
                break;
            }
        }
    }

    if (config_.output.deviceId.empty()) {
        const auto cables = devices_.virtualCableOutputs();
        // "CABLE Input" is the side you send audio *to*; its counterpart
        // "CABLE Output" is what other applications then pick as a microphone.
        const auto preferred = std::find_if(
            cables.begin(), cables.end(),
            [](const audio::DeviceInfo& d) { return icontains(d.name, "cable input"); });

        if (preferred != cables.end()) {
            config_.output.deviceId   = preferred->id;
            config_.output.deviceName = preferred->name;
        } else if (!cables.empty()) {
            config_.output.deviceId   = cables.front().id;
            config_.output.deviceName = cables.front().name;
        }
    }

    buildChainFromConfig();

    if (config_.autoStart && !config_.output.deviceId.empty())
        startEngine();

    // A background scan on every start keeps newly installed plugins visible
    // without the user having to ask; cached entries are not re-probed.
    scanner_.startScan({}, /*full=*/false);

    return true;
}

void App::shutdown()
{
    if (!engine_)
        return;

    scanner_.cancelScan();

    stopEngine();

    captureChainToConfig();
    config_.captureFrom(params_);
    config_.save();

    // Editors must go before the plugins that own their views.
    for (auto& node : chainNodes_) {
        if (node->kind() == dsp::NodeKind::Vst3Plugin)
            static_cast<host::Vst3Plugin*>(node.get())->closeEditor();
    }

    engine_->chain().setNodes({});
    chainNodes_.clear();
    engine_.reset();
}

void App::setWindowSize(int width, int height)
{
    if (width > 200 && height > 200 &&
        (config_.windowWidth != width || config_.windowHeight != height)) {
        config_.windowWidth  = width;
        config_.windowHeight = height;
        markDirty();
    }
}

void App::markDirty()
{
    configDirty_ = true;
}

void App::saveIfDirty()
{
    if (!configDirty_)
        return;

    const double now = ImGui::GetTime();
    if (now - lastSaveTime_ < kSaveIntervalSeconds)
        return;

    captureChainToConfig();
    config_.captureFrom(params_);
    config_.save();

    configDirty_  = false;
    lastSaveTime_ = now;
}

double App::currentSampleRate() const
{
    if (engine_ && engine_->isRunning()) {
        const auto status = engine_->status();
        if (status.outputSampleRate > 0.0)
            return status.outputSampleRate;
    }
    return config_.output.sampleRate > 0 ? config_.output.sampleRate : 48000;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

void App::startEngine()
{
    startupError_.clear();

    if (config_.input.deviceId.empty()) {
        startupError_ = "no input device selected";
        return;
    }
    if (config_.output.deviceId.empty()) {
        startupError_ = "no output device selected";
        return;
    }

    if (!engine_->start(config_.toEngineConfig())) {
        const auto status = engine_->status();
        startupError_ = !status.inputError.empty() ? ("input: " + status.inputError)
                      : !status.outputError.empty() ? ("output: " + status.outputError)
                                                    : "the audio devices could not be opened";
        return;
    }

    runningConfig_ = config_;

    // Plugins were created for whatever rate was current before; the engine may
    // have negotiated a different one, so the chain is re-prepared for real.
    const auto status = engine_->status();
    engine_->chain().prepare(status.outputSampleRate, config_.maxBlockFrames,
                             config_.internalChannels);
}

void App::stopEngine()
{
    if (engine_)
        engine_->stop();
}

void App::restartEngine()
{
    stopEngine();
    startEngine();
}

// ---------------------------------------------------------------------------
// Chain
// ---------------------------------------------------------------------------

void App::buildChainFromConfig()
{
    chainNodes_.clear();

    const double sampleRate = currentSampleRate();

    // A configuration with no chain at all is a first run; give it the default
    // voice path rather than a bypassed signal.
    if (config_.chain.empty()) {
        config_.chain.push_back({"gate", false, "", "", "", ""});
        config_.chain.push_back({"equalizer", false, "", "", "", ""});
    }

    for (const auto& entry : config_.chain) {
        dsp::NodePtr node;

        if (entry.kind == "gate") {
            node = std::make_shared<dsp::NoiseGate>(params_, meters_);
        } else if (entry.kind == "equalizer") {
            node = std::make_shared<dsp::GraphicEq>(params_);
        } else if (entry.kind == "vst3") {
            host::PluginDescriptor descriptor;
            descriptor.path   = entry.pluginPath;
            descriptor.uid    = entry.pluginUid;
            descriptor.name   = entry.pluginName;

            std::string error;
            auto plugin = host::Vst3Plugin::create(descriptor, sampleRate,
                                                   config_.maxBlockFrames,
                                                   config_.internalChannels, error);
            if (!plugin) {
                RV_WARN("could not restore plugin \"%s\": %s",
                        entry.pluginName.c_str(), error.c_str());
                continue;
            }

            if (!entry.pluginState.empty())
                plugin->loadState(entry.pluginState);

            node = std::shared_ptr<dsp::ProcessorNode>(plugin.release());
        }

        if (!node)
            continue;

        node->setBypassed(entry.bypassed);
        chainNodes_.push_back(std::move(node));
    }

    publishChain();
}

void App::publishChain()
{
    engine_->chain().setNodes(chainNodes_);
    markDirty();
}

void App::addPlugin(const host::PluginDescriptor& descriptor)
{
    std::string error;
    auto plugin = host::Vst3Plugin::create(descriptor, currentSampleRate(),
                                           config_.maxBlockFrames,
                                           config_.internalChannels, error);
    if (!plugin) {
        startupError_ = "could not load \"" + descriptor.name + "\": " + error;
        RV_ERROR("%s", startupError_.c_str());
        return;
    }

    chainNodes_.push_back(std::shared_ptr<dsp::ProcessorNode>(plugin.release()));
    publishChain();
}

void App::removeNode(size_t index)
{
    if (index >= chainNodes_.size())
        return;

    auto& node = chainNodes_[index];
    if (node->kind() == dsp::NodeKind::Vst3Plugin) {
        auto* plugin = static_cast<host::Vst3Plugin*>(node.get());
        plugin->closeEditor();
        if (inspectedPlugin_ == plugin)
            inspectedPlugin_ = nullptr;
    }

    chainNodes_.erase(chainNodes_.begin() + static_cast<std::ptrdiff_t>(index));
    publishChain();
}

void App::moveNode(size_t from, size_t to)
{
    if (from >= chainNodes_.size() || to >= chainNodes_.size() || from == to)
        return;

    auto node = chainNodes_[from];
    chainNodes_.erase(chainNodes_.begin() + static_cast<std::ptrdiff_t>(from));
    chainNodes_.insert(chainNodes_.begin() + static_cast<std::ptrdiff_t>(to), node);
    publishChain();
}

void App::captureChainToConfig()
{
    config_.chain.clear();

    for (const auto& node : chainNodes_) {
        ChainEntryConfig entry;
        entry.bypassed = node->isBypassed();

        switch (node->kind()) {
            case dsp::NodeKind::Gate:
                entry.kind = "gate";
                break;
            case dsp::NodeKind::Equalizer:
                entry.kind = "equalizer";
                break;
            case dsp::NodeKind::Vst3Plugin: {
                auto* plugin = static_cast<host::Vst3Plugin*>(node.get());
                entry.kind        = "vst3";
                entry.pluginPath  = plugin->descriptor().path;
                entry.pluginUid   = plugin->descriptor().uid;
                entry.pluginName  = plugin->descriptor().name;
                entry.pluginState = plugin->saveState();
                break;
            }
        }

        config_.chain.push_back(std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void App::render()
{
    devices_.refreshIfStale(2000);
    if (engine_)
        engine_->updateSlowMeters();
    engine_->chain().collectGarbage();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);

    renderTopBar();

    const float transportHeight = 108.0f;
    const float bodyHeight = ImGui::GetContentRegionAvail().y - transportHeight -
                             ImGui::GetStyle().ItemSpacing.y;

    const float leftWidth  = 320.0f;
    const float rightWidth = 340.0f;
    const float centreWidth = ImGui::GetContentRegionAvail().x - leftWidth - rightWidth -
                              ImGui::GetStyle().ItemSpacing.x * 2;

    ImGui::BeginChild("##left", ImVec2(leftWidth, bodyHeight), ImGuiChildFlags_Borders);
    renderIoPanel();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##centre", ImVec2(centreWidth, bodyHeight), ImGuiChildFlags_None);
    renderProcessingPanel();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(rightWidth, bodyHeight), ImGuiChildFlags_Borders);
    renderChainPanel();
    ImGui::EndChild();

    renderTransportBar();

    ImGui::End();

    if (showPluginBrowser_)
        renderPluginBrowser();
    if (showLog_)
        renderLogWindow();

    saveIfDirty();
}

void App::renderTopBar()
{
    const auto status = engine_->status();
    const bool running = engine_->isRunning();

    ImGui::PushFont(fonts_.heading, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kText));
    ImGui::TextUnformatted("RadioVoice");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::SameLine(0, 16);
    if (running)
        statusPill("RUNNING", theme::kSignal);
    else if (!startupError_.empty())
        statusPill("ERROR", theme::kDanger);
    else
        statusPill("STOPPED", theme::kTextDim);

    if (running) {
        ImGui::SameLine(0, 16);
        char text[128];

        std::snprintf(text, sizeof(text), "%.1f ms",
                      static_cast<double>(meters_.latencyMs.load()));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("latency");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 5);
        ImGui::TextUnformatted(text);

        ImGui::SameLine(0, 18);
        const float load = meters_.cpuLoad.load();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("dsp");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 5);
        std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(load * 100.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(loadColour(load)));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();

        const u32 xruns = meters_.inputXruns.load() + meters_.outputXruns.load();
        ImGui::SameLine(0, 18);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("dropouts");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 5);
        std::snprintf(text, sizeof(text), "%u", xruns);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::toVec4(xruns > 0 ? theme::kWarning : theme::kTextDim));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 18);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("drift");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 5);
        std::snprintf(text, sizeof(text), "%+.0f ppm",
                      static_cast<double>(meters_.driftPpm.load()));
        ImGui::TextUnformatted(text);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Correction applied to keep the capture and playback clocks aligned.\n"
                "A steady value of a few dozen ppm is normal for two separate devices.");
        }
    }

    // Right-aligned controls.
    {
        const float buttonWidth = 96.0f;
        const float logWidth    = 70.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             buttonWidth - logWidth - ImGui::GetStyle().ItemSpacing.x);

        if (ImGui::Button(running ? "Stop" : "Start", ImVec2(buttonWidth, 0))) {
            if (running)
                stopEngine();
            else
                startEngine();
        }

        ImGui::SameLine();
        const int problems = log::problemCount();
        if (problems > 0)
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
        if (ImGui::Button("Log", ImVec2(logWidth, 0)))
            showLog_ = !showLog_;
        if (problems > 0)
            ImGui::PopStyleColor();
    }

    if (!startupError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kDanger));
        ImGui::TextWrapped("%s", startupError_.c_str());
        ImGui::PopStyleColor();
    }

    if (running && meters_.inputStarved.load()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
        ImGui::TextWrapped(
            "\"%s\" is open but is not delivering any audio. Check that the device is "
            "not muted or disabled in Windows sound settings, and - for a virtual "
            "microphone - that the application driving it is actually running.",
            config_.input.deviceName.c_str());
        ImGui::PopStyleColor();
    }

    if (running && deviceSelectionDiffers()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
        ImGui::TextUnformatted("Device settings changed.");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart audio"))
            restartEngine();
    }

    ImGui::Separator();
}

bool App::deviceSelectionDiffers() const
{
    const auto& a = config_;
    const auto& b = runningConfig_;

    return a.input.deviceId != b.input.deviceId || a.input.backend != b.input.backend ||
           a.input.wasapiMode != b.input.wasapiMode || a.input.sampleRate != b.input.sampleRate ||
           a.output.deviceId != b.output.deviceId || a.output.backend != b.output.backend ||
           a.output.wasapiMode != b.output.wasapiMode ||
           a.output.sampleRate != b.output.sampleRate ||
           a.maxBlockFrames != b.maxBlockFrames || a.internalChannels != b.internalChannels;
}

// ---------------------------------------------------------------------------
// I/O panel
// ---------------------------------------------------------------------------

void App::renderDeviceSelector(const char* label, DeviceConfig& device, bool isInput)
{
    ImGui::PushID(label);

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    // --- backend ----------------------------------------------------------
    const BackendType backends[] = {BackendType::Wasapi, BackendType::DirectSound,
                                    BackendType::Asio};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##backend", backendName(device.backend))) {
        for (BackendType backend : backends) {
            const bool selected = backend == device.backend;
            if (ImGui::Selectable(backendName(backend), selected)) {
                device.backend = backend;
                device.deviceId.clear();
                device.deviceName.clear();
                markDirty();
            }
        }
        ImGui::EndCombo();
    }

    // --- device -----------------------------------------------------------
    const auto list = isInput ? devices_.inputs(device.backend)
                              : devices_.outputs(device.backend);

    std::string preview = device.deviceName.empty() ? "(select a device)" : device.deviceName;

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        if (list.empty())
            ImGui::TextDisabled("no devices for this backend");

        for (const auto& info : list) {
            std::string entry = info.name;
            if (info.isDefault)
                entry += "   (default)";
            if (info.isVirtualCable)
                entry += "   [virtual cable]";

            const bool selected = info.id == device.deviceId;
            const bool usable   = info.usable();

            if (!usable)
                ImGui::BeginDisabled();

            if (ImGui::Selectable(entry.c_str(), selected)) {
                device.deviceId   = info.id;
                device.deviceName = info.name;
                if (info.defaultSampleRate > 0)
                    device.sampleRate = info.defaultSampleRate;
                markDirty();
            }

            if (!usable) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", info.unavailableReason.c_str());
            }
        }
        ImGui::EndCombo();
    }

    // --- mode and rate ----------------------------------------------------
    if (device.backend == BackendType::Wasapi) {
        bool exclusive = device.wasapiMode == WasapiMode::Exclusive;
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("exclusive mode");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 34);
        if (toggleSwitch("exclusive", &exclusive)) {
            device.wasapiMode = exclusive ? WasapiMode::Exclusive : WasapiMode::Shared;
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Exclusive mode bypasses the Windows mixer for lower latency,\n"
                "but locks the device: no other application can use it, and the\n"
                "sample rate must match what the driver is set to.");
        }
    }

    const int rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    char rateText[32];
    std::snprintf(rateText, sizeof(rateText), "%d Hz", device.sampleRate);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##rate", rateText)) {
        for (int rate : rates) {
            char text[32];
            std::snprintf(text, sizeof(text), "%d Hz", rate);
            if (ImGui::Selectable(text, rate == device.sampleRate)) {
                device.sampleRate = rate;
                markDirty();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::PopID();
}

void App::renderIoPanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Audio I/O");
    ImGui::PopFont();
    ImGui::Spacing();

    renderDeviceSelector("Microphone", config_.input, true);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderDeviceSelector("Output", config_.output, false);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- block size -------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted("Processing block");
    ImGui::PopStyleColor();

    const int blockSizes[] = {64, 128, 256, 512, 1024};
    char blockText[32];
    std::snprintf(blockText, sizeof(blockText), "%d samples", config_.maxBlockFrames);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##block", blockText)) {
        for (int size : blockSizes) {
            char text[32];
            std::snprintf(text, sizeof(text), "%d samples", size);
            if (ImGui::Selectable(text, size == config_.maxBlockFrames)) {
                config_.maxBlockFrames = size;
                markDirty();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Largest block handed to plugins.\n"
                          "Smaller reacts faster; larger is easier on the CPU.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- live status ------------------------------------------------------
    const auto status = engine_->status();
    if (status.running) {
        char text[96];

        std::snprintf(text, sizeof(text), "%.0f Hz / %d ch",
                      status.inputSampleRate, status.inputChannels);
        infoRow("capture", text, theme::kTextDim);

        infoRow("format", status.inputFormat.c_str(), theme::kTextDim);

        std::snprintf(text, sizeof(text), "%d frames", status.inputBufferFrames);
        infoRow("buffer", text, theme::kTextDim);

        ImGui::Spacing();

        std::snprintf(text, sizeof(text), "%.0f Hz / %d ch",
                      status.outputSampleRate, status.outputChannels);
        infoRow("render", text, theme::kTextDim);

        infoRow("format", status.outputFormat.c_str(), theme::kTextDim);

        std::snprintf(text, sizeof(text), "%d frames", status.outputBufferFrames);
        infoRow("buffer", text, theme::kTextDim);
    } else {
        ImGui::TextDisabled("audio stopped");
    }

    ImGui::Spacing();
    renderVirtualCableHint();
}

void App::renderVirtualCableHint()
{
    if (devices_.anyVirtualCableInstalled()) {
        // Already installed - offer the shortcut only when it is not in use.
        const auto cables = devices_.virtualCableOutputs();
        const bool alreadyRouted =
            std::any_of(cables.begin(), cables.end(), [&](const audio::DeviceInfo& d) {
                return d.id == config_.output.deviceId;
            });

        if (!alreadyRouted && !cables.empty()) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
            ImGui::TextWrapped(
                "A virtual cable is installed. Route the output to it so other "
                "applications can pick up the processed signal.");
            ImGui::PopStyleColor();

            for (const auto& cable : cables) {
                if (ImGui::SmallButton(("Use " + cable.name).c_str())) {
                    config_.output.deviceId   = cable.id;
                    config_.output.deviceName = cable.name;
                    config_.output.backend    = cable.backend;
                    markDirty();
                }
            }
        }
        return;
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
    ImGui::TextUnformatted("No virtual cable found");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextWrapped(
        "Windows has no built-in way for one application to expose an audio "
        "stream as a microphone to another - that needs a driver. Install a "
        "virtual cable, then set the output above to \"CABLE Input\" and choose "
        "\"CABLE Output\" as the microphone in Discord, OBS, Teams and so on.");
    ImGui::PopStyleColor();

    if (ImGui::SmallButton("Open the VB-CABLE download page")) {
        ::ShellExecuteW(nullptr, L"open", toWide(kVirtualCableUrl).c_str(),
                        nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---------------------------------------------------------------------------
// Processing panel
// ---------------------------------------------------------------------------

void App::renderProcessingPanel()
{
    // Tall enough for both knob rows of the gate plus its labels; anything
    // shorter clips the second row behind a scrollbar.
    const float dynamicsHeight = 260.0f;
    const float eqHeight = ImGui::GetContentRegionAvail().y - dynamicsHeight -
                           ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("##eq", ImVec2(0, eqHeight), ImGuiChildFlags_Borders);
    {
        bool eqEnabled = params_.eqEnabled.load();
        ImGui::PushFont(fonts_.medium, 0.0f);
        ImGui::TextUnformatted("Equalizer");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 34);
        if (toggleSwitch("eqEnable", &eqEnabled)) {
            params_.eqEnabled.store(eqEnabled);
            params_.touch();
            markDirty();
        }

        bool hpf = params_.hpfEnabled.load();
        bool lpf = params_.lpfEnabled.load();
        if (ImGui::Checkbox("High-pass", &hpf)) {
            params_.hpfEnabled.store(hpf);
            params_.touch();
            markDirty();
        }
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("Low-pass", &lpf)) {
            params_.lpfEnabled.store(lpf);
            params_.touch();
            markDirty();
        }
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("drag handles - wheel over a handle sets Q");

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float curveHeight = available.y - 96.0f;

        if (eqEditor("eqCurve", params_, engine_->inputSpectrum(),
                     static_cast<float>(currentSampleRate()),
                     ImVec2(available.x, std::max(120.0f, curveHeight)))) {
            markDirty();
        }

        // Band sliders under the curve, aligned to it.
        ImGui::Spacing();
        const float bandWidth = available.x / kEqBands;
        for (int band = 0; band < kEqBands; ++band) {
            ImGui::PushID(band);
            if (band > 0)
                ImGui::SameLine();

            ImGui::BeginGroup();
            float gain = params_.eqGainDb[band].load();
            ImGui::SetNextItemWidth(bandWidth - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::SliderFloat("##band", &gain, -18.0f, 18.0f, "%+.1f")) {
                params_.eqGainDb[band].store(gain);
                params_.touch();
                markDirty();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                params_.eqGainDb[band].store(0.0f);
                params_.touch();
                markDirty();
            }

            const std::string label = formatHz(kEqCenters[band]);
            const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (bandWidth - labelWidth) * 0.5f - 4.0f);
            ImGui::TextDisabled("%s", label.c_str());
            ImGui::EndGroup();

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##dynamics", ImVec2(0, 0), ImGuiChildFlags_None);
    {
        const float half = (ImGui::GetContentRegionAvail().x -
                            ImGui::GetStyle().ItemSpacing.x) * 0.55f;

        ImGui::BeginChild("##gate", ImVec2(half, 0), ImGuiChildFlags_Borders);
        renderGatePanel();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##limiter", ImVec2(0, 0), ImGuiChildFlags_Borders);
        renderLimiterPanel();
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void App::renderGatePanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Noise Gate");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 34);

    bool enabled = params_.gateEnabled.load();
    if (toggleSwitch("gateEnable", &enabled)) {
        params_.gateEnabled.store(enabled);
        params_.touch();
        markDirty();
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60);
    statusPill(meters_.gateOpen.load() ? "OPEN" : "SHUT",
               meters_.gateOpen.load() ? theme::kSignal : theme::kTextFaint);

    ImGui::Spacing();

    auto knobRow = [&](const char* label, std::atomic<float>& target, float minimum,
                       float maximum, const char* format, float defaultValue, bool logarithmic) {
        float value = target.load();
        const bool changed = logarithmic
                                 ? knobLog(label, &value, minimum, maximum, format, defaultValue)
                                 : knob(label, &value, minimum, maximum, format, defaultValue);
        if (changed) {
            target.store(value);
            params_.touch();
            markDirty();
        }
    };

    knobRow("threshold", params_.gateThresholdDb, -80.0f, 0.0f, "%.0f dB", -45.0f, false);
    ImGui::SameLine();
    knobRow("range", params_.gateRangeDb, -90.0f, -6.0f, "%.0f dB", -60.0f, false);
    ImGui::SameLine();
    knobRow("hysteresis", params_.gateHysteresisDb, 0.0f, 24.0f, "%.0f dB", 6.0f, false);
    ImGui::SameLine();
    knobRow("sidechain HP", params_.gateSidechainHpfHz, 20.0f, 1000.0f, "%.0f Hz", 120.0f, true);

    ImGui::Spacing();

    knobRow("attack", params_.gateAttackMs, 0.1f, 50.0f, "%.1f ms", 2.0f, true);
    ImGui::SameLine();
    knobRow("hold", params_.gateHoldMs, 0.0f, 1000.0f, "%.0f ms", 120.0f, false);
    ImGui::SameLine();
    knobRow("release", params_.gateReleaseMs, 5.0f, 2000.0f, "%.0f ms", 180.0f, true);
    ImGui::SameLine();
    knobRow("lookahead", params_.gateLookaheadMs, 0.0f, 20.0f, "%.1f ms", 3.0f, false);

    ImGui::SameLine(0, 18);
    ImGui::BeginGroup();
    ImGui::TextDisabled("GR");
    gainReductionMeter("gateGr", meters_.gateReductionDb.load(), -60.0f, ImVec2(14, 72));
    ImGui::EndGroup();
}

void App::renderLimiterPanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Output Limiter");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 34);

    bool enabled = params_.limiterEnabled.load();
    if (toggleSwitch("limiterEnable", &enabled)) {
        params_.limiterEnabled.store(enabled);
        markDirty();
    }

    ImGui::Spacing();

    float ceiling = params_.limiterCeilingDb.load();
    if (knob("ceiling", &ceiling, -12.0f, 0.0f, "%.1f dB", -1.0f)) {
        params_.limiterCeilingDb.store(ceiling);
        markDirty();
    }

    ImGui::SameLine();
    float release = params_.limiterReleaseMs.load();
    if (knobLog("release", &release, 10.0f, 500.0f, "%.0f ms", 80.0f)) {
        params_.limiterReleaseMs.store(release);
        markDirty();
    }

    ImGui::SameLine(0, 18);
    ImGui::BeginGroup();
    ImGui::TextDisabled("GR");
    gainReductionMeter("limiterGr", meters_.limiterReductionDb.load(), -24.0f, ImVec2(14, 72));
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextWrapped("Catches overshoots before they reach the virtual cable, "
                       "where the receiving application could not undo them.");
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Chain panel
// ---------------------------------------------------------------------------

void App::renderChainPanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Processing Chain");
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 96);
    if (ImGui::Button("Add plugin", ImVec2(96, 0))) {
        showPluginBrowser_ = true;
        pluginFilter_[0] = '\0';
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextWrapped("Signal flows top to bottom. Drag to reorder.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const float listHeight = inspectedPlugin_ ? ImGui::GetContentRegionAvail().y * 0.5f : 0.0f;

    ImGui::BeginChild("##chainList", ImVec2(0, listHeight), ImGuiChildFlags_None);

    size_t moveFrom = SIZE_MAX, moveTo = SIZE_MAX;
    size_t removeIndex = SIZE_MAX;

    for (size_t i = 0; i < chainNodes_.size(); ++i) {
        auto& node = chainNodes_[i];
        ImGui::PushID(static_cast<int>(node->id()));

        const bool bypassed = node->isBypassed();
        const bool isPlugin = node->kind() == dsp::NodeKind::Vst3Plugin;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec4(theme::kPanelRaised));
        ImGui::BeginChild("##item", ImVec2(0, 62), ImGuiChildFlags_Borders);

        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::toVec4(bypassed ? theme::kTextFaint : theme::kText));
        ImGui::TextUnformatted(node->name().c_str());
        ImGui::PopStyleColor();

        // Drag handle covers the label, which is the natural grab target.
        if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            const float delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y;
            if (delta < -30.0f && i > 0) {
                moveFrom = i;
                moveTo   = i - 1;
                ImGui::ResetMouseDragDelta();
            } else if (delta > 30.0f && i + 1 < chainNodes_.size()) {
                moveFrom = i;
                moveTo   = i + 1;
                ImGui::ResetMouseDragDelta();
            }
        }

        if (isPlugin) {
            auto* plugin = static_cast<host::Vst3Plugin*>(node.get());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
            ImGui::TextUnformatted(plugin->descriptor().vendor.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
            ImGui::TextUnformatted("built-in");
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - 150);

        bool bypassToggle = bypassed;
        if (toggleSwitch("bypass", &bypassToggle)) {
            node->setBypassed(bypassToggle);
            markDirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(bypassed ? "bypassed" : "active");

        if (isPlugin) {
            auto* plugin = static_cast<host::Vst3Plugin*>(node.get());

            ImGui::SameLine();
            if (ImGui::SmallButton("UI")) {
                if (!plugin->openEditor(mainWindow_))
                    inspectedPlugin_ = plugin; // no view of its own: use the list
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Params"))
                inspectedPlugin_ = (inspectedPlugin_ == plugin) ? nullptr : plugin;
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kDanger));
        if (ImGui::SmallButton("X"))
            removeIndex = i;
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::PopID();
    }

    if (chainNodes_.empty())
        ImGui::TextDisabled("the chain is empty");

    ImGui::EndChild();

    if (moveFrom != SIZE_MAX)
        moveNode(moveFrom, moveTo);
    if (removeIndex != SIZE_MAX)
        removeNode(removeIndex);

    // --- built-in modules that are not in the chain ------------------------
    const bool hasGate = std::any_of(chainNodes_.begin(), chainNodes_.end(),
                                     [](const dsp::NodePtr& n) {
                                         return n->kind() == dsp::NodeKind::Gate;
                                     });
    const bool hasEq = std::any_of(chainNodes_.begin(), chainNodes_.end(),
                                   [](const dsp::NodePtr& n) {
                                       return n->kind() == dsp::NodeKind::Equalizer;
                                   });

    if (!hasGate || !hasEq) {
        ImGui::Separator();
        if (!hasGate && ImGui::SmallButton("Add noise gate")) {
            chainNodes_.push_back(std::make_shared<dsp::NoiseGate>(params_, meters_));
            publishChain();
        }
        if (!hasEq && ImGui::SmallButton("Add equalizer")) {
            chainNodes_.push_back(std::make_shared<dsp::GraphicEq>(params_));
            publishChain();
        }
    }

    if (inspectedPlugin_) {
        ImGui::Separator();
        renderPluginParameters();
    }
}

void App::renderPluginParameters()
{
    // The pointer is only valid while the node is still in the chain.
    const bool stillPresent =
        std::any_of(chainNodes_.begin(), chainNodes_.end(), [&](const dsp::NodePtr& n) {
            return n.get() == static_cast<dsp::ProcessorNode*>(inspectedPlugin_);
        });
    if (!stillPresent) {
        inspectedPlugin_ = nullptr;
        return;
    }

    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted(inspectedPlugin_->name().c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 20);
    if (ImGui::SmallButton("x"))
        inspectedPlugin_ = nullptr;

    ImGui::BeginChild("##params", ImVec2(0, 0), ImGuiChildFlags_None);

    const auto& parameters = inspectedPlugin_->parameters();
    if (parameters.empty())
        ImGui::TextDisabled("this plugin exposes no parameters");

    for (const auto& info : parameters) {
        ImGui::PushID(static_cast<int>(info.id));

        float value = static_cast<float>(inspectedPlugin_->parameterValue(info.id));
        const std::string display = inspectedPlugin_->parameterDisplay(info.id);

        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextUnformatted(info.title.c_str());
        ImGui::PopStyleColor();
        rightLabel(display.c_str(), theme::kText);

        if (info.isReadOnly)
            ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##value", &value, 0.0f, 1.0f, "")) {
            inspectedPlugin_->setParameterValue(info.id, value);
            markDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            inspectedPlugin_->setParameterValue(info.id, info.defaultNormalized);
            markDirty();
        }

        if (info.isReadOnly)
            ImGui::EndDisabled();

        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Transport bar
// ---------------------------------------------------------------------------

void App::renderTransportBar()
{
    ImGui::BeginChild("##transport", ImVec2(0, 0), ImGuiChildFlags_Borders);

    auto meterColumn = [&](const char* title, float peak, float rms,
                           std::atomic<float>& gainDb, const char* gainId) {
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        levelMeter(title, dsp::gainToDb(peak), dsp::gainToDb(rms), ImVec2(240, 14), true);

        float value = gainDb.load();
        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat(gainId, &value, -30.0f, 30.0f, "%+.1f dB")) {
            gainDb.store(value);
            markDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            gainDb.store(0.0f);
            markDirty();
        }
        ImGui::EndGroup();
    };

    meterColumn("INPUT", meters_.inputPeak.load(), meters_.inputRms.load(),
                params_.inputGainDb, "##inGain");

    ImGui::SameLine(0, 28);

    meterColumn("OUTPUT", meters_.outputPeak.load(), meters_.outputRms.load(),
                params_.outputGainDb, "##outGain");

    ImGui::SameLine(0, 28);

    ImGui::BeginGroup();
    ImGui::Spacing();

    bool mute = params_.mute.load();
    if (mute)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::toVec4(theme::kDanger));
    if (ImGui::Button(mute ? "MUTED" : "Mute", ImVec2(90, 30))) {
        params_.mute.store(!mute);
        markDirty();
    }
    if (mute)
        ImGui::PopStyleColor();

    ImGui::SameLine();

    bool bypass = params_.bypassAll.load();
    if (bypass)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::toVec4(theme::kWarning));
    if (ImGui::Button(bypass ? "BYPASSED" : "Bypass all", ImVec2(110, 30))) {
        params_.bypassAll.store(!bypass);
        markDirty();
    }
    if (bypass)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends the raw microphone signal straight through, for A/B.");

    ImGui::EndGroup();

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Plugin browser
// ---------------------------------------------------------------------------

void App::renderPluginBrowser()
{
    ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Add plugin", &showPluginBrowser_)) {
        ImGui::End();
        return;
    }

    if (scanner_.isScanning()) {
        ImGui::ProgressBar(scanner_.progress(), ImVec2(-1, 0));
        const std::string current = scanner_.currentItem();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextWrapped("%s", current.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Cancel"))
            scanner_.cancelScan();
    } else {
        if (ImGui::Button("Rescan"))
            scanner_.startScan({}, /*full=*/false);
        ImGui::SameLine();
        if (ImGui::Button("Full rescan"))
            scanner_.startScan({}, /*full=*/true);
        ImGui::SameLine();
        ImGui::TextDisabled("VST3 folders are scanned automatically at start-up");
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "search by name or vendor",
                             pluginFilter_, sizeof(pluginFilter_));

    ImGui::BeginChild("##list", ImVec2(0, -60), ImGuiChildFlags_Borders);

    const auto plugins = scanner_.plugins();
    int shown = 0;

    for (const auto& descriptor : plugins) {
        if (!descriptor.isEffect())
            continue;

        const std::string filter = pluginFilter_;
        if (!filter.empty() && !icontains(descriptor.name, filter) &&
            !icontains(descriptor.vendor, filter))
            continue;

        ++shown;
        ImGui::PushID(descriptor.key().c_str());

        if (ImGui::Selectable(descriptor.name.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                addPlugin(descriptor);
                showPluginBrowser_ = false;
            }
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::Text("  %s   %s", descriptor.vendor.c_str(), descriptor.subCategories.c_str());
        ImGui::PopStyleColor();

        ImGui::PopID();
    }

    if (shown == 0) {
        ImGui::TextDisabled(plugins.empty()
                                ? "no VST3 plugins found - run a scan, or check that plugins "
                                  "are installed in Common Files\\VST3"
                                : "nothing matches the filter");
    }

    ImGui::EndChild();

    const auto blacklist = scanner_.blacklist();
    if (!blacklist.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
        ImGui::Text("%zu bundle(s) skipped because they failed to load", blacklist.size());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            std::string tooltip;
            for (const auto& entry : blacklist)
                tooltip += entry.path + "\n    " + entry.reason + "\n";
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Retry these"))
            scanner_.clearBlacklist();
    }

    ImGui::TextDisabled("double-click a plugin to append it to the chain");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

void App::renderLogWindow()
{
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", &showLog_)) {
        ImGui::End();
        return;
    }

    ImGui::PushFont(fonts_.small, 0.0f);
    ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    for (const auto& entry : log::snapshot()) {
        ImU32 colour = theme::kTextDim;
        switch (entry.level) {
            case log::Level::Debug:   colour = theme::kTextFaint; break;
            case log::Level::Info:    colour = theme::kTextDim;   break;
            case log::Level::Warning: colour = theme::kWarning;   break;
            case log::Level::Error:   colour = theme::kDanger;    break;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(colour));
        ImGui::Text("[%8.3f] %s", entry.timeSeconds, entry.text.c_str());
        ImGui::PopStyleColor();
    }

    // Follow the tail unless the user has scrolled up to read something.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopFont();

    ImGui::End();
}

} // namespace rv::app
