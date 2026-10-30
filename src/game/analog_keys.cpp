// The stick becomes the digital IK_AnalogUp/Down/Left/Right key events only past the player's threshold (+0xEBC).
// Nothing in play is bound to them; the interaction list and other selectors step on them. The slower walking
// speeds of the move keys stay under the threshold, so a held move key counts as the full stick here.
#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kPadIndex = 0x1F4;  // viewport: the controller port it reads
constexpr uint32_t kAnalogUp = 0xC4;   // IK_AnalogUp, then Down, Left, Right

}  // namespace

// the stick as one IK_Analog key (thiscall viewport, float axis, int direction +1/-1, int key, ret 0xC)
X86_EXTERN_FUNC(__imp__sub_0004AF95);
X86_FUNC(sub_0004AF95) {
    uint32_t key = R32(ctx.esp + 12) - kAnalogUp;
    int keyboard = input::KeyboardPort();
    if (key < 4 && keyboard >= 0 && R32(ctx.ecx + kPadIndex) == uint32_t(keyboard) && input::StickKeys() >> key & 1)
        G<float>(ctx.esp + 4) = float(int32_t(R32(ctx.esp + 8)));
    __imp__sub_0004AF95(ctx, base);
}
