// The title's Direct3D 8 calls drawn with deko3d (SCDA_GPU=dk), the Switch's own thin GPU API (NVN's design), linked
// from devkitPro. It follows gpu_vk.cpp draw for draw: the same HLSL from gpu/shaders.cpp, the same constants, targets
// kept by surface memory, textures by guest address, the same render scale, SMAA and present pass. What differs is
// what Vulkan's driver did for it: command memory, fences, descriptor tables, memory blocks and the swap chain are
// the renderer's own here, and state is set as the hardware takes it, with no driver validation in between.
//
// Shaders: vk_shader.cpp's SPIR-V as GLSL for uam (dk_shader.cpp), compiled to a DKSH on the build threads; every
// DKSH is kept in shaders.dkb beside the log, so the next run loads it into code memory at once.
//
// Conventions: the device takes DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne, D3D's clip space and
// rows as they are (row 0 of every target the top), so the viewport is never upside down; with the origin at the top
// D3D's windings carry over as gpu_gl.cpp has them. Every write is ordered before every later access by one full
// barrier between passes, copies and uploads (pending_), as gpu_vk.cpp does.
#include <SDL.h>
#include <deko3d.h>
#include <dxgiformat.h>
#include <pthread.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <map>
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
#include "port/dk_shader.h"
#include "port/gpu_common.h"
#include "port/port.h"

namespace gpu {

namespace {

using namespace common;
using vk::Stage;

// the XDK's blend, compare and stencil values are the GL enums the console's GPU took
DkBlendFactor Blend(uint32_t v) {
    switch (v) {
        case 0: return DkBlendFactor_Zero;
        case 0x300: return DkBlendFactor_SrcColor;
        case 0x301: return DkBlendFactor_InvSrcColor;
        case 0x302: return DkBlendFactor_SrcAlpha;
        case 0x303: return DkBlendFactor_InvSrcAlpha;
        case 0x304: return DkBlendFactor_DstAlpha;
        case 0x305: return DkBlendFactor_InvDstAlpha;
        case 0x306: return DkBlendFactor_DstColor;
        case 0x307: return DkBlendFactor_InvDstColor;
        case 0x308: return DkBlendFactor_SrcAlphaSaturate;
        case 0x8001: return DkBlendFactor_ConstColor;
        case 0x8002: return DkBlendFactor_InvConstColor;
        case 0x8003: return DkBlendFactor_ConstAlpha;
        case 0x8004: return DkBlendFactor_InvConstAlpha;
        default: return DkBlendFactor_One;
    }
}

DkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return DkBlendOp_Sub;
        case 0x800B: return DkBlendOp_RevSub;
        case 0x8007: return DkBlendOp_Min;
        case 0x8008: return DkBlendOp_Max;
        default: return DkBlendOp_Add;
    }
}

// NEVER to ALWAYS as 0 to 7, deko3d's from 1
DkCompareOp Cmp(uint32_t v) { return DkCompareOp(CmpIndex(v) + 1); }

DkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return DkStencilOp_Zero;
        case 0x1E01: return DkStencilOp_Replace;
        case 0x1E02: return DkStencilOp_Incr;
        case 0x1E03: return DkStencilOp_Decr;
        case 0x150A: return DkStencilOp_Invert;
        case 0x8507: return DkStencilOp_IncrWrap;
        case 0x8508: return DkStencilOp_DecrWrap;
        default: return DkStencilOp_Keep;
    }
}

DkWrapMode Address(uint32_t v) {
    switch (v) {
        case 2: return DkWrapMode_MirroredRepeat;
        case 3: case 5: return DkWrapMode_ClampToEdge;
        case 4: return DkWrapMode_ClampToBorder;
        default: return DkWrapMode_Repeat;
    }
}

constexpr DkImageFormat kColourFormat = DkImageFormat_BGRA8_Unorm;  // guest memory's byte order: Sync copies it as it is
constexpr DkImageFormat kDepthFormat = DkImageFormat_Z24S8;

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---- memory and images

struct Mem {
    DkMemBlock block = nullptr;
    uint32_t pool = ~0u;  // the image pool's block it was carved from, ~0u: a block of its own
    uint32_t off = 0, size = 0;
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
    explicit operator bool() const { return block != nullptr; }
};

struct Image {
    DkImage img;
    bool valid = false;
    Mem mem;
    DkImageFormat format = kColourFormat;
    DkImageType type = DkImageType_2D;
    uint32_t w = 0, h = 0, d = 1, layers = 1, levels = 1;
    bool depth = false;
    bool render = false;  // a render target: the copy engine's layout is not the 3D engine's for it
    uint32_t desc = 0;  // its image descriptor's slot, 0 for none
};

struct Buffer {
    Mem mem;
    uint32_t size = 0;
};

struct Target {
    Image img;
    Image fb;  // a copy draws read while drawing to the target
    bool depth = false;
    uint32_t gw = 0, gh = 0;  // the console's size
    uint32_t w = 0, h = 0;    // the host image's
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0;
    uint64_t sig = 0;
    bool drawn = false;
    bool fixed = false;
    std::vector<uint8_t> read;
};

struct Tex {
    Image img;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    bool norm = false;
};

// a draw's four texture stages: the image descriptor slot each samples
struct DrawTextures {
    TexInfo info[4];
    uint32_t desc[4] = {};
    uint32_t levels[4] = {};
    bool rt[4] = {};
};

// all 32 bit fields, hashed whole
struct SamplerDesc {
    uint32_t min = DkFilter_Nearest, mag = DkFilter_Nearest, mip = DkMipFilter_Nearest;
    uint32_t wrap[3] = {DkWrapMode_Repeat, DkWrapMode_Repeat, DkWrapMode_Repeat};
    float aniso = 1, bias = 0, min_lod = 0, max_lod = 1000;
    float border[4] = {};
    uint32_t compare = 0;  // DkCompareOp for a shadow map, 0 without
};

SamplerDesc StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    SamplerDesc sd;
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    sd.mag = lin(mag) ? DkFilter_Linear : DkFilter_Nearest;
    sd.min = lin(min) ? DkFilter_Linear : DkFilter_Nearest;
    sd.mip = mip == kTexfLinear ? DkMipFilter_Linear : DkMipFilter_Nearest;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    sd.aniso = float(std::min<uint32_t>(an, 16));
    sd.wrap[0] = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.wrap[1] = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.wrap[2] = Address(t[kTssAddressW]);
    sd.bias = Bits(t[kTssMipLodBias]);
    sd.min_lod = float(t[kTssMaxMipLevel]);
    sd.max_lod = mip ? 1000.0f : sd.min_lod;
    Color(t[kTssBorderColor], sd.border);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, deko3d tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.min = sd.mag = lin(min) || lin(mag) ? DkFilter_Linear : DkFilter_Nearest;
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

// a DXGI vertex format as deko3d's attribute size and type; false where there is none
bool VertexFormat(uint32_t f, DkVtxAttribState& a) {
    a.isBgra = 0;
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT: a.size = DkVtxAttribSize_1x32; a.type = DkVtxAttribType_Float; return true;
        case DXGI_FORMAT_R32G32_FLOAT: a.size = DkVtxAttribSize_2x32; a.type = DkVtxAttribType_Float; return true;
        case DXGI_FORMAT_R32G32B32_FLOAT: a.size = DkVtxAttribSize_3x32; a.type = DkVtxAttribType_Float; return true;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: a.size = DkVtxAttribSize_4x32; a.type = DkVtxAttribType_Float; return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            a.size = DkVtxAttribSize_4x8;
            a.type = DkVtxAttribType_Unorm;
            a.isBgra = 1;
            return true;
        case DXGI_FORMAT_R16_SNORM: a.size = DkVtxAttribSize_1x16; a.type = DkVtxAttribType_Snorm; return true;
        case DXGI_FORMAT_R16G16_SNORM: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Snorm; return true;
        case DXGI_FORMAT_R16G16B16A16_SNORM: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Snorm; return true;
        case DXGI_FORMAT_R8_UNORM: a.size = DkVtxAttribSize_1x8; a.type = DkVtxAttribType_Unorm; return true;
        case DXGI_FORMAT_R8G8_UNORM: a.size = DkVtxAttribSize_2x8; a.type = DkVtxAttribType_Unorm; return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Unorm; return true;
        case DXGI_FORMAT_R16_SINT: a.size = DkVtxAttribSize_1x16; a.type = DkVtxAttribType_Sint; return true;
        case DXGI_FORMAT_R16G16_SINT: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Sint; return true;
        case DXGI_FORMAT_R16G16B16A16_SINT: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Sint; return true;
        case DXGI_FORMAT_R32_UINT: a.size = DkVtxAttribSize_1x32; a.type = DkVtxAttribType_Uint; return true;
        default: return false;
    }
}

DkPrimitive PrimitiveOf(const Assembled& a) {
    if (a.topology == Topology::Points) return DkPrimitive_Points;
    if (a.topology == Topology::Lines) return a.strip ? DkPrimitive_LineStrip : DkPrimitive_Lines;
    return a.strip ? DkPrimitive_TriangleStrip : DkPrimitive_Triangles;
}

// a shader by the hash of its HLSL: compiled, read from shaders.dkb, or being built
struct Built {
    enum State : uint8_t { kKept, kBuilding, kReady, kFailed };
    State state = kKept;
    bool from_binary = false;
    DkShader obj;
    bool valid = false;
    int8_t locations[16];
    dk::Pair pairs[dk::kMaxPairs];
    uint8_t pair_count = 0;
    Stage stage = Stage::Vertex;
    std::string entry;
    std::vector<uint8_t> binary;  // kKept: not loaded yet
};

// a draw key's shader as the render thread last resolved it
struct Resolved {
    uint64_t hash = 0;  // 0: the HLSL did not come out
    const DkShader* obj = nullptr;
    const int8_t* locations = nullptr;
    const dk::Pair* pairs = nullptr;
    uint8_t pair_count = 0;
};

constexpr uint32_t kSegments = 4;
constexpr size_t kRingSize = 32u << 20;
constexpr size_t kSegment = kRingSize / kSegments;
constexpr int kBuildThreads = 2;
constexpr uint32_t kSubmits = 8;
constexpr size_t kFramesQueued = 2;
constexpr uint32_t kCmdChunk = 1u << 20;
constexpr uint32_t kImageBlock = 32u << 20;
constexpr uint32_t kCodeBlock = 16u << 20;
constexpr uint32_t kImageDescs = 16384;
constexpr uint32_t kSamplerDescs = 4096;
constexpr uint32_t kSwapImages = 3;
constexpr char kBinaryMagic[8] = {'S', 'C', 'D', 'A', 'D', 'K', '1', 0};

class DkBackend final : public Backend {
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
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(game_ramp_.data(), ramp, game_ramp_.size());
        has_game_ramp_ = true;
        ramp_set_ = ~0u;
    }

private:
    struct Alloc {
        uint8_t* cpu = nullptr;
        DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
    };
    struct Submission {
        DkCmdBuf cmd = nullptr;
        DkFence fence{};
        uint64_t serial = 0;
        std::vector<DkMemBlock> chunks;  // command memory, reused in turn
        uint32_t used = 0;
    };
    struct Pool {
        DkMemBlock block = nullptr;
        std::map<uint32_t, uint32_t> free;  // offset to size
    };

    // device, memory, images
    bool CreateDevice();
    Mem Allocate(uint32_t size, uint32_t align, uint32_t flags, bool image);
    void Free(Mem& m);
    bool CreateImage(Image& img, DkImageFormat fmt, DkImageType type, uint32_t w, uint32_t h, uint32_t d,
                     uint32_t layers, uint32_t levels, bool render);
    void DestroyImage(Image& img);
    bool CreateBuffer(Buffer& b, uint32_t size, bool read);
    void DestroyBuffer(Buffer& b);
    DkImageView View(const Image& img) const;  // the whole image as shaders sample it
    DkImageView LevelView(const Image& img, uint32_t level) const;
    uint32_t NewImageDesc(const Image& img);
    static void AddCmdMem(void* user, DkCmdBuf cmd, size_t min);

    // command buffers, the ring, ordering
    DkCmdBuf Cmd() {
        recorded_ = true;
        return subs_[cur_].cmd;
    }
    void Begin();
    uint64_t Submit();
    void Poll();
    void WaitSerial(uint64_t serial);
    void Collect();
    void Retire(Image& img);
    void Retire(Buffer& buf);
    Alloc Upload(size_t size, size_t align);
    void Barrier();
    void Outside();
    void Inside(Target* rt, Target* ds);
    void BindOutput(const Image* colour, const Image* depth);
    void Descriptors();
    void CopyToImage(const Image& img, const Alloc& src, uint32_t level, uint32_t layer, uint32_t w, uint32_t h,
                     uint32_t d, uint32_t row_bytes, uint32_t rows);
    Buffer& Readback(uint32_t bytes);
    bool ReadTarget(Target& t, std::vector<uint8_t>& px);
    Image* Scratch(uint32_t w, uint32_t h);
    Image* Staging(uint32_t w, uint32_t h);
    void CopyOut(const Image& src, uint32_t w, uint32_t h, DkGpuAddr dst);
    void DumpImage(const Image& img, const char* name);
    std::vector<uint32_t> dump_textures_;
    uint32_t images_dbg_[4] = {};  // diagnostic: linear textures to dump at the next present
    size_t dumped_textures_ = 0;
    Image* GuestSized(Target& t);
    void Blit(const Image& src, uint32_t sw, uint32_t sh, const Image& dst, uint32_t dw, uint32_t dh);
    void ClearImage(Image& img, float z);
    void ApplyRenderScale();

    // shaders
    bool LoadShader(Built& b, const std::vector<uint8_t>& dksh);
    bool Build(uint64_t hash, const std::string& hlsl, Stage stage, const std::string& entry, Built& out);
    void Keep(uint64_t hash, const Built& b);
    void LoadKept();
    void StartBuilders();
    void Builder();
    const Resolved& Shader(uint64_t key, Stage stage, const std::string& (*gen)(void*), void* arg);
    void Request(Resolved& r, std::string hlsl, Stage stage, const char* entry);
    bool Lookup(Resolved& r);
    void CreateBlit();
    void Await(std::initializer_list<Resolved*> shaders);
    void BindTexturePairs(const Resolved& ps, const uint32_t images[4], const uint32_t samplers[4]);
    void PostState(const DkViewport& vp, const DkScissor& sc);
    void DrawPost(const Image& out, uint32_t w, uint32_t h, bool clear, const Resolved& vs, const Resolved& ps,
                  const float* params, size_t bytes, std::initializer_list<const Image*> images);
    const Image* Smaa(const Target& t);
    bool EnsureSmaa(uint32_t w, uint32_t h);
    uint32_t Sampler(const SamplerDesc& d);
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool BindTargets(const State& s);
    void UploadTarget(Target& t, const Surface& s);
    uint32_t FeedbackCopy(Target& t);

    // textures
    uint32_t BindTexture(const State& s, int stage, DrawTextures& stages);
    uint32_t BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    uint32_t BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p);
    void ReleaseOldTextures();

    // a draw
    void ForgetState();
    void ApplyState(const State& s, const Assembled& a);
    void SetViewportScissor(const State& s, uint32_t gw);
    void BindShaders(const DkShader* vs, const DkShader* ps);
    struct Vertices {
        DkBufExtents buf[16];
    };
    bool UploadVertices(const Assembled& a, Vertices& v);
    void SetVertexInputs(const Assembled& a, const int8_t* locations, const Vertices& v);
    void ClearTargets(Target* rt, Target* ds, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                      uint32_t color, float z, uint32_t stencil);
    void CountFps();
    bool Swapchain();
    void DestroySwapchain();

    DkDevice device_ = nullptr;
    DkQueue queue_ = nullptr;
    std::mutex alloc_lock_;
    std::vector<Pool> pools_;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    std::array<Submission, kSubmits> subs_;
    uint32_t cur_ = 0;
    uint64_t serial_ = 1;
    uint64_t done_ = 0;
    bool recorded_ = false;
    std::vector<std::pair<uint64_t, Image>> dead_images_;
    std::vector<std::pair<uint64_t, Buffer>> dead_buffers_;
    std::deque<uint64_t> frames_;

    // descriptor tables: image descriptors by slot (0 unused: no image), samplers by slot
    Mem image_descs_, sampler_descs_;
    std::vector<uint32_t> free_descs_;
    uint32_t next_desc_ = 1;
    uint32_t next_sampler_ = 0;
    bool descs_dirty_ = false;

    // shader code: one block, carved in turn; new code invalidates the GPU's shader cache before the next draw
    std::mutex code_lock_;
    std::vector<DkMemBlock> code_blocks_;
    uint32_t code_used_ = 0;
    std::atomic<bool> code_new_{false};

    Buffer ring_;
    size_t ring_off_ = 0;
    uint32_t segment_ = 0;
    uint64_t segment_serial_[kSegments] = {};
    Buffer readback_;
    float scale_ = 1.0f;
    std::unordered_map<uint64_t, Image> scratch_;
    std::unordered_map<uint64_t, Image> staging_;  // plain images between the copy engine and render targets

    // the bound targets, and what the next draw wants
    bool pending_ = false;  // copies, uploads or other targets written since the last barrier
    bool drawn_ = false;    // draws into the bound targets since the last barrier
    bool output_set_ = false;
    Target* cur_rt_ = nullptr;
    Target* cur_ds_ = nullptr;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;

    // state as last bound in this command buffer; known_ false: none of it
    bool known_ = false;
    struct Dyn {
        DkColorState colour;
        DkBlendState blend;
        float constants[4];
        DkColorWriteState mask;
        DkDepthStencilState ds;
        uint32_t sref, smask, swrite;
        DkRasterizerState rast;
        float bias_const, bias_slope;
        DkViewport vp;
        DkScissor sc;
    } dyn_{};
    const DkShader* bound_vs_ = nullptr;
    const DkShader* bound_ps_ = nullptr;
    std::array<DkVtxAttribState, 16> vin_attr_{};
    std::array<DkVtxBufferState, 16> vin_bind_{};
    uint32_t vin_attrs_ = 0, vin_binds_ = 0;
    bool vin_set_ = false;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    Image dummy_[4];

    std::unordered_map<uint64_t, Resolved> keys_;
    std::mutex build_lock_;
    std::condition_variable build_cv_;
    std::unordered_map<uint64_t, Built> built_;
    struct Job {
        uint64_t hash;
        std::string hlsl;
    };
    std::deque<Job> jobs_;
    FILE* kept_file_ = nullptr;
    size_t kept_ = 0, kept_failed_ = 0;
    Assembled asm_;

    Resolved blit_vs_, blit_ps_;
    Resolved smaa_vs_[3], smaa_ps_[3];
    Image smaa_[3], smaa_area_, smaa_search_;
    uint32_t smaa_w_ = 0, smaa_h_ = 0;
    bool smaa_built_ = false, smaa_failed_ = false;
    uint32_t blit_linear_ = 0, blit_point_ = 0;
    Image lut_;
    bool gamma_ = false;
    uint32_t ramp_set_ = ~0u;
    std::array<uint8_t, 768> game_ramp_{};
    bool has_game_ramp_ = false;

    // the swap chain
    DkSwapchain swapchain_ = nullptr;
    Image swap_[kSwapImages];
    uint32_t swap_w_ = 0, swap_h_ = 0;
    bool vsync_ = true;
    uint32_t asked_w_ = 0, asked_h_ = 0;

    static constexpr int kLateSlots = 3;
    static constexpr uint64_t kLateKeep = 1800;
    static constexpr uint32_t kLateKeepBytes = 256u << 10;
    struct Late {
        Buffer buf[kLateSlots];
        uint64_t serial[kLateSlots] = {};
        DkFence* fence[kLateSlots] = {};
        bool done[kLateSlots] = {};
        uint64_t seq[kLateSlots] = {};
        uint64_t frame[kLateSlots] = {};
        uint64_t delivered = 0;
        uint64_t locked = 0;
        uint32_t gw = 0, gh = 0;
        uint32_t next = 0;
    };
    void ResetLate(Late& l);
    void PollLate(Late& l);
    void CopyLate(Late& l, Target& t);
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    uint64_t late_seq_ = 0;
    std::atomic<uint64_t> presents_seen_{0};

    uint64_t presents_ = 0;
    uint32_t fps_n_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t draws_ = 0, last_draws_ = 0, last_presents_ = 0, skipped_ = 0;
    std::chrono::steady_clock::time_point log_t_ = std::chrono::steady_clock::now();
    uint64_t log_cpu_ = 0;

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

// ---- device, memory, images

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
    LOGE("dk: %s: %s (%d)", context ? context : "", message ? message : "", int(result));
    logx::Flush();
}

bool DkBackend::CreateDevice() {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
    dm.cbDebug = DebugCallback;
    logx::Flush();  // deko3d's release library ends the process on a failed creation
    device_ = dkDeviceCreate(&dm);
    if (!device_) return false;
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device_);
    qm.flags = DkQueueFlags_Graphics;
    queue_ = dkQueueCreate(&qm);
    return queue_ != nullptr;
}

// image memory carved from big blocks, first fit; anything else a block of its own
Mem DkBackend::Allocate(uint32_t size, uint32_t align, uint32_t flags, bool image) {
    Mem m;
    align = std::max<uint32_t>(align, 1);
    if (image && size <= kImageBlock) {
        std::lock_guard lock(alloc_lock_);
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t p = 0; p < pools_.size(); p++) {
                Pool& pool = pools_[p];
                for (auto it = pool.free.begin(); it != pool.free.end(); ++it) {
                    uint32_t at = AlignUp(it->first, align);
                    uint32_t end = it->first + it->second;
                    if (at + size > end) continue;
                    uint32_t start = it->first;
                    pool.free.erase(it);
                    if (at > start) pool.free[start] = at - start;
                    if (end > at + size) pool.free[at + size] = end - (at + size);
                    m.block = pool.block;
                    m.pool = p;
                    m.off = at;
                    m.size = size;
                    m.gpu = dkMemBlockGetGpuAddr(pool.block) + at;
                    return m;
                }
            }
            if (pass) break;
            DkMemBlockMaker mm;
            dkMemBlockMakerDefaults(&mm, device_, kImageBlock);
            mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
            logx::Flush();
            DkMemBlock b = dkMemBlockCreate(&mm);
            if (!b) return m;
            Pool pool;
            pool.block = b;
            pool.free[0] = kImageBlock;
            pools_.push_back(std::move(pool));
        }
        return m;
    }
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, device_, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = flags | (image ? DkMemBlockFlags_Image : 0u);
    logx::Flush();
    m.block = dkMemBlockCreate(&mm);
    if (!m.block) return m;
    m.size = size;
    m.gpu = dkMemBlockGetGpuAddr(m.block);
    if ((flags & DkMemBlockFlags_CpuAccessMask) != 0) m.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(m.block));
    return m;
}

void DkBackend::Free(Mem& m) {
    if (!m.block) return;
    if (m.pool == ~0u) {
        dkMemBlockDestroy(m.block);
    } else {
        std::lock_guard lock(alloc_lock_);
        auto& free = pools_[m.pool].free;
        uint32_t off = m.off, size = m.size;
        auto next = free.lower_bound(off);
        if (next != free.end() && off + size == next->first) {
            size += next->second;
            next = free.erase(next);
        }
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == off) {
                prev->second += size;
                m = Mem{};
                return;
            }
        }
        free[off] = size;
    }
    m = Mem{};
}

DkImageView DkBackend::View(const Image& img) const {
    DkImageView v;
    dkImageViewDefaults(&v, &img.img);
    v.type = img.type;
    if (img.depth) v.dsSource = DkDsSource_Depth;
    return v;
}

DkImageView DkBackend::LevelView(const Image& img, uint32_t level) const {
    DkImageView v = View(img);
    v.mipLevelOffset = uint8_t(level);
    v.mipLevelCount = 1;
    return v;
}

uint32_t DkBackend::NewImageDesc(const Image& img) {
    uint32_t slot;
    if (!free_descs_.empty()) {
        slot = free_descs_.back();
        free_descs_.pop_back();
    } else if (next_desc_ < kImageDescs) {
        slot = next_desc_++;
    } else {
        LOG_FIRST(4, "dk: the image descriptor table is full");
        return 0;
    }
    DkImageView v = View(img);
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &v, false, false);
    std::memcpy(image_descs_.cpu + size_t(slot) * sizeof(d), &d, sizeof(d));
    descs_dirty_ = true;
    return slot;
}

// render: a target too; every image can take copies and blits
bool DkBackend::CreateImage(Image& img, DkImageFormat fmt, DkImageType type, uint32_t w, uint32_t h, uint32_t d,
                            uint32_t layers, uint32_t levels, bool render) {
    img = Image{};
    img.format = fmt;
    img.type = type;
    img.w = w;
    img.h = h;
    img.d = d;
    img.layers = layers;
    img.levels = levels;
    img.depth = fmt == DkImageFormat_Z24S8 || fmt == DkImageFormat_ZF32;
    img.render = render;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.type = type;
    lm.format = fmt;
    // no hardware compression: uploads, copies and blits go through the 2D and copy engines, which write and read
    // the plain layout (a compressed target written by them stayed black: the loading screen, the menu's video)
    lm.flags = DkImageFlags_Usage2DEngine | (render ? DkImageFlags_UsageRender : 0u);
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    lm.dimensions[2] = type == DkImageType_3D ? d : type == DkImageType_Cubemap ? 6 : 1;
    lm.mipLevels = levels;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    img.mem = Allocate(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout), 0, true);
    if (!img.mem) {
        LOGE("dk: no memory for a %ux%u image", w, h);
        return false;
    }
    dkImageInitialize(&img.img, &layout, img.mem.block, img.mem.off);
    img.valid = true;
    img.desc = NewImageDesc(img);
    return true;
}

void DkBackend::DestroyImage(Image& img) {
    if (img.desc) free_descs_.push_back(img.desc);
    Free(img.mem);
    img = Image{};
}

// read: CPU cached and GPU uncached, for read-backs; else CPU uncached, written once and read by the GPU
bool DkBackend::CreateBuffer(Buffer& b, uint32_t size, bool read) {
    b.mem = Allocate(size, DK_MEMBLOCK_ALIGNMENT,
                     DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                     false);
    b.size = b.mem ? size : 0;
    return bool(b.mem);
}

void DkBackend::DestroyBuffer(Buffer& b) {
    Free(b.mem);
    b = Buffer{};
}

// command memory runs out: the submission's next chunk, a new one when it has none
void DkBackend::AddCmdMem(void* user, DkCmdBuf cmd, size_t min) {
    auto* self = static_cast<DkBackend*>(user);
    Submission& sub = self->subs_[self->cur_];
    if (sub.used >= sub.chunks.size() || min > kCmdChunk) {
        DkMemBlockMaker mm;
        dkMemBlockMakerDefaults(&mm, self->device_, AlignUp(uint32_t(std::max<size_t>(min, kCmdChunk)), DK_MEMBLOCK_ALIGNMENT));
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        logx::Flush();
        DkMemBlock b = dkMemBlockCreate(&mm);
        sub.chunks.insert(sub.chunks.begin() + sub.used, b);
    }
    DkMemBlock b = sub.chunks[sub.used++];
    dkCmdBufAddMemory(cmd, b, 0, dkMemBlockGetSize(b));
}

bool DkBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    scale_ = app::RenderScale(guest_h_);
    LOGI("dk: render scale %.3f", scale_);
    if (!CreateDevice()) {
        LOGE("dk: no device or queue");
        return false;
    }
    for (Submission& sub : subs_) {
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, device_);
        cm.userData = this;
        cm.cbAddMem = AddCmdMem;
        sub.cmd = dkCmdBufCreate(&cm);
        if (!sub.cmd) {
            LOGE("dk: no command buffers");
            return false;
        }
    }
    image_descs_ = Allocate(kImageDescs * sizeof(DkImageDescriptor), DK_MEMBLOCK_ALIGNMENT,
                            DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, false);
    sampler_descs_ = Allocate(kSamplerDescs * sizeof(DkSamplerDescriptor), DK_MEMBLOCK_ALIGNMENT,
                              DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, false);
    if (!image_descs_ || !sampler_descs_ || !CreateBuffer(ring_, kRingSize, false)) {
        LOGE("dk: no descriptor tables or upload ring");
        return false;
    }
    std::memset(image_descs_.cpu, 0, kImageDescs * sizeof(DkImageDescriptor));
    if (!Swapchain()) return false;
    Begin();

    bool dummies = CreateImage(dummy_[kTexNone], kColourFormat, DkImageType_2D, 1, 1, 1, 1, 1, true) &&
                   CreateImage(dummy_[kTex2D], kColourFormat, DkImageType_2D, 1, 1, 1, 1, 1, true) &&
                   CreateImage(dummy_[kTex3D], kColourFormat, DkImageType_3D, 1, 1, 2, 1, 1, false) &&
                   CreateImage(dummy_[kTexCube], kColourFormat, DkImageType_Cubemap, 1, 1, 1, 6, 1, false) &&
                   CreateImage(lut_, kColourFormat, DkImageType_2D, 256, 1, 1, 1, 1, false);
    if (!dummies) return false;
    // what a stage without a texture samples: zeros, as a null descriptor gives them on D3D12
    for (Image& img : dummy_) {
        uint32_t bytes = img.w * img.h * img.d * 4 * (img.type == DkImageType_Cubemap ? 6 : 1);
        Alloc a = Upload(bytes, 16);
        std::memset(a.cpu, 0, bytes);
        Outside();
        for (uint32_t f = 0; f < (img.type == DkImageType_Cubemap ? 6u : 1u); f++) {
            Alloc face{a.cpu + f * img.w * img.h * 4, a.gpu + f * img.w * img.h * 4};
            CopyToImage(img, face, 0, f, img.w, img.h, img.d, img.w * 4, img.h);
        }
    }

    LoadKept();
    StartBuilders();
    CreateBlit();
    if (!blit_vs_.obj || !blit_ps_.obj) return false;
    LOGI("dk: renderer up, the title draws at %ux%u", width, height);
    port::Breadcrumb("dk renderer up");
    return true;
}

// ---- command buffers, the ring, ordering

void DkBackend::Begin() {
    Submission& sub = subs_[cur_];
    if (sub.serial > done_) {
        auto timed = Time(kRingWait);
        dkFenceWait(&sub.fence, -1);
        done_ = sub.serial;
    }
    dkCmdBufClear(sub.cmd);
    sub.used = 0;
    AddCmdMem(this, sub.cmd, kCmdChunk);
    DkCmdBuf c = sub.cmd;
    // the tables for every draw; what the CPU rewrote since (the ring, descriptors, code) is read afresh
    dkCmdBufBindImageDescriptorSet(c, image_descs_.gpu, kImageDescs);
    dkCmdBufBindSamplerDescriptorSet(c, sampler_descs_.gpu, kSamplerDescs);
    dkCmdBufBarrier(c, DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                        DkInvalidateFlags_L2Cache);
    descs_dirty_ = false;
    code_new_ = false;
    recorded_ = false;
    known_ = false;
    vin_set_ = false;
    output_set_ = false;
    cur_rt_ = cur_ds_ = nullptr;
    bound_vs_ = bound_ps_ = nullptr;
}

uint64_t DkBackend::Submit() {
    Submission& sub = subs_[cur_];
    dkCmdBufSignalFence(sub.cmd, &sub.fence, true);
    DkCmdList list = dkCmdBufFinishList(sub.cmd);
    if (dkQueueIsInErrorState(queue_)) {
        LOGE("dk: the GPU queue is in an error state");
        logx::Flush();
    }
    dkQueueSubmitCommands(queue_, list);
    dkQueueFlush(queue_);
    uint64_t serial = serial_++;
    sub.serial = serial;
    cur_ = (cur_ + 1) % kSubmits;
    pending_ = true;
    Begin();
    return serial;
}

void DkBackend::Poll() {
    for (Submission& sub : subs_) {
        if (sub.serial > done_ && dkFenceWait(&sub.fence, 0) == DkResult_Success) done_ = std::max(done_, sub.serial);
    }
}

void DkBackend::WaitSerial(uint64_t serial) {
    if (serial >= serial_) Submit();
    while (done_ < serial) {
        Submission* oldest = nullptr;
        for (Submission& sub : subs_) {
            if (sub.serial > done_ && (!oldest || sub.serial < oldest->serial)) oldest = &sub;
        }
        if (!oldest) break;
        dkFenceWait(&oldest->fence, -1);
        done_ = oldest->serial;
    }
}

void DkBackend::Collect() {
    for (auto it = dead_images_.begin(); it != dead_images_.end();) {
        if (it->first <= done_) {
            DestroyImage(it->second);
            it = dead_images_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = dead_buffers_.begin(); it != dead_buffers_.end();) {
        if (it->first <= done_) {
            DestroyBuffer(it->second);
            it = dead_buffers_.erase(it);
        } else {
            ++it;
        }
    }
}

void DkBackend::Retire(Image& img) {
    if (img.valid) dead_images_.emplace_back(serial_, img);
    img = Image{};
}

void DkBackend::Retire(Buffer& buf) {
    if (buf.mem) dead_buffers_.emplace_back(serial_, buf);
    buf = Buffer{};
}

DkBackend::Alloc DkBackend::Upload(size_t size, size_t align) {
    if (size > kSegment) {
        Buffer b;
        if (!CreateBuffer(b, uint32_t(size), false)) return {};
        Alloc a{b.mem.cpu, b.mem.gpu};
        Retire(b);
        return a;
    }
    size_t off = (ring_off_ + align - 1) & ~(align - 1);
    size_t end = (segment_ + 1) * kSegment;
    if (off + size > end) {
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
    return {ring_.mem.cpu + off, ring_.mem.gpu + off};
}

// every write so far before every access after: passes, copies and uploads are few enough a frame
void DkBackend::Barrier() {
    dkCmdBufBarrier(Cmd(), DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Zcull);
    pending_ = false;
    drawn_ = false;
}

// before a copy, an upload or other targets: the writes before it done
void DkBackend::Outside() {
    if (pending_ || drawn_) Barrier();
}

// new descriptors or shader code made since the last draw: the GPU's caches of them forget
void DkBackend::Descriptors() {
    uint32_t flags = (descs_dirty_ ? DkInvalidateFlags_Descriptors : 0u) |
                     (code_new_.exchange(false) ? DkInvalidateFlags_Shader : 0u);
    if (flags) dkCmdBufBarrier(Cmd(), DkBarrier_None, flags);
    descs_dirty_ = false;
}

void DkBackend::BindOutput(const Image* colour, const Image* depth) {
    DkImageView cv, dv;
    if (colour) {
        cv = LevelView(*colour, 0);
        cv.type = DkImageType_2D;
    }
    if (depth) {
        dv = LevelView(*depth, 0);
        dv.type = DkImageType_2D;
    }
    const DkImageView* cs[1] = {&cv};
    dkCmdBufBindRenderTargets(Cmd(), colour ? cs : nullptr, colour ? 1 : 0, depth ? &dv : nullptr);
    output_set_ = true;
}

// drawing into these targets; the writes before (copies, uploads, other targets) ordered first. Draws into the same
// targets one after the other need no barrier: the GPU keeps their order
void DkBackend::Inside(Target* rt, Target* ds) {
    bool same = output_set_ && cur_rt_ == rt && cur_ds_ == ds;
    if (same && !pending_) return;
    if (pending_ || (!same && drawn_)) Barrier();
    if (!same) {
        BindOutput(rt ? &rt->img : nullptr, ds ? &ds->img : nullptr);
        cur_rt_ = rt;
        cur_ds_ = ds;
    }
}

// the caller is Outside(); rows: of pixels, of blocks for BC
void DkBackend::CopyToImage(const Image& img, const Alloc& src, uint32_t level, uint32_t layer, uint32_t w,
                            uint32_t h, uint32_t d, uint32_t row_bytes, uint32_t rows) {
    if (!src.cpu) return;
    // deko3d's buffer layout: the row's bytes and the slice's bytes (not its rows)
    DkCopyBuf b = {src.gpu, row_bytes, row_bytes * rows};
    if (img.render) {
        // into a plain image with the copy engine, then into the target with the 2D engine
        Image* st = Staging(w, h);
        if (!st) return;
        DkImageView sv = LevelView(*st, 0), dv = LevelView(img, 0);
        DkImageRect r = {0, 0, 0, w, h, 1};
        dkCmdBufCopyBufferToImage(Cmd(), &b, &sv, &r, 0);
        dkCmdBufBarrier(Cmd(), DkBarrier_Full, DkInvalidateFlags_Image);
        dkCmdBufBlitImage(Cmd(), &sv, &r, &dv, &r, 0, 0);
        pending_ = true;
        return;
    }
    DkImageView v = LevelView(img, level);
    DkImageRect r = {0, 0, img.type == DkImageType_3D ? 0 : layer, w, h, img.type == DkImageType_3D ? d : 1};
    dkCmdBufCopyBufferToImage(Cmd(), &b, &v, &r, 0);
    pending_ = true;
}

Buffer& DkBackend::Readback(uint32_t bytes) {
    if (readback_.size < bytes) {
        Retire(readback_);
        CreateBuffer(readback_, std::max<uint32_t>(bytes, 4u << 20), true);
    }
    return readback_;
}

bool DkBackend::ReadTarget(Target& t, std::vector<uint8_t>& px) {
    uint32_t bytes = t.gw * t.gh * 4;
    Buffer& rb = Readback(bytes);
    if (!rb.mem.cpu) return false;
    Image* src = GuestSized(t);
    if (!src) return false;
    CopyOut(*src, t.gw, t.gh, rb.mem.gpu);
    WaitSerial(Submit());
    (void)0;  // uncached: nothing to flush
    px.assign(rb.mem.cpu, rb.mem.cpu + bytes);
    return true;
}

// diagnostic: a BGRA image's level 0 as shots/<name>.ppm beside the log (waits for the GPU)
void DkBackend::DumpImage(const Image& img, const char* name) {
    if (img.format != kColourFormat || img.type != DkImageType_2D) return;
    uint32_t bytes = img.w * img.h * 4;
    Buffer& rb = Readback(bytes);
    if (!rb.mem.cpu) return;
    CopyOut(img, img.w, img.h, rb.mem.gpu);
    WaitSerial(Submit());
    (void)0;  // uncached: nothing to flush
    std::vector<uint8_t> px(rb.mem.cpu, rb.mem.cpu + bytes);  // one copy out of uncached memory
    std::filesystem::path dir = port::HomeDir() / "shots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FILE* f = std::fopen((dir / (std::string(name) + ".ppm")).string().c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", img.w, img.h);
        std::vector<uint8_t> rgb(size_t(img.w) * img.h * 3);
        uint64_t sum = 0;
        for (uint32_t i = 0, o = 0; i < bytes; i += 4, o += 3) {
            rgb[o] = px[i + 2];
            rgb[o + 1] = px[i + 1];
            rgb[o + 2] = px[i];
            sum += rgb[o] + rgb[o + 1] + rgb[o + 2];
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
        LOGI("dk: dumped %s (%ux%u), mean %.1f", name, img.w, img.h, double(sum) / (3.0 * img.w * img.h));
    }
}

// a plain (not render) colour image of this size: the copy engine writes and reads it in the layout the 2D engine
// and the samplers use; render targets go through it
Image* DkBackend::Staging(uint32_t w, uint32_t h) {
    Image& img = staging_[uint64_t(w) << 32 | h];
    if (!img.valid && !CreateImage(img, kColourFormat, DkImageType_2D, w, h, 1, 1, 1, false)) return nullptr;
    return &img;
}

// w x h pixels of a colour image's level 0 into a buffer, rows of w * 4 bytes; a render target by way of a plain image
void DkBackend::CopyOut(const Image& src, uint32_t w, uint32_t h, DkGpuAddr dst) {
    const Image* from = &src;
    if (src.render) {
        Image* st = Staging(w, h);
        if (!st) return;
        Outside();
        DkImageView a = LevelView(src, 0), b = LevelView(*st, 0);
        DkImageRect r = {0, 0, 0, w, h, 1};
        dkCmdBufBlitImage(Cmd(), &a, &r, &b, &r, 0, 0);
        pending_ = true;
        from = st;
    }
    Outside();
    DkImageView v = LevelView(*from, 0);
    DkImageRect r = {0, 0, 0, w, h, 1};
    DkCopyBuf b = {dst, w * 4, w * 4 * h};
    dkCmdBufCopyImageToBuffer(Cmd(), &v, &r, &b, 0);
    dkCmdBufBarrier(Cmd(), DkBarrier_Full, DkInvalidateFlags_L2Cache);
    pending_ = true;
}

Image* DkBackend::Scratch(uint32_t w, uint32_t h) {
    Image& img = scratch_[uint64_t(w) << 32 | h];
    if (!img.valid && !CreateImage(img, kColourFormat, DkImageType_2D, w, h, 1, 1, 1, true)) return nullptr;
    return &img;
}

Image* DkBackend::GuestSized(Target& t) {
    if (t.w == t.gw && t.h == t.gh) return &t.img;
    Image* img = Scratch(t.gw, t.gh);
    if (img) Blit(t.img, t.w, t.h, *img, t.gw, t.gh);
    return img;
}

void DkBackend::Blit(const Image& src, uint32_t sw, uint32_t sh, const Image& dst, uint32_t dw, uint32_t dh) {
    Outside();
    DkImageView s = LevelView(src, 0), d = LevelView(dst, 0);
    DkImageRect a = {0, 0, 0, sw, sh, 1}, b = {0, 0, 0, dw, dh, 1};
    dkCmdBufBlitImage(Cmd(), &s, &a, &d, &b, DkBlitFlag_FilterLinear, 0);
    pending_ = true;
}

// a whole colour image to zero, a depth image to z and stencil 0
void DkBackend::ClearImage(Image& img, float z) {
    Outside();
    BindOutput(img.depth ? nullptr : &img, img.depth ? &img : nullptr);
    cur_rt_ = cur_ds_ = nullptr;
    output_set_ = false;
    DkScissor sc = {0, 0, img.w, img.h};
    dkCmdBufSetScissors(Cmd(), 0, &sc, 1);
    dyn_.sc.width = ~0u;
    if (img.depth) {
        dkCmdBufClearDepthStencil(Cmd(), true, z, 0xFF, 0);
    } else {
        dkCmdBufClearColorFloat(Cmd(), 0, DkColorMask_RGBA, 0, 0, 0, 0);
    }
    pending_ = true;
}

void DkBackend::ApplyRenderScale() {
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) <= 1e-4f) return;
    scale_ = want;
    Outside();
    cur_rt_ = cur_ds_ = nullptr;
    output_set_ = false;
    bound_rt_ = bound_ds_ = nullptr;
    for (auto& [key, t] : targets_) {
        if (t.fixed) continue;
        uint32_t w = std::max(1u, uint32_t(std::lround(float(t.gw) * scale_)));
        uint32_t h = std::max(1u, uint32_t(std::lround(float(t.gh) * scale_)));
        if (w == t.w && h == t.h) continue;
        Image img;
        if (!CreateImage(img, t.depth ? kDepthFormat : kColourFormat, DkImageType_2D, w, h, 1, 1, 1, true)) continue;
        if (t.depth) {
            ClearImage(img, 1.0f);
        } else {
            Blit(t.img, t.w, t.h, img, w, h);
        }
        Retire(t.img);
        Retire(t.fb);
        t.img = img;
        t.w = w;
        t.h = h;
    }
    LOGI("dk: render scale %.3f, %ux%u for the %ux%u screen", scale_, uint32_t(std::lround(guest_w_ * scale_)),
         uint32_t(std::lround(guest_h_ * scale_)), guest_w_, guest_h_);
}

// ---- shaders

// a DKSH's code into code memory: the control section stays where it is, deko3d keeps what it needs of it
bool DkBackend::LoadShader(Built& b, const std::vector<uint8_t>& dksh) {
    struct Header {
        uint32_t magic, header_sz, control_sz, code_sz, programs_off, programs;
    } h;
    if (dksh.size() < sizeof(h)) return false;
    std::memcpy(&h, dksh.data(), sizeof(h));
    if (h.magic != 0x48534B44u || h.programs != 1 || uint64_t(h.control_sz) + h.code_sz > dksh.size()) return false;
    std::lock_guard lock(code_lock_);
    uint32_t at = AlignUp(code_used_, DK_SHADER_CODE_ALIGNMENT);
    if (code_blocks_.empty() || uint64_t(at) + h.code_sz > kCodeBlock - DK_SHADER_CODE_UNUSABLE_SIZE) {
        DkMemBlockMaker mm;
        dkMemBlockMakerDefaults(&mm, device_, kCodeBlock);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        logx::Flush();
        DkMemBlock block = dkMemBlockCreate(&mm);
        if (!block) return false;
        code_blocks_.push_back(block);
        at = 0;
    }
    DkMemBlock block = code_blocks_.back();
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block)) + at, dksh.data() + h.control_sz, h.code_sz);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, block, at);
    m.control = dksh.data();
    m.programId = 0;
    dkShaderInitialize(&b.obj, &m);
    if (!dkShaderIsValid(&b.obj)) return false;
    code_used_ = at + h.code_sz;
    code_new_ = true;
    b.valid = true;
    return true;
}

bool DkBackend::Build(uint64_t hash, const std::string& hlsl, Stage stage, const std::string& entry, Built& out) {
    auto t0 = std::chrono::steady_clock::now();
    compiles_++;
    dk::Dksh d;
    std::memset(out.locations, -1, sizeof(out.locations));
    out.stage = stage;
    out.entry = entry;
    if (!dk::CompileHlsl(hlsl, entry.c_str(), stage, d)) return false;
    std::memcpy(out.locations, d.locations, sizeof(out.locations));
    std::memcpy(out.pairs, d.pairs, sizeof(out.pairs));
    out.pair_count = d.pair_count;
    if (!LoadShader(out, d.bytes)) {
        LOGE("dk: deko3d did not take a %s shader", stage == Stage::Vertex ? "vertex" : "pixel");
        return false;
    }
    out.binary = std::move(d.bytes);
    Keep(hash, out);
    out.binary.clear();
    out.binary.shrink_to_fit();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 50.0) LOGI("dk: a %s shader built in %.0f ms", stage == Stage::Vertex ? "vertex" : "pixel", ms);
    return true;
}

// shaders.dkb: the magic and the revision, then records: hash, stage, the 16 input locations, the pairs, the entry's
// length and name, the DKSH's length and the DKSH
void DkBackend::Keep(uint64_t hash, const Built& b) {
    std::lock_guard lock(build_lock_);
    if (!kept_file_) return;
    uint8_t stage = uint8_t(b.stage);
    uint8_t entry = uint8_t(b.entry.size());
    uint32_t n = uint32_t(b.binary.size());
    std::fwrite(&hash, 8, 1, kept_file_);
    std::fwrite(&stage, 1, 1, kept_file_);
    std::fwrite(b.locations, 1, 16, kept_file_);
    std::fwrite(&b.pair_count, 1, 1, kept_file_);
    std::fwrite(b.pairs, sizeof(dk::Pair), dk::kMaxPairs, kept_file_);
    std::fwrite(&entry, 1, 1, kept_file_);
    std::fwrite(b.entry.data(), 1, entry, kept_file_);
    std::fwrite(&n, 4, 1, kept_file_);
    std::fwrite(b.binary.data(), 1, n, kept_file_);
    std::fflush(kept_file_);
}

void DkBackend::LoadKept() {
    std::string path = (port::HomeDir() / "shaders.dkb").string();
    bool fresh = true;
    uint32_t revision = dk::kRevision;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char magic[8];
        uint32_t rev = 0;
        bool same = std::fread(magic, 1, 8, f) == 8 && !std::memcmp(magic, kBinaryMagic, 8) &&
                    std::fread(&rev, 4, 1, f) == 1 && rev == revision;
        if (!same) LOGI("dk: %s is from another revision, shaders compile afresh", path.c_str());
        fresh = !same;
        for (; same;) {
            uint64_t hash;
            uint8_t stage, entry;
            uint32_t n;
            Built b;
            if (std::fread(&hash, 8, 1, f) != 1 || std::fread(&stage, 1, 1, f) != 1 ||
                std::fread(b.locations, 1, 16, f) != 16 || std::fread(&b.pair_count, 1, 1, f) != 1 ||
                std::fread(b.pairs, sizeof(dk::Pair), dk::kMaxPairs, f) != dk::kMaxPairs ||
                std::fread(&entry, 1, 1, f) != 1) break;
            b.entry.resize(entry);
            if (std::fread(b.entry.data(), 1, entry, f) != entry || std::fread(&n, 4, 1, f) != 1 || n > (4u << 20)) break;
            b.binary.resize(n);
            if (std::fread(b.binary.data(), 1, n, f) != n) break;
            b.stage = Stage(stage);
            built_[hash] = std::move(b);
        }
        std::fclose(f);
    }
    kept_file_ = std::fopen(path.c_str(), fresh ? "wb" : "ab");
    if (!kept_file_) {
        LOGW("dk: %s could not be written, shaders are not kept", path.c_str());
        return;
    }
    if (fresh) {
        std::fwrite(kBinaryMagic, 1, 8, kept_file_);
        std::fwrite(&revision, 4, 1, kept_file_);
        std::fflush(kept_file_);
    }
    for (auto& [hash, b] : built_) jobs_.push_back({hash, {}});
    kept_ = built_.size();
    LOGI("dk: %zu shaders kept in %s", kept_, path.c_str());
}

void DkBackend::StartBuilders() {
    for (int i = 0; i < kBuildThreads; i++) {
        // glslang, SPIRV-Cross and uam nest deep
        constexpr size_t kStack = 8u << 20;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kStack);
        pthread_t t;
        pthread_create(&t, &attr, [](void* self) -> void* {
            port::ThreadStarted();
            port::NameThread("dk builder");
            static_cast<DkBackend*>(self)->Builder();
            return nullptr;
        }, this);
        pthread_detach(t);
        pthread_attr_destroy(&attr);
    }
}

void DkBackend::Builder() {
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
                if (it->second.state != Built::kKept || it->second.binary.empty()) continue;
                it->second.state = Built::kBuilding;
                it->second.from_binary = true;
                b.binary = std::move(it->second.binary);
            }
            b.stage = it->second.stage;
            b.entry = it->second.entry;
            std::memcpy(b.locations, it->second.locations, sizeof(b.locations));
            std::memcpy(b.pairs, it->second.pairs, sizeof(b.pairs));
            b.pair_count = it->second.pair_count;
        }
        bool ok;
        if (job.hlsl.empty()) {
            ok = LoadShader(b, b.binary);
            if (!ok) kept_failed_++;
        } else {
            ok = Build(job.hash, job.hlsl, b.stage, b.entry, b);
        }
        std::lock_guard lock(build_lock_);
        Built& to = built_[job.hash];
        to.obj = b.obj;
        to.valid = b.valid;
        std::memcpy(to.locations, b.locations, sizeof(to.locations));
        std::memcpy(to.pairs, b.pairs, sizeof(to.pairs));
        to.pair_count = b.pair_count;
        to.binary.clear();
        to.binary.shrink_to_fit();
        to.state = ok ? Built::kReady : job.hlsl.empty() ? Built::kKept : Built::kFailed;
        to.from_binary = false;
        build_cv_.notify_all();
        if (jobs_.empty() && kept_) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            LOGI("dk: the %zu kept shaders loaded in %.0f ms%s", kept_, ms, kept_failed_ ? ", some refused" : "");
            kept_ = 0;
        }
    }
}

const Resolved& DkBackend::Shader(uint64_t key, Stage stage, const std::string& (*gen)(void*), void* arg) {
    auto it = keys_.find(key);
    if (it == keys_.end()) {
        it = keys_.emplace(key, Resolved{}).first;
        std::string hlsl = gen(arg);
        if (!hlsl.empty()) Request(it->second, std::move(hlsl), stage, "main");
    } else if (!it->second.obj && it->second.hash && !Lookup(it->second)) {
        keys_.erase(it);
        static const Resolved none;
        return none;
    }
    return it->second;
}

// a kept DKSH is loaded here at once; a compile runs on the build threads, whose stacks are deep enough for uam,
// and without async_shaders the draw waits for it
void DkBackend::Request(Resolved& r, std::string hlsl, Stage stage, const char* entry) {
    uint64_t hash = Hash(hlsl.data(), hlsl.size(), uint64_t(stage) + 1);
    r.hash = Hash(entry, std::strlen(entry), hash) | 1;
    std::unique_lock lock(build_lock_);
    Built& b = built_[r.hash];
    if (b.state == Built::kKept && !b.binary.empty()) {
        b.state = Built::kBuilding;
        b.from_binary = true;
        std::vector<uint8_t> binary = std::move(b.binary);
        Built made;
        lock.unlock();
        bool ok;
        {
            auto timed = Time(kBuildWait);
            ok = LoadShader(made, binary);
        }
        lock.lock();
        b.obj = made.obj;
        b.valid = made.valid;
        b.from_binary = false;
        b.state = ok ? Built::kReady : Built::kKept;
        build_cv_.notify_all();
        if (!ok) kept_failed_++;
    }
    if (b.state == Built::kKept) {
        b.state = Built::kBuilding;
        b.from_binary = false;
        b.stage = stage;
        b.entry = entry;
        jobs_.push_front({r.hash, std::move(hlsl)});
        build_cv_.notify_one();
        if (!app::Get().async_shaders) {
            auto timed = Time(kBuildWait);
            build_cv_.wait(lock, [&] { return b.state != Built::kBuilding; });
        }
    }
    lock.unlock();
    Lookup(r);
}

bool DkBackend::Lookup(Resolved& r) {
    std::unique_lock lock(build_lock_);
    auto it = built_.find(r.hash);
    if (it == built_.end()) return true;
    Built& b = it->second;
    if (b.state == Built::kBuilding && b.from_binary) {
        auto timed = Time(kBuildWait);
        build_cv_.wait(lock, [&] { return b.state != Built::kBuilding || !b.from_binary; });
    }
    if (b.state == Built::kReady && b.valid) {
        r.obj = &b.obj;
        r.locations = b.locations;
        r.pairs = b.pairs;
        r.pair_count = b.pair_count;
    } else if (b.state == Built::kFailed) {
        r.hash = 0;
    }
    return !(b.state == Built::kKept && b.binary.empty());
}

void DkBackend::CreateBlit() {
    Request(blit_vs_, kBlitHlsl, Stage::Vertex, "vs");
    Request(blit_ps_, kBlitHlsl, Stage::Pixel, "psmain");
    Await({&blit_vs_, &blit_ps_});
    if (!blit_vs_.obj || !blit_ps_.obj) LOGE("dk: the present pass did not compile");
    SamplerDesc lin;
    lin.min = lin.mag = DkFilter_Linear;
    lin.wrap[0] = lin.wrap[1] = lin.wrap[2] = DkWrapMode_ClampToEdge;
    lin.max_lod = 0;
    blit_linear_ = Sampler(lin);
    SamplerDesc pt = lin;
    pt.min = pt.mag = DkFilter_Nearest;
    blit_point_ = Sampler(pt);
}

void DkBackend::Await(std::initializer_list<Resolved*> shaders) {
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

// the pixel shader's texture and sampler pairs as deko3d's handles, at its sampler bindings
void DkBackend::BindTexturePairs(const Resolved& ps, const uint32_t images[4], const uint32_t samplers[4]) {
    if (!ps.pair_count) return;
    DkResHandle h[dk::kMaxPairs];
    for (uint32_t k = 0; k < ps.pair_count; k++) {
        const dk::Pair& p = ps.pairs[k];
        h[k] = dkMakeTextureHandle(images[p.texture & 3], samplers[p.sampler & 3]);
    }
    dkCmdBufBindTextures(Cmd(), DkStage_Fragment, 0, h, ps.pair_count);
}

void DkBackend::PostState(const DkViewport& vp, const DkScissor& sc) {
    DkCmdBuf c = Cmd();
    dkCmdBufSetViewports(c, 0, &vp, 1);
    dkCmdBufSetScissors(c, 0, &sc, 1);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = 0;
    ds.depthWriteEnable = 0;
    dkCmdBufBindRasterizerState(c, &rs);
    dkCmdBufBindColorState(c, &cs);
    dkCmdBufBindColorWriteState(c, &cw);
    dkCmdBufBindDepthStencilState(c, &ds);
    dkCmdBufBindVtxAttribState(c, nullptr, 0);
    dkCmdBufBindVtxBufferState(c, nullptr, 0);
    known_ = false;
    vin_set_ = false;
}

void DkBackend::DrawPost(const Image& out, uint32_t w, uint32_t h, bool clear, const Resolved& vs, const Resolved& ps,
                         const float* params, size_t bytes, std::initializer_list<const Image*> images) {
    Alloc cb = Upload(bytes, DK_UNIFORM_BUF_ALIGNMENT);
    std::memcpy(cb.cpu, params, bytes);
    Outside();
    BindOutput(&out, nullptr);
    cur_rt_ = cur_ds_ = nullptr;
    output_set_ = false;
    DkCmdBuf c = Cmd();
    PostState({0, 0, float(w), float(h), 0.0f, 1.0f}, {0, 0, w, h});
    if (clear) dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0, 0, 0, 0);
    Descriptors();
    BindShaders(vs.obj, ps.obj);
    dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 1, cb.gpu, uint32_t(AlignUp(uint32_t(bytes), 16)));
    dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 1, cb.gpu, uint32_t(AlignUp(uint32_t(bytes), 16)));
    uint32_t imgs[4] = {dummy_[kTex2D].desc, dummy_[kTex2D].desc, dummy_[kTex2D].desc, dummy_[kTex2D].desc};
    size_t i = 0;
    for (const Image* img : images) imgs[i++] = img->desc;
    uint32_t smps[4] = {blit_linear_, blit_point_, blit_linear_, blit_linear_};
    BindTexturePairs(ps, imgs, smps);
    dkCmdBufDraw(c, DkPrimitive_Triangles, 3, 1, 0, 0);
    pending_ = true;
}

bool DkBackend::EnsureSmaa(uint32_t w, uint32_t h) {
    if (smaa_failed_) return false;
    if (!smaa_built_) {
        static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
        const char* vs[3] = {"vs_edges", "vs_weights", "vs_blend"};
        const char* ps[3] = {"smaa_edges", "smaa_weights", "smaa_blend"};
        for (int i = 0; i < 3; i++) {
            Request(smaa_vs_[i], hlsl, Stage::Vertex, vs[i]);
            Request(smaa_ps_[i], hlsl, Stage::Pixel, ps[i]);
        }
        Await({&smaa_vs_[0], &smaa_vs_[1], &smaa_vs_[2], &smaa_ps_[0], &smaa_ps_[1], &smaa_ps_[2]});
        smaa_built_ = true;
        for (int i = 0; i < 3; i++) smaa_failed_ |= !smaa_vs_[i].obj || !smaa_ps_[i].obj;
        struct Table {
            Image& img;
            DkImageFormat fmt;
            uint32_t w, h, bpp;
            const uint8_t* data;
        } tables[2] = {{smaa_area_, DkImageFormat_RG8_Unorm, 160, 560, 2, kSmaaArea},
                       {smaa_search_, DkImageFormat_R8_Unorm, 64, 16, 1, kSmaaSearch}};
        static_assert(sizeof(kSmaaArea) == 160 * 560 * 2 && sizeof(kSmaaSearch) == 64 * 16);
        for (Table& l : tables) {
            if (smaa_failed_ || !CreateImage(l.img, l.fmt, DkImageType_2D, l.w, l.h, 1, 1, 1, false)) {
                smaa_failed_ = true;
                break;
            }
            Alloc a = Upload(size_t(l.w) * l.h * l.bpp, 16);
            std::memcpy(a.cpu, l.data, size_t(l.w) * l.h * l.bpp);
            Outside();
            CopyToImage(l.img, a, 0, 0, l.w, l.h, 1, l.w * l.bpp, l.h);
        }
        if (smaa_failed_) {
            LOGE("dk: SMAA could not be set up, it stays off");
            return false;
        }
        LOGI("dk: SMAA ready");
    }
    if (smaa_w_ == w && smaa_h_ == h) return true;
    const DkImageFormat fmt[3] = {DkImageFormat_RG8_Unorm, DkImageFormat_RGBA8_Unorm, kColourFormat};
    for (int i = 0; i < 3; i++) {
        if (smaa_[i].valid) Retire(smaa_[i]);
        if (!CreateImage(smaa_[i], fmt[i], DkImageType_2D, w, h, 1, 1, 1, true)) {
            smaa_w_ = smaa_h_ = 0;
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    return true;
}

const Image* DkBackend::Smaa(const Target& t) {
    if (!EnsureSmaa(t.w, t.h)) return nullptr;
    float p[4] = {1.0f / t.w, 1.0f / t.h, float(t.w), float(t.h)};
    DrawPost(smaa_[0], t.w, t.h, true, smaa_vs_[0], smaa_ps_[0], p, sizeof(p), {&t.img});
    DrawPost(smaa_[1], t.w, t.h, true, smaa_vs_[1], smaa_ps_[1], p, sizeof(p), {&smaa_[0], &smaa_area_, &smaa_search_});
    DrawPost(smaa_[2], t.w, t.h, false, smaa_vs_[2], smaa_ps_[2], p, sizeof(p), {&t.img, &smaa_[1]});
    return &smaa_[2];
}

void DkBackend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    ramp_set_ = brightness << 8 | contrast;
    uint8_t ramp[256 * 4];
    gamma_ = GammaRamp(brightness, contrast, has_game_ramp_ ? game_ramp_.data() : nullptr, ramp);
    Alloc a = Upload(sizeof(ramp), 16);
    std::memcpy(a.cpu, ramp, sizeof(ramp));
    Outside();
    CopyToImage(lut_, a, 0, 0, 256, 1, 1, 256 * 4, 1);
}

uint32_t DkBackend::Sampler(const SamplerDesc& d) {
    uint64_t key = Hash(&d, sizeof(d));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (next_sampler_ >= kSamplerDescs) {
        LOG_FIRST(4, "dk: the sampler table is full");
        return 0;
    }
    DkSampler s;
    dkSamplerDefaults(&s);
    s.minFilter = DkFilter(d.min);
    s.magFilter = DkFilter(d.mag);
    s.mipFilter = DkMipFilter(d.mip);
    for (int i = 0; i < 3; i++) s.wrapMode[i] = DkWrapMode(d.wrap[i]);
    s.lodBias = std::clamp(d.bias, -15.0f, 15.0f);
    s.lodClampMin = d.min_lod;
    s.lodClampMax = std::max(d.max_lod, d.min_lod);
    s.maxAnisotropy = std::max(1.0f, d.aniso);
    s.compareEnable = d.compare != 0;
    s.compareOp = d.compare ? DkCompareOp(d.compare) : DkCompareOp_Never;
    for (int i = 0; i < 4; i++) s.borderColor[i].value_f = d.border[i];
    uint32_t slot = next_sampler_++;
    DkSamplerDescriptor desc;
    dkSamplerDescriptorInitialize(&desc, &s);
    std::memcpy(sampler_descs_.cpu + size_t(slot) * sizeof(desc), &desc, sizeof(desc));
    descs_dirty_ = true;
    samplers_[key] = slot;
    return slot;
}

// ---- targets

void DkBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    Target& t = it->second;
    if (cur_rt_ == &t || cur_ds_ == &t) {
        Outside();
        cur_rt_ = cur_ds_ = nullptr;
        output_set_ = false;
    }
    if (bound_rt_ == &t || bound_ds_ == &t) bound_rt_ = bound_ds_ = nullptr;
    Retire(t.img);
    Retire(t.fb);
    targets_.erase(it);
}

Target* DkBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header || (!depth && IsShadowColour(s))) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    auto it = targets_.find(TargetKey(data, depth));
    if (it == targets_.end()) return nullptr;
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    return it->second.gw == gw && it->second.gh == gh ? &it->second : nullptr;
}

Target* DkBackend::EnsureTarget(const Surface& s, bool depth) {
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
    t.gw = std::max(gw, 1u);
    t.gh = std::max(gh, 1u);
    t.fixed = depth && g_shadow.blur_hi && s.data == g_shadow.map;
    float sc = t.fixed ? 1.0f : scale_;
    t.w = std::max(1u, uint32_t(std::lround(float(t.gw) * sc)));
    t.h = std::max(1u, uint32_t(std::lround(float(t.gh) * sc)));
    if (!CreateImage(t.img, depth ? kDepthFormat : kColourFormat, DkImageType_2D, t.w, t.h, 1, 1, 1, true)) {
        return nullptr;
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key] = std::move(t);
    // a new target starts cleared, as d3d12.cpp's
    ClearImage(ref.img, 1.0f);
    return &ref;
}

bool DkBackend::BindTargets(const State& s) {
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

void DkBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth || Format(d.format).host != Host::BGRA8) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    uint32_t w = std::min(d.width, t.gw), h = std::min(d.height, t.gh);
    bool scaled = t.w != t.gw || t.h != t.gh;
    LOG_FIRST(200, "dk: CPU pixels into target %08X: %ux%u, pitch %u, %zu bytes, host %ux%u", t.data, w, h, pitch,
              px.size(), t.w, t.h);
    Image* into = scaled ? Scratch(t.gw, t.gh) : &t.img;
    if (!into) return;
    Alloc a = Upload(size_t(pitch) * h, 16);
    if (a.cpu) std::memcpy(a.cpu, px.data(), std::min(px.size(), size_t(pitch) * h));
    Outside();
    CopyToImage(*into, a, 0, 0, w, h, 1, pitch, h);
    if (scaled) Blit(*into, t.gw, t.gh, t.img, t.w, t.h);
    static int dumped = 0;
    if (dumped < 2) {
        char name[48];
        std::snprintf(name, sizeof(name), "upload_%d_%08X_scratch", dumped, t.data);
        DumpImage(*into, name);
        std::snprintf(name, sizeof(name), "upload_%d_%08X_target", dumped++, t.data);
        DumpImage(t.img, name);
    }
    if (!t.read.empty() && d.width == t.gw && d.height == t.gh && pitch == t.gw * 4) t.read = px;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

uint32_t DkBackend::FeedbackCopy(Target& t) {
    if (!t.fb.valid && !CreateImage(t.fb, kColourFormat, DkImageType_2D, t.w, t.h, 1, 1, 1, true)) return 0;
    Outside();
    DkImageView s = LevelView(t.img, 0), d = LevelView(t.fb, 0);
    DkImageRect r = {0, 0, 0, t.w, t.h, 1};
    dkCmdBufCopyImage(Cmd(), &s, &r, &d, &r, 0);
    pending_ = true;
    return t.fb.desc;
}

// ---- textures

uint32_t DkBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                      DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return 0;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    stages.levels[stage] = 1;
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    if (&t == draw_rt_) return FeedbackCopy(t);
    return t.img.desc;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img.valid && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

DkImageFormat TextureFormat(const FormatInfo& fi) {
    switch (fi.host) {
        case Host::BC1: return DkImageFormat_RGBA_BC1;
        case Host::BC2: return DkImageFormat_RGBA_BC2;
        case Host::BC3: return DkImageFormat_RGBA_BC3;
        case Host::R32F: return fi.depth ? DkImageFormat_ZF32 : DkImageFormat_R32_Float;
        default: return kColourFormat;
    }
}

bool DkBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    tx.levels = d.levels;
    if (fi.compressed && ((d.width & 3) || (d.height & 3))) tx.levels = 1;
    uint32_t z = d.cube ? 1 : std::max(d.depth, 1u);
    DkImageType type = d.cube ? DkImageType_Cubemap : z > 1 ? DkImageType_3D : DkImageType_2D;
    LOG_FIRST(300, "dk: texture %08X %ux%ux%u format %02X (%s, host %d) levels %u%s%s", d.data, d.width, d.height, z,
              d.format, fi.name, int(fi.host), tx.levels, d.linear ? " linear" : "", d.cube ? " cube" : "");
    return CreateImage(tx.img, TextureFormat(fi), type, d.width, d.height, z, d.cube ? 6 : 1, tx.levels, false);
}

void DkBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p) {
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
            uint32_t row = bc ? ((w + 3) / 4) * block : pitch;
            uint32_t rows = bc ? (h + 3) / 4 : h;
            size_t bytes = size_t(row) * rows * z;
            px.resize(std::max(px.size(), bytes));
            Alloc a = Upload(bytes, 16);
            if (!a.cpu) continue;
            std::memcpy(a.cpu, px.data(), bytes);
            Outside();
            CopyToImage(tx.img, a, l, f, w, h, z, row, rows);
        }
    }
}

uint32_t DkBackend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (fi.host == Host::None) return 0;
    Tex& tx = textures_[d.data];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(d.data);
        return 0;
    }
    if (tx.frame != presents_ + 1 || !same) {
        tx.frame = presents_ + 1;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data);
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)));
            if (d.linear && dump_textures_.size() < 4) dump_textures_.push_back(d.data);
        }
    }
    stages.levels[stage] = tx.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return tx.img.desc;
}

uint32_t DkBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return 0;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(stage, d, fi, stages);
}

void DkBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;
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

// ---- a draw

void DkBackend::ForgetState() {
    known_ = true;
    std::memset(&dyn_, 0xFF, sizeof(dyn_));
}

void DkBackend::ApplyState(const State& s, const Assembled& a) {
    DkCmdBuf c = Cmd();
    if (!known_) ForgetState();
    // blend
    bool blend = SimpleState(s, kMBlendEnable) != 0;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = blend ? 1 : 0;
    if (std::memcmp(&cs, &dyn_.colour, sizeof(cs))) {
        dkCmdBufBindColorState(c, &cs);
        dyn_.colour = cs;
    }
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    if (blend) {
        bs.srcColorBlendFactor = bs.srcAlphaBlendFactor = Blend(SimpleState(s, kMSrcBlend));
        bs.dstColorBlendFactor = bs.dstAlphaBlendFactor = Blend(SimpleState(s, kMDestBlend));
        bs.colorBlendOp = bs.alphaBlendOp = BlendOp(SimpleState(s, kMBlendOp));
    }
    if (std::memcmp(&bs, &dyn_.blend, sizeof(bs))) {
        dkCmdBufBindBlendStates(c, 0, &bs, 1);
        dyn_.blend = bs;
    }
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    if (std::memcmp(bf, dyn_.constants, sizeof(bf))) {
        dkCmdBufSetBlendConst(c, bf[0], bf[1], bf[2], bf[3]);
        std::memcpy(dyn_.constants, bf, sizeof(bf));
    }
    // the console's enables are one per byte, A R G B
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    DkColorWriteState mask;
    dkColorWriteStateDefaults(&mask);
    dkColorWriteStateSetMask(&mask, 0,
                             ((cw >> 16) & 1 ? DkColorMask_R : 0u) | ((cw >> 8) & 1 ? DkColorMask_G : 0u) |
                                 (cw & 1 ? DkColorMask_B : 0u) | ((cw >> 24) & 1 ? DkColorMask_A : 0u));
    if (mask.masks != dyn_.mask.masks) {
        dkCmdBufBindColorWriteState(c, &mask);
        dyn_.mask = mask;
    }

    // depth and stencil
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    bool ztest = bound_ds_ && s.rs[kRsZEnable];
    ds.depthTestEnable = ztest;
    ds.depthWriteEnable = ztest && SimpleState(s, kMZWriteEnable);
    ds.depthCompareOp = ztest ? Cmp(SimpleState(s, kMZFunc)) : DkCompareOp_Always;
    ds.stencilTestEnable = bound_ds_ && s.rs[kRsStencilEnable];
    ds.stencilFrontFailOp = ds.stencilBackFailOp = StencilOp(s.rs[kRsStencilFail]);
    ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = StencilOp(SimpleState(s, kMStencilZFail));
    ds.stencilFrontPassOp = ds.stencilBackPassOp = StencilOp(SimpleState(s, kMStencilPass));
    ds.stencilFrontCompareOp = ds.stencilBackCompareOp = Cmp(SimpleState(s, kMStencilFunc));
    if (std::memcmp(&ds, &dyn_.ds, sizeof(ds))) {
        dkCmdBufBindDepthStencilState(c, &ds);
        dyn_.ds = ds;
    }
    uint32_t sref = SimpleState(s, kMStencilRef) & 0xFF, smask = SimpleState(s, kMStencilMask) & 0xFF;
    uint32_t swrite = SimpleState(s, kMStencilWriteMask) & 0xFF;
    if (sref != dyn_.sref || smask != dyn_.smask || swrite != dyn_.swrite) {
        dkCmdBufSetStencil(c, DkFace_FrontAndBack, uint8_t(swrite), uint8_t(sref), uint8_t(smask));
        dyn_.sref = sref;
        dyn_.smask = smask;
        dyn_.swrite = swrite;
    }

    // rasterizer: with the origin at the top the image is D3D's, and so are its windings
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    uint32_t cull = s.rs[kRsCullMode];
    rs.cullMode = cull && a.topology == Topology::Triangles ? DkFace_Back : DkFace_None;
    rs.frontFace = cull == kCullCw ? DkFrontFace_CCW : DkFrontFace_CW;
    DkPolygonMode fill = s.rs[kRsFillMode] == kFillWireframe ? DkPolygonMode_Line : DkPolygonMode_Fill;
    rs.polygonModeFront = rs.polygonModeBack = fill;
    // depth bias in depth buffer units, as for the console's 24 bit buffer
    bool bias = bound_ds_ && a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
    rs.depthBiasEnableMask = bias ? DkPolygonFlag_All : 0;
    if (std::memcmp(&rs, &dyn_.rast, sizeof(rs))) {
        dkCmdBufBindRasterizerState(c, &rs);
        dyn_.rast = rs;
    }
    float bconst = bias ? float(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale)) : 0;
    float bslope = bias ? Bits(SimpleState(s, kMSlopeScale)) : 0;
    if (bconst != dyn_.bias_const || bslope != dyn_.bias_slope) {
        dkCmdBufSetDepthBias(c, bconst, 0.0f, bslope);
        dyn_.bias_const = bconst;
        dyn_.bias_slope = bslope;
    }
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void DkBackend::SetViewportScissor(const State& s, uint32_t gw) {
    int W = int(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    int H = int(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    DkViewport vp = {0, 0, float(W), float(H), 0.0f, 1.0f};
    if (std::memcmp(&vp, &dyn_.vp, sizeof(vp))) {
        dkCmdBufSetViewports(Cmd(), 0, &vp, 1);
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
    DkScissor r = {uint32_t(x0), uint32_t(y0), uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))};
    if (std::memcmp(&r, &dyn_.sc, sizeof(r))) {
        dkCmdBufSetScissors(Cmd(), 0, &r, 1);
        dyn_.sc = r;
    }
}

void DkBackend::BindShaders(const DkShader* vs, const DkShader* ps) {
    if (vs == bound_vs_ && ps == bound_ps_) return;
    const DkShader* shaders[2] = {vs, ps};
    dkCmdBufBindShaders(Cmd(), DkStageFlag_GraphicsMask, shaders, 2);
    bound_vs_ = vs;
    bound_ps_ = ps;
}

bool DkBackend::UploadVertices(const Assembled& a, Vertices& v) {
    // every buffer reaches the last vertex read, zeros after the guest's (D3D12 gives 0 past a buffer's end)
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
        if (need > kSegment) {
            LOG_FIRST(8, "dk: %llu bytes of vertices in one draw, left out", (unsigned long long)need);
            return false;
        }
        Alloc vb = Upload(size_t(need), 16);
        if (b.src) std::memcpy(vb.cpu, b.src, b.bytes);
        if (need > b.bytes) std::memset(vb.cpu + b.bytes, 0, size_t(need - b.bytes));
        v.buf[i] = {vb.gpu, uint32_t(need)};
    }
    return true;
}

void DkBackend::SetVertexInputs(const Assembled& a, const int8_t* locations, const Vertices& v) {
    std::array<DkVtxBufferState, 16> bind{};
    uint32_t binds = std::min<uint32_t>(a.buffer_count, 16);
    for (uint32_t i = 0; i < binds; i++) bind[i] = {a.buffers[i].stride, 0};
    // attribute n is the shader's location n; a location the draw does not feed reads 0
    std::array<DkVtxAttribState, 16> attr{};
    for (auto& x : attr) {
        x = {};
        x.isFixed = 1;
        x.size = DkVtxAttribSize_4x32;
        x.type = DkVtxAttribType_Float;
    }
    uint32_t attrs = 0;
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1) || locations[r] < 0) continue;
        const VertexElement& e = a.elements[size_t(r)];
        DkVtxAttribState d{};
        if (!VertexFormat(e.format, d)) {
            LOG_FIRST(8, "dk: vertex format %u not handled", e.format);
            continue;
        }
        d.bufferId = e.buffer;
        d.offset = e.offset;
        d.isFixed = 0;
        uint32_t loc = uint32_t(locations[r]);
        attr[loc] = d;
        attrs = std::max(attrs, loc + 1);
    }
    auto same = [](const auto& x, uint32_t n, const auto& y, uint32_t m) {
        return n == m && !std::memcmp(x.data(), y.data(), n * sizeof(x[0]));
    };
    DkCmdBuf c = Cmd();
    if (!vin_set_ || !same(attr, attrs, vin_attr_, vin_attrs_)) {
        dkCmdBufBindVtxAttribState(c, attr.data(), attrs);
        std::copy_n(attr.begin(), attrs, vin_attr_.begin());
        vin_attrs_ = attrs;
    }
    if (!vin_set_ || !same(bind, binds, vin_bind_, vin_binds_)) {
        dkCmdBufBindVtxBufferState(c, bind.data(), binds);
        std::copy_n(bind.begin(), binds, vin_bind_.begin());
        vin_binds_ = binds;
    }
    vin_set_ = true;
    if (binds) dkCmdBufBindVtxBuffers(c, 0, v.buf, binds);
}

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

void DkBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
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

    DrawTextures stages;
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        auto timed = Time(kTextures);
        for (int i = 0; i < 4; i++) stages.desc[i] = BindTexture(s, i, stages);
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
        vs = &Shader(sh.vkey, Stage::Vertex, [](void* p) -> const std::string& {
            auto* g = static_cast<VertexGen*>(p);
            return g->out = VertexHlsl(g->s, g->a, g->sh);
        }, &vg);
        PixelGen pg{pk, {}};
        ps = &Shader(sh.pkey ^ kPixelKeySalt, Stage::Pixel, [](void* p) -> const std::string& {
            auto* g = static_cast<PixelGen*>(p);
            return g->out = PixelHlsl(g->pk);
        }, &pg);
        if (!vs->obj || !ps->obj) {
            skipped_++;
            return;
        }
    }

    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    Alloc vc, dc;
    {
        auto timed = Time(kConstants);
        vc = Upload(sizeof(s.vsc), DK_UNIFORM_BUF_ALIGNMENT);
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
        dc = Upload(sizeof(c), DK_UNIFORM_BUF_ALIGNMENT);
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
        uint32_t images[4], smps[4];
        for (int i = 0; i < 4; i++) {
            uint8_t kind = stages.info[i].kind;
            images[i] = stages.desc[i] ? stages.desc[i] : dummy_[kind <= kTexCube ? kind : 0].desc;
            smps[i] = Sampler(StageSampler(s, i, stages, clamp, aniso));
            images_dbg_[i] = images[i];
        }
        Descriptors();
        DkCmdBuf c = Cmd();
        uint32_t vbytes = uint32_t(AlignUp(uint32_t(sizeof(s.vsc)), 16));
        uint32_t dbytes = uint32_t(AlignUp(uint32_t(sizeof(DrawConstants)), 16));
        dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, vc.gpu, vbytes);
        dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 1, dc.gpu, dbytes);
        dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 0, vc.gpu, vbytes);
        dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 1, dc.gpu, dbytes);
        BindTexturePairs(*ps, images, smps);
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
        DkPrimitive p = PrimitiveOf(a);
        if (a.index_count) {
            dkCmdBufBindIdxBuffer(Cmd(), a.index16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, ib.gpu);
            dkCmdBufDrawIndexed(Cmd(), p, a.index_count, 1, 0, a.base_vertex, 0);
        } else {
            dkCmdBufDraw(Cmd(), p, a.draw_count, 1, 0, 0);
        }
    }
    drawn_ = true;
    // diagnostic: the first draws' state, and their target right after them
    if (draws_ <= 40) {
        LOGI("dk: draw %llu: prim %d count %u idx %u into %08X%s, cull %u front %u fill %u, z %u/%u func %u, "
             "stencil %u, blend %u, mask %X, vp %.0fx%.0f, scissor %u,%u %ux%u, tex %u %u %u %u, rect %d%d%d%d",
             (unsigned long long)draws_, int(PrimitiveOf(a)), a.draw_count, a.index_count,
             bound_rt_ ? bound_rt_->data : 0u, bound_ds_ ? " +depth" : "", uint32_t(dyn_.rast.cullMode),
             uint32_t(dyn_.rast.frontFace), uint32_t(dyn_.rast.polygonModeFront), uint32_t(dyn_.ds.depthTestEnable),
             uint32_t(dyn_.ds.depthWriteEnable), uint32_t(dyn_.ds.depthCompareOp), uint32_t(dyn_.ds.stencilTestEnable),
             uint32_t(dyn_.colour.blendEnableMask), dyn_.mask.masks & 0xF, dyn_.vp.width, dyn_.vp.height, dyn_.sc.x,
             dyn_.sc.y, dyn_.sc.width, dyn_.sc.height, images_dbg_[0], images_dbg_[1], images_dbg_[2], images_dbg_[3],
             stages.info[0].rect, stages.info[1].rect, stages.info[2].rect, stages.info[3].rect);
        if (draws_ <= 6 && bound_rt_) {
            char name[48];
            std::snprintf(name, sizeof(name), "draw_%02llu_%08X", (unsigned long long)draws_, bound_rt_->data);
            DumpImage(bound_rt_->img, name);
        }
    }
}

void DkBackend::ClearTargets(Target* rt, Target* ds, const State& s, uint32_t count, const uint8_t* rects,
                             uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) {
        ClearTargets(rt, nullptr, s, count, rects, flags, color, z, stencil);
        ClearTargets(nullptr, ds, s, count, rects, flags, color, z, stencil);
        return;
    }
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    Target& t = rt ? *rt : *ds;
    std::vector<DkScissor> rs;
    float sc = float(t.w) / t.gw;
    auto add = [&](int x0, int y0, int x1, int y1) {
        x0 = std::clamp(int(x0 * sc), 0, int(t.w));
        y0 = std::clamp(int(y0 * sc), 0, int(t.h));
        x1 = std::clamp(int(x1 * sc), 0, int(t.w));
        y1 = std::clamp(int(y1 * sc), 0, int(t.h));
        if (x1 > x0 && y1 > y0) rs.push_back({uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)});
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
    Inside(rt, ds);
    DkCmdBuf c = Cmd();
    // a clear writes every channel and the whole stencil byte, as on the console
    DkColorWriteState all;
    dkColorWriteStateDefaults(&all);
    dkCmdBufBindColorWriteState(c, &all);
    dyn_.mask.masks = ~0u;
    float col[4];
    Color(color, col);
    for (const DkScissor& r : rs) {
        dkCmdBufSetScissors(c, 0, &r, 1);
        if (rt) dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, col[0], col[1], col[2], col[3]);
        if (ds && (flags & (kClearZ | kClearStencil))) {
            dkCmdBufClearDepthStencil(c, (flags & kClearZ) != 0, std::clamp(z, 0.0f, 1.0f),
                                      (flags & kClearStencil) ? 0xFF : 0, uint8_t(stencil & 0xFF));
        }
    }
    dyn_.sc.width = ~0u;
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
    drawn_ = true;
}

void DkBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    auto timed = Time(kClear);
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (rt || ds) ClearTargets(rt, ds, s, count, rects, flags, color, z, stencil);
}

void DkBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                     uint32_t dst_points) {
    auto timed = Time(kCopy);
    Target* src_t = FindTarget(src, false);
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "dk: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    auto host = [](int v, uint32_t size, uint32_t guest) { return uint32_t(int64_t(v) * size / guest); };
    bool same = uint64_t(src_t->w) * dst_t->gw == uint64_t(dst_t->w) * src_t->gw &&
                uint64_t(src_t->h) * dst_t->gh == uint64_t(dst_t->h) * src_t->gh;
    DkImageView sv = LevelView(src_t->img, 0), dv = LevelView(dst_t->img, 0);
    bool any = false;
    for (uint32_t i = 0; i < n; i++) {
        int x0 = 0, y0 = 0, x1 = int(std::min(src_t->gw, dst_t->gw)), y1 = int(std::min(src_t->gh, dst_t->gh));
        int dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(src_rects, i);
            x0 = std::max(r.left, 0);
            y0 = std::max(r.top, 0);
            x1 = std::min(r.right, int(src_t->gw));
            y1 = std::min(r.bottom, int(src_t->gh));
            dx = x0;
            dy = y0;
            if (dst_points) {
                dx = int32_t(kernel::R32(dst_points + i * 8));
                dy = int32_t(kernel::R32(dst_points + i * 8 + 4));
            }
        }
        if (x1 <= x0 || y1 <= y0 || dx < 0 || dy < 0 || dx >= int(dst_t->gw) || dy >= int(dst_t->gh)) continue;
        x1 = std::min(x1, x0 + int(dst_t->gw) - dx);
        y1 = std::min(y1, y0 + int(dst_t->gh) - dy);
        uint32_t sx0 = host(x0, src_t->w, src_t->gw), sy0 = host(y0, src_t->h, src_t->gh);
        uint32_t sx1 = host(x1, src_t->w, src_t->gw), sy1 = host(y1, src_t->h, src_t->gh);
        uint32_t dx0 = host(dx, dst_t->w, dst_t->gw), dy0 = host(dy, dst_t->h, dst_t->gh);
        uint32_t dx1 = host(dx + x1 - x0, dst_t->w, dst_t->gw), dy1 = host(dy + y1 - y0, dst_t->h, dst_t->gh);
        if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0) continue;
        if (!any) Outside();
        any = true;
        DkImageRect a = {sx0, sy0, 0, sx1 - sx0, sy1 - sy0, 1}, b = {dx0, dy0, 0, dx1 - dx0, dy1 - dy0, 1};
        // at one scale a blit unfiltered is the copy it was; between scales it stretches
        dkCmdBufBlitImage(Cmd(), &sv, &a, &dv, &b, same ? 0u : uint32_t(DkBlitFlag_FilterLinear), 0);
    }
    if (any) pending_ = true;
    dst_t->drawn = true;
}

void DkBackend::Sync(uint32_t data, uint32_t) {
    auto timed = Time(kSync);
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    std::vector<uint8_t> px;
    if (!ReadTarget(t, px)) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void DkBackend::ResetLate(Late& l) {
    for (int i = 0; i < kLateSlots; i++) Retire(l.buf[i]);
    l = Late{};
}

void DkBackend::PollLate(Late& l) {
    for (int i = 0; i < kLateSlots; i++) {
        if (!l.serial[i] || l.serial[i] > done_) continue;
        l.serial[i] = 0;
        l.done[i] = true;
    }
}

bool DkBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kLateSlots; i++) {
        bool finished = l.done[i] || (l.serial[i] && l.fence[i] && dkFenceWait(l.fence[i], 0) == DkResult_Success);
        bool recent = now - l.frame[i] <= uint64_t(kLateSlots) + 1;
        if (finished && recent && (best < 0 || l.seq[i] > l.seq[best])) best = i;
    }
    if (best < 0) return false;
    if (l.seq[best] > l.delivered) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        (void)0;  // uncached: nothing to flush
        const uint8_t* src = l.buf[best].mem.cpu;
        for (uint32_t y = 0; y < l.gh; y++) {
            std::memcpy(dst + size_t(y) * pitch, src + size_t(y) * l.gw * 4, l.gw * 4);
        }
        l.delivered = l.seq[best];
    }
    return true;
}

void DkBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        ResetLate(l);
        l.gw = t.gw;
        l.gh = t.gh;
    }
    l.locked = presents_;
    Poll();
    PollLate(l);
    CopyLate(l, t);
}

void DkBackend::CopyLate(Late& l, Target& t) {
    uint32_t slot = l.next;
    if (l.serial[slot]) return;
    uint32_t bytes = t.gw * t.gh * 4;
    if (!l.buf[slot].mem && !CreateBuffer(l.buf[slot], bytes, true)) {
        LOGE("dk: no read-back buffer");
        return;
    }
    Image* src = GuestSized(t);
    if (!src) return;
    CopyOut(*src, t.gw, t.gh, l.buf[slot].mem.gpu);
    l.serial[slot] = serial_;
    l.fence[slot] = &subs_[cur_].fence;
    l.done[slot] = false;
    l.seq[slot] = ++late_seq_;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kLateSlots;
}

void DkBackend::CountFps() {
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
        LOGR("dk: %.1f fps, %llu draws a frame (%llu waited for shaders), %zu targets, %zu textures, %zu shaders, "
             "%zu to build", double(app::g_fps), (unsigned long long)((draws_ - last_draws_) / frames),
             (unsigned long long)skipped_, targets_.size(), textures_.size(), keys_.size(), building);
        static const char* const kNames[kPhases] = {"assemble", "textures", "tex upload", "shaders", "shader wait",
                                                    "constants", "stages", "state", "vertices", "draw call",
                                                    "ring wait", "clear", "copy", "sync", "present", "swap",
                                                    "targets", "keys", "whole draws"};
        char line[512];
        int at = std::snprintf(line, sizeof(line), "dk: ms a frame:");
        for (int i = 0; i < kPhases && at < int(sizeof(line)); i++)
            at += std::snprintf(line + at, sizeof(line) - size_t(at), " %s %.2f", kNames[i],
                                double(phase_ns_[i]) / 1e6 / double(frames));
        LOGR("%s", line);
        uint64_t cpu = port::ThreadCpuNs();
        double wall = std::chrono::duration<double, std::milli>(now - log_t_).count();
        if (log_cpu_ && cpu)
            LOGR("dk: the render thread ran on a CPU %.0f ms of %.0f ms (%.0f%%)", double(cpu - log_cpu_) / 1e6, wall,
                 100.0 * double(cpu - log_cpu_) / 1e6 / wall);
        log_cpu_ = cpu;
        LOGR("dk: a frame: %.1f texture uploads (%.0f KB), %.2f shader compiles", double(tex_uploads_) / double(frames),
             double(tex_upload_bytes_) / 1024.0 / double(frames), double(compiles_.exchange(0)) / double(frames));
        std::memset(phase_ns_, 0, sizeof(phase_ns_));
        tex_uploads_ = tex_upload_bytes_ = 0;
        skipped_ = 0;
        last_presents_ = presents_;
        last_draws_ = draws_;
        log_t_ = now;
    }
}

// ---- the swap chain and the present

void DkBackend::DestroySwapchain() {
    if (!swapchain_) return;
    dkQueueWaitIdle(queue_);
    dkSwapchainDestroy(swapchain_);
    swapchain_ = nullptr;
    for (Image& img : swap_) DestroyImage(img);
}

// the swap chain at the window's size, on the default window (no SDL window holds it: sdl_window.cpp)
bool DkBackend::Swapchain() {
    uint32_t bw, bh;
    port::DrawableSize(bw, bh);
    asked_w_ = bw;
    asked_h_ = bh;
    DestroySwapchain();
    NWindow* win = nwindowGetDefault();
    if (!win || !nwindowIsValid(win)) {
        LOGE("dk: no default window");
        return false;
    }
    if (Result rc = nwindowSetDimensions(win, bw, bh); R_FAILED(rc)) LOGW("dk: window size %ux%u refused (0x%X)", bw, bh, rc);
    for (Image& img : swap_) {
        img = Image{};
        DkImageLayoutMaker lm;
        dkImageLayoutMakerDefaults(&lm, device_);
        lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = bw;
        lm.dimensions[1] = bh;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &lm);
        img.mem = Allocate(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout), 0, true);
        if (!img.mem) return false;
        dkImageInitialize(&img.img, &layout, img.mem.block, img.mem.off);
        img.valid = true;
        img.format = DkImageFormat_RGBA8_Unorm;
        img.w = bw;
        img.h = bh;
    }
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) images[i] = &swap_[i].img;
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device_, win, images, kSwapImages);
    logx::Flush();
    swapchain_ = dkSwapchainCreate(&sm);
    if (!swapchain_) return false;
    vsync_ = app::Get().vsync;
    dkSwapchainSetSwapInterval(swapchain_, vsync_ ? 1 : 0);
    swap_w_ = bw;
    swap_h_ = bh;
    LOGI("dk: swap chain %ux%u", bw, bh);
    return true;
}

void DkBackend::Present(const Surface& front, bool pillarbox) {
    auto timed = Time(kPresent);
    const app::Settings& set = app::Get();
    uint32_t bw, bh;
    port::DrawableSize(bw, bh);
    if (bw != asked_w_ || bh != asked_h_) {
        if (recorded_) Submit();
        Swapchain();
    } else if (set.vsync != vsync_ && swapchain_) {
        vsync_ = set.vsync;
        dkSwapchainSetSwapInterval(swapchain_, vsync_ ? 1 : 0);
    }
    if ((set.brightness << 8 | set.contrast) != ramp_set_) UploadGammaRamp(set.brightness, set.contrast);
    ApplyRenderScale();
    {
        std::lock_guard lock(late_lock_);
        for (auto& [data, l] : late_) {
            if (presents_ - l.locked > kLateKeep || l.gw * l.gh * 4 > kLateKeepBytes) continue;
            auto it = targets_.find(TargetKey(data, false));
            if (it != targets_.end() && it->second.gw == l.gw && it->second.gh == l.gh) CopyLate(l, it->second);
        }
    }
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    LOG_FIRST(150, "dk: present %llu: front %08X %ux%u (host %ux%u), %llu draws so far", (unsigned long long)presents_,
              t ? t->data : 0u, t ? t->gw : 0u, t ? t->gh : 0u, t ? t->w : 0u, t ? t->h : 0u,
              (unsigned long long)draws_);
    while (dumped_textures_ < dump_textures_.size()) {
        uint32_t a = dump_textures_[dumped_textures_];
        auto it = textures_.find(a);
        char name[48];
        std::snprintf(name, sizeof(name), "tex_%08X_%zu", a, dumped_textures_++);
        if (it != textures_.end() && it->second.img.valid) DumpImage(it->second.img, name);
    }
    if (t && (presents_ < 3 || presents_ % 300 == 0) && presents_ <= 1500) {
        char name[48];
        std::snprintf(name, sizeof(name), "front_%04llu_%08X", (unsigned long long)presents_, t->data);
        DumpImage(t->img, name);
    }
    const Image* shown = t ? &t->img : nullptr;
    if (t && set.smaa) {
        if (const Image* aa = Smaa(*t)) shown = aa;
    }

    float c[20] = {0, 0, 1, 1, gamma_ ? 1.0f : 0.0f, 0, 1, 1};
    uint32_t mode = set.colour_blind;
    if (mode) {
        std::array<float, 9> colour = ColourBlindMatrix(mode);
        for (int row = 0; row < 3; row++) std::memcpy(c + 8 + row * 4, colour.data() + row * 3, 3 * sizeof(float));
        c[11] = 1.0f;
    }
    Alloc cb = Upload(sizeof(c), DK_UNIFORM_BUF_ALIGNMENT);
    std::memcpy(cb.cpu, c, sizeof(c));

    if (!swapchain_) {
        Submit();
    } else {
        // the frame's draws to the GPU first, then the swap image waited for: the GPU need not wait for it
        if (recorded_) Submit();
        int idx;
        {
            auto swap = Time(kSwap);
            idx = dkQueueAcquireImage(queue_, swapchain_);
        }
        Image& out = swap_[idx];
        Outside();
        BindOutput(&out, nullptr);
        cur_rt_ = cur_ds_ = nullptr;
        output_set_ = false;
        DkCmdBuf cmd = Cmd();
        DkScissor all = {0, 0, swap_w_, swap_h_};
        dkCmdBufSetScissors(cmd, 0, &all, 1);
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
        if (shown) {
            float aspect = app::Aspect();
            float w = float(swap_w_);
            float h = float(swap_h_);
            float vw = std::min(w, h * aspect);
            float vh = vw / aspect;
            if (pillarbox) vw = vh * 4.0f / 3.0f;
            float x = std::floor((w - vw) / 2), y = std::floor((h - vh) / 2);
            PostState({x, y, vw, vh, 0.0f, 1.0f}, all);
            Descriptors();
            BindShaders(blit_vs_.obj, blit_ps_.obj);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, cb.gpu, sizeof(c));
            dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, cb.gpu, sizeof(c));
            uint32_t imgs[4] = {shown->desc, lut_.desc, dummy_[kTex2D].desc, dummy_[kTex2D].desc};
            uint32_t smps[4] = {blit_linear_, blit_point_, blit_linear_, blit_linear_};
            BindTexturePairs(blit_ps_, imgs, smps);
            dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        }
        dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
        uint64_t serial = Submit();
        frames_.push_back(serial);
        auto swap = Time(kSwap);
        dkQueuePresentImage(queue_, swapchain_, idx);
    }
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

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<DkBackend>(); }
void SavePipelineCache() {}

}  // namespace gpu
