// The menu interaction tick hands Magma whole milliseconds, truncated from the frame time. Above 1000 fps every
// frame truncates to 0 and menus stop; below that they run slow. The dropped fraction is carried to the next frame.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "xdk/hle.h"

using namespace kernel;

// ProcessTick(float seconds), thiscall: ftol(seconds * 1000) to Magma, then the seconds to a virtual
X86_EXTERN_FUNC(__imp__sub_002303AF);
X86_FUNC(sub_002303AF) {
    static double carry = 0.0;
    float seconds;
    std::memcpy(&seconds, HostPtr(ctx.esp + 4), 4);
    double ms = std::max(0.0, double(seconds)) * 1000.0 + carry;
    double whole = std::floor(ms);
    carry = ms - whole;
    // half a millisecond over, so the guest's truncation lands on whole
    seconds = float((whole + 0.5) / 1000.0);
    std::memcpy(HostPtr(ctx.esp + 4), &seconds, 4);
    __imp__sub_002303AF(ctx, base);
}
