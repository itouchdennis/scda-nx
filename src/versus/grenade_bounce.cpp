// A grenade hitting the world traces along its velocity first, and blows up at once if that trace hits nothing or
// hits a Fake Backdrop surface (the skybox). A grenade touching the floor at a shallow angle starts the trace inside
// the floor, so it hits nothing and explodes before its timer.
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kHitWallTrace = 0x3C23A;  // return address of the trace in the grenade's HitWall (sub_0003C150)

constexpr uint32_t kHitActor = 0x04, kHitPrimitive = 0x20, kHitItem = 0x28;  // FCheckResult
constexpr uint32_t kXLevel = 0x1C0, kActors = 0x2C;                          // actor's ULevel, its Actors (0 = Level)
constexpr uint32_t kFlags = 0x30C, kBounce = 0x4000000;                      // actor bitfield, bBounce
constexpr uint32_t kNodes = 0x54, kSurfs = 0x94, kNodeSize = 0x58, kNodeSurf = 0x2C;  // UModel, FBspNode
constexpr uint32_t kSurfSize = 0x2C, kSurfFlags = 0x14, kFakeBackdrop = 0x80;          // FBspSurf PolyFlags

bool Backdrop(uint32_t hit) {
    uint32_t model = R32(hit + kHitPrimitive), item = R32(hit + kHitItem);
    uint32_t surf = R32(R32(model + kNodes) + item * kNodeSize + kNodeSurf);
    return G<uint8_t>(R32(model + kSurfs) + surf * kSurfSize + kSurfFlags) & kFakeBackdrop;
}

}  // namespace

// ULevel::SingleLineCheck(hit, source, end, start, flags, extent), thiscall
X86_EXTERN_FUNC(__imp__sub_0010BBE0);
X86_FUNC(sub_0010BBE0) {
    uint32_t ret = R32(ctx.esp), hit = R32(ctx.esp + 4), grenade = R32(ctx.esp + 8);
    __imp__sub_0010BBE0(ctx, base);
    if (ret != kHitWallTrace) return;
    uint32_t actor = R32(hit + kHitActor), level = R32(R32(R32(grenade + kXLevel) + kActors));
    if (actor && (actor != level || !Backdrop(hit))) return;
    // any other actor takes HitWall's bounce; bBounce is set as the explosion branch would
    W32(grenade + kFlags, R32(grenade + kFlags) | kBounce);
    W32(hit + kHitActor, grenade);
}
