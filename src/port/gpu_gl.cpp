// The title's Direct3D 8 calls drawn with OpenGL 4.3 (SCDA_GPU=gl): Linux on Mesa, the Switch on switch-mesa's
// nouveau through EGL. It follows gpu/d3d12.cpp draw for draw: the same HLSL from gpu/shaders.cpp (as GLSL through
// gl_shader.cpp), the same constants, targets kept by surface memory, textures by guest address.
//
// glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) gives D3D's conventions: row 0 of every texture and target is the top,
// viewports, scissors and gl_FragCoord count from it, depth runs 0 to 1. Only the window's framebuffer is drawn the
// GL way up, in Present.
//
// Not here yet, as on D3D12: render scale (targets are the console's size, the frame is scaled to the window),
// MSAA, SMAA, shader model 3 HDR and soft shadows, ambient occlusion, the texture pack, captures, the settings panel.
#include <SDL.h>
#include <GL/glcorearb.h>
#ifdef SCDA_SWITCH
#include <EGL/egl.h>
#endif
#include <dxgiformat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "port/gl_shader.h"
#include "port/gpu_common.h"
#include "port/port.h"

namespace gpu {

namespace {

using namespace common;

// GL_EXT_texture_compression_s3tc and GL_EXT_texture_filter_anisotropic, which glcorearb.h leaves out
constexpr GLenum kDxt1 = 0x83F1;
constexpr GLenum kDxt3 = 0x83F2;
constexpr GLenum kDxt5 = 0x83F3;
constexpr GLenum kMaxAnisotropy = 0x84FE;

#define SCDA_GL_FUNCS(X)                                                                                              \
    X(PFNGLGETSTRINGPROC, GetString)                                                                                  \
    X(PFNGLGETSTRINGIPROC, GetStringi)                                                                                \
    X(PFNGLGETINTEGERVPROC, GetIntegerv)                                                                              \
    X(PFNGLGETERRORPROC, GetError)                                                                                    \
    X(PFNGLENABLEPROC, Enable)                                                                                        \
    X(PFNGLDISABLEPROC, Disable)                                                                                      \
    X(PFNGLVIEWPORTPROC, Viewport)                                                                                    \
    X(PFNGLSCISSORPROC, Scissor)                                                                                      \
    X(PFNGLCLEARPROC, Clear)                                                                                          \
    X(PFNGLCLEARCOLORPROC, ClearColor)                                                                                \
    X(PFNGLCLEARDEPTHPROC, ClearDepth)                                                                                \
    X(PFNGLCLEARSTENCILPROC, ClearStencil)                                                                            \
    X(PFNGLCOLORMASKPROC, ColorMask)                                                                                  \
    X(PFNGLDEPTHMASKPROC, DepthMask)                                                                                  \
    X(PFNGLDEPTHFUNCPROC, DepthFunc)                                                                                  \
    X(PFNGLSTENCILMASKPROC, StencilMask)                                                                              \
    X(PFNGLSTENCILFUNCPROC, StencilFunc)                                                                              \
    X(PFNGLSTENCILOPPROC, StencilOp)                                                                                  \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate)                                                                  \
    X(PFNGLBLENDEQUATIONPROC, BlendEquation)                                                                          \
    X(PFNGLBLENDCOLORPROC, BlendColor)                                                                                \
    X(PFNGLCULLFACEPROC, CullFace)                                                                                    \
    X(PFNGLFRONTFACEPROC, FrontFace)                                                                                  \
    X(PFNGLPOLYGONMODEPROC, PolygonMode)                                                                              \
    X(PFNGLPOLYGONOFFSETPROC, PolygonOffset)                                                                          \
    X(PFNGLPIXELSTOREIPROC, PixelStorei)                                                                              \
    X(PFNGLCLIPCONTROLPROC, ClipControl)                                                                              \
    X(PFNGLGENTEXTURESPROC, GenTextures)                                                                              \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures)                                                                        \
    X(PFNGLBINDTEXTUREPROC, BindTexture)                                                                              \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                                                          \
    X(PFNGLTEXIMAGE2DPROC, TexImage2D)                                                                                \
    X(PFNGLTEXIMAGE3DPROC, TexImage3D)                                                                                \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D)                                                                          \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, CompressedTexImage2D)                                                            \
    X(PFNGLCOMPRESSEDTEXIMAGE3DPROC, CompressedTexImage3D)                                                            \
    X(PFNGLTEXPARAMETERIPROC, TexParameteri)                                                                          \
    X(PFNGLGENSAMPLERSPROC, GenSamplers)                                                                              \
    X(PFNGLSAMPLERPARAMETERIPROC, SamplerParameteri)                                                                  \
    X(PFNGLSAMPLERPARAMETERFPROC, SamplerParameterf)                                                                  \
    X(PFNGLSAMPLERPARAMETERFVPROC, SamplerParameterfv)                                                                \
    X(PFNGLBINDSAMPLERPROC, BindSampler)                                                                              \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                                                                      \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                                                                \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                                                      \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                                                            \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                                                        \
    X(PFNGLDRAWBUFFERPROC, DrawBuffer)                                                                                \
    X(PFNGLREADBUFFERPROC, ReadBuffer)                                                                                \
    X(PFNGLREADPIXELSPROC, ReadPixels)                                                                                \
    X(PFNGLCOPYIMAGESUBDATAPROC, CopyImageSubData)                                                                    \
    X(PFNGLGENBUFFERSPROC, GenBuffers)                                                                                \
    X(PFNGLDELETEBUFFERSPROC, DeleteBuffers)                                                                          \
    X(PFNGLBINDBUFFERPROC, BindBuffer)                                                                                \
    X(PFNGLBUFFERDATAPROC, BufferData)                                                                                \
    X(PFNGLBUFFERSTORAGEPROC, BufferStorage)                                                                          \
    X(PFNGLMAPBUFFERRANGEPROC, MapBufferRange)                                                                        \
    X(PFNGLBINDBUFFERRANGEPROC, BindBufferRange)                                                                      \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                                                      \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                                                      \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray)                                                      \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray)                                                    \
    X(PFNGLVERTEXATTRIBFORMATPROC, VertexAttribFormat)                                                                \
    X(PFNGLVERTEXATTRIBIFORMATPROC, VertexAttribIFormat)                                                              \
    X(PFNGLVERTEXATTRIBBINDINGPROC, VertexAttribBinding)                                                              \
    X(PFNGLBINDVERTEXBUFFERPROC, BindVertexBuffer)                                                                    \
    X(PFNGLDRAWARRAYSPROC, DrawArrays)                                                                                \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC, DrawElementsBaseVertex)                                                        \
    X(PFNGLCREATESHADERPROC, CreateShader)                                                                            \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                                                            \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                                                          \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                                                              \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                                                                    \
    X(PFNGLDELETESHADERPROC, DeleteShader)                                                                            \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                                                          \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                                                            \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                                                              \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                                                            \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                                                          \
    X(PFNGLPROGRAMPARAMETERIPROC, ProgramParameteri)                                                                  \
    X(PFNGLGETPROGRAMBINARYPROC, GetProgramBinary)                                                                    \
    X(PFNGLPROGRAMBINARYPROC, ProgramBinary)                                                                          \
    X(PFNGLISPROGRAMPROC, IsProgram)                                                                                  \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                                                                  \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                                                                \
    X(PFNGLUNIFORM2UIPROC, Uniform2ui)                                                                                \
    X(PFNGLBINDBUFFERBASEPROC, BindBufferBase)                                                                        \
    X(PFNGLDISPATCHCOMPUTEPROC, DispatchCompute)                                                                      \
    X(PFNGLMEMORYBARRIERPROC, MemoryBarrier)                                                                          \
    X(PFNGLFENCESYNCPROC, FenceSync)                                                                                  \
    X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync)                                                                        \
    X(PFNGLDELETESYNCPROC, DeleteSync)                                                                                \
    X(PFNGLFLUSHPROC, Flush)                                                                                          \
    X(PFNGLFINISHPROC, Finish)

struct GlFuncs {
#define X(type, name) type name = nullptr;
    SCDA_GL_FUNCS(X)
#undef X
};
GlFuncs gl;

bool LoadGl() {
    bool ok = true;
#define X(type, name)                                                                                                 \
    gl.name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));                                              \
    if (!gl.name) {                                                                                                   \
        LOGE("gl: no gl" #name);                                                                                      \
        ok = false;                                                                                                   \
    }
    SCDA_GL_FUNCS(X)
#undef X
    return ok;
}

// the XDK's blend, compare and stencil values are the GL enums the console's GPU took
GLenum Blend(uint32_t v) {
    switch (v) {
        case 0: case 1:
        case 0x300: case 0x301: case 0x302: case 0x303: case 0x304: case 0x305: case 0x306: case 0x307: case 0x308:
        case 0x8001: case 0x8002: case 0x8003: case 0x8004:
            return v;
        default: return GL_ONE;
    }
}

GLenum BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: case 0x800B: case 0x8007: case 0x8008: return v;
        default: return GL_FUNC_ADD;
    }
}

GLenum Cmp(uint32_t v) { return GL_NEVER + CmpIndex(v); }

GLenum StencilOp(uint32_t v) {
    switch (v) {
        case 0: case 0x1E01: case 0x1E02: case 0x1E03: case 0x150A: case 0x8507: case 0x8508: return v;
        default: return GL_KEEP;
    }
}

GLenum Address(uint32_t v) {
    switch (v) {
        case 2: return GL_MIRRORED_REPEAT;
        case 3: case 5: return GL_CLAMP_TO_EDGE;
        case 4: return GL_CLAMP_TO_BORDER;
        default: return GL_REPEAT;
    }
}

struct Target {
    GLuint tex = 0;
    GLuint fb = 0;  // a copy draws read while drawing to the target
    bool depth = false;
    uint32_t gw = 0, gh = 0;  // the console's size
    uint32_t w = 0, h = 0;    // the host texture's
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0;          // last lock seen
    uint64_t sig = 0;            // guest memory as last matched
    bool drawn = false;          // the GPU wrote it since the last readback
    std::vector<uint8_t> read;   // what Sync left in guest memory
};

struct Tex {
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    bool norm = false;  // a normalization cube
};

// a draw's four texture stages
struct DrawTextures {
    TexInfo info[4];
    GLuint tex[4] = {};
    GLenum target[4] = {};
    uint32_t levels[4] = {};
    bool rt[4] = {};  // stage samples a render target
};

struct Shader {
    GLuint id = 0;  // 0: it did not compile
    int8_t locations[16];
    // a draw's shaders compile only when their program was not linked ahead: the GLSL waits here till then
    uint64_t hash = 0;  // of the GLSL, 0: it did not translate
    GLenum stage = 0;
    std::string glsl;
};

// streaming memory for vertices, indices and constants: one persistently mapped buffer in kSegments parts, each
// fenced when the ring moves past it and waited for before it is written again
constexpr uint32_t kSegments = 4;
#ifdef SCDA_SWITCH
constexpr size_t kRingSize = 32u << 20;
#else
constexpr size_t kRingSize = 64u << 20;
#endif

struct SamplerDesc {
    GLint min = GL_NEAREST_MIPMAP_NEAREST, mag = GL_NEAREST;
    GLint wrap[3] = {GL_REPEAT, GL_REPEAT, GL_REPEAT};
    float aniso = 1, bias = 0, min_lod = 0, max_lod = 1000;
    float border[4] = {};
    GLint compare = 0;  // GL_LEQUAL and the like for a shadow map, 0 without
};

SamplerDesc StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    SamplerDesc sd;
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    bool mip_linear = mip == kTexfLinear;
    sd.mag = lin(mag) ? GL_LINEAR : GL_NEAREST;
    if (lin(min)) sd.min = mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
    else sd.min = mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    sd.aniso = float(std::min<uint32_t>(an, 16));
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.wrap[0] = GLint(Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]));
    sd.wrap[1] = GLint(Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]));
    sd.wrap[2] = GLint(Address(t[kTssAddressW]));
    sd.bias = Bits(t[kTssMipLodBias]);
    sd.min_lod = float(t[kTssMaxMipLevel]);
    sd.max_lod = mip ? 1000.0f : sd.min_lod;
    Color(t[kTssBorderColor], sd.border);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, GL tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.min = sd.mag = lin(min) || lin(mag) ? GL_LINEAR : GL_NEAREST;
        sd.compare = GLint(GL_NEVER + swap[CmpIndex(s.rs[kRsShadowFunc])]);
        sd.aniso = 1;
    }
    return sd;
}

// a DXGI vertex format as glVertexAttrib(I)Format takes it
struct AttribFormat {
    GLint size;
    GLenum type;
    GLboolean normalized;
    bool integer;
};

bool VertexFormat(uint32_t f, AttribFormat& o) {
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT: o = {1, GL_FLOAT, GL_FALSE, false}; return true;
        case DXGI_FORMAT_R32G32_FLOAT: o = {2, GL_FLOAT, GL_FALSE, false}; return true;
        case DXGI_FORMAT_R32G32B32_FLOAT: o = {3, GL_FLOAT, GL_FALSE, false}; return true;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: o = {4, GL_FLOAT, GL_FALSE, false}; return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM: o = {GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R16_SNORM: o = {1, GL_SHORT, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R16G16_SNORM: o = {2, GL_SHORT, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R16G16B16A16_SNORM: o = {4, GL_SHORT, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R8_UNORM: o = {1, GL_UNSIGNED_BYTE, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R8G8_UNORM: o = {2, GL_UNSIGNED_BYTE, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM: o = {4, GL_UNSIGNED_BYTE, GL_TRUE, false}; return true;
        case DXGI_FORMAT_R16_SINT: o = {1, GL_SHORT, GL_FALSE, true}; return true;
        case DXGI_FORMAT_R16G16_SINT: o = {2, GL_SHORT, GL_FALSE, true}; return true;
        case DXGI_FORMAT_R16G16B16A16_SINT: o = {4, GL_SHORT, GL_FALSE, true}; return true;
        case DXGI_FORMAT_R32_UINT: o = {1, GL_UNSIGNED_INT, GL_FALSE, true}; return true;
        default: return false;
    }
}

GLenum PrimitiveMode(const Assembled& a) {
    if (a.topology == Topology::Points) return GL_POINTS;
    if (a.topology == Topology::Lines) return a.strip ? GL_LINE_STRIP : GL_LINES;
    return a.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
}

class GlBackend final : public Backend {
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
        Current();
        gl.Flush();
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
        GLintptr off = 0;
    };

    void Current();
    bool CreateRing();
    Alloc Upload(size_t size, size_t align);
    GLuint CompileStage(GLenum stage, const std::string& glsl);
    Shader CompileHlslShader(const std::string& hlsl, const char* entry, vk::Stage stage);
    GLuint LinkProgram(GLuint vs, GLuint ps);
    Shader TranslateShader(const std::string& hlsl, vk::Stage stage);
    bool Ready(Shader& sh);
    void LoadWarm();
    void KeepShader(const Shader& sh);
    void KeepProgram(uint64_t bkey, const Shader& vs, const Shader& ps);
    GLuint WarmShader(uint64_t hash);
    void Warm(double budget_ms);
    void StartLinker();
    void Linker();
    void CreateBlit();
    void CreateGammaLut();
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    GLuint Sampler(const SamplerDesc& d);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void BindFramebuffer(Target* rt, Target* ds);
    bool BindTargets(const State& s);
    void UploadTarget(Target& t, const Surface& s);
    GLuint FeedbackCopy(Target& t);
    void Blit(GLuint src, const float uvrect[4], bool gamma, bool alpha, const float* colour);

    // textures
    GLuint BindTexture(const State& s, int stage, DrawTextures& stages);
    GLuint BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    GLuint BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p);
    void ReleaseOldTextures();

    // a draw
    GLuint EnsureProgram(const State& s, const Assembled& a, DrawShaders& sh, const PixelKey& pk,
                         const int8_t*& locations);
    void ApplyState(const State& s, const Assembled& a);
    void SetViewportScissor(const State& s, uint32_t gw);
    void UploadConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, GuestSize size);
    void BindStages(const State& s, const Assembled& a, const DrawTextures& stages);
    bool SetVertexInputs(const Assembled& a, const int8_t* locations);
    void DrawGeometry(const Assembled& a);
    void ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                     uint32_t color, float z, uint32_t stencil);
    void CountFps();
    void SaveShot(Target& t);

    SDL_Window* window_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
    std::thread::id current_;
    uint32_t guest_w_ = 640, guest_h_ = 480;

    GLuint ring_ = 0;
    uint8_t* ring_map_ = nullptr;
    size_t ring_off_ = 0;
    uint32_t segment_ = 0;
    GLsync fences_[kSegments] = {};
    GLint ubo_align_ = 256;
    GLuint vao_ = 0;
    uint32_t attribs_ = 0;  // enabled vertex attribute arrays, bit per location

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, GLuint> fbos_;  // by colour texture << 32 | depth texture
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the targets as the draw's textures bind
    Target* draw_ds_ = nullptr;
    GLuint read_fbo_ = 0;
    GLuint upload_fbo_ = 0;

    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint64_t, GLuint> samplers_;
    std::unordered_map<uint64_t, Shader> shaders_;   // vertex keys, pixel keys salted
    std::unordered_map<uint64_t, GLuint> programs_;  // by both keys
    // async shaders: programs link on the linker thread in a context shared with ctx_ (mesa's link of a new effect
    // took ~300 ms on the render thread, a freeze on each first shot); the draw is skipped until it is done
    struct LinkJob {
        uint64_t key;
        GLuint vs, ps;
    };
    void* link_ctx_ = nullptr;  // the linker's EGLContext
    bool linker_ = false;       // the linker thread runs
    std::mutex link_lock_;
    std::condition_variable link_cv_;
    std::vector<LinkJob> link_jobs_;                      // under link_lock_
    std::vector<std::pair<uint64_t, GLuint>> linked_;     // under link_lock_
    std::unordered_set<uint64_t> linking_;                // the render thread's
    std::unordered_set<uint64_t> failed_;                 // failed on the linker: linked here again
    // the switch mesa keeps no program binaries and no disk cache: the GLSL of every program linked is kept on the
    // SD card instead (shaders.lst) and the next start links those programs again while the render thread idles
    // (loading screens, menus), before a draw needs them
    std::unordered_map<uint64_t, std::pair<GLenum, std::string>> warm_glsl_;  // shaders read, by hash
    std::unordered_map<uint64_t, GLuint> warm_shaders_;                      // compiled from it, by hash
    std::vector<std::pair<uint64_t, uint64_t>> warm_queue_;                  // programs to link, by shader hashes
    size_t warm_next_ = 0;
    std::unordered_map<uint64_t, GLuint> warm_;    // linked ahead, by bkey
    std::unordered_set<uint64_t> kept_;            // shader hashes and bkeys in the file
    FILE* warm_file_ = nullptr;
    uint64_t warm_draws_ = 0;                      // draws_ at the last present
    Assembled asm_;

    GLuint blit_prog_ = 0;
    GLuint blit_linear_ = 0, blit_point_ = 0;
    GLuint lut_ = 0;
    bool gamma_ = false;
    uint32_t ramp_set_ = ~0u;
    std::array<uint8_t, 768> game_ramp_{};
    bool has_game_ramp_ = false;

    // NOOVERWRITE locks: each surface read back into a ring of persistently mapped buffers, the newest the GPU has
    // finished handed to the title without waiting (gpu/d3d12.cpp does the same)
    static constexpr int kLateSlots = 3;
    struct Late {
        GLuint pbo[kLateSlots] = {};
        const uint8_t* map[kLateSlots] = {};
        GLsync fence[kLateSlots] = {};  // set while the GPU still writes the slot
        bool done[kLateSlots] = {};
        uint64_t seq[kLateSlots] = {};
        uint64_t frame[kLateSlots] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0;
        uint32_t next = 0;
    };
    void ResetLate(Late& l);
    GLuint LateProgram();
    void PollLate(Late& l);
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;  // DeliverLate runs on the title's thread
    uint64_t late_seq_ = 0;
    // a compute copy into the slot: nouveau's ReadPixels into a buffer waits for the GPU
    GLuint late_prog_ = 0;
    bool late_prog_tried_ = false;
    std::atomic<uint64_t> presents_seen_{0};

    uint64_t presents_ = 0;
    uint32_t fps_n_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t draws_ = 0, last_draws_ = 0, last_presents_ = 0;
    std::chrono::steady_clock::time_point log_t_ = std::chrono::steady_clock::now();
    uint64_t log_cpu_ = 0;  // the render thread's CPU time at log_t_

    // where the render thread's time goes, by phase, logged with the frame rate
    enum Phase { kAssemble, kTextures, kTexUpload, kProgram, kCompile, kConstants, kStages, kState, kVertices,
                 kDraw, kRingWait, kClear, kCopy, kSync, kPresent, kSwap, kTargets, kKeys, kDrawAll, kPhases };
    uint64_t phase_ns_[kPhases] = {};
    uint64_t tex_uploads_ = 0, tex_upload_bytes_ = 0, compiles_ = 0;
    struct Timed {
        uint64_t& into;
        std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
        ~Timed() {
            into += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t)
                                 .count());
        }
    };
    Timed Time(Phase p) { return Timed{phase_ns_[p]}; }
    bool vsync_set_ = false, vsync_ = true;
};

// the context follows the calls: Init runs on the title's thread, everything after on the render thread
void GlBackend::Current() {
    if (current_ == std::this_thread::get_id()) return;
    if (SDL_GL_MakeCurrent(window_, ctx_) != 0) LOGE("gl: context not made current: %s", SDL_GetError());
    current_ = std::this_thread::get_id();
}

bool GlBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    window_ = static_cast<SDL_Window*>(app::Window());
    if (!window_) {
        LOGE("gl: no window");
        return false;
    }
    ctx_ = SDL_GL_CreateContext(window_);
    if (!ctx_) {
        LOGE("gl: no OpenGL 4.3 context: %s", SDL_GetError());
        return false;
    }
    current_ = std::this_thread::get_id();
    if (!LoadGl()) return false;
    LOGI("gl: %s, %s, %s", reinterpret_cast<const char*>(gl.GetString(GL_VERSION)),
         reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)), reinterpret_cast<const char*>(gl.GetString(GL_VENDOR)));
    gl.ClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align_);
    if (!CreateRing()) return false;
    gl.GenVertexArrays(1, &vao_);
    gl.BindVertexArray(vao_);
    // the index buffer binding belongs to the vertex array
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring_);
    gl.GenFramebuffers(1, &read_fbo_);
    gl.GenFramebuffers(1, &upload_fbo_);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.Enable(GL_SCISSOR_TEST);
    CreateBlit();
    CreateGammaLut();
    if (!blit_prog_) return false;
    LoadWarm();
    StartLinker();
    LOGI("gl: renderer up, the title draws at %ux%u", width, height);
    port::Breadcrumb("gl renderer up");
    // the render thread takes the context over
    SDL_GL_MakeCurrent(window_, nullptr);
    current_ = {};
    return true;
}

bool GlBackend::CreateRing() {
    gl.GenBuffers(1, &ring_);
    gl.BindBuffer(GL_ARRAY_BUFFER, ring_);
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    gl.BufferStorage(GL_ARRAY_BUFFER, GLsizeiptr(kRingSize), nullptr, flags);
    ring_map_ = static_cast<uint8_t*>(gl.MapBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(kRingSize), flags));
    if (!ring_map_) {
        LOGE("gl: the upload ring could not be mapped (0x%X)", gl.GetError());
        return false;
    }
    return true;
}

GlBackend::Alloc GlBackend::Upload(size_t size, size_t align) {
    constexpr size_t kSegment = kRingSize / kSegments;
    size = std::min(size, kSegment);
    size_t off = (ring_off_ + align - 1) & ~(align - 1);
    size_t end = (segment_ + 1) * kSegment;
    if (off + size > end) {
        // the segment is done: fenced, and the next one waited for
        fences_[segment_] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        segment_ = (segment_ + 1) % kSegments;
        if (GLsync f = fences_[segment_]) {
            auto timed = Time(kRingWait);
            gl.ClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, ~0ull);
            gl.DeleteSync(f);
            fences_[segment_] = nullptr;
        }
        off = segment_ * kSegment;
    }
    ring_off_ = off + size;
    return {ring_map_ + off, GLintptr(off)};
}

GLuint GlBackend::CompileStage(GLenum stage, const std::string& glsl) {
    GLuint id = gl.CreateShader(stage);
    const char* text = glsl.c_str();
    gl.ShaderSource(id, 1, &text, nullptr);
    gl.CompileShader(id);
    GLint ok = 0;
    gl.GetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl.GetShaderInfoLog(id, sizeof(log), nullptr, log);
        LOGE("gl: %s shader: %s",
             stage == GL_VERTEX_SHADER ? "vertex" : stage == GL_COMPUTE_SHADER ? "compute" : "pixel", log);
        LOGE("%s", glsl.c_str());
        gl.DeleteShader(id);
        return 0;
    }
    return id;
}

Shader GlBackend::CompileHlslShader(const std::string& hlsl, const char* entry, vk::Stage stage) {
    auto timed = Time(kCompile);
    compiles_++;
    Shader sh;
    std::memset(sh.locations, -1, sizeof(sh.locations));
    gl::Glsl glsl;
    if (!gl::HlslToGlsl(hlsl, entry, stage, glsl)) return sh;
    sh.id = CompileStage(stage == vk::Stage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, glsl.src);
    std::memcpy(sh.locations, glsl.locations, sizeof(sh.locations));
    return sh;
}

GLuint GlBackend::LinkProgram(GLuint vs, GLuint ps) {
    if (!vs || !ps) return 0;
    auto t0 = std::chrono::steady_clock::now();
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, ps);
    gl.LinkProgram(p);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("gl: program link: %s", log);
        return 0;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 20.0) LOGI("gl: program %u linked in %.0f ms", p, ms);
    return p;
}

Shader GlBackend::TranslateShader(const std::string& hlsl, vk::Stage stage) {
    auto timed = Time(kCompile);
    Shader sh;
    std::memset(sh.locations, -1, sizeof(sh.locations));
    gl::Glsl glsl;
    if (!gl::HlslToGlsl(hlsl, "main", stage, glsl)) return sh;
    sh.stage = stage == vk::Stage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    sh.hash = Hash(glsl.src.data(), glsl.src.size(), sh.stage) | 1;
    sh.glsl = std::move(glsl.src);
    std::memcpy(sh.locations, glsl.locations, sizeof(sh.locations));
    return sh;
}

// compiles a draw's shader the first time a program needs it
bool GlBackend::Ready(Shader& sh) {
    if (!sh.id && !sh.glsl.empty()) {
        auto timed = Time(kCompile);
        KeepShader(sh);
        sh.id = WarmShader(sh.hash);
        if (!sh.id) {
            compiles_++;
            sh.id = CompileStage(sh.stage, sh.glsl);
            warm_shaders_[sh.hash] = sh.id;
            warm_glsl_.erase(sh.hash);
        }
        sh.glsl.clear();
        sh.glsl.shrink_to_fit();
        if (!sh.id) sh.hash = 0;
    }
    return sh.id != 0;
}

namespace {
constexpr char kWarmMagic[8] = {'S', 'C', 'D', 'A', 'G', 'L', '1', 0};
}

// shaders.lst: the magic, then records: 'S' hash, stage, length, GLSL; 'P' the two shaders' hashes
void GlBackend::LoadWarm() {
    std::string path = (port::HomeDir() / "shaders.lst").string();
    bool fresh = true;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char magic[8];
        if (std::fread(magic, 1, 8, f) == 8 && !std::memcmp(magic, kWarmMagic, 8)) {
            fresh = false;
            for (int tag; (tag = std::fgetc(f)) != EOF;) {
                if (tag == 'S') {
                    uint64_t hash;
                    uint32_t stage, n;
                    if (std::fread(&hash, 8, 1, f) != 1 || std::fread(&stage, 4, 1, f) != 1 ||
                        std::fread(&n, 4, 1, f) != 1 || n > (1u << 20)) break;
                    std::string text(n, '\0');
                    if (std::fread(text.data(), 1, n, f) != n) break;  // a record cut short: the rest is lost
                    warm_glsl_[hash] = {GLenum(stage), std::move(text)};
                    kept_.insert(hash);
                } else if (tag == 'P') {
                    uint64_t h[2];
                    if (std::fread(h, 8, 2, f) != 2) break;
                    uint64_t bkey = Hash(&h[1], sizeof(h[1]), h[0]);
                    if (kept_.insert(bkey).second) warm_queue_.push_back({h[0], h[1]});
                } else {
                    break;
                }
            }
        }
        std::fclose(f);
    }
    warm_file_ = std::fopen(path.c_str(), fresh ? "wb" : "ab");
    if (!warm_file_) {
        LOGW("gl: %s could not be written, programs are not kept", path.c_str());
        return;
    }
    if (fresh) {
        std::fwrite(kWarmMagic, 1, 8, warm_file_);
        std::fflush(warm_file_);
    }
    LOGI("gl: shader list %s, %zu programs to link ahead", path.c_str(), warm_queue_.size());
}

void GlBackend::KeepShader(const Shader& sh) {
    if (!warm_file_ || !sh.hash || sh.glsl.empty() || !kept_.insert(sh.hash).second) return;
    uint32_t stage = sh.stage, n = uint32_t(sh.glsl.size());
    std::fputc('S', warm_file_);
    std::fwrite(&sh.hash, 8, 1, warm_file_);
    std::fwrite(&stage, 4, 1, warm_file_);
    std::fwrite(&n, 4, 1, warm_file_);
    std::fwrite(sh.glsl.data(), 1, n, warm_file_);
    std::fflush(warm_file_);
}

void GlBackend::KeepProgram(uint64_t bkey, const Shader& vs, const Shader& ps) {
    if (!warm_file_ || !kept_.insert(bkey).second) return;
    uint64_t h[2] = {vs.hash, ps.hash};
    std::fputc('P', warm_file_);
    std::fwrite(h, 8, 2, warm_file_);
    std::fflush(warm_file_);
}

GLuint GlBackend::WarmShader(uint64_t hash) {
    auto it = warm_shaders_.find(hash);
    if (it != warm_shaders_.end()) return it->second;
    auto g = warm_glsl_.find(hash);
    if (g == warm_glsl_.end()) return 0;
    GLuint id = CompileStage(g->second.first, g->second.second);
    warm_glsl_.erase(g);
    warm_shaders_[hash] = id;
    return id;
}

// links kept programs until the budget is spent; the first draw that needs one finds it linked
void GlBackend::Warm(double budget_ms) {
    if (warm_next_ >= warm_queue_.size()) return;
    auto t0 = std::chrono::steady_clock::now();
    size_t done = 0;
    while (warm_next_ < warm_queue_.size()) {
        auto [vh, ph] = warm_queue_[warm_next_++];
        uint64_t bkey = Hash(&ph, sizeof(ph), vh);
        if (!warm_.count(bkey)) {
            GLuint vs = WarmShader(vh), ps = WarmShader(ph);
            if (GLuint p = LinkProgram(vs, ps)) warm_[bkey] = p;
            done++;
        }
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budget_ms)
            break;
    }
    if (warm_next_ >= warm_queue_.size()) {
        LOGI("gl: all %zu kept programs linked ahead", warm_queue_.size());
        warm_queue_.clear();
        warm_queue_.shrink_to_fit();
        warm_glsl_.clear();
    } else if (done) {
        LOGR("gl: linked %zu kept programs ahead, %zu to go", done, warm_queue_.size() - warm_next_);
    }
}

// the linker's context shares ctx_'s objects and binds no surface (the window's is the render thread's alone); only
// on the Switch, where SDL's GL is EGL
void GlBackend::StartLinker() {
#ifdef SCDA_SWITCH
    // made with EGL itself, ctx_ as its share context: SDL's SHARE_WITH_CURRENT_CONTEXT gave one that shared
    // nothing (its programs got names the renderer already used, a garbled then black picture)
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext share = eglGetCurrentContext();
    EGLint id = 0, n = 0;
    EGLConfig config = nullptr;
    eglQueryContext(dpy, share, EGL_CONFIG_ID, &id);
    const EGLint want[] = {EGL_CONFIG_ID, id, EGL_NONE};
    eglChooseConfig(dpy, want, &config, 1, &n);
    eglBindAPI(EGL_OPENGL_API);
    const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = n ? eglCreateContext(dpy, config, share, attribs) : EGL_NO_CONTEXT;
    if (ctx == EGL_NO_CONTEXT) {
        LOGW("gl: no shared context, programs link on the render thread (0x%X)", unsigned(eglGetError()));
        return;
    }
    link_ctx_ = ctx;
    std::promise<bool> up;
    std::future<bool> ok = up.get_future();
    GLuint probe = blit_prog_;
    std::thread([this, dpy, probe, &up] {
        port::NameThread("gl linker");
        eglBindAPI(EGL_OPENGL_API);
        bool current = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGLContext(link_ctx_));
        if (!current) LOGW("gl: the linker's context not made current (0x%X)", unsigned(eglGetError()));
        // the renderer's blit program must be known here, else the contexts share no objects
        bool shared = current && gl.IsProgram(probe);
        if (current && !shared) LOGW("gl: the linker's context shares no objects, programs link on the render thread");
        up.set_value(shared);
        if (shared) Linker();
        else if (current) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }).detach();
    linker_ = ok.get();
    if (linker_) LOGI("gl: programs link on the linker thread");
#endif
}

void GlBackend::Linker() {
    for (;;) {
        LinkJob job;
        {
            std::unique_lock lock(link_lock_);
            link_cv_.wait(lock, [this] { return !link_jobs_.empty(); });
            job = link_jobs_.front();
            link_jobs_.erase(link_jobs_.begin());
        }
        GLuint p = LinkProgram(job.vs, job.ps);
        std::lock_guard lock(link_lock_);
        linked_.emplace_back(job.key, p);
    }
}

void GlBackend::CreateBlit() {
    Shader vs = CompileHlslShader(kBlitHlsl, "vs", vk::Stage::Vertex);
    Shader ps = CompileHlslShader(kBlitHlsl, "psmain", vk::Stage::Pixel);
    blit_prog_ = LinkProgram(vs.id, ps.id);
    SamplerDesc lin;
    lin.min = GL_LINEAR;
    lin.mag = GL_LINEAR;
    lin.wrap[0] = lin.wrap[1] = lin.wrap[2] = GL_CLAMP_TO_EDGE;
    blit_linear_ = Sampler(lin);
    SamplerDesc pt = lin;
    pt.min = pt.mag = GL_NEAREST;
    blit_point_ = Sampler(pt);
}

// gamma ramp as a 256 x 1 lookup
void GlBackend::CreateGammaLut() {
    gl.GenTextures(1, &lut_);
    gl.BindTexture(GL_TEXTURE_2D, lut_);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
}

void GlBackend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    ramp_set_ = brightness << 8 | contrast;
    uint8_t ramp[256 * 4];
    gamma_ = GammaRamp(brightness, contrast, has_game_ramp_ ? game_ramp_.data() : nullptr, ramp);
    gl.BindTexture(GL_TEXTURE_2D, lut_);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_BGRA, GL_UNSIGNED_BYTE, ramp);
}

GLuint GlBackend::Sampler(const SamplerDesc& d) {
    uint64_t key = Hash(&d, sizeof(d));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    GLuint s;
    gl.GenSamplers(1, &s);
    gl.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, d.min);
    gl.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, d.mag);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_S, d.wrap[0]);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_T, d.wrap[1]);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_R, d.wrap[2]);
    gl.SamplerParameterf(s, GL_TEXTURE_LOD_BIAS, d.bias);
    gl.SamplerParameterf(s, GL_TEXTURE_MIN_LOD, d.min_lod);
    gl.SamplerParameterf(s, GL_TEXTURE_MAX_LOD, d.max_lod);
    gl.SamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, d.border);
    if (d.aniso > 1) gl.SamplerParameterf(s, kMaxAnisotropy, d.aniso);
    if (d.compare) {
        gl.SamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        gl.SamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, d.compare);
    }
    samplers_[key] = s;
    return s;
}

// targets

void GlBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    Target& t = it->second;
    if (bound_rt_ == &t || bound_ds_ == &t) bound_rt_ = bound_ds_ = nullptr;
    for (auto f = fbos_.begin(); f != fbos_.end();) {
        if (uint32_t(f->first >> 32) == t.tex || uint32_t(f->first) == t.tex) {
            gl.DeleteFramebuffers(1, &f->second);
            f = fbos_.erase(f);
        } else {
            ++f;
        }
    }
    gl.DeleteTextures(1, &t.tex);
    if (t.fb) gl.DeleteTextures(1, &t.fb);
    targets_.erase(it);
}

Target* GlBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header || (!depth && IsShadowColour(s))) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    auto it = targets_.find(TargetKey(data, depth));
    if (it == targets_.end()) return nullptr;
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    return it->second.gw == gw && it->second.gh == gh ? &it->second : nullptr;
}

Target* GlBackend::EnsureTarget(const Surface& s, bool depth) {
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
    gl.GenTextures(1, &t.tex);
    gl.BindTexture(GL_TEXTURE_2D, t.tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    if (depth) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, GLsizei(t.w), GLsizei(t.h), 0, GL_DEPTH_STENCIL,
                      GL_UNSIGNED_INT_24_8, nullptr);
    } else {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.w), GLsizei(t.h), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key] = std::move(t);
    // a new target starts cleared, as d3d12.cpp's
    BindFramebuffer(depth ? nullptr : &ref, depth ? &ref : nullptr);
    gl.Disable(GL_SCISSOR_TEST);
    if (depth) {
        gl.DepthMask(GL_TRUE);
        gl.StencilMask(0xFF);
        gl.ClearDepth(1.0);
        gl.ClearStencil(0);
        gl.Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    } else {
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.ClearColor(0, 0, 0, 0);
        gl.Clear(GL_COLOR_BUFFER_BIT);
    }
    gl.Enable(GL_SCISSOR_TEST);
    return &ref;
}

void GlBackend::BindFramebuffer(Target* rt, Target* ds) {
    uint64_t key = uint64_t(rt ? rt->tex : 0) << 32 | (ds ? ds->tex : 0);
    auto it = fbos_.find(key);
    if (it != fbos_.end()) {
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, it->second);
    } else {
        GLuint fbo;
        gl.GenFramebuffers(1, &fbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        if (rt) gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
        if (ds) gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds->tex, 0);
        gl.DrawBuffer(rt ? GL_COLOR_ATTACHMENT0 : GL_NONE);
        GLenum st = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) LOGE("gl: framebuffer incomplete (0x%X)", st);
        fbos_[key] = fbo;
    }
    bound_rt_ = rt;
    bound_ds_ = ds;
}

bool GlBackend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
    BindFramebuffer(rt, ds);
    return true;
}

// CPU pixels of a colour surface into its host target; the target is the console's size, so all of them
void GlBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth || Format(d.format).host != Host::BGRA8) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    uint32_t w = std::min(d.width, t.w), h = std::min(d.height, t.h);
    gl.BindTexture(GL_TEXTURE_2D, t.tex);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / 4));
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(w), GLsizei(h), GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (!t.read.empty() && d.width == t.gw && d.height == t.gh && pitch == t.gw * 4) t.read = px;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// a copy of a colour target for a draw that reads the target it draws to
GLuint GlBackend::FeedbackCopy(Target& t) {
    if (!t.fb) {
        gl.GenTextures(1, &t.fb);
        gl.BindTexture(GL_TEXTURE_2D, t.fb);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.w), GLsizei(t.h), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    }
    gl.CopyImageSubData(t.tex, GL_TEXTURE_2D, 0, 0, 0, 0, t.fb, GL_TEXTURE_2D, 0, 0, 0, 0, GLsizei(t.w),
                        GLsizei(t.h), 1);
    return t.fb;
}

// the full screen triangle over the bound framebuffer's viewport; the caller sets it and the clip control
void GlBackend::Blit(GLuint src, const float uvrect[4], bool gamma, bool alpha, const float* colour) {
    float c[20] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], gamma ? 1.0f : 0.0f, alpha ? 1.0f : 0.0f, 1, 1};
    if (colour) {
        for (int row = 0; row < 3; row++) std::memcpy(c + 8 + row * 4, colour + row * 3, 3 * sizeof(float));
        c[11] = 1.0f;
    }
    Alloc cb = Upload(sizeof(c), size_t(ubo_align_));
    std::memcpy(cb.cpu, c, sizeof(c));
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 1, ring_, cb.off, sizeof(c));
    gl.UseProgram(blit_prog_);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src);
    gl.BindSampler(0, blit_linear_);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, lut_);
    gl.BindSampler(1, blit_point_);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_POLYGON_OFFSET_FILL);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (uint32_t i = 0; i < 16; i++) {
        if (attribs_ >> i & 1) gl.DisableVertexAttribArray(i);
    }
    attribs_ = 0;
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

// textures

GLenum KindTarget(uint8_t kind) {
    if (kind == kTexCube) return GL_TEXTURE_CUBE_MAP;
    if (kind == kTex3D) return GL_TEXTURE_3D;
    return GL_TEXTURE_2D;
}

GLuint GlBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
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
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (&t == draw_rt_) return FeedbackCopy(t);
    return t.tex;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.tex && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

bool GlBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    if (tx.tex) gl.DeleteTextures(1, &tx.tex);
    tx = Tex{};
    tx.d = d;
    tx.levels = d.levels;
    // DXT that is not whole blocks: one level padded to them, as d3d12.cpp
    if (fi.compressed && ((d.width & 3) || (d.height & 3))) tx.levels = 1;
    tx.target = KindTarget(TextureKind(d));
    gl.GenTextures(1, &tx.tex);
    gl.BindTexture(tx.target, tx.tex);
    gl.TexParameteri(tx.target, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(tx.target, GL_TEXTURE_MAX_LEVEL, GLint(tx.levels - 1));
    return tx.tex != 0;
}

void GlBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p) {
    auto timed = Time(kTexUpload);
    tex_uploads_++;
    tex_upload_bytes_ += TotalSize(d);
    gl.BindTexture(tx.target, tx.tex);
    uint32_t faces = d.cube ? 6 : 1;
    bool bc = fi.compressed;
    GLenum bc_fmt = fi.host == Host::BC1 ? kDxt1 : fi.host == Host::BC2 ? kDxt3 : kDxt5;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        GLenum face = d.cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : tx.target;
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, nullptr, px, pitch);
            if (d.cube && !l) CheckNormCube(tx.norm, d, fi, f, px, pitch);
            uint32_t w = std::max(d.width >> l, 1u), h = std::max(d.height >> l, 1u);
            uint32_t z = std::max(d.depth >> l, 1u);
            if (d.linear) {
                w = d.width;
                h = d.height;
                z = 1;
            }
            if (bc) {
                size_t bytes = size_t((w + 3) / 4) * ((h + 3) / 4) * block * z;
                if (tx.levels == 1 && l == 0) {
                    // padded to whole blocks
                    w = (w + 3) & ~3u;
                    h = (h + 3) & ~3u;
                }
                px.resize(bytes);
                if (tx.target == GL_TEXTURE_3D) {
                    gl.CompressedTexImage3D(face, GLint(l), bc_fmt, GLsizei(w), GLsizei(h), GLsizei(z), 0,
                                            GLsizei(bytes), px.data());
                } else {
                    gl.CompressedTexImage2D(face, GLint(l), bc_fmt, GLsizei(w), GLsizei(h), 0, GLsizei(bytes),
                                            px.data());
                }
                continue;
            }
            bool depth = fi.host == Host::R32F;
            GLint internal = depth ? GL_R32F : GL_RGBA8;
            GLenum format = depth ? GL_RED : GL_BGRA;
            GLenum type = depth ? GL_FLOAT : GL_UNSIGNED_BYTE;
            px.resize(size_t(pitch) * h * z);
            gl.PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / 4));
            if (tx.target == GL_TEXTURE_3D) {
                gl.TexImage3D(face, GLint(l), internal, GLsizei(w), GLsizei(h), GLsizei(z), 0, format, type,
                              px.data());
            } else {
                gl.TexImage2D(face, GLint(l), internal, GLsizei(w), GLsizei(h), 0, format, type, px.data());
            }
        }
    }
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

GLuint GlBackend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (fi.host == Host::None) return 0;
    Tex& tx = textures_[d.data];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(d.data);
        return 0;
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
    return tx.tex;
}

GLuint GlBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return 0;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    stages.target[stage] = KindTarget(info.kind);
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(stage, d, fi, stages);
}

void GlBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 1024;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            gl.DeleteTextures(1, &it->second.tex);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

// a draw

GLuint GlBackend::EnsureProgram(const State& s, const Assembled& a, DrawShaders& sh, const PixelKey& pk,
                                const int8_t*& locations) {
    auto vit = shaders_.find(sh.vkey);
    if (vit == shaders_.end()) {
        vit = shaders_.emplace(sh.vkey, TranslateShader(VertexHlsl(s, a, sh), vk::Stage::Vertex)).first;
    }
    locations = vit->second.locations;
    uint64_t pk_salted = sh.pkey ^ kPixelKeySalt;
    auto pit = shaders_.find(pk_salted);
    if (pit == shaders_.end()) {
        pit = shaders_.emplace(pk_salted, TranslateShader(PixelHlsl(pk), vk::Stage::Pixel)).first;
    }
    uint64_t key = Hash(&pk_salted, sizeof(pk_salted), sh.vkey);
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second;
    Shader& vs = vit->second;
    Shader& ps = pit->second;
    uint64_t bkey = Hash(&ps.hash, sizeof(ps.hash), vs.hash);
    if (vs.hash && ps.hash) {
        auto w = warm_.find(bkey);
        if (w != warm_.end()) {
            programs_[key] = w->second;
            warm_.erase(w);
            return programs_[key];
        }
    }
    if (!Ready(vs) || !Ready(ps)) {
        programs_[key] = 0;
        return 0;
    }
    if (linker_ && app::Get().async_shaders && !failed_.count(key)) {
        {
            std::lock_guard lock(link_lock_);
            for (auto& [k, p] : linked_) {
                if (p) programs_[k] = p;
                else failed_.insert(k);
                linking_.erase(k);
            }
            linked_.clear();
            if (!programs_.count(key) && linking_.insert(key).second) {
                link_jobs_.push_back({key, vit->second.id, pit->second.id});
                link_cv_.notify_one();
            }
        }
        it = programs_.find(key);
        return it != programs_.end() ? it->second : 0;
    }
    GLuint p = LinkProgram(vs.id, ps.id);
    programs_[key] = p;
    if (p) KeepProgram(bkey, vs, ps);
    return p;
}

void GlBackend::ApplyState(const State& s, const Assembled& a) {
    // blend
    if (SimpleState(s, kMBlendEnable)) {
        gl.Enable(GL_BLEND);
        GLenum src = Blend(SimpleState(s, kMSrcBlend));
        GLenum dst = Blend(SimpleState(s, kMDestBlend));
        gl.BlendFuncSeparate(src, dst, src, dst);
        gl.BlendEquation(BlendOp(SimpleState(s, kMBlendOp)));
        float bf[4];
        Color(SimpleState(s, kMBlendColor), bf);
        gl.BlendColor(bf[0], bf[1], bf[2], bf[3]);
    } else {
        gl.Disable(GL_BLEND);
    }
    // the console's enables are one per byte, A R G B
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    gl.ColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);

    // depth and stencil
    bool zen = bound_ds_ && s.rs[kRsZEnable];
    if (zen) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(Cmp(SimpleState(s, kMZFunc)));
        gl.DepthMask(SimpleState(s, kMZWriteEnable) ? GL_TRUE : GL_FALSE);
    } else {
        gl.Disable(GL_DEPTH_TEST);
    }
    if (bound_ds_ && s.rs[kRsStencilEnable]) {
        gl.Enable(GL_STENCIL_TEST);
        gl.StencilFunc(Cmp(SimpleState(s, kMStencilFunc)), GLint(SimpleState(s, kMStencilRef) & 0xFF),
                       SimpleState(s, kMStencilMask) & 0xFF);
        gl.StencilOp(StencilOp(s.rs[kRsStencilFail]), StencilOp(SimpleState(s, kMStencilZFail)),
                     StencilOp(SimpleState(s, kMStencilPass)));
        gl.StencilMask(SimpleState(s, kMStencilWriteMask) & 0xFF);
    } else {
        gl.Disable(GL_STENCIL_TEST);
    }
    // polygon offset in depth buffer units, the target is always 24 bit
    if (bound_ds_ && a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        gl.Enable(GL_POLYGON_OFFSET_FILL);
        gl.Enable(GL_POLYGON_OFFSET_LINE);
        gl.PolygonOffset(Bits(SimpleState(s, kMSlopeScale)),
                         float(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale)));
    } else {
        gl.Disable(GL_POLYGON_OFFSET_FILL);
        gl.Disable(GL_POLYGON_OFFSET_LINE);
    }

    // rasterizer: GL_UPPER_LEFT flips the facing with the image, so D3D's windings carry over as they are
    uint32_t cull = s.rs[kRsCullMode];
    if (cull && a.topology == Topology::Triangles) {
        gl.Enable(GL_CULL_FACE);
        gl.CullFace(GL_BACK);
        // culling clockwise leaves counter clockwise as the front
        gl.FrontFace(cull == kCullCw ? GL_CCW : GL_CW);
    } else {
        gl.Disable(GL_CULL_FACE);
    }
    gl.PolygonMode(GL_FRONT_AND_BACK, s.rs[kRsFillMode] == kFillWireframe ? GL_LINE : GL_FILL);
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void GlBackend::SetViewportScissor(const State& s, uint32_t gw) {
    int W = int(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    int H = int(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    gl.Viewport(0, 0, W, H);
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
    gl.Scissor(x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0));
}

void GlBackend::UploadConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, GuestSize size) {
    Alloc vc = Upload(sizeof(s.vsc), size_t(ubo_align_));
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
    Alloc dc = Upload(sizeof(c), size_t(ubo_align_));
    std::memcpy(dc.cpu, &c, sizeof(c));
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 0, ring_, vc.off, sizeof(s.vsc));
    gl.BindBufferRange(GL_UNIFORM_BUFFER, 1, ring_, dc.off, sizeof(c));
}

void GlBackend::BindStages(const State& s, const Assembled& a, const DrawTextures& stages) {
    uint32_t aniso = app::Get().aniso;
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    for (int i = 0; i < 4; i++) {
        gl.ActiveTexture(GL_TEXTURE0 + i);
        gl.BindTexture(stages.target[i] ? stages.target[i] : KindTarget(stages.info[i].kind), stages.tex[i]);
        gl.BindSampler(GLuint(i), Sampler(StageSampler(s, i, stages, clamp, aniso)));
    }
}

uint32_t FormatBytes(uint32_t f) {
    AttribFormat o;
    if (!VertexFormat(f, o)) return 16;
    uint32_t n = o.size == GL_BGRA ? 4 : uint32_t(o.size);
    uint32_t each = o.type == GL_FLOAT || o.type == GL_UNSIGNED_INT ? 4 : o.type == GL_SHORT ? 2 : 1;
    return n * each;
}

bool GlBackend::SetVertexInputs(const Assembled& a, const int8_t* locations) {
    // GL drivers read what the draw addresses straight from the buffer (Mesa's vertex fetch fallbacks on the CPU
    // too), where D3D12 gives 0 past its end: every buffer reaches the last vertex read, zeros after the guest's
    uint32_t end[16] = {};
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        end[e.buffer] = std::max(end[e.buffer], uint32_t(e.offset) + FormatBytes(e.format));
    }
    uint64_t last = MaxVertex(a);
    GLintptr offs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        uint64_t need = std::max<uint64_t>(b.bytes, last * b.stride + std::max(end[i], b.stride));
        if (need > b.bytes) LOG_FIRST(8, "gl: a draw reads %llu bytes of a %u byte vertex buffer, zeros after",
                                      (unsigned long long)need, b.bytes);
        if (need > kRingSize / kSegments) {
            LOG_FIRST(8, "gl: %llu bytes of vertices in one draw, left out", (unsigned long long)need);
            return false;
        }
        Alloc vb = Upload(size_t(need), 16);
        if (b.src) std::memcpy(vb.cpu, b.src, b.bytes);
        if (need > b.bytes) std::memset(vb.cpu + b.bytes, 0, size_t(need - b.bytes));
        offs[i] = vb.off;
    }
    uint32_t want = 0;
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1) || locations[r] < 0) continue;
        const VertexElement& e = a.elements[size_t(r)];
        AttribFormat f;
        if (!VertexFormat(e.format, f)) {
            LOG_FIRST(8, "gl: vertex format %u not handled", e.format);
            continue;
        }
        GLuint loc = GLuint(locations[r]);
        want |= 1u << loc;
        if (f.integer) gl.VertexAttribIFormat(loc, f.size, f.type, e.offset);
        else gl.VertexAttribFormat(loc, f.size, f.type, f.normalized, e.offset);
        gl.VertexAttribBinding(loc, e.buffer);
    }
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        gl.BindVertexBuffer(i, ring_, offs[i], GLsizei(a.buffers[i].stride));
    }
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t bit = 1u << i;
        if ((want & bit) && !(attribs_ & bit)) gl.EnableVertexAttribArray(i);
        if (!(want & bit) && (attribs_ & bit)) gl.DisableVertexAttribArray(i);
    }
    attribs_ = want;
    return true;
}

void GlBackend::DrawGeometry(const Assembled& a) {
    GLenum mode = PrimitiveMode(a);
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        gl.DrawElementsBaseVertex(mode, GLsizei(a.index_count), a.index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                  reinterpret_cast<const void*>(ib.off), a.base_vertex);
    } else {
        gl.DrawArrays(mode, 0, GLsizei(a.draw_count));
    }
}

void GlBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    auto all = Time(kDrawAll);
    Current();
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
        for (int i = 0; i < 4; i++) stages.tex[i] = BindTexture(s, i, stages);
        // uploads and copies may have bound other framebuffers
        BindFramebuffer(draw_rt_, draw_ds_);
    }
    PixelKey pk;
    {
        auto timed = Time(kKeys);
        pk = MakePixelKey(s, stages.info);
        pk.quad = sh.quad;
        sh.pkey = Hash(&pk, sizeof(pk));
    }

    const int8_t* locations = nullptr;
    GLuint prog;
    {
        auto timed = Time(kProgram);
        prog = EnsureProgram(s, a, sh, pk, locations);
        if (!prog) return;
        gl.UseProgram(prog);
    }

    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    {
        auto timed = Time(kConstants);
        UploadConstants(s, sh, stages, size);
    }
    {
        auto timed = Time(kStages);
        BindStages(s, a, stages);
    }
    {
        auto timed = Time(kState);
        ApplyState(s, a);
        SetViewportScissor(s, size.w);
    }
    bool inputs;
    {
        auto timed = Time(kVertices);
        inputs = SetVertexInputs(a, locations);
    }
    if (inputs) {
        auto timed = Time(kDraw);
        DrawGeometry(a);
    }
}

void GlBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                            uint32_t color, float z, uint32_t stencil) {
    std::vector<std::array<int, 4>> rs;
    float sc = float(t.w) / t.gw;
    auto add = [&](int x0, int y0, int x1, int y1) {
        rs.push_back({std::clamp(int(x0 * sc), 0, int(t.w)), std::clamp(int(y0 * sc), 0, int(t.h)),
                      std::clamp(int(x1 * sc), 0, int(t.w)), std::clamp(int(y1 * sc), 0, int(t.h))});
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
    GLbitfield mask = 0;
    if (t.depth) {
        BindFramebuffer(nullptr, &t);
        if (flags & kClearZ) {
            mask |= GL_DEPTH_BUFFER_BIT;
            gl.DepthMask(GL_TRUE);
            gl.ClearDepth(std::clamp(z, 0.0f, 1.0f));
        }
        if (flags & kClearStencil) {
            mask |= GL_STENCIL_BUFFER_BIT;
            gl.StencilMask(0xFF);
            gl.ClearStencil(GLint(stencil & 0xFF));
        }
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        BindFramebuffer(&t, nullptr);
        float col[4];
        Color(color, col);
        gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.ClearColor(col[0], col[1], col[2], col[3]);
        mask = GL_COLOR_BUFFER_BIT;
    }
    for (const auto& r : rs) {
        if (r[2] <= r[0] || r[3] <= r[1]) continue;
        gl.Scissor(r[0], r[1], r[2] - r[0], r[3] - r[1]);
        gl.Clear(mask);
    }
    t.drawn = true;
}

void GlBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    Current();
    auto timed = Time(kClear);
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (rt) ClearTarget(*rt, s, count, rects, flags, color, z, stencil);
    if (ds) ClearTarget(*ds, s, count, rects, flags, color, z, stencil);
}

void GlBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                     uint32_t dst_points) {
    Current();
    auto timed = Time(kCopy);
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "gl: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
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
        gl.CopyImageSubData(src_t->tex, GL_TEXTURE_2D, 0, x0, y0, 0, dst_t->tex, GL_TEXTURE_2D, 0, dx, dy, 0, x1 - x0,
                            y1 - y0, 1);
    }
    dst_t->drawn = true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void GlBackend::Sync(uint32_t data, uint32_t) {
    Current();
    auto timed = Time(kSync);
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    std::vector<uint8_t> px(size_t(t.gw) * t.gh * 4);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo_);
    gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.ReadPixels(0, 0, GLsizei(t.gw), GLsizei(t.gh), GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void GlBackend::ResetLate(Late& l) {
    for (int i = 0; i < kLateSlots; i++) {
        if (l.fence[i]) gl.DeleteSync(l.fence[i]);
        if (l.pbo[i]) gl.DeleteBuffers(1, &l.pbo[i]);
    }
    l = Late{};
}

// packs the target as ReadPixels' GL_BGRA / GL_UNSIGNED_BYTE rows from the bottom; RGBA8 survives the float exactly
const char* kLateCopyGlsl = R"(#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D src;
layout(std430, binding = 0) writeonly buffer Dst { uint px[]; };
layout(location = 0) uniform uvec2 size;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= size.x || p.y >= size.y) return;
    uvec4 c = uvec4(round(clamp(texelFetch(src, ivec2(p), 0), 0.0, 1.0) * 255.0));
    px[p.y * size.x + p.x] = c.b | c.g << 8 | c.r << 16 | c.a << 24;
}
)";

GLuint GlBackend::LateProgram() {
    if (late_prog_tried_) return late_prog_;
    late_prog_tried_ = true;
    GLuint cs = CompileStage(GL_COMPUTE_SHADER, kLateCopyGlsl);
    if (!cs) return 0;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, cs);
    gl.LinkProgram(p);
    gl.DeleteShader(cs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("gl: late copy link: %s", log);
        return 0;
    }
    LOGI("gl: NOOVERWRITE read-backs copy by compute");
    late_prog_ = p;
    return p;
}

// render thread: slots the GPU has finished become deliverable
void GlBackend::PollLate(Late& l) {
    for (int i = 0; i < kLateSlots; i++) {
        if (!l.fence[i]) continue;
        GLenum r = gl.ClientWaitSync(l.fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) continue;
        gl.DeleteSync(l.fence[i]);
        l.fence[i] = nullptr;
        l.done[i] = true;
    }
}

bool GlBackend::DeliverLate(uint32_t data) {
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
        for (uint32_t y = 0; y < l.gh; y++) {
            std::memcpy(dst + size_t(y) * pitch, l.map[best] + size_t(y) * l.gw * 4, l.gw * 4);
        }
        l.delivered = l.seq[best];
    }
    return true;
}

void GlBackend::RecordLate(uint32_t data) {
    Current();
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
    PollLate(l);
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot]) return;
    GLsizeiptr bytes = GLsizeiptr(t.gw) * t.gh * 4;
    constexpr GLbitfield kFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    if (!l.pbo[slot]) {
        gl.GenBuffers(1, &l.pbo[slot]);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, l.pbo[slot]);
        gl.BufferStorage(GL_PIXEL_PACK_BUFFER, bytes, nullptr, kFlags | GL_CLIENT_STORAGE_BIT);
        l.map[slot] = static_cast<const uint8_t*>(gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, kFlags));
        if (!l.map[slot]) {
            LOGE("gl: a read-back buffer could not be mapped (0x%X)", gl.GetError());
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gl.DeleteBuffers(1, &l.pbo[slot]);
            l.pbo[slot] = 0;
            return;
        }
    } else {
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, l.pbo[slot]);
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (GLuint prog = LateProgram()) {
        // the draws bind their own program and textures
        gl.UseProgram(prog);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, t.tex);
        gl.BindSampler(0, blit_point_);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, l.pbo[slot]);
        gl.Uniform2ui(0, t.gw, t.gh);
        gl.DispatchCompute((t.gw + 7) / 8, (t.gh + 7) / 8, 1);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        gl.MemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    } else {
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, l.pbo[slot]);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo_);
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
        gl.ReadPixels(0, 0, GLsizei(t.gw), GLsizei(t.gh), GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    l.fence[slot] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    l.done[slot] = false;
    l.seq[slot] = ++late_seq_;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kLateSlots;
}

void GlBackend::CountFps() {
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
        LOGR("gl: %.1f fps, %llu draws a frame, %zu targets, %zu textures, %zu programs", double(app::g_fps),
             (unsigned long long)((draws_ - last_draws_) / frames), targets_.size(), textures_.size(),
             programs_.size());
        static const char* const kNames[kPhases] = {"assemble", "textures", "tex upload", "program", "compile",
                                                    "constants", "stages", "state", "vertices", "draw call",
                                                    "ring wait", "clear", "copy", "sync", "present", "swap",
                                                    "targets", "keys", "whole draws"};
        char line[512];
        int at = std::snprintf(line, sizeof(line), "gl: ms a frame:");
        for (int i = 0; i < kPhases && at < int(sizeof(line)); i++)
            at += std::snprintf(line + at, sizeof(line) - size_t(at), " %s %.2f", kNames[i],
                                double(phase_ns_[i]) / 1e6 / double(frames));
        LOGR("%s", line);
        uint64_t cpu = port::ThreadCpuNs();
        double wall = std::chrono::duration<double, std::milli>(now - log_t_).count();
        if (log_cpu_ && cpu)
            LOGR("gl: the render thread ran on a CPU %.0f ms of %.0f ms (%.0f%%)", double(cpu - log_cpu_) / 1e6, wall,
                 100.0 * double(cpu - log_cpu_) / 1e6 / wall);
        log_cpu_ = cpu;
        LOGR("gl: a frame: %.1f texture uploads (%.0f KB), %.2f shader compiles", double(tex_uploads_) / double(frames),
             double(tex_upload_bytes_) / 1024.0 / double(frames), double(compiles_) / double(frames));
        std::memset(phase_ns_, 0, sizeof(phase_ns_));
        tex_uploads_ = tex_upload_bytes_ = compiles_ = 0;
        last_presents_ = presents_;
        last_draws_ = draws_;
        log_t_ = now;
    }
}

// SCDA_SHOT_EVERY=n: every nth frame as shots/frame_<present>.ppm beside the log, for looking at a run without a
// screen
void GlBackend::SaveShot(Target& t) {
    static const uint64_t every = [] {
        const char* e = std::getenv("SCDA_SHOT_EVERY");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    if (!every || presents_ % every) return;
    std::vector<uint8_t> px(size_t(t.w) * t.h * 4);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo_);
    gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.ReadPixels(0, 0, GLsizei(t.w), GLsizei(t.h), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::filesystem::path dir = port::HomeDir() / "shots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[48];
    std::snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)presents_);
    if (FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", t.w, t.h);
        for (size_t i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
    }
}

void GlBackend::Present(const Surface& front, bool pillarbox) {
    Current();
    auto timed = Time(kPresent);
    const app::Settings& set = app::Get();
    if (!vsync_set_ || set.vsync != vsync_) {
        vsync_ = set.vsync;
        vsync_set_ = true;
        SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
    }
    if ((set.brightness << 8 | set.contrast) != ramp_set_) UploadGammaRamp(set.brightness, set.contrast);
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) SaveShot(*t);

    // the window's framebuffer the GL way up
    uint32_t bw, bh;
    port::DrawableSize(bw, bh);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    bound_rt_ = bound_ds_ = nullptr;
    gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    if (t) {
        float aspect = app::Aspect();
        float w = float(bw);
        float h = float(bh);
        float vw = std::min(w, h * aspect);
        float vh = vw / aspect;
        if (pillarbox) vw = vh * 4.0f / 3.0f;
        gl.Viewport(GLint((w - vw) / 2), GLint((h - vh) / 2), GLsizei(vw), GLsizei(vh));
        float uv[4] = {0, 0, 1, 1};
        uint32_t mode = set.colour_blind;
        std::array<float, 9> colour = mode ? ColourBlindMatrix(mode) : std::array<float, 9>{};
        Blit(t->tex, uv, gamma_, false, mode ? colour.data() : nullptr);
    }
    gl.Enable(GL_SCISSOR_TEST);
    gl.ClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    {
        auto swap = Time(kSwap);
        SDL_GL_SwapWindow(window_);
    }
    presents_++;
    if (presents_ == 1) port::Breadcrumb("first frame");
    presents_seen_.store(presents_);
    {
        std::lock_guard lock(late_lock_);
        for (auto& [data, l] : late_) PollLate(l);
    }
    ReleaseOldTextures();
    // kept programs link where a frame has time to spare: loading screens draw nothing, menus little
    uint64_t drawn = draws_ - warm_draws_;
    warm_draws_ = draws_;
    if (drawn == 0) Warm(200.0);
    else if (drawn < 150) Warm(25.0);
    CountFps();
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<GlBackend>(); }
void SavePipelineCache() {}

}  // namespace gpu
