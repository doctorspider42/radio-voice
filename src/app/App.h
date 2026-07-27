#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/Config.h"
#include "audio/DeviceEnumerator.h"
#include "audio/Engine.h"
#include "core/Params.h"
#include "gui/Theme.h"
#include "host/PluginScanner.h"
#include "host/vst3/Vst3Plugin.h"

namespace rv::app {

/// Application state and the whole user interface.
///
/// Everything the UI touches lives here: the parameter block the audio thread
/// reads, the engine, the device list, the plugin scanner and the saved
/// configuration. `render` is called once per frame from the platform layer and
/// owns no windowing details of its own.
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// `mainWindow` is the HWND plugin editors are parented to. `loaded` is the
    /// configuration the platform layer already read to size the window, passed
    /// in rather than read again so the file is parsed exactly once.
    bool initialize(void* mainWindow, const gui::Fonts& fonts, Config loaded);
    void shutdown();

    /// Draws one frame into the current ImGui context.
    void render();

    bool wantsExit() const { return wantsExit_; }

    /// Window size to persist, refreshed by the platform layer.
    void setWindowSize(int width, int height);

private:
    // --- panels -----------------------------------------------------------
    void renderTopBar();
    void renderIoPanel();
    void renderVirtualCableHint();
    void renderProcessingPanel();
    void renderGatePanel();
    void renderCompressorPanel();
    void renderLimiterPanel();
    void renderChainPanel();
    void renderTransportBar();
    void renderPluginBrowser();
    void renderPluginParameters();
    void renderLogWindow();

    // --- device helpers ---------------------------------------------------
    /// `allowAsio` is false for the monitor: an ASIO driver opens its device
    /// exclusively, so offering it there would let the user pick a backend that
    /// can only contend with the main path.
    void renderDeviceSelector(const char* label, DeviceConfig& device, bool isInput,
                              bool allowAsio = true);
    bool deviceSelectionDiffers() const;

    // --- engine -----------------------------------------------------------
    void startEngine();
    void stopEngine();
    void restartEngine();

    // --- chain ------------------------------------------------------------
    void buildChainFromConfig();
    void publishChain();
    void addPlugin(const host::PluginDescriptor& descriptor);
    void removeNode(size_t index);
    void moveNode(size_t from, size_t to);
    void captureChainToConfig();

    double currentSampleRate() const;

    void markDirty();
    void saveIfDirty();

    // --- state ------------------------------------------------------------
    Params  params_;
    Meters  meters_;
    Config  config_;

    std::unique_ptr<audio::Engine> engine_;
    audio::DeviceEnumerator        devices_;
    host::PluginScanner            scanner_;

    /// UI-side mirror of the chain, in order. The engine holds borrowed
    /// pointers to the same nodes.
    std::vector<dsp::NodePtr> chainNodes_;

    /// The configuration that is actually running, so the UI can tell the user
    /// when a pending change needs a restart.
    Config runningConfig_;

    void*      mainWindow_ = nullptr;
    gui::Fonts fonts_{};

    bool wantsExit_          = false;
    bool showPluginBrowser_  = false;
    bool showLog_            = false;
    bool configDirty_        = false;
    double lastSaveTime_     = 0.0;

    /// Node whose parameter list is open in the side panel, or null.
    host::Vst3Plugin* inspectedPlugin_ = nullptr;

    char pluginFilter_[128] = {};

    /// Set when the engine last failed to start, shown until the next attempt.
    std::string startupError_;
};

} // namespace rv::app
