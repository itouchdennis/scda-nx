#include "core/log.h"

#ifdef SCDA_PORT
#include "port/port.h"
#endif
#ifdef SCDA_SWITCH
#include <unistd.h>
#endif

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
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();
#if defined(NDEBUG) && !defined(SCDA_VERBOSE_LOG)
static bool g_verbose = false;
#else
static bool g_verbose = true;
#endif
static bool g_off = false;  // scda.toml's logging off: no file

bool Verbose() { return g_verbose; }

void SetLogging(bool full, bool reports) {
    g_verbose = full;
    g_off = !full && !reports;
}

constexpr int kKeep = 30;  // debug logs kept

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

// the Switch keeps the log in memory until a flush (twice a second, port/switch_platform.cpp)
static void Buffer() {
#ifdef SCDA_SWITCH
    if (g_file) std::setvbuf(g_file, nullptr, _IOFBF, 1 << 20);
#endif
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    if (g_off) return;
    fs::create_directories(dir, ec);
    if (!g_verbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        Buffer();
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
    Buffer();
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    if (!g_verbose && lv < Level::Error) return;
#ifdef SCDA_PORT
    if (g_off && !port::LogToStderr()) return;
#endif
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // the time in whole milliseconds: newlib's %f shares one conversion buffer, which garbled the Switch's figures
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
    unsigned long long sec = (unsigned long long)ms / 1000, frac = (unsigned long long)ms % 1000;
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[4300];
    int n = g_verbose ? std::snprintf(line, sizeof(line), "[%5llu.%03llu] [%s] [t%04zx] %s\n", sec, frac, names[int(lv)], size_t(tid), msg)
                     : std::snprintf(line, sizeof(line), "[%5llu.%03llu] %s%s\n", sec, frac, lv == Level::Error ? "error: " : "", msg);
    n = std::min(n, int(sizeof(line)) - 1);
    if (g_file) {
        std::fwrite(line, 1, size_t(n), g_file);
#ifndef SCDA_SWITCH
        if (lv >= Level::Warn) std::fflush(g_file);  // the Switch flushes twice a second: each one is an SD write
#endif
    }
#ifdef SCDA_PORT
    if (port::LogToStderr()) std::fwrite(line, 1, size_t(n), stderr);
#else
    std::fwrite(line, 1, size_t(n), stderr);
#endif
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fflush(g_file);
#ifdef SCDA_SWITCH
    // FAT keeps the file's size in its directory entry until a sync: without one a log cut short by a hang or a
    // power off reads as empty
    fsync(fileno(g_file));
#endif
}

bool TryFlush() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock || !g_file) return false;
    std::fflush(g_file);
    return true;
}

}  // namespace logx
