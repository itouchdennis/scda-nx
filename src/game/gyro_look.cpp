// Gyro aiming: the gyro's turn is added to the player controller's rotation before the camera update reads it, so
// the camera's own pitch clamp holds it.
#include <cmath>

#include "app/app.h"
#include "core/gyro.h"
#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// controller: +0x1F4 its rotation's pitch, up positive, +0x1F8 its yaw, right positive (65536 a turn)
constexpr uint32_t kPitch = 0x1F4;
constexpr uint32_t kYaw = 0x1F8;
constexpr float kUnitsPerDegree = 65536.0f / 360.0f;

// the fractions of a unit not turned yet
float g_yaw_rest = 0.0f;
float g_pitch_rest = 0.0f;

// the whole units of degrees plus the rest carried over; the new rest is kept
int32_t TakeUnits(float degrees, float& rest) {
    float total = degrees * kUnitsPerDegree + rest;
    float whole = std::trunc(total);
    rest = total - whole;
    return int32_t(whole);
}

void AddUnits(uint32_t address, int32_t units) { W32(address, R32(address) + uint32_t(units)); }

}  // namespace

namespace game {

// the player controller's camera update, before it reads the rotation; a synced co-op session carries the pad only
void ApplyGyro(uint32_t controller) {
    const app::Settings& s = app::Get();
    gyro::Turn turn = gyro::Take(s.gyro);
    if (app::MenuOpen() || input::NetSession()) return;
    if (turn.recenter) W32(controller + kPitch, 0);
    // the gyro's yaw is left positive and its pitch down positive
    float yaw = s.invert_pad_x ? turn.yaw : -turn.yaw;
    float pitch = s.invert_pad_y ? turn.pitch : -turn.pitch;
    AddUnits(controller + kYaw, TakeUnits(yaw, g_yaw_rest));
    AddUnits(controller + kPitch, TakeUnits(pitch, g_pitch_rest));
}

}  // namespace game
