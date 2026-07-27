#include "host/PluginScanner.h"

#include <windows.h>

#include <objbase.h> // CoInitializeEx: plugin DLLs routinely use COM on load

#include <algorithm>
#include <fstream>

#include <json.hpp>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"

#if RV_HAS_VST3
#include "pluginterfaces/vst/ivstaudioprocessor.h" // kVstAudioEffectClass
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/utility/uid.h"
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace rv::host {
namespace {

constexpr int kCacheVersion = 1;

/// VST3 on Windows is a folder ending in .vst3 containing
/// Contents/x86_64-win/<name>.vst3, though a bare DLL named *.vst3 is still
/// seen in the wild. `Module::create` accepts either, so both are offered to it.
bool looksLikeBundle(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".vst3";
}

} // namespace

PluginScanner::PluginScanner() = default;

PluginScanner::~PluginScanner()
{
    cancelScan();
    if (thread_.joinable())
        thread_.join();
}

fs::path PluginScanner::sentinelPath() const
{
    return paths::dataDir() / L"scan-in-progress.txt";
}

void PluginScanner::writeSentinel(const std::string& path) const
{
    std::ofstream file(sentinelPath(), std::ios::trunc);
    if (file) {
        file << path;
        // Flushed immediately: the whole point is that this survives a crash
        // that happens microseconds later inside the plugin's DllMain.
        file.flush();
    }
}

void PluginScanner::clearSentinel() const
{
    std::error_code ec;
    fs::remove(sentinelPath(), ec);
}

std::string PluginScanner::currentItem() const
{
    std::lock_guard lock(mutex_);
    return currentItem_;
}

std::vector<PluginDescriptor> PluginScanner::plugins() const
{
    std::lock_guard lock(mutex_);
    return plugins_;
}

std::vector<BlacklistedPlugin> PluginScanner::blacklist() const
{
    std::lock_guard lock(mutex_);
    return blacklist_;
}

void PluginScanner::clearBlacklist()
{
    {
        std::lock_guard lock(mutex_);
        blacklist_.clear();
    }
    saveCache();
}

std::vector<std::string> PluginScanner::extraDirectories() const
{
    std::lock_guard lock(mutex_);
    return extraDirectories_;
}

void PluginScanner::setExtraDirectories(std::vector<std::string> directories)
{
    {
        std::lock_guard lock(mutex_);
        extraDirectories_ = std::move(directories);
    }
    saveCache();
}

void PluginScanner::loadCache()
{
    // A leftover sentinel means the previous run died while probing that
    // bundle. Blacklisting it is the only way to get back to a startable state.
    {
        std::ifstream sentinel(sentinelPath());
        if (sentinel) {
            std::string crashed;
            std::getline(sentinel, crashed);
            sentinel.close();
            clearSentinel();

            if (!crashed.empty()) {
                std::lock_guard lock(mutex_);
                blacklist_.push_back({crashed,
                                      "the previous scan crashed while loading this bundle"});
                RV_WARN("blacklisted \"%s\": it crashed the previous scan", crashed.c_str());
            }
        }
    }

    std::ifstream file(paths::pluginCacheFile());
    if (!file)
        return;

    json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        RV_WARN("plugin cache is unreadable (%s); it will be rebuilt", e.what());
        return;
    }

    if (root.value("version", 0) != kCacheVersion) {
        RV_INFO("plugin cache is from an older version; rescanning");
        return;
    }

    std::lock_guard lock(mutex_);

    for (const auto& entry : root.value("plugins", json::array())) {
        PluginDescriptor descriptor;
        descriptor.path          = entry.value("path", "");
        descriptor.uid           = entry.value("uid", "");
        descriptor.name          = entry.value("name", "");
        descriptor.vendor        = entry.value("vendor", "");
        descriptor.category      = entry.value("category", "");
        descriptor.subCategories = entry.value("subCategories", "");
        descriptor.version       = entry.value("version", "");
        descriptor.sdkVersion    = entry.value("sdkVersion", "");
        if (!descriptor.path.empty() && !descriptor.uid.empty())
            plugins_.push_back(std::move(descriptor));
    }

    for (const auto& entry : root.value("blacklist", json::array())) {
        blacklist_.push_back({entry.value("path", ""), entry.value("reason", "")});
    }

    for (const auto& entry : root.value("extraDirectories", json::array())) {
        if (entry.is_string())
            extraDirectories_.push_back(entry.get<std::string>());
    }

    RV_INFO("plugin cache loaded: %zu plugins, %zu blacklisted",
            plugins_.size(), blacklist_.size());
}

void PluginScanner::saveCache() const
{
    json root;
    root["version"] = kCacheVersion;

    {
        std::lock_guard lock(mutex_);

        json plugins = json::array();
        for (const auto& descriptor : plugins_) {
            plugins.push_back({{"path", descriptor.path},
                               {"uid", descriptor.uid},
                               {"name", descriptor.name},
                               {"vendor", descriptor.vendor},
                               {"category", descriptor.category},
                               {"subCategories", descriptor.subCategories},
                               {"version", descriptor.version},
                               {"sdkVersion", descriptor.sdkVersion}});
        }
        root["plugins"] = std::move(plugins);

        json blacklist = json::array();
        for (const auto& entry : blacklist_)
            blacklist.push_back({{"path", entry.path}, {"reason", entry.reason}});
        root["blacklist"] = std::move(blacklist);

        root["extraDirectories"] = extraDirectories_;
    }

    std::ofstream file(paths::pluginCacheFile(), std::ios::trunc);
    if (file)
        file << root.dump(2);
}

std::vector<fs::path> PluginScanner::findBundles(const std::vector<fs::path>& directories)
{
    std::vector<fs::path> found;
    std::error_code ec;

    for (const auto& directory : directories) {
        if (!fs::is_directory(directory, ec))
            continue;

        // A .vst3 bundle is itself a directory, so recursion must not descend
        // into one once it has been identified.
        fs::recursive_directory_iterator it(
            directory, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (looksLikeBundle(it->path())) {
                found.push_back(it->path());
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
            }
        }
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

void PluginScanner::startScan(std::vector<fs::path> directories, bool full)
{
    if (scanning_.load(std::memory_order_acquire))
        return;

    if (thread_.joinable())
        thread_.join();

    if (directories.empty()) {
        directories = paths::defaultVst3Directories();
        std::lock_guard lock(mutex_);
        for (const auto& extra : extraDirectories_)
            directories.push_back(fs::path(toWide(extra)));
    }

    cancel_.store(false, std::memory_order_release);
    scanning_.store(true, std::memory_order_release);
    progress_.store(0.0f, std::memory_order_relaxed);

    thread_ = std::thread(&PluginScanner::scanThread, this, std::move(directories), full);
}

void PluginScanner::cancelScan()
{
    cancel_.store(true, std::memory_order_release);
}

void PluginScanner::scanThread(std::vector<fs::path> directories, bool full)
{
    // Plugin DLLs routinely use COM in DllMain and in their factory.
    const HRESULT comStatus = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comStatus);

    const auto bundles = findBundles(directories);
    RV_INFO("scanning %zu VST3 bundle(s)", bundles.size());

    if (full) {
        std::lock_guard lock(mutex_);
        plugins_.clear();
    }

    for (size_t i = 0; i < bundles.size(); ++i) {
        if (cancel_.load(std::memory_order_acquire))
            break;

        progress_.store(static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, bundles.size())),
                        std::memory_order_relaxed);

        const std::string path = toUtf8(bundles[i].wstring());

        {
            std::lock_guard lock(mutex_);
            currentItem_ = path;

            const bool blacklisted =
                std::any_of(blacklist_.begin(), blacklist_.end(),
                            [&](const BlacklistedPlugin& b) { return b.path == path; });
            if (blacklisted)
                continue;

            if (!full) {
                const bool known =
                    std::any_of(plugins_.begin(), plugins_.end(),
                                [&](const PluginDescriptor& d) { return d.path == path; });
                if (known)
                    continue;
            }
        }

        probeBundle(bundles[i]);
    }

    progress_.store(1.0f, std::memory_order_relaxed);

    {
        std::lock_guard lock(mutex_);
        currentItem_.clear();
        std::sort(plugins_.begin(), plugins_.end(),
                  [](const PluginDescriptor& a, const PluginDescriptor& b) {
                      if (a.vendor != b.vendor)
                          return a.vendor < b.vendor;
                      return a.name < b.name;
                  });
    }

    saveCache();
    scanning_.store(false, std::memory_order_release);

    if (comOwned)
        ::CoUninitialize();
}

#if RV_HAS_VST3

void PluginScanner::probeBundle(const fs::path& bundle)
{
    const std::string path = toUtf8(bundle.wstring());

    writeSentinel(path);

    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);

    if (!module) {
        clearSentinel();
        std::lock_guard lock(mutex_);
        blacklist_.push_back({path, error.empty() ? "the bundle could not be loaded" : error});
        RV_WARN("VST3 \"%s\" failed to load: %s", path.c_str(), error.c_str());
        return;
    }

    std::vector<PluginDescriptor> found;

    // The factory holds a reference counted pointer *into the plugin DLL*. It
    // has to be destroyed before the module is, or its destructor calls
    // Release() on code that has already been unmapped. Scoping it here makes
    // that ordering explicit rather than relying on declaration order.
    {
    const auto factory = module->getFactory();

    // Many plugins leave the per-class vendor empty and only fill it in on the
    // factory, so the factory value is the fallback rather than showing blanks.
    const std::string factoryVendor = factory.info().vendor();

    for (const auto& classInfo : factory.classInfos()) {
        // Only audio processor classes are instantiable as chain nodes; the
        // controller classes a bundle also exports are created via the
        // component, never directly.
        if (classInfo.category() != kVstAudioEffectClass)
            continue;

        PluginDescriptor descriptor;
        descriptor.path          = path;
        descriptor.uid           = classInfo.ID().toString();
        descriptor.name          = classInfo.name();
        descriptor.vendor        = classInfo.vendor().empty() ? factoryVendor
                                                              : classInfo.vendor();
        descriptor.category      = classInfo.category();
        descriptor.subCategories = classInfo.subCategoriesString();
        descriptor.version       = classInfo.version();
        descriptor.sdkVersion    = classInfo.sdkVersion();

        // Instruments have no audio input to process, so they cannot sit in a
        // microphone chain. Recorded rather than hidden, so the UI can say why.
        if (icontains(descriptor.subCategories, "Instrument"))
            descriptor.skipReason = "instrument, not an effect";

        found.push_back(std::move(descriptor));
    }
    } // factory released here, while the module is still loaded

    // Nothing is kept loaded after a scan.
    module.reset();
    clearSentinel();

    std::lock_guard lock(mutex_);
    for (auto& descriptor : found) {
        const auto existing =
            std::find_if(plugins_.begin(), plugins_.end(),
                         [&](const PluginDescriptor& d) { return d.key() == descriptor.key(); });
        if (existing != plugins_.end())
            *existing = std::move(descriptor);
        else
            plugins_.push_back(std::move(descriptor));
    }
}

#else // !RV_HAS_VST3

void PluginScanner::probeBundle(const fs::path&)
{
    // Without the SDK there is nothing to probe; bundles are still enumerated
    // so the UI can report how many were found and explain why none are usable.
}

#endif

} // namespace rv::host
