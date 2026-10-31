#import <Cocoa/Cocoa.h>
#import <ServiceManagement/ServiceManagement.h>
#include "core/Platform.h"
#include "core/Paths.h"
#include "core/Autostart.h"
#include "core/Strings.h"
#include "core/Log.h"
#include <cstdlib>

namespace fs = std::filesystem;
namespace rv::platform {
AutoreleasePool::AutoreleasePool() : pool_([[NSAutoreleasePool alloc] init]) {}
AutoreleasePool::~AutoreleasePool() { [static_cast<NSAutoreleasePool*>(pool_) drain]; }
void openUrl(const std::string& url)
{
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
}
}
namespace rv::paths {
const fs::path& dataDir()
{
    static const fs::path dir = [] {
        if (const char* overridePath = std::getenv("RADIOVOICE_DATA_DIR")) {
            fs::path path(overridePath); fs::create_directories(path); return path;
        }
        NSURL* base = [[[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask] firstObject];
        fs::path path = fs::path(base.fileSystemRepresentation) / "RadioVoice";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}
fs::path configFile() { return dataDir() / "config.json"; }
fs::path pluginCacheFile() { return dataDir() / "plugins.json"; }
fs::path logFile() { return dataDir() / "radiovoice.log"; }
fs::path presetsDir() { auto p = dataDir() / "presets"; fs::create_directories(p); return p; }
fs::path updatesDir() { auto p = dataDir() / "updates"; fs::create_directories(p); return p; }
std::vector<fs::path> defaultVst3Directories()
{
    return {fs::path(NSHomeDirectory().fileSystemRepresentation) / "Library/Audio/Plug-Ins/VST3",
        "/Library/Audio/Plug-Ins/VST3"};
}
fs::path pickDirectory(void*, const wchar_t* title)
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    if (title) panel.message = [NSString stringWithUTF8String:toUtf8(title).c_str()];
    if ([panel runModal] == NSModalResponseOK) return fs::path(panel.URL.fileSystemRepresentation);
    return {};
}
}
namespace rv::autostart {
bool enabled() { return SMAppService.mainAppService.status == SMAppServiceStatusEnabled; }
bool setEnabled(bool on)
{
    NSError* error = nil;
    BOOL ok = on ? [SMAppService.mainAppService registerAndReturnError:&error]
                 : [SMAppService.mainAppService unregisterAndReturnError:&error];
    if (!ok) RV_WARN("Login item: %s", error.localizedDescription.UTF8String);
    return ok;
}
}
