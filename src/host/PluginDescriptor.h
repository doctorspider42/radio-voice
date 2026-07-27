#pragma once

#include <string>
#include <vector>

namespace rv::host {

/// One instantiable audio class inside a .vst3 bundle.
///
/// A bundle can export several classes (an effect and its companion
/// controller, or a whole product family), so a descriptor identifies a class
/// by UID, not just the file it lives in.
struct PluginDescriptor {
    std::string path;          ///< Absolute path of the .vst3 bundle.
    std::string uid;           ///< Class UID, canonical VST3 string form.
    std::string name;
    std::string vendor;
    std::string category;      ///< "Audio Module Class" for processors.
    std::string subCategories; ///< e.g. "Fx|Dynamics"
    std::string version;
    std::string sdkVersion;

    /// Populated when the class was rejected, e.g. an instrument rather than
    /// an effect. Kept so the UI can explain the omission.
    std::string skipReason;

    bool isEffect() const { return skipReason.empty(); }

    /// Identity used by saved configurations. The bundle path alone is not
    /// enough (multiple classes) and the UID alone is not enough (the same
    /// plugin may be installed twice).
    std::string key() const { return path + "|" + uid; }
};

/// A bundle that failed to load or crashed the scanner, so it is not retried.
struct BlacklistedPlugin {
    std::string path;
    std::string reason;
};

} // namespace rv::host
