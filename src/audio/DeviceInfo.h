#pragma once

#include <string>
#include <vector>

#include "core/Types.h"

namespace rv::audio {

struct DeviceInfo {
    DeviceId    id;                 ///< Backend-specific, stable across restarts.
    std::string name;               ///< Friendly name for the UI.
    BackendType backend = BackendType::Wasapi;

    bool isInput   = false;
    bool isOutput  = false;
    bool isDefault = false;

    int maxInputChannels  = 0;
    int maxOutputChannels = 0;

    /// The device's own preferred rate. For WASAPI shared mode this is the
    /// mixer rate; for exclusive and ASIO it is what the hardware is set to.
    int defaultSampleRate = 48000;

    /// Best-effort list of rates the device accepts. Empty means "unknown",
    /// which the UI presents as "let the driver decide".
    std::vector<int> supportedSampleRates;

    /// Set for endpoints whose name matches a known loopback/virtual-cable
    /// driver. The UI promotes these as output candidates because routing to
    /// one is what makes the processed signal selectable in other applications.
    bool isVirtualCable = false;

    /// Human-readable reason the device cannot be used, empty when usable.
    std::string unavailableReason;

    bool usable() const { return unavailableReason.empty(); }
};

/// True when the endpoint name matches a virtual audio cable driver.
/// Matching on name is unavoidable: none of these drivers expose a
/// distinguishing property, and every one of them is identified by its
/// endpoint name everywhere else in the ecosystem too.
bool looksLikeVirtualCable(const std::string& deviceName);

} // namespace rv::audio
