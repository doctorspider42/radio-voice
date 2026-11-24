#include "host/PluginGuard.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <json.hpp>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"

namespace rv::host::guard {
namespace {

namespace fs = std::filesystem;

/// How long a plugin call may hold the window before the user is asked.
constexpr auto kHangThreshold = std::chrono::seconds(10);

fs::path markerFile() { return paths::dataDir() / L"plugin-in-progress.json"; }

const char* stageName(Stage stage) { return stage == Stage::Load ? "load" : "editor"; }

/// The scope currently held, shared with the watchdog. `generation` changes on
/// every entry, so a prompt raised for one call is not acted on for the next.
std::mutex        g_mutex;
std::string       g_name;
Stage             g_stage = Stage::Editor;
std::atomic<long long> g_enteredAt{0}; ///< steady_clock ticks; 0 while outside.
std::atomic<unsigned>  g_generation{0};
std::atomic<bool>      g_watchdogRunning{false};

long long nowTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

#ifdef _WIN32
/// Starts a fresh copy that waits for this one to go, then ends this one. The
/// marker is deliberately left in place: it is how the new copy learns which
/// plugin to keep away from.
[[noreturn]] void restartWithoutPlugin()
{
    std::wstring executable(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length && length < executable.size()) {
        executable.resize(length);
        std::wstring command = L"\"" + executable + L"\" --after-hang " + std::to_wstring(::GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                             &startup, &child)) {
            ::CloseHandle(child.hThread);
            ::CloseHandle(child.hProcess);
        }
    }
    RV_ERROR("restarting after a plugin stopped responding");
    ::TerminateProcess(::GetCurrentProcess(), 3);
    for (;;) ::Sleep(INFINITE);
}

void watchdogThread()
{
    unsigned prompted = ~0u;
    for (;;) {
        ::Sleep(500);
        const long long entered = g_enteredAt.load();
        const unsigned  generation = g_generation.load();
        if (!entered || generation == prompted)
            continue;
        const auto held = std::chrono::steady_clock::duration(nowTicks() - entered);
        if (held < kHangThreshold)
            continue;

        prompted = generation;
        std::string name;
        Stage stage;
        {
            std::lock_guard lock(g_mutex);
            name  = g_name;
            stage = g_stage;
        }
        RV_ERROR("VST3 \"%s\" has not returned from its %s for %lld s", name.c_str(), stageName(stage),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(held).count()));

        const std::wstring wideName = toWide(name);
        const std::wstring text =
            (stage == Stage::Editor ? L"The editor of \"" + wideName + L"\" has stopped responding"
                                    : L"\"" + wideName + L"\" has stopped responding while loading") +
            L", and the RadioVoice window is waiting for it.\n\n"
            L"Restart RadioVoice now? It will come back with this plugin " +
            (stage == Stage::Editor ? std::wstring(L"showing its parameter list instead of its editor.")
                                    : std::wstring(L"switched off, so the rest of the chain keeps working.")) +
            L"\n\nChoose No to keep waiting.";
        const int answer = ::MessageBoxW(nullptr, text.c_str(), L"RadioVoice",
                                         MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);

        // The call may have come back while the question was on screen.
        if (answer == IDYES && g_enteredAt.load() && g_generation.load() == generation)
            restartWithoutPlugin();
    }
}
#endif

} // namespace

Scope::Scope(const std::string& uid, const std::string& name, Stage stage)
{
    {
        std::lock_guard lock(g_mutex);
        g_name  = name;
        g_stage = stage;
    }
    ++g_generation;
    g_enteredAt.store(nowTicks());

    std::ofstream file(markerFile(), std::ios::binary | std::ios::trunc);
    file << nlohmann::json{{"uid", uid}, {"name", name}, {"stage", stageName(stage)}}.dump();
}

Scope::~Scope()
{
    g_enteredAt.store(0);
    std::error_code error;
    fs::remove(markerFile(), error);
}

std::optional<Incident> takeLeftover()
{
    const fs::path path = markerFile();
    std::error_code error;
    if (!fs::exists(path, error))
        return std::nullopt;

    std::optional<Incident> incident;
    try {
        std::ifstream file(path, std::ios::binary);
        const auto data = nlohmann::json::parse(file);
        Incident found;
        found.uid   = data.value("uid", "");
        found.name  = data.value("name", "");
        found.stage = data.value("stage", "") == "load" ? Stage::Load : Stage::Editor;
        if (!found.uid.empty())
            incident = std::move(found);
    } catch (const std::exception&) {
        // A marker cut short by the crash itself names nothing usable.
    }
    fs::remove(path, error);
    return incident;
}

void startWatchdog()
{
#ifdef _WIN32
    if (g_watchdogRunning.exchange(true))
        return;
    try {
        std::thread(watchdogThread).detach();
    } catch (const std::exception& e) {
        g_watchdogRunning = false;
        RV_ERROR("could not start the plugin watchdog: %s", e.what());
    }
#endif
}

void waitForProcess(unsigned long pid, unsigned long timeoutMs)
{
#ifdef _WIN32
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return;
    ::WaitForSingleObject(process, timeoutMs);
    ::CloseHandle(process);
#else
    (void)pid;
    (void)timeoutMs;
#endif
}

} // namespace rv::host::guard
