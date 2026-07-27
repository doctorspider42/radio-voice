#include "core/Types.h"

namespace rv {

const char* toString(BackendType b)
{
    switch (b) {
        case BackendType::Wasapi:      return "WASAPI";
        case BackendType::DirectSound: return "DirectSound";
        case BackendType::Asio:        return "ASIO";
    }
    return "?";
}

const char* toString(WasapiMode m)
{
    switch (m) {
        case WasapiMode::Shared:    return "Shared";
        case WasapiMode::Exclusive: return "Exclusive";
    }
    return "?";
}

} // namespace rv
