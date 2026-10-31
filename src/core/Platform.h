#pragma once
#include <string>

namespace rv::platform {
void openUrl(const std::string& url);
/// Cocoa objects created by a plugin scan need a pool on its worker thread.
class AutoreleasePool {
public:
    AutoreleasePool();
    ~AutoreleasePool();
    AutoreleasePool(const AutoreleasePool&) = delete;
    AutoreleasePool& operator=(const AutoreleasePool&) = delete;
private:
#ifdef __APPLE__
    void* pool_ = nullptr;
#endif
};
}
