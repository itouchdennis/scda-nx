#include "port/gl_shader.h"

#include <spirv_glsl.hpp>

#include <cstring>

#include "core/log.h"

namespace gpu::gl {

constexpr uint32_t kGlslVersion = 430;

bool HlslToGlsl(const std::string& hlsl, const char* entry, vk::Stage stage, Glsl& out) {
    vk::Spirv spv;
    if (!vk::CompileHlsl(hlsl, entry, stage, spv)) return false;
    std::memcpy(out.locations, spv.locations, sizeof(out.locations));
    try {
        spirv_cross::CompilerGLSL glsl(std::move(spv.code));
        spirv_cross::CompilerGLSL::Options o;
        o.version = kGlslVersion;
        o.es = false;
        // the backend sets glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE): D3D's clip space as it is
        o.vertex.fixup_clipspace = false;
        o.vertex.flip_vert_y = false;
        glsl.set_common_options(o);
        if (stage == vk::Stage::Vertex) {
            // EQUAL depth passes (the sun's light on what the shadows leave lit) need bit exact positions across
            // programs; GL promises that only for an invariant gl_Position, precise alone is not enough
            for (const spirv_cross::BuiltInResource& b : glsl.get_shader_resources().builtin_outputs) {
                if (b.builtin == spv::BuiltInPosition) glsl.set_decoration(b.resource.id, spv::DecorationInvariant);
            }
        }
        glsl.build_dummy_sampler_for_combined_images();
        glsl.build_combined_image_samplers();
        for (const spirv_cross::CombinedImageSampler& c : glsl.get_combined_image_samplers()) {
            // tN goes with sN; a texture handed to a function (shadow()) has an entry for the parameter too, which
            // takes no binding
            if (!glsl.has_decoration(c.image_id, spv::DecorationBinding)) continue;
            uint32_t unit = glsl.get_decoration(c.image_id, spv::DecorationBinding) - vk::kBindingTexture;
            glsl.set_decoration(c.combined_id, spv::DecorationBinding, unit);
            glsl.set_name(c.combined_id, "tex_unit" + std::to_string(unit));
        }
        out.src = glsl.compile();
    } catch (const std::exception& e) {
        LOGE("gl: SPIRV-Cross: %s", e.what());
        return false;
    }
    return true;
}

}  // namespace gpu::gl
