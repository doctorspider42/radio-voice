#include "core/Strings.h"

#include <windows.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace rv {

std::string toUtf8(std::wstring_view w)
{
    if (w.empty())
        return {};

    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};

    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring toWide(std::string_view s)
{
    if (s.empty())
        return {};

    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                             nullptr, 0);
    if (needed <= 0)
        return {};

    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), needed);
    return out;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool icontains(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;

    const size_t last = haystack.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        if (iequals(haystack.substr(i, needle.size()), needle))
            return true;
    }
    return false;
}

std::string formatDb(float db, int decimals)
{
    if (db <= -96.0f || !std::isfinite(db))
        return "-inf dB";

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.*f dB", decimals, static_cast<double>(db));
    return buf;
}

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Reverse lookup, built once. 0xFF marks a character that is not part of the
/// alphabet, which lets the decoder skip whitespace and line breaks silently.
const unsigned char* base64ReverseTable()
{
    static const auto table = [] {
        std::array<unsigned char, 256> t{};
        t.fill(0xFF);
        for (unsigned char i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
        return t;
    }();
    return table.data();
}

} // namespace

std::string base64Encode(const std::vector<unsigned char>& data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned int triple =
            (static_cast<unsigned int>(data[i]) << 16) |
            (static_cast<unsigned int>(data[i + 1]) << 8) |
            static_cast<unsigned int>(data[i + 2]);
        out += kBase64Alphabet[(triple >> 18) & 0x3F];
        out += kBase64Alphabet[(triple >> 12) & 0x3F];
        out += kBase64Alphabet[(triple >> 6) & 0x3F];
        out += kBase64Alphabet[triple & 0x3F];
    }

    if (i < data.size()) {
        const size_t remaining = data.size() - i;
        unsigned int triple = static_cast<unsigned int>(data[i]) << 16;
        if (remaining == 2)
            triple |= static_cast<unsigned int>(data[i + 1]) << 8;

        out += kBase64Alphabet[(triple >> 18) & 0x3F];
        out += kBase64Alphabet[(triple >> 12) & 0x3F];
        out += (remaining == 2) ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=';
        out += '=';
    }

    return out;
}

std::vector<unsigned char> base64Decode(std::string_view text)
{
    const unsigned char* reverse = base64ReverseTable();

    std::vector<unsigned char> out;
    out.reserve(text.size() / 4 * 3);

    unsigned int accumulator = 0;
    int          bits        = 0;

    for (char c : text) {
        if (c == '=')
            break;
        const unsigned char value = reverse[static_cast<unsigned char>(c)];
        if (value == 0xFF)
            continue; // whitespace or padding noise

        accumulator = (accumulator << 6) | value;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
        }
    }

    return out;
}

std::string formatHz(float hz)
{
    char buf[32];
    if (hz >= 1000.0f) {
        const double k = hz / 1000.0;
        // Drop the decimal when it would only ever read ".0".
        std::snprintf(buf, sizeof(buf), k >= 10.0 ? "%.1f kHz" : "%.2f kHz", k);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f Hz", static_cast<double>(hz));
    }
    return buf;
}

} // namespace rv
