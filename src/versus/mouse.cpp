// Mouse look in versus. The controller's native PlayerInput (sub_0003FB80) turns aTurn / aLookUp into the look
// rates it leaves at +0x4D8 / +0x4DC (+-1000 is full stick) through a 220 dead zone, a clamp to full stick, a response
// curve and smoothing; the mouse replaces its outputs after it, so none of those touch it. The binoculars drop the
// rates while the stick magnitude it leaves is 0, so that follows the mouse as well. Divided by dt, a count turns the
// same at any frame rate.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "game/magma.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
void NameEntryTick(X86Context& ctx, uint32_t top);  // name_entry.cpp
}
namespace versus {
void OptionsTick(X86Context& ctx, uint32_t top);  // options_menu.cpp
}

namespace {

// viewport: +0x30 actor, the local player's controller
constexpr uint32_t kViewportActor = 0x30;
// controller: +0x4D0 stick magnitude after the curve and +0x4D4 smoothed (0 to 1), +0x4D8 turn rate, +0x4DC look up
// rate, +0x4E0 / +0x4E4 the largest |aTurn| / |aLookUp| seen (the rates are aTurn and aLookUp over them), +0x504 aTurn,
// +0x50C aLookUp, +0x720 (+0x320 flags 0x180: no look), +0xF4C bit 0x400 invert look (the look rate only)
constexpr uint32_t kMagnitude = 0x4D0;
constexpr uint32_t kSmoothMagnitude = 0x4D4;
constexpr uint32_t kTurnRate = 0x4D8;
constexpr uint32_t kLookRate = 0x4DC;
constexpr uint32_t kTurnMax = 0x4E0;
constexpr uint32_t kLookMax = 0x4E4;
constexpr uint32_t kTurn = 0x504;
constexpr uint32_t kLookUp = 0x50C;
constexpr uint32_t kLookBlock = 0x720;
constexpr uint32_t kLookBlockFlags = 0x320;
constexpr uint32_t kOptions = 0xF4C;
constexpr uint32_t kNoLook = 0x180;
constexpr uint32_t kInvertLook = 0x400;
// +0xF48 bit 0x2000000 while the binoculars are up (the sniper view)
constexpr uint32_t kFlags = 0xF48;
constexpr uint32_t kBinoculars = 0x2000000;
// controller +0xF3C bReloadROF, held by the pad's Y (the ini's ReloadROF alias) along with jump: the reload key holds
// it alone. Its IK_R=ForceReload binding is a PC leftover that does nothing here.
constexpr uint32_t kReloadButton = 0xF3C;
// the binoculars zoom on the D-pad; one press is held 80 ms (input::TapButtons), then let go for at least a frame
constexpr uint16_t kDpadUp = 1 << 0;
constexpr uint16_t kDpadDown = 1 << 1;
constexpr auto kZoomTapGap = std::chrono::milliseconds(150);
// PlayerInput's other way out: the object at 0x3AA3FC with +0x34 bit 2 set (sub_000DC110)
constexpr uint32_t kLookGate = 0x3AA3FC;
constexpr uint32_t kLookGateFlags = 0x34;
constexpr float kFullStick = 1000.0f;
// full stick for one second is this many counts
constexpr float kCountsPerStickSecond = 1500.0f;
// object +0xC its state frame, frame +0x18 the state, object +0x20 its name (an FName index of FNames.txt)
constexpr uint32_t kStateFrame = 0x0C, kStateNode = 0x18, kName = 0x20;
// the player controller's states with the weapon raised, and the draw into them
constexpr uint32_t kWeaponStates[] = {
    1612493,  // s_PlayerWalkingSt2H
    1612491,  // s_PlayerWalkingCr2H
    1614465,  // s_PlayerWalkingWeapon
    1612606,  // s_PlayerWalkingMario_T_0HTo2H
    1612926,  // s_PlayerWalkingWeapon_T_CrToSt
    1612974,  // s_PlayerWalkingWeapon_T_StToCr
    1612737,  // VandammeWeapon (split jump, gun out)
    1614290,  // VandammeWaitForWeapon
};
// controller +0x9A8 its EPlayerCam (vtable 0x328868): +0x19B0 yaw speed 40000, +0x19B4 pitch speed 25000 (floats)
constexpr uint32_t kCamera = 0x9A8, kCameraVtable = 0x328868, kYawSpeed = 0x19B0, kPitchSpeed = 0x19B4;
// the binoculars slow the look down with the zoom; the mouse gets some of it back
constexpr float kBinocularSpeed = 1.5f;

uint32_t g_player = 0;
// the counts of the last viewport update, for every PlayerInput after it
float g_dx = 0.0f;
float g_dy = 0.0f;
bool g_binoculars = false;
bool g_reload = false;
uint32_t g_pitch_camera = 0;  // the camera given the yaw speed to pitch with, and its own pitch speed
uint32_t g_pitch_speed = 0;
int g_zoom = 0;  // wheel notches not pressed yet, up positive
std::chrono::steady_clock::time_point g_zoom_next;

bool Looking(uint32_t controller) {
    uint32_t gate = R32(kLookGate);
    if (gate && (R32(gate + kLookGateFlags) >> 2 & 1)) return false;
    uint32_t block = R32(controller + kLookBlock);
    return !block || !(R32(block + kLookBlockFlags) & kNoLook);
}

// in the binoculars the wheel zooms as D-pad up and down, one press a notch
void Zoom(uint32_t controller) {
    bool binoculars = controller && (R32(controller + kFlags) & kBinoculars);
    if (binoculars != g_binoculars) {
        g_binoculars = binoculars;
        g_zoom = 0;
        input::SetView(binoculars ? input::View::Eev : input::View::Normal);
    }
    if (!binoculars) return;
    int notches = input::TakeWheel();
    if (notches * g_zoom < 0) g_zoom = 0;
    g_zoom += notches;
    auto now = std::chrono::steady_clock::now();
    if (!g_zoom || now < g_zoom_next) return;
    input::TapButtons(g_zoom > 0 ? kDpadUp : kDpadDown);
    g_zoom += g_zoom > 0 ? -1 : 1;
    g_zoom_next = now + kZoomTapGap;
}

void WriteFloat(uint32_t address, float value) { W32(address, std::bit_cast<uint32_t>(value)); }

float Rate(float counts, float seconds) {
    float rate = kFullStick * counts / (kCountsPerStickSecond * seconds);
    return g_binoculars ? rate * kBinocularSpeed : rate;
}

bool WeaponRaised(uint32_t controller) {
    uint32_t frame = R32(controller + kStateFrame);
    uint32_t state = frame ? R32(frame + kStateNode) : 0;
    uint32_t name = state ? R32(state + kName) : 0;
    return std::find(std::begin(kWeaponStates), std::end(kWeaponStates), name) != std::end(kWeaponStates);
}

// the sensitivity of the player's view: the sniper scope, the weapon raised, else free look
float ViewSensitivity(uint32_t controller) {
    const app::Settings& s = app::Get();
    if (g_binoculars) return s.sniper_sensitivity;
    return WeaponRaised(controller) ? s.weapon_sensitivity : s.free_look_sensitivity;
}

// the rate, and the axis it would have come from
void Look(uint32_t rate, uint32_t axis, uint32_t max, float r, bool invert) {
    float m = std::bit_cast<float>(R32(max));
    WriteFloat(rate, invert ? -r : r);
    WriteFloat(axis, m > 0.0f ? r * m / kFullStick : r);
}

// The camera pitches by the look rate times its pitch speed, 0.625x its yaw speed, as story's EPlayerCam. While the
// mouse looks, the camera pitches at the yaw speed too.
void PitchSpeed(uint32_t controller, bool mouse) {
    uint32_t cam = controller ? R32(controller + kCamera) : 0;
    if (cam && R32(cam) != kCameraVtable) cam = 0;
    if (g_pitch_camera && g_pitch_camera == cam && !mouse) W32(cam + kPitchSpeed, g_pitch_speed);
    if (!mouse || g_pitch_camera != cam) g_pitch_camera = 0;
    if (!mouse || !cam || g_pitch_camera) return;
    g_pitch_camera = cam;
    g_pitch_speed = R32(cam + kPitchSpeed);
    W32(cam + kPitchSpeed, R32(cam + kYawSpeed));
}

// Menus, as in story: the cursor focuses the control under it and selects a list's row, a click presses A or
// scrolls on a list's arrows, the wheel steps up and down.
// manager: +0x14 begin, +0x18 end of the input stack's {page, flags}; drawer: +0xAC page width, +0xAE height
constexpr uint32_t kMagma = 0x3D8D74;
constexpr uint32_t kDrawer = 0x3D8D8C;
constexpr uint32_t kStackBegin = 0x14;
constexpr uint32_t kStackEnd = 0x18;
constexpr uint32_t kStackEntry = 8;
constexpr uint32_t kDrawerWidth = 0xAC;
constexpr uint32_t kDrawerHeight = 0xAE;
// page: +4 id, +0xC begin, +0x10 end of instances, +0x24 focused
constexpr uint32_t kPageId = 0x04;
constexpr uint32_t kPageBegin = 0x0C;
constexpr uint32_t kPageEnd = 0x10;
constexpr uint32_t kPageFocused = 0x24;
// instance: +4 shown, +5 hidden, +0x10 widget, +0xB8 flags (2 enabled, 8 focused), vtable +0xC Focus(direction, notify)
constexpr uint32_t kInstanceShown = 0x04;
constexpr uint32_t kInstanceHidden = 0x05;
constexpr uint32_t kInstanceWidget = 0x10;
constexpr uint32_t kInstanceFlags = 0xB8;
constexpr uint8_t kInstanceEnabled = 2;
constexpr uint32_t kFocusSlot = 0x0C;
// widget vtable: +0xC type, +0x10 GetBounds(short rect[4] left right top bottom), +0x28 MouseDown, +0x2C MouseUp and
// +0x34 MouseMove (instance, event: +4 point), +0x3C MouseLeave(instance)
// Slots are byte offsets into the vtable.
constexpr uint32_t kTypeSlot = 0x0C;
constexpr uint32_t kGetBoundsSlot = 0x10;
constexpr uint32_t kMouseDownSlot = 0x28;
constexpr uint32_t kMouseUpSlot = 0x2C;
constexpr uint32_t kMouseMoveSlot = 0x34;
constexpr uint32_t kMouseLeaveSlot = 0x3C;
// widget types: the focusable ones are 1, 2, 3, 7, a list and a slider
constexpr uint32_t kListType = 8;
constexpr uint32_t kSliderType = 10;
// a short rect[4]: left, right, top, bottom
constexpr uint32_t kRectRight = 0x02;
constexpr uint32_t kRectTop = 0x04;
constexpr uint32_t kRectBottom = 0x06;
// list: +0x30 up arrow, +0x44 down arrow, +0x54 selection, +0x68 begin, +0x6C end of its 32 byte items, +0x88 up arrow
// rect, +0x98 down arrow rect
constexpr uint32_t kListUpArrow = 0x30;
constexpr uint32_t kListDownArrow = 0x44;
constexpr uint32_t kListSelection = 0x54;
constexpr uint32_t kListItemsBegin = 0x68;
constexpr uint32_t kListItemsEnd = 0x6C;
constexpr uint32_t kListItemSize = 32;
constexpr uint32_t kListUpRect = 0x88;
constexpr uint32_t kListDownRect = 0x98;
// slider: +0x5C track, +0x70 float value, +0x80 vertical, +0x88 decrease arrow rect, +0x90 increase arrow rect, +0x98
// thumb rect, +0xA0 track rect, +0xAC flags (4 the thumb is dragged: MouseMove sets the value under the point)
constexpr uint32_t kSliderTrack = 0x5C;
constexpr uint32_t kSliderVertical = 0x80;
constexpr uint32_t kSliderDecreaseRect = 0x88;
constexpr uint32_t kSliderIncreaseRect = 0x90;
constexpr uint32_t kSliderThumbRect = 0x98;
constexpr uint32_t kSliderTrackRect = 0xA0;
constexpr uint32_t kSliderValue = 0x70;
constexpr uint32_t kSliderFlags = 0xAC;
constexpr uint8_t kSliderDragged = 4;
// page units, measured on screen: with a track an arrow is drawn inside its image, from its image's left / right
constexpr int16_t kTrackDecreaseLeft = 1;
constexpr int16_t kTrackDecreaseRight = -5;
constexpr int16_t kTrackIncreaseLeft = 3;
constexpr int16_t kTrackIncreaseRight = -3;
// with no track the arrow images are laid out away from the drawn arrows (Aiming Sensitivity): the decrease arrow from
// its image's left, the increase arrow from its image's right
constexpr int16_t kNoTrackDecreaseFrom = -5;
constexpr int16_t kNoTrackDecreaseTo = 3;
constexpr int16_t kNoTrackIncreaseFrom = -11;
constexpr int16_t kNoTrackIncreaseTo = -3;
// page and list calls (thiscall)
constexpr uint32_t kPageSetFocus = 0x25C420;
constexpr uint32_t kListHitTest = 0x25DF00;
constexpr uint32_t kListSelect = 0x25EE00;
// instance thiscall(event): runs its listeners' key release slot, as the list's own release handler 0x25F4A0 does
constexpr uint32_t kListenersKeyUp = 0x25D3D0;
// Magma key codes a list steps on
constexpr uint32_t kMagmaUp = 0x26;
constexpr uint32_t kMagmaDown = 0x28;
// pages whose input is not a menu's
constexpr uint32_t kHudPages[] = {game::magma::Id("p_hudatt"), game::magma::Id("p_interatt")};
constexpr uint32_t kPopUp = game::magma::Id("p_popupmessageauto");
// viewport +0x18 client, +0x28 engine, vtable +0x7C InputEvent(viewport, key, action, delta)
constexpr uint32_t kViewportClient = 0x18;
constexpr uint32_t kClientEngine = 0x28;
constexpr uint32_t kInputEventSlot = 0x7C;
constexpr uint32_t kKeyA = 0xC8;
constexpr uint32_t kKeyUp = 0xD0;
constexpr uint32_t kKeyDown = 0xD1;
constexpr uint32_t kKeyLeft = 0xD2;
constexpr uint32_t kKeyRight = 0xD3;
constexpr uint32_t kPress = 1;
constexpr uint32_t kRelease = 3;

struct Ui {
    X86Context& ctx;
    uint32_t viewport;
    uint32_t scratch;  // scratch: point at +0, rect at +8, mouse event at +0x10
};
constexpr uint32_t kPointY = 0x02;
constexpr uint32_t kScratchRect = 0x08;
constexpr uint32_t kScratchEvent = 0x10;
constexpr uint32_t kScratchEventPoint = 0x14;  // the mouse event's +4 point

uint32_t Call(Ui& u, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args = {}) {
    u.ctx.ecx = self;
    return CallGuest(u.ctx, fn, args);
}

uint32_t Virtual(Ui& u, uint32_t self, uint32_t offset, std::initializer_list<uint32_t> args = {}) {
    return Call(u, R32(R32(self) + offset), self, args);
}

void Tap(Ui& u, uint32_t key) {
    uint32_t engine = R32(R32(u.viewport + kViewportClient) + kClientEngine);
    Virtual(u, engine, kInputEventSlot, {u.viewport, key, kPress, 0});
    Virtual(u, engine, kInputEventSlot, {u.viewport, key, kRelease, 0});
}

bool Shown(uint32_t instance) {
    return G<uint8_t>(instance + kInstanceShown) && !G<uint8_t>(instance + kInstanceHidden);
}

bool Focusable(Ui& u, uint32_t instance) {
    uint32_t widget = R32(instance + kInstanceWidget);
    if (!widget || !Shown(instance)) return false;
    switch (Virtual(u, widget, kTypeSlot)) {
        case 1: case 2: case 3: case 7: case kListType: case kSliderType:
            return G<uint8_t>(instance + kInstanceFlags) & kInstanceEnabled;
        default: return false;
    }
}

bool Inside(Ui& u, uint32_t rect) {
    int16_t x = int16_t(R16(u.scratch));
    int16_t y = int16_t(R16(u.scratch + kPointY));
    int16_t l = int16_t(R16(rect));
    int16_t r = int16_t(R16(rect + kRectRight));
    int16_t t = int16_t(R16(rect + kRectTop));
    int16_t b = int16_t(R16(rect + kRectBottom));
    return l < r && t < b && x >= l && x <= r && y >= t && y <= b;
}

// A click steps a horizontal slider only on its drawn arrows, which can sit away from their images (Aiming
// Sensitivity) and are smaller than them: one on an arrow steps with a point inside its image, false for any other
// (a thumb is dragged, Voice Masking's value box takes nothing). The point to step with, none for no step.
std::optional<uint32_t> SliderPoint(uint32_t widget, uint32_t point) {
    if (R32(widget + kSliderVertical)) return point;
    int16_t x = int16_t(point);
    int16_t y = int16_t(point >> 16);
    auto at = [&](uint32_t rect, int16_t from, int16_t to) {
        int16_t t = int16_t(R16(rect + kRectTop));
        int16_t b = int16_t(R16(rect + kRectBottom));
        return y >= t && y <= b && x >= from && x <= to;
    };
    uint32_t down = widget + kSliderDecreaseRect;
    uint32_t up = widget + kSliderIncreaseRect;
    int16_t down_left = int16_t(R16(down));
    int16_t down_right = int16_t(R16(down + kRectRight));
    int16_t up_left = int16_t(R16(up));
    int16_t up_right = int16_t(R16(up + kRectRight));
    bool track = R32(widget + kSliderTrack);
    auto centre = [](uint32_t rect) {
        int16_t cx = (int16_t(R16(rect)) + int16_t(R16(rect + kRectRight))) / 2;
        int16_t cy = (int16_t(R16(rect + kRectTop)) + int16_t(R16(rect + kRectBottom))) / 2;
        return uint32_t(uint16_t(cx)) | uint32_t(uint16_t(cy)) << 16;
    };
    bool on_down = false;
    if (track) on_down = at(down, down_left + kTrackDecreaseLeft, down_right + kTrackDecreaseRight);
    else on_down = at(down, down_left + kNoTrackDecreaseFrom, down_left + kNoTrackDecreaseTo);
    if (on_down) return centre(down);
    bool on_up = false;
    if (track) on_up = at(up, up_left + kTrackIncreaseLeft, up_right + kTrackIncreaseRight);
    else on_up = at(up, up_right + kNoTrackIncreaseFrom, up_right + kNoTrackIncreaseTo);
    if (on_up) return centre(up);
    return std::nullopt;
}

uint32_t g_page = 0;
uint32_t g_clicks = 0;
uint32_t g_point = ~0u;
bool g_pressing = false;
uint32_t g_drag = 0;  // the slider instance a held press drags

// the list's item under the point, -1 on none: its rows past the last item hit too
int32_t ListItem(Ui& u, uint32_t list) {
    int32_t hit = int32_t(Call(u, kListHitTest, list, {u.scratch}));
    return hit < int32_t((R32(list + kListItemsEnd) - R32(list + kListItemsBegin)) / kListItemSize) ? hit : -1;
}

bool ListArrow(Ui& u, uint32_t list) {
    return (R32(list + kListUpArrow) && Inside(u, list + kListUpRect)) ||
           (R32(list + kListDownArrow) && Inside(u, list + kListDownRect));
}

void Mouse(Ui& u, uint32_t instance, uint32_t handler, uint32_t point) {
    W32(u.scratch + kScratchEvent, 0);
    W32(u.scratch + kScratchEventPoint, point);
    Virtual(u, R32(instance + kInstanceWidget), handler, {instance, u.scratch + kScratchEvent});
}

// a slider with a draggable thumb: the press takes the thumb to the point, and its value to the nearest whole step
// (the audio volumes go 0 to 10)
bool Draggable(uint32_t widget) {
    uint32_t thumb = widget + kSliderThumbRect;
    uint32_t bar = widget + kSliderTrackRect;
    // left and right, then top and bottom, one read each
    return R32(widget + kSliderTrack) && (R32(thumb) != R32(bar) || R32(thumb + kRectTop) != R32(bar + kRectTop));
}

void Drag(Ui& u, uint32_t handler, uint32_t point) {
    Mouse(u, g_drag, handler, point);
    float& value = G<float>(R32(g_drag + kInstanceWidget) + kSliderValue);
    value = std::round(value);
}

constexpr float kPageWidth = 1.1f;  // pages are drawn this wide about the screen's centre

// window cursor to page space: pages are 4:3 in the middle of the frame, drawn 1.1 wide; none without a cursor
std::optional<uint32_t> CursorPoint() {
    int x;
    int y;
    int w;
    int h;
    if (!input::Cursor(x, y, w, h) || w <= 0 || h <= 0) return std::nullopt;
    uint32_t drawer = R32(kDrawer);
    float sw = drawer ? float(int16_t(R16(drawer + kDrawerWidth))) : 0;
    float sh = drawer ? float(int16_t(R16(drawer + kDrawerHeight))) : 0;
    if (sw <= 0 || sh <= 0) {
        sw = 640;
        sh = 480;
    }
    float aspect = app::Aspect();
    float k = app::Narrow();
    float vw = std::min(float(w), h * aspect);
    float vh = vw / aspect;
    float fx = ((x - (w - vw) / 2) / vw - (1 - k) / 2) / k;
    fx = 0.5f + (fx - 0.5f) / kPageWidth;
    int px = int(fx * sw);
    int py = int((y - (h - vh) / 2) / vh * sh);
    return uint32_t(uint16_t(int16_t(px))) | uint32_t(uint16_t(int16_t(py))) << 16;
}

// a held press keeps dragging the slider it began on wherever the cursor goes; letting go ends it, click and all
bool HeldDrag(Ui& u, uint32_t focus, uint32_t point, bool pressing, bool moved) {
    if (g_drag && (g_drag != focus || !Shown(g_drag))) g_drag = 0;
    if (!g_drag) return false;
    if (pressing) {
        if (moved) Drag(u, kMouseMoveSlot, point);
        return true;
    }
    Drag(u, kMouseMoveSlot, point);
    Drag(u, kMouseUpSlot, point);
    Virtual(u, R32(g_drag + kInstanceWidget), kMouseLeaveSlot, {g_drag});
    g_drag = 0;
    return true;
}

// the first focusable instance under the point, or the focused one when it is there too; 0 for none
uint32_t Hovered(Ui& u, uint32_t top, uint32_t focus) {
    uint32_t hovered = 0;
    for (uint32_t it = R32(top + kPageBegin), end = R32(top + kPageEnd); it < end; it += 4) {
        uint32_t instance = R32(it);
        if (!instance || !Focusable(u, instance)) continue;
        uint32_t widget = R32(instance + kInstanceWidget);
        Virtual(u, widget, kGetBoundsSlot, {u.scratch + kScratchRect});
        uint32_t type = Virtual(u, widget, kTypeSlot);
        // a slider's bounds are its arrows alone: its row reaches back over the label on its left
        if (type == kSliderType) W16(u.scratch + kScratchRect, 0);
        // a list's bounds hold its empty rows too (Session parameters' rows down to ADVANCED PARAMETERS)
        if (!Inside(u, u.scratch + kScratchRect)) continue;
        if (type == kListType && ListItem(u, widget) < 0 && !ListArrow(u, widget)) continue;
        if (!hovered || instance == focus) hovered = instance;
    }
    return hovered;
}

void StartDrag(Ui& u, uint32_t hovered, uint32_t widget, uint32_t point) {
    Mouse(u, hovered, kMouseDownSlot, point);
    G<uint8_t>(widget + kSliderFlags) |= kSliderDragged;
    g_drag = hovered;
    Drag(u, kMouseMoveSlot, point);
}

void StepSlider(Ui& u, uint32_t hovered, uint32_t widget, uint32_t at) {
    if (!R32(widget + kSliderTrack)) {
        // with no track the page keeps the value (Advanced parameters' sliders stay at their minimum): the D-pad
        // steps it as on the pad
        Tap(u, int16_t(at) < int16_t(R16(widget + kSliderIncreaseRect)) ? kKeyLeft : kKeyRight);
    } else {
        // its own mouse handlers step on the arrows
        Mouse(u, hovered, kMouseDownSlot, at);
        Mouse(u, hovered, kMouseUpSlot, at);
        Virtual(u, widget, kMouseLeaveSlot, {hovered});
    }
}

// Pages that act on the key's release (Visit a map's picture and description) are told of it as the list would.
void ListKeyUp(Ui& u, uint32_t instance, uint32_t key) {
    W32(u.scratch + kScratchEvent, key);
    W32(u.scratch + kScratchEventPoint, 0);
    Call(u, kListenersKeyUp, instance, {u.scratch + kScratchEvent});
}

// The hovered item is reached by D-pad steps, as on the pad: the page acts on the step (the session page's map and
// game mode lists change its picture and briefing), not on a selection set from outside. A step that does not move
// the list leaves the rest to a direct select.
void ListClick(Ui& u, uint32_t hovered, uint32_t widget, bool clicked) {
    int32_t hit = ListItem(u, widget);
    int32_t steps = int32_t((R32(widget + kListItemsEnd) - R32(widget + kListItemsBegin)) / kListItemSize);
    int32_t from = int32_t(R32(widget + kListSelection));
    for (int32_t at = from; hit >= 0 && at != hit && steps-- > 0;) {
        Tap(u, hit < at ? kKeyUp : kKeyDown);
        int32_t now = int32_t(R32(widget + kListSelection));
        if (now == at) {
            Call(u, kListSelect, widget, {uint32_t(hit), 0});
            Virtual(u, hovered, kFocusSlot, {0, 1});
            break;
        }
        at = now;
    }
    if (hit >= 0 && hit != from) ListKeyUp(u, hovered, hit < from ? kMagmaUp : kMagmaDown);
    if (!clicked) return;
    if (R32(widget + kListUpArrow) && Inside(u, widget + kListUpRect)) Tap(u, kKeyUp);
    else if (R32(widget + kListDownArrow) && Inside(u, widget + kListDownRect)) Tap(u, kKeyDown);
    else if (hit >= 0) Tap(u, kKeyA);
}

// the top page of the input stack, 0 for none
uint32_t TopInputPage() {
    uint32_t m = R32(kMagma);
    if (!m) return 0;
    uint32_t first = R32(m + kStackBegin);
    uint32_t last = R32(m + kStackEnd);
    if (!first || last <= first) return 0;
    return R32(last - kStackEntry);
}

struct Pointer {
    uint32_t point;
    bool moved;
    bool clicked;
    bool pressing;
    bool pressed;
};

// this update's cursor and buttons against the last's, which they replace
Pointer UpdatePointer(uint32_t top) {
    Pointer p;
    p.point = g_point;
    uint32_t clicks = input::Clicks();
    // a new page starts from where the cursor rests
    bool fresh = top != g_page;
    std::optional<uint32_t> cursor = CursorPoint();
    if (cursor) p.point = *cursor;
    p.moved = cursor && p.point != g_point && !fresh;
    p.clicked = clicks != g_clicks && !fresh;
    p.pressing = input::Pressing() && !fresh;
    p.pressed = p.pressing && !g_pressing;
    g_pressing = p.pressing;
    if (fresh) g_drag = 0;
    g_page = top;
    g_point = p.point;
    g_clicks = clicks;
    return p;
}

// true while a menu takes the input: the top input page shows its focused control, or is the "press A" pop up.
// The HUD pages keep a hidden list focused through play, shown for the pad alone.
bool UpdateMenu(Ui& u) {
    uint32_t top = TopInputPage();
    uint32_t id = top ? R32(top + kPageId) : 0;
    uint32_t focus = top ? R32(top + kPageFocused) : 0;
    Pointer p = UpdatePointer(top);
    bool hud = std::ranges::find(kHudPages, id) != std::end(kHudPages);
    if (hud || !(id == kPopUp || (focus && Shown(focus)))) return false;
    if (id == kPopUp && p.clicked) Tap(u, kKeyA);
    if (id == kPopUp) return true;
    W32(u.scratch, p.point);
    if (HeldDrag(u, focus, p.point, p.pressing, p.moved)) return true;
    if (!p.moved && !p.clicked && !p.pressed) return true;
    uint32_t hovered = Hovered(u, top, focus);
    if (!hovered) return true;
    if (hovered != focus) Call(u, kPageSetFocus, top, {hovered});
    uint32_t widget = R32(hovered + kInstanceWidget);
    uint32_t type = Virtual(u, widget, kTypeSlot);
    // a click too quick to be seen held still takes the thumb there (the next frame lets it go)
    if (type == kSliderType && (p.pressed || p.clicked) && Draggable(widget) &&
        (Inside(u, widget + kSliderTrackRect) || Inside(u, widget + kSliderThumbRect))) {
        StartDrag(u, hovered, widget, p.point);
        return true;
    }
    if (type == kSliderType && p.clicked) {
        if (std::optional<uint32_t> at = SliderPoint(widget, p.point)) StepSlider(u, hovered, widget, *at);
    }
    if (type != kListType) {
        if (p.clicked && type != kSliderType) Tap(u, kKeyA);
        return true;
    }
    ListClick(u, hovered, widget, p.clicked);
    return true;
}

}  // namespace

// thiscall(viewport, reset, float delta seconds), ret 8
X86_EXTERN_FUNC(__imp__sub_00188E80);
X86_FUNC(sub_00188E80) {
    uint32_t viewport = ctx.ecx;
    __imp__sub_00188E80(ctx, base);
    g_player = R32(viewport + kViewportActor);
    Zoom(g_player);
    // guest calls go below a scratch block under the caller's stack
    uint32_t esp = ctx.esp;
    constexpr uint32_t kScratchSize = 0x40;
    constexpr uint32_t kBelowScratch = 0x10;
    Ui u{ctx, viewport, (esp - kScratchSize) & ~0xFu};
    ctx.esp = u.scratch - kBelowScratch;
    bool menu = UpdateMenu(u);
    input::SetMenu(menu);
    game::NameEntryTick(ctx, g_page);
    versus::OptionsTick(ctx, g_page);
    for (int n = input::TakeMenuWheel(); menu && n; n += n > 0 ? -1 : 1) Tap(u, n > 0 ? kKeyUp : kKeyDown);
    // set and cleared on the key's edges only, as the button command does: Y's own presses keep it otherwise
    bool reload = input::EventKeyHeld(input::KeyEvent::Reload) && !menu;
    if (g_player && reload != g_reload) G<uint8_t>(g_player + kReloadButton) = reload;
    g_reload = reload;
    ctx.esp = esp;
    // counts from a menu or a load are dropped, not saved up for the next look
    if (!input::TakeMouse(g_dx, g_dy)) {
        g_dx = 0.0f;
        g_dy = 0.0f;
    }
}

// the stick's turn and look up rates times the controller sensitivities
void ScalePadLook(uint32_t controller) {
    if (!Looking(controller)) return;
    input::LookScale scale = input::PadLookScale(g_binoculars || WeaponRaised(controller));
    float turn = std::bit_cast<float>(R32(controller + kTurnRate));
    float up = std::bit_cast<float>(R32(controller + kLookRate));
    WriteFloat(controller + kTurnRate, turn * scale.x);
    WriteFloat(controller + kLookRate, up * scale.y);
    float before = std::hypot(turn, up);
    if (!g_binoculars || before == 0.0f) return;
    // the binoculars turn by the rates' direction times the magnitude
    float magnitude = std::bit_cast<float>(R32(controller + kMagnitude));
    magnitude *= std::hypot(turn * scale.x, up * scale.y) / before;
    WriteFloat(controller + kMagnitude, magnitude);
    WriteFloat(controller + kSmoothMagnitude, magnitude);
}

// thiscall(controller, float delta seconds), ret 4
X86_EXTERN_FUNC(__imp__sub_0003FB80);
X86_FUNC(sub_0003FB80) {
    uint32_t controller = ctx.ecx;
    float seconds = std::bit_cast<float>(R32(ctx.esp + 4));
    __imp__sub_0003FB80(ctx, base);
    if (controller != g_player) return;
    bool mouse = (g_dx != 0.0f || g_dy != 0.0f) && seconds > 0.0f && Looking(controller);
    PitchSpeed(controller, mouse);
    if (!mouse) {
        ScalePadLook(controller);
        return;
    }
    // the stick's up is positive, the mouse's is negative
    bool invert = R32(controller + kOptions) & kInvertLook;
    float view = ViewSensitivity(controller);
    const app::Settings& s = app::Get();
    bool aim = g_binoculars || WeaponRaised(controller);
    float turn = Rate(g_dx, seconds) * view * (aim ? s.aim_horizontal_sensitivity : 1.0f);
    float up = Rate(-g_dy, seconds) * view * (aim ? s.aim_vertical_sensitivity : 1.0f);
    if (turn != 0.0f) Look(controller + kTurnRate, controller + kTurn, controller + kTurnMax, turn, false);
    if (up != 0.0f) Look(controller + kLookRate, controller + kLookUp, controller + kLookMax, up, invert);
    // the binoculars turn by the rates' direction times this magnitude, so it is left above full stick there
    float magnitude = std::sqrt(turn * turn + up * up) / kFullStick;
    if (!g_binoculars) magnitude = std::min(1.0f, magnitude);
    WriteFloat(controller + kMagnitude, magnitude);
    WriteFloat(controller + kSmoothMagnitude, magnitude);
}
