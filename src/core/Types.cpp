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

const char* toString(InputMix m)
{
    switch (m) {
        case InputMix::Stereo:    return "Stereo";
        case InputMix::MonoLeft:  return "Mono (left)";
        case InputMix::MonoRight: return "Mono (right)";
        case InputMix::MonoSum:   return "Mono (sum)";
    }
    return "?";
}

} // namespace rv
