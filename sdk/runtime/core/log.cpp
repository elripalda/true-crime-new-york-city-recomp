#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;
static std::atomic<bool> g_crash_mode{false};
static FILE* g_file = nullptr;
static fs::path g_file_path;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

constexpr int kKeep = 30;

// Opens the log without narrowing the install path through the ANSI code page.
static FILE* OpenLog(const fs::path& path, bool append) {
#ifdef _WIN32
    wchar_t mode[4] = {append ? L'a' : L'w', 0, 0, 0};
    return _wfopen(path.c_str(), mode);
#else
    return std::fopen(path.c_str(), append ? "a" : "w");
#endif
}

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const fs::path& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_file_path = dir / (prefix + ".log");
        g_file = OpenLog(g_file_path, append);
    } else {
        std::vector<std::pair<int, fs::path>> runs;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
        std::sort(runs.begin(), runs.end());
        for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
        char name[256];
        std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
        g_file_path = dir / name;
        g_file = OpenLog(g_file_path, false);
    }
    const auto bytes = g_file_path.u8string();
    g_path.assign(bytes.begin(), bytes.end());
}

const std::string& Path() { return g_path; }

void EnterCrashMode() { g_crash_mode.store(true, std::memory_order_relaxed); }

void Write(Level lv, const char* fmt, ...) {
    static const bool env_verbose = std::getenv("XBE_VERBOSE") != nullptr;  // release: XBE_VERBOSE keeps every level
    if (!kVerbose && !env_verbose && lv < Level::Error) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    char line[4300];
    int n = kVerbose ? std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg)
                     : std::snprintf(line, sizeof(line), "[%9.3f] %s%s\n", t, lv == Level::Error ? "error: " : "", msg);
    if (n < 0) return;
    n = std::min(n, int(sizeof(line)) - 1);
    auto emit = [&](bool have_lock) {
        if (g_file && have_lock) {
            std::fwrite(line, 1, size_t(n), g_file);
            if (lv >= Level::Warn) std::fflush(g_file);
        }
        std::fwrite(line, 1, size_t(n), stderr);
    };
    if (g_crash_mode.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        // A fault inside the logger itself already holds g_mutex: still write the
        // line to stderr instead of deadlocking the crash filter.
        emit(lock.owns_lock());
    } else {
        std::lock_guard<std::mutex> lock(g_mutex);
        emit(true);
    }
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
