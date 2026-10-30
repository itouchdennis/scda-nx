// Controllers through SDL2's game controller API, read on the main thread (SDL2 wants its events and joystick
// updates there) into XInput's report. A controller takes the lowest free slot when it is plugged in and keeps it
// until it is unplugged, as input.cpp's SDL3 code numbers them on Windows. On the Switch SDL reports the docked or
// handheld Joy-Con pair, or each Pro Controller, as one controller.
#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "port/pads.h"

namespace port {

namespace {

struct Slot {
    SDL_GameController* pad = nullptr;
    SDL_JoystickID id = -1;
    XINPUT_GAMEPAD state{};
    uint16_t left = 0, right = 0;  // the motors as last asked
    bool motors_changed = false;
};

std::mutex g_lock;
std::array<Slot, 4> g_slots;

SHORT Axis(SDL_GameController* pad, SDL_GameControllerAxis axis) { return SDL_GameControllerGetAxis(pad, axis); }
// SDL's sticks have y down; not plain negation, -32768 has no positive
SHORT Up(SHORT y) { return SHORT(-1 - y); }
BYTE Trigger(SDL_GameController* pad, SDL_GameControllerAxis axis) {
    return BYTE(std::max<int>(SDL_GameControllerGetAxis(pad, axis), 0) >> 7);
}

XINPUT_GAMEPAD Read(SDL_GameController* pad) {
    // Xbox labels: on a Nintendo layout SDL still names the bottom face button A by position (SOUTH). swap_ab (the
    // Switch's default) makes them follow the labels instead: the right one, Nintendo's A, is the Xbox's A, so it
    // confirms in the game and in every menu, and the bottom one, Nintendo's B, goes back
    const bool swap = app::Get().swap_ab;
    constexpr std::pair<SDL_GameControllerButton, WORD> kButtons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        {SDL_CONTROLLER_BUTTON_START, XINPUT_GAMEPAD_START},
        {SDL_CONTROLLER_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {SDL_CONTROLLER_BUTTON_A, XINPUT_GAMEPAD_A},
        {SDL_CONTROLLER_BUTTON_B, XINPUT_GAMEPAD_B},
        {SDL_CONTROLLER_BUTTON_X, XINPUT_GAMEPAD_X},
        {SDL_CONTROLLER_BUTTON_Y, XINPUT_GAMEPAD_Y},
    };
    XINPUT_GAMEPAD g{};
    for (auto [button, mask] : kButtons) {
        if (swap && mask == XINPUT_GAMEPAD_A) mask = XINPUT_GAMEPAD_B;
        else if (swap && mask == XINPUT_GAMEPAD_B) mask = XINPUT_GAMEPAD_A;
        if (SDL_GameControllerGetButton(pad, button)) g.wButtons |= mask;
    }
    g.bLeftTrigger = Trigger(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    g.bRightTrigger = Trigger(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    g.sThumbLX = Axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    g.sThumbLY = Up(Axis(pad, SDL_CONTROLLER_AXIS_LEFTY));
    g.sThumbRX = Axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    g.sThumbRY = Up(Axis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
    return g;
}

}  // namespace

void StartPads() {
    static std::once_flag once;
    std::call_once(once, [] {
        SDL_version v;
        SDL_GetVersion(&v);
        LOGI("input: SDL %d.%d.%d", v.major, v.minor, v.patch);
    });
}

void PadAdded(int device_index) {
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    std::lock_guard lock(g_lock);
    if (std::any_of(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.pad && s.id == id; })) return;
    auto slot = std::find_if(g_slots.begin(), g_slots.end(), [](const Slot& s) { return !s.pad; });
    if (slot == g_slots.end()) return;
    slot->pad = SDL_GameControllerOpen(device_index);
    slot->id = slot->pad ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot->pad)) : -1;
    slot->state = {};
    const char* name = slot->pad ? SDL_GameControllerName(slot->pad) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_slots.begin()) + 1, slot->pad ? "plugged in" : "failed to open",
         name ? name : "");
}

void PadRemoved(int instance_id) {
    std::lock_guard lock(g_lock);
    for (size_t i = 0; i < g_slots.size(); i++) {
        Slot& s = g_slots[i];
        if (!s.pad || s.id != instance_id) continue;
        SDL_GameControllerClose(s.pad);
        s = {};
        LOGI("input: controller %zu unplugged", i + 1);
    }
}

void UpdatePads() {
    std::lock_guard lock(g_lock);
    for (Slot& s : g_slots) {
        if (!s.pad) continue;
        s.state = Read(s.pad);
        if (s.motors_changed) {
            s.motors_changed = false;
            // the motors run until set again, as XInput's do: SDL's rumble stops after its duration
            SDL_GameControllerRumble(s.pad, s.left, s.right, s.left || s.right ? 0xFFFFFFFFu : 0);
        }
    }
}

bool ReadPad(int slot, XINPUT_GAMEPAD& g) {
    std::lock_guard lock(g_lock);
    const Slot& s = g_slots[size_t(slot)];
    if (!s.pad) return false;
    g = s.state;
    return true;
}

void SetMotors(int slot, uint16_t left, uint16_t right) {
    std::lock_guard lock(g_lock);
    Slot& s = g_slots[size_t(slot)];
    if (s.left == left && s.right == right) return;
    s.left = left;
    s.right = right;
    s.motors_changed = true;
}

}  // namespace port
