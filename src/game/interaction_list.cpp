// The HUD's interaction list. The interact key focuses it (the box with BACK TO GAME on top); while it is focused
// its key handler steps it on the AnalogUp / AnalogDown aliases, which the mouse wheel taps there (mouse.cpp),
// as SCCT PC's handler takes IK_MouseWheelUp / Down. A synced session has both players' lists on each machine.
#include <array>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// interaction list: vtable 0x5491D0, +0x1C its player's menus manager (+0x0C the player number), +0x22 focused
constexpr uint32_t kListVtable = 0x5491D0;
constexpr uint32_t kOwner = 0x1C, kOwnerPlayer = 0x0C, kFocused = 0x22;

std::array<uint32_t, 3> g_lists{};  // the list focused last: [0] any player's, [n] player n's

bool Focused(uint32_t list) { return list && R32(list) == kListVtable && G<uint8_t>(list + kFocused); }

}  // namespace

namespace game {

bool InteractionListFocused(uint32_t player) { return Focused(g_lists[player < g_lists.size() ? player : 0]); }

}  // namespace game

// the list's focus (thiscall, no args)
X86_EXTERN_FUNC(__imp__sub_001B9755);
X86_FUNC(sub_001B9755) {
    uint32_t list = ctx.ecx;
    __imp__sub_001B9755(ctx, base);
    if (!G<uint8_t>(list + kFocused)) return;
    g_lists[0] = list;
    uint32_t owner = R32(list + kOwner), player = owner ? R32(owner + kOwnerPlayer) : 0;
    if (player < g_lists.size()) g_lists[player] = list;
}
