#include "host/vst3/WindowsPluginProbe.h"

#ifdef _WIN32

#include <windows.h>
#include <objbase.h>

#include <fstream>
#include <memory>

#include <json.hpp>

#include "host/vst3/Vst3Plugin.h"

namespace rv::host {
namespace {

constexpr DWORD kProbeTimeoutMs = 10000;

struct CloseHandle {
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;

struct TempFile {
    std::filesystem::path path;
    ~TempFile()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

} // namespace

std::string probePlugin(const PluginDescriptor& plugin, dsp::Chain::Format format,
                        const std::string& state)
{
    wchar_t tempDir[MAX_PATH]{};
    wchar_t tempPath[MAX_PATH]{};
    const DWORD dirLength = ::GetTempPathW(MAX_PATH, tempDir);
    if (!dirLength || dirLength >= MAX_PATH || !::GetTempFileNameW(tempDir, L"rvp", 0, tempPath))
        return "Could not create a temporary VST3 probe file.";
    const TempFile request{tempPath};
    {
        std::ofstream file(request.path, std::ios::binary | std::ios::trunc);
        const nlohmann::json data{
            {"path", plugin.path}, {"uid", plugin.uid}, {"name", plugin.name}, {"state", state},
            {"sampleRate", format.sampleRate}, {"maxFrames", format.maxFrames}, {"channels", format.channels},
        };
        file << data;
        file.close();
        if (!file)
            return "Could not write the VST3 probe request.";
    }

    std::wstring executable(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        return "Could not locate the VST3 probe executable.";
    executable.resize(length);

    Handle job(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return "Could not create the VST3 probe job.";

    std::wstring command = L"\"" + executable + L"\" --probe-vst3 \"" + request.path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        return "Could not start the VST3 probe.";
    Handle process(child.hProcess);
    Handle thread(child.hThread);
    if (!::AssignProcessToJobObject(job.get(), process.get()) || ::ResumeThread(thread.get()) == DWORD(-1)) {
        ::TerminateProcess(process.get(), 1);
        return "Could not run the VST3 probe.";
    }

    const DWORD waited = ::WaitForSingleObject(process.get(), kProbeTimeoutMs);
    if (waited == WAIT_TIMEOUT) {
        ::TerminateJobObject(job.get(), 1);
        ::WaitForSingleObject(process.get(), 1000);
        return "The plugin did not respond within 10 seconds.";
    }
    DWORD exitCode = 0;
    if (waited != WAIT_OBJECT_0 || !::GetExitCodeProcess(process.get(), &exitCode))
        return "Could not get the VST3 probe result.";
    if (exitCode == 0)
        return {};
    if (exitCode == 2) {
        std::ifstream file(request.path, std::ios::binary);
        std::string error;
        if (std::getline(file, error) && !error.empty())
            return error;
    }
    return "The plugin exited unexpectedly during initialization.";
}

int runPluginProbe(const std::filesystem::path& request)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const HRESULT comStatus = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comStatus))
        return 1;

    std::string error;
    try {
        const auto data = [&] {
            std::ifstream file(request, std::ios::binary);
            return nlohmann::json::parse(file);
        }();
        PluginDescriptor descriptor;
        descriptor.path = data.at("path").get<std::string>();
        descriptor.uid = data.at("uid").get<std::string>();
        descriptor.name = data.at("name").get<std::string>();
        auto plugin = Vst3Plugin::create(descriptor, data.at("sampleRate").get<double>(),
                                         data.at("maxFrames").get<int>(), data.at("channels").get<int>(), error);
        const auto state = data.at("state").get<std::string>();
        if (!plugin && error.empty())
            error = "The plugin could not be loaded.";
        if (plugin && !state.empty() && !plugin->loadState(state))
            error = "The plugin could not restore its saved settings.";
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "The plugin raised an unexpected error.";
    }
    ::CoUninitialize();

    if (error.empty())
        return 0;
    std::ofstream file(request, std::ios::binary | std::ios::trunc);
    file << error;
    file.close();
    return file ? 2 : 1;
}

} // namespace rv::host

#endif
