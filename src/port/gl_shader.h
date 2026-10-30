#pragma once
// The renderer's HLSL as GLSL 4.30 for the OpenGL backend (port/gpu_gl.cpp): vk_shader.cpp's SPIR-V, then SPIRV-Cross.
// Bindings as the D3D12 root signature has them: uniform blocks b0 and b1 at 0 and 1; each texture tN joined with the
// sampler it is read with into one sampler at texture unit N.
#include <cstdint>
#include <string>

#include "port/vk_shader.h"

namespace gpu::gl {

struct Glsl {
    std::string src;
    // a vertex shader's inputs: location by ATTR register, -1 where the shader has none
    int8_t locations[16];
};

// false with the log told why; any thread
bool HlslToGlsl(const std::string& hlsl, const char* entry, vk::Stage stage, Glsl& out);

}  // namespace gpu::gl
