#include "app/App.h"

#include "core/Platform.h"

#include <algorithm>
#include <cctype>
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

#ifdef __APPLE__
constexpr const char* kVirtualCableUrl = "https://existential.audio/blackhole/";
#else
constexpr const char* kVirtualCableUrl = "https://vb-audio.com/Cable/";
#endif
constexpr const char* kReleasesUrl =
    "https://github.com/doctorspider42/radio-voice/releases";

/// Configuration is written at most this often while knobs are moving; the
/// pending state is always flushed on exit.
constexpr double kSaveIntervalSeconds = 3.0;

/// Width of gui::toggleSwitch, for right-aligning one on a row.
float switchWidth() { return ImGui::GetFrameHeight() * 0.72f * 1.85f; }

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
    gui::setWidgetFonts(fonts_);
}

bool App::initialize(void* mainWindow, const gui::Fonts& fonts, float dpiScale,
                     Config loaded, bool allowAutoStart)
{
    mainWindow_ = mainWindow;
    fonts_      = fonts;
    dpiScale_   = dpiScale > 0.0f ? dpiScale : 1.0f;
    gui::setWidgetFonts(fonts_);

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
        const auto inputs = devices_.inputs(kDefaultBackend);
        for (const auto& device : inputs) {
            if (device.isDefault) {
                config_.input.backend    = kDefaultBackend;
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

    // The compressor is where most of the listening happens, so the console
    // opens on it rather than on whichever module happens to be first.
    for (const auto& node : chainNodes_) {
        if (node->kind() == dsp::NodeKind::Compressor)
            selectedUnit_ = node->id();
    }

    if (allowAutoStart && config_.autoStart && !config_.output.deviceId.empty())
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

        // Only a plugin's switch is stored with the chain entry. A built-in
        // module's lives in the parameter block, which is what both its panel
        // and the chain list now read - and honouring a `bypassed` written by a
        // build that kept the two apart would restore the copy the user cannot
        // see, over the one they can.
        if (node->kind() == dsp::NodeKind::Vst3Plugin)
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
        if (embeddedPlugin_ == plugin)
            embeddedPlugin_ = nullptr;
        if (editorOpening_ == plugin)
            editorOpening_ = nullptr;
        if (embedFailed_ == plugin)
            embedFailed_ = nullptr;
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
// Devices
// ---------------------------------------------------------------------------

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

void App::recoverInvalidatedDevice()
{
    if (!engine_ || !engine_->deviceRecoveryNeeded()) {
        deviceRecoverySeenAt_ = -1.0;
        return;
    }

    const double now = ImGui::GetTime();
    if (deviceRecoverySeenAt_ < 0.0) {
        deviceRecoverySeenAt_ = now;
        return;
    }

    // The old endpoint has already gone, but Windows may need a moment to
    // create its replacement. Retrying immediately would only reopen the same
    // stale id and turn a recoverable PnP transition into a startup error.
    constexpr double kEndpointSettleSeconds = 1.0;
    if (now - deviceRecoverySeenAt_ < kEndpointSettleSeconds)
        return;

    deviceRecoverySeenAt_ = -1.0;
    devices_.refresh();
    RV_INFO("audio endpoint was rebuilt; reopening the processing streams");
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

    if (label && *label) {
        std::string caps = label;
        for (char& c : caps)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        capsLabel(caps.c_str(), theme::kTextDim);
        ImGui::Spacing();
    }

    // --- backend ----------------------------------------------------------
    // A segmented control rather than a list: there are never more than three,
    // and seeing all of them at once is what tells someone ASIO is an option.
#ifdef __APPLE__
    BackendType backends[] = {BackendType::CoreAudio};
    const char* backendLabels[] = {"COREAUDIO"};
    const int backendCount = 1;
#else
    BackendType backends[3] = {BackendType::Wasapi, BackendType::DirectSound,
                               BackendType::Asio};
    const char* backendLabels[3] = {"WASAPI", "DIRECTSOUND", "ASIO"};
    const int   backendCount     = allowAsio ? 3 : 2;

#endif

    int current = 0;
    for (int i = 0; i < backendCount; ++i) {
        if (backends[i] == device.backend)
            current = i;
    }

    if (segmented("##backend", backendLabels, backendCount, &current)) {
        {
            const BackendType backend = backends[current];
            {
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
    }

    ImGui::Spacing();

    // --- device -----------------------------------------------------------
    const auto list = isInput ? devices_.inputs(device.backend)
                              : devices_.outputs(device.backend);

    // Every device on its own row rather than inside a drop-down. This screen
    // exists to choose one, there is room to show them all, and a closed list
    // hides exactly the comparison the choice depends on.
    if (list.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("no devices for this backend");
        ImGui::PopStyleColor();
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rowHeight = ImGui::GetFrameHeight() * 1.55f;

    for (const auto& info : list) {
        ImGui::PushID(info.id.c_str());

        const bool selected = info.id == device.deviceId;
        const bool usable   = info.usable();

        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + rowHeight);

        if (!usable)
            ImGui::BeginDisabled();
        if (ImGui::InvisibleButton("##row", ImVec2(rowMax.x - rowMin.x, rowHeight)) && !selected) {
            device.deviceId   = info.id;
            device.deviceName = info.name;
            if (info.defaultSampleRate > 0)
                device.sampleRate = info.defaultSampleRate;
            markDirty();
        }
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (!usable) {
            ImGui::EndDisabled();
            if (hovered)
                ImGui::SetTooltip("%s", info.unavailableReason.c_str());
        }

        draw->AddRectFilled(rowMin, rowMax,
                            selected  ? theme::withAlpha(theme::kAccent, 0.07f)
                            : hovered ? theme::kControl
                                      : theme::kPanelRaised,
                            9.0f);
        draw->AddRect(rowMin, rowMax,
                      selected ? theme::withAlpha(theme::kAccent, 0.55f) : theme::kBorder, 9.0f);

        const float  radio = rowHeight * 0.16f;
        const ImVec2 dot(rowMin.x + rowHeight * 0.42f, rowMin.y + rowHeight * 0.5f);
        draw->AddCircle(dot, radio, selected ? theme::kAccent : theme::kBorderBright, 20, 2.0f);
        if (selected)
            draw->AddCircleFilled(dot, radio * 0.45f, theme::kAccent, 12);

        const float textX = dot.x + rowHeight * 0.42f;
        const float lineH = ImGui::GetTextLineHeight();
        draw->PushClipRect(rowMin, ImVec2(rowMax.x - rowHeight * 2.2f, rowMax.y), true);
        draw->AddText(ImVec2(textX, rowMin.y + rowHeight * 0.5f - lineH * 0.95f),
                      usable ? theme::kText : theme::kTextFaint, info.name.c_str());
        draw->PopClipRect();

        char meta[64];
        std::snprintf(meta, sizeof(meta), "%d Hz%s", info.defaultSampleRate,
                      info.isDefault ? " - default" : "");
        ImGui::PushFont(fonts_.small, fonts_.small->LegacySize);
        draw->AddText(ImVec2(textX, rowMin.y + rowHeight * 0.5f + lineH * 0.05f),
                      theme::kTextFaint, info.defaultSampleRate > 0 ? meta : (info.isDefault ? "default" : ""));
        ImGui::PopFont();

        if (info.isVirtualCable) {
            ImGui::PushFont(fonts_.caps, fonts_.caps->LegacySize);
            const char* tag = "VIRTUAL CABLE";
            const float width = spacedTextWidth(tag, 1.2f) + 16.0f;
            const ImVec2 tagMin(rowMax.x - width - 10.0f, rowMin.y + rowHeight * 0.5f - 10.0f);
            const ImVec2 tagMax(rowMax.x - 10.0f, rowMin.y + rowHeight * 0.5f + 10.0f);
            draw->AddRectFilled(tagMin, tagMax, theme::withAlpha(theme::kAccent, 0.12f), 10.0f);
            drawSpacedText(draw, ImVec2(tagMin.x + 8.0f, tagMin.y + (20.0f - fonts_.caps->LegacySize) * 0.5f),
                           theme::kAccentLight, tag, 1.2f);
            ImGui::PopFont();
        }

        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
    }

    ImGui::Spacing();

    // --- mode and rate ----------------------------------------------------
    if (device.backend == BackendType::Wasapi) {
        bool exclusive = device.wasapiMode == WasapiMode::Exclusive;
        ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextFaint));
        ImGui::TextUnformatted("Exclusive mode");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - switchWidth());
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

#ifdef __APPLE__
    capsLabel("SAMPLE RATE");
    const auto selected = devices_.find(device.deviceId, device.backend);
    ImGui::Text("%d Hz", selected ? selected->defaultSampleRate : device.sampleRate);
    ImGui::TextWrapped("Change the device rate in Audio MIDI Setup. Use 48000 Hz on the output for noise suppression.");
#else
    const int rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    char rateText[32];
    std::snprintf(rateText, sizeof(rateText), "%d Hz", device.sampleRate);

    capsLabel("SAMPLE RATE");
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

#endif

    ImGui::PopID();
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
#ifdef __APPLE__
        "Install BlackHole 2ch, then choose BlackHole as the output here and "
        "as the microphone in Discord, OBS, Teams or your receiving application. "
        "Use Monitor to hear the processed signal through headphones."
#else
        "Windows has no built-in way for one application to expose an audio "
        "stream as a microphone to another - that needs a driver. Install a "
        "virtual cable, then set the output above to \"CABLE Input\" and choose "
        "\"CABLE Output\" as the microphone in Discord, OBS, Teams and so on."
#endif
    );
    ImGui::PopStyleColor();

#ifdef __APPLE__
    if (ImGui::SmallButton("Get BlackHole 2ch")) {
#else
    if (ImGui::SmallButton("Open the VB-CABLE download page")) {
#endif
        platform::openUrl(kVirtualCableUrl);
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

    // Held locally for the rest of the frame: removing the plugin clears the
    // member, and everything below dereferences it.
    host::Vst3Plugin* plugin = inspectedPlugin_;

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
// Options
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
#ifdef __APPLE__
    bool startAtLogin = autostart::enabled();
    if (ImGui::Checkbox("Start at login", &startAtLogin))
        autostart::setEnabled(startAtLogin);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Manage RadioVoice in System Settings > General > Login Items.");
#else
    bool startWithWindows = autostart::enabled();
    if (ImGui::Checkbox("Start with Windows", &startWithWindows))
        autostart::setEnabled(startWithWindows);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A per-user entry under Run, pointing at this executable\n"
                          "with --minimized. The installer's option writes the same\n"
                          "one, so the two cannot drift apart.");
    }

#endif

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
#ifdef __APPLE__
    ImGui::TextWrapped("macOS preview builds are updated manually.");
    if (ImGui::SmallButton("Open releases"))
        platform::openUrl(kReleasesUrl);
    return;
#endif

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

    // "UPDATES" rather than "VERSION": the heading is what someone scans for,
    // and nobody opens this menu looking for a version number - they open it
    // looking for the switch that governs the checking.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::toVec4(theme::kTextDim));
    ImGui::TextUnformatted("UPDATES");
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
        platform::openUrl(kReleasesUrl);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens the release list on GitHub.");
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
        ImGui::TextWrapped("%s", pathToUtf8(dir).c_str());
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
        const bool exists = std::filesystem::exists(pathFromUtf8(extras[i]));
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
            const std::string text = pathToUtf8(chosen);
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
    if (ImGui::Button("Clear log"))
        logClearFailed_ = !log::clear();

    ImGui::SameLine();
    ImGui::TextDisabled("click a line to copy it");

    if (logClearFailed_)
        ImGui::TextColored(theme::toVec4(theme::kDanger),
                           "The log was cleared from memory, but the file could not be reset.");

    ImGui::Separator();

    ImGui::PushFont(fonts_.small, fonts_.small->LegacySize);
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
