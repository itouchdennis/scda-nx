// The EEV, the sniper scope and the sticky camera with the mouse wheel. All are states of the player controller.
// EEV and sniper scope: the pad's left stick click (ZoomInOut) steps EEEV.ZoomIdx or the scope's FOVIndex through
// FOVs and wraps. The wheel sets a target level instead, up zooms in and down zooms out, stopping at the ends, and the
// click is tapped until the level is reached. Sticky camera: the input side zooms it with the stick and remaps its keys.
// Scripted events (in-game cinematics): s_ScriptedEvent's ProcessPauseMenu skips them, so Enter presses Start too.
// Aiming with no interaction in reach, the pad's A reloads; the keyboard has a reload key instead.
// Carrying a body, the keyboard's alt fire puts it down, as on SCCT PC.
#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/input.h"
#include "game/unreal.h"
#include "xdk/hle.h"

using namespace kernel;
namespace unreal = game::unreal;

namespace {

constexpr uint32_t kViewportActor = 0x34;
// controller: +0x510 bit 0x8000 bInTransition
constexpr uint32_t kTransition = 0x510, kTransitionBit = 0x8000;
constexpr uint16_t kLeftThumb = 1 << 6;
// a tap the game did not act on (EEV busy scanning or hacking) drops the target
constexpr auto kTapTimeout = std::chrono::milliseconds(400);

uint32_t g_zoomer = 0;  // the EEV or scope being zoomed, 0 for none
int g_target = 0, g_last = 0;
bool g_tapping = false;
std::chrono::steady_clock::time_point g_tap_time;

// zoom: the object's level index, fovs: its FOVs array (+0 data, +4 count)
void Zoom(uint32_t object, uint32_t zoom, uint32_t fovs) {
    int notches = input::TakeWheel();
    if (!object || !zoom || !fovs) {
        g_zoomer = 0;
        return;
    }
    int level = int(R32(object + zoom)), count = int(R32(object + fovs + 4));
    auto now = std::chrono::steady_clock::now();
    if (object != g_zoomer) {
        g_zoomer = object;
        g_target = g_last = level;
        g_tapping = false;
    }
    if (g_tapping && level != g_last) {
        g_tapping = false;
        g_last = level;
    } else if (g_tapping && now - g_tap_time > kTapTimeout) {
        g_tapping = false;
        g_target = level;
    }
    // the player clicked the stick or the game reset the zoom
    if (!g_tapping && level != g_last) g_target = level;
    g_last = level;
    if (notches && count > 0) g_target = std::clamp(g_target + notches, 0, count - 1);
    if (!g_tapping && level != g_target) {
        input::TapButtons(kLeftThumb);
        g_tapping = true;
        g_tap_time = now;
    }
}

void Eev(uint32_t pc) {
    uint32_t eev_offset = unreal::Offset(pc, unreal::Name("EEV"));
    uint32_t eev = eev_offset ? R32(pc + eev_offset) : 0;
    Zoom(eev, eev ? unreal::Offset(eev, unreal::Name("ZoomIdx")) : 0,
         eev ? unreal::Offset(eev, unreal::Name("FOVs")) : 0);
}

// EMainGun.GetScope: the sniper attachment while it is the active one; the default scope has a single FOV
void Sniper(uint32_t pc) {
    uint32_t gun_offset = unreal::Offset(pc, unreal::Name("ActiveGun"));
    uint32_t gun = gun_offset ? R32(pc + gun_offset) : 0;
    uint32_t active = gun ? unreal::Offset(gun, unreal::Name("ActiveAttach")) : 0;
    uint32_t sniper = gun ? unreal::Offset(gun, unreal::Name("Sniper")) : 0;
    uint32_t scope = active && sniper && R32(gun + active) == R32(gun + sniper) ? R32(gun + sniper) : 0;
    Zoom(scope, scope ? unreal::Offset(scope, unreal::Name("FOVIndex")) : 0,
         scope ? unreal::Offset(scope, unreal::Name("FOVs")) : 0);
}

// s_PlayerSniping and the states extending it
bool Sniping(uint32_t state) {
    static constexpr uint32_t kSniping[] = {
        unreal::Name("s_PlayerSniping"),         unreal::Name("s_RappellingSniping"),
        unreal::Name("s_DualRappellingSniping"), unreal::Name("s_SplitSniping"),
        unreal::Name("s_OverTrainSniping"),      unreal::Name("s_TeamShouldersSniping"),
    };
    return std::find(std::begin(kSniping), std::end(kSniping), state) != std::end(kSniping);
}

bool Targeting(uint32_t state) {
    static constexpr uint32_t kTargeting[] = {
        unreal::Name("s_FirstPersonTargeting"),  unreal::Name("s_Targeting"),
        unreal::Name("s_RappellingTargeting"),   unreal::Name("s_DualRappellingTargeting"),
        unreal::Name("s_SplitTargeting"),        unreal::Name("s_HOHTargeting"),
        unreal::Name("s_GrabTargeting"),         unreal::Name("s_OverTrainTargeting"),
        unreal::Name("s_TeamShouldersTargeting"), unreal::Name("s_CoopHangedTargeting"),
    };
    return std::find(std::begin(kTargeting), std::end(kTargeting), state) != std::end(kTargeting);
}

// the other states whose PlayerMove moves the pawn only past m_gentleForce, as aiming does
bool GentleMove(uint32_t state) {
    static constexpr uint32_t kGentle[] = {
        unreal::Name("s_EEV"),   unreal::Name("s_FirstPersonView"),
        unreal::Name("s_Throw"), unreal::Name("s_Shoutout"),
    };
    return Sniping(state) || std::find(std::begin(kGentle), std::end(kGentle), state) != std::end(kGentle);
}

// controller IManager (EInteractionManager), its Interactions a TArray (+0 data, +4 count)
bool InteractReloads(uint32_t pc, uint32_t state) {
    if (!Targeting(state)) return false;
    uint32_t manager_offset = unreal::Offset(pc, unreal::Name("IManager"));
    uint32_t manager = manager_offset ? R32(pc + manager_offset) : 0;
    uint32_t list = manager ? unreal::Offset(manager, unreal::Name("Interactions")) : 0;
    return list && R32(manager + list + 4) == 0;
}

}  // namespace

namespace game {

// the viewport, each input update on the game thread
void UpdateView(uint32_t viewport) {
    uint32_t pc = R32(viewport + kViewportActor);
    uint32_t state = unreal::StateOf(pc);
    input::View view = input::View::Normal;
    float zoom_speed = 0.0f;
    bool sniping = Sniping(state);
    if (state == unreal::Name("s_EEV") || sniping) {
        view = input::View::Eev;
    } else if (state == unreal::Name("s_ScriptedEvent")) {
        view = input::View::ScriptedEvent;
    } else if (state == unreal::Name("s_Ledge") || state == unreal::Name("s_Fence") ||
               state == unreal::Name("s_NarrowLadder") || state == unreal::Name("s_Pipe") ||
               state == unreal::Name("s_PipeSlideDown") || state == unreal::Name("s_Vent") ||
               state == unreal::Name("s_Carry") || state == unreal::Name("s_SBRopeClimb") ||
               state == unreal::Name("s_Rappelling") || state == unreal::Name("s_DualRappelling")) {
        view = input::View::Ledge;
    } else if (state == unreal::Name("s_PickLock") || state == unreal::Name("s_HandCuffed")) {
        view = input::View::Lockpick;
    } else if (state == unreal::Name("s_OpticCable") || state == unreal::Name("s_ElevatorInteract")) {
        view = input::View::BackOut;
    } else if (state == unreal::Name("s_KeyPadInteract") || state == unreal::Name("s_KeyPadInteractHanged")) {
        view = input::View::Keypad;
    } else if (state == unreal::Name("s_Camera")) {
        uint32_t base_offset = unreal::Offset(pc, unreal::Name("Base"));
        uint32_t camera = base_offset ? R32(pc + base_offset) : 0;
        bool sticky = unreal::IsA(camera, unreal::Name("EStickyCamera"));
        uint32_t speed = sticky ? unreal::Offset(camera, unreal::Name("ZoomSpeed")) : 0;
        if (speed) {
            view = input::View::StickyCamera;
            std::memcpy(&zoom_speed, HostPtr(camera + speed), 4);
        }
    }
    input::SetView(view, zoom_speed);
    input::SetInteractReloads(InteractReloads(pc, state));
    input::SetTargeting(Targeting(state) || GentleMove(state));
    input::SetAiming(Targeting(state) || sniping);
    input::SetCarrying(state == unreal::Name("s_Carry"));
    input::SetHanging(state == unreal::Name("s_Ledge"));
    input::SetLadder(state == unreal::Name("s_NarrowLadder"), R32(pc + kTransition) & kTransitionBit);
    input::SetPipe(state == unreal::Name("s_Pipe"));
    if (view == input::View::Eev && sniping) Sniper(pc);
    else if (view == input::View::Eev) Eev(pc);
    else g_zoomer = 0;
}

}  // namespace game
