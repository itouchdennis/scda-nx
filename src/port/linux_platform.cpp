// port/port.h on Linux
#include <SDL.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "port/port.h"

namespace port {

namespace {
std::vector<std::string> g_args;
std::string g_command_line;
bool g_relaunched = false;
}  // namespace

void Init(int argc, char** argv) {
    for (int i = 0; i < argc; i++) g_args.emplace_back(argv[i]);
    for (int i = 1; i < argc; i++) {
        if (i > 1) g_command_line += ' ';
        g_command_line += argv[i];
        g_relaunched |= std::strcmp(argv[i], "--relaunched") == 0;
    }
}

const std::string& CommandLine() { return g_command_line; }
bool LogToStderr() { return true; }
bool Relaunched() { return g_relaunched; }

// LANG=de_DE.UTF-8 and the like
std::string SystemLanguage() {
    const char* lang = std::getenv("LC_ALL");
    if (!lang || !*lang) lang = std::getenv("LANG");
    std::string l = lang ? lang : "";
    if (l.rfind("fr", 0) == 0) return "fra";
    if (l.rfind("de", 0) == 0) return "deu";
    if (l.rfind("it", 0) == 0) return "ita";
    if (l.rfind("es", 0) == 0) return "esp";
    return "int";
}

// SCDA_HOME, else the executable's folder
std::filesystem::path HomeDir() {
    static const std::filesystem::path dir = [] {
        if (const char* home = std::getenv("SCDA_HOME"); home && *home) return std::filesystem::path(home);
        char self[4096] = {};
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        return std::filesystem::path(std::string(self, n > 0 ? size_t(n) : 0)).parent_path();
    }();
    return dir;
}

void Fatal(const std::string& title, const std::string& text) {
    LOGE("%s", text.c_str());
    logx::Flush();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
}

void NameThread(const char* name) {
    char n[16] = {};
    std::strncpy(n, name, sizeof(n) - 1);
    pthread_setname_np(pthread_self(), n);
}

void Breadcrumb(const char*) {}
void SetBootLog(bool) {}

size_t ThreadStackSize() { return 64u << 20; }
void ThreadStarted() {}
uint64_t ThreadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void Exit(int code) {
    logx::Flush();
    std::_Exit(code);
}

// exec the image's executable beside this one, the arguments kept and --relaunched added
void Relaunch(const char* exe, const std::string& extra) {
    logx::Flush();
    // the executables are lower case here: scda, scda_versus
    std::string name = exe;
    for (char& c : name) c = char(std::tolower(uint8_t(c)));
    std::string path = (HomeDir() / name).string();
    std::vector<std::string> args{path};
    bool marked = false;
    for (size_t i = 1; i < g_args.size(); i++) {
        if (g_args[i].rfind("--coop-host=", 0) == 0) continue;
        marked |= g_args[i] == "--relaunched";
        args.push_back(g_args[i]);
    }
    if (!marked) args.emplace_back("--relaunched");
    if (!extra.empty()) args.push_back(extra);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(path.c_str(), argv.data());
    LOGE("relaunch: cannot start %s (%s)", path.c_str(), std::strerror(errno));
    Exit(1);
}

}  // namespace port
