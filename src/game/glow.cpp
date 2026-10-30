// Glow and bloom composite, thiscall(textures[4], coords[4]): a stage reads its chain level at the screen position
// times xy plus zw. The game's offsets leave the chain half a texel of its 256 level, less half a screen pixel,
// up and left of the scene: unseen at 480p, several pixels at higher sizes.
#include <cstring>

#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

X86_EXTERN_FUNC(__imp__sub_002EAF44);
X86_FUNC(sub_002EAF44) {
    const gpu::Viewport& view = d3d::State().viewport;
    float shift[2] = {0.5f / 256 - 0.5f / float(view.w), 0.5f / 256 - 0.5f / float(view.h)};
    float c[16];
    void* coords = HostPtr(R32(ctx.esp + 8));
    std::memcpy(c, coords, sizeof(c));
    for (int i = 0; i < 16; i += 4)
        if (c[i] == 1.0f && c[i + 1] == -1.0f) {  // a chain level, not the screen copy
            c[i + 2] -= shift[0];
            c[i + 3] -= shift[1];
        }
    std::memcpy(coords, c, sizeof(c));
    __imp__sub_002EAF44(ctx, base);
}
