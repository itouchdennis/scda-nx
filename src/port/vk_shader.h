#pragma once
// The renderer's HLSL (gpu/shaders.cpp, the backend's own passes) to SPIR-V with glslang's HLSL front end, laid out as
// the D3D12 root signature has it: cbuffers b0 and b1 at bindings 0 and 1, textures t0 to t3 at 2 to 5, samplers s0
// to s3 at 6 to 9, all in set 0.
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::vk {

constexpr uint32_t kBindingUbo = 0;
constexpr uint32_t kBindingTexture = 2;
constexpr uint32_t kBindingSampler = 6;
constexpr uint32_t kBindings = 10;

enum class Stage { Vertex, Pixel };

struct Spirv {
    std::vector<uint32_t> code;
    // a vertex shader's inputs: location by ATTR register, -1 where the shader has none
    int8_t locations[16];
};

// false with the log told why; any thread
bool CompileHlsl(const std::string& src, const char* entry, Stage stage, Spirv& out);

}  // namespace gpu::vk
