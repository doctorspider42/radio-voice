#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host/PluginDescriptor.h"

namespace rv::host {

/// Discovers VST3 bundles and remembers what it found.
///
/// Scanning means loading third-party code into this process, and a
/// badly-behaved plugin can take the whole application down while it is being
/// probed. Guarding against that properly needs an out-of-process scanner;
/// short of that, this class writes the bundle it is about to touch to a
/// sentinel file and clears it afterwards. If the file still exists at the next
/// start, that bundle was what crashed, and it is blacklisted instead of being
/// tried again - so one hostile plugin cannot make the application unstartable.
class PluginScanner {
public:
    PluginScanner();
    ~PluginScanner();

    PluginScanner(const PluginScanner&) = delete;
    PluginScanner& operator=(const PluginScanner&) = delete;

    /// Reads the cached results and converts any leftover sentinel into a
    /// blacklist entry. Call once at start-up.
    void loadCache();
    void saveCache() const;

    /// Kicks off a background scan. `directories` empty means the standard
    /// VST3 locations. `full` re-probes bundles already in the cache.
    void startScan(std::vector<std::filesystem::path> directories, bool full);
    void cancelScan();

    bool  isScanning() const { return scanning_.load(std::memory_order_acquire); }
    float progress() const { return progress_.load(std::memory_order_relaxed); }
    std::string currentItem() const;

    /// Descriptors of everything usable that was found.
    std::vector<PluginDescriptor> plugins() const;

    std::vector<BlacklistedPlugin> blacklist() const;
    void clearBlacklist();

    /// Additional folders the user added, persisted with the cache.
    std::vector<std::string> extraDirectories() const;
    void setExtraDirectories(std::vector<std::string> directories);

private:
    void scanThread(std::vector<std::filesystem::path> directories, bool full);
    void probeBundle(const std::filesystem::path& bundle);

    static std::vector<std::filesystem::path> findBundles(
        const std::vector<std::filesystem::path>& directories);

    std::filesystem::path sentinelPath() const;
    void writeSentinel(const std::string& path) const;
    void clearSentinel() const;

    mutable std::mutex             mutex_;
    std::vector<PluginDescriptor>  plugins_;
    std::vector<BlacklistedPlugin> blacklist_;
    std::vector<std::string>       extraDirectories_;
    std::string                    currentItem_;

    std::thread       thread_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<float> progress_{0.0f};
};

} // namespace rv::host
