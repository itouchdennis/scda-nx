// The profile boxes (Reading Profile List, Loading Profile, and the other "Please don't close the game" boxes of the
// profile menu) stay up at least 3.5 s whatever the drive does, as SCCT's Versus timers held "Loading profiles...";
// here 1.0 s. The menu's tick waits while its box is up until the operation's start time (+0xFC) plus 3.5
// (0x3381E0, a float the compiler shares with game code, so it stays), then lets the operation finish.
#include <chrono>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// profile menu: +0xFC the operation's start time (float seconds over a 2^24 offset, so 2 s steps), +0x120 its box up
constexpr uint32_t kStart = 0xFC;
constexpr uint32_t kBoxUp = 0x120;
constexpr auto kShown = std::chrono::milliseconds(1000);

struct Box {
    uint32_t menu = 0;
    uint32_t start = 0;  // the start time's bits when the box came up (a next operation writes its own)
    std::chrono::steady_clock::time_point since;
};
Box g_box;  // game thread

}  // namespace

// the profile menu's tick (thiscall menu, 2 arguments, ret 8): a box up for kShown has its start time cleared, which
// passes the 3.5 s wait; the box still closes only when the operation is done
X86_EXTERN_FUNC(__imp__sub_001A06F0);
X86_FUNC(sub_001A06F0) {
    uint32_t menu = ctx.ecx;
    uint32_t start = R32(menu + kStart);
    auto now = std::chrono::steady_clock::now();
    if (!G<uint8_t>(menu + kBoxUp)) {
        g_box = {};
    } else if (g_box.menu != menu || (start && start != g_box.start)) {
        g_box = {menu, start, now};
    } else if (now - g_box.since >= kShown) {
        W32(menu + kStart, 0);  // 0.0f
    }
    __imp__sub_001A06F0(ctx, base);
}
