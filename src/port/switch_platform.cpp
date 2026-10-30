// port/port.h on the Switch (libnx). The game lives in sdmc:/switch/scda: scda.nro, game/ with the extracted disc,
// scda.toml, logs/ and hdd/. A relaunch (front end to mission) hands hbloader the next NRO to load with
// envSetNextLoad and exits; the launch data travels in hdd/launch.bin as on Windows.
#include <arpa/inet.h>  // struct in_addr for nxlink.h
#include <switch.h>
#ifdef SCDA_VK
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "port/port.h"

// what __libnx_initheap left out of the heap hbloader handed over, for boot.txt
namespace {
uint64_t g_heap_left_out = 0;
int g_heap_left_ranges = 0;
}  // namespace

// libnx's own (weak) takes hbloader's heap as it is. A run before this one in the process (a relaunch) may have left
// pages of it inaccessible, a stack mirror's source or transfer memory it did not close, and the first allocation
// over one faults (tmemCreate's memset, before main). The heap is the largest stretch of it that is plain read/write.
extern "C" char __tls_start[], __tls_end[];

extern "C" void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    void* addr = nullptr;
    size_t size = 0;
    if (envHasHeapOverride()) {
        addr = envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        u64 available = 0, used = 0;
        svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        if (available > used + 0x200000) size = (available - used - 0x200000) & ~u64(0x1FFFFF);
        if (!size) size = 0x2000000 * 16;
        if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
    u64 begin = u64(addr), end = begin + size, best_at = 0, best_size = 0, run_at = 0, run_size = 0;
    // a run's GPU buffers stay uncached after nvservices unmaps them: those pages are plain heap again once marked
    // cached
    for (u64 at = begin; at < end;) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, at)) || !mi.size) break;
        u64 from = std::max<u64>(mi.addr, at), to = std::min<u64>(mi.addr + mi.size, end);
        if (mi.type == MemType_Heap && mi.perm == Perm_Rw && mi.attr == MemAttr_IsUncached)
            svcSetMemoryAttribute(reinterpret_cast<void*>(from), to - from, MemAttr_IsUncached, 0);
        at = to;
    }
    for (u64 at = begin; at < end;) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, at)) || !mi.size) break;
        u64 from = std::max<u64>(mi.addr, at), to = std::min<u64>(mi.addr + mi.size, end);
        if (mi.type == MemType_Heap && mi.perm == Perm_Rw && !mi.attr) {
            if (!run_size) run_at = from;
            run_size += to - from;
            if (run_size > best_size) best_at = run_at, best_size = run_size;
        } else {
            run_size = 0;
            g_heap_left_ranges++;
        }
        at = to;
    }
    if (best_size) {
        g_heap_left_out = size - best_size;
        addr = reinterpret_cast<void*>(best_at);
        size = best_size;
    }
    fake_heap_start = static_cast<char*>(addr);
    fake_heap_end = static_cast<char*>(addr) + size;
}

namespace port {

namespace {
const std::filesystem::path kHome = "sdmc:/switch/scda";
std::string g_command_line;
bool g_relaunched = false;
bool g_sockets = false;

// FastLoad boost takes the CPU to 1785 MHz but drops the GPU to 76.8 MHz; the CPU is raised alone instead
constexpr u32 kCpuFast = 1785000000;
constexpr u32 kCpuNormal = 1020000000;

bool SetCpuClock(u32 hz) {
    if (R_FAILED(clkrstInitialize())) return false;
    ClkrstSession s;
    bool ok = R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3));
    if (ok) {
        ok = R_SUCCEEDED(clkrstSetClockRate(&s, hz));
        clkrstCloseSession(&s);
    }
    clkrstExit();
    return ok;
}

u32 ClockRate(PcvModuleId module) {
    u32 hz = 0;
    if (R_FAILED(clkrstInitialize())) return 0;
    ClkrstSession s;
    if (R_SUCCEEDED(clkrstOpenSession(&s, module, 3))) {
        clkrstGetClockRate(&s, &hz);
        clkrstCloseSession(&s);
    }
    clkrstExit();
    return hz;
}

void RaiseClocks() {
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    bool ok = SetCpuClock(kCpuFast);
    LOGI("clocks: CPU %u MHz%s, GPU %u MHz", ClockRate(PcvModuleId_CpuBus) / 1000000, ok ? "" : " (not raised)",
         ClockRate(PcvModuleId_GPU) / 1000000);
}

void RestoreClocks() {
    SetCpuClock(kCpuNormal);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}
}  // namespace

// started by nxlink -s: stderr goes to the PC, and the log with it (core/log.cpp)
bool g_nxlink = false;
bool g_boot_log = true;  // scda.toml's logging

void SetBootLog(bool on) { g_boot_log = on; }

// beside the logs; a relaunch adds its run to its predecessors' (menu to mission and back), a fresh start begins anew
void Breadcrumb(const char* step) {
    if (g_nxlink) std::fprintf(stderr, "[boot] %s\n", step);
    if (!g_boot_log) return;
    static bool first = true;
    // from hbloader's arguments, not Init's: a crash before main must not wipe the runs before it
    const char* args = static_cast<const char*>(envGetArgv());
    bool relaunched = g_relaunched || (args && std::strstr(args, "--relaunched"));
    if (FILE* f = std::fopen("sdmc:/switch/scda/logs/boot.txt", first && !relaunched ? "w" : "a")) {
        if (first) std::fprintf(f, "--- %s\n", relaunched ? "relaunched" : "started");
        std::fprintf(f, "%llu ms %s\n", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000), step);
        std::fclose(f);
    }
    first = false;
}

// what ended the process when it was no CPU exception: an uncaught C++ exception or abort()
void WriteCrash(const char* what) {
    if (g_nxlink) std::fprintf(stderr, "[crash] %s\n", what);
    Breadcrumb((std::string("CRASH: ") + what).c_str());
    if (FILE* f = std::fopen("sdmc:/switch/scda/logs/crash.txt", "w")) {
        std::fprintf(f, "%s\n", what);
        std::fclose(f);
    }
    logx::TryFlush();
}

void Init(int argc, char** argv) {
    // before anything: socketInitializeDefault is what nxlink's stdio redirect needs
    g_sockets = R_SUCCEEDED(socketInitializeDefault());
    g_nxlink = g_sockets && __nxlink_host.s_addr && nxlinkStdio() >= 0;
    for (int i = 1; i < argc; i++) g_relaunched |= std::strcmp(argv[i], "--relaunched") == 0;
    std::error_code ec;
    std::filesystem::create_directories(kHome / "logs", ec);
    Breadcrumb("main");
    if (g_heap_left_out) {
        char step[96];
        std::snprintf(step, sizeof(step), "heap: %llu MB left out in %d ranges a run before left inaccessible",
                      (unsigned long long)(g_heap_left_out >> 20), g_heap_left_ranges);
        Breadcrumb(step);
    }
    std::set_terminate([] {
        std::string what = "terminate";
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& e) {
            what += std::string(": ") + e.what();
        } catch (...) {
            what += ": unknown exception";
        }
        WriteCrash(what.c_str());
        svcExitProcess();
    });
    std::signal(SIGABRT, [](int) {
        WriteCrash("abort");
        svcExitProcess();
    });
    for (int i = 1; i < argc; i++) {
        if (i > 1) g_command_line += ' ';
        g_command_line += argv[i];
    }
    // the log reaches the card twice a second, not with every line (each write is an SD access)
    std::thread([] {
        for (int n = 1;; n++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            logx::Flush();
            if (n % 20 == 0) Breadcrumb("alive");  // how long a run lasted, when its log did not survive
        }
    }).detach();
    // the game handles home and the power button through the applet's own suspension; its threads keep the CPU
    RaiseClocks();
}

const std::string& CommandLine() { return g_command_line; }
bool LogToStderr() { return g_nxlink; }
bool Relaunched() { return g_relaunched; }

std::string SystemLanguage() {
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code))) setMakeLanguage(code, &lang);
        setExit();
    }
    switch (lang) {
        case SetLanguage_FR:
        case SetLanguage_FRCA: return "fra";
        case SetLanguage_DE: return "deu";
        case SetLanguage_IT: return "ita";
        case SetLanguage_ES:
        case SetLanguage_ES419: return "esp";
        default: return "int";
    }
}

std::filesystem::path HomeDir() { return kHome; }

// the error applet: the player sees why the game will not start, then it closes
void Fatal(const std::string& title, const std::string& text) {
    LOGE("%s", text.c_str());
    logx::Flush();
    ErrorApplicationConfig c;
    std::string dialog = title + ": " + text;
    if (R_SUCCEEDED(errorApplicationCreate(&c, dialog.c_str(), nullptr))) errorApplicationShow(&c);
}

void NameThread(const char*) {}

#ifdef SCDA_VK
// NVK presents to the default NWindow through VK_NN_vi_surface; no SDL window holds it (sdl_window.cpp)
std::vector<const char*> VulkanInstanceExtensions() {
    return {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
}

bool CreateVulkanSurface(void* instance, uint64_t* surface) {
    NWindow* win = nwindowGetDefault();
    if (!win || !nwindowIsValid(win)) {
        LOGE("vk: no default window");
        return false;
    }
    // the swap chain takes the window's size: the frame's, as DrawableSize gives it
    uint32_t w, h;
    DrawableSize(w, h);
    if (Result rc = nwindowSetDimensions(win, w, h); R_FAILED(rc)) LOGW("vk: window size %ux%u refused (0x%X)", w, h, rc);
    auto create = reinterpret_cast<PFN_vkCreateViSurfaceNN>(
        vkGetInstanceProcAddr(static_cast<VkInstance>(instance), "vkCreateViSurfaceNN"));
    VkViSurfaceCreateInfoNN ci{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    ci.window = win;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create ? create(static_cast<VkInstance>(instance), &ci, nullptr, &s) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r != VK_SUCCESS) {
        LOGE("vk: no surface on the window (%d)", r);
        return false;
    }
    *surface = uint64_t(s);
    return true;
}
#else
// the OpenGL renderer's window is SDL's
std::vector<const char*> VulkanInstanceExtensions() { return {}; }
bool CreateVulkanSurface(void*, uint64_t*) { return false; }
#endif

// SCDA_STACK_MB in the environment file overrides it (16 MB as nfsu2-sw's game thread)
size_t ThreadStackSize() {
    const char* mb = std::getenv("SCDA_STACK_MB");
    size_t n = mb ? size_t(std::atoi(mb)) : 0;
    return (n >= 1 && n <= 64 ? n : 16) << 20;
}

// libnx starts every thread with core 0 preferred: deal them round robin over the cores the process may use, 0 to 2
// (core 3 is the system's and does not time-slice the default priority)
void ThreadStarted() {
    static std::atomic<int> next{1};  // core 0 runs the main thread's event loop
    u64 mask = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) return;
    mask &= 0x7;
    if (!mask || !(mask & (mask - 1))) return;
    int k = next.fetch_add(1) % __builtin_popcountll(mask);
    int core = 0;
    for (; core < 3; core++)
        if (((mask >> core) & 1) && k-- == 0) break;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, u32(mask));
}

uint64_t ThreadCpuNs() {
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, u64(-1)))) return 0;
    return armTicksToNs(ticks);
}

extern "C" Result __real_threadClose(Thread* t);

namespace {
// the threads libnx started and has not closed, for LeaveForNextLoad (threadCreate wrapped at link time); a detached
// thread is never closed, so its Thread stays valid after it ends
std::mutex g_threads_lock;
std::vector<Thread*> g_threads;
// the detached ones (libnx's __pthread_t starts with its Thread): no one joins them, so once ended they are closed
// here. Ended threads otherwise keep their kernel thread for good, and the process may hold only so many (about 96):
// a run's dozens of ended sound threads, and every run before it, left the third run none to start
std::vector<Thread*> g_detached;

// the kernel threads this process holds, and how many it may
void ThreadsInUse(s64* used, s64* limit) {
    *used = *limit = -1;
    Handle rl = INVALID_HANDLE;
    if (R_FAILED(svcGetInfo(reinterpret_cast<u64*>(&rl), InfoType_ResourceLimit, INVALID_HANDLE, 0)) || !rl) return;
    svcGetResourceLimitCurrentValue(used, rl, LimitableResource_Threads);
    svcGetResourceLimitLimitValue(limit, rl, LimitableResource_Threads);
    svcCloseHandle(rl);
}

// closes the detached threads that have ended (g_threads_lock held); returns how many
int ReapEnded() {
    int n = 0;
    Handle self = threadGetCurHandle();
    for (size_t i = 0; i < g_threads.size();) {
        Thread* t = g_threads[i];
        bool detached = std::find(g_detached.begin(), g_detached.end(), t) != g_detached.end();
        if (detached && t->handle != self && R_SUCCEEDED(svcWaitSynchronizationSingle(t->handle, 0))) {
            __real_threadClose(t);
            g_threads.erase(g_threads.begin() + i);
            std::erase(g_detached, t);
            n++;
        } else {
            i++;
        }
    }
    return n;
}

// Relaunch's arguments are set: the main thread leaves for them
std::atomic<bool> g_leave{false};

// the condition variable each thread waits on (condvarWaitTimeout wrapped at link time). The kernel keeps a wait by the
// condvar's address, and a paused thread keeps waiting into the next run: that run's objects sit at the same addresses
// (the same heap, allocated in the same order), and its notify_one woke the paused waiter instead of its own (the
// render thread of a mission never saw its first batch). The leave wakes these first
constexpr int kWaitSlots = 256;
std::atomic<CondVar*> g_waits[kWaitSlots];
std::atomic<int> g_wait_slots{0};
thread_local int t_wait_slot = -1;

// heap pages the next NRO could not use, to boot.txt (written to a buffer that needs no allocation)
void ReportHeap(FILE* f) {
    u64 at = 0, size = 0;
    svcGetInfo(&at, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    u64 end = at + size, blocked = 0, borrowed = 0, mapped = 0, uncached = 0;
    int listed = 0;
    while (at < end) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, at)) || !mi.size) break;
        if (mi.type == MemType_Heap && (mi.perm != Perm_Rw || mi.attr)) {
            blocked += mi.size;
            if (mi.attr & MemAttr_IsBorrowed) borrowed += mi.size;
            else if (mi.attr & MemAttr_IsDeviceMapped) mapped += mi.size;
            else if (mi.attr & MemAttr_IsUncached) uncached += mi.size;
            if (listed++ < 16)
                std::fprintf(f, "  heap %llX+%llX perm %u attr %u\n", (unsigned long long)mi.addr,
                             (unsigned long long)mi.size, unsigned(mi.perm), unsigned(mi.attr));
        }
        at = mi.addr + mi.size;
    }
    std::fprintf(f, "  heap blocked %llu MB in %d ranges: %llu MB transfer memory, %llu MB GPU mapped, %llu MB uncached\n",
                 (unsigned long long)(blocked >> 20), listed, (unsigned long long)(borrowed >> 20),
                 (unsigned long long)(mapped >> 20), (unsigned long long)(uncached >> 20));
}

// heap pages of the given attribute (transfer memory a service holds, buffers the GPU has mapped)
u64 HeapWith(u32 attr) {
    u64 at = 0, size = 0, total = 0;
    svcGetInfo(&at, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    for (u64 end = at + size; at < end;) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, at)) || !mi.size) break;
        if (mi.type == MemType_Heap && (mi.attr & attr)) total += mi.size;
        at = mi.addr + mi.size;
    }
    return total;
}

// the GPU's buffers stay uncached once nvservices has unmapped them; marked cached they are plain heap again
u64 HeapRecache() {
    u64 at = 0, size = 0, total = 0;
    svcGetInfo(&at, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    for (u64 end = at + size; at < end;) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, at)) || !mi.size) break;
        if (mi.type == MemType_Heap && mi.perm == Perm_Rw && mi.attr == MemAttr_IsUncached &&
            R_SUCCEEDED(svcSetMemoryAttribute(reinterpret_cast<void*>(mi.addr), mi.size, MemAttr_IsUncached, 0)))
            total += mi.size;
        at = mi.addr + mi.size;
    }
    return total;
}

// a service's transfer memory, once its session is closed: the pages come back to the heap when this side's
// handle goes too. The TransferMemory is libnx's own (static), found next to a session of the same file, so it is
// only taken for one when it looks like one: an open handle over heap that is lent out
bool CloseTransferMemory(TransferMemory* t) {
    if (!t || !t->handle || !t->size || !t->src_addr || (u64(t->src_addr) & 0xFFF)) return false;
    MemoryInfo mi{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&mi, &page, u64(t->src_addr))) || mi.type != MemType_Heap ||
        !(mi.attr & MemAttr_IsBorrowed))
        return false;
    svcCloseHandle(t->handle);
    t->handle = INVALID_HANDLE;
    return true;
}

// libnx sends every ioctl through a clone of the nvdrv session it keeps to itself (nv.c's g_nvSrvClone, not
// exported); nvservices keeps this run's GPU memory for as long as either session is open. nvIoctl's first
// adrp/add pair loads the clone's address (libnx 4.12)
Service* NvSessionClone() {
    const uint32_t* code = reinterpret_cast<const uint32_t*>(&nvIoctl);
    for (int i = 0; i + 1 < 128; i++) {
        uint32_t a = code[i], b = code[i + 1], rd = a & 31;
        if ((a & 0x9F000000) != 0x90000000) continue;  // adrp
        if ((b & 0xFFC00000) != 0x91000000 || (b & 31) != rd || ((b >> 5) & 31) != rd) continue;  // add rd, rd, #imm
        int64_t imm21 = int64_t(((a >> 5) & 0x7FFFF) << 2 | ((a >> 29) & 3));
        int64_t pages = (imm21 << 43) >> 43;
        uint64_t at = (uint64_t(&code[i]) & ~uint64_t(0xFFF)) + uint64_t(pages << 12) + ((b >> 10) & 0xFFF);
        Service* s = reinterpret_cast<Service*>(at);
        return s != nvGetServiceSession() && serviceIsActive(s) ? s : nullptr;
    }
    return nullptr;
}

// hbloader runs the next NRO in this process, at this address and on this heap. What this run leaves behind is
// there in the next: a thread left going would run on in its code and memory, and heap pages this run made
// inaccessible fault its first allocations past them (the second run crashed so before its log opened). Every other
// thread is paused for good (Horizon cannot end another thread) and gives back the heap its stack mirrors (16 MB a
// guest thread), the guest's mappings go, the GPU and audio services let go of their transfer memory, and the
// process leaves without exit handlers a paused thread could hold up. The main thread's stack is the loader's.
// the size threadCreate mapped the stack mirror with: Thread::stack_sz leaves out the reent and TLS area above the
// stack, and svcUnmapMemory refuses any other size (every relaunch so far kept all 40 stacks, 16 MB each, locked in
// the heap). libnx 4.12's threadClose computes it the same way
size_t StackMirrorSize(const Thread* t) {
    size_t tls = (size_t(__tls_end - __tls_start) + 0xF) & ~size_t(0xF);
    return (tls + t->stack_sz + 0x126F) & ~size_t(0xFFF);
}

[[noreturn]] void LeaveForNextLoad() {
    // unbuffered: each step is on the card before the next begins, so a step that hangs is the last one listed
    FILE* f = g_boot_log ? std::fopen("sdmc:/switch/scda/logs/boot.txt", "a") : nullptr;
    if (f) std::setvbuf(f, nullptr, _IONBF, 0);
    auto step = [&](const char* what) {
        if (f) std::fprintf(f, "%llu ms relaunch: %s\n", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000),
                            what);
    };
    Handle self = threadGetCurHandle();
    std::vector<Thread*> threads;
    int reaped;
    {
        std::lock_guard lock(g_threads_lock);
        reaped = ReapEnded();
        threads = g_threads;
    }
    s64 used, limit;
    ThreadsInUse(&used, &limit);
    LOGI("relaunch: %d ended threads closed, pausing %zu threads; the process holds %lld of %lld threads", reaped,
         threads.size(), (long long)used, (long long)limit);
    logx::Flush();
    // a thread paused inside malloc would hold its lock for good, and freeing below needs it; held here, no paused
    // thread has it. Nothing else that may take a lock runs from here on
    __malloc_lock(_REENT);
    step("malloc locked");
    if (f) std::fprintf(f, "relaunch: %d ended threads closed, %lld of %lld threads held\n", reaped, (long long)used,
                        (long long)limit);
    for (Thread* t : threads) {
        if (t->handle != self) svcSetThreadActivity(t->handle, ThreadActivity_Paused);
    }
    if (envGetMainThreadHandle() != self) svcSetThreadActivity(envGetMainThreadHandle(), ThreadActivity_Paused);
    step("threads paused");
    // every waiter off its condvar: each takes its mutex back (or waits for it, on a thread of this run) and stays paused
    int woken = 0;
    for (int i = 0, n = std::min(g_wait_slots.load(), kWaitSlots); i < n; i++) {
        if (CondVar* c = g_waits[i].load()) {
            svcSignalProcessWideKey(c, -1);
            woken++;
        }
    }
    if (f) std::fprintf(f, "relaunch: %d condvar waits woken\n", woken);
    int stacks = 0;
    for (Thread* t : threads) {
        if (t->handle != self && R_SUCCEEDED(svcUnmapMemory(t->stack_mirror, t->stack_mem, StackMirrorSize(t)))) stacks++;
    }
    ReleaseGuest();
    step("stacks and guest mappings given back");
    // the sessions closed, not nvExit/audrenExit: their cleanup waits for the service to give the transfer memory
    // back before it closes the session the service holds it for, and so never returns. The memory the services
    // let go of late the next run's heap leaves out (__libnx_initheap)
    // libnx 4.12 keeps each one's transfer memory just before the session (audren.c's g_audrenWorkBuf, nv.c's
    // g_nvTransfermem)
    Service* audio = audrenGetServiceSession_AudioRenderer();
    auto* audio_memory = reinterpret_cast<TransferMemory*>(reinterpret_cast<char*>(audio) - sizeof(TransferMemory));
    if (serviceIsActive(audio)) serviceClose(audio);
    step("audio renderer closed");
    // the clone's handle only: a paused thread may be inside an ioctl on it, and a close request would wait behind it
    Service* clone = NvSessionClone();
    auto* gpu_memory =
        clone ? reinterpret_cast<TransferMemory*>(reinterpret_cast<char*>(clone) - sizeof(TransferMemory)) : nullptr;
    if (clone) svcCloseHandle(clone->session);
    if (serviceIsActive(nvGetServiceSession())) serviceClose(nvGetServiceSession());
    step(clone ? "nvdrv and its clone closed" : "nvdrv closed, its clone not found");
    bool audio_closed = CloseTransferMemory(audio_memory), gpu_closed = CloseTransferMemory(gpu_memory);
    step(audio_closed && gpu_closed ? "audio and GPU transfer memory closed"
         : audio_closed             ? "audio transfer memory closed, the GPU's not found"
         : gpu_closed               ? "GPU transfer memory closed, the audio renderer's not found"
                                    : "no transfer memory found to close");
    // bsdsocket holds its transfer memory and sessions for good otherwise, and the next run opens its own
    if (g_sockets) socketExit();
    g_sockets = false;
    step("sockets closed");
    // the next run's heap is what is plain read/write when it starts: give the services up to 3 s to let go of ours
    u64 mapped = HeapWith(MemAttr_IsDeviceMapped), lent = HeapWith(MemAttr_IsBorrowed);
    for (int i = 0; (mapped || lent) && i < 300; i++) {
        svcSleepThread(10'000'000);
        mapped = HeapWith(MemAttr_IsDeviceMapped);
        lent = HeapWith(MemAttr_IsBorrowed);
    }
    u64 recached = HeapRecache();
    if (f)
        std::fprintf(f, "%llu ms relaunch: %llu MB still mapped for the GPU, %llu MB still lent out, %llu MB cached again\n",
                     (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000), (unsigned long long)(mapped >> 20),
                     (unsigned long long)(lent >> 20), (unsigned long long)(recached >> 20));
    if (f) {
        std::fprintf(f, "relaunch: %d of %zu stacks back, %s thread\n", stacks, threads.size(),
                     self == envGetMainThreadHandle() ? "main" : "a guest");
        ReportHeap(f);
    }
    RestoreClocks();
    step("leaving for the next NRO");
    if (f) std::fclose(f);
    std::_Exit(0);
}
}  // namespace

void ServiceRelaunch() {
    if (g_leave.load()) LeaveForNextLoad();
}

void Exit(int code) {
    logx::Flush();
    if (g_sockets) socketExit();
    RestoreClocks();
    std::exit(code);
}

// hbloader starts the image's NRO after this one exits: sdmc:/switch/scda/scda.nro for default.xbe,
// scda_versus.nro for sc4_adv.xbe
void Relaunch(const char* exe, const std::string& extra) {
    std::string name = exe;
    for (char& c : name) c = char(std::tolower(uint8_t(c)));
    std::string path = (kHome / (name + ".nro")).string();
    std::string args = "\"" + path + "\" --relaunched";
    // a relaunch keeps the arguments, the co-op host's values are written afresh (kernel/boot.cpp)
    for (size_t at = 0; at < g_command_line.size();) {
        size_t end = g_command_line.find(' ', at);
        if (end == std::string::npos) end = g_command_line.size();
        std::string a = g_command_line.substr(at, end - at);
        if (!a.empty() && a != "--relaunched" && a.rfind("--coop-host=", 0) != 0) args += " " + a;
        at = end + 1;
    }
    if (!extra.empty()) args += " " + extra;
    Result rc = envHasNextLoad() ? envSetNextLoad(path.c_str(), args.c_str()) : Result(-1);
    if (R_FAILED(rc)) {
        LOGE("relaunch: the loader cannot start %s (0x%X); start it from the homebrew menu", path.c_str(), rc);
        Exit(1);
    }
    LOGI("relaunch: %s", args.c_str());
    Breadcrumb("relaunch");
    logx::Flush();
    // the main thread leaves (its event loop looks every 2 ms), so no thread's stack mirror is left in the heap; this
    // one waits to be paused, or leaves itself when the main thread does not come
    g_leave = true;
    if (threadGetCurHandle() != envGetMainThreadHandle()) {
        for (int i = 0; i < 300; i++) svcSleepThread(10'000'000);
        LOGW("relaunch: the main thread did not take over, leaving from this one");
    }
    LeaveForNextLoad();
}

}  // namespace port

extern "C" {
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio,
                           int cpuid);
Result __real_threadClose(Thread* t);
Result __real_condvarWaitTimeout(CondVar* c, Mutex* m, u64 timeout);

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz, int prio,
                           int cpuid) {
    Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
    std::lock_guard lock(port::g_threads_lock);
    if (R_SUCCEEDED(rc)) {
        port::g_threads.push_back(t);
        port::ReapEnded();
    } else {
        s64 used, limit;
        port::ThreadsInUse(&used, &limit);
        LOGE("thread: threadCreate failed 0x%X, the process holds %lld of %lld threads", rc, (long long)used,
             (long long)limit);
    }
    return rc;
}

Result __wrap_threadClose(Thread* t) {
    {
        std::lock_guard lock(port::g_threads_lock);
        auto& v = port::g_threads;
        v.erase(std::remove(v.begin(), v.end(), t), v.end());
    }
    return __real_threadClose(t);
}

Result __wrap_condvarWaitTimeout(CondVar* c, Mutex* m, u64 timeout) {
    int& slot = port::t_wait_slot;
    if (slot < 0) slot = port::g_wait_slots.fetch_add(1);
    if (slot >= port::kWaitSlots) return __real_condvarWaitTimeout(c, m, timeout);
    port::g_waits[slot].store(c);
    Result rc = __real_condvarWaitTimeout(c, m, timeout);
    port::g_waits[slot].store(nullptr);
    return rc;
}
}

// newlib's pthread_detach goes to this hook, which libnx leaves out: pthread_detach then fails with ENOSYS, and
// std::thread::detach throws ("Function not implemented"), on the first detached thread. A detached thread is
// never joined, so the thread it was keeps its stack and handle after it ends; the ones here run for the whole
// game (log, timers, sound, rendering), and kernel::Spawn's guest threads too.
extern "C" int __syscall_thread_detach(struct __pthread_t* p) {
    std::lock_guard lock(port::g_threads_lock);
    port::g_detached.push_back(reinterpret_cast<Thread*>(p));
    return 0;
}

// SDL's audio thread asks for SDL_THREAD_PRIORITY_TIME_CRITICAL, which switch-sdl2 makes 0x3B: the one time-sliced
// priority, where the busy guest threads keep it waiting in 10 ms slices while its two 21 ms wave buffers run dry
// (crackles). Wrapped at link time (--wrap, portable.cmake): time critical becomes 0x2B, as burnout3-sw found.
extern "C" int __real_SDL_SYS_SetThreadPriority(int prio);
extern "C" int __wrap_SDL_SYS_SetThreadPriority(int prio) {
    if (prio == 3 && R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B))) {
        LOGI("audio: SDL's thread at priority 0x2B");
        return 0;
    }
    return __real_SDL_SYS_SetThreadPriority(prio);
}

// libnx calls this for a CPU exception anywhere in the process: the host and guest state into the log, which is
// then written out, before the process ends. Host addresses are relative to the NRO's start, for addr2line on
// scda.elf.
extern "C" {
extern char __start__[];
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    uint64_t start = reinterpret_cast<uint64_t>(__start__), base = reinterpret_cast<uint64_t>(kernel::g_base);
    uint64_t far = ctx->far.x;
    if (port::LogToStderr())
        std::fprintf(stderr, "[crash] exception %u at pc elf +%llX lr elf +%llX far %llX\n", ctx->error_desc,
                     (unsigned long long)(ctx->pc.x - start), (unsigned long long)(ctx->lr.x - start),
                     (unsigned long long)far);
    // straight to a file of its own first: the log may not be open yet, or held by the thread that crashed
    char step[96];
    std::snprintf(step, sizeof(step), "CRASH: exception %u at pc elf +%llX", ctx->error_desc,
                  (unsigned long long)(ctx->pc.x - start));
    port::Breadcrumb(step);
    if (FILE* f = std::fopen("sdmc:/switch/scda/logs/crash.txt", "w")) {
        std::fprintf(f, "exception %u\npc  elf +%llX\nlr  elf +%llX\nsp  %llX\nfar %llX (guest %08llX)\n",
                     ctx->error_desc, (unsigned long long)(ctx->pc.x - start), (unsigned long long)(ctx->lr.x - start),
                     (unsigned long long)ctx->sp.x, (unsigned long long)far,
                     (unsigned long long)(base && far - base < (1ull << 32) ? far - base : 0));
        for (int i = 0; i < 29; i++) std::fprintf(f, "x%-2d %llX\n", i, (unsigned long long)ctx->cpu_gprs[i].x);
        std::fclose(f);
    }
    LOGE("crash: exception %u at pc %llX (elf +%llX), lr elf +%llX, far %llX%s%08llX", ctx->error_desc,
         (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - start),
         (unsigned long long)(ctx->lr.x - start), (unsigned long long)far,
         far - base < (1ull << 32) ? ", guest " : ", outside the guest ",
         (unsigned long long)(far - base < (1ull << 32) ? far - base : 0));
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("  on guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 128);
    }
    logx::TryFlush();
    svcExitProcess();
}
}
