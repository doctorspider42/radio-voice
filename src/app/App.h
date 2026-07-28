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
    bool initialize(void* mainWindow, const gui::Fonts& fonts, float dpiScale,
                    Config loaded);

    /// Called when the window moves to a display of a different scale. The
    /// fonts have already been rebuilt by the platform layer; this adopts them
    /// along with the factor every hard-coded dimension is multiplied by.
    void setDpiScale(float dpiScale, const gui::Fonts& fonts);
    void shutdown();

    /// Draws one frame into the current ImGui context.
    void render();

    bool wantsExit() const { return wantsExit_; }

    /// Window size to persist, refreshed by the platform layer.
    void setWindowSize(int width, int height);

    // --- notification area ---------------------------------------------------
    //
    // The icon and its menu belong to the platform layer, which is the only
    // place allowed to touch an HWND. These are what it needs to build the menu
    // and act on what was chosen.
    //
    // All of it is called between frames rather than during one: the menu is
    // modal, so the render loop is parked while it is up.

    bool trayEnabled() const { return config_.trayEnabled; }
    bool minimizeToTray() const { return config_.trayEnabled && config_.minimizeToTray; }
    bool closeToTray() const { return config_.trayEnabled && config_.closeToTray; }
    bool startMinimized() const { return config_.startMinimized; }

    bool isMuted() const { return params_.mute.load(); }
    void setMuted(bool muted) { params_.mute.store(muted); markDirty(); }

    bool isEngineRunning() const { return engine_ && engine_->isRunning(); }
    void toggleEngine();

    void requestExit() { wantsExit_ = true; }

    /// One line for the tray tooltip.
    std::string trayTooltip() const;

    /// Writes the configuration out immediately instead of at the next frame.
    /// A window hidden to the tray does not render, so the periodic flush that
    /// `render` performs never comes round; anything changed from the menu
    /// would otherwise be lost on a hard shutdown.
    void saveConfigNow();

private:
    // --- panels -----------------------------------------------------------
    void renderTopBar();
    void renderIoPanel();
    void renderVirtualCableHint();
    void renderProcessingPanel();
    void renderDenoisePanel();
    void renderGatePanel();
    void renderCompressorPanel();
    void renderLimiterPanel();
    void renderChainPanel();
    void renderTransportBar();
    void renderPluginBrowser();

    /// Where the scanner looks: the built-in locations, and whatever the user
    /// added on top of them.
    void renderScanFolders();

    void renderPluginParameters();
    void renderLogWindow();

    /// Contents of the popup behind the cog: the log, and where the window
    /// goes when it is dismissed.
    void renderOptionsMenu();

    // --- device helpers ---------------------------------------------------
    /// Picks a monitor device when none is chosen yet, preferring the system
    /// default and never a virtual cable - monitoring into another cable is
    /// silence, which is the problem this feature exists to solve.
    void ensureMonitorDevice();

    /// Applies a changed monitor selection to the running engine. Called every
    /// frame; does nothing until something actually moved.
    void syncMonitor();

    /// Restarts the engine once a changed device selection has settled.
    void applyPendingDeviceChange();

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

    /// Converts a dimension written in the code into device pixels.
    ///
    /// Every literal in the layout is expressed at 100% scale, which is only
    /// the right number on one class of display. ImGui scales its own padding
    /// and rasterises the fonts larger, so without this the text grows and the
    /// panels holding it do not - which is not a cosmetic problem but an
    /// overlapping one.
    float px(float logical) const { return logical * dpiScale_; }

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

    /// The monitor selection the engine has already been told about. Compared
    /// against the live config each frame to notice a change without needing
    /// every widget that can cause one to remember to report it.
    DeviceConfig appliedMonitor_;
    bool         appliedMonitorEnabled_ = false;

    /// When the device selection first diverged from what is running, in
    /// ImGui's clock. Negative when it matches.
    ///
    /// The delay before restarting is not politeness. Changing a backend picks
    /// a device for you, and the user's own choice lands a moment later -
    /// restarting on the first change would tear the engine down twice and open
    /// a device nobody asked for in between.
    double deviceChangeSeenAt_ = -1.0;

    void*      mainWindow_ = nullptr;
    gui::Fonts fonts_{};
    float      dpiScale_ = 1.0f;

    bool wantsExit_          = false;
    bool showPluginBrowser_  = false;
    bool showLog_            = false;
    bool configDirty_        = false;
    double lastSaveTime_     = 0.0;

    /// Node whose parameter list is open in the side panel, or null.
    host::Vst3Plugin* inspectedPlugin_ = nullptr;

    /// Chain entry being dragged, by node id, or zero when nothing is.
    ///
    /// Tracked here rather than through ImGui's active-item state because the
    /// list reorders underneath the pointer while the drag is in progress, and
    /// the grab has to survive that.
    u64 dragNodeId_ = 0;
    /// Distance from the top of the grabbed row to where it was grabbed, so the
    /// row keeps its position under the pointer instead of jumping.
    float dragGrabOffsetY_ = 0.0f;

    char pluginFilter_[128] = {};

    /// Set when the engine last failed to start, shown until the next attempt.
    std::string startupError_;
};

} // namespace rv::app
