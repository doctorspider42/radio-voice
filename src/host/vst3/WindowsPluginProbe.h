#pragma once

#ifdef _WIN32

#include <filesystem>
#include <string>

#include "dsp/Chain.h"
#include "host/PluginDescriptor.h"

namespace rv::host {

/// Test activation in a child process before letting the DLL into the host.
/// A DLL can stall Windows' loader and block threads other than its caller.
std::string probePlugin(const PluginDescriptor& plugin, dsp::Chain::Format format,
                        const std::string& state);

int runPluginProbe(const std::filesystem::path& request);

} // namespace rv::host

#endif
