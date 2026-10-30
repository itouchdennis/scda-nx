// Co-op mouse look. The lockstep records only the six pad axes (Net-Z PlayerInput::SetAxis sub_002A4114 takes
// IK_JoyX/Y/Z/R/U/V) and replays every player's input through UInput::DirectAxis on each machine, so a mouse axis
// event turned only the local copy and the session desynced. The mouse rides on IK_JoyR and IK_JoyU, the trigger
// axes nothing binds: UXboxViewport::UpdateInput records them each update, and the mouse takes the triggers' place
// there, recorded at the same moment as the sticks. The replay turns them back into IK_MouseX and IK_MouseY, whose
// binding (Axis aMouseX Speed=0.001) then runs as in single player, on both machines.
#include <utility>

#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kJoyR = 0xE3, kJoyU = 0xE8, kMouseX = 0xE4, kMouseY = 0xE5;
// UInput: +0xEEC engine; it records instead of acting while +0xECC and +0xEDC are set and +0xED0 is clear.
// Engine: +0xC8 and +0xCC, or +0xDC, in a synced session; split screen sets +0xCC alone, and there the triggers
// must stay triggers.
constexpr uint32_t kEngine = 0xEEC, kRecordA = 0xECC, kRecordB = 0xEDC, kReplaying = 0xED0;
constexpr uint32_t kSessionNet = 0xC8, kSessionA = 0xCC, kSessionB = 0xDC;

float g_dy = 0.0f;  // game thread: UpdateInput sends IK_JoyR, then IK_JoyU

float Mouse(uint32_t key) {
    if (key == kJoyU) return std::exchange(g_dy, 0.0f);
    float dx = 0.0f, dy = 0.0f;
    if (!input::TakeMouse(dx, dy)) dx = dy = 0.0f;
    g_dy = dy;
    return dx;
}

}  // namespace

// UInput::DirectAxis(key, length, delta), thiscall
X86_EXTERN_FUNC(__imp__sub_00246AD6);
X86_FUNC(sub_00246AD6) {
    uint32_t self = ctx.ecx, key = R32(ctx.esp + 4);
    uint32_t engine = R32(self + kEngine);
    if ((key == kJoyR || key == kJoyU) && engine &&
        ((R32(engine + kSessionNet) && R32(engine + kSessionA)) || R32(engine + kSessionB))) {
        if (R32(self + kRecordA) && R32(self + kRecordB) && !R32(self + kReplaying)) {
            float value = Mouse(key);
            std::memcpy(HostPtr(ctx.esp + 8), &value, 4);
        } else {
            W32(ctx.esp + 4, key == kJoyR ? kMouseX : kMouseY);
        }
    }
    __imp__sub_00246AD6(ctx, base);
}
