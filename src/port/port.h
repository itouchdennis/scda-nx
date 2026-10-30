#pragma once
// What the Windows host asks of Win32 and the port asks of its platform: SDL2 on Linux (linux_*.cpp), SDL2 and libnx
// on the Switch (switch_*.cpp); the shared parts in the other files here.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace port {

// one of app::kLanguages from the system's language, "int" when the game lacks it
std::string SystemLanguage();
// the screen's size: the desktop mode on Linux, 1280x720 handheld or 1920x1080 docked on the Switch
uint32_t ScreenWidth();
uint32_t ScreenHeight();
uint32_t RefreshRate();  // 0 when unknown
// the folder the executable runs from: beside the binary on Linux, sdmc:/switch/scda on the Switch
std::filesystem::path HomeDir();
// a message the player must see before the game exits (no game files, a failed boot)
void Fatal(const std::string& title, const std::string& text);
// names the calling host thread for the debugger and the log
void NameThread(const char* name);
// a host thread's stack: recompiled code nests host frames as deep as the guest's calls. 64 MB of address space on
// Linux; on the Switch the stack is real heap, so less.
size_t ThreadStackSize();
// first thing on each new host thread: the Switch deals the threads over its cores (libnx puts all on core 0)
void ThreadStarted();
// the calling thread's time on a CPU so far, in ns (0 where unknown): set against wall time it shows waiting for a core
uint64_t ThreadCpuNs();
// the log goes to stderr too: always on Linux, on the Switch when started by nxlink -s (stderr is the PC then)
bool LogToStderr();
// a step of the start reached, written to boot.txt at once on the Switch (no-op elsewhere): what the log cannot
// show when the process dies before its first write
void Breadcrumb(const char* step);
// scda.toml's logging off: no boot.txt either (before Init; crash.txt is still written on a crash)
void SetBootLog(bool on);
// the platform's setup before anything else runs (log device, exception handler) and its exit (the Switch: back to the
// HOME menu, the other threads paused and the services closed first)
void Init(int argc, char** argv);
[[noreturn]] void Exit(int code);
// the player chose to quit (the game mode menu's Exit): the main loop ends, as closing the window does on Windows
void RequestQuit();
// starts another image of this title (front end to mission): the process ends, the next run is told it was relaunched;
// extra: arguments for it (--coop-host=rate,floor)
[[noreturn]] void Relaunch(const char* exe, const std::string& extra);
bool Relaunched();
#ifdef __SWITCH__
// each frame: the CPU at this many MHz (the CPU clock setting), the GPU and the memory at the most a game gets, set
// again when either changes or the console is docked or taken out
void KeepClocks(uint32_t cpu_mhz);
// the main thread's event loop: leaves once Relaunch (for the next NRO) or Exit (for the HOME menu) has been called
void ServiceRelaunch();
#endif
// the arguments this run started with, space separated (--coop-host= and the like, as on Windows)
const std::string& CommandLine();

// the Vulkan renderer's window: the instance extensions its surface needs, the surface, the size it presents at
std::vector<const char*> VulkanInstanceExtensions();
// instance is a VkInstance, surface receives a VkSurfaceKHR; false when the platform has none
bool CreateVulkanSurface(void* instance, uint64_t* surface);
// the Switch sizes its window (NWindow) itself, and the swap chain's images take that size; it changes only while no
// swap chain holds the window's buffers. Elsewhere the surface follows the window: never differs, nothing to set
bool SurfaceSizeDiffers(uint32_t w, uint32_t h);
void SetSurfaceSize(uint32_t w, uint32_t h);
void DrawableSize(uint32_t& w, uint32_t& h);

#ifdef SCDA_SWITCH
// the guest's 4 GB on Horizon (switch_memory.cpp): reserved address space, both physical windows on one RAM, and
// commits in whole granules; null or false when Horizon refuses
uint8_t* ReserveGuest();
bool MapPhysical(uint32_t first, uint32_t second, uint32_t size);
bool CommitGuest(uint32_t va, uint32_t len);
// the Switch, before a relaunch: every mapping of the guest space given back, the heap pages it lent the kernel
// with them (hbloader starts the next NRO in this process, on this heap); the guest's threads must be stopped
void ReleaseGuest();
void DecommitGuest(uint32_t va, uint32_t len);  // zeroed in place: the granule stays mapped
#endif

}  // namespace port
