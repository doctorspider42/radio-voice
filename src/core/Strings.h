#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rv {

/// Native wide strings at OS boundaries; internal text is always UTF-8.
std::string  toUtf8(std::wstring_view w);
std::wstring toWide(std::string_view s);
/// VST3 strings are UTF-16 even on platforms with 32-bit wchar_t.
std::string fromUtf16(std::u16string_view text);
inline std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
inline std::filesystem::path pathFromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

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
