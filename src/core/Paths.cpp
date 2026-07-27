#include "core/Paths.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace rv::paths {
namespace {

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &raw)))
        return {};

    fs::path p(raw);
    ::CoTaskMemFree(raw);
    return p;
}

} // namespace

const fs::path& dataDir()
{
    // Resolved once: the folder is fixed for the process lifetime and creating
    // it repeatedly would touch the disk on every accessor call.
    static const fs::path dir = [] {
        fs::path base = knownFolder(FOLDERID_RoamingAppData);
        if (base.empty())
            base = fs::temp_directory_path();

        fs::path d = base / L"RadioVoice";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

fs::path configFile()      { return dataDir() / L"config.json"; }
fs::path pluginCacheFile() { return dataDir() / L"plugins.json"; }
fs::path logFile()         { return dataDir() / L"radiovoice.log"; }

fs::path presetsDir()
{
    fs::path d = dataDir() / L"presets";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::vector<fs::path> defaultVst3Directories()
{
    std::vector<fs::path> out;

    const fs::path localApp = knownFolder(FOLDERID_LocalAppData);
    if (!localApp.empty())
        out.push_back(localApp / L"Programs" / L"Common" / L"VST3");

    const fs::path commonFiles = knownFolder(FOLDERID_ProgramFilesCommon);
    if (!commonFiles.empty())
        out.push_back(commonFiles / L"VST3");

    // 32-bit plugins cannot be loaded into this 64-bit process, but the folder
    // is listed so the UI can explain why nothing was found there.
    const fs::path commonFiles32 = knownFolder(FOLDERID_ProgramFilesCommonX86);
    if (!commonFiles32.empty() && commonFiles32 != commonFiles)
        out.push_back(commonFiles32 / L"VST3");

    std::error_code ec;
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const fs::path& p) { return !fs::is_directory(p, ec); }),
              out.end());
    return out;
}

} // namespace rv::paths
