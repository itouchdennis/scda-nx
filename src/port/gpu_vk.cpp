// The title's Direct3D 8 calls drawn with Vulkan 1.3 (SCDA_GPU=vk): the PC on its driver (RADV and the like), the
// Switch on mesa-switch's NVK, linked in. It follows gpu/d3d12.cpp draw for draw as gpu_gl.cpp does: the same HLSL
// from gpu/shaders.cpp, here as SPIR-V straight from vk_shader.cpp, the same constants, targets kept by surface
// memory, textures by guest address.
//
// Shaders are shader objects (VK_EXT_shader_object): each one compiles on its own, every state is set as the draw
// needs it, so a draw never waits on a pipeline. A shader compiled once is kept as the driver's binary in
// shaders.vkb beside the log: the next run, and the next image after a relaunch, makes it from there in a moment,
// on the build threads before a draw asks for it. A shader not kept yet compiles there too while its draws are
// skipped (async_shaders, as d3d12.cpp).
//
// The viewport's height is negative: D3D's clip space and its windings as they are, row 0 of every target the top.
// All images stay in the GENERAL layout; render passes, copies and uploads are ordered by a barrier between them.
//
// SMAA over the shown frame as d3d12.cpp's (smaa_shader.h). Not here yet, as with OpenGL: render scale, MSAA, shader
// model 3 HDR and soft shadows, ambient occlusion, the texture pack, captures, the settings panel.
#include <SDL.h>
#include <dxgiformat.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/smaa_shader.h"
#include "gpu/smaa_textures.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "port/gpu_common.h"
#include "port/port.h"
#include "port/vk_device.h"
#include "port/vk_shader.h"

namespace gpu {

namespace {

using namespace common;
using vk::fn;

// the XDK's blend, compare and stencil values are the GL enums the console's GPU took
VkBlendFactor Blend(uint32_t v) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

constexpr VkFormat kColourFormat = VK_FORMAT_B8G8R8A8_UNORM;  // guest memory's byte order: Sync copies it as it is

struct Target {
    vk::Image img;
    vk::Image fb;  // a copy draws read while drawing to the target
    bool depth = false;
    uint32_t gw = 0, gh = 0;  // the console's size
    uint32_t w = 0, h = 0;    // the host image's
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0;          // last lock seen
    uint64_t sig = 0;            // guest memory as last matched
    bool drawn = false;          // the GPU wrote it since the last readback
    std::vector<uint8_t> read;   // what Sync left in guest memory
};

struct Tex {
    vk::Image img;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    bool norm = false;  // a normalization cube
};

// a draw's four texture stages
struct DrawTextures {
    TexInfo info[4];
    VkImageView view[4] = {};
    uint32_t levels[4] = {};
    bool rt[4] = {};  // stage samples a render target
};

// all 32 bit fields, hashed whole
struct SamplerDesc {
    uint32_t min = VK_FILTER_NEAREST, mag = VK_FILTER_NEAREST, mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    uint32_t wrap[3] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                        VK_SAMPLER_ADDRESS_MODE_REPEAT};
    float aniso = 1, bias = 0, min_lod = 0, max_lod = 1000;
    float border[4] = {};
    uint32_t compare = 0;  // VkCompareOp + 1 for a shadow map, 0 without
};

SamplerDesc StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    SamplerDesc sd;
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    sd.mag = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.min = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mip = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    sd.aniso = float(std::min<uint32_t>(an, 16));
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.wrap[0] = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.wrap[1] = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.wrap[2] = Address(t[kTssAddressW]);
    sd.bias = Bits(t[kTssMipLodBias]);
    sd.min_lod = float(t[kTssMaxMipLevel]);
    sd.max_lod = mip ? 1000.0f : sd.min_lod;
    Color(t[kTssBorderColor], sd.border);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.min = sd.mag = lin(min) || lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.compare = swap[CmpIndex(s.rs[kRsShadowFunc])] + 1u;
        sd.aniso = 1;
    }
    return sd;
}

uint32_t FormatBytes(uint32_t f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: return 8;
        case DXGI_FORMAT_R8_UNORM: return 1;
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8G8_UNORM: return 2;
        default: return 4;
    }
}

VkPrimitiveTopology PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (a.topology == Topology::Lines) return a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// EQUAL depth passes (the sun's light on what the shadows leave lit) need bit exact positions across shaders:
// position is marked invariant, as gpu_gl.cpp does for GLSL
void MarkPositionInvariant(std::vector<uint32_t>& code) {
    constexpr uint32_t kOpDecorate = 71, kOpMemberDecorate = 72;
    constexpr uint32_t kBuiltIn = 11, kPosition = 0, kInvariant = 18;
    for (size_t i = 5; i < code.size();) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!n || i + n > code.size()) return;
        if (op == kOpDecorate && n >= 4 && code[i + 2] == kBuiltIn && code[i + 3] == kPosition) {
            code.insert(code.begin() + ptrdiff_t(i + n), {3u << 16 | kOpDecorate, code[i + 1], kInvariant});
            return;
        }
        if (op == kOpMemberDecorate && n >= 5 && code[i + 3] == kBuiltIn && code[i + 4] == kPosition) {
            code.insert(code.begin() + ptrdiff_t(i + n), {4u << 16 | kOpMemberDecorate, code[i + 1], code[i + 2], kInvariant});
            return;
        }
        i += n;
    }
}

// a shader by the hash of its HLSL: compiled, read from shaders.vkb, or being built
struct Built {
    enum State : uint8_t { kKept, kBuilding, kReady, kFailed };
    State state = kKept;
    bool from_binary = false;  // kBuilding: from the kept binary, done in a moment
    VkShaderEXT obj = VK_NULL_HANDLE;
    int8_t locations[16];
    vk::Stage stage = vk::Stage::Vertex;
    std::string entry;
    std::vector<uint8_t> binary;  // kKept: not made yet
};

// a draw key's shader as the render thread last resolved it
struct Resolved {
    uint64_t hash = 0;  // 0: the HLSL did not come out
    VkShaderEXT obj = VK_NULL_HANDLE;
    const int8_t* locations = nullptr;
};

// streaming memory for vertices, indices, constants and uploads: one host visible buffer in kSegments parts, each
// waited for before it is written again
constexpr uint32_t kSegments = 4;
#ifdef SCDA_SWITCH
constexpr size_t kRingSize = 32u << 20;
constexpr int kBuildThreads = 2;
#else
constexpr size_t kRingSize = 64u << 20;
constexpr int kBuildThreads = 3;
#endif
constexpr size_t kSegment = kRingSize / kSegments;
constexpr uint32_t kSubmits = 8;           // command buffers in turn
constexpr size_t kFramesQueued = 2;        // presents the GPU may be behind
constexpr char kBinaryMagic[8] = {'S', 'C', 'D', 'A', 'V', 'K', '1', 0};

class VkBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
              uint32_t dst_points) override;
    void Present(const Surface& front, bool pillarbox) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override {
        if (!DeliverLate(data)) Sync(data, 0);
        RecordLate(data);
    }
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override {
        if (recorded_) Submit();
    }
    // the title's ramp (its shadow calibration), applied with the settings' curve from the next Present on
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(game_ramp_.data(), ramp, game_ramp_.size());
        has_game_ramp_ = true;
        ramp_set_ = ~0u;
    }

private:
    struct Alloc {
        uint8_t* cpu = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
    };
    struct Submission {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;  // in flight while above done_
    };

    // command buffers, the ring, ordering
    VkCommandBuffer Cmd() {
        recorded_ = true;
        return subs_[cur_].cmd;
    }
    void Begin();
    uint64_t Submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    void Poll();
    void WaitSerial(uint64_t serial);
    void Collect();
    void Retire(vk::Image& img);
    void Retire(vk::Buffer& buf);
    Alloc Upload(size_t size, size_t align);
    void Barrier();
    void Outside();
    void Inside(Target* rt, Target* ds);
    void InitLayout(const vk::Image& img);
    void CopyToImage(const vk::Image& img, const Alloc& src, uint32_t level, uint32_t layer, uint32_t w, uint32_t h,
                     uint32_t d, uint32_t row_texels);
    vk::Buffer& Readback(VkDeviceSize bytes);
    bool ReadTarget(Target& t, std::vector<uint8_t>& px);

    // shaders
    VkShaderEXT MakeShader(vk::Stage stage, const char* entry, const void* code, size_t size, bool binary);
    bool Build(uint64_t hash, const std::string& hlsl, vk::Stage stage, const std::string& entry, Built& out);
    void Keep(uint64_t hash, const Built& b);
    void LoadKept();
    void StartBuilders();
    void Builder();
    const Resolved& Shader(uint64_t key, vk::Stage stage, const std::string& (*gen)(void*), void* arg);
    void Request(Resolved& r, std::string hlsl, vk::Stage stage, const char* entry);
    bool Lookup(Resolved& r);
    void CreateBlit();
    // the build threads done with these, their objects looked up
    void Await(std::initializer_list<Resolved*> shaders);
    // the full screen state of the present and post passes: one triangle, nothing tested or blended
    void PostState(const VkViewport& vp, const VkRect2D& sc, const float constants[4]);
    // a full screen pass into out (w x h): b1 the params, t0 to t2 the views (a dummy where null), s0 linear, s1 point
    void DrawPost(const vk::Image& out, uint32_t w, uint32_t h, bool clear, const Resolved& vs, const Resolved& ps,
                  const float* params, size_t bytes, std::initializer_list<VkImageView> views);
    // SMAA's blend of the frame t holds, null when it cannot run (its shaders failed)
    const vk::Image* Smaa(const Target& t);
    bool EnsureSmaa(uint32_t w, uint32_t h);
    VkSampler Sampler(const SamplerDesc& d);
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool BindTargets(const State& s);
    void UploadTarget(Target& t, const Surface& s);
    VkImageView FeedbackCopy(Target& t);

    // textures
    VkImageView BindTexture(const State& s, int stage, DrawTextures& stages);
    VkImageView BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    VkImageView BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p);
    void ReleaseOldTextures();

    // a draw
    void SetFixedState();
    void ApplyState(const State& s, const Assembled& a);
    void SetViewportScissor(const State& s, uint32_t gw);
    void BindShaders(VkShaderEXT vs, VkShaderEXT ps);
    struct Vertices {
        VkBuffer buf[16];
        VkDeviceSize off[16];
    };
    bool UploadVertices(const Assembled& a, Vertices& v);
    void SetVertexInputs(const Assembled& a, const int8_t* locations, const Vertices& v);
    void ClearTargets(Target* rt, Target* ds, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                      uint32_t color, float z, uint32_t stencil);
    void CountFps();
    void SaveShot(Target& t);
    bool Swapchain();

    vk::Device dev_;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<Submission, kSubmits> subs_;
    uint32_t cur_ = 0;
    uint64_t serial_ = 1;  // the command buffer being recorded
    uint64_t done_ = 0;    // the GPU has finished every serial up to here
    bool recorded_ = false;
    std::vector<std::pair<uint64_t, vk::Image>> dead_images_;
    std::vector<std::pair<uint64_t, vk::Buffer>> dead_buffers_;
    std::deque<uint64_t> frames_;  // the serials that ended each queued present

    vk::Buffer ring_;
    size_t ring_off_ = 0;
    uint32_t segment_ = 0;
    uint64_t segment_serial_[kSegments] = {};
    VkDeviceSize ubo_align_ = 256;
    vk::Buffer readback_;

    // the open render pass, and what the next draw wants
    bool rendering_ = false;
    bool pending_ = false;  // GPU writes since the last barrier
    Target* cur_rt_ = nullptr;
    Target* cur_ds_ = nullptr;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the targets as the draw's textures bind
    Target* draw_ds_ = nullptr;

    // dynamic state as last set in this command buffer; fixed_ false: none of it
    bool fixed_ = false;
    struct Dyn {
        VkBool32 blend;
        VkColorBlendEquationEXT eq;
        float constants[4];
        VkColorComponentFlags mask;
        VkBool32 ztest, zwrite;
        VkCompareOp zfunc;
        VkBool32 stest;
        VkStencilOp sfail, szfail, spass;
        VkCompareOp sfunc;
        uint32_t sref, smask, swrite;
        VkBool32 bias;
        float bias_const, bias_slope;
        VkCullModeFlags cull;
        VkFrontFace front;
        VkPolygonMode fill;
        VkPrimitiveTopology topo;
        VkViewport vp;
        VkRect2D sc;
    } dyn_{};
    VkShaderEXT bound_vs_ = VK_NULL_HANDLE, bound_ps_ = VK_NULL_HANDLE;
    std::vector<VkVertexInputBindingDescription2EXT> vin_bind_;
    std::vector<VkVertexInputAttributeDescription2EXT> vin_attr_;
    bool vin_set_ = false;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    vk::Image dummy_[4];  // by TexKind: what an empty stage samples

    // shaders: the render thread's keys, then the builds every thread shares
    std::unordered_map<uint64_t, Resolved> keys_;  // vertex keys, pixel keys salted
    std::mutex build_lock_;
    std::condition_variable build_cv_;   // a job, or a build done
    std::unordered_map<uint64_t, Built> built_;
    struct Job {
        uint64_t hash;
        std::string hlsl;  // empty: make the kept binary
    };
    std::deque<Job> jobs_;
    FILE* kept_file_ = nullptr;  // under build_lock_
    size_t kept_ = 0, kept_failed_ = 0;
    Assembled asm_;

    Resolved blit_vs_, blit_ps_;
    // SMAA: edges, blend weights, the blended frame, each the frame's size; the area and search lookups
    Resolved smaa_vs_[3], smaa_ps_[3];
    vk::Image smaa_[3], smaa_area_, smaa_search_;
    uint32_t smaa_w_ = 0, smaa_h_ = 0;
    bool smaa_built_ = false, smaa_failed_ = false;
    VkSampler blit_linear_ = VK_NULL_HANDLE, blit_point_ = VK_NULL_HANDLE;
    vk::Image lut_;
    bool gamma_ = false;
    uint32_t ramp_set_ = ~0u;
    std::array<uint8_t, 768> game_ramp_{};
    bool has_game_ramp_ = false;
    std::vector<VkSemaphore> acquire_;   // round robin, each with the serial that waited on it
    std::vector<uint64_t> acquire_serial_;
    std::vector<VkSemaphore> rendered_;  // by swap chain image
    uint32_t acquire_next_ = 0;
    bool swap_stale_ = false;
    uint32_t asked_w_ = 0, asked_h_ = 0;  // the size Swapchain was last asked for, which the surface may not give

    // NOOVERWRITE locks: each surface copied into a ring of host buffers, the newest the GPU has finished handed to
    // the title without waiting (gpu/d3d12.cpp does the same)
    static constexpr int kLateSlots = 3;
    struct Late {
        vk::Buffer buf[kLateSlots];
        uint64_t serial[kLateSlots] = {};  // set while the GPU still writes the slot
        bool done[kLateSlots] = {};
        uint64_t seq[kLateSlots] = {};
        uint64_t frame[kLateSlots] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0;
        uint32_t next = 0;
    };
    void ResetLate(Late& l);
    void PollLate(Late& l);
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;  // DeliverLate runs on the title's thread
    uint64_t late_seq_ = 0;
    std::atomic<uint64_t> presents_seen_{0};

    uint64_t presents_ = 0;
    uint32_t fps_n_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t draws_ = 0, last_draws_ = 0, last_presents_ = 0, skipped_ = 0;
    std::chrono::steady_clock::time_point log_t_ = std::chrono::steady_clock::now();
    uint64_t log_cpu_ = 0;  // the render thread's CPU time at log_t_

    // where the render thread's time goes, by phase, logged with the frame rate
    enum Phase { kAssemble, kTextures, kTexUpload, kShaders, kBuildWait, kConstants, kStages, kState, kVertices,
                 kDraw, kRingWait, kClear, kCopy, kSync, kPresent, kSwap, kTargets, kKeys, kDrawAll, kPhases };
    uint64_t phase_ns_[kPhases] = {};
    uint64_t tex_uploads_ = 0, tex_upload_bytes_ = 0;
    std::atomic<uint64_t> compiles_{0};
    struct Timed {
        uint64_t& into;
        std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
        ~Timed() {
            into += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t)
                                 .count());
        }
    };
    Timed Time(Phase p) { return Timed{phase_ns_[p]}; }
};

bool VkBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
#ifdef SCDA_SWITCH
    // Mesa's own shader cache on the SD card would keep a second copy of shaders.vkb, with writer threads of its own
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
#endif
    if (!dev_.Init()) return false;
    VkDevice dev = dev_.dev;
    ubo_align_ = std::max<VkDeviceSize>(dev_.props.limits.minUniformBufferOffsetAlignment, 16);

    // the root signature's layout: cbuffers b0 b1, textures t0 to t3, samplers s0 to s3, pushed with each draw
    VkDescriptorSetLayoutBinding b[vk::kBindings] = {};
    for (uint32_t i = 0; i < vk::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[i].descriptorType = i < vk::kBindingTexture   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < vk::kBindingSampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                        : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = vk::kBindings;
    sl.pBindings = b;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = dev_.family;
    if (fn.vkCreateDescriptorSetLayout(dev, &sl, nullptr, &set_layout_) != VK_SUCCESS ||
        fn.vkCreatePipelineLayout(dev, &pl, nullptr, &layout_) != VK_SUCCESS ||
        fn.vkCreateCommandPool(dev, &cp, nullptr, &pool_) != VK_SUCCESS) {
        LOGE("vk: no layouts or command pool");
        return false;
    }
    for (Submission& sub : subs_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (fn.vkAllocateCommandBuffers(dev, &ai, &sub.cmd) != VK_SUCCESS ||
            fn.vkCreateFence(dev, &fi, nullptr, &sub.fence) != VK_SUCCESS) {
            LOGE("vk: no command buffers");
            return false;
        }
    }
    constexpr VkBufferUsageFlags kRingUse = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!dev_.CreateBuffer(ring_, kRingSize, kRingUse, true) || !ring_.mem.map) {
        LOGE("vk: the upload ring could not be made");
        return false;
    }
    if (!Swapchain()) return false;
    Begin();

    // what a stage without a texture samples: zeros, as a null descriptor gives them on D3D12
    bool dummies = dev_.CreateImage(dummy_[kTexNone], kColourFormat, 1, 1, 1, 1, 1, 0) &&
                   dev_.CreateImage(dummy_[kTex2D], kColourFormat, 1, 1, 1, 1, 1, 0) &&
                   dev_.CreateImage(dummy_[kTex3D], kColourFormat, 1, 1, 2, 1, 1, 0) &&
                   dev_.CreateImage(dummy_[kTexCube], kColourFormat, 1, 1, 1, 6, 1, 0, true) &&
                   dev_.CreateImage(lut_, kColourFormat, 256, 1, 1, 1, 1, 0);
    if (!dummies) return false;
    for (const vk::Image& img : dummy_) {
        InitLayout(img);
        VkClearColorValue zero{};
        VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers};
        fn.vkCmdClearColorImage(Cmd(), img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    InitLayout(lut_);
    pending_ = true;

    LoadKept();
    StartBuilders();
    CreateBlit();
    if (!blit_vs_.obj || !blit_ps_.obj) return false;
    LOGI("vk: renderer up, the title draws at %ux%u", width, height);
    port::Breadcrumb("vk renderer up");
    return true;
}

// command buffers, the ring, ordering

void VkBackend::Begin() {
    Submission& sub = subs_[cur_];
    if (sub.serial > done_) {
        auto timed = Time(kRingWait);
        fn.vkWaitForFences(dev_.dev, 1, &sub.fence, VK_TRUE, UINT64_MAX);
        done_ = sub.serial;
    }
    fn.vkResetFences(dev_.dev, 1, &sub.fence);
    fn.vkResetCommandBuffer(sub.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(sub.cmd, &bi);
    recorded_ = false;
    fixed_ = false;
    vin_set_ = false;
    bound_vs_ = bound_ps_ = VK_NULL_HANDLE;
}

// the command buffer to the queue, the next one begun; its serial
uint64_t VkBackend::Submit(VkSemaphore wait, VkSemaphore signal) {
    if (rendering_) {
        fn.vkCmdEndRendering(Cmd());
        rendering_ = false;
    }
    Submission& sub = subs_[cur_];
    fn.vkEndCommandBuffer(sub.cmd);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sub.cmd;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    VkResult r = fn.vkQueueSubmit(dev_.queue, 1, &si, sub.fence);
    if (r != VK_SUCCESS) LOGE("vk: submit failed (%d)", r);
    uint64_t serial = serial_++;
    sub.serial = serial;
    cur_ = (cur_ + 1) % kSubmits;
    // a draw after this one must not take the last one's barrier for its own
    pending_ = true;
    Begin();
    return serial;
}

void VkBackend::Poll() {
    for (const Submission& sub : subs_) {
        if (sub.serial > done_ && fn.vkGetFenceStatus(dev_.dev, sub.fence) == VK_SUCCESS) {
            // in order on the one queue: everything before it is done too
            done_ = sub.serial;
        }
    }
}

void VkBackend::WaitSerial(uint64_t serial) {
    if (serial >= serial_) Submit();
    while (done_ < serial) {
        const Submission* oldest = nullptr;
        for (const Submission& sub : subs_) {
            if (sub.serial > done_ && (!oldest || sub.serial < oldest->serial)) oldest = &sub;
        }
        if (!oldest) break;
        fn.vkWaitForFences(dev_.dev, 1, &oldest->fence, VK_TRUE, UINT64_MAX);
        done_ = oldest->serial;
    }
}

void VkBackend::Collect() {
    auto dead = [this](auto& list, auto destroy) {
        for (auto it = list.begin(); it != list.end();) {
            if (it->first <= done_) {
                destroy(it->second);
                it = list.erase(it);
            } else {
                ++it;
            }
        }
    };
    dead(dead_images_, [this](vk::Image& i) { dev_.DestroyImage(i); });
    dead(dead_buffers_, [this](vk::Buffer& b) { dev_.DestroyBuffer(b); });
}

// destroyed once the GPU is past what was recorded so far
void VkBackend::Retire(vk::Image& img) {
    if (img.image) dead_images_.emplace_back(serial_, img);
    img = vk::Image{};
}

void VkBackend::Retire(vk::Buffer& buf) {
    if (buf.buffer) dead_buffers_.emplace_back(serial_, buf);
    buf = vk::Buffer{};
}

VkBackend::Alloc VkBackend::Upload(size_t size, size_t align) {
    if (size > kSegment) {
        // bigger than a segment (a large texture level): a buffer of its own for this command buffer
        vk::Buffer b;
        if (!dev_.CreateBuffer(b, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return {};
        Alloc a{b.mem.map, b.buffer, 0};
        Retire(b);
        return a;
    }
    size_t off = (ring_off_ + align - 1) & ~(align - 1);
    size_t end = (segment_ + 1) * kSegment;
    if (off + size > end) {
        // the segment is done: the next one waited for. The old one is the command buffer's recording after that
        // wait (which may submit): a draw half streamed reads it from there
        uint32_t old = segment_;
        segment_ = (segment_ + 1) % kSegments;
        if (uint64_t s = segment_serial_[segment_]; s > done_) {
            auto timed = Time(kRingWait);
            WaitSerial(s);
        }
        segment_serial_[old] = serial_;
        off = segment_ * kSegment;
    }
    ring_off_ = off + size;
    return {ring_.mem.map + off, ring_.buffer, off};
}

// every write so far before every access after: passes, copies and uploads are few enough a frame
void VkBackend::Barrier() {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    fn.vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                            nullptr, 0, nullptr);
    pending_ = false;
}

// out of the render pass, for a copy or an upload
void VkBackend::Outside() {
    if (rendering_) {
        fn.vkCmdEndRendering(Cmd());
        rendering_ = false;
        pending_ = true;
    }
    if (pending_) Barrier();
}

// into a render pass on these targets, the open one when it is theirs
void VkBackend::Inside(Target* rt, Target* ds) {
    if (rendering_ && cur_rt_ == rt && cur_ds_ == ds) return;
    Outside();
    Target* any = rt ? rt : ds;
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    if (rt) {
        colour.imageView = rt->img.attach;
        colour.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &colour;
    }
    if (ds) {
        depth.imageView = ds->img.attach;
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &depth;
        ri.pStencilAttachment = &depth;
    }
    fn.vkCmdBeginRendering(Cmd(), &ri);
    rendering_ = true;
    cur_rt_ = rt;
    cur_ds_ = ds;
}

// a new image from UNDEFINED into GENERAL, where it stays
void VkBackend::InitLayout(const vk::Image& img) {
    Outside();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, img.levels, 0, img.layers};
    fn.vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                            0, nullptr, 1, &b);
}

// the caller is Outside(); row_texels 0: rows tightly packed (DXT blocks)
void VkBackend::CopyToImage(const vk::Image& img, const Alloc& src, uint32_t level, uint32_t layer, uint32_t w,
                            uint32_t h, uint32_t d, uint32_t row_texels) {
    if (!src.buffer) return;
    VkBufferImageCopy r{};
    r.bufferOffset = src.off;
    r.bufferRowLength = row_texels;
    bool depth = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    r.imageSubresource = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), level,
                          layer, 1};
    r.imageExtent = {w, h, d};
    fn.vkCmdCopyBufferToImage(Cmd(), src.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    pending_ = true;
}

vk::Buffer& VkBackend::Readback(VkDeviceSize bytes) {
    if (readback_.size < bytes) {
        Retire(readback_);
        dev_.CreateBuffer(readback_, std::max<VkDeviceSize>(bytes, 4u << 20), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                          true);
    }
    return readback_;
}

// a colour target's pixels now, rows of w * 4 bytes as guest memory has them; waits for the GPU
bool VkBackend::ReadTarget(Target& t, std::vector<uint8_t>& px) {
    VkDeviceSize bytes = VkDeviceSize(t.w) * t.h * 4;
    vk::Buffer& rb = Readback(bytes);
    if (!rb.buffer || !rb.mem.map) return false;
    Outside();
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {t.w, t.h, 1};
    fn.vkCmdCopyImageToBuffer(Cmd(), t.img.image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &r);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    fn.vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                            0, nullptr);
    WaitSerial(Submit());
    px.assign(rb.mem.map, rb.mem.map + bytes);
    return true;
}

// shaders

VkShaderEXT VkBackend::MakeShader(vk::Stage stage, const char* entry, const void* code, size_t size, bool binary) {
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    bool vs = stage == vk::Stage::Vertex;
    ci.stage = vs ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    ci.nextStage = vs ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
    ci.codeType = binary ? VK_SHADER_CODE_TYPE_BINARY_EXT : VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = size;
    ci.pCode = code;
    ci.pName = entry;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set_layout_;
    VkShaderEXT obj = VK_NULL_HANDLE;
    VkResult r = fn.vkCreateShadersEXT(dev_.dev, 1, &ci, nullptr, &obj);
    if (r != VK_SUCCESS) {
        if (!binary) LOGE("vk: the driver did not take a %s shader (%d)", vs ? "vertex" : "pixel", r);
        return VK_NULL_HANDLE;
    }
    return obj;
}

// HLSL to SPIR-V to the driver's shader, its binary kept; any thread
bool VkBackend::Build(uint64_t hash, const std::string& hlsl, vk::Stage stage, const std::string& entry, Built& out) {
    auto t0 = std::chrono::steady_clock::now();
    compiles_++;
    vk::Spirv spv;
    std::memset(out.locations, -1, sizeof(out.locations));
    out.stage = stage;
    out.entry = entry;
    if (!vk::CompileHlsl(hlsl, entry.c_str(), stage, spv)) return false;
    if (stage == vk::Stage::Vertex) MarkPositionInvariant(spv.code);
    std::memcpy(out.locations, spv.locations, sizeof(out.locations));
    out.obj = MakeShader(stage, entry.c_str(), spv.code.data(), spv.code.size() * 4, false);
    if (!out.obj) return false;
    size_t n = 0;
    if (fn.vkGetShaderBinaryDataEXT(dev_.dev, out.obj, &n, nullptr) == VK_SUCCESS && n) {
        out.binary.resize(n);
        if (fn.vkGetShaderBinaryDataEXT(dev_.dev, out.obj, &n, out.binary.data()) != VK_SUCCESS) out.binary.clear();
        out.binary.resize(n);
        Keep(hash, out);
        out.binary.clear();
        out.binary.shrink_to_fit();
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 50.0) LOGI("vk: a %s shader built in %.0f ms", stage == vk::Stage::Vertex ? "vertex" : "pixel", ms);
    return true;
}

// shaders.vkb: the magic, the driver's binary UUID and version, then records: hash, stage, the 16 input locations,
// the entry's length and name, the binary's length and the binary
void VkBackend::Keep(uint64_t hash, const Built& b) {
    std::lock_guard lock(build_lock_);
    if (!kept_file_) return;
    uint8_t stage = uint8_t(b.stage);
    uint8_t entry = uint8_t(b.entry.size());
    uint32_t n = uint32_t(b.binary.size());
    std::fwrite(&hash, 8, 1, kept_file_);
    std::fwrite(&stage, 1, 1, kept_file_);
    std::fwrite(b.locations, 1, 16, kept_file_);
    std::fwrite(&entry, 1, 1, kept_file_);
    std::fwrite(b.entry.data(), 1, entry, kept_file_);
    std::fwrite(&n, 4, 1, kept_file_);
    std::fwrite(b.binary.data(), 1, n, kept_file_);
    std::fflush(kept_file_);
}

void VkBackend::LoadKept() {
    std::string path = (port::HomeDir() / "shaders.vkb").string();
    bool fresh = true;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char magic[8];
        uint8_t uuid[VK_UUID_SIZE];
        uint32_t version = 0;
        bool same = std::fread(magic, 1, 8, f) == 8 && !std::memcmp(magic, kBinaryMagic, 8) &&
                    std::fread(uuid, 1, VK_UUID_SIZE, f) == VK_UUID_SIZE && std::fread(&version, 4, 1, f) == 1 &&
                    !std::memcmp(uuid, dev_.binary_uuid, VK_UUID_SIZE) && version == dev_.binary_version;
        if (!same) LOGI("vk: %s is from another driver, shaders compile afresh", path.c_str());
        fresh = !same;
        for (; same;) {
            uint64_t hash;
            uint8_t stage, entry;
            uint32_t n;
            Built b;
            if (std::fread(&hash, 8, 1, f) != 1 || std::fread(&stage, 1, 1, f) != 1 ||
                std::fread(b.locations, 1, 16, f) != 16 || std::fread(&entry, 1, 1, f) != 1) break;
            b.entry.resize(entry);
            if (std::fread(b.entry.data(), 1, entry, f) != entry || std::fread(&n, 4, 1, f) != 1 || n > (16u << 20)) {
                break;
            }
            b.binary.resize(n);
            if (std::fread(b.binary.data(), 1, n, f) != n) break;  // a record cut short: the rest is lost
            b.stage = vk::Stage(stage);
            built_[hash] = std::move(b);
        }
        std::fclose(f);
    }
    kept_file_ = std::fopen(path.c_str(), fresh ? "wb" : "ab");
    if (!kept_file_) {
        LOGW("vk: %s could not be written, shaders are not kept", path.c_str());
        return;
    }
    if (fresh) {
        std::fwrite(kBinaryMagic, 1, 8, kept_file_);
        std::fwrite(dev_.binary_uuid, 1, VK_UUID_SIZE, kept_file_);
        std::fwrite(&dev_.binary_version, 4, 1, kept_file_);
        std::fflush(kept_file_);
    }
    // the build threads make every kept shader before the draws ask for them
    for (auto& [hash, b] : built_) jobs_.push_back({hash, {}});
    kept_ = built_.size();
    LOGI("vk: %zu shaders kept in %s", kept_, path.c_str());
}

void VkBackend::StartBuilders() {
    for (int i = 0; i < kBuildThreads; i++) {
        // glslang and the driver's compiler nest deep
        constexpr size_t kStack = 8u << 20;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kStack);
        pthread_t t;
        pthread_create(&t, &attr, [](void* self) -> void* {
            port::ThreadStarted();
            port::NameThread("vk builder");
            static_cast<VkBackend*>(self)->Builder();
            return nullptr;
        }, this);
        pthread_detach(t);
        pthread_attr_destroy(&attr);
    }
}

void VkBackend::Builder() {
    // below the render thread and the title's: a build may take its time, a frame may not
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        Job job;
        Built b;
        {
            std::unique_lock lock(build_lock_);
            build_cv_.wait(lock, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
            auto it = built_.find(job.hash);
            if (it == built_.end()) continue;
            if (job.hlsl.empty()) {
                // a kept binary, unless a draw has made it already
                if (it->second.state != Built::kKept || it->second.binary.empty()) continue;
                it->second.state = Built::kBuilding;
                it->second.from_binary = true;
                b.binary = std::move(it->second.binary);
            }
            b.stage = it->second.stage;
            b.entry = it->second.entry;
            std::memcpy(b.locations, it->second.locations, sizeof(b.locations));
        }
        bool ok;
        if (job.hlsl.empty()) {
            b.obj = MakeShader(b.stage, b.entry.c_str(), b.binary.data(), b.binary.size(), true);
            ok = b.obj != VK_NULL_HANDLE;
            if (!ok) kept_failed_++;
        } else {
            ok = Build(job.hash, job.hlsl, b.stage, b.entry, b);
        }
        std::lock_guard lock(build_lock_);
        Built& to = built_[job.hash];
        to.obj = b.obj;
        std::memcpy(to.locations, b.locations, sizeof(to.locations));
        to.binary.clear();
        to.binary.shrink_to_fit();
        // a kept binary the driver refuses: the draw that wants it compiles it again
        to.state = ok ? Built::kReady : job.hlsl.empty() ? Built::kKept : Built::kFailed;
        to.from_binary = false;
        build_cv_.notify_all();
        bool idle = jobs_.empty();
        if (idle && kept_) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            LOGI("vk: the %zu kept shaders made in %.0f ms%s", kept_, ms, kept_failed_ ? ", some refused" : "");
            kept_ = 0;
        }
    }
}

// a draw key's shader: the HLSL made, hashed and its build asked for on the key's first draw; null until built
const Resolved& VkBackend::Shader(uint64_t key, vk::Stage stage, const std::string& (*gen)(void*), void* arg) {
    auto it = keys_.find(key);
    if (it == keys_.end()) {
        it = keys_.emplace(key, Resolved{}).first;
        std::string hlsl = gen(arg);
        if (!hlsl.empty()) Request(it->second, std::move(hlsl), stage, "main");
    } else if (!it->second.obj && it->second.hash && !Lookup(it->second)) {
        // its kept binary was refused: the next draw makes the HLSL again for a build
        keys_.erase(it);
        static const Resolved none;
        return none;
    }
    return it->second;
}

void VkBackend::Request(Resolved& r, std::string hlsl, vk::Stage stage, const char* entry) {
    uint64_t hash = Hash(hlsl.data(), hlsl.size(), uint64_t(stage) + 1);
    r.hash = Hash(entry, std::strlen(entry), hash) | 1;
    std::unique_lock lock(build_lock_);
    // a new one is kKept without a binary: built from the HLSL below
    Built& b = built_[r.hash];
    if (b.state == Built::kKept && !b.binary.empty()) {
        // kept, not made yet: made here, it takes a moment
        b.state = Built::kBuilding;
        b.from_binary = true;
        std::vector<uint8_t> binary = std::move(b.binary);
        std::string name = b.entry;
        lock.unlock();
        VkShaderEXT obj;
        {
            auto timed = Time(kBuildWait);
            obj = MakeShader(stage, name.c_str(), binary.data(), binary.size(), true);
        }
        lock.lock();
        b.obj = obj;
        b.from_binary = false;
        b.state = obj ? Built::kReady : Built::kKept;
        build_cv_.notify_all();
        if (!obj) kept_failed_++;
    }
    if (b.state == Built::kKept) {
        b.state = Built::kBuilding;
        b.from_binary = false;
        b.stage = stage;
        b.entry = entry;
        if (app::Get().async_shaders) {
            jobs_.push_front({r.hash, std::move(hlsl)});
            build_cv_.notify_one();
        } else {
            lock.unlock();
            Built made;
            bool ok;
            {
                auto timed = Time(kBuildWait);
                ok = Build(r.hash, hlsl, stage, entry, made);
            }
            lock.lock();
            b.obj = made.obj;
            std::memcpy(b.locations, made.locations, sizeof(b.locations));
            b.state = ok ? Built::kReady : Built::kFailed;
            build_cv_.notify_all();
        }
    }
    lock.unlock();
    Lookup(r);
}

// a build's result for the key; a kept binary being made is waited for, a compile is not. false: the kept binary
// was refused and the HLSL is wanted again
bool VkBackend::Lookup(Resolved& r) {
    std::unique_lock lock(build_lock_);
    auto it = built_.find(r.hash);
    if (it == built_.end()) return true;
    Built& b = it->second;
    if (b.state == Built::kBuilding && b.from_binary) {
        auto timed = Time(kBuildWait);
        build_cv_.wait(lock, [&] { return b.state != Built::kBuilding || !b.from_binary; });
    }
    if (b.state == Built::kReady) {
        r.obj = b.obj;
        r.locations = b.locations;
    } else if (b.state == Built::kFailed) {
        r.hash = 0;
    }
    return !(b.state == Built::kKept && b.binary.empty());
}

void VkBackend::CreateBlit() {
    Request(blit_vs_, kBlitHlsl, vk::Stage::Vertex, "vs");
    Request(blit_ps_, kBlitHlsl, vk::Stage::Pixel, "psmain");
    Await({&blit_vs_, &blit_ps_});
    if (!blit_vs_.obj || !blit_ps_.obj) LOGE("vk: the present pass did not compile");
    SamplerDesc lin;
    lin.min = lin.mag = VK_FILTER_LINEAR;
    lin.wrap[0] = lin.wrap[1] = lin.wrap[2] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lin.max_lod = 0;
    blit_linear_ = Sampler(lin);
    SamplerDesc pt = lin;
    pt.min = pt.mag = VK_FILTER_NEAREST;
    blit_point_ = Sampler(pt);
}

// asked for on the build threads: the renderer waits for them here
void VkBackend::Await(std::initializer_list<Resolved*> shaders) {
    for (Resolved* r : shaders) {
        std::unique_lock lock(build_lock_);
        build_cv_.wait(lock, [&] {
            auto it = built_.find(r->hash);
            return it == built_.end() || it->second.state != Built::kBuilding;
        });
        lock.unlock();
        Lookup(*r);
    }
}

void VkBackend::PostState(const VkViewport& vp, const VkRect2D& sc, const float constants[4]) {
    VkCommandBuffer cmd = Cmd();
    SetFixedState();
    fn.vkCmdSetViewportWithCount(cmd, 1, &vp);
    fn.vkCmdSetScissorWithCount(cmd, 1, &sc);
    VkBool32 off = VK_FALSE;
    VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                               VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    VkColorComponentFlags mask = 0xF;
    fn.vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &off);
    fn.vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
    fn.vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
    fn.vkCmdSetBlendConstants(cmd, constants);
    fn.vkCmdSetDepthTestEnable(cmd, VK_FALSE);
    fn.vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
    fn.vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
    fn.vkCmdSetStencilTestEnable(cmd, VK_FALSE);
    fn.vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                         VK_COMPARE_OP_ALWAYS);
    fn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    fn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    fn.vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
    fn.vkCmdSetDepthBias(cmd, 0, 0, 0);
    fn.vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    fn.vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    fn.vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
    fn.vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    fn.vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
}

void VkBackend::DrawPost(const vk::Image& out, uint32_t w, uint32_t h, bool clear, const Resolved& vs,
                         const Resolved& ps, const float* params, size_t bytes,
                         std::initializer_list<VkImageView> views) {
    Alloc cb = Upload(bytes, size_t(ubo_align_));
    std::memcpy(cb.cpu, params, bytes);
    Outside();  // the pass before written, for this one to read
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = out.attach;
    colour.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colour.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    VkCommandBuffer cmd = Cmd();
    fn.vkCmdBeginRendering(cmd, &ri);
    // D3D's clip space, as the present pass: the viewport upside down
    const float zero[4] = {};
    PostState({0, float(h), float(w), -float(h), 0.0f, 1.0f}, {{0, 0}, {w, h}}, zero);
    BindShaders(vs.obj, ps.obj);
    VkDescriptorBufferInfo ubo{cb.buffer, cb.off, bytes};
    VkDescriptorImageInfo img[3];
    size_t i = 0;
    for (VkImageView v : views) img[i++] = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
    for (; i < 3; i++) img[i] = {VK_NULL_HANDLE, dummy_[kTex2D].view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo smp[2] = {{blit_linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                                    {blit_point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet wr[3] = {};
    for (auto& x : wr) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstBinding = vk::kBindingUbo + 1;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[0].pBufferInfo = &ubo;
    wr[1].dstBinding = vk::kBindingTexture;
    wr[1].descriptorCount = 3;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wr[1].pImageInfo = img;
    wr[2].dstBinding = vk::kBindingSampler;
    wr[2].descriptorCount = 2;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    wr[2].pImageInfo = smp;
    fn.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, wr);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRendering(cmd);
    pending_ = true;
    // the draws after this set their state afresh
    fixed_ = false;
    vin_set_ = false;
}

// the shaders once, the first time SMAA is on (a moment's wait; kept in the shader cache after), the lookups once, the
// targets at the frame's size
bool VkBackend::EnsureSmaa(uint32_t w, uint32_t h) {
    if (smaa_failed_) return false;
    if (!smaa_built_) {
        static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
        const char* vs[3] = {"vs_edges", "vs_weights", "vs_blend"};
        const char* ps[3] = {"smaa_edges", "smaa_weights", "smaa_blend"};
        for (int i = 0; i < 3; i++) {
            Request(smaa_vs_[i], hlsl, vk::Stage::Vertex, vs[i]);
            Request(smaa_ps_[i], hlsl, vk::Stage::Pixel, ps[i]);
        }
        Await({&smaa_vs_[0], &smaa_vs_[1], &smaa_vs_[2], &smaa_ps_[0], &smaa_ps_[1], &smaa_ps_[2]});
        smaa_built_ = true;
        for (int i = 0; i < 3; i++) smaa_failed_ |= !smaa_vs_[i].obj || !smaa_ps_[i].obj;
        // smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
        struct Table {
            vk::Image& img;
            VkFormat fmt;
            uint32_t w, h, bpp;
            const uint8_t* data;
        } tables[2] = {{smaa_area_, VK_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea},
                        {smaa_search_, VK_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch}};
        static_assert(sizeof(kSmaaArea) == 160 * 560 * 2 && sizeof(kSmaaSearch) == 64 * 16);
        for (Table& l : tables) {
            if (smaa_failed_ || !dev_.CreateImage(l.img, l.fmt, l.w, l.h, 1, 1, 1, 0)) {
                smaa_failed_ = true;
                break;
            }
            InitLayout(l.img);
            Alloc a = Upload(size_t(l.w) * l.h * l.bpp, 16);
            std::memcpy(a.cpu, l.data, size_t(l.w) * l.h * l.bpp);
            Outside();
            CopyToImage(l.img, a, 0, 0, l.w, l.h, 1, l.w);
        }
        if (smaa_failed_) {
            LOGE("vk: SMAA could not be set up, it stays off");
            return false;
        }
        LOGI("vk: SMAA ready");
    }
    if (smaa_w_ == w && smaa_h_ == h) return true;
    const VkFormat fmt[3] = {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, kColourFormat};
    for (int i = 0; i < 3; i++) {
        if (smaa_[i].image) Retire(smaa_[i]);
        if (!dev_.CreateImage(smaa_[i], fmt[i], w, h, 1, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            smaa_w_ = smaa_h_ = 0;
            return false;
        }
        InitLayout(smaa_[i]);
    }
    smaa_w_ = w;
    smaa_h_ = h;
    return true;
}

const vk::Image* VkBackend::Smaa(const Target& t) {
    if (!EnsureSmaa(t.w, t.h)) return nullptr;
    float p[4] = {1.0f / t.w, 1.0f / t.h, float(t.w), float(t.h)};
    DrawPost(smaa_[0], t.w, t.h, true, smaa_vs_[0], smaa_ps_[0], p, sizeof(p), {t.img.view});
    DrawPost(smaa_[1], t.w, t.h, true, smaa_vs_[1], smaa_ps_[1], p, sizeof(p),
             {smaa_[0].view, smaa_area_.view, smaa_search_.view});
    DrawPost(smaa_[2], t.w, t.h, false, smaa_vs_[2], smaa_ps_[2], p, sizeof(p), {t.img.view, smaa_[1].view});
    return &smaa_[2];
}

void VkBackend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    ramp_set_ = brightness << 8 | contrast;
    uint8_t ramp[256 * 4];
    gamma_ = GammaRamp(brightness, contrast, has_game_ramp_ ? game_ramp_.data() : nullptr, ramp);
    Alloc a = Upload(sizeof(ramp), 16);
    std::memcpy(a.cpu, ramp, sizeof(ramp));
    Outside();
    CopyToImage(lut_, a, 0, 0, 256, 1, 1, 256);
}

VkSampler VkBackend::Sampler(const SamplerDesc& d) {
    uint64_t key = Hash(&d, sizeof(d));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = VkFilter(d.mag);
    ci.minFilter = VkFilter(d.min);
    ci.mipmapMode = VkSamplerMipmapMode(d.mip);
    ci.addressModeU = VkSamplerAddressMode(d.wrap[0]);
    ci.addressModeV = VkSamplerAddressMode(d.wrap[1]);
    ci.addressModeW = VkSamplerAddressMode(d.wrap[2]);
    float bias_max = dev_.props.limits.maxSamplerLodBias;
    ci.mipLodBias = std::clamp(d.bias, -bias_max, bias_max);
    ci.anisotropyEnable = d.aniso > 1 && dev_.max_anisotropy > 1;
    ci.maxAnisotropy = std::min(d.aniso, dev_.max_anisotropy);
    ci.compareEnable = d.compare != 0;
    ci.compareOp = d.compare ? VkCompareOp(d.compare - 1) : VK_COMPARE_OP_NEVER;
    ci.minLod = d.min_lod;
    ci.maxLod = std::max(d.max_lod, d.min_lod);
    VkSamplerCustomBorderColorCreateInfoEXT border{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    bool any_border = d.wrap[0] == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                      d.wrap[1] == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                      d.wrap[2] == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (any_border && dev_.custom_border) {
        std::memcpy(border.customBorderColor.float32, d.border, sizeof(d.border));
        border.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &border;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else {
        // the three Vulkan has without the extension, the nearest
        bool white = d.border[0] + d.border[1] + d.border[2] > 1.5f;
        ci.borderColor = d.border[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : white            ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                            : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }
    VkSampler s = VK_NULL_HANDLE;
    if (fn.vkCreateSampler(dev_.dev, &ci, nullptr, &s) != VK_SUCCESS) LOGE("vk: no sampler");
    samplers_[key] = s;
    return s;
}

// targets

void VkBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    Target& t = it->second;
    if (cur_rt_ == &t || cur_ds_ == &t) {
        Outside();
        cur_rt_ = cur_ds_ = nullptr;
    }
    if (bound_rt_ == &t || bound_ds_ == &t) bound_rt_ = bound_ds_ = nullptr;
    Retire(t.img);
    Retire(t.fb);
    targets_.erase(it);
}

Target* VkBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header || (!depth && IsShadowColour(s))) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    auto it = targets_.find(TargetKey(data, depth));
    if (it == targets_.end()) return nullptr;
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    return it->second.gw == gw && it->second.gh == gh ? &it->second : nullptr;
}

Target* VkBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header || (!depth && IsShadowColour(s))) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint64_t key = TargetKey(data, depth);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(key);
    if (it != targets_.end()) {
        if (it->second.gw == gw && it->second.gh == gh) return &it->second;
        ReleaseTarget(it);
    }
    Target t;
    t.depth = depth;
    t.gw = t.w = std::max(gw, 1u);
    t.gh = t.h = std::max(gh, 1u);
    VkImageUsageFlags use = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!dev_.CreateImage(t.img, depth ? dev_.depth_format : kColourFormat, t.w, t.h, 1, 1, 1, use)) return nullptr;
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key] = std::move(t);
    // a new target starts cleared, as d3d12.cpp's
    InitLayout(ref.img);
    VkImageSubresourceRange all{ref.img.aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue z{1.0f, 0};
        fn.vkCmdClearDepthStencilImage(Cmd(), ref.img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
    } else {
        VkClearColorValue zero{};
        fn.vkCmdClearColorImage(Cmd(), ref.img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    pending_ = true;
    return &ref;
}

bool VkBackend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
    bound_rt_ = rt;
    bound_ds_ = ds;
    return true;
}

// CPU pixels of a colour surface into its host target; the target is the console's size, so all of them
void VkBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth || Format(d.format).host != Host::BGRA8) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    uint32_t w = std::min(d.width, t.w), h = std::min(d.height, t.h);
    Alloc a = Upload(size_t(pitch) * h, 16);
    if (a.cpu) std::memcpy(a.cpu, px.data(), std::min(px.size(), size_t(pitch) * h));
    Outside();
    CopyToImage(t.img, a, 0, 0, w, h, 1, pitch / 4);
    if (!t.read.empty() && d.width == t.gw && d.height == t.gh && pitch == t.gw * 4) t.read = px;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// a copy of a colour target for a draw that reads the target it draws to
VkImageView VkBackend::FeedbackCopy(Target& t) {
    if (!t.fb.image) {
        if (!dev_.CreateImage(t.fb, kColourFormat, t.w, t.h, 1, 1, 1, 0)) return VK_NULL_HANDLE;
        InitLayout(t.fb);
    }
    Outside();
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {t.w, t.h, 1};
    fn.vkCmdCopyImage(Cmd(), t.img.image, VK_IMAGE_LAYOUT_GENERAL, t.fb.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    pending_ = true;
    return t.fb.view;
}

// textures

VkImageView VkBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                         DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return VK_NULL_HANDLE;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    stages.levels[stage] = 1;
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (&t == draw_rt_) return FeedbackCopy(t);
    return t.img.view;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img.image && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

VkFormat TextureFormat(const FormatInfo& fi) {
    switch (fi.host) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        // depth as float, sampled with a compare
        case Host::R32F: return fi.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
        default: return kColourFormat;
    }
}

bool VkBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    tx.levels = d.levels;
    // DXT that is not whole blocks: one level, as d3d12.cpp
    if (fi.compressed && ((d.width & 3) || (d.height & 3))) tx.levels = 1;
    uint32_t w = d.width, h = d.height, z = d.cube ? 1 : std::max(d.depth, 1u);
    if (!dev_.CreateImage(tx.img, TextureFormat(fi), w, h, z, d.cube ? 6 : 1, tx.levels, 0, d.cube)) return false;
    InitLayout(tx.img);
    return true;
}

void VkBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p) {
    auto timed = Time(kTexUpload);
    tex_uploads_++;
    tex_upload_bytes_ += TotalSize(d);
    uint32_t faces = d.cube ? 6 : 1;
    bool bc = fi.compressed;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, nullptr, px, pitch);
            if (d.cube && !l) CheckNormCube(tx.norm, d, fi, f, px, pitch);
            uint32_t w = std::max(d.width >> l, 1u), h = std::max(d.height >> l, 1u);
            uint32_t z = d.cube ? 1 : std::max(d.depth >> l, 1u);
            if (d.linear) {
                w = d.width;
                h = d.height;
                z = 1;
            }
            size_t bytes = bc ? size_t((w + 3) / 4) * ((h + 3) / 4) * block * z : size_t(pitch) * h * z;
            px.resize(std::max(px.size(), bytes));
            Alloc a = Upload(bytes, 16);
            if (!a.cpu) continue;
            std::memcpy(a.cpu, px.data(), bytes);
            Outside();
            CopyToImage(tx.img, a, l, f, w, h, z, bc ? 0 : pitch / 4);
        }
    }
}

VkImageView VkBackend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (fi.host == Host::None) return VK_NULL_HANDLE;
    Tex& tx = textures_[d.data];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(d.data);
        return VK_NULL_HANDLE;
    }
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same) {
        tx.frame = presents_ + 1;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data);
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)));
        }
    }
    stages.levels[stage] = tx.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return tx.img.view;
}

VkImageView VkBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return VK_NULL_HANDLE;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(stage, d, fi, stages);
}

void VkBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 1024;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Retire(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

// a draw

// the states shader objects want set that the title never changes, once per command buffer
void VkBackend::SetFixedState() {
    if (fixed_) return;
    VkCommandBuffer c = Cmd();
    fn.vkCmdSetRasterizerDiscardEnable(c, VK_FALSE);
    fn.vkCmdSetRasterizationSamplesEXT(c, VK_SAMPLE_COUNT_1_BIT);
    VkSampleMask all = ~0u;
    fn.vkCmdSetSampleMaskEXT(c, VK_SAMPLE_COUNT_1_BIT, &all);
    fn.vkCmdSetAlphaToCoverageEnableEXT(c, VK_FALSE);
    fn.vkCmdSetDepthBoundsTestEnable(c, VK_FALSE);
    fn.vkCmdSetPrimitiveRestartEnable(c, VK_FALSE);
    fn.vkCmdSetLineWidth(c, 1.0f);
    fixed_ = true;
    // and every state after it set again
    dyn_ = {};
    dyn_.blend = 2;
    dyn_.ztest = 2;
    dyn_.stest = 2;
    dyn_.bias = 2;
    dyn_.cull = ~0u;
    dyn_.topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    dyn_.fill = VK_POLYGON_MODE_MAX_ENUM;
    dyn_.front = VK_FRONT_FACE_MAX_ENUM;
    dyn_.mask = ~0u;
    dyn_.zfunc = dyn_.sfunc = VK_COMPARE_OP_MAX_ENUM;
    dyn_.vp.width = -1;
    dyn_.sc.extent.width = ~0u;
    dyn_.eq.colorBlendOp = VK_BLEND_OP_MAX_ENUM;
    dyn_.sfail = VK_STENCIL_OP_MAX_ENUM;
    dyn_.constants[0] = -1;
    dyn_.bias_const = -1e30f;
    dyn_.sref = ~0u;
}

void VkBackend::ApplyState(const State& s, const Assembled& a) {
    VkCommandBuffer c = Cmd();
    SetFixedState();
    // blend
    VkBool32 blend = SimpleState(s, kMBlendEnable) ? VK_TRUE : VK_FALSE;
    if (blend != dyn_.blend) {
        fn.vkCmdSetColorBlendEnableEXT(c, 0, 1, &blend);
        dyn_.blend = blend;
    }
    VkColorBlendEquationEXT eq{};
    eq.srcColorBlendFactor = eq.srcAlphaBlendFactor = Blend(SimpleState(s, kMSrcBlend));
    eq.dstColorBlendFactor = eq.dstAlphaBlendFactor = Blend(SimpleState(s, kMDestBlend));
    eq.colorBlendOp = eq.alphaBlendOp = BlendOp(SimpleState(s, kMBlendOp));
    if (!blend) eq = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                      VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    if (std::memcmp(&eq, &dyn_.eq, sizeof(eq))) {
        fn.vkCmdSetColorBlendEquationEXT(c, 0, 1, &eq);
        dyn_.eq = eq;
    }
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    if (std::memcmp(bf, dyn_.constants, sizeof(bf))) {
        fn.vkCmdSetBlendConstants(c, bf);
        std::memcpy(dyn_.constants, bf, sizeof(bf));
    }
    // the console's enables are one per byte, A R G B
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    VkColorComponentFlags mask = ((cw >> 16) & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) |
                                 ((cw >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                 (cw & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) | ((cw >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (mask != dyn_.mask) {
        fn.vkCmdSetColorWriteMaskEXT(c, 0, 1, &mask);
        dyn_.mask = mask;
    }

    // depth and stencil
    VkBool32 ztest = bound_ds_ && s.rs[kRsZEnable] ? VK_TRUE : VK_FALSE;
    VkBool32 zwrite = ztest && SimpleState(s, kMZWriteEnable) ? VK_TRUE : VK_FALSE;
    VkCompareOp zfunc = ztest ? Cmp(SimpleState(s, kMZFunc)) : VK_COMPARE_OP_ALWAYS;
    if (ztest != dyn_.ztest) fn.vkCmdSetDepthTestEnable(c, ztest);
    if (zwrite != dyn_.zwrite || ztest != dyn_.ztest) fn.vkCmdSetDepthWriteEnable(c, zwrite);
    if (zfunc != dyn_.zfunc) fn.vkCmdSetDepthCompareOp(c, zfunc);
    dyn_.ztest = ztest;
    dyn_.zwrite = zwrite;
    dyn_.zfunc = zfunc;
    VkBool32 stest = bound_ds_ && s.rs[kRsStencilEnable] ? VK_TRUE : VK_FALSE;
    if (stest != dyn_.stest) {
        fn.vkCmdSetStencilTestEnable(c, stest);
        dyn_.stest = stest;
    }
    VkStencilOp sfail = StencilOp(s.rs[kRsStencilFail]), szfail = StencilOp(SimpleState(s, kMStencilZFail));
    VkStencilOp spass = StencilOp(SimpleState(s, kMStencilPass));
    VkCompareOp sfunc = Cmp(SimpleState(s, kMStencilFunc));
    if (sfail != dyn_.sfail || szfail != dyn_.szfail || spass != dyn_.spass || sfunc != dyn_.sfunc) {
        fn.vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_AND_BACK, sfail, spass, szfail, sfunc);
        dyn_.sfail = sfail;
        dyn_.szfail = szfail;
        dyn_.spass = spass;
        dyn_.sfunc = sfunc;
    }
    uint32_t sref = SimpleState(s, kMStencilRef) & 0xFF, smask = SimpleState(s, kMStencilMask) & 0xFF;
    uint32_t swrite = SimpleState(s, kMStencilWriteMask) & 0xFF;
    if (sref != dyn_.sref) fn.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, sref);
    if (smask != dyn_.smask || dyn_.sref == ~0u) fn.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
    if (swrite != dyn_.swrite || dyn_.sref == ~0u) fn.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, swrite);
    dyn_.sref = sref;
    dyn_.smask = smask;
    dyn_.swrite = swrite;
    // depth bias in depth buffer units, as for the console's 24 bit buffer
    VkBool32 bias = bound_ds_ && a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
    if (bias != dyn_.bias) {
        fn.vkCmdSetDepthBiasEnable(c, bias);
        dyn_.bias = bias;
    }
    float bconst = bias ? float(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale)) : 0;
    float bslope = bias ? Bits(SimpleState(s, kMSlopeScale)) : 0;
    if (bconst != dyn_.bias_const || bslope != dyn_.bias_slope) {
        fn.vkCmdSetDepthBias(c, bconst, 0.0f, bslope);
        dyn_.bias_const = bconst;
        dyn_.bias_slope = bslope;
    }

    // rasterizer: with the viewport upside down the image is D3D's, and so are its windings
    uint32_t cull = s.rs[kRsCullMode];
    VkCullModeFlags cm = cull && a.topology == Topology::Triangles ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // culling clockwise leaves counter clockwise as the front
    VkFrontFace ff = cull == kCullCw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (cm != dyn_.cull) fn.vkCmdSetCullMode(c, cm);
    if (ff != dyn_.front) fn.vkCmdSetFrontFace(c, ff);
    dyn_.cull = cm;
    dyn_.front = ff;
    VkPolygonMode fill = s.rs[kRsFillMode] == kFillWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    if (fill != dyn_.fill) {
        fn.vkCmdSetPolygonModeEXT(c, fill);
        dyn_.fill = fill;
    }
    VkPrimitiveTopology topo = PrimitiveTopology(a);
    if (topo != dyn_.topo) {
        fn.vkCmdSetPrimitiveTopology(c, topo);
        dyn_.topo = topo;
    }
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void VkBackend::SetViewportScissor(const State& s, uint32_t gw) {
    int W = int(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    int H = int(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    VkViewport vp{0, float(H), float(W), -float(H), 0.0f, 1.0f};
    if (std::memcmp(&vp, &dyn_.vp, sizeof(vp))) {
        fn.vkCmdSetViewportWithCount(Cmd(), 1, &vp);
        dyn_.vp = vp;
    }
    float sc = float(W) / gw;
    int x0 = int(s.viewport.x * sc);
    int y0 = int(s.viewport.y * sc);
    int x1 = int((s.viewport.x + s.viewport.w) * sc);
    int y1 = int((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int(s.scissors[0] * sc));
        y0 = std::max(y0, int(s.scissors[1] * sc));
        x1 = std::min(x1, int(s.scissors[2] * sc));
        y1 = std::min(y1, int(s.scissors[3] * sc));
    }
    x0 = std::clamp(x0, 0, W);
    y0 = std::clamp(y0, 0, H);
    x1 = std::clamp(x1, 0, W);
    y1 = std::clamp(y1, 0, H);
    VkRect2D r{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    if (std::memcmp(&r, &dyn_.sc, sizeof(r))) {
        fn.vkCmdSetScissorWithCount(Cmd(), 1, &r);
        dyn_.sc = r;
    }
}

void VkBackend::BindShaders(VkShaderEXT vs, VkShaderEXT ps) {
    if (vs == bound_vs_ && ps == bound_ps_) return;
    VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkShaderEXT objs[2] = {vs, ps};
    fn.vkCmdBindShadersEXT(Cmd(), 2, stages, objs);
    bound_vs_ = vs;
    bound_ps_ = ps;
}

bool VkBackend::UploadVertices(const Assembled& a, Vertices& v) {
    // what the draw addresses past a buffer's end is undefined in Vulkan, where D3D12 gives 0: every buffer reaches
    // the last vertex read, zeros after the guest's
    uint32_t end[16] = {};
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        end[e.buffer] = std::max(end[e.buffer], uint32_t(e.offset) + FormatBytes(e.format));
    }
    uint64_t last = MaxVertex(a);
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        uint64_t need = std::max<uint64_t>(b.bytes, last * b.stride + std::max(end[i], b.stride));
        if (need > b.bytes) LOG_FIRST(8, "vk: a draw reads %llu bytes of a %u byte vertex buffer, zeros after",
                                      (unsigned long long)need, b.bytes);
        if (need > kSegment) {
            LOG_FIRST(8, "vk: %llu bytes of vertices in one draw, left out", (unsigned long long)need);
            return false;
        }
        Alloc vb = Upload(size_t(need), 16);
        if (b.src) std::memcpy(vb.cpu, b.src, b.bytes);
        if (need > b.bytes) std::memset(vb.cpu + b.bytes, 0, size_t(need - b.bytes));
        v.buf[i] = vb.buffer;
        v.off[i] = vb.off;
    }
    return true;
}

void VkBackend::SetVertexInputs(const Assembled& a, const int8_t* locations, const Vertices& v) {
    std::vector<VkVertexInputBindingDescription2EXT> bind;
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        VkVertexInputBindingDescription2EXT d{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        d.binding = i;
        d.stride = a.buffers[i].stride;
        d.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        d.divisor = 1;
        bind.push_back(d);
    }
    std::vector<VkVertexInputAttributeDescription2EXT> attr;
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1) || locations[r] < 0) continue;
        const VertexElement& e = a.elements[size_t(r)];
        VkFormat f = vk::FromDxgi(e.format);
        if (f == VK_FORMAT_UNDEFINED) {
            LOG_FIRST(8, "vk: vertex format %u not handled", e.format);
            continue;
        }
        VkVertexInputAttributeDescription2EXT d{VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        d.location = uint32_t(locations[r]);
        d.binding = e.buffer;
        d.format = f;
        d.offset = e.offset;
        attr.push_back(d);
    }
    auto same = [](const auto& x, const auto& y) {
        return x.size() == y.size() && !std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0]));
    };
    if (!vin_set_ || !same(bind, vin_bind_) || !same(attr, vin_attr_)) {
        fn.vkCmdSetVertexInputEXT(Cmd(), uint32_t(bind.size()), bind.data(), uint32_t(attr.size()), attr.data());
        vin_bind_ = std::move(bind);
        vin_attr_ = std::move(attr);
        vin_set_ = true;
    }
    if (a.buffer_count) fn.vkCmdBindVertexBuffers(Cmd(), 0, a.buffer_count, v.buf, v.off);
}

// for the draw key cache: the HLSL made only on a key's first draw
struct VertexGen {
    const State& s;
    const Assembled& a;
    const DrawShaders& sh;
    std::string out;
};
struct PixelGen {
    const PixelKey& pk;
    std::string out;
};

void VkBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    auto all = Time(kDrawAll);
    Assembled& a = asm_;
    {
        auto timed = Time(kAssemble);
        if (!Assemble(s, prim, start, count, indices, a)) return;
    }
    {
        auto timed = Time(kTargets);
        if (!BindTargets(s)) return;
    }
    draws_++;

    DrawShaders sh;
    {
        auto timed = Time(kKeys);
        sh = ClassifyShaders(s, a, count);
        if (sh.prog && !s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
        sh.vkey = VertexKey(s, a, sh);
    }

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        auto timed = Time(kTextures);
        for (int i = 0; i < 4; i++) stages.view[i] = BindTexture(s, i, stages);
    }
    PixelKey pk;
    {
        auto timed = Time(kKeys);
        pk = MakePixelKey(s, stages.info);
        pk.quad = sh.quad;
        sh.pkey = Hash(&pk, sizeof(pk));
    }

    const Resolved* vs;
    const Resolved* ps;
    {
        auto timed = Time(kShaders);
        VertexGen vg{s, a, sh, {}};
        vs = &Shader(sh.vkey, vk::Stage::Vertex, [](void* p) -> const std::string& {
            auto* g = static_cast<VertexGen*>(p);
            return g->out = VertexHlsl(g->s, g->a, g->sh);
        }, &vg);
        PixelGen pg{pk, {}};
        ps = &Shader(sh.pkey ^ kPixelKeySalt, vk::Stage::Pixel, [](void* p) -> const std::string& {
            auto* g = static_cast<PixelGen*>(p);
            return g->out = PixelHlsl(g->pk);
        }, &pg);
        if (!vs->obj || !ps->obj) {
            skipped_++;
            return;
        }
    }

    // everything the draw streams goes in first: a full ring may submit, the render pass begins after
    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    Alloc vc, dc;
    {
        auto timed = Time(kConstants);
        vc = Upload(sizeof(s.vsc), size_t(ubo_align_));
        std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
        DrawConstants c{};
        FillSurfaceConstants(s, size, c);
        FillCombinerConstants(s, c);
        c.hdr[0] = 1;
        c.hdr[1] = 1;
        FillTextureConstants(s, stages.rt, c);
        if (sh.fixed_transform) {
            FillTransformConstants(s, size, c);
            FillLightConstants(s, sh.fk, c);
        }
        dc = Upload(sizeof(c), size_t(ubo_align_));
        std::memcpy(dc.cpu, &c, sizeof(c));
    }
    Vertices verts;
    {
        auto timed = Time(kVertices);
        if (!UploadVertices(a, verts)) return;
    }
    Alloc ib;
    uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
    if (a.index_count) {
        ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
    }

    Inside(bound_rt_, bound_ds_);
    {
        auto timed = Time(kStages);
        uint32_t aniso = app::Get().aniso;
        bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
        VkDescriptorBufferInfo ubo[2] = {{vc.buffer, vc.off, sizeof(s.vsc)}, {dc.buffer, dc.off, sizeof(DrawConstants)}};
        VkDescriptorImageInfo img[4], smp[4];
        VkWriteDescriptorSet w[3] = {};
        for (int i = 0; i < 4; i++) {
            VkImageView v = stages.view[i];
            uint8_t kind = stages.info[i].kind;
            img[i] = {VK_NULL_HANDLE, v ? v : dummy_[kind <= kTexCube ? kind : 0].view, VK_IMAGE_LAYOUT_GENERAL};
            smp[i] = {Sampler(StageSampler(s, i, stages, clamp, aniso)), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        for (auto& x : w) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstBinding = vk::kBindingUbo;
        w[0].descriptorCount = 2;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = ubo;
        w[1].dstBinding = vk::kBindingTexture;
        w[1].descriptorCount = 4;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[1].pImageInfo = img;
        w[2].dstBinding = vk::kBindingSampler;
        w[2].descriptorCount = 4;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[2].pImageInfo = smp;
        fn.vkCmdPushDescriptorSetKHR(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, w);
    }
    {
        auto timed = Time(kState);
        BindShaders(vs->obj, ps->obj);
        ApplyState(s, a);
        SetViewportScissor(s, size.w);
    }
    {
        auto timed = Time(kVertices);
        SetVertexInputs(a, vs->locations, verts);
    }
    {
        auto timed = Time(kDraw);
        if (a.index_count) {
            fn.vkCmdBindIndexBuffer(Cmd(), ib.buffer, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            fn.vkCmdDrawIndexed(Cmd(), a.index_count, 1, 0, a.base_vertex, 0);
        } else {
            fn.vkCmdDraw(Cmd(), a.draw_count, 1, 0, 0);
        }
    }
}

void VkBackend::ClearTargets(Target* rt, Target* ds, const State& s, uint32_t count, const uint8_t* rects,
                             uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) {
        ClearTargets(rt, nullptr, s, count, rects, flags, color, z, stencil);
        ClearTargets(nullptr, ds, s, count, rects, flags, color, z, stencil);
        return;
    }
    // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    Target& t = rt ? *rt : *ds;
    std::vector<VkClearRect> rs;
    float sc = float(t.w) / t.gw;
    auto add = [&](int x0, int y0, int x1, int y1) {
        x0 = std::clamp(int(x0 * sc), 0, int(t.w));
        y0 = std::clamp(int(y0 * sc), 0, int(t.h));
        x1 = std::clamp(int(x1 * sc), 0, int(t.w));
        y1 = std::clamp(int(y1 * sc), 0, int(t.h));
        if (x1 > x0 && y1 > y0) rs.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        const auto& v = s.viewport;
        add(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
    }
    if (rs.empty()) return;
    // into the open render pass where it holds these targets, else one on them alone
    if (!(rendering_ && (!rt || rt == cur_rt_) && (!ds || ds == cur_ds_))) Inside(rt, ds);
    VkClearAttachment at[2];
    uint32_t n = 0;
    if (rt) {
        at[n] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        Color(color, at[n].clearValue.color.float32);
        n++;
        rt->drawn = true;
    }
    if (ds) {
        VkImageAspectFlags aspect = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                    ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        at[n] = {aspect, 0, {}};
        at[n].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        n++;
        ds->drawn = true;
    }
    fn.vkCmdClearAttachments(Cmd(), n, at, uint32_t(rs.size()), rs.data());
}

void VkBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    auto timed = Time(kClear);
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (rt || ds) ClearTargets(rt, ds, s, count, rects, flags, color, z, stencil);
}

void VkBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                     uint32_t dst_points) {
    auto timed = Time(kCopy);
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vk: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        int x0 = 0, y0 = 0, x1 = int(std::min(src_t->w, dst_t->w)), y1 = int(std::min(src_t->h, dst_t->h));
        int dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(src_rects, i);
            x0 = std::max(r.left, 0);
            y0 = std::max(r.top, 0);
            x1 = std::min(r.right, int(src_t->w));
            y1 = std::min(r.bottom, int(src_t->h));
            dx = x0;
            dy = y0;
            if (dst_points) {
                dx = int32_t(kernel::R32(dst_points + i * 8));
                dy = int32_t(kernel::R32(dst_points + i * 8 + 4));
            }
        }
        if (x1 <= x0 || y1 <= y0 || dx < 0 || dy < 0 || dx >= int(dst_t->w) || dy >= int(dst_t->h)) continue;
        x1 = std::min(x1, x0 + int(dst_t->w) - dx);
        y1 = std::min(y1, y0 + int(dst_t->h) - dy);
        VkImageCopy r{};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.srcOffset = {x0, y0, 0};
        r.dstOffset = {dx, dy, 0};
        r.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
        regions.push_back(r);
    }
    if (!regions.empty()) {
        Outside();
        fn.vkCmdCopyImage(Cmd(), src_t->img.image, VK_IMAGE_LAYOUT_GENERAL, dst_t->img.image, VK_IMAGE_LAYOUT_GENERAL,
                          uint32_t(regions.size()), regions.data());
        pending_ = true;
    }
    dst_t->drawn = true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void VkBackend::Sync(uint32_t data, uint32_t) {
    auto timed = Time(kSync);
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    std::vector<uint8_t> px;
    if (!ReadTarget(t, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.w * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void VkBackend::ResetLate(Late& l) {
    for (int i = 0; i < kLateSlots; i++) Retire(l.buf[i]);
    l = Late{};
}

// render thread: slots the GPU has finished become deliverable
void VkBackend::PollLate(Late& l) {
    for (int i = 0; i < kLateSlots; i++) {
        if (!l.serial[i] || l.serial[i] > done_) continue;
        l.serial[i] = 0;
        l.done[i] = true;
    }
}

bool VkBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kLateSlots; i++) {
        bool recent = now - l.frame[i] <= uint64_t(kLateSlots) + 1;
        if (l.done[i] && recent && (best < 0 || l.seq[i] > l.seq[best])) best = i;
    }
    if (best < 0) return false;
    if (l.seq[best] > l.delivered) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        const uint8_t* src = l.buf[best].mem.map;
        for (uint32_t y = 0; y < l.gh; y++) {
            std::memcpy(dst + size_t(y) * pitch, src + size_t(y) * l.gw * 4, l.gw * 4);
        }
        l.delivered = l.seq[best];
    }
    return true;
}

void VkBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        ResetLate(l);
        l.gw = t.gw;
        l.gh = t.gh;
    }
    Poll();
    PollLate(l);
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.serial[slot]) return;
    VkDeviceSize bytes = VkDeviceSize(t.gw) * t.gh * 4;
    if (!l.buf[slot].buffer &&
        !dev_.CreateBuffer(l.buf[slot], bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)) {
        LOGE("vk: no read-back buffer");
        return;
    }
    Outside();
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {t.gw, t.gh, 1};
    fn.vkCmdCopyImageToBuffer(Cmd(), t.img.image, VK_IMAGE_LAYOUT_GENERAL, l.buf[slot].buffer, 1, &r);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    fn.vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                            0, nullptr);
    pending_ = true;
    l.serial[slot] = serial_;
    l.done[slot] = false;
    l.seq[slot] = ++late_seq_;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kLateSlots;
}

void VkBackend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        app::g_fps = float(fps_n_ / el);
        fps_n_ = 0;
        fps_t_ = now;
    }
    if (now - log_t_ >= std::chrono::seconds(10)) {
        uint64_t frames = std::max<uint64_t>(1, presents_ - last_presents_);
        size_t building;
        {
            std::lock_guard lock(build_lock_);
            building = jobs_.size();
        }
        LOGR("vk: %.1f fps, %llu draws a frame (%llu waited for shaders), %zu targets, %zu textures, %zu shaders, "
             "%zu to build", double(app::g_fps), (unsigned long long)((draws_ - last_draws_) / frames),
             (unsigned long long)skipped_, targets_.size(), textures_.size(), keys_.size(), building);
        static const char* const kNames[kPhases] = {"assemble", "textures", "tex upload", "shaders", "shader wait",
                                                    "constants", "stages", "state", "vertices", "draw call",
                                                    "ring wait", "clear", "copy", "sync", "present", "swap",
                                                    "targets", "keys", "whole draws"};
        char line[512];
        int at = std::snprintf(line, sizeof(line), "vk: ms a frame:");
        for (int i = 0; i < kPhases && at < int(sizeof(line)); i++)
            at += std::snprintf(line + at, sizeof(line) - size_t(at), " %s %.2f", kNames[i],
                                double(phase_ns_[i]) / 1e6 / double(frames));
        LOGR("%s", line);
        uint64_t cpu = port::ThreadCpuNs();
        double wall = std::chrono::duration<double, std::milli>(now - log_t_).count();
        if (log_cpu_ && cpu)
            LOGR("vk: the render thread ran on a CPU %.0f ms of %.0f ms (%.0f%%)", double(cpu - log_cpu_) / 1e6, wall,
                 100.0 * double(cpu - log_cpu_) / 1e6 / wall);
        log_cpu_ = cpu;
        LOGR("vk: a frame: %.1f texture uploads (%.0f KB), %.2f shader compiles", double(tex_uploads_) / double(frames),
             double(tex_upload_bytes_) / 1024.0 / double(frames), double(compiles_.exchange(0)) / double(frames));
        std::memset(phase_ns_, 0, sizeof(phase_ns_));
        tex_uploads_ = tex_upload_bytes_ = 0;
        skipped_ = 0;
        last_presents_ = presents_;
        last_draws_ = draws_;
        log_t_ = now;
    }
}

// SCDA_SHOT_EVERY=n: every nth frame as shots/frame_<present>.ppm beside the log, for looking at a run without a
// screen
void VkBackend::SaveShot(Target& t) {
    static const uint64_t every = [] {
        const char* e = std::getenv("SCDA_SHOT_EVERY");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    if (!every || presents_ % every) return;
    std::vector<uint8_t> px;
    if (!ReadTarget(t, px)) return;
    std::filesystem::path dir = port::HomeDir() / "shots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[48];
    std::snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)presents_);
    if (FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", t.w, t.h);
        for (size_t i = 0; i < px.size(); i += 4) {
            uint8_t rgb[3] = {px[i + 2], px[i + 1], px[i]};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
}

// the swap chain at the window's size and the vsync setting, with a semaphore for each of its images
bool VkBackend::Swapchain() {
    uint32_t bw, bh;
    port::DrawableSize(bw, bh);
    asked_w_ = bw;
    asked_h_ = bh;
    if (dev_.swapchain) fn.vkDeviceWaitIdle(dev_.dev);
    bool ok = dev_.Resize(bw, bh, app::Get().vsync);
    swap_stale_ = !ok;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore s : rendered_) fn.vkDestroySemaphore(dev_.dev, s, nullptr);
    rendered_.clear();
    for (size_t i = 0; i < dev_.swap_images.size(); i++) {
        VkSemaphore s = VK_NULL_HANDLE;
        fn.vkCreateSemaphore(dev_.dev, &si, nullptr, &s);
        rendered_.push_back(s);
    }
    while (acquire_.size() < dev_.swap_images.size() + 1) {
        VkSemaphore s = VK_NULL_HANDLE;
        fn.vkCreateSemaphore(dev_.dev, &si, nullptr, &s);
        acquire_.push_back(s);
        acquire_serial_.push_back(0);
    }
    return ok;
}

void VkBackend::Present(const Surface& front, bool pillarbox) {
    auto timed = Time(kPresent);
    const app::Settings& set = app::Get();
    uint32_t bw, bh;
    port::DrawableSize(bw, bh);
    // against the size asked for, not the one made: a surface that keeps another size would rebuild every frame
    if (swap_stale_ || set.vsync != dev_.vsync || bw != asked_w_ || bh != asked_h_) {
        if (rendering_ || recorded_) Submit();
        Swapchain();
    }
    if ((set.brightness << 8 | set.contrast) != ramp_set_) UploadGammaRamp(set.brightness, set.contrast);
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) SaveShot(*t);
    // what is shown: the frame, or SMAA's blend of it, made before the swap chain image is asked for
    VkImageView shown = t ? t->img.view : VK_NULL_HANDLE;
    if (t && set.smaa) {
        if (const vk::Image* aa = Smaa(*t)) shown = aa->view;
    }

    // the uniforms first, then the swap chain image
    float c[20] = {0, 0, 1, 1, gamma_ ? 1.0f : 0.0f, 0, 1, 1};
    uint32_t mode = set.colour_blind;
    if (mode) {
        std::array<float, 9> colour = ColourBlindMatrix(mode);
        for (int row = 0; row < 3; row++) std::memcpy(c + 8 + row * 4, colour.data() + row * 3, 3 * sizeof(float));
        c[11] = 1.0f;
    }
    Alloc cb = Upload(sizeof(c), size_t(ubo_align_));
    std::memcpy(cb.cpu, c, sizeof(c));

    uint32_t idx = 0;
    uint32_t sem = acquire_next_;
    acquire_next_ = (acquire_next_ + 1) % uint32_t(acquire_.size());
    WaitSerial(acquire_serial_[sem]);
    VkResult r = dev_.swapchain ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_SUCCESS) {
        auto swap = Time(kSwap);
        r = fn.vkAcquireNextImageKHR(dev_.dev, dev_.swapchain, UINT64_MAX, acquire_[sem], VK_NULL_HANDLE, &idx);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        // no image to show this frame: the work goes to the GPU all the same
        swap_stale_ = true;
        Submit();
    } else {
        Outside();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dev_.swap_images[idx];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn.vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colour.imageView = dev_.swap_views[idx];
        colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colour.clearValue.color = {{0, 0, 0, 1}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {dev_.swap_w, dev_.swap_h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &colour;
        fn.vkCmdBeginRendering(Cmd(), &ri);
        if (t) {
            float aspect = app::Aspect();
            float w = float(dev_.swap_w);
            float h = float(dev_.swap_h);
            float vw = std::min(w, h * aspect);
            float vh = vw / aspect;
            if (pillarbox) vw = vh * 4.0f / 3.0f;
            float x = std::floor((w - vw) / 2), y = std::floor((h - vh) / 2);
            VkCommandBuffer cmd = Cmd();
            PostState({x, y + vh, vw, -vh, 0.0f, 1.0f}, {{0, 0}, {dev_.swap_w, dev_.swap_h}}, c);
            BindShaders(blit_vs_.obj, blit_ps_.obj);
            VkDescriptorBufferInfo ubo{cb.buffer, cb.off, sizeof(c)};
            VkDescriptorImageInfo img[2] = {{VK_NULL_HANDLE, shown, VK_IMAGE_LAYOUT_GENERAL},
                                            {VK_NULL_HANDLE, lut_.view, VK_IMAGE_LAYOUT_GENERAL}};
            VkDescriptorImageInfo smp[2] = {{blit_linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                                            {blit_point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
            VkWriteDescriptorSet wr[3] = {};
            for (auto& x : wr) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[0].dstBinding = vk::kBindingUbo + 1;
            wr[0].descriptorCount = 1;
            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[0].pBufferInfo = &ubo;
            wr[1].dstBinding = vk::kBindingTexture;
            wr[1].descriptorCount = 2;
            wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            wr[1].pImageInfo = img;
            wr[2].dstBinding = vk::kBindingSampler;
            wr[2].descriptorCount = 2;
            wr[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            wr[2].pImageInfo = smp;
            fn.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, wr);
            fn.vkCmdDraw(cmd, 3, 1, 0, 0);
            // the draws after this set their state afresh
            fixed_ = false;
            vin_set_ = false;
        }
        fn.vkCmdEndRendering(Cmd());
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        fn.vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        uint64_t serial = Submit(acquire_[sem], rendered_[idx]);
        acquire_serial_[sem] = serial;
        frames_.push_back(serial);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered_[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &dev_.swapchain;
        pi.pImageIndices = &idx;
        auto swap = Time(kSwap);
        r = fn.vkQueuePresentKHR(dev_.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    }
    // the GPU at most kFramesQueued presents behind
    while (frames_.size() > kFramesQueued) {
        auto wait = Time(kSwap);
        WaitSerial(frames_.front());
        frames_.pop_front();
    }
    presents_++;
    if (presents_ == 1) port::Breadcrumb("first frame");
    presents_seen_.store(presents_);
    Poll();
    {
        std::lock_guard lock(late_lock_);
        for (auto& [data, l] : late_) PollLate(l);
    }
    Collect();
    ReleaseOldTextures();
    CountFps();
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<VkBackend>(); }
void SavePipelineCache() {}

}  // namespace gpu
