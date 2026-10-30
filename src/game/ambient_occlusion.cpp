// Ambient occlusion goes in where the translucent pass starts: every opaque surface is lit and in the depth buffer,
// and glass, smoke and the HUD land on top.
#include "app/app.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

// DrawTranslucent(render context, hit proxy), cdecl. Context +0 scene node; node +8 parent, +0x8C camera to screen
X86_EXTERN_FUNC(__imp__sub_002AC2FC);
X86_FUNC(sub_002AC2FC) {
    uint32_t node = R32(R32(ctx.esp + 4));
    uint32_t mode = app::Get().ambient_occlusion;
    if (mode && !R32(ctx.esp + 8) && !R32(node + 8)) d3d::Occlusion(&G<float>(node + 0x8C), mode == 2);
    __imp__sub_002AC2FC(ctx, base);
}
