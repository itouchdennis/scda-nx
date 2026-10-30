// Xbox Live is gone from the versus menus: the sign-in status after the profile name, the X sign-in key and its
// legend on the main menu, the XBOX LIVE button, whose place goes to System Link, renamed LAN, and Appear Offline,
// which leaves the "System Link / Xbox Live" options page to voice alone, renamed Voice. The save box speaks of the PC.
// Deleting this file brings them back.
#include <array>
#include <string_view>

#include "game/magma.h"
#include "xdk/hle.h"

using namespace kernel;

namespace versus {
uint32_t OptionsText(uint32_t section, uint32_t section2, uint32_t key);  // options_menu.cpp
uint32_t KeyText(uint32_t text);                                          // prompts.cpp
}  // namespace versus

namespace {

// localization database find (thiscall(section, section2, key*, package), ret 0x10): returns the text in the table.
// Sections and keys are cooked name indices, or crc32 of the lowercase name for Magma pages and elements.
struct Text {
    uint32_t section;
    uint32_t key;
    std::u16string_view text;
};
constexpr uint32_t kMagmaSection = 0x178B;  // [Magma]
using game::magma::Id;
constexpr uint32_t kMainMenu = Id("p_mainmenu");
constexpr uint32_t kSystemLinkPage = Id("p_systemlink");
constexpr uint32_t kOptionsPage = Id("p_options");
constexpr uint32_t kOnlineOptionsPage = Id("p_options_online");
constexpr uint32_t kXboxLive = Id("xboxlive");
constexpr uint32_t kSystemLink = Id("systemlink");
constexpr uint32_t kTutorials = Id("tutorials");
constexpr uint32_t kOptionsButton = Id("options");
constexpr uint32_t kTitle = Id("title");
constexpr uint32_t kOnline = Id("online");
static_assert(kMainMenu == 0x8BBFE343 && kSystemLinkPage == 0xE12C2CDF && kOptionsPage == 0xD7F513E8);
static_assert(kOnlineOptionsPage == 0xE95B9F21 && kXboxLive == 0x44F5EDB2 && kSystemLink == 0xB67F2AD0);
static_assert(kTutorials == 0xC21EFE28 && kOptionsButton == 0xD035FA87 && kTitle == 0x2B36786B);
static_assert(kOnline == 0x9E32BEEA);
constexpr Text kTexts[] = {
    {kMagmaSection, 0x5065, u""},  // Live_NoSignIn "Not signed In"
    {kMagmaSection, 0x5064, u""},  // Live_FailedSignIn "Sign-In Failed"
    {kMagmaSection, 0x4E83, u"Please don't close the game or turn off your PC."},  // PopUp_MC_Body, the save box
    {kMainMenu, kSystemLink, u"LAN"},
    {kSystemLinkPage, kTitle, u"LAN"},
    {kOptionsPage, kOnline, u"VOICE"},
    {kOnlineOptionsPage, kTitle, u"Voice Options"},
};

// Magma localize by [Magma] key (cdecl(key)); the main menu asks it for the X legend just before this return address
constexpr uint32_t kSignInLegend = 0x1A764E;
constexpr uint32_t kNullText = 0x4F05;

// Menu pages: +0x28 the Magma page; P_Options_Online +0xC8 CHK_Appear. Page: +0xC begin, +0x10 end of instance
// pointers, +0x24 focus.
// Button and checkbox instance: +8 id, +0x10 widget (+4 its current position frame), +0x18 begin, +0x1C end of
// keyframe pointers (keyframe +0x18 its position frame), +0xA8 up and +0xAC down neighbour instances, +0xB8 bit 0
// default.
// Position frame (vtable 0x361FC8): +0xE y (short, page units).
constexpr uint32_t kMenuPage = 0x28;
constexpr uint32_t kAppear = 0xC8;
// page
constexpr uint32_t kPageBegin = 0x0C;
constexpr uint32_t kPageEnd = 0x10;
constexpr uint32_t kPageFocused = 0x24;
// instance
constexpr uint32_t kInstanceId = 0x08;
constexpr uint32_t kInstanceWidget = 0x10;
constexpr uint32_t kKeyframes = 0x18;
constexpr uint32_t kKeyframesEnd = 0x1C;
constexpr uint32_t kKeyFrame = 0x18;
constexpr uint32_t kWidgetFrame = 0x04;
constexpr uint32_t kNeighbourUp = 0xA8;
constexpr uint32_t kNeighbourDown = 0xAC;
constexpr uint32_t kInstanceFlags = 0xB8;
constexpr uint32_t kDefault = 0x01;
constexpr uint32_t kMaxKeyframes = 64;
// position frame
constexpr uint32_t kPositionFrameVtable = 0x361FC8;
constexpr uint32_t kFrameY = 0x0E;
constexpr uint32_t kSetEnabled = 0x25CFB0;  // thiscall instance (bool)
constexpr uint32_t kResetFocus = 0x25C1D0;  // cdecl(page): the default enabled button takes the focus
constexpr int kOffPage = -1000;             // no hidden flag in this Magma: the button is moved off the canvas

uint32_t Guest(std::u16string_view text) {
    uint32_t va = mem::AllocSystem(uint32_t(text.size() + 1) * 2);
    for (size_t i = 0; i < text.size(); i++) W16(va + uint32_t(i) * 2, text[i]);
    W16(va + uint32_t(text.size()) * 2, 0);
    return va;
}

// the texts' guest copies, made on the first lookup that finds one
uint32_t GuestText(size_t index) {
    static const auto guest = [] {
        std::array<uint32_t, std::size(kTexts)> va{};
        for (size_t i = 0; i < va.size(); i++) va[i] = Guest(kTexts[i].text);
        return va;
    }();
    return guest[index];
}

uint32_t Instance(uint32_t page, uint32_t id) {
    for (uint32_t it = R32(page + kPageBegin), end = R32(page + kPageEnd); it && it < end; it += 4)
        if (uint32_t inst = R32(it); inst && R32(inst + kInstanceId) == id) return inst;
    return 0;
}

template <typename F>
void EachFrame(uint32_t inst, F f) {
    uint32_t begin = R32(inst + kKeyframes);
    uint32_t end = R32(inst + kKeyframesEnd);
    if (begin && end >= begin && end - begin <= kMaxKeyframes * 4)
        for (uint32_t it = begin; it < end; it += 4)
            if (uint32_t key = R32(it)) f(R32(key + kKeyFrame));
    if (uint32_t widget = R32(inst + kInstanceWidget)) f(R32(widget + kWidgetFrame));
}

constexpr int kNoY = 0x7FFF;  // no position frame seen yet

int FirstY(uint32_t inst) {
    int y = kNoY;
    EachFrame(inst, [&](uint32_t frame) {
        if (y == kNoY && frame && R32(frame) == kPositionFrameVtable) y = int16_t(R16(frame + kFrameY));
    });
    return y;
}

void Move(uint32_t inst, int dy) {
    EachFrame(inst, [&](uint32_t frame) {
        if (frame && R32(frame) == kPositionFrameVtable)
            W16(frame + kFrameY, uint16_t(int16_t(R16(frame + kFrameY)) + dy));
    });
}

constexpr int kMaxRow = 100;  // a step this tall or more is no row step

// Takes a button out of its page: disabled and off the canvas, the rows under it up one, its neighbours linked past
// it, the default and the focus to the one below. Returns the row height, 0 when the page was done already.
int Remove(X86Context& ctx, uint32_t page, uint32_t gone, std::initializer_list<uint32_t> below) {
    uint32_t up = R32(gone + kNeighbourUp);
    uint32_t down = R32(gone + kNeighbourDown);
    if (!up || !down || R32(down + kNeighbourUp) != gone || !below.size()) return 0;
    int row = FirstY(*below.begin()) - FirstY(gone);
    if (row > 0 && row < kMaxRow)
        for (uint32_t inst : below) Move(inst, -row);
    Move(gone, kOffPage);
    ctx.ecx = gone;
    CallGuest(ctx, kSetEnabled, {0});
    if (G<uint8_t>(gone + kInstanceFlags) & kDefault) {
        G<uint8_t>(gone + kInstanceFlags) &= ~kDefault;
        G<uint8_t>(down + kInstanceFlags) |= kDefault;
    }
    W32(up + kNeighbourDown, down);
    W32(down + kNeighbourUp, up);
    W32(page + kPageFocused, 0);
    CallGuest(ctx, kResetFocus, {page});
    return row;
}

}  // namespace

// the localization database find
X86_EXTERN_FUNC(__imp__sub_00157A90);
X86_FUNC(sub_00157A90) {
    uint32_t section = R32(ctx.esp + 4);
    uint32_t section2 = R32(ctx.esp + 8);
    uint32_t key = R32(ctx.esp + 0xC);
    uint32_t id = key ? R32(key) : 0;
    if (uint32_t text = versus::OptionsText(section, section2, id)) {
        ctx.eax = text;
        ctx.esp += 0x14;
        return;
    }
    for (auto& t : kTexts) {
        if ((section != t.section && section2 != t.section) || id != t.key) continue;
        ctx.eax = GuestText(size_t(&t - kTexts));
        ctx.esp += 0x14;
        return;
    }
    __imp__sub_00157A90(ctx, base);
    if (uint32_t text = versus::KeyText(ctx.eax)) ctx.eax = text;
}

// Magma localize by [Magma] key
X86_EXTERN_FUNC(__imp__sub_00192390);
X86_FUNC(sub_00192390) {
    if (R32(ctx.esp) == kSignInLegend) W32(ctx.esp + 4, kNullText);
    __imp__sub_00192390(ctx, base);
}

// main menu button keys (thiscall(?, event), ret 8): event 0x82 is X, which signs in or out
constexpr uint32_t kSignInEvent = 0x82;
X86_EXTERN_FUNC(__imp__sub_001A71C0);
X86_FUNC(sub_001A71C0) {
    if (R32(R32(ctx.esp + 8)) != kSignInEvent) return __imp__sub_001A71C0(ctx, base);
    ctx.eax = (ctx.eax & ~0xFFu) | 1;  // al = 1: handled
    ctx.esp += 0xC;
}

// P_MainMenu open (thiscall, no arguments): XBOX LIVE goes, System Link, Tutorials and Options move up a row
X86_EXTERN_FUNC(__imp__sub_001A72F0);
X86_FUNC(sub_001A72F0) {
    uint32_t menu = ctx.ecx;
    __imp__sub_001A72F0(ctx, base);
    uint32_t page = R32(menu + kMenuPage);
    if (!page) return;
    uint32_t live = Instance(page, kXboxLive);
    uint32_t lan = Instance(page, kSystemLink);
    uint32_t tutorials = Instance(page, kTutorials);
    uint32_t options = Instance(page, kOptionsButton);
    if (!live || !lan || !tutorials || !options) return;
    if (int row = Remove(ctx, page, live, {lan, tutorials, options}))
        LOGI("versus: XBOX LIVE button removed, LAN / Tutorials / Options up %d", row);
}

// P_Options_Online open (thiscall, no arguments): Appear Offline goes, Voice On Speaker moves up a row
X86_EXTERN_FUNC(__imp__sub_001C4910);
X86_FUNC(sub_001C4910) {
    uint32_t menu = ctx.ecx;
    __imp__sub_001C4910(ctx, base);
    uint32_t page = R32(menu + kMenuPage);
    uint32_t appear = R32(menu + kAppear);
    if (!page || !appear) return;
    if (int row = Remove(ctx, page, appear, {R32(appear + kNeighbourDown)}))
        LOGI("versus: Appear Offline removed, Voice On Speaker up %d", row);
}
