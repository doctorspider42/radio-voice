#include "core/Log.h"

#include "core/Paths.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace rv::log {
namespace {

constexpr size_t kMaxEntries = 1000;

struct State {
    std::mutex        mutex;
    std::deque<Entry> entries;
    std::FILE*        file = nullptr;
    int               problems = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool              initialised = false;
};

State& state()
{
    static State s;
    return s;
}

const char* levelTag(Level l)
{
    switch (l) {
        case Level::Debug:   return "DBG";
        case Level::Info:    return "INF";
        case Level::Warning: return "WRN";
        case Level::Error:   return "ERR";
    }
    return "???";
}

} // namespace

void init()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialised)
        return;

    s.initialised = true;

    // Plain byte mode, not "ccs=UTF-8": that mode makes the stream
    // wide-oriented, and every write below is narrow. Mixing the two is
    // undefined and crashes on the first fprintf. Log text is already UTF-8,
    // so the bytes only need a BOM in front for editors to detect it.
    s.file = _wfopen(paths::logFile().c_str(), L"wb");
    if (s.file)
        std::fwrite("\xEF\xBB\xBF", 1, 3, s.file);
}

void shutdown()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
}

void write(Level level, const char* fmt, ...)
{
    char buffer[2048];

    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (n < 0)
        return;

    State& s = state();
    const double t = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - s.start).count();

    std::lock_guard lock(s.mutex);

    s.entries.push_back({level, t, buffer});
    if (s.entries.size() > kMaxEntries)
        s.entries.pop_front();

    if (level >= Level::Warning)
        ++s.problems;

    if (s.file) {
        std::fprintf(s.file, "[%9.3f] %s  %s\n", t, levelTag(level), buffer);
        // Flushed eagerly: the most interesting log line is usually the last
        // one before a crash inside a third-party plugin.
        std::fflush(s.file);
    }

#ifdef _DEBUG
    ::OutputDebugStringA(buffer);
    ::OutputDebugStringA("\n");
#endif
}

std::vector<Entry> snapshot()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return {s.entries.begin(), s.entries.end()};
}

int problemCount()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.problems;
}

} // namespace rv::log
