#pragma once

#include <string>
#include <vector>

namespace rv::log {

enum class Level { Debug, Info, Warning, Error };

struct Entry {
    Level       level;
    double      timeSeconds; ///< Since process start.
    std::string text;
};

/// Opens the log file. Safe to call more than once; later calls are ignored.
void init();
void shutdown();

/// printf-style. Thread-safe, but takes a mutex and touches the filesystem -
/// never call it from an audio callback. Audio threads report problems by
/// bumping counters that the UI thread drains instead.
void write(Level level, const char* fmt, ...);

/// Copy of the in-memory tail, newest last, for the log panel in the UI.
std::vector<Entry> snapshot();

/// Clears the in-memory tail and starts a new on-disk log. Returns false when
/// the log file could not be reopened; logging then continues in memory.
bool clear();

/// Number of entries at Warning or above since start, so the UI can badge the
/// log button without copying the whole buffer every frame.
int problemCount();

} // namespace rv::log

#define RV_DEBUG(...) ::rv::log::write(::rv::log::Level::Debug,   __VA_ARGS__)
#define RV_INFO(...)  ::rv::log::write(::rv::log::Level::Info,    __VA_ARGS__)
#define RV_WARN(...)  ::rv::log::write(::rv::log::Level::Warning, __VA_ARGS__)
#define RV_ERROR(...) ::rv::log::write(::rv::log::Level::Error,   __VA_ARGS__)
