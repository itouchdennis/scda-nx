// The world conversion of a root motion delta loses a few yaw units a frame, so at high fps turns end short (ledge
// corners drop the player). A yaw-only turn of an upright actor gets its exact delta.
#include <cstdlib>
#include <unordered_map>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// the root motion physics' call (sub_00280D8B), its actor in ebx
constexpr uint32_t kRootMotionCall = 0x280E65;
// mesh instance: +0x3A8 the frame's root rotation delta in mesh space; actor: +0x1F4 Rotation (pitch, yaw, roll)
constexpr uint32_t kMeshDelta = 0x3A8;
constexpr uint32_t kRotation = 0x1F4;
// the conversion's error, with room
constexpr int32_t kTolerance = 8;

std::unordered_map<uint32_t, int32_t> g_sign;  // per mesh instance: a mirrored animation turns the other way

bool Upright(uint32_t rotator) { return !(R32(rotator) & 0xFFFF) && !(R32(rotator + 8) & 0xFFFF); }

}  // namespace

// mesh instance root motion (thiscall: out location delta, out rotation delta, new frame, to world, delta time)
X86_EXTERN_FUNC(__imp__sub_00216CFD);
X86_FUNC(sub_00216CFD) {
    uint32_t instance = ctx.ecx;
    uint32_t ret = R32(ctx.esp), out = R32(ctx.esp + 8), world = R32(ctx.esp + 0x10);
    __imp__sub_00216CFD(ctx, base);
    uint32_t actor = ctx.ebx;
    if (ret != kRootMotionCall || !ctx.eax || !world || !out) return;
    if (!Upright(instance + kMeshDelta) || !Upright(actor + kRotation)) return;
    int32_t exact = int32_t(R32(instance + kMeshDelta + 4));
    int32_t got = int32_t(R32(out + 4));
    int32_t& sign = g_sign[instance];
    if (std::abs(got) > kTolerance) sign = (got < 0) == (exact < 0) ? 1 : -1;
    if (!sign) sign = 1;
    int32_t yaw = sign * exact;
    if (std::abs(got - yaw) <= kTolerance) W32(out + 4, uint32_t(yaw));
}
