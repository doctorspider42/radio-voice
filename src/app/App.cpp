#include "app/App.h"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "core/Autostart.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "dsp/Compressor.h"
#include "dsp/GraphicEq.h"
#include "dsp/NoiseGate.h"
#include "dsp/NoiseSuppressor.h"
#include "dsp/Smoothing.h"
#include "gui/EqEditor.h"
#include "gui/Widgets.h"

namespace rv::app {
namespace {

using namespace rv::gui;

constexpr const char* kVirtualCableUrl = "https://vb-audio.com/Cable/";
constexpr const char* kReleasesUrl =
    "https://github.com/doctorspider42/radio-voice/releases";

/// Draws text, shortened with an ellipsis when it will not fit.
///
/// Needed wherever a name of unknown length shares a row with controls that
/// must stay reachable: ImGui neither wraps nor clips inline text, so a long
/// vendor name would otherwise run underneath the buttons beside it.
void textFitted(const char* text, float maxWidth)
{
    if (maxWidth <= 0.0f)
        return;

    if (ImGui::CalcTextSize(text).x <= maxWidth) {
        ImGui::TextUnformatted(text);
        return;
    }

    std::string shortened = text;
    while (!shortened.empty() &&
           ImGui::CalcTextSize((shortened + "...").c_str()).x > maxWidth)
        shortened.pop_back();

    shortened += "...";
    ImGui::TextUnformatted(shortened.c_str());

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
}

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

void App::setDpiScale(float dpiScale, const gui::Fonts& fonts)
{
    dpiScale_ = dpiScale > 0.0f ? dpiScale : 1.0f;
    fonts_    = fonts;
}

bool App::initialize(void* mainWindow, const gui::Fonts& fonts, float dpiScale,
                     Config loaded)
{
    mainWindow_ = mainWindow;
    fonts_      = fonts;
    dpiScale_   = dpiScale > 0.0f ? dpiScale : 1.0f;

    config_ = std::move(loaded);
    config_.applyTo(params_);

    engine_ = std::make_unique<audio::Engine>(params_, meters_);

    devices_.refresh();
    scanner_.loadCache();

    // A first run has no device selection; default to the system's own
    // choices, and point the output at a virtual cable if one is installed,
    // since that is the entire purpose of the application.
    //
    // The backend is assigned along with the id, never left as it was. A device
    // id is only meaningful to the backend that produced it - a WASAPI endpoint
    // id handed to the ASIO backend is read as a driver name, and the failure
    // that follows names a GUID that appears nowhere the user can act on.
    if (config_.input.deviceId.empty()) {
        const auto inputs = devices_.inputs(BackendType::Wasapi);
        for (const auto& device : inputs) {
            if (device.isDefault) {
                config_.input.backend    = BackendType::Wasapi;
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

        const audio::DeviceInfo* chosen = nullptr;
        if (preferred != cables.end())
            chosen = &(*preferred);
        else if (!cables.empty())
            chosen = &cables.front();

        if (chosen) {
            config_.output.backend    = chosen->backend;
            config_.output.deviceId   = chosen->id;
            config_.output.deviceName = chosen->name;
        }
    }

    if (config_.monitorEnabled)
        ensureMonitorDevice();

    // Recorded before the engine starts, because startEngine opens the monitor
    // from this same configuration. Leaving it unset would make the first
    // syncMonitor see a change that has already been applied and close and
    // reopen a device that was working.
    appliedMonitor_        = config_.monitor;
    appliedMonitorEnabled_ = config_.monitorEnabled;

    buildChainFromConfig();

    if (config_.autoStart && !config_.output.deviceId.empty())
        startEngine();

    // A background scan on every start keeps newly installed plugins visible
    // without the user having to ask; cached entries are not re-probed.
    scanner_.startScan({}, /*full=*/false);

    // The worker exists either way, so that "Check now" works with automatic
    // checking off. With it off it simply sits waiting and touches nothing.
    updater_.start(config_.checkForUpdates);

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

void App::toggleEngine()
{
    if (!engine_)
        return;

    if (engine_->isRunning())
        stopEngine();
    else
        startEngine();
}

bool App::updateReady() const
{
    return updater_.status().state == Updater::State::Ready;
}

std::string App::updateVersion() const
{
    const auto status = updater_.status();
    return status.state == Updater::State::Ready ? status.version : std::string{};
}

void App::requestUpdateInstall()
{
    const auto status = updater_.status();
    if (status.state != Updater::State::Ready || status.installer.empty())
        return;

    pendingInstaller_ = status.installer;
    wantsExit_        = true;

    RV_INFO("update: installing %s on the way out", status.version.c_str());
}

bool App::takeUpdateAnnouncement(std::string& version)
{
    const auto status = updater_.status();
    if (status.state != Updater::State::Ready || status.version.empty())
        return false;
    if (status.version == announcedUpdate_)
        return false;

    announcedUpdate_ = status.version;
    version          = status.version;
    return true;
}

std::string App::trayTooltip() const
{
    // NOTIFYICONDATA::szTip holds 128 characters, so there is room; what there
    // is no room for is a second line the shell would clip.
    if (!isEngineRunning())
        return "RadioVoice - stopped";
    if (isMuted())
        return "RadioVoice - muted";
    return "RadioVoice - running";
}

void App::saveConfigNow()
{
    if (!engine_)
        return;

    captureChainToConfig();
    config_.captureFrom(params_);
    config_.save();

    configDirty_  = false;
    lastSaveTime_ = ImGui::GetTime();
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

    // Catch a device that does not belong to its backend before the backend
    // gets a chance to fail on it. Opening a WASAPI endpoint id as an ASIO
    // driver name produces an error naming a GUID, which tells the user
    // nothing they can act on.
    auto checkSelection = [&](DeviceConfig& device, bool isInput) -> std::string {
        const char* side = isInput ? "input" : "output";

        if (device.deviceId.empty())
            return std::string("no ") + side + " device selected";

        const auto list = isInput ? devices_.inputs(device.backend)
                                  : devices_.outputs(device.backend);
        if (list.empty()) {
            return std::string("no ") + toString(device.backend) + " " + side +
                   " devices are available on this system";
        }

        const bool present = std::any_of(
            list.begin(), list.end(),
            [&](const audio::DeviceInfo& info) { return info.id == device.deviceId; });

        if (present)
            return {};

        // The identifier is gone but the device may not be. Reinstalling a
        // driver, or replugging an interface, destroys and recreates the
        // endpoint: same hardware, same name, new identifier. Refusing to start
        // then asks the user to re-pick a device from a list in which it is
        // already sitting, unchanged, right where they left it.
        //
        // Only an unambiguous match is adopted. Two devices sharing a name is
        // ordinary - two identical interfaces, or one card exposing several
        // ports - and silently guessing between them would route audio
        // somewhere the user did not choose, which is worse than an error.
        if (!device.deviceName.empty()) {
            const audio::DeviceInfo* match = nullptr;
            int matches = 0;

            for (const auto& info : list) {
                if (info.name == device.deviceName && info.usable()) {
                    match = &info;
                    ++matches;
                }
            }

            if (matches == 1) {
                RV_INFO("%s device \"%s\" reappeared with a new id under %s; "
                        "rebinding to it", side, device.deviceName.c_str(),
                        toString(device.backend));
                device.deviceId = match->id;
                markDirty();
                return {};
            }

            if (matches > 1) {
                return std::string(side) + " device \"" + device.deviceName +
                       "\" is no longer at the identifier it was saved under, and "
                       "more than one device now carries that name - pick the one "
                       "you want from the list";
            }
        }

        // Phrased without an article before the backend name, so it reads
        // correctly for "WASAPI", "DirectSound" and "ASIO" alike.
        return std::string(side) + " device \"" +
               (device.deviceName.empty() ? device.deviceId : device.deviceName) +
               "\" is not available under " + toString(device.backend) +
               " - it may be unplugged, or the backend may have been changed "
               "without picking a new device";
    };

    if (auto problem = checkSelection(config_.input, true); !problem.empty()) {
        startupError_ = problem;
        return;
    }
    if (auto problem = checkSelection(config_.output, false); !problem.empty()) {
        startupError_ = problem;
        return;
    }

    // The monitor gets the same treatment, but its verdict is discarded: it
    // exists so the operator can hear themselves, and losing that is no reason
    // to stop sending audio to where it is actually going. What matters here is
    // the side effect - a monitor that came back under a new identifier is
    // rebound, exactly like the other two. The engine reports the rest.
    if (config_.monitorEnabled && !config_.monitor.deviceId.empty())
        (void)checkSelection(config_.monitor, false);

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
        // Suppression first: everything after it - the gate's threshold, the
        // compressor's envelope - is deciding on a signal, and it should be the
        // cleaned one. Then the gate, so the compressor is not asked to ride
        // the noise floor, and the EQ before the compressor so it reacts to the
        // shaped signal.
        config_.chain.push_back({"denoise", false, "", "", "", ""});
        config_.chain.push_back({"gate", false, "", "", "", ""});
        config_.chain.push_back({"equalizer", false, "", "", "", ""});
        config_.chain.push_back({"compressor", false, "", "", "", ""});
    }

    for (const auto& entry : config_.chain) {
        dsp::NodePtr node;

        if (entry.kind == "denoise") {
            node = std::make_shared<dsp::NoiseSuppressor>(params_, meters_);
        } else if (entry.kind == "gate") {
            node = std::make_shared<dsp::NoiseGate>(params_, meters_);
        } else if (entry.kind == "equalizer") {
            node = std::make_shared<dsp::GraphicEq>(params_);
        } else if (entry.kind == "compressor") {
            node = std::make_shared<dsp::Compressor>(params_, meters_);
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

    // The built-in modules are part of the signal path, not optional additions,
    // so any that a stored configuration is missing are appended rather than
    // left out. That covers a configuration written by an older build - which
    // did let them be deleted - and it means the only way to take one out of
    // the path is to switch it off, which is reversible in one click and keeps
    // its position in the order.
    //
    // Appended, not inserted at their canonical position: wherever the chain
    // was left, adding to the end is the one placement that cannot silently
    // reorder anything the user arranged deliberately.
    const auto ensure = [this](dsp::NodeKind kind, auto make) {
        const bool present = std::any_of(
            chainNodes_.begin(), chainNodes_.end(),
            [kind](const dsp::NodePtr& n) { return n->kind() == kind; });
        if (!present)
            chainNodes_.push_back(make());
    };

    ensure(dsp::NodeKind::NoiseSuppressor,
           [this] { return std::make_shared<dsp::NoiseSuppressor>(params_, meters_); });
    ensure(dsp::NodeKind::Gate,
           [this] { return std::make_shared<dsp::NoiseGate>(params_, meters_); });
    ensure(dsp::NodeKind::Equalizer,
           [this] { return std::make_shared<dsp::GraphicEq>(params_); });
    ensure(dsp::NodeKind::Compressor,
           [this] { return std::make_shared<dsp::Compressor>(params_, meters_); });

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
            case dsp::NodeKind::NoiseSuppressor:
                entry.kind = "denoise";
                break;
            case dsp::NodeKind::Gate:
                entry.kind = "gate";
                break;
            case dsp::NodeKind::Equalizer:
                entry.kind = "equalizer";
                break;
            case dsp::NodeKind::Compressor:
                entry.kind = "compressor";
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

    // Both act on what the widgets left in config_ last frame, so they run
    // before anything is drawn again.
    syncMonitor();
    applyPendingDeviceChange();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12.0f), px(10.0f)));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);

    renderTopBar();

    const float transportHeight = px(108.0f);
    const float bodyHeight = ImGui::GetContentRegionAvail().y - transportHeight -
                             ImGui::GetStyle().ItemSpacing.y;

    // Proportional with a floor and a ceiling, not fixed. Fixed side columns
    // look right at the size they were designed for and squeeze the centre to
    // nothing on a smaller window - which is where the EQ and the dynamics
    // panels live, so it is the worst place to lose room.
    const float totalWidth = ImGui::GetContentRegionAvail().x;

    // Same treatment as the chain, mirrored: once the devices are set they are
    // rarely touched again, so the column holding them is worth reclaiming
    // while working on the sound.
    const float leftWidth = config_.ioCollapsed
                                ? px(34.0f)
                                : std::clamp(totalWidth * 0.23f, px(250.0f), px(340.0f));

    // Folded away, the chain keeps just enough width for the button that brings
    // it back; everything it gives up goes to the centre column.
    const float rightWidth = config_.chainCollapsed
                                 ? px(34.0f)
                                 : std::clamp(totalWidth * 0.24f, px(260.0f), px(360.0f));

    const float centreWidth = totalWidth - leftWidth - rightWidth -
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

        // A single number never explains itself, and here one term almost
        // always dominates the rest - usually the clock bridge, which no amount
        // of adding or removing plugins will move.
        if (ImGui::IsItemHovered()) {
            const auto parts = engine_->latencyBreakdown();
            ImGui::SetTooltip(
                "input device    %5.1f ms\n"
                "clock bridge    %5.1f ms\n"
                "chain           %5.1f ms\n"
                "limiter         %5.1f ms\n"
                "output device   %5.1f ms\n"
                "                ------\n"
                "total           %5.1f ms\n"
                "\n"
                "The clock bridge is the buffer that lets the capture and\n"
                "playback devices run on their own clocks. It is sized from\n"
                "the device period, so exclusive mode - or a smaller period in\n"
                "Windows sound settings - is what shrinks it.\n"
                "\n"
                "Shared-mode drivers often report zero device latency, so the\n"
                "true figure is somewhat higher than this.",
                static_cast<double>(parts.inputDevice),
                static_cast<double>(parts.bridge),
                static_cast<double>(parts.chain),
                static_cast<double>(parts.limiter),
                static_cast<double>(parts.outputDevice),
                static_cast<double>(parts.total()));
        }

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
        const float monitorWidth = px(92.0f);
        const float buttonWidth  = px(84.0f);
        const float restartWidth = px(84.0f);
        const float gearWidth    = ImGui::GetFrameHeight();
        const float spacing      = ImGui::GetStyle().ItemSpacing.x;

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             monitorWidth - buttonWidth - restartWidth - gearWidth -
                             spacing * 3);

        // Monitoring is the one routing decision an operator changes mid-take -
        // headphones on to check a plugin, off again when it feeds back - so it
        // belongs next to Start rather than three sections down the panel.
        {
            const auto status = engine_->status();
            const bool lit    = config_.monitorEnabled;
            const bool failed = lit && running && !status.monitorError.empty();

            // Lit when monitoring, red when it was asked for and could not be
            // opened. An unlit button keeps the default style rather than a
            // third colour, so "on" is the only state that draws the eye.
            int pushed = 0;
            if (failed) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::toVec4(theme::kDanger));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::toVec4(theme::kDanger));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::toVec4(theme::kDanger));
                pushed = 3;
            } else if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::toVec4(theme::kAccentDim));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::toVec4(theme::kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::toVec4(theme::kAccentFaint));
                pushed = 3;
            }

            if (ImGui::Button("Monitor", ImVec2(monitorWidth, 0))) {
                config_.monitorEnabled = !config_.monitorEnabled;
                if (config_.monitorEnabled)
                    ensureMonitorDevice();
                markDirty();
            }

            if (pushed)
                ImGui::PopStyleColor(pushed);

            if (ImGui::IsItemHovered()) {
                if (failed) {
                    ImGui::SetTooltip("Monitoring is on but the device could not be "
                                      "opened:\n%s", status.monitorError.c_str());
                } else if (config_.monitor.deviceName.empty()) {
                    ImGui::SetTooltip("Hear the processed signal on a second device.\n"
                                      "Pick one under Audio I/O.");
                } else {
                    ImGui::SetTooltip("Hear the processed signal on \"%s\".\n"
                                      "Takes effect immediately - no restart.",
                                      config_.monitor.deviceName.c_str());
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(running ? "Stop" : "Start", ImVec2(buttonWidth, 0))) {
            if (running)
                stopEngine();
            else
                startEngine();
        }

        // Always present, not only when a setting changed. Restarting the
        // stream is the first thing to try for anything that sounds wrong, and
        // hiding it behind a condition means it is missing exactly when it is
        // wanted.
        ImGui::SameLine();
        if (ImGui::Button("Restart", ImVec2(restartWidth, 0)))
            restartEngine();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stops and reopens both audio devices.");

        // Everything that is set once and then forgotten - the log, the version,
        // and where the window goes when it is dismissed - lives behind the cog.
        // The transport bar is read at a glance while talking, and more words of
        // chrome there cost more than the click they save.
        ImGui::SameLine();
        const int  problems = log::problemCount();
        const auto update   = updater_.status();
        const bool open     = ImGui::IsPopupOpen("##options");

        // Two things can want attention through one cog, so they are ranked
        // rather than blended: something went wrong beats something is
        // available, and the tooltip says which it is either way.
        const bool waitingUpdate = update.state == Updater::State::Available ||
                                   update.state == Updater::State::Downloading ||
                                   update.state == Updater::State::Ready;

        const ImU32 gearTint = problems > 0     ? theme::kWarning
                               : waitingUpdate  ? theme::kAccent
                                                : theme::kTextDim;

        if (gearButton("##options-button", gearWidth, gearTint, open))
            ImGui::OpenPopup("##options");

        if (ImGui::IsItemHovered()) {
            char note[192];
            int  written = std::snprintf(note, sizeof(note),
                                         "Options - the log, the version, and where "
                                         "the window goes.");

            if (problems > 0) {
                written += std::snprintf(note + written, sizeof(note) - written,
                                         "\n%d problem%s logged.", problems,
                                         problems == 1 ? "" : "s");
            }
            if (update.state == Updater::State::Ready) {
                std::snprintf(note + written, sizeof(note) - written,
                              "\nVersion %s is ready to install.", update.version.c_str());
            } else if (update.state == Updater::State::Available) {
                std::snprintf(note + written, sizeof(note) - written,
                              "\nVersion %s has been released.", update.version.c_str());
            }

            ImGui::SetTooltip("%s", note);
        }

        // Constrained because the update section inside can carry a paragraph of
        // release notes, and an auto-sized popup holding one is either a column
        // of single words or the width of the screen.
        ImGui::SetNextWindowSizeConstraints(ImVec2(px(330.0f), 0.0f),
                                            ImVec2(px(420.0f), FLT_MAX));
        if (ImGui::BeginPopup("##options")) {
            renderOptionsMenu();
            ImGui::EndPopup();
        }
    }

    if (!startupError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kDanger));
        ImGui::TextWrapped("%s", startupError_.c_str());
        ImGui::PopStyleColor();

        // The error message is the thing most worth pasting somewhere, and
        // ImGui text cannot be selected with the mouse.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("click to copy");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                ImGui::SetClipboardText(startupError_.c_str());
        }
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
        // Reports rather than instructs: the restart is already on its way, and
        // asking for a click that is about to become unnecessary is worse than
        // saying nothing. Kept visible because the stream does drop out for a
        // moment, and unexplained silence is alarming.
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextUnformatted("Device settings changed - reopening the stream...");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
}

void App::ensureMonitorDevice()
{
    if (!config_.monitor.deviceId.empty())
        return;

    const auto list = devices_.outputs(config_.monitor.backend);

    auto acceptable = [](const audio::DeviceInfo& info) {
        // Never a virtual cable: routing the monitor into another cable is
        // silence, which is exactly the situation being escaped.
        return info.usable() && !info.isVirtualCable;
    };

    const audio::DeviceInfo* chosen = nullptr;
    for (const auto& info : list) {
        if (acceptable(info) && info.isDefault) {
            chosen = &info;
            break;
        }
    }
    if (!chosen) {
        for (const auto& info : list) {
            if (acceptable(info)) {
                chosen = &info;
                break;
            }
        }
    }

    if (!chosen)
        return;

    config_.monitor.deviceId   = chosen->id;
    config_.monitor.deviceName = chosen->name;
    if (chosen->defaultSampleRate > 0)
        config_.monitor.sampleRate = chosen->defaultSampleRate;
}

void App::syncMonitor()
{
    const auto& m = config_.monitor;
    const auto& a = appliedMonitor_;

    const bool changed = config_.monitorEnabled != appliedMonitorEnabled_ ||
                         m.deviceId != a.deviceId || m.backend != a.backend ||
                         m.wasapiMode != a.wasapiMode || m.sampleRate != a.sampleRate;
    if (!changed)
        return;

    appliedMonitor_        = m;
    appliedMonitorEnabled_ = config_.monitorEnabled;

    audio::StreamConfig stream;
    stream.deviceId     = m.deviceId;
    stream.backend      = m.backend;
    stream.wasapiMode   = m.wasapiMode;
    stream.sampleRate   = m.sampleRate;
    stream.channels     = m.channels;
    stream.bufferFrames = m.bufferFrames;

    engine_->applyMonitor(stream, config_.monitorEnabled);
}

void App::applyPendingDeviceChange()
{
    if (!engine_->isRunning() || !deviceSelectionDiffers()) {
        deviceChangeSeenAt_ = -1.0;
        return;
    }

    const double now = ImGui::GetTime();
    if (deviceChangeSeenAt_ < 0.0) {
        deviceChangeSeenAt_ = now;
        return;
    }

    // Long enough that a backend switch and the device choice that follows it
    // count as one change, short enough to feel automatic.
    constexpr double kSettleSeconds = 0.5;
    if (now - deviceChangeSeenAt_ < kSettleSeconds)
        return;

    deviceChangeSeenAt_ = -1.0;
    restartEngine();
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

    // The monitor is deliberately absent. It is applied live by syncMonitor,
    // so listing it here would raise a restart prompt for a change that has
    // already taken effect.
}

// ---------------------------------------------------------------------------
// I/O panel
// ---------------------------------------------------------------------------

void App::renderDeviceSelector(const char* label, DeviceConfig& device, bool isInput,
                               bool allowAsio)
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
            if (backend == BackendType::Asio && !allowAsio)
                continue;
            const bool selected = backend == device.backend;
            if (ImGui::Selectable(backendName(backend), selected) && !selected) {
                device.backend = backend;

                // Pick a device for the new backend straight away rather than
                // leaving the selection empty. An empty selection is a dead end:
                // the restart prompt appears, restarting fails, and the reason
                // is a second step the user has not been told about yet.
                const auto list = isInput ? devices_.inputs(backend)
                                          : devices_.outputs(backend);

                const auto preferred = std::find_if(
                    list.begin(), list.end(),
                    [](const audio::DeviceInfo& info) {
                        return info.isDefault && info.usable();
                    });

                const audio::DeviceInfo* chosen = nullptr;
                if (preferred != list.end()) {
                    chosen = &(*preferred);
                } else {
                    const auto usable = std::find_if(
                        list.begin(), list.end(),
                        [](const audio::DeviceInfo& info) { return info.usable(); });
                    if (usable != list.end())
                        chosen = &(*usable);
                }

                if (chosen) {
                    device.deviceId   = chosen->id;
                    device.deviceName = chosen->name;
                    if (chosen->defaultSampleRate > 0)
                        device.sampleRate = chosen->defaultSampleRate;
                } else {
                    device.deviceId.clear();
                    device.deviceName.clear();
                }

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
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));
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
    // ---- folded away -----------------------------------------------------
    if (config_.ioCollapsed) {
        const float strip = ImGui::GetContentRegionAvail().x;

        if (ImGui::Button(">", ImVec2(-1.0f, 0.0f))) {
            config_.ioCollapsed = false;
            markDirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the audio devices");

        ImGui::Spacing();

        ImGui::PushFont(fonts_.small, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        for (const char* c = "AUDIO"; *c; ++c) {
            const char letter[2] = {*c, '\0'};
            const float width = ImGui::CalcTextSize(letter).x;
            ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + (strip - width) * 0.5f);
            ImGui::TextUnformatted(letter);
        }
        ImGui::PopStyleColor();

        // The one fact worth keeping visible while the panel is away: what rate
        // the stream actually negotiated. It is the number that explains a
        // surprise, and it is two characters wide.
        const auto status = engine_->status();
        if (status.running && status.outputSampleRate > 0.0) {
            ImGui::Spacing();
            char rate[8];
            std::snprintf(rate, sizeof(rate), "%.0fk", status.outputSampleRate / 1000.0);
            const float rateWidth = ImGui::CalcTextSize(rate).x;
            ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + (strip - rateWidth) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kAccent));
            ImGui::TextUnformatted(rate);
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();

        return;
    }

    // ---- expanded --------------------------------------------------------
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Audio I/O");
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(24.0f));
    if (ImGui::Button("<", ImVec2(px(24.0f), 0))) {
        config_.ioCollapsed = true;
        markDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fold the audio devices away");

    ImGui::Spacing();

    renderDeviceSelector("Microphone", config_.input, true);

    // --- input fold-down --------------------------------------------------
    // Sits with the input device because it is a property of how that device is
    // read, not of the processing. The common case it exists for: an interface
    // presenting two channels with a single microphone on the first, where
    // taking both gives a voice on one side and silence on the other.
    {
        const InputMix modes[] = {InputMix::Stereo, InputMix::MonoLeft,
                                  InputMix::MonoRight, InputMix::MonoSum};
        auto current = static_cast<InputMix>(params_.inputMix.load());

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##inputmix", toString(current))) {
            for (InputMix mode : modes) {
                if (ImGui::Selectable(toString(mode), mode == current)) {
                    params_.inputMix.store(static_cast<int>(mode));
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "How the capture device's channels are folded into the chain.\n"
                "Pick a single channel when one microphone is plugged into a\n"
                "stereo input; summing both would halve its level.");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderDeviceSelector("Output", config_.output, false);

    {
        bool mono = params_.monoOutput.load();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("mono output");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));
        if (toggleSwitch("monoOut", &mono)) {
            params_.monoOutput.store(mono);
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sums the processed signal to mono before it reaches the\n"
                              "output device, so both channels carry the same audio.");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- monitor ----------------------------------------------------------
    // A virtual cable cannot be listened to - that is what makes it a cable.
    // This is the second render device that carries the same signal, so the
    // operator hears what is being sent while it is being sent.
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextUnformatted("Monitor");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));

        bool enabled = config_.monitorEnabled;
        if (toggleSwitch("monitorOn", &enabled)) {
            config_.monitorEnabled = enabled;
            if (enabled)
                ensureMonitorDevice();
            markDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "A second output carrying the same processed signal.\n"
                "Sending to a virtual cable is silent by design; this is how\n"
                "you hear yourself while it happens.\n"
                "Starts and stops immediately - the main path is untouched.");
        }

        if (config_.monitorEnabled) {
            ImGui::Spacing();
            renderDeviceSelector("Monitor device", config_.monitor, false,
                                 /*allowAsio=*/false);

            float gainDb = params_.monitorGainDb.load();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##monitorgain", &gainDb, -60.0f, 12.0f, "%+.1f dB")) {
                params_.monitorGainDb.store(gainDb);
                markDirty();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Headphone level. Affects only what you hear -\n"
                                  "the signal being sent onwards is untouched.");
            }

            bool monitorMuted = params_.monitorMute.load();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
            ImGui::TextUnformatted("mute monitor");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 ImGui::GetContentRegionAvail().x - px(34.0f));
            if (toggleSwitch("monitorMute", &monitorMuted)) {
                params_.monitorMute.store(monitorMuted);
                markDirty();
            }

            const auto monitorStatus = engine_->status();
            if (!monitorStatus.monitorError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
                ImGui::TextWrapped("monitor: %s", monitorStatus.monitorError.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

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
    // Two knob rows plus the checkbox row the compressor adds. Knobs wrap onto
    // further rows on a narrow window, so this is a floor rather than an exact
    // fit - the panels scroll if a very narrow window pushes them past it.
    const float dynamicsHeight = px(305.0f);

    // One row: a switch, a slider and a speech indicator. Small on purpose -
    // the suppressor has one control worth having, and giving it a panel the
    // size of the compressor's would imply otherwise.
    const float denoiseHeight = ImGui::GetFrameHeight() * 2.0f +
                                ImGui::GetStyle().ItemSpacing.y +
                                ImGui::GetStyle().WindowPadding.y * 2.0f;

    // The equaliser takes what is left once the other two have been accounted
    // for. Both of their heights are subtracted up front rather than measuring
    // the remaining space after drawing them, so the order they appear in can
    // change without the arithmetic quietly following it.
    const float eqHeight = ImGui::GetContentRegionAvail().y - denoiseHeight -
                           dynamicsHeight - ImGui::GetStyle().ItemSpacing.y * 2.0f;

    ImGui::BeginChild("##eq", ImVec2(0, eqHeight), ImGuiChildFlags_Borders);
    {
        bool eqEnabled = params_.eqEnabled.load();
        ImGui::PushFont(fonts_.medium, 0.0f);
        ImGui::TextUnformatted("Equalizer");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));
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
        const float curveHeight = available.y - px(96.0f);

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

    ImGui::BeginChild("##denoise", ImVec2(0, denoiseHeight), ImGuiChildFlags_Borders);
    renderDenoisePanel();
    ImGui::EndChild();

    ImGui::BeginChild("##dynamics", ImVec2(0, 0), ImGuiChildFlags_None);
    {
        const float spacing   = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;

        // Three panels abreast need roughly this much to show their knobs
        // without wrapping them into a scrollbar. Below it the limiter - the
        // smallest of the three - drops to its own full-width row underneath.
        const float kThreeColumnMinimum = px(660.0f);

        if (available >= kThreeColumnMinimum) {
            const float usable = available - spacing * 2;

            // The gate and the compressor carry four knobs per row each, so
            // they get near-equal width. Making the gate narrower is what
            // pushes its fourth knob onto a third row and starts the panel
            // scrolling, hiding controls that had room a moment earlier.
            ImGui::BeginChild("##gate", ImVec2(usable * 0.39f, 0), ImGuiChildFlags_Borders);
            renderGatePanel();
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##comp", ImVec2(usable * 0.40f, 0), ImGuiChildFlags_Borders);
            renderCompressorPanel();
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##limiter", ImVec2(0, 0), ImGuiChildFlags_Borders);
            renderLimiterPanel();
            ImGui::EndChild();
        } else {
            const float usable    = available - spacing;
            const float topHeight = ImGui::GetContentRegionAvail().y * 0.62f;

            ImGui::BeginChild("##gate", ImVec2(usable * 0.46f, topHeight),
                              ImGuiChildFlags_Borders);
            renderGatePanel();
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##comp", ImVec2(0, topHeight), ImGuiChildFlags_Borders);
            renderCompressorPanel();
            ImGui::EndChild();

            ImGui::BeginChild("##limiter", ImVec2(0, 0), ImGuiChildFlags_Borders);
            renderLimiterPanel();
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
}

void App::renderDenoisePanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Noise Suppressor");
    ImGui::PopFont();

    // Whether the model is actually running is not something to leave the user
    // to infer from the sound. A build without it, or a stream at the wrong
    // rate, says so here rather than offering a switch that does nothing.
    const dsp::NoiseSuppressor* module = nullptr;
    for (const auto& node : chainNodes_) {
        if (node->kind() == dsp::NodeKind::NoiseSuppressor) {
            module = static_cast<const dsp::NoiseSuppressor*>(node.get());
            break;
        }
    }

    const bool unavailable = module && !module->isActive();

    if (unavailable) {
        ImGui::SameLine();
        ImGui::PushFont(fonts_.small, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
        ImGui::TextUnformatted("  unavailable");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", module->inactiveReason().c_str());
    } else {
        // Speech probability straight from the model, which is the one number
        // that says whether it is hearing what it is supposed to.
        const float speech = meters_.speechProbability.load();
        ImGui::SameLine();
        ImGui::PushFont(fonts_.small, 0.0f);
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            theme::toVec4(speech > 0.5f ? theme::kSignal : theme::kTextFaint));
        ImGui::Text("  speech %.0f%%", static_cast<double>(speech * 100.0f));
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));

    bool enabled = params_.denoiseEnabled.load();
    if (unavailable)
        ImGui::BeginDisabled();
    if (toggleSwitch("denoiseEnable", &enabled)) {
        params_.denoiseEnabled.store(enabled);
        markDirty();
    }

    float amount = params_.denoiseAmount.load() * 100.0f;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##denoiseAmount", &amount, 0.0f, 100.0f, "%.0f%% suppression")) {
        params_.denoiseAmount.store(amount * 0.01f);
        markDirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "How much of the cleaned signal replaces the original.\n"
            "Unlike the gate, this removes steady noise while you are talking -\n"
            "but taking all of it can sound processed in a quiet room, so\n"
            "leaving a little of the original in is the usual remedy.\n"
            "Costs 10 ms of latency whenever this module is in the chain.");
    }

    if (unavailable)
        ImGui::EndDisabled();
}

void App::renderGatePanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Noise Gate");
    ImGui::PopFont();

    // The state badge and the switch share the right-hand end of the header, so
    // the space for both is reserved up front. Positioning the badge by a fixed
    // offset back from the switch made them overlap as soon as the text changed
    // width.
    const bool  open      = meters_.gateOpen.load();
    const char* stateText = open ? "OPEN" : "SHUT";

    const float kSwitchWidth = px(34.0f);
    const float pillWidth = ImGui::CalcTextSize(stateText).x + 20.0f; // statusPill padding
    const float spacing   = ImGui::GetStyle().ItemSpacing.x;

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                         pillWidth - kSwitchWidth - spacing);

    statusPill(stateText, open ? theme::kSignal : theme::kTextFaint);

    ImGui::SameLine();
    bool enabled = params_.gateEnabled.load();
    if (toggleSwitch("gateEnable", &enabled)) {
        params_.gateEnabled.store(enabled);
        params_.touch();
        markDirty();
    }

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

    // Labels are abbreviated because the knob widget sizes itself to its widest
    // part; spelled out, "sidechain HP" alone would be wider than the four
    // knobs it shares a row with.
    knobRow("thresh", params_.gateThresholdDb, -80.0f, 0.0f, "%.0f dB", -45.0f, false);
    ImGui::SameLine();
    knobRow("range", params_.gateRangeDb, -90.0f, -6.0f, "%.0f dB", -60.0f, false);
    ImGui::SameLine();
    knobRow("hyst", params_.gateHysteresisDb, 0.0f, 24.0f, "%.0f dB", 6.0f, false);
    ImGui::SameLine();
    knobRow("SC HP", params_.gateSidechainHpfHz, 20.0f, 1000.0f, "%.0f Hz", 120.0f, true);

    ImGui::Spacing();

    knobRow("attack", params_.gateAttackMs, 0.1f, 50.0f, "%.1f ms", 2.0f, true);
    ImGui::SameLine();
    knobRow("hold", params_.gateHoldMs, 0.0f, 1000.0f, "%.0f ms", 120.0f, false);
    ImGui::SameLine();
    knobRow("release", params_.gateReleaseMs, 5.0f, 2000.0f, "%.0f ms", 180.0f, true);
    ImGui::SameLine();
    knobRow("look", params_.gateLookaheadMs, 0.0f, 20.0f, "%.1f ms", 3.0f, false);

    ImGui::SameLine(0, 12);
    ImGui::BeginGroup();
    ImGui::TextDisabled("GR");
    gainReductionMeter("gateGr", meters_.gateReductionDb.load(), -60.0f, ImVec2(px(14.0f), px(72.0f)));
    ImGui::EndGroup();
}

void App::renderCompressorPanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Compressor");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));

    bool enabled = params_.compEnabled.load();
    if (toggleSwitch("compEnable", &enabled)) {
        params_.compEnabled.store(enabled);
        params_.touch();
        markDirty();
    }

    ImGui::Spacing();

    auto knobRow = [&](const char* label, std::atomic<float>& target, float minimum,
                       float maximum, const char* format, float defaultValue,
                       bool logarithmic) {
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

    knobRow("thresh", params_.compThresholdDb, -60.0f, 0.0f, "%.0f dB", -18.0f, false);
    ImGui::SameLine();
    knobRow("ratio", params_.compRatio, 1.0f, 20.0f, "%.1f:1", 3.0f, true);
    ImGui::SameLine();
    knobRow("knee", params_.compKneeDb, 0.0f, 24.0f, "%.0f dB", 6.0f, false);
    ImGui::SameLine();

    // With auto make-up on, the knob shows what the compressor computed rather
    // than a stale manual value it is not using.
    const bool autoMakeup = params_.compAutoMakeup.load();
    if (autoMakeup) {
        float computed = dsp::Compressor::autoMakeupDb(params_);
        ImGui::BeginDisabled();
        knob("makeup", &computed, -12.0f, 24.0f, "%.1f dB", 0.0f);
        ImGui::EndDisabled();
    } else {
        knobRow("makeup", params_.compMakeupDb, -12.0f, 24.0f, "%.1f dB", 0.0f, false);
    }

    ImGui::Spacing();

    knobRow("attack", params_.compAttackMs, 0.1f, 200.0f, "%.1f ms", 8.0f, true);
    ImGui::SameLine();
    knobRow("release", params_.compReleaseMs, 10.0f, 2000.0f, "%.0f ms", 120.0f, true);
    ImGui::SameLine();
    knobRow("SC HP", params_.compSidechainHpfHz, 20.0f, 1000.0f, "%.0f Hz", 100.0f, true);
    ImGui::SameLine();
    knobRow("look", params_.compLookaheadMs, 0.0f, 20.0f, "%.1f ms", 2.0f, false);

    ImGui::SameLine(0, 12);
    ImGui::BeginGroup();
    ImGui::TextDisabled("GR");
    gainReductionMeter("compGr", meters_.compressorReductionDb.load(), -24.0f, ImVec2(px(14.0f), px(72.0f)));
    ImGui::EndGroup();

    ImGui::Spacing();

    bool autoMakeupToggle = autoMakeup;
    if (ImGui::Checkbox("auto makeup", &autoMakeupToggle)) {
        params_.compAutoMakeup.store(autoMakeupToggle);
        params_.touch();
        markDirty();
    }

    ImGui::SameLine(0, 16);
    bool rms = params_.compRmsDetection.load();
    if (ImGui::Checkbox("RMS", &rms)) {
        params_.compRmsDetection.store(rms);
        params_.touch();
        markDirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("RMS follows loudness and sounds transparent on speech.\n"
                          "Off is peak detection: catches every transient, reacts harder.");
    }
}

void App::renderLimiterPanel()
{
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Output Limiter");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(34.0f));

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
    gainReductionMeter("limiterGr", meters_.limiterReductionDb.load(), -24.0f, ImVec2(px(14.0f), px(72.0f)));
    ImGui::EndGroup();

    // Only worth the space when there is space; on a narrow window the knobs
    // matter more than the explanation.
    if (ImGui::GetContentRegionAvail().y > 40.0f) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextWrapped("Catches overshoots before they reach the virtual cable, "
                           "where the receiving application could not undo them.");
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------------------
// Chain panel
// ---------------------------------------------------------------------------

void App::renderChainPanel()
{
    // ---- folded away -----------------------------------------------------
    if (config_.chainCollapsed) {
        const float strip = ImGui::GetContentRegionAvail().x;

        if (ImGui::Button("<", ImVec2(-1.0f, 0.0f))) {
            config_.chainCollapsed = false;
            markDirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the processing chain");

        ImGui::Spacing();

        // One letter per line: a vertical label is the only way to name a strip
        // this narrow, and it keeps the collapsed column identifiable rather
        // than just a mystery button.
        ImGui::PushFont(fonts_.small, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        for (const char* c = "CHAIN"; *c; ++c) {
            const char letter[2] = {*c, '\0'};
            const float width = ImGui::CalcTextSize(letter).x;
            ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + (strip - width) * 0.5f);
            ImGui::TextUnformatted(letter);
        }
        ImGui::PopStyleColor();

        // The count is what someone actually wants to know while it is folded:
        // whether anything is in the chain at all.
        ImGui::Spacing();
        char count[8];
        std::snprintf(count, sizeof(count), "%zu", chainNodes_.size());
        const float countWidth = ImGui::CalcTextSize(count).x;
        ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + (strip - countWidth) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kAccent));
        ImGui::TextUnformatted(count);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        return;
    }

    // ---- expanded --------------------------------------------------------
    if (ImGui::Button(">", ImVec2(px(24.0f), 0))) {
        config_.chainCollapsed = true;
        markDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fold the processing chain away");

    ImGui::SameLine();
    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted("Processing Chain");
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(96.0f));
    if (ImGui::Button("Add plugin", ImVec2(px(96.0f), 0))) {
        showPluginBrowser_ = true;
        pluginFilter_[0] = '\0';
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextWrapped("Signal flows top to bottom. Drag to reorder.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const ImGuiStyle& listStyle = ImGui::GetStyle();

    // Height derived from what a row holds rather than written down. Two lines
    // of widgets plus the child's own padding is what it is; a hard-coded 62
    // was a few pixels short at this font size, and the shortfall showed up as
    // a scrollbar on every entry that could travel almost nowhere.
    const float rowHeight = ImGui::GetFrameHeight() * 2.0f + listStyle.ItemSpacing.y +
                            listStyle.WindowPadding.y * 2.0f;
    const float rowPitch = rowHeight + listStyle.ItemSpacing.y;

    // Tall enough for the entries, never taller. Sizing it to the remaining
    // space put a scrollbar beside a list that already fitted.
    // Half a row of headroom, which is exactly how far a grabbed entry can
    // travel out of its slot. Without it the bottom entry, dragged downwards,
    // would overflow the list and summon the scrollbar for the length of the
    // gesture. Empty and invisible the rest of the time.
    const float contentHeight = rowPitch * static_cast<float>(chainNodes_.size()) +
                                rowPitch * 0.5f;
    const float available     = ImGui::GetContentRegionAvail().y;
    const float listHeight    = inspectedPlugin_
                                    ? std::min(contentHeight, available * 0.5f)
                                    : std::min(contentHeight, available);

    ImGui::BeginChild("##chainList", ImVec2(0, listHeight), ImGuiChildFlags_None);

    size_t moveFrom = SIZE_MAX, moveTo = SIZE_MAX;
    size_t removeIndex = SIZE_MAX;

    const float listTopY = ImGui::GetCursorScreenPos().y;
    const bool  dragging = dragNodeId_ != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (dragNodeId_ != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        dragNodeId_ = 0;

    for (size_t i = 0; i < chainNodes_.size(); ++i) {
        auto& node = chainNodes_[i];
        ImGui::PushID(static_cast<int>(node->id()));

        const bool bypassed = node->isBypassed();
        const bool isPlugin = node->kind() == dsp::NodeKind::Vst3Plugin;
        const bool isDragged = dragging && node->id() == dragNodeId_;

        // The grabbed row leaves the grid and follows the pointer. The offset
        // is bounded to roughly one row because the list reorders as soon as
        // the pointer passes the halfway mark, so the row is never far from the
        // slot it belongs in - which is what keeps it from being drawn over the
        // entries submitted after it.
        const float slotTop = ImGui::GetCursorPosY();
        if (isDragged) {
            // Expressed as a displacement from where the row would sit, rather
            // than converting a screen position into window coordinates. The
            // two spaces differ by padding and scroll, and getting that
            // conversion subtly wrong is a bug that only appears once the list
            // is long enough to scroll.
            const float wantedScreenY  = ImGui::GetIO().MousePos.y - dragGrabOffsetY_;
            const float naturalScreenY = listTopY + static_cast<float>(i) * rowPitch;
            // Half a row, not a whole one. The list reorders as the pointer
            // passes each halfway mark, so a larger travel is unreachable
            // anywhere except the two ends - where it would only push the row
            // outside the list and bring back the scrollbar.
            const float displacement = std::clamp(wantedScreenY - naturalScreenY,
                                                  -rowPitch * 0.5f, rowPitch * 0.5f);

            ImGui::SetCursorPosY(slotTop + displacement);
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::toVec4(theme::kPanelRaised));
        if (isDragged)
            ImGui::PushStyleColor(ImGuiCol_Border, theme::toVec4(theme::kAccent));
        ImGui::BeginChild("##item", ImVec2(0, rowHeight), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // The name is a Selectable purely so it can be grabbed. It was plain
        // text, and plain text is not an item ImGui tracks - IsItemActive is
        // never true for it, so reordering could not fire at all and the hint
        // above the list was describing something that did not exist.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::toVec4(bypassed ? theme::kTextFaint : theme::kText));
        ImGui::Selectable(node->name().c_str(), isDragged,
                          ImGuiSelectableFlags_AllowOverlap);
        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && !dragging)
            ImGui::SetTooltip("Drag up or down to move this in the chain");

        // Grabbing records where within the row the pointer landed, so the row
        // stays put under it rather than snapping its top to the cursor.
        if (dragNodeId_ == 0 && ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            dragNodeId_      = node->id();
            dragGrabOffsetY_ = ImGui::GetIO().MousePos.y - ImGui::GetWindowPos().y;
        }

        // The trailing controls are right-aligned against a width measured from
        // the widgets themselves rather than a constant.
        //
        // The previous fixed offset was wrong twice over: it was subtracted
        // from GetContentRegionAvail, which after SameLine is the space *left*
        // rather than the row's width, and it assumed one set of buttons when a
        // plugin row carries two more than a built-in one. Both errors put the
        // switch on top of the label.
        const ImGuiStyle& style = ImGui::GetStyle();

        auto smallButtonWidth = [&style](const char* text) {
            return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f;
        };

        // Matches the geometry inside toggleSwitch.
        float trailing = ImGui::GetFrameHeight() * 0.75f * 1.9f;
        if (isPlugin) {
            trailing += style.ItemSpacing.x + smallButtonWidth("UI");
            trailing += style.ItemSpacing.x + smallButtonWidth("Params");
            trailing += style.ItemSpacing.x + smallButtonWidth("X");
        }

        // The right edge in window-local coordinates, which is what
        // SetCursorPosX speaks. Taken while the cursor is still at the start of
        // the row, because GetContentRegionAvail measures from wherever the
        // cursor happens to be.
        const float labelLeft = ImGui::GetCursorPosX();
        const float rowRight  = labelLeft + ImGui::GetContentRegionAvail().x;
        const float labelRoom = rowRight - labelLeft - trailing - style.ItemSpacing.x;

        const char* subtitle = isPlugin
            ? static_cast<host::Vst3Plugin*>(node.get())->descriptor().vendor.c_str()
            : "built-in";

        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        textFitted(subtitle, labelRoom);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        // Never left of where the label ended: on a very narrow panel the
        // controls run out of the row rather than sitting on top of the text.
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowRight - trailing));

        // The switch reads as "enabled", not "bypassed": on and lit means the
        // module is doing something. The inverse - a lit switch meaning the
        // module is switched out of the path - is backwards from how every
        // other control in the application behaves.
        bool enabled = !bypassed;
        if (toggleSwitch("enable", &enabled)) {
            node->setBypassed(!enabled);
            markDirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(enabled ? "active - click to bypass" : "bypassed");

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

        // Only a plugin can be taken out. The built-in modules are the signal
        // path itself; removing one leaves a state the user has to know to
        // recover from, while switching it off does the same to the sound, in
        // one click, without losing where it sat in the order. That order is
        // the whole reason it matters - an EQ after a plugin is a different
        // sound from an EQ before it.
        if (isPlugin) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kDanger));
            if (ImGui::SmallButton("X"))
                removeIndex = i;
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(isDragged ? 2 : 1);

        // Only the displaced row needs putting back. Rewriting the cursor after
        // every entry meant trusting rowPitch to match ImGui's own advance
        // exactly, and any disagreement would accumulate down the list.
        if (isDragged)
            ImGui::SetCursorPosY(slotTop + rowPitch);

        // Where the pointer is decides the position, not how far it has
        // travelled. A threshold on the distance is what made this jump: it
        // fired once, reset, and needed another full push to fire again, so the
        // row lagged the cursor and then caught up in a leap.
        if (isDragged) {
            const float centreY = ImGui::GetIO().MousePos.y - dragGrabOffsetY_ +
                                  rowHeight * 0.5f;
            const float slot    = (centreY - listTopY) / rowPitch;

            auto target = static_cast<size_t>(std::clamp(
                static_cast<int>(std::floor(slot)), 0,
                static_cast<int>(chainNodes_.size()) - 1));

            if (target != i) {
                moveFrom = i;
                moveTo   = target;
            }
        }

        ImGui::PopID();
    }

    if (chainNodes_.empty())
        ImGui::TextDisabled("the chain is empty");

    // Moving the cursor is not enough to tell a window how tall its contents
    // are - only submitting an item does that, and the row above may have left
    // the cursor past the last one it drew. A zero-sized item is the documented
    // way to say "the content really does reach this far".
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    ImGui::EndChild();

    if (moveFrom != SIZE_MAX)
        moveNode(moveFrom, moveTo);
    if (removeIndex != SIZE_MAX)
        removeNode(removeIndex);

    // No "add the built-ins back" controls: they cannot leave the chain any
    // more, so there is nothing to add. buildChainFromConfig appends any that
    // an older configuration is missing.

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

    // Held locally for the rest of the frame. The close button clears the
    // member, and everything below dereferences it - reading it again after the
    // click is a null dereference, which is a crash to the desktop rather than
    // anything the user could connect to having closed a panel.
    host::Vst3Plugin* plugin = inspectedPlugin_;

    ImGui::PushFont(fonts_.medium, 0.0f);
    ImGui::TextUnformatted(plugin->name().c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - px(20.0f));
    if (ImGui::SmallButton("x")) {
        inspectedPlugin_ = nullptr;
        return;
    }

    ImGui::BeginChild("##params", ImVec2(0, 0), ImGuiChildFlags_None);

    const auto& parameters = plugin->parameters();
    if (parameters.empty())
        ImGui::TextDisabled("this plugin exposes no parameters");

    for (const auto& info : parameters) {
        ImGui::PushID(static_cast<int>(info.id));

        float value = static_cast<float>(plugin->parameterValue(info.id));
        const std::string display = plugin->parameterDisplay(info.id);

        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextUnformatted(info.title.c_str());
        ImGui::PopStyleColor();
        rightLabel(display.c_str(), theme::kText);

        if (info.isReadOnly)
            ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##value", &value, 0.0f, 1.0f, "")) {
            plugin->setParameterValue(info.id, value);
            markDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            plugin->setParameterValue(info.id, info.defaultNormalized);
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

void App::renderOptionsMenu()
{
    // The log first: it is the one item here anyone opens twice, and the only
    // one that ever has something to say. The count is spelled out because the
    // cog can only turn amber - it cannot say how amber.
    const int problems = log::problemCount();
    char logLabel[64];
    if (problems > 0) {
        std::snprintf(logLabel, sizeof(logLabel), "Log - %d problem%s", problems,
                      problems == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kWarning));
    } else {
        std::snprintf(logLabel, sizeof(logLabel), "Log");
    }
    if (ImGui::MenuItem(logLabel, nullptr, showLog_))
        showLog_ = !showLog_;
    if (problems > 0)
        ImGui::PopStyleColor();

    // The one line of this menu that is an action rather than a setting, so it
    // is only here while there is something to act on. The same decision can be
    // reached from the section at the bottom; this is the version of it that
    // does not need reading first.
    if (updateReady()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kAccent));
        char installLabel[64];
        std::snprintf(installLabel, sizeof(installLabel), "Install update %s...",
                      updateVersion().c_str());
        if (ImGui::MenuItem(installLabel))
            requestUpdateInstall();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Closes RadioVoice, installs, and starts it again.\n"
                              "Windows will ask for permission.");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted("NOTIFICATION AREA");
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (ImGui::Checkbox("Show an icon in the notification area", &config_.trayEnabled))
        markDirty();

    ImGui::BeginDisabled(!config_.trayEnabled);

    if (ImGui::Checkbox("Minimising hides the window", &config_.minimizeToTray))
        markDirty();

    if (ImGui::Checkbox("Closing hides the window", &config_.closeToTray))
        markDirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The window's X then puts RadioVoice away instead of\n"
                          "shutting it down. Quit from the tray icon's menu.");

    if (ImGui::Checkbox("Start with no window", &config_.startMinimized))
        markDirty();

    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Read from the registry rather than mirrored into the configuration: the
    // installer writes the same entry, and a copy here would go stale the first
    // time the two disagreed.
    bool startWithWindows = autostart::enabled();
    if (ImGui::Checkbox("Start with Windows", &startWithWindows))
        autostart::setEnabled(startWithWindows);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A per-user entry under Run, pointing at this executable\n"
                          "with --minimized. The installer's option writes the same\n"
                          "one, so the two cannot drift apart.");
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextWrapped("Hiding the window changes nothing about the audio path - "
                       "the microphone carries on being processed.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderUpdateSection();
}

void App::renderUpdateSection()
{
    const auto update = updater_.status();

    const auto tinted = [](ImU32 colour, const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(colour));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };

    // The release notes are the CHANGELOG section for that version, which is
    // written for exactly this moment - someone deciding whether to take it.
    const auto notes = [&] {
        if (update.notes.empty())
            return;

        ImGui::Spacing();
        // Sized rather than stretched: this sits in a popup that fits itself to
        // its contents, and a child asking for "whatever is available" in one of
        // those is a width defined in terms of itself.
        ImGui::BeginChild("##notes", ImVec2(px(300.0f), px(150.0f)),
                          ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextWrapped("%s", update.notes.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
    };

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted("VERSION");
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::Text("RadioVoice %s", RV_VERSION);
    ImGui::Spacing();

    switch (update.state) {
        case Updater::State::Idle:
            tinted(theme::kTextFaint, "No check has run yet.");
            break;

        case Updater::State::Checking:
            tinted(theme::kTextDim, "Asking GitHub...");
            break;

        case Updater::State::UpToDate:
            tinted(theme::kSignal, "This is the newest release.");
            break;

        case Updater::State::Available:
            ImGui::Text("Version %s has been released.", update.version.c_str());
            notes();
            ImGui::Spacing();
            if (update.downloadable) {
                if (ImGui::Button("Download it", ImVec2(px(150.0f), 0)))
                    updater_.download();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Fetches the installer and keeps it until you "
                                      "ask for it.\nNothing is installed yet.");
                }
            } else {
                tinted(theme::kTextFaint,
                       "That release carries no installer, so there is nothing to "
                       "fetch from here.");
            }
            break;

        case Updater::State::Downloading:
            ImGui::Text("Downloading version %s...", update.version.c_str());
            ImGui::Spacing();
            // A negative fraction is ImGui's indeterminate bar, which is what a
            // server that sent no Content-Length leaves us with.
            ImGui::ProgressBar(update.progress >= 0.0f
                                   ? update.progress
                                   : static_cast<float>(-1.0 * ImGui::GetTime()),
                               ImVec2(px(300.0f), 0.0f));
            break;

        case Updater::State::Ready:
            ImGui::Text("Version %s is ready to install.", update.version.c_str());
            notes();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, theme::toVec4(theme::kAccentDim));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::toVec4(theme::kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::toVec4(theme::kAccentFaint));
            if (ImGui::Button("Install and restart", ImVec2(px(180.0f), 0)))
                requestUpdateInstall();
            ImGui::PopStyleColor(3);

            ImGui::Spacing();
            tinted(theme::kTextFaint,
                   "RadioVoice closes, Windows asks whether to let the installer "
                   "run, and it starts again when the installer is done. Devices, "
                   "chain and every setting are kept.");
            break;

        case Updater::State::Failed:
            tinted(theme::kWarning, "The last attempt did not get there:");
            tinted(theme::kTextDim, update.error.c_str());
            break;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Check for updates automatically", &config_.checkForUpdates)) {
        updater_.setAutomatic(config_.checkForUpdates);
        markDirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Once a day, RadioVoice asks GitHub which release is the\n"
                          "newest. That is one request and nothing else - the\n"
                          "installer is only fetched when you ask for it, and only\n"
                          "installed when you say so.");
    }

    const bool busy = update.state == Updater::State::Checking ||
                      update.state == Updater::State::Downloading;

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Check now", ImVec2(px(110.0f), 0)))
        updater_.checkNow();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Releases", ImVec2(px(110.0f), 0))) {
        ::ShellExecuteW(nullptr, L"open", toWide(kReleasesUrl).c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens the release list on GitHub.");
}

void App::renderTransportBar()
{
    ImGui::BeginChild("##transport", ImVec2(0, 0), ImGuiChildFlags_Borders);

    auto meterColumn = [&](const char* title, float peak, float rms,
                           std::atomic<float>& gainDb, const char* gainId) {
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        levelMeter(title, dsp::gainToDb(peak), dsp::gainToDb(rms), ImVec2(px(240.0f), px(14.0f)), true);

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
    if (ImGui::Button(mute ? "MUTED" : "Mute", ImVec2(px(90.0f), px(30.0f)))) {
        params_.mute.store(!mute);
        markDirty();
    }
    if (mute)
        ImGui::PopStyleColor();

    ImGui::SameLine();

    bool bypass = params_.bypassAll.load();
    if (bypass)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::toVec4(theme::kWarning));
    if (ImGui::Button(bypass ? "BYPASSED" : "Bypass all", ImVec2(px(110.0f), px(30.0f)))) {
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

void App::renderScanFolders()
{
    if (!ImGui::CollapsingHeader("Scan folders"))
        return;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
    ImGui::TextWrapped(
        "The standard VST3 locations are always searched. Add folders here for "
        "plugins kept elsewhere - another drive, a portable collection, a "
        "network share.");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // The built-in locations are shown, not just implied. Someone whose plugins
    // are missing needs to know where the scanner already looked before being
    // asked to add somewhere else.
    for (const auto& dir : paths::defaultVst3Directories()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", toUtf8(dir.wstring()).c_str());
        ImGui::PopStyleColor();
    }

    auto extras = scanner_.extraDirectories();
    bool changed = false;

    for (size_t i = 0; i < extras.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::SmallButton("x")) {
            extras.erase(extras.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop searching this folder");

        ImGui::SameLine();

        // A folder that has gone away - an unplugged drive, a share that is
        // down - is worth flagging rather than quietly scanning nothing.
        const bool exists = std::filesystem::exists(std::filesystem::path(toWide(extras[i])));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              theme::toVec4(exists ? theme::kText : theme::kWarning));
        ImGui::TextWrapped("%s", extras[i].c_str());
        ImGui::PopStyleColor();
        if (!exists && ImGui::IsItemHovered())
            ImGui::SetTooltip("This folder is not reachable right now");

        ImGui::PopID();
    }

    ImGui::Spacing();

    if (ImGui::Button("Add folder...")) {
        const auto chosen = paths::pickDirectory(mainWindow_, L"Folder with VST3 plugins");
        if (!chosen.empty()) {
            const std::string text = toUtf8(chosen.wstring());
            if (std::find(extras.begin(), extras.end(), text) == extras.end()) {
                extras.push_back(text);
                changed = true;
            }
        }
    }

    if (changed) {
        scanner_.setExtraDirectories(extras);

        // Scanned straight away rather than waiting for a Rescan click. Adding
        // a folder is only ever asked for because something in it is wanted
        // now, and a list that stays empty until a second, undiscovered button
        // is pressed reads as the feature not working.
        scanner_.startScan({}, /*full=*/false);
    }
}

void App::renderPluginBrowser()
{
    ImGui::SetNextWindowSize(ImVec2(px(680.0f), px(520.0f)), ImGuiCond_FirstUseEver);
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

    renderScanFolders();

    ImGui::Separator();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "search by name or vendor",
                             pluginFilter_, sizeof(pluginFilter_));

    ImGui::BeginChild("##list", ImVec2(0, -px(60.0f)), ImGuiChildFlags_Borders);

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
    ImGui::SetNextWindowSize(ImVec2(px(760.0f), px(420.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", &showLog_)) {
        ImGui::End();
        return;
    }

    const auto entries = log::snapshot();

    // Formatting one line, shared by what is drawn and what is copied, so the
    // text on the clipboard is the text that was on screen.
    auto formatLine = [](const log::Entry& entry) {
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "[%8.3f] ", entry.timeSeconds);
        return std::string(prefix) + entry.text;
    };

    if (ImGui::Button("Copy all")) {
        std::string all;
        all.reserve(entries.size() * 80);
        for (const auto& entry : entries) {
            all += formatLine(entry);
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
    }

    ImGui::SameLine();
    if (ImGui::Button("Copy problems")) {
        std::string problems;
        for (const auto& entry : entries) {
            if (entry.level >= log::Level::Warning) {
                problems += formatLine(entry);
                problems += '\n';
            }
        }
        ImGui::SetClipboardText(problems.empty() ? "(no warnings or errors)"
                                                 : problems.c_str());
    }

    ImGui::SameLine();
    ImGui::TextDisabled("click a line to copy it");

    ImGui::Separator();

    ImGui::PushFont(fonts_.small, 0.0f);
    ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];

        ImU32 colour = theme::kTextDim;
        switch (entry.level) {
            case log::Level::Debug:   colour = theme::kTextFaint; break;
            case log::Level::Info:    colour = theme::kTextDim;   break;
            case log::Level::Warning: colour = theme::kWarning;   break;
            case log::Level::Error:   colour = theme::kDanger;    break;
        }

        const std::string line = formatLine(entry);

        // A Selectable rather than plain text: ImGui text is not selectable, so
        // without this the one thing anyone wants from a log - pasting it
        // somewhere - is impossible.
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(colour));
        if (ImGui::Selectable(line.c_str()))
            ImGui::SetClipboardText(entry.text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // Follow the tail unless the user has scrolled up to read something.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopFont();

    ImGui::End();
}

} // namespace rv::app
