// The perf branch's diagnostics on the Switch, while scda.toml's profiling is on; with logging off the log holds the
// measurements alone (fps, frame time, the render thread, these), which full logging's every line would slow:
// - every 10 s, each named thread's CPU time over the window and its preferred core, and the cores' sums: whether the
//   game's thread shares its core with others
// - a sampling profiler on the busiest threads of the window before: about 500 times a second each is paused, its
//   program counter counted, and let go. A thread in a system call (an svc at or just before its program counter)
//   has its return address counted too, and every return address on its stack: who made the call, and the calls
//   that led there. logs/profile.txt holds the counts of the whole run
//   (offsets into scda.elf from the module's base, which addr2line or nm turn into functions), the log the top ones
//   of each window.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "port/port.h"

namespace port {

namespace {

bool g_profiling = false;

constexpr u64 kWindowNs = 10'000'000'000;
constexpr u64 kSampleNs = 2'000'000;  // between rounds of samples
constexpr int kSampled = 3;           // threads sampled at once, the busiest
constexpr double kSampledShare = 0.2;  // of a core over the last window, to be sampled
constexpr int kTopInLog = 12;
constexpr u32 kBucket = 16;  // bytes of code counted as one place

struct Named {
    Handle handle = INVALID_HANDLE;
    std::string name;
    u64 ticks = 0;  // CPU ticks at the window's start
    double share = 0;  // of a core over the last window
    bool sampled = false;
    std::unordered_map<u32, u32> window;  // bucket offset to samples, this window
    std::unordered_map<u32, u32> callers;  // of the samples in a system call: the caller's bucket, this window
    std::unordered_map<u32, u32> stacks;   // and the return addresses on their stacks, each once a sample
};

std::mutex g_lock;
std::vector<Named> g_named;
std::unordered_map<std::string, std::unordered_map<u32, u32>> g_run;  // by thread name, the whole run
std::unordered_map<std::string, std::unordered_map<u32, u32>> g_run_callers;
std::unordered_map<std::string, std::unordered_map<u32, u32>> g_run_stacks;
u64 g_base = 0;  // the module's: where scda.elf's address 0 is
u64 g_text = 0;  // its code's size
constexpr u64 kStackScan = 4096;  // bytes of a stack searched for return addresses
constexpr size_t kStackFrames = 24;
std::atomic<u32> g_failed{0};  // samples the kernel refused

u64 Ticks(Handle h) {
    u64 t = 0;
    return R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, h, u64(-1))) ? t : ~0ull;
}

int PreferredCore(Handle h) {
    s32 core = -1;
    u64 mask = 0;
    return R_SUCCEEDED(svcGetThreadCoreMask(&core, &mask, h)) ? core : -1;
}

struct Sample {
    u64 pc = 0;
    u64 lr = 0;
    bool svc = false;
    size_t frames = 0;
    u32 stack[kStackFrames];  // return addresses' buckets, a system call's only
};

// the module's code: the region this function is in (an NRO's text begins at its address 0)
void FindModule() {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, reinterpret_cast<u64>(&FindModule)))) return;
    g_base = mi.addr;
    g_text = mi.size;
}

bool InModule(u64 pc) { return pc - g_base < g_text; }

u32 Insn(u64 at) {
    u32 insn = 0;
    std::memcpy(&insn, reinterpret_cast<const void*>(at), 4);
    return insn;
}

bool IsSvc(u32 insn) { return (insn & 0xFFE0001F) == 0xD4000001; }

// a thread in a system call: the kernel gives the svc's own address for one blocked there, the next for one let go
bool InSvc(u64 pc) { return IsSvc(Insn(pc)) || (pc - g_base >= 4 && IsSvc(Insn(pc - 4))); }

// an address a bl or blr returns to
bool IsReturn(u64 v) {
    if (!InModule(v) || v & 3 || v - g_base < 4) return false;
    u32 insn = Insn(v - 4);
    return (insn & 0xFC000000) == 0x94000000 || (insn & 0xFFFFFC1F) == 0xD63F0000;
}

u32 Bucket(u64 at) { return u32((at - g_base) / kBucket * kBucket); }

// the return addresses on a paused thread's stack, from sp up to the end of its stack's region
void ScanStack(u64 sp, Sample& s) {
    MemoryInfo mi{};
    u32 pi = 0;
    if (!sp || R_FAILED(svcQueryMemory(&mi, &pi, sp)) || !(mi.perm & Perm_R)) return;
    u64 end = std::min(mi.addr + mi.size, sp + kStackScan);
    for (u64 at = sp & ~7ull; at + 8 <= end && s.frames < kStackFrames; at += 8) {
        u64 v = *reinterpret_cast<const u64*>(at);
        if (!IsReturn(v)) continue;
        u32 b = Bucket(v);
        if (std::find(s.stack, s.stack + s.frames, b) == s.stack + s.frames) s.stack[s.frames++] = b;
    }
}

// the thread paused, where it was read (and its stack, in a system call), and let go; pc 0 when the kernel would not
Sample Take(Handle h) {
    Sample s;
    if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) return s;
    ThreadContext ctx{};
    if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h)) && InModule(ctx.pc.x)) {
        s.pc = ctx.pc.x;
        s.lr = ctx.lr;
        s.svc = InSvc(s.pc);
        if (s.svc) ScanStack(ctx.sp, s);
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    return s;
}

void WriteCounts(FILE* f, const char* what, const std::string& name, const std::unordered_map<u32, u32>& counts) {
    std::vector<std::pair<u32, u32>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::fprintf(f, "%s %s\n", what, name.c_str());
    for (auto& [off, n] : v) std::fprintf(f, "%08X %u\n", off, n);
}

// every name's counts of the run so far, as offsets into scda.elf; overwritten each window
void WriteRun() {
    FILE* f = std::fopen("sdmc:/switch/scda/logs/profile.txt", "w");
    if (!f) return;
    std::fprintf(f, "# offsets into scda.elf (%u byte buckets), samples; module base %llX\n", kBucket,
                 (unsigned long long)g_base);
    std::fprintf(f, "# thread: where it was; callers: who made the system call it was in; stacks: the return addresses "
                    "on the stack in a system call, each once a sample\n");
    for (auto& [name, counts] : g_run) WriteCounts(f, "thread", name, counts);
    for (auto& [name, counts] : g_run_callers) WriteCounts(f, "callers", name, counts);
    for (auto& [name, counts] : g_run_stacks) WriteCounts(f, "stacks", name, counts);
    std::fclose(f);
}

void EndWindow(u64 window_ns) {
    double cores[3] = {};
    std::vector<Named*> by_share;
    for (Named& n : g_named) {
        u64 t = Ticks(n.handle);
        if (t == ~0ull) {
            n.share = 0;
            continue;
        }
        n.share = double(armTicksToNs(t - n.ticks)) / double(window_ns);
        n.ticks = t;
        int core = PreferredCore(n.handle);
        if (core >= 0 && core < 3) cores[core] += n.share;
        if (n.share >= 0.01)
            LOGR("perf: %s on core %d: %.0f%% of a core", n.name.c_str(), core, n.share * 100);
        by_share.push_back(&n);
    }
    LOGR("perf: cores 0 1 2 asked for %.0f%% %.0f%% %.0f%% by the named threads, %u samples refused", cores[0] * 100,
         cores[1] * 100, cores[2] * 100, g_failed.exchange(0));
    // the window's top places of each sampled thread, then the next window's threads
    for (Named& n : g_named) {
        if (!n.sampled || n.window.empty()) continue;
        std::vector<std::pair<u32, u32>> v(n.window.begin(), n.window.end());
        u32 total = 0;
        for (auto& [off, c] : v) total += c;
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::string line;
        char part[32];
        for (int i = 0; i < kTopInLog && i < int(v.size()); i++) {
            std::snprintf(part, sizeof(part), " %X:%.1f%%", v[size_t(i)].first, 100.0 * v[size_t(i)].second / total);
            line += part;
        }
        LOGR("perf: %s, %u samples, top:%s", n.name.c_str(), total, line.c_str());
        auto& run = g_run[n.name];
        for (auto& [off, c] : v) run[off] += c;
        auto& callers = g_run_callers[n.name];
        for (auto& [off, c] : n.callers) callers[off] += c;
        auto& stacks = g_run_stacks[n.name];
        for (auto& [off, c] : n.stacks) stacks[off] += c;
        n.window.clear();
        n.callers.clear();
        n.stacks.clear();
    }
    std::sort(by_share.begin(), by_share.end(), [](Named* a, Named* b) { return a->share > b->share; });
    for (Named& n : g_named) n.sampled = false;
    for (int i = 0; i < kSampled && i < int(by_share.size()); i++)
        by_share[size_t(i)]->sampled = by_share[size_t(i)]->share >= kSampledShare;
    WriteRun();
}

void Profile(void*) {
    u64 window_start = armTicksToNs(armGetSystemTick());
    for (;;) {
        svcSleepThread(kSampleNs);
        if (!g_profiling) continue;
        std::lock_guard lock(g_lock);
        for (Named& n : g_named) {
            if (!n.sampled) continue;
            Sample s = Take(n.handle);
            if (!s.pc) {
                g_failed++;
                continue;
            }
            n.window[Bucket(s.pc)]++;
            if (!s.svc) continue;
            if (InModule(s.lr)) n.callers[Bucket(s.lr)]++;
            for (size_t i = 0; i < s.frames; i++) n.stacks[s.stack[i]]++;
        }
        u64 now = armTicksToNs(armGetSystemTick());
        if (now - window_start >= kWindowNs) {
            EndWindow(now - window_start);
            window_start = now;
        }
    }
}

}  // namespace

void NameThread(const char* name) {
    std::lock_guard lock(g_lock);
    Named n;
    n.handle = threadGetCurHandle();
    n.name = name;
    n.ticks = Ticks(n.handle);
    // a name seen before (guest threads end and start again under new ids) keeps its own entry
    g_named.push_back(std::move(n));
}

void ThreadEnded() {
    Handle h = threadGetCurHandle();
    std::lock_guard lock(g_lock);
    g_named.erase(std::remove_if(g_named.begin(), g_named.end(), [&](const Named& n) { return n.handle == h; }),
                  g_named.end());
}

void SetProfiling(bool on) { g_profiling = on; }

void StartProfiler() {
    NameThread("main");
    static Thread t;
    // above the game's threads, so a round of samples is not held up; on core 0 with the main thread's event loop
    if (!g_profiling) return;
    FindModule();
    if (R_SUCCEEDED(threadCreate(&t, Profile, nullptr, nullptr, 0x10000, 0x20, 0))) threadStart(&t);
    LOGR("perf: profiler on, module base %llX, %llu MB of code", (unsigned long long)g_base,
         (unsigned long long)(g_text >> 20));
}

}  // namespace port
