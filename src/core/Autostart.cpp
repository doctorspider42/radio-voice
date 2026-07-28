#include "core/Autostart.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>

#include "core/Log.h"

namespace rv::autostart {
namespace {

constexpr wchar_t kRunKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"RadioVoice";

/// Full path of the running executable.
std::wstring executablePath()
{
    // MAX_PATH is not enough on a machine with long paths enabled, and
    // GetModuleFileNameW truncates rather than failing when the buffer is
    // short - so the only reliable test is whether it filled the buffer
    // exactly, and the answer is to try again with a bigger one.
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (written == 0)
            return {};
        if (written < path.size()) {
            path.resize(written);
            return path;
        }
        if (path.size() >= 32768)
            return {};
        path.resize(path.size() * 2);
    }
}

/// What the value should contain: the executable in quotes, then the flag that
/// keeps a sign-in from throwing a window at the user.
std::wstring desiredCommand()
{
    const std::wstring exe = executablePath();
    if (exe.empty())
        return {};
    return L'"' + exe + L"\" --minimized";
}

std::wstring readValue()
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};

    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring value;

    if (::RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes > 0) {
        value.resize(bytes / sizeof(wchar_t));
        if (::RegQueryValueExW(key, kValueName, nullptr, nullptr,
                               reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
            value.clear();
        } else {
            // The stored length includes the terminator when the writer wrote
            // one, and does not when it did not.
            while (!value.empty() && value.back() == L'\0')
                value.pop_back();
        }
    }

    ::RegCloseKey(key);
    return value;
}

} // namespace

bool enabled()
{
    const std::wstring stored = readValue();
    if (stored.empty())
        return false;

    const std::wstring exe = executablePath();
    if (exe.empty())
        return false;

    // Compared by containment rather than equality: the arguments may differ
    // from what this build would write, and an entry pointing at this
    // executable is on regardless of how it was phrased.
    //
    // Case-insensitive, because Windows paths are.
    std::wstring haystack = stored;
    std::wstring needle   = exe;
    const auto fold = [](std::wstring& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    };
    fold(haystack);
    fold(needle);

    return haystack.find(needle) != std::wstring::npos;
}

bool setEnabled(bool on)
{
    HKEY key = nullptr;
    LSTATUS status = ::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
                                       REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                       &key, nullptr);
    if (status != ERROR_SUCCESS) {
        RV_WARN("could not open the Run key (%ld)", static_cast<long>(status));
        return false;
    }

    if (on) {
        const std::wstring command = desiredCommand();
        if (command.empty()) {
            ::RegCloseKey(key);
            RV_WARN("could not determine this executable's path; autostart not set");
            return false;
        }

        status = ::RegSetValueExW(
            key, kValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = ::RegDeleteValueW(key, kValueName);
        // Removing something that was not there is the requested state, not a
        // failure.
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }

    ::RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        RV_WARN("could not %s the autostart entry (%ld)", on ? "write" : "remove",
                static_cast<long>(status));
        return false;
    }

    RV_INFO("autostart %s", on ? "enabled" : "disabled");
    return true;
}

} // namespace rv::autostart
