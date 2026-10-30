#include "game/unreal.h"

#include <unordered_map>

#include "xdk/hle.h"

using namespace kernel;

namespace game::unreal {

namespace {

// object: +0xC state frame, +0x20 name, +0x24 class; state frame: +0x1C state; class: +0x2C super, +0x38 first
// own property; property: +4 next, +0xC name, +0x10 offset << 16 | array size
constexpr uint32_t kStateFrame = 0x0C, kName = 0x20, kClass = 0x24, kFrameState = 0x1C;
constexpr uint32_t kSuper = 0x2C, kProperties = 0x38, kNext = 0x04, kPropertyName = 0x0C, kPropertyOffset = 0x10;

// game thread; classes can be reloaded, so a hit is checked against the property's name again
std::unordered_map<uint64_t, uint32_t> g_found;

}  // namespace

uint32_t NameOf(uint32_t object) { return object ? R32(object + kName) : 0; }

uint32_t StateOf(uint32_t object) {
    uint32_t frame = object ? R32(object + kStateFrame) : 0;
    uint32_t state = frame ? R32(frame + kFrameState) : 0;
    return NameOf(state);
}

bool IsA(uint32_t object, uint32_t class_name) {
    for (uint32_t c = object ? R32(object + kClass) : 0, depth = 0; c && depth < 64; c = R32(c + kSuper), depth++)
        if (R32(c + kName) == class_name) return true;
    return false;
}

uint32_t Offset(uint32_t object, uint32_t property) {
    uint32_t cls = object ? R32(object + kClass) : 0;
    if (!cls) return 0;
    uint64_t key = uint64_t(cls) << 32 | property;
    auto it = g_found.find(key);
    if (it != g_found.end() && R32(it->second + kPropertyName) == property)
        return R32(it->second + kPropertyOffset) >> 16;
    for (uint32_t c = cls, depth = 0; c && depth < 64; c = R32(c + kSuper), depth++)
        for (uint32_t p = R32(c + kProperties), n = 0; p && n < 4096; p = R32(p + kNext), n++)
            if (R32(p + kPropertyName) == property) {
                g_found[key] = p;
                return R32(p + kPropertyOffset) >> 16;
            }
    return 0;
}

}  // namespace game::unreal
