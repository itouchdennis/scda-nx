#include "port/dk_shader.h"

#include <spirv_glsl.hpp>

#include <cstring>
#include <mutex>

#include "core/log.h"
#include "uam_api.h"

namespace gpu::dk {

namespace {

// uam keeps Mesa's GLSL front end in static state: one compile at a time, on any thread
std::mutex g_uam_lock;
bool g_uam_ready = false;

// the GLSL as uam takes it: GLSL 4.60 without Vulkan semantics, every resource with an explicit binding; D3D's clip
// space as it is (the device takes DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne)
bool ToGlsl(std::vector<uint32_t> code, vk::Stage stage, std::string& glsl, Dksh& out) {
    try {
        spirv_cross::CompilerGLSL c(std::move(code));
        spirv_cross::CompilerGLSL::Options o;
        o.version = 460;
        o.es = false;
        o.vulkan_semantics = false;
        o.separate_shader_objects = true;
        o.enable_420pack_extension = true;
        o.vertex.fixup_clipspace = false;
        o.vertex.flip_vert_y = false;
        c.set_common_options(o);
        if (stage == vk::Stage::Vertex) {
            // EQUAL depth passes (the sun's light on what the shadows leave lit) need bit exact positions across
            // shaders, as gpu_gl.cpp and gpu_vk.cpp mark them
            for (const spirv_cross::BuiltInResource& b : c.get_shader_resources().builtin_outputs) {
                if (b.builtin == spv::BuiltInPosition) c.set_decoration(b.resource.id, spv::DecorationInvariant);
            }
        }
        c.build_dummy_sampler_for_combined_images();
        c.build_combined_image_samplers();
        out.pair_count = 0;
        for (const spirv_cross::CombinedImageSampler& cis : c.get_combined_image_samplers()) {
            // a texture handed to a function has an entry for the parameter too, which takes no binding
            if (!c.has_decoration(cis.image_id, spv::DecorationBinding)) continue;
            if (out.pair_count >= kMaxPairs) {
                LOGE("dk: more than %u texture and sampler pairs in one shader", kMaxPairs);
                return false;
            }
            uint32_t t = c.get_decoration(cis.image_id, spv::DecorationBinding) - vk::kBindingTexture;
            uint32_t s = c.has_decoration(cis.sampler_id, spv::DecorationBinding)
                             ? c.get_decoration(cis.sampler_id, spv::DecorationBinding) - vk::kBindingSampler
                             : t;  // the dummy sampler of a texture read without one (Load): its own stage's
            uint32_t k = out.pair_count++;
            out.pairs[k] = {uint8_t(t), uint8_t(s)};
            c.set_decoration(cis.combined_id, spv::DecorationBinding, k);
            c.unset_decoration(cis.combined_id, spv::DecorationDescriptorSet);
            c.set_name(cis.combined_id, "tex_pair" + std::to_string(k));
        }
        // the uniform blocks at bindings 0 and 1 (b0 and b1), no descriptor sets
        for (const spirv_cross::Resource& r : c.get_shader_resources().uniform_buffers) {
            c.unset_decoration(r.id, spv::DecorationDescriptorSet);
        }
        glsl = c.compile();
    } catch (const std::exception& e) {
        LOGE("dk: SPIRV-Cross: %s", e.what());
        return false;
    }
    return true;
}

}  // namespace

bool CompileHlsl(const std::string& hlsl, const char* entry, vk::Stage stage, Dksh& out) {
    vk::Spirv spv;
    if (!vk::CompileHlsl(hlsl, entry, stage, spv)) return false;
    std::memcpy(out.locations, spv.locations, sizeof(out.locations));
    std::string glsl;
    if (!ToGlsl(std::move(spv.code), stage, glsl, out)) return false;
    uam::Result r;
    {
        std::lock_guard lock(g_uam_lock);
        if (!g_uam_ready) {
            uam::init(true);
            g_uam_ready = true;
        }
        r = uam::compile(stage == vk::Stage::Vertex ? uam::Stage::Vertex : uam::Stage::Fragment, glsl.c_str());
    }
    if (!r.ok) {
        LOGE("dk: uam did not take a %s shader: %s", stage == vk::Stage::Vertex ? "vertex" : "pixel", r.log.c_str());
        LOGE("%s", glsl.c_str());
        return false;
    }
    out.bytes = std::move(r.dksh);
    return !out.bytes.empty();
}

}  // namespace gpu::dk
