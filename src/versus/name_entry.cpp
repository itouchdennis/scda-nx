// Names on the keyboard in versus, as in story: on P_VirtualKeyboard the keys type instead of pressing the pad,
// Backspace deletes and Enter presses A, which accepts while the wheel rests on its centre. Only letters and digits
// type, a space only where the keyboard allows one; versus's wheel has small letters too, so the case is kept. The
// game's own append keeps its length limit.
#include <iterator>

#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// keyboard (vtable 0x339140): +0x28 its page, +0xC8 spaces allowed
constexpr uint32_t kPageOf = 0x28;
constexpr uint32_t kSpaces = 0xC8;
// thiscall keyboard: (const wchar_t* character), ()
constexpr uint32_t kAppend = 0x1AE800;
constexpr uint32_t kDelete = 0x1AE8E0;

uint32_t g_live[4];  // constructed keyboards (game thread)
uint32_t g_chars = 0;
bool g_typing = false;

void Type(X86Context& ctx, uint32_t kb, char16_t c) {
    ctx.ecx = kb;
    if (c == u'\b') {
        CallGuest(ctx, kDelete, {});
        return;
    }
    bool typed = (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
    if (!typed && !(c == u' ' && G<uint8_t>(kb + kSpaces))) return;
    W16(g_chars, c);
    W16(g_chars + 2, 0);
    CallGuest(ctx, kAppend, {g_chars});
}

void Track(uint32_t kb) {
    for (uint32_t& k : g_live)
        if (!k) {
            k = kb;
            return;
        }
    LOG_FIRST(1, "name entry: more than %zu keyboards", std::size(g_live));
}

}  // namespace

namespace game {

// game thread, every input update, with the page on top of the input stack
void NameEntryTick(X86Context& ctx, uint32_t top) {
    uint32_t kb = 0;
    for (uint32_t k : g_live)
        if (top && k && R32(k + kPageOf) == top) kb = k;
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

// the keyboard's two constructors (thiscall, return this) and its destructor
X86_EXTERN_FUNC(__imp__sub_001ADE90);
X86_FUNC(sub_001ADE90) {
    __imp__sub_001ADE90(ctx, base);
    Track(ctx.eax);
}

X86_EXTERN_FUNC(__imp__sub_001AE0A0);
X86_FUNC(sub_001AE0A0) {
    __imp__sub_001AE0A0(ctx, base);
    Track(ctx.eax);
}

X86_EXTERN_FUNC(__imp__sub_001AE2F0);
X86_FUNC(sub_001AE2F0) {
    for (uint32_t& k : g_live)
        if (k == ctx.ecx) k = 0;
    __imp__sub_001AE2F0(ctx, base);
}
