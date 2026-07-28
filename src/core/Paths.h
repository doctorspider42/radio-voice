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

/// Where a downloaded installer waits until the user asks for it, created on
/// demand.
///
/// Under %APPDATA% rather than the temporary directory: the file survives a
/// cleanup tool running between the download and the click that installs it,
/// and it sits beside the log that would explain a failed update.
std::filesystem::path updatesDir();

/// Standard VST3 search locations, in the order the specification lists them:
/// per-user first, then machine-wide.
std::vector<std::filesystem::path> defaultVst3Directories();

/// Shows the system folder picker. Returns an empty path when cancelled.
///
/// The shell dialog rather than a hand-rolled tree: it already knows about
/// mapped drives, network locations, junctions and the places a user actually
/// keeps things, and typing a path by hand is how a trailing space becomes a
/// folder that silently contains no plugins.
std::filesystem::path pickDirectory(void* ownerWindow, const wchar_t* title);

} // namespace rv::paths
