// The OPSAT key closes the OPSAT from any of its pages, as SCCT's PC version does. On Xbox the key does nothing
// inside it, and B / crouch only backs out one page.
#include <cwctype>
#include <string_view>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// COpsatMenu: +0x18 its manager (+0x14 input handled), +0x18C open. Only opening the OPSAT sets it, so a training
// video from its list closes with it, and one from a PLAY VIDEO interaction (the same page, the OPSAT closed) does not.
constexpr uint32_t kManager = 0x18, kHandled = 0x14, kOpen = 0x18C;
// the full close the in-game menus run when the player controller resets the OPSAT (NotifyMenu(Opsat, 0, 1))
constexpr uint32_t kClose = 0x19A1B0;    // thiscall COpsatMenu
// P_Goals: +0x188 the tab shown (0 Primary, 1 Opposing, 2 Opportunity). The input handler steps it with NextTab /
// PreviousTab (which pass Opposing by where a mission has none) and refills the list (thiscall COpsatMenu each).
constexpr uint32_t kTab = 0x188, kNextTab = 0x192334, kFillGoals = 0x194C19;
uint32_t g_goals = 0;  // the COpsatMenu that last filled P_Goals
constexpr uint32_t kEventAction = 0x0C;  // input event: FString* bound action

bool IsAction(uint32_t event, std::wstring_view name) {
    uint32_t fs = event ? R32(event + kEventAction) : 0;
    uint32_t data = fs ? R32(fs) : 0, count = fs ? R32(fs + 4) : 0;
    if (!data || count != name.size() + 1) return false;
    for (size_t i = 0; i < name.size(); i++)
        if (std::towlower(wchar_t(R16(data + uint32_t(i) * 2))) != std::towlower(name[i])) return false;
    return true;
}

}  // namespace

// COpsatMenu input handler (thiscall, the element, the input event, ret 8, returns handled)
X86_EXTERN_FUNC(__imp__sub_00199620);
X86_FUNC(sub_00199620) {
    uint32_t menu = ctx.ecx;
    if (G<uint8_t>(menu + kOpen) && IsAction(R32(ctx.esp + 8), L"OpsatMenu")) {
        LOGI("opsat: closing");
        CallGuest(ctx, kClose, {});
        G<uint8_t>(R32(menu + kManager) + kHandled) = 1;
        ctx.eax = 1;
        ctx.esp += 12;  // ret 8
        return;
    }
    __imp__sub_00199620(ctx, base);
}

// P_Goals' list fill (thiscall COpsatMenu), as the page shows and on each tab change
X86_EXTERN_FUNC(__imp__sub_00194C19);
X86_FUNC(sub_00194C19) {
    g_goals = ctx.ecx;
    __imp__sub_00194C19(ctx, base);
}

namespace game {

// mouse.cpp: a click on a P_Goals tab goes there through NextTab, as the pad's steps do; a tab the mission passes
// over is left alone
void GoalsTab(X86Context& ctx, uint32_t tab) {
    uint32_t menu = g_goals, from = menu ? R32(menu + kTab) : tab;
    if (!menu || from == tab) return;
    for (int i = 0; i < 3 && R32(menu + kTab) != tab; i++) {
        ctx.ecx = menu;
        CallGuest(ctx, kNextTab, {});
    }
    if (R32(menu + kTab) != tab) {
        W32(menu + kTab, from);
        return;
    }
    ctx.ecx = menu;
    CallGuest(ctx, kFillGoals, {});
}

}  // namespace game
