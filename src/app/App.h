#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Config.h"
#include "audio/DeviceEnumerator.h"
#include "audio/Engine.h"
#include "core/Params.h"
#include "core/Updater.h"
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

    // --- updates -------------------------------------------------------------
    //
    // Also for the platform layer: the window may be hidden for days at a time,
    // which is exactly when an update is worth mentioning, and it is the layer
    // that owns the notification icon that can mention it.

    /// Whether a verified installer is sitting on disk, waiting to be run.
    bool updateReady() const;

    /// Version that installer carries, or empty.
    std::string updateVersion() const;

    /// Asks for the downloaded installer to be started once this process is
    /// down, and requests exit. Setup replaces the executable this application
    /// is running from, so it cannot be started from here.
    void requestUpdateInstall();

    /// What the platform layer should launch after everything is shut down, or
    /// empty when no update was asked for.
    const std::filesystem::path& pendingInstaller() const { return pendingInstaller_; }

    /// True once per newly downloaded version, so that the tray icon can say so
    /// exactly once. The window being on screen is not checked here - the caller
    /// knows whether the interface has already shown it.
    bool takeUpdateAnnouncement(std::string& version);

private:
    // --- shell ------------------------------------------------------------
    //
    // The window is a header, a body and a footer. The body is either the
    // console - the rack of modules on the left and the selected one opened up
    // on the right - or the device screen, which is set once and then left
    // alone and so does not deserve permanent room beside the sound.

    void renderHeader();
    void renderNotices();
    void renderRack(ImVec2 size);
    void renderModuleView(ImVec2 size);
    void renderFooter();
    void renderDevicesScreen();
    void renderVirtualCableHint();
    void renderPluginBrowser();

    // --- module views, one per kind of rack unit ----------------------------
    void renderSuppressorView(ImVec2 origin, ImVec2 size);
    void renderGateView(ImVec2 origin, ImVec2 size);
    void renderEqualizerView(ImVec2 origin, ImVec2 size);
    void renderCompressorView(ImVec2 origin, ImVec2 size);
    void renderLimiterView(ImVec2 origin, ImVec2 size);
    void renderPluginView(host::Vst3Plugin* plugin, ImVec2 origin, ImVec2 size);

    /// Samples the meters into the histories the displays scroll through.
    void sampleHistory();

    /// Hides an embedded plugin editor that was on screen last frame and is
    /// not this frame. A child window does not disappear on its own when the
    /// interface stops drawing the area it covers.
    void syncEmbeddedEditor();

    /// Where the scanner looks: the built-in locations, and whatever the user
    /// added on top of them.
    void renderScanFolders();

    void renderPluginParameters();
    void renderLogWindow();

    /// Contents of the popup behind the cog: the log, and where the window
    /// goes when it is dismissed.
    void renderOptionsMenu();

    /// The tail of that popup: what this build is, and everything to do with
    /// replacing it.
    void renderUpdateSection();

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

    /// Reopens the main streams after Windows rebuilds an endpoint beneath an
    /// active WASAPI client. This is particularly common just after a virtual
    /// cable driver first starts.
    void recoverInvalidatedDevice();

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
    Updater                        updater_;

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

    /// Time the current endpoint invalidation was first observed. A brief wait
    /// gives Plug and Play time to publish the replacement endpoint before the
    /// saved device id is rebound by name.
    double deviceRecoverySeenAt_ = -1.0;

    void*      mainWindow_ = nullptr;
    gui::Fonts fonts_{};
    float      dpiScale_ = 1.0f;

    bool wantsExit_          = false;
    bool showPluginBrowser_  = false;
    bool showLog_            = false;
    bool logClearFailed_     = false;
    bool configDirty_        = false;
    double lastSaveTime_     = 0.0;

    /// The device screen replaces the console while it is open.
    bool showDevices_ = false;

    /// Rack unit opened in the module view: a chain node's id, or
    /// kLimiterUnit for the output limiter, which sits outside the chain.
    static constexpr u64 kLimiterUnit = ~u64{0};
    u64 selectedUnit_ = 0;

    /// Equaliser band the side panel edits.
    int selectedBand_ = 3;

    /// A plugin's own editor is shown inside the module view unless the user
    /// asked for the generic parameter list instead.
    bool pluginShowsParameters_ = false;

    /// Plugin whose editor is embedded right now, and the one that asked to be
    /// this frame; see syncEmbeddedEditor.
    host::Vst3Plugin* embeddedPlugin_ = nullptr;
    host::Vst3Plugin* embeddedWanted_ = nullptr;

    /// Plugin whose editor is about to be created, and how many frames of the
    /// spinner have gone out for it; see renderPluginView.
    host::Vst3Plugin* editorOpening_ = nullptr;
    int               openingFrames_ = 0;

    /// Plugin whose editor could not be docked, so it is not retried every frame.
    host::Vst3Plugin* embedFailed_ = nullptr;

    /// Where each rack unit is drawn, by node id, easing towards its slot.
    std::unordered_map<u64, float> unitY_;

    /// What the scrolling displays draw: the last few seconds of the meters,
    /// sampled at a fixed rate so the scroll speed does not follow the frame
    /// rate.
    struct History {
        static constexpr int kLength = 180;
        float values[kLength];
        int   head = 0;

        /// Levels start at silence rather than at 0 dBFS, or the first six
        /// seconds would show a wall of full scale scrolling away.
        explicit History(float initial = 0.0f)
        {
            for (float& value : values)
                value = initial;
        }

        void push(float value)
        {
            values[head] = value;
            head = (head + 1) % kLength;
        }
        /// Oldest first: `at(0)` is six seconds ago, `at(kLength - 1)` is now.
        float at(int index) const { return values[(head + index) % kLength]; }
        float latest() const { return at(kLength - 1); }
    };

    History inputHistory_{-120.0f};  ///< dBFS, peak.
    History outputHistory_{-120.0f}; ///< dBFS, peak.
    History gateHistory_;      ///< Gain reduction, dB.
    History gateOpenHistory_;  ///< 1 when open.
    History compHistory_;      ///< Gain reduction, dB.
    History limiterHistory_;   ///< Gain reduction, dB.
    History speechHistory_;    ///< 0..1.
    double  lastHistorySample_ = 0.0;

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

    /// Installer to run after this process has exited; see `pendingInstaller`.
    std::filesystem::path pendingInstaller_;

    /// Version the notification icon has already announced, so that a balloon
    /// appears once per update rather than once per second.
    std::string announcedUpdate_;
};

} // namespace rv::app
