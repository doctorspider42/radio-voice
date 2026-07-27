#pragma once

#include <filesystem>
#include <vector>

namespace rv::paths {

/// %APPDATA%\RadioVoice, created on first call.
const std::filesystem::path& dataDir();

std::filesystem::path configFile();      ///< config.json
std::filesystem::path pluginCacheFile(); ///< plugins.json - the VST3 scan cache
std::filesystem::path presetsDir();      ///< user presets, created on demand
std::filesystem::path logFile();

/// Standard VST3 search locations, in the order the specification lists them:
/// per-user first, then machine-wide.
std::vector<std::filesystem::path> defaultVst3Directories();

} // namespace rv::paths
