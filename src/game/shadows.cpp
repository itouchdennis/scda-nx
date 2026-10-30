// The renderer makes its targets once, at console sizes: a 512x512 shadow map, and aliased over its memory the
// swizzled 256..16 chain that blurs shadow edges on screen. The host sizes both; this tells it where they are.
// With the SM3 SOFT shadow filter the host filters the shadow like Chaos Theory's shader model 3 path instead.
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// D3DDrv.D3DRenderDevice, each a D3D resource header {Common, Data, Lock, Format, Size}
constexpr uint32_t kShadowMap = 0x4CD8;    // 512x512 LIN_D16 texture
constexpr uint32_t kShadowColor = 0x4CEC;  // 512x512 surface bound beside it, Data 0
constexpr uint32_t kBlurTop = 0x4BA4;      // 256x256 A8R8G8B8, then 128, 64 and 32 up to the last
constexpr uint32_t kBlurLast = 0x4BF4;     // 16x16
constexpr uint32_t kBlurLastBytes = 16 * 16 * 4;
constexpr uint32_t kResData = 4;

// DrawLevel's call of BlurShadowEdges returns here, with ebx its render context, whose +0 is the scene node
constexpr uint32_t kDrawLevelReturn = 0x2B28C8;
constexpr uint32_t kCameraToScreen = 0x8C;  // scene node's camera to screen matrix

}  // namespace

// CreateTargets(width, height), thiscall
X86_EXTERN_FUNC(__imp__sub_002E4552);
X86_FUNC(sub_002E4552) {
    uint32_t device = ctx.ecx;
    __imp__sub_002E4552(ctx, base);
    gpu::ShadowTargets t;
    t.map = R32(device + kShadowMap + kResData);
    t.color = R32(device + kShadowColor + kResData);
    t.blur_lo = R32(device + kBlurTop + kResData);
    t.blur_hi = R32(device + kBlurLast + kResData) + kBlurLastBytes;
    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        gpu::g_shadow = t;
    }
    LOGI("shadows: map %08X, edge blur %08X-%08X", t.map, t.blur_lo, t.blur_hi);
}

// BlurShadowEdges, thiscall render interface: the frame's shadow alpha down the 256..16 chain and back up into it
X86_EXTERN_FUNC(__imp__sub_002EF9A1);
X86_FUNC(sub_002EF9A1) {
    bool soft = app::Get().shadow_filter >= 3 && R32(ctx.esp) == kDrawLevelReturn;
    if (soft) d3d::FilterShadows(&G<float>(R32(ctx.ebx) + kCameraToScreen));
    __imp__sub_002EF9A1(ctx, base);
    if (soft) d3d::ApplyShadows();
}
