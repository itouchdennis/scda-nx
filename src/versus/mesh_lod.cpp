// Skeletal meshes at full detail at any distance. The mesh instance's draw picks its LOD from the actor's size over its
// distance times the actor's LODBias against each LOD's display factor; a huge bias for that call keeps the first.
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kLodBias = 0x1F4;  // actor float, 1 by default

}  // namespace

// skeletal mesh instance draw (thiscall instance; the actor at its first argument's +0)
X86_EXTERN_FUNC(__imp__sub_000CC710);
X86_FUNC(sub_000CC710) {
    uint32_t args = R32(ctx.esp + 4);
    uint32_t actor = args ? R32(args) : 0;
    if (!actor) return __imp__sub_000CC710(ctx, base);
    float bias = G<float>(actor + kLodBias);
    G<float>(actor + kLodBias) = 1e6f;
    __imp__sub_000CC710(ctx, base);
    G<float>(actor + kLodBias) = bias;
}
