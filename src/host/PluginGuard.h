#pragma once

#include <optional>
#include <string>

namespace rv::host::guard {

/// What the application was doing inside a plugin when it stopped.
enum class Stage {
    Load,   ///< Instantiating the plugin and restoring its settings.
    Editor, ///< Creating or attaching the plugin's own editor.
};

/// A plugin call the previous session never came back from.
struct Incident {
    std::string uid;
    std::string name;
    Stage       stage = Stage::Editor;
};

/// Marks the calling thread as inside `name` for as long as the scope lives.
///
/// Two things watch the mark. A file in the data directory records it, so that
/// when a plugin crashes the process outright or the user has to kill a hung
/// window, the next launch knows which plugin to keep away from (see
/// `takeLeftover`). And on Windows a watchdog thread notices a mark that has
/// been held too long and offers to restart RadioVoice without that plugin.
///
/// Only meant for calls on the thread that draws the window - the ones that
/// freeze everything when they do not return. Scopes do not nest.
class Scope {
public:
    Scope(const std::string& uid, const std::string& name, Stage stage);
    ~Scope();

    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
};

/// The incident left behind by the previous session, if it died inside a
/// plugin call. The record is removed as it is read.
std::optional<Incident> takeLeftover();

/// Starts the thread that notices a plugin call that does not return. Does
/// nothing outside Windows, or when it is already running.
void startWatchdog();

/// Waits for the process `pid` to exit, for at most `timeoutMs`. Used by the
/// instance the watchdog starts, so it does not find its predecessor still
/// holding the single-instance lock.
void waitForProcess(unsigned long pid, unsigned long timeoutMs);

} // namespace rv::host::guard
