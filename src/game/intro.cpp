// Skip intros: the boot state of the front end plays LogosVideo, then shows esrb.tga for 3 s, then the
// SplashScreen (SplinterCellLogo.tga) that stays up while the menu loads. The Xbox build has no -nointro switch,
// so the two calls are dropped at their callees.
#include <cwctype>
#include <string>

#include "app/app.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// front end config: +4 LogosVideo, +0x10 SplashScreen, +0x1C SplashVideo, +0x28 IntroVideo (FString data, count, max)
constexpr uint32_t kLogosVideo = 0x651340 + 0x04;

std::wstring GuestString(uint32_t va) {
    std::wstring s;
    for (uint16_t c; va && s.size() < 260 && (c = R16(va)); va += 2) s += wchar_t(std::towlower(c));
    return s;
}

}  // namespace

// front end PlayVideo(name, ...), thiscall, 5 arguments
X86_EXTERN_FUNC(__imp__sub_00039AD4);
X86_FUNC(sub_00039AD4) {
    uint32_t name = R32(ctx.esp + 4);
    if (app::Get().skip_intros && name && name == R32(kLogosVideo)) {
        LOGI("intro: skipped the logos video");
        ctx.eax = 0;
        ctx.esp += 4 + 5 * 4;
        return;
    }
    __imp__sub_00039AD4(ctx, base);
}

// front end ShowScreen(path, ...), thiscall, 4 arguments; a zero return also skips the rating screen's wait
X86_EXTERN_FUNC(__imp__sub_00037274);
X86_FUNC(sub_00037274) {
    if (app::Get().skip_intros) {
        std::wstring path = GuestString(R32(ctx.esp + 4));
        if (path.ends_with(L"esrb.tga")) {
            LOGI("intro: skipped the rating screen");
            ctx.eax = 0;
            ctx.esp += 4 + 4 * 4;
            return;
        }
    }
    __imp__sub_00037274(ctx, base);
}
