// The camera turns by aTurn * m_yawSpeed and pitches by aLookUp * m_pitchSpeed
// (40000 and 25000 in SplinterCell4Settings.ini), making vertical look 0.625x as fast as horizontal.
// Mouse look bypasses the stick's clamp and curve, using aTurn / aLookUp directly,
// so vertical and horizontal mouse look both use the yaw speed.
// The stick's look takes the controller sensitivities as scaled speeds, the gyro turns the rotation first.
#include <bit>

#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
void ApplyGyro(uint32_t controller);  // gyro_look.cpp
}

namespace {

// EPlayerCam: +0x300 m_yawSpeed, +0x304 m_pitchSpeed, +0x3F8 its controller; controller: +0x390 aMouseX, +0x394 aMouseY
constexpr uint32_t kYawSpeed = 0x300, kPitchSpeed = 0x304, kController = 0x3F8;
constexpr uint32_t kMouseX = 0x390, kMouseY = 0x394;

bool Moved(uint32_t controller, uint32_t offset) { return R32(controller + offset) & 0x7FFFFFFF; }

}  // namespace

// EPlayerCam's rotation update, thiscall
X86_EXTERN_FUNC(__imp__sub_000FD37E);
X86_FUNC(sub_000FD37E) {
    uint32_t cam = ctx.ecx;
    uint32_t controller = R32(cam + kController);
    if (!controller) {
        __imp__sub_000FD37E(ctx, base);
        return;
    }
    game::ApplyGyro(controller);
    uint32_t yaw = R32(cam + kYawSpeed);
    uint32_t pitch = R32(cam + kPitchSpeed);
    if (Moved(controller, kMouseX) || Moved(controller, kMouseY)) {
        W32(cam + kPitchSpeed, yaw);
    } else {
        input::LookScale scale = input::PadLookScale(input::Aiming());
        W32(cam + kYawSpeed, std::bit_cast<uint32_t>(std::bit_cast<float>(yaw) * scale.x));
        W32(cam + kPitchSpeed, std::bit_cast<uint32_t>(std::bit_cast<float>(pitch) * scale.y));
    }
    __imp__sub_000FD37E(ctx, base);
    W32(cam + kYawSpeed, yaw);
    W32(cam + kPitchSpeed, pitch);
}
