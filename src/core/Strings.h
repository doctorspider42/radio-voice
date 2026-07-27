#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rv {

/// UTF-8 <-> UTF-16 conversion. Every Win32 and VST3 boundary in this project
/// is wide; everything internal (config, logs, ImGui) is UTF-8.
std::string  toUtf8(std::wstring_view w);
std::wstring toWide(std::string_view s);

/// Case-insensitive comparison, ASCII only - adequate for file extensions and
/// device-name matching, and free of locale surprises.
bool iequals(std::string_view a, std::string_view b);
bool icontains(std::string_view haystack, std::string_view needle);

/// Formats a linear gain as a dB string, with "-inf" below the audible floor.
std::string formatDb(float db, int decimals = 1);

/// 900 -> "900 Hz", 4200 -> "4.2 kHz"
std::string formatHz(float hz);

/// Base64, used to carry opaque plugin state through the JSON configuration.
std::string           base64Encode(const std::vector<unsigned char>& data);
std::vector<unsigned char> base64Decode(std::string_view text);

} // namespace rv
