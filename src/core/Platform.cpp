#include "core/Platform.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include "core/Strings.h"
namespace rv::platform {
AutoreleasePool::AutoreleasePool() = default;
AutoreleasePool::~AutoreleasePool() = default;
void openUrl(const std::string& url)
{
    ::ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}
#endif
