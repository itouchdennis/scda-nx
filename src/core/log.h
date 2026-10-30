#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>

namespace logx {

// Report lines (system, settings, the game found) and errors go to every build's log; a release build leaves out
// the rest, a debug build writes everything
enum class Level { Debug, Info, Warn, Error, Report };

// release: dir/prefix.log, started over unless append (a relaunch carries on its session's log); debug: a numbered
// dir/prefix_NNN.log per run, the newest 30 kept
void Init(const std::string& dir, const std::string& prefix, bool append);
void Write(Level lv, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Flush();
// Flush unless another thread holds the log (a crash handler, which must not wait)
bool TryFlush();
const std::string& Path();

// SCDA_VERBOSE_LOG: a release build with the debug log (the port's console tests, where the log is all there is)
#if defined(NDEBUG) && !defined(SCDA_VERBOSE_LOG)
inline constexpr bool kVerbose = false;
#else
inline constexpr bool kVerbose = true;
#endif

}  // namespace logx

#define LOGD(...) ::logx::Write(::logx::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::logx::Write(::logx::Level::Info, __VA_ARGS__)
#define LOGW(...) ::logx::Write(::logx::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::logx::Write(::logx::Level::Error, __VA_ARGS__)
#define LOGR(...) ::logx::Write(::logx::Level::Report, __VA_ARGS__)

// logs only the first n times a call site is reached
#define LOG_FIRST(n, ...)                                  \
    do {                                                   \
        static std::atomic<int> _c{0};                     \
        if (_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGI(__VA_ARGS__); \
    } while (0)
