// D3DDevice_GetProjectionViewportMatrix reads the matrix the XDK builds in its own SetTransform and SetViewport, which
// ours replace: the device's copy stayed zero, and so did what versus' optic camo builds from it (its spy pass drew
// nothing). Here it is the projection times the viewport transform, from the state the renderer keeps.
#include <cstring>

#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

// stdcall (D3DMATRIX* out), ret 4
X86_FUNC(sub_00282B30) {
    uint32_t out = R32(ctx.esp + 4);
    float m[16];
    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        const gpu::State& s = d3d::State();
        const auto& p = s.transforms[3];  // D3DTS_PROJECTION
        // c-38 and c-37: the viewport's scale and offset, as the vertex programs get them. The XDK's matrix leaves out
        // the screen space offset (0.53125 by default), which the camo adds itself (c95): taken off here so its spy
        // lands exactly on the one drawn with c-37, else edges seen side on fail its depth test and show through
        constexpr float kScreenOffset = 0.53125f;
        const auto& scale = s.vsc[58];
        const float offset[3] = {s.vsc[59][0] - kScreenOffset, s.vsc[59][1] - kScreenOffset, s.vsc[59][2]};
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 3; c++) m[r * 4 + c] = p[r * 4 + c] * scale[c] + p[r * 4 + 3] * offset[c];
        for (int r = 0; r < 4; r++) m[r * 4 + 3] = p[r * 4 + 3];
    }
    std::memcpy(HostPtr(out), m, sizeof(m));
    Return(ctx, 0, 1);
}
