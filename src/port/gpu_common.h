#pragma once
// What the port's renderers (gpu_gl.cpp, gpu_vk.cpp) share, all of it as gpu/d3d12.cpp has it: the draw's constants,
// its shader keys, guest memory signatures of targets, CopyRects' lists, the present pass and its gamma and colour
// blind corrections.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

namespace gpu::common {

// the present pass: the frame scaled into the window, then gamma (a 256 x 1 lookup) and a colour matrix
extern const char* const kBlitHlsl;

float Bits(uint32_t v);
void Color(uint32_t argb, float* o);

// D3DCMP_NEVER to ALWAYS as 0 to 7, the order GL and Vulkan take them in
inline uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format);

// bytes per row of a 32 bit surface in guest memory, rounded to 64
inline uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// a surface's guest memory hashed whole up to 64 KB, a bigger one as evenly spaced samples
uint64_t MemSig(uint32_t data, uint32_t total);

// T: a target with gw, gh, data, bytes, epoch, sig, drawn and read (what Sync left in guest memory).
// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
template <class T>
bool Changed(const T& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
template <class T>
bool LatchCpuWrite(T& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// only methods render_thread.cpp's StateRanges sends reach the render thread
inline uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

// a draw's shaders: what they are made from and their cache keys
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t pkey = 0;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestSize {
    uint32_t w;
    uint32_t h;
};

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count);
uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh);
std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh);

// DrawConstants as d3d12.cpp fills them; rt: the stages that sample a render target
void FillSurfaceConstants(const State& s, GuestSize size, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
void FillTextureConstants(const State& s, const bool rt[4], DrawConstants& c);
void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);

// D3DTEXTUREFILTERTYPE and D3DTEXTUREADDRESS values the samplers test
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes; see d3d12.cpp
bool UnitTexcoords(const Assembled& a);
bool AnyStageWraps(const State& s);

constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR
constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW
constexpr float kDepth24Max = 16777215.0f;   // a 24 bit depth buffer's units per 1.0

// a target's key by its surface's colour or depth
inline uint64_t TargetKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// the shadow map's colour surface: no memory, colour writes off
bool IsShadowColour(const Surface& s);

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
struct GuestRect {
    int32_t left, top, right, bottom;
};
GuestRect ReadGuestRect(uint32_t rects, uint32_t i);

TexKind TextureKind(const TextureDesc& d);

// a normalization cube: each face's centre points along its axis, 255 or 0 there, 128 across; norm is set on face 0
// and cleared by any face that is not
void CheckNormCube(bool& norm, const TextureDesc& d, const FormatInfo& fi, uint32_t face,
                   const std::vector<uint8_t>& px, uint32_t pitch);

// the highest vertex the draw reads, relative to the uploaded buffers
uint32_t MaxVertex(const Assembled& a);

// brightness and contrast from 0 to 100, 50/50 leaves the image unchanged, after game: the title's own ramp
// (D3DDevice_SetGammaRamp, its shadow calibration: red, green and blue, 256 each) or null. The ramp as BGRA, false for
// identity
bool GammaRamp(uint32_t brightness, uint32_t contrast, const uint8_t* game, uint8_t ramp[256 * 4]);

// colour blind modes 1 to 3 as d3d12.cpp: Machado 2009 simulation, then Fidaner's daltonize; rows of a 3x3 matrix
std::array<float, 9> ColourBlindMatrix(uint32_t mode);

}  // namespace gpu::common
