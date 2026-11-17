#pragma once
// The renderer's HLSL for the deko3d backend (port/gpu_dk.cpp): vk_shader.cpp's SPIR-V, then SPIRV-Cross to GLSL 4.60
// as uam (devkitPro's deko3d shader compiler, Mesa's GLSL front end and nouveau's code generator) takes it, then uam
// to a DKSH. deko3d samples through combined texture handles (an image and a sampler descriptor), so every texture tN
// read with a sampler sM becomes one sampler at its own binding, and `pairs` says which tN and sM it joins: the draw
// makes the handle for each binding from them.
#include <cstdint>
#include <string>
#include <vector>

#include "port/vk_shader.h"

namespace gpu::dk {

constexpr uint32_t kMaxPairs = 16;

struct Pair {
    uint8_t texture = 0;  // tN
    uint8_t sampler = 0;  // sM
};

struct Dksh {
    std::vector<uint8_t> bytes;  // a whole .dksh: header and control section, then the code
    // a vertex shader's inputs: location by ATTR register, -1 where the shader has none
    int8_t locations[16];
    // binding k of the shader's samplers joins texture pairs[k].texture with sampler pairs[k].sampler
    Pair pairs[kMaxPairs];
    uint8_t pair_count = 0;
};

// HLSL to a DKSH; false with the log told why. Any thread: uam is not reentrant and runs under a lock of its own
bool CompileHlsl(const std::string& hlsl, const char* entry, vk::Stage stage, Dksh& out);

// changes when the GLSL written or the compiler changes: a kept DKSH from another revision is compiled again
constexpr uint32_t kRevision = 4;  // 4: uam with dFdy fixed

}  // namespace gpu::dk
