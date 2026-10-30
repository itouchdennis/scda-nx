// The camera's hit, tilt and shake effects step once a frame, and tilt and shake fade only on the frame they reach
// their swing target, so at high fps each swing takes many frames and a shake lasts several times longer (as SCCT
// PC). They step at the console's 30 Hz on game time instead, and each frame is drawn between the last two steps.
// The tilt is added to the controller's own rotation on every update (the weapon jump), so it is also scaled by the
// game time the update covers: the same total kick as 30 fps, spread over the frames.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// EPlayerCam ints, m_hitRoll .. m_shakeFadeOut
constexpr uint32_t kEffects = 0x1958;
enum Effect {
    HitRoll, HitFadeOut, TiltPitch, TiltTarget, TiltSpeed, TiltFadeOut, ShakeRoll, ShakeTarget, ShakeSpeed,
    ShakeFadeOut, kEffectCount
};
using Effects = std::array<int32_t, kEffectCount>;

constexpr uint32_t kLevel = 0x1BC, kTimeSeconds = 0x3D0;  // camera's LevelInfo, its game time

constexpr float kStep = 1.0f / 30;
constexpr int kMaxSteps = 4;        // a longer hitch drops the rest
constexpr float kResumeGap = 1.0f;  // a longer gap or time going back starts over from the fields

struct Cam {
    Effects sim{};   // after the last step
    Effects left{};  // as written back after the last call, to spot script writes
    Effects back{};  // before the last step
    float accumulator = 0, last_time = 0;
    float tilt_rest = 0;  // the fraction of a unit of tilt not yet added
};
std::unordered_map<uint32_t, Cam> g_cams;

Effects Read(uint32_t cam) {
    Effects e;
    std::memcpy(e.data(), HostPtr(cam + kEffects), sizeof(e));
    return e;
}

void Write(uint32_t cam, const Effects& e) { std::memcpy(HostPtr(cam + kEffects), e.data(), sizeof(e)); }

// the game's Approach (sub_0016D690) at the step time, truncated as its ftol does
int32_t Approach(int32_t current, int32_t target, int32_t speed) {
    float s = float(speed) * kStep, d = float(target) - float(current);
    float step = -s > d ? -s : (s > d ? d : s);
    return int32_t(std::trunc(double(current) + step));
}

// a swing chases its target, and each time it arrives the target fades and flips sides
void Step(Effects& e) {
    if (e[HitRoll]) e[HitRoll] = Approach(e[HitRoll], 0, e[HitFadeOut]);
    if (e[ShakeRoll] || e[ShakeTarget]) {
        if (e[ShakeRoll] == e[ShakeTarget]) e[ShakeTarget] = -Approach(e[ShakeTarget], 0, e[ShakeFadeOut]);
        e[ShakeRoll] = Approach(e[ShakeRoll], e[ShakeTarget], e[ShakeSpeed]);
    }
    if (e[TiltPitch] || e[TiltTarget]) {
        if (e[TiltPitch] == e[TiltTarget]) {
            e[TiltTarget] = -Approach(e[TiltTarget], 0, e[TiltFadeOut]);
            // a fade-out of -1 is a single kick, over once it has swung back
            if (e[TiltFadeOut] == -1 && e[TiltTarget] >= 0) {
                e[TiltPitch] = e[TiltTarget] = 0;
                return;
            }
        }
        e[TiltPitch] = Approach(e[TiltPitch], e[TiltTarget], e[TiltSpeed]);
    }
}

}  // namespace

// EPlayerCam effect update, thiscall(pitch, yaw, roll): adds the effects to the view rotation, then steps them by
// the level's frame time
X86_EXTERN_FUNC(__imp__sub_0004E500);
X86_FUNC(sub_0004E500) {
    uint32_t cam = ctx.ecx, level = R32(cam + kLevel);
    if (!level) return __imp__sub_0004E500(ctx, base);
    float now = G<float>(level + kTimeSeconds);
    Effects fields = Read(cam);
    auto [it, added] = g_cams.try_emplace(cam);
    Cam& c = it->second;
    float elapsed = 0;
    if (added || now < c.last_time || now - c.last_time > kResumeGap) {
        c.sim = c.left = c.back = fields;
        c.accumulator = 0;
        c.tilt_rest = 0;
    } else {
        // Hit, Tilt, Shake and ResetEffects write the fields directly
        for (int i = 0; i < kEffectCount; i++)
            if (fields[i] != c.left[i]) c.sim[i] = c.back[i] = fields[i];
        elapsed = std::min(now - c.last_time, kMaxSteps * kStep);
        c.accumulator += now - c.last_time;  // the update runs more than once a frame
    }
    c.last_time = now;
    for (int steps = 0; c.accumulator >= kStep; steps++) {
        if (steps == kMaxSteps) {
            c.accumulator = 0;
            break;
        }
        c.back = c.sim;
        Step(c.sim);
        c.accumulator -= kStep;
    }
    // the stock update applies the values it finds, then moves them; with no speeds they stay put
    float alpha = c.accumulator / kStep;
    Effects drawn = c.sim;
    for (Effect i : {HitRoll, ShakeRoll})
        drawn[i] = int32_t(std::lround(c.back[i] + (c.sim[i] - c.back[i]) * alpha));
    float tilt = c.back[TiltPitch] + (c.sim[TiltPitch] - c.back[TiltPitch]) * alpha;
    float kick = tilt * elapsed / kStep + c.tilt_rest;
    drawn[TiltPitch] = int32_t(std::lround(kick));
    c.tilt_rest = c.sim[TiltPitch] || c.sim[TiltTarget] ? kick - drawn[TiltPitch] : 0;
    drawn[HitFadeOut] = drawn[TiltSpeed] = drawn[TiltFadeOut] = drawn[ShakeSpeed] = drawn[ShakeFadeOut] = 0;
    Write(cam, drawn);
    __imp__sub_0004E500(ctx, base);
    Write(cam, c.left = c.sim);
}
