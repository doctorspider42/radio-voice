#pragma once

#include <string>
#include <vector>

#include "audio/Engine.h"
#include "core/Params.h"

namespace rv::app {

/// One entry of the saved processing chain.
///
/// Built-in modules are identified by kind alone; a plugin additionally carries
/// the bundle path, the class UID and its opaque state, so a saved setup comes
/// back with every knob where it was left.
struct ChainEntryConfig {
    std::string kind;     ///< "gate", "equalizer" or "vst3"
    bool        bypassed = false;

    std::string pluginPath;
    std::string pluginUid;
    std::string pluginName;  ///< Only for the error message when it is missing.
    std::string pluginState; ///< Base64.
};

struct DeviceConfig {
    BackendType backend    = BackendType::Wasapi;
    WasapiMode  wasapiMode = WasapiMode::Shared;
    DeviceId    deviceId;
    std::string deviceName; ///< Remembered so the UI can say what went missing.
    int         sampleRate   = 48000;
    int         channels     = 2;
    int         bufferFrames = 0;
};

struct Config {
    DeviceConfig input;
    DeviceConfig output;

    /// Second render device carrying the same signal, so the operator can hear
    /// what is being sent into a virtual cable - which is otherwise silent by
    /// construction.
    DeviceConfig monitor;
    bool         monitorEnabled = false;

    int  internalChannels = 2;
    int  maxBlockFrames   = 256;
    bool autoStart        = true;

    std::vector<ChainEntryConfig> chain;

    int windowWidth  = 1360;
    int windowHeight = 840;

    /// Notification-area behaviour.
    ///
    /// The processor is useful with nothing on screen: it sits between the
    /// microphone and whatever is listening, and once it is set up there is
    /// rarely anything to look at. So the window being gone and the engine
    /// being stopped are deliberately separate events - closing the window with
    /// `closeToTray` on leaves the audio path exactly where it was.
    bool trayEnabled    = true;
    bool minimizeToTray = true;
    bool closeToTray    = false;
    /// Start with no window shown. Set by the installer's "start with Windows"
    /// shortcut, which passes --minimized, and settable from the tray menu.
    bool startMinimized = false;

    /// Whether the side panels are folded away to a narrow strip.
    bool chainCollapsed = false;
    bool ioCollapsed    = false;

    /// Values of every knob, stored flat.
    struct ParamValues {
        float inputGainDb  = 0.0f;
        float outputGainDb = 0.0f;
        bool  mute      = false;
        bool  bypassAll = false;

        float monitorGainDb = 0.0f;
        bool  monitorMute   = false;

        bool  denoiseEnabled = true;
        float denoiseAmount  = 0.9f;

        bool  hpfEnabled = true;
        float hpfHz      = 80.0f;
        bool  lpfEnabled = false;
        float lpfHz      = 16000.0f;

        bool  eqEnabled = true;
        float eqGainDb[kEqBands] = {};
        float eqQ[kEqBands]      = {};

        bool  gateEnabled       = true;
        float gateThresholdDb   = -45.0f;
        float gateHysteresisDb  = 6.0f;
        float gateRangeDb       = -60.0f;
        float gateAttackMs      = 2.0f;
        float gateHoldMs        = 120.0f;
        float gateReleaseMs     = 180.0f;
        float gateLookaheadMs   = 3.0f;
        float gateSidechainHpfHz = 120.0f;

        bool  compEnabled        = true;
        float compThresholdDb    = -18.0f;
        float compRatio          = 3.0f;
        float compKneeDb         = 6.0f;
        float compAttackMs       = 8.0f;
        float compReleaseMs      = 120.0f;
        float compMakeupDb       = 0.0f;
        bool  compAutoMakeup     = true;
        float compLookaheadMs    = 2.0f;
        float compSidechainHpfHz = 100.0f;
        bool  compRmsDetection   = true;

        int   inputMix   = 0;   ///< InputMix
        bool  monoOutput = false;

        bool  limiterEnabled   = true;
        float limiterCeilingDb = -1.0f;
        float limiterReleaseMs = 80.0f;
    } params;

    /// Reads config.json. Missing or unreadable files yield defaults, which is
    /// exactly what a first run needs.
    static Config load();
    void save() const;

    /// Copies the stored values into the live parameter block, and back.
    void applyTo(Params& target) const;
    void captureFrom(const Params& source);

    audio::EngineConfig toEngineConfig() const;
};

} // namespace rv::app
