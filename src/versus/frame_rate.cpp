// Before each swap the render device sleeps until 2 / refresh less 1 ms has passed since the last
// one: 31 fps at the console's 60 Hz. A huge refresh for that call turns it off, and app::Pace caps the frame.
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kRefresh = 0x46D8;  // render device int, the display refresh rate (60, 50 on PAL)

}  // namespace

// render device present: the frame limit, then D3DDevice_Swap (thiscall device, one argument)
X86_EXTERN_FUNC(__imp__sub_001E4C90);
X86_FUNC(sub_001E4C90) {
    uint32_t device = ctx.ecx;
    uint32_t refresh = R32(device + kRefresh);
    W32(device + kRefresh, 0x7FFFFFFF);
    __imp__sub_001E4C90(ctx, base);
    W32(device + kRefresh, refresh);
}
