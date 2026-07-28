#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace rv {

/// Watches the project's GitHub releases, and fetches the installer when asked.
///
/// One background thread does all of it. A check is a network round trip and a
/// download is several megabytes of one; neither belongs on the thread drawing
/// the interface, and neither may block the audio path. The rest of the
/// application only ever reads `status()`, which hands back a copy taken under
/// the lock - there is no callback, so nothing that happens out here can reach
/// into a half-drawn frame.
///
/// The thread rather than a timer in the frame loop, because the interesting
/// case is the one where no frame is drawn at all: this is a tray application,
/// and the machine it is most useful on is the one where it has been running
/// hidden for a week.
///
/// What this class deliberately does not do is start the installer. Setup has
/// to replace an executable that is still running, so the launch happens in the
/// platform layer after the window and the engine are down - see
/// `App::requestUpdateInstall`.
class Updater {
public:
    enum class State {
        Idle,         ///< Nothing has been asked for yet.
        Checking,
        UpToDate,
        Available,    ///< A newer release exists; nothing downloaded yet.
        Downloading,
        Ready,        ///< The installer is on disk, and its digest matched.
        Failed,
    };

    struct Status {
        State state = State::Idle;

        /// Version of the newest release, once a check has succeeded. Set for
        /// `UpToDate` too, where it equals this build's own.
        std::string version;

        /// Its release notes, which are the CHANGELOG section for that version.
        std::string notes;

        /// Whether that release actually carries an installer. A release
        /// published while the installer job failed does not, and offering a
        /// download button for it would be a lie.
        bool downloadable = false;

        /// 0..1 while downloading; negative when the server did not say how
        /// large the file is, which a progress bar has to be told about rather
        /// than left to render as "no progress at all".
        float progress = 0.0f;

        /// Why the last attempt failed, phrased for someone who is not going to
        /// read the log.
        std::string error;

        /// The verified installer, once the state is `Ready`.
        std::filesystem::path installer;
    };

    Updater();
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    /// Starts the worker. `automatic` is the saved preference: with it off,
    /// nothing reaches the network until `checkNow` is called.
    void start(bool automatic);

    /// Follows the checkbox in the interface. Turning it on schedules a check
    /// shortly afterwards rather than immediately - the setting is usually
    /// changed while looking at the panel that would report the result.
    void setAutomatic(bool automatic);

    /// Asks for a check now. Ignored while the worker is busy with either a
    /// check or a download.
    void checkNow();

    /// Downloads the installer for the release the last check found. Ignored
    /// unless the state is `Available` and it has one.
    void download();

    Status status() const;

private:
    enum class Command { None, Check, Download };

    void run();
    void doCheck();
    void doDownload();

    /// The worker's own accessors, which is why they take the lock themselves.
    void setState(State state);
    void fail(std::string message);

    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    std::thread             worker_;

    Status  status_;
    Command command_ = Command::None;
    bool    automatic_ = false;
    bool    stopping_  = false;

    std::chrono::steady_clock::time_point nextCheck_{};

    /// What the release the last check found points at. Not in `Status`: the
    /// interface has no use for a URL, and a download in progress must not be
    /// retargeted by a check that lands underneath it.
    std::string assetUrl_;
    std::string assetName_;
    std::string assetDigest_; ///< "sha256:..." when GitHub published one.
    unsigned long long assetSize_ = 0;

    /// Read inside the transfer loop, so that quitting does not wait for a
    /// download to finish over a slow connection.
    std::atomic<bool> cancel_{false};
};

} // namespace rv
