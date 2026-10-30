// The guest's 4 GB on Horizon (kernel/memory.cpp's Switch path). The address space is a reservation in libnx's ASLR
// region, so every guest address lands at base + VA. Memory is mapped as sa2-nx does it: a heap block becomes module
// code memory at an address of its own (svcMapProcessCodeMemory, then made read-write) and that alias is mapped into
// the window with svcMapProcessMemory. A code-memory object per block (svcCreateCodeMemory) ran the kernel out of
// resources (0xCE01) after a few dozen: each keeps its page list for good. Commits come in 2 MB granules, and the
// guest's allocators fill their ranges from the bottom, so a few dozen cover a whole run. A decommitted range is
// zeroed in place, as a fresh commit reads.
// The two windows onto physical memory (0x80000000 and 0xF0000000, write combined) are the same pages. svcMapProcessMemory
// refuses heap memory as its source (0xD401, InvalidCurrentMemory; sa2-nx measured the same), so the backing first
// becomes module code memory at an address of its own (svcMapProcessCodeMemory, then made read-write), and both windows
// map that. Where the kernel refuses any step, the windows get memory of their own, which only matters to code that
// reaches the same physical page through both.
#include <switch.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "port/port.h"

namespace port {

namespace {

constexpr uint64_t kGuestSpace = 1ull << 32;
constexpr uint64_t kGranule = 2u << 20;

std::mutex g_lock;
uint8_t* g_base = nullptr;
std::vector<bool> g_mapped(kGuestSpace / kGranule, false);

// what ReleaseGuest undoes: views of a code alias over a heap block, or a code-memory object's view
struct Mapping {
    uint8_t* views[2];
    int n;
    void* alias;    // null: a code-memory object
    void* backing;
    Handle code;
    size_t len;
};
std::vector<Mapping> g_mappings;

// a code-memory view of fresh heap memory at p (where the kernel lacks the mapping calls MapViews needs)
bool MapFresh(void* p, size_t len) {
    void* src = aligned_alloc(0x1000, len);
    if (!src) {
        LOGE("memory: out of heap for %zu KB at %p", len >> 10, p);
        return false;
    }
    Handle h = 0;
    Result rc = svcCreateCodeMemory(&h, src, len);
    if (R_SUCCEEDED(rc)) rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, p, len, Perm_Rw);
    if (R_FAILED(rc)) {
        LOGE("memory: code-memory view of %zu KB at %p failed: 0x%X", len >> 10, p, rc);
        if (h) svcCloseHandle(h);
        std::free(src);
        return false;
    }
    std::memset(p, 0, len);
    g_mappings.push_back({{static_cast<uint8_t*>(p), nullptr}, 1, nullptr, src, h, len});
    return true;
}

bool CanMapViews() {
    return envGetOwnProcessHandle() != INVALID_HANDLE && envIsSyscallHinted(0x77) && envIsSyscallHinted(0x73) &&
           envIsSyscallHinted(0x74);
}

// fresh zeroed memory seen at every view in views[0..n), through a module code alias; a failure undoes what it did
Result MapViews(uint8_t* const* views, int n, size_t len) {
    Handle proc = envGetOwnProcessHandle();
    void* backing = aligned_alloc(kGranule, len);
    if (!backing) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    std::memset(backing, 0, len);
    virtmemLock();
    void* alias = virtmemFindCodeMemory(len, 0x1000);
    Result rc = alias ? svcMapProcessCodeMemory(proc, uint64_t(alias), uint64_t(backing), len)
                      : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) {
        std::free(backing);
        return rc;
    }
    // the backing is the kernel's now (no access until unmapped): only the alias is touched
    rc = svcSetProcessMemoryPermission(proc, uint64_t(alias), len, Perm_Rw);
    int mapped = 0;
    for (; R_SUCCEEDED(rc) && mapped < n; mapped++) {
        rc = svcMapProcessMemory(views[mapped], proc, uint64_t(alias), len);
        if (R_FAILED(rc)) break;
    }
    if (R_FAILED(rc)) {
        while (mapped--) svcUnmapProcessMemory(views[mapped], proc, uint64_t(alias), len);
        svcUnmapProcessCodeMemory(proc, uint64_t(alias), uint64_t(backing), len);
        std::free(backing);
        return rc;
    }
    Mapping m{{views[0], n > 1 ? views[1] : nullptr}, n, alias, backing, 0, len};
    g_mappings.push_back(m);
    return rc;
}

}  // namespace

uint8_t* ReserveGuest() {
    LOGI("memory: syscall hints MapProcessCodeMemory %d SetProcessMemoryPermission %d MapProcessMemory %d, "
         "own process handle %d",
         envIsSyscallHinted(0x77), envIsSyscallHinted(0x73), envIsSyscallHinted(0x74),
         envGetOwnProcessHandle() != INVALID_HANDLE);
    virtmemLock();
    void* at = virtmemFindAslr(kGuestSpace + 0x10000, 0x10000);
    if (at) virtmemAddReservation(at, kGuestSpace + 0x10000);
    virtmemUnlock();
    g_base = static_cast<uint8_t*>(at);
    Breadcrumb(g_base ? "guest space reserved" : "guest space NOT reserved");
    return g_base;
}

bool MapPhysical(uint32_t first, uint32_t second, uint32_t size) {
    std::lock_guard lock(g_lock);
    uint8_t* views[] = {g_base + first, g_base + second};
    Result rc = CanMapViews() ? MapViews(views, 2, size) : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    if (R_SUCCEEDED(rc)) {
        LOGI("memory: physical windows %08X and %08X alias (code alias, svcMapProcessMemory)", first, second);
    } else {
        LOGW("memory: physical windows cannot alias (0x%X): each gets memory of its own", rc);
        if (!MapFresh(g_base + first, size) || !MapFresh(g_base + second, size)) {
            Breadcrumb("physical memory NOT mapped");
            return false;
        }
    }
    Breadcrumb(R_SUCCEEDED(rc) ? "physical windows aliased" : "physical windows separate");
    for (uint64_t g = first / kGranule; g < (uint64_t(first) + size) / kGranule; g++) g_mapped[g] = true;
    for (uint64_t g = second / kGranule; g < (uint64_t(second) + size) / kGranule; g++) g_mapped[g] = true;
    return true;
}

bool CommitGuest(uint32_t va, uint32_t len) {
    std::lock_guard lock(g_lock);
    uint64_t end = uint64_t(va) + len;
    for (uint64_t g = va / kGranule; g * kGranule < end; g++) {
        if (g_mapped[g]) continue;
        uint8_t* view = g_base + g * kGranule;
        if (CanMapViews()) {
            Result rc = MapViews(&view, 1, kGranule);
            if (R_FAILED(rc)) {
                LOGE("memory: 2048 KB at %08llX failed: 0x%X", (unsigned long long)(g * kGranule), rc);
                return false;
            }
        } else if (!MapFresh(view, kGranule)) {
            return false;
        }
        g_mapped[g] = true;
    }
    return true;
}

// no lock, no log, no allocation: the threads that might hold them are paused for good
void ReleaseGuest() {
    Handle proc = envGetOwnProcessHandle();
    for (const Mapping& m : g_mappings) {
        if (m.alias) {
            for (int i = 0; i < m.n; i++) svcUnmapProcessMemory(m.views[i], proc, uint64_t(m.alias), m.len);
            svcUnmapProcessCodeMemory(proc, uint64_t(m.alias), uint64_t(m.backing), m.len);
        } else {
            svcControlCodeMemory(m.code, CodeMapOperation_UnmapOwner, m.views[0], m.len, Perm_None);
            svcCloseHandle(m.code);
        }
    }
}

void DecommitGuest(uint32_t va, uint32_t len) { std::memset(g_base + va, 0, len); }

}  // namespace port
