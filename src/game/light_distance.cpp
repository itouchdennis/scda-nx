// Lights fade out and switch off past their TurnOffDistance, and their shadows fade out between ShadowsTurnOffStart
// and ShadowsTurnOffDistance. The shadow distance setting scales all three.
#include <cstring>
#include <map>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "game/unreal.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the shadow fade's view: +0x28 its lights, a list of {light, next}
constexpr uint32_t kViewLights = 0x28, kNodeNext = 0x04;
// actor +0x270 its level; level +0x2C its actors, +0x30 how many
constexpr uint32_t kXLevel = 0x270, kActors = 0x2C, kActorCount = 0x30;
constexpr uint32_t kLightClass = game::unreal::Name("Light");
// Light: +0 vtable, +0x20 name; 0 is "never" for both distances
constexpr uint32_t kName = 0x20;
constexpr uint32_t kTurnOffDistance = 0x29C;  // +0x2A0 TurnOffDistancePercentageFadeIn is a fraction of it
constexpr uint32_t kShadowsTurnOffStart = 0x2A4;
constexpr uint32_t kShadowsTurnOffDistance = 0x2A8;
constexpr size_t kPrune = 4096;

// the map's distances, and what was last written; an entry stands while the object at its address still matches
struct Light {
    uint32_t vtable, name;
    float turn_off, shadows_start, shadows_distance;
    float written[3];  // the three distances as last written
};

std::mutex g_lock;
std::map<uint32_t, Light> g_lights;
float g_scale = 1.0f;

void SetF(uint32_t va, float f) { std::memcpy(HostPtr(va), &f, 4); }

void Write(uint32_t light, Light& l, float scale) {
    bool shadows = l.shadows_distance > 0.0f;
    l.written[0] = l.turn_off * scale;
    l.written[1] = shadows ? l.shadows_start * scale : l.shadows_start;
    l.written[2] = shadows ? l.shadows_distance * scale : l.shadows_distance;
    SetF(light + kTurnOffDistance, l.written[0]);
    SetF(light + kShadowsTurnOffStart, l.written[1]);
    SetF(light + kShadowsTurnOffDistance, l.written[2]);
}

bool StillThere(uint32_t light, const Light& l) {
    return mem::IsCommitted(light) && mem::IsCommitted(light + kShadowsTurnOffDistance + 3) && R32(light) == l.vtable &&
           R32(light + kName) == l.name && GF(light + kTurnOffDistance) == l.written[0] &&
           GF(light + kShadowsTurnOffStart) == l.written[1] && GF(light + kShadowsTurnOffDistance) == l.written[2];
}

void Forget() {
    for (auto it = g_lights.begin(); it != g_lights.end();)
        it = StillThere(it->first, it->second) ? std::next(it) : g_lights.erase(it);
}

void Update() {
    float scale = float(app::Get().shadow_distance);
    if (scale == g_scale) return;
    g_scale = scale;
    Forget();
    for (auto& [light, l] : g_lights) Write(light, l, g_scale);
    LOGI("shadow distance: x%g, %zu lights in play", g_scale, g_lights.size());
}

void Track(uint32_t light) {
    if (g_lights.size() >= kPrune) Forget();
    Light& l = g_lights[light] = {R32(light), R32(light + kName), GF(light + kTurnOffDistance),
                                  GF(light + kShadowsTurnOffStart), GF(light + kShadowsTurnOffDistance), {}};
    Write(light, l, g_scale);
}

bool Tracked(uint32_t light) {
    auto it = g_lights.find(light);
    return it != g_lights.end() && StillThere(light, it->second);
}

uint32_t TakeUpLevel(uint32_t level) {
    uint32_t actors = level ? R32(level + kActors) : 0, count = level ? R32(level + kActorCount) : 0, found = 0;
    for (uint32_t i = 0; actors && i < count; i++) {
        uint32_t actor = R32(actors + 4 * i);
        if (actor && !Tracked(actor) && game::unreal::IsA(actor, kLightClass)) {
            Track(actor);
            found++;
        }
    }
    return found;
}

}  // namespace

// ALight::PostBeginPlay, thiscall; ELight's and EDynamicLight's call it first. It already takes TurnOffDistance
// down by 0.9 on one path, so the scale goes on what it leaves.
X86_EXTERN_FUNC(__imp__sub_0027902C);
X86_FUNC(sub_0027902C) {
    uint32_t light = ctx.ecx;
    __imp__sub_0027902C(ctx, base);
    std::lock_guard<std::mutex> lock(g_lock);
    Update();
    Track(light);
}

// UObject::SavePackage, cdecl: a save holds the map's distances, never scaled ones
X86_EXTERN_FUNC(__imp__sub_00063E97);
X86_FUNC(sub_00063E97) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Forget();
        for (auto& [light, l] : g_lights) Write(light, l, 1.0f);
        LOGI("shadow distance: %zu lights at the map's distances for a save", g_lights.size());
    }
    __imp__sub_00063E97(ctx, base);
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [light, l] : g_lights) Write(light, l, g_scale);
}

// each light's shadow fade, once a view: a changed setting reaches the lights in play before it is computed. A light
// in view that PostBeginPlay never saw means a saved level (a save load, Restart Mission skip it): every light of its
// level is taken up then, with the map's distances, which the save above keeps. The view holds only lights near
// enough to draw, so the far ones are found through the level.
X86_EXTERN_FUNC(__imp__sub_002AA0B0);
X86_FUNC(sub_002AA0B0) {
    uint32_t view = R32(ctx.esp + 4);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Update();
        for (uint32_t node = R32(view + kViewLights), n = 0; node && n < kPrune; node = R32(node + kNodeNext), n++) {
            uint32_t light = R32(node);
            if (!light || Tracked(light) || !game::unreal::IsA(light, kLightClass)) continue;
            uint32_t found = TakeUpLevel(R32(light + kXLevel));
            if (!Tracked(light)) Track(light);
            LOG_FIRST(20, "shadow distance: %u lights of the level taken up", found);
            break;
        }
    }
    __imp__sub_002AA0B0(ctx, base);
}
