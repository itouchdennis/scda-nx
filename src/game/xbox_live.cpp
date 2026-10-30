// Xbox Live is gone from the menus: the sign-in status line over every menu, the Xbox Live item of the solo main
// menu and the pause menu, and the Xbox Live button of the co-op main menu, whose LAN button takes the top row.
// Deleting this file brings them back.
#include "xdk/hle.h"

using namespace kernel;

namespace {

// Live status updater (thiscall, ret 8): +0x2734 the L_username text of system.mgm's A_Panel_Live area, the
// "SIGN-IN FAILED" / "NOT SIGNED IN" line that every menu page instances
constexpr uint32_t kStatusText = 0x2734;
// instance: +4 id, +0x18 bit 0 visible, +0x1C up and +0x20 down neighbour ids, +0x3C up and +0x40 down neighbour
// instances (resolved from the ids at load, what navigation follows), +0x4C bit 0 default;
// +0x0C keyframes: +4 begin, +8 end of keyframe pointers; keyframe +4 its frame; position frame (vtable 0x5681C4)
// +8 x, +0xA y (short)
constexpr uint32_t kId = 0x04, kVisible = 0x18, kUp = 0x1C, kDown = 0x20, kFlags = 0x4C, kDefault = 0x01;
constexpr uint32_t kUpInstance = 0x3C, kDownInstance = 0x40;
constexpr uint32_t kKeyframes = 0x0C, kFrame = 0x04, kPositionFrame = 0x5681C4, kY = 0x0A;
constexpr uint32_t kSetEnabled = 0x2C87E0;  // thiscall instance (bool)
// co-op menu: +0x24 P_MainCOOP, then its buttons B_Online, B_Split, B_LAN, B_Options
constexpr uint32_t kOnline = 0x28, kSplit = 0x2C, kLan = 0x30, kOptions = 0x34;
constexpr uint32_t kOnlineName = 0x546578, kOptionsName = 0x546598;
// return addresses of the calls that add the Xbox Live items
constexpr uint32_t kSoloItem = 0x1A67DA;   // solo main menu AddItem(text, 0), the last of its list
constexpr uint32_t kPauseItem = 0x18CBC0;  // pause menu item (list, id 8, FString), by value
constexpr uint32_t kFStringFree = 0x36116;

template <typename F>
void EachFrame(uint32_t instance, F f) {
    uint32_t keys = R32(instance + kKeyframes);
    uint32_t begin = keys ? R32(keys + 4) : 0, end = keys ? R32(keys + 8) : 0;
    if (!begin || end < begin || end - begin > 4 * 64) return;
    for (uint32_t it = begin; it < end; it += 4) {
        uint32_t key = R32(it), frame = key ? R32(key + kFrame) : 0;
        if (frame && R32(frame) == kPositionFrame) f(frame);
    }
}

int FirstY(uint32_t instance) {
    int y = 0x7FFF;
    EachFrame(instance, [&](uint32_t frame) {
        if (y == 0x7FFF) y = int16_t(R16(frame + kY));
    });
    return y;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00291D64);
X86_FUNC(sub_00291D64) {
    uint32_t self = ctx.ecx;
    __imp__sub_00291D64(ctx, base);
    if (uint32_t text = R32(self + kStatusText)) G<uint8_t>(text + kVisible) &= ~1;
}

// list AddItem (thiscall(text, flag), ret 8, returns the index)
X86_EXTERN_FUNC(__imp__sub_002C5160);
X86_FUNC(sub_002C5160) {
    if (R32(ctx.esp) != kSoloItem) return __imp__sub_002C5160(ctx, base);
    ctx.esp += 0xC;  // ret 8: the item is never added
}

// pause menu AddItem (stdcall(list, id, FString), ret 0x14): the FString is the callee's to free
X86_EXTERN_FUNC(__imp__sub_0018B00D);
X86_FUNC(sub_0018B00D) {
    if (R32(ctx.esp) != kPauseItem) return __imp__sub_0018B00D(ctx, base);
    ctx.ecx = ctx.esp + 0xC;
    CallGuest(ctx, kFStringFree, {});
    ctx.esp += 0x18;  // ret 0x14
}

// co-op menu page lookup (thiscall, returns al): the co-op buttons are placed in main.mgm, not listed, so B_Online
// is hidden and disabled, and the other three are laid out again from its row as LAN, Split Screen, Options
X86_EXTERN_FUNC(__imp__sub_001A7137);
X86_FUNC(sub_001A7137) {
    uint32_t menu = ctx.ecx;
    __imp__sub_001A7137(ctx, base);
    uint32_t online = R32(menu + kOnline), split = R32(menu + kSplit), lan = R32(menu + kLan);
    uint32_t options = R32(menu + kOptions);
    if (!(ctx.eax & 0xFF) || !online || !split || !lan || !options) return;
    if (R32(split + kUp) != R32(online + kId)) return;  // this page is done
    int top = FirstY(online), row = FirstY(split) - top;
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.ecx = online;
    CallGuest(ctx, kSetEnabled, {0});
    ctx.eax = eax, ctx.ecx = ecx;
    G<uint8_t>(online + kVisible) &= ~1;
    if (G<uint8_t>(online + kFlags) & kDefault) {
        G<uint8_t>(online + kFlags) &= ~kDefault;
        G<uint8_t>(lan + kFlags) |= kDefault;
    }
    const uint32_t order[] = {lan, split, options};
    for (int i = 0; i < 3; i++) {
        W32(order[i] + kUp, R32(order[(i + 2) % 3] + kId));
        W32(order[i] + kDown, R32(order[(i + 1) % 3] + kId));
        W32(order[i] + kUpInstance, order[(i + 2) % 3]);
        W32(order[i] + kDownInstance, order[(i + 1) % 3]);
        if (row <= 0 || row >= 100) continue;
        int move = top + i * row - FirstY(order[i]);
        EachFrame(order[i], [&](uint32_t frame) { W16(frame + kY, uint16_t(int16_t(R16(frame + kY)) + move)); });
    }
    LOGI("xbox live: co-op button hidden, LAN / Split Screen / Options from y %d, row %d", top, row);
}

// focus by name (cdecl(page, name)): the co-op menu falls back to B_Online when Split Screen is disabled under focus
X86_EXTERN_FUNC(__imp__sub_001A6132);
X86_FUNC(sub_001A6132) {
    if (R32(ctx.esp + 8) == kOnlineName) W32(ctx.esp + 8, kOptionsName);
    __imp__sub_001A6132(ctx, base);
}
