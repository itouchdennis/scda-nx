// Names (new profile) on the keyboard: on P_VirtualKeyboard the keys type instead of pressing the pad, Backspace
// deletes and Enter presses A, which accepts while the wheel rests on its centre. A typed character goes through the
// game's own rules: it must be on one of the page's character pages (all of them, so digits need no switch, a small
// letter types its capital), the name has a length limit, and a space needs a page that offers it. Only letters and
// digits type: the symbols on the digit page stay the pad's.
#include <cwctype>
#include <iterator>

#include "core/input.h"
#include "game/magma.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kPage = game::magma::Id("P_VirtualKeyboard");
// CVirtualKeyboard: +0x1C set up, +0x2C its page, +0x38 the name's text element, +0xB0 FString every character page,
// +0xC8 FString the name, +0xD4 its length limit, +0xE4 spaces allowed, +0xE5 the name is the one it opened with
// and the first character replaces it; FString: +0 data, +4 count with the null, +8 capacity
constexpr uint32_t kReady = 0x1C, kPageOf = 0x2C, kText = 0x38, kCharset = 0xB0, kName = 0xC8, kLimit = 0xD4;
constexpr uint32_t kSpaces = 0xE4, kOpening = 0xE5;
constexpr uint32_t kDelete = 0x1C6F3F, kAddSpace = 0x1C7007;  // thiscall CVirtualKeyboard, its X and white button
constexpr uint32_t kAppend = 0x370F1, kRealloc = 0x1E460F;    // thiscall FString: += const wchar_t*, (element size)

uint32_t g_live[4];  // constructed keyboards (game thread)
uint32_t g_chars = 0;
bool g_typing = false;

bool Offered(uint32_t kb, char16_t c) {
    uint32_t data = R32(kb + kCharset), count = R32(kb + kCharset + 4);
    for (uint32_t i = 0; data && i + 1 < count; i++)
        if (R16(data + i * 2) == c) return true;
    return false;
}

void Type(X86Context& ctx, uint32_t kb, char16_t c) {
    ctx.ecx = kb;
    if (c == u'\b') {
        CallGuest(ctx, kDelete, {});
        return;
    }
    if (c == u' ') {
        if (G<uint8_t>(kb + kSpaces)) CallGuest(ctx, kAddSpace, {});
        return;
    }
    c = char16_t(std::towupper(c));
    if (!((c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9')) || !Offered(kb, c)) return;
    if (G<uint8_t>(kb + kOpening)) {
        W32(kb + kName + 4, 0);
        W32(kb + kName + 8, 0);
        ctx.ecx = kb + kName;
        CallGuest(ctx, kRealloc, {2});
        G<uint8_t>(kb + kOpening) = 0;
    }
    uint32_t count = R32(kb + kName + 4);
    if ((count ? count - 1 : 0) >= R32(kb + kLimit)) return;
    // with a filler behind it the game's delete takes the filler off again and shows the name, with the key sound,
    // as for a letter from the wheel; it only deletes where the name is shown
    uint32_t text = R32(kb + kText);
    bool shown = text && R32(text + 8);
    W16(g_chars, c);
    W16(g_chars + 2, shown ? c : 0);
    W16(g_chars + 4, 0);
    ctx.ecx = kb + kName;
    CallGuest(ctx, kAppend, {g_chars});
    if (!shown) return;
    ctx.ecx = kb;
    CallGuest(ctx, kDelete, {});
}

}  // namespace

namespace game {

// game thread, every input update, with the page on top of the input stack
void NameEntryTick(X86Context& ctx, uint32_t top) {
    uint32_t kb = 0;
    if (top && R32(top + 0x14) == kPage)
        for (uint32_t k : g_live)
            if (k && R32(k + kPageOf) == top && G<uint8_t>(k + kReady)) kb = k;
    bool typing = kb != 0;
    if (typing != g_typing) LOGI("name entry: %s, keyboard %08X", typing ? "typing" : "off", kb);
    g_typing = typing;
    input::SetTextEntry(typing);
    if (!typing) return;
    if (!g_chars) g_chars = mem::AllocSystem(16);
    if (!g_chars) return;
    for (char16_t c : input::TakeChars()) Type(ctx, kb, c);
}

}  // namespace game

// CVirtualKeyboard's constructor and destructor (thiscall)
X86_EXTERN_FUNC(__imp__sub_001C6511);
X86_FUNC(sub_001C6511) {
    __imp__sub_001C6511(ctx, base);
    for (uint32_t& k : g_live)
        if (!k) {
            k = ctx.eax;
            return;
        }
    LOG_FIRST(1, "name entry: more than %zu keyboards", std::size(g_live));
}

X86_EXTERN_FUNC(__imp__sub_001C65E7);
X86_FUNC(sub_001C65E7) {
    for (uint32_t& k : g_live)
        if (k == ctx.ecx) k = 0;
    __imp__sub_001C65E7(ctx, base);
}
