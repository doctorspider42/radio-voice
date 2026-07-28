#include "core/Updater.h"

#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <string_view>
#include <system_error>
#include <vector>

#include <json.hpp>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "core/Types.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace rv {
namespace {

// The repository, written down here and in the installer's AppUrl. There is no
// update server: the release list GitHub already publishes for the workflow in
// .github/workflows/release.yml is the whole distribution channel.
constexpr wchar_t kLatestReleaseUrl[] =
    L"https://api.github.com/repos/doctorspider42/radio-voice/releases/latest";

// GitHub rejects a request without a user agent, and asks for these two on the
// REST API. Pinning the API version is what stops a future default from
// changing the shape of the document underneath us.
constexpr wchar_t kApiHeaders[] =
    L"Accept: application/vnd.github+json\r\n"
    L"X-GitHub-Api-Version: 2022-11-28";

constexpr auto kCheckInterval = std::chrono::hours(24);

// Not on startup. Sign-in is when the network stack is least likely to be up,
// and it is also the moment the user is waiting for something else.
constexpr auto kFirstCheckDelay = std::chrono::seconds(30);

// Larger than any installer this project could plausibly produce - the current
// one is a few megabytes - and small enough that a redirect to something absurd
// cannot exhaust memory before it is noticed.
constexpr size_t kMaxDownloadBytes = 128u * 1024u * 1024u;

/// Turns RV_VERSION, which is a narrow string literal, into a wide one.
#define RV_WIDEN_INNER(x) L##x
#define RV_WIDEN(x) RV_WIDEN_INNER(x)
constexpr wchar_t kUserAgent[] = L"RadioVoice/" RV_WIDEN(RV_VERSION);

/// Closes a WinHTTP handle on the way out. Three are opened per request, and
/// every early return would otherwise have to remember all three.
class Handle {
public:
    Handle() = default;
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle& operator=(HINTERNET handle)
    {
        reset();
        handle_ = handle;
        return *this;
    }

    operator HINTERNET() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    void reset()
    {
        if (handle_)
            ::WinHttpCloseHandle(handle_);
        handle_ = nullptr;
    }

    HINTERNET handle_ = nullptr;
};

/// The system's own wording for a WinHTTP error, which lives in winhttp.dll
/// rather than in the message table FormatMessage searches by default.
std::string describeError(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;

    ::FormatMessageW(flags, ::GetModuleHandleW(L"winhttp.dll"), code, 0,
                     reinterpret_cast<wchar_t*>(&text), 0, nullptr);

    std::string result;
    if (text) {
        result = toUtf8(text);
        ::LocalFree(text);
    }

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' ' || result.back() == '.'))
        result.pop_back();

    if (result.empty())
        result = "error " + std::to_string(code);
    return result;
}

/// Host of a URL, lowercased. Empty when it cannot be parsed.
std::wstring urlHost(const std::wstring& url)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1); // ask for a pointer into `url`

    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName)
        return {};

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return host;
}

/// Whether a URL points at GitHub.
///
/// Belt and braces: the URL comes out of a JSON document fetched over TLS from
/// GitHub in the first place. But what it names is an executable that the user
/// will be asked to run with administrative rights, so a redirect off to
/// somewhere else is worth one string comparison to rule out.
bool isGitHubUrl(const std::wstring& url)
{
    const std::wstring host = urlHost(url);
    if (host.empty())
        return false;

    const auto endsWith = [&](std::wstring_view suffix) {
        return host.size() > suffix.size() &&
               host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    // objects.githubusercontent.com is where release assets actually live; the
    // download URL redirects there.
    return host == L"github.com" || host == L"api.github.com" ||
           endsWith(L".github.com") || endsWith(L".githubusercontent.com");
}

using ProgressFn = std::function<void(u64 received, u64 total)>;

/// One HTTPS GET, following redirects, into `body`.
///
/// WinHTTP rather than a library: the certificate trust decisions are the
/// system's, a proxy configured for the machine is honoured without asking, and
/// it costs nothing at build time. Fetching and maintaining a third-party HTTP
/// stack for what amounts to two requests would be the larger dependency.
bool httpGet(const std::wstring& url, const wchar_t* headers, std::string& body,
             std::string& error, const std::atomic<bool>& cancel,
             const ProgressFn& progress = {})
{
    body.clear();

    URL_COMPONENTS parts{};
    parts.dwStructSize      = sizeof(parts);
    parts.dwSchemeLength    = static_cast<DWORD>(-1);
    parts.dwHostNameLength  = static_cast<DWORD>(-1);
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = "the update URL could not be parsed";
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "the update URL is not https";
        return false;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0)
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    Handle session;
    session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = describeError(::GetLastError());
        return false;
    }

    // Generous, because this runs unattended and nobody is watching the clock -
    // but bounded, because a hung socket would otherwise keep the worker thread
    // alive across a shutdown.
    ::WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);

    Handle connection;
    connection = ::WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) {
        error = describeError(::GetLastError());
        return false;
    }

    Handle request;
    request = ::WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!request) {
        error = describeError(::GetLastError());
        return false;
    }

    if (!::WinHttpSendRequest(request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                              headers ? static_cast<DWORD>(-1) : 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(request, nullptr)) {
        error = describeError(::GetLastError());
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!::WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                               WINHTTP_NO_HEADER_INDEX)) {
        error = describeError(::GetLastError());
        return false;
    }

    if (statusCode != 200) {
        // The two worth naming. Everything else is a number the log can carry.
        if (statusCode == 403 || statusCode == 429) {
            error = "GitHub is rate-limiting this address; try again later";
        } else if (statusCode == 404) {
            error = "the release could not be found (404)";
        } else {
            error = "GitHub answered " + std::to_string(statusCode);
        }
        return false;
    }

    u64 total = 0;
    {
        DWORD length = 0;
        DWORD size   = sizeof(length);
        if (::WinHttpQueryHeaders(request,
                                  WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &length, &size,
                                  WINHTTP_NO_HEADER_INDEX)) {
            total = length;
        }
    }

    if (total > kMaxDownloadBytes) {
        error = "the download is implausibly large";
        return false;
    }

    if (progress)
        progress(0, total);

    std::vector<char> buffer(64 * 1024);
    for (;;) {
        if (cancel.load()) {
            error = "cancelled";
            return false;
        }

        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request, &available)) {
            error = describeError(::GetLastError());
            return false;
        }
        if (available == 0)
            break;

        const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!::WinHttpReadData(request, buffer.data(), wanted, &read)) {
            error = describeError(::GetLastError());
            return false;
        }
        if (read == 0)
            break;

        body.append(buffer.data(), read);
        if (body.size() > kMaxDownloadBytes) {
            error = "the download is implausibly large";
            return false;
        }

        if (progress)
            progress(body.size(), total);
    }

    return true;
}

/// SHA-256 of `data`, lowercase hex. Empty when the platform refuses, which is
/// treated as "cannot verify" rather than "does not match".
std::string sha256Hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};

    DWORD digestLength = 0;
    DWORD copied       = 0;
    std::vector<unsigned char> digest;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;

    if (::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                            reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
                            &copied, 0) >= 0 &&
        digestLength > 0 &&
        ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {

        digest.resize(digestLength);
        ok = ::BCryptHashData(hash,
                              reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                              static_cast<ULONG>(data.size()), 0) >= 0 &&
             ::BCryptFinishHash(hash, digest.data(), digestLength, 0) >= 0;

        ::BCryptDestroyHash(hash);
    }

    ::BCryptCloseAlgorithmProvider(algorithm, 0);

    if (!ok)
        return {};

    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0F]);
    }
    return text;
}

/// Whether a file on disk is the asset described by `size` and `digest`.
///
/// Used to recognise an installer downloaded during an earlier run: without it,
/// an update the user keeps putting off is fetched again every day, several
/// megabytes at a time, for a file that is already there.
///
/// The checks are the ones the download itself makes, applied to the same bytes,
/// so a file that passes here is one that would have passed then.
bool isTheSameAsset(const fs::path& file, u64 size, const std::string& digest)
{
    std::error_code ec;
    if (!fs::exists(file, ec))
        return false;

    const auto onDisk = fs::file_size(file, ec);
    if (ec || onDisk == 0 || onDisk > kMaxDownloadBytes)
        return false;
    if (size > 0 && onDisk != size)
        return false;

    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return false;

    std::string contents(static_cast<size_t>(onDisk), '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream)
        return false;

    if (contents.size() < 2 || contents[0] != 'M' || contents[1] != 'Z')
        return false;

    // No published digest leaves size and shape as the only evidence - which is
    // all the download had to go on either.
    if (digest.rfind("sha256:", 0) != 0)
        return true;

    const std::string actual = sha256Hex(contents);
    return !actual.empty() && iequals(actual, digest.substr(7));
}

/// A string field, or empty.
///
/// `value()` is not enough on its own: GitHub sends an explicit null for
/// fields it has nothing to put in, and converting one to std::string throws.
std::string stringField(const json& node, const char* key)
{
    const auto it = node.find(key);
    if (it == node.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

struct Version {
    int  parts[3]{};
    bool valid = false;
};

/// Parses "0.2.2" or "v0.2.2", and nothing else.
///
/// Deliberately strict about what follows the patch number. A tag this cannot
/// read is reported rather than guessed at: offering an update on the strength
/// of a misread version is worse than offering none.
Version parseVersion(std::string_view text)
{
    Version version;
    size_t  i = 0;

    if (i < text.size() && (text[i] == 'v' || text[i] == 'V'))
        ++i;

    for (int component = 0; component < 3; ++component) {
        if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i])))
            return {};

        int value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            if (value > 1000000)
                return {};
            ++i;
        }
        version.parts[component] = value;

        if (component < 2) {
            if (i >= text.size() || text[i] != '.')
                return {};
            ++i;
        }
    }

    if (i != text.size())
        return {};

    version.valid = true;
    return version;
}

bool isNewer(const Version& candidate, const Version& current)
{
    for (int i = 0; i < 3; ++i) {
        if (candidate.parts[i] != current.parts[i])
            return candidate.parts[i] > current.parts[i];
    }
    return false;
}

/// Release notes as written in CHANGELOG.md, trimmed and bounded. A body long
/// enough to matter is a body nobody reads in a popup anyway.
std::string tidyNotes(std::string notes)
{
    constexpr size_t kMaxNotes = 4000;

    while (!notes.empty() && (notes.back() == '\n' || notes.back() == '\r' ||
                              notes.back() == ' ' || notes.back() == '\t'))
        notes.pop_back();

    if (notes.size() > kMaxNotes) {
        notes.resize(kMaxNotes);
        notes += "\n...";
    }
    return notes;
}

} // namespace

Updater::Updater() = default;

Updater::~Updater()
{
    if (!worker_.joinable())
        return;

    cancel_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void Updater::start(bool automatic)
{
    if (worker_.joinable())
        return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        automatic_ = automatic;
        nextCheck_ = std::chrono::steady_clock::now() + kFirstCheckDelay;
    }

    worker_ = std::thread([this] { run(); });
}

void Updater::setAutomatic(bool automatic)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (automatic_ == automatic)
            return;
        automatic_ = automatic;
        if (automatic)
            nextCheck_ = std::chrono::steady_clock::now() + kFirstCheckDelay;
    }
    wake_.notify_all();
}

void Updater::checkNow()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == State::Checking || status_.state == State::Downloading)
            return;
        command_ = Command::Check;
    }
    wake_.notify_all();
}

void Updater::download()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != State::Available || !status_.downloadable)
            return;
        command_ = Command::Download;
    }
    wake_.notify_all();
}

Updater::Status Updater::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Updater::setState(State state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    if (state != State::Failed)
        status_.error.clear();
}

void Updater::fail(std::string message)
{
    RV_WARN("update: %s", message.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = State::Failed;
    status_.error = std::move(message);
}

void Updater::run()
{
    for (;;) {
        Command command = Command::None;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopping_)
                    return;

                if (command_ != Command::None) {
                    command  = command_;
                    command_ = Command::None;
                    break;
                }

                if (automatic_) {
                    if (std::chrono::steady_clock::now() >= nextCheck_) {
                        command = Command::Check;
                        break;
                    }
                    wake_.wait_until(lock, nextCheck_);
                } else {
                    wake_.wait(lock);
                }
            }

            // Scheduled from the start of the check rather than its end, so a
            // machine that is asleep for two days still gets one check on waking
            // instead of one per missed day.
            if (command == Command::Check)
                nextCheck_ = std::chrono::steady_clock::now() + kCheckInterval;
        }

        if (command == Command::Check)
            doCheck();
        else if (command == Command::Download)
            doDownload();
    }
}

void Updater::doCheck()
{
    // What is already downloaded, so that the daily check does not throw away an
    // installer the user has simply not got round to running yet.
    std::string    readyVersion;
    fs::path       readyInstaller;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == State::Ready) {
            readyVersion   = status_.version;
            readyInstaller = status_.installer;
        }
    }

    // An installer waiting on disk is worth more than the news that GitHub was
    // unreachable, so a failed check puts that state back rather than replacing
    // it with an error nobody can act on.
    std::error_code exists;
    const bool haveReady = !readyVersion.empty() && fs::exists(readyInstaller, exists);

    const auto restoreReady = [&] {
        if (!haveReady)
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state     = State::Ready;
        status_.version   = readyVersion;
        status_.installer = readyInstaller;
        status_.progress  = 1.0f;
        return true;
    };

    setState(State::Checking);

    std::string body;
    std::string error;
    if (!httpGet(kLatestReleaseUrl, kApiHeaders, body, error, cancel_)) {
        if (cancel_.load() || restoreReady())
            return;
        fail("could not reach GitHub - " + error);
        return;
    }

    json root;
    try {
        root = json::parse(body);
    } catch (const std::exception& e) {
        if (restoreReady())
            return;
        fail(std::string("GitHub's answer could not be read - ") + e.what());
        return;
    }

    const std::string tag    = stringField(root, "tag_name");
    const Version latest     = parseVersion(tag);
    const Version current    = parseVersion(RV_VERSION);

    if (!latest.valid) {
        if (restoreReady())
            return;
        fail("the newest release is tagged \"" + tag + "\", which is not a version "
             "this build knows how to compare");
        return;
    }

    // The tag without its "v", which is what the user thinks of as the version.
    const std::string version = tag.front() == 'v' || tag.front() == 'V'
                                    ? tag.substr(1)
                                    : tag;

    if (!isNewer(latest, current)) {
        RV_INFO("update: %s is the newest release; this is %s", version.c_str(), RV_VERSION);
        if (restoreReady())
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        status_.state        = State::UpToDate;
        status_.version      = version;
        status_.notes.clear();
        status_.downloadable = false;
        status_.error.clear();
        return;
    }

    // The installer, which is the only asset the release carries. Matched by
    // extension rather than by exact name so that renaming the artifact does not
    // silently stop updates working.
    std::string url;
    std::string name;
    std::string digest;
    u64         size = 0;

    const auto assets = root.find("assets");
    if (assets != root.end() && assets->is_array()) {
        for (const auto& asset : *assets) {
            const std::string assetName = stringField(asset, "name");
            if (assetName.size() < 4 ||
                !iequals(assetName.substr(assetName.size() - 4), ".exe"))
                continue;

            url    = stringField(asset, "browser_download_url");
            name   = assetName;
            digest = stringField(asset, "digest");
            const auto sizeField = asset.find("size");
            if (sizeField != asset.end() && sizeField->is_number_unsigned())
                size = sizeField->get<u64>();
            break;
        }
    }

    const bool downloadable = !url.empty() && isGitHubUrl(toWide(url));
    if (!url.empty() && !downloadable)
        RV_WARN("update: %s is not a GitHub address; ignoring it", url.c_str());

    RV_INFO("update: %s is available (this is %s)", version.c_str(), RV_VERSION);

    // It may already be here, downloaded during an earlier run and never
    // installed. Checked before the state is published, so the button says
    // "install" rather than "download" the moment the check lands.
    fs::path alreadyHere;
    if (downloadable && !name.empty()) {
        const fs::path candidate = paths::updatesDir() / toWide(name);
        if (isTheSameAsset(candidate, size, digest)) {
            alreadyHere = candidate;
            RV_INFO("update: %s was already downloaded", name.c_str());
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!alreadyHere.empty()) {
        status_.state        = State::Ready;
        status_.version      = version;
        status_.notes        = tidyNotes(stringField(root, "body"));
        status_.downloadable = true;
        status_.progress     = 1.0f;
        status_.installer    = alreadyHere;
        status_.error.clear();

        assetUrl_    = url;
        assetName_   = name;
        assetDigest_ = digest;
        assetSize_   = size;
        return;
    }

    status_.state        = State::Available;
    status_.version      = version;
    status_.notes        = tidyNotes(stringField(root, "body"));
    status_.downloadable = downloadable;
    status_.progress     = 0.0f;
    status_.error.clear();
    status_.installer.clear();

    assetUrl_    = downloadable ? url : std::string{};
    assetName_   = name;
    assetDigest_ = digest;
    assetSize_   = size;
}

void Updater::doDownload()
{
    std::string url;
    std::string name;
    std::string digest;
    std::string version;
    u64         expectedSize = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url          = assetUrl_;
        name         = assetName_;
        digest       = assetDigest_;
        version      = status_.version;
        expectedSize = assetSize_;

        if (url.empty())
            return;

        status_.state    = State::Downloading;
        status_.progress = 0.0f;
        status_.error.clear();
    }

    RV_INFO("update: downloading %s", name.c_str());

    std::string body;
    std::string error;
    const bool ok = httpGet(
        toWide(url), nullptr, body, error, cancel_, [this](u64 received, u64 total) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.progress = total > 0 ? static_cast<float>(static_cast<double>(received) /
                                                              static_cast<double>(total))
                                         : -1.0f;
        });

    if (!ok) {
        if (cancel_.load())
            return;
        fail("the download failed - " + error);
        return;
    }

    if (expectedSize > 0 && body.size() != expectedSize) {
        fail("the download is not the size GitHub said it would be");
        return;
    }

    // Every Windows executable starts with these two bytes. A proxy that
    // answered with a login page instead would not.
    if (body.size() < 2 || body[0] != 'M' || body[1] != 'Z') {
        fail("what was downloaded is not a Windows program");
        return;
    }

    // GitHub publishes "sha256:<hex>" for release assets. When it is there it is
    // checked; when it is not, the transfer was still TLS to a GitHub host, and
    // refusing to update over a missing field would break the moment the API
    // changed its mind about it.
    if (digest.rfind("sha256:", 0) == 0) {
        const std::string expected = digest.substr(7);
        const std::string actual   = sha256Hex(body);

        if (actual.empty()) {
            RV_WARN("update: SHA-256 could not be computed on this machine; "
                    "the published digest was not checked");
        } else if (!iequals(actual, expected)) {
            fail("the downloaded installer does not match the checksum GitHub "
                 "published for it");
            return;
        } else {
            RV_INFO("update: checksum verified");
        }
    } else {
        RV_WARN("update: the release published no checksum for %s", name.c_str());
    }

    std::error_code ec;
    const fs::path directory = paths::updatesDir();

    // Whatever is in there is from an update that has already been installed, or
    // one that was superseded before it was. Either way it is a few megabytes
    // that will never be run again.
    for (const auto& entry : fs::directory_iterator(directory, ec))
        fs::remove(entry.path(), ec);

    const fs::path target = directory / toWide(name.empty() ? "RadioVoice-setup.exe" : name);

    {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (file)
            file.write(body.data(), static_cast<std::streamsize>(body.size()));

        if (!file) {
            fail("the installer could not be written to " +
                 toUtf8(directory.wstring()));
            return;
        }
    }

    RV_INFO("update: %s is ready at %s", version.c_str(),
            toUtf8(target.wstring()).c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    status_.state     = State::Ready;
    status_.progress  = 1.0f;
    status_.installer = target;
    status_.error.clear();
}

} // namespace rv
