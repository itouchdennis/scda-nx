// Keypads on the keyboard and mouse, as SCCT PC. Numpad digits press their button, Backspace, Delete and Numpad *
// press * (which clears the code) and Numpad . presses #. v2's keypad key handler (sub_001661F2) still has SCCT PC's
// table, but on the console it only selects the button and no key ever reaches it, so the button is selected, pushed
// and lit here, as the pad's Interaction press does. As SCCT PC, a mouse move selects the button under the cursor (none
// off the keys) and a click presses it; SCCT traced a cursor ray it had through the buttons, here each button's place
// is projected with the frame's view. With none selected a press of A would index the buttons with -1, so it does
// nothing, and the first move (move keys, arrows, stick) lands on 5, where the keypad starts.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>

#include "app/app.h"
#include "core/input.h"
#include "game/unreal.h"
#include "xdk/hle.h"

using namespace kernel;
namespace unreal = game::unreal;

namespace game {
bool ProjectModel(const float point[3], float& x, float& y);  // fov.cpp
}

namespace {

constexpr uint32_t kViewportActor = 0x34;
// EKeyPadInteraction +0x2D4 MyKeyPad; EKeyPad +0x3FC SelectedButton (0-8 the digits 1-9, 9 *, 10 0, 11 #),
// +0x458 KeyButtons data, +0x45C count, in SelectedButton's order
constexpr uint32_t kMyKeyPad = 0x2D4, kSelectedButton = 0x3FC, kButtons = 0x458, kButtonCount = 0x45C;
constexpr uint32_t kMaxButtons = 12;
// thiscall on the keypad, no arguments
constexpr uint32_t kGlowSelected = 0x14AD42, kKeyPushed = 0x14AE47;
// Each button actor stands at the keypad's pivot, its mesh modelled in place. A button is under the cursor inside
// its key top: the face of the mesh's bounding box (UStaticMesh +0x2C min, +0x38 max, local) at max x, the side the
// keys stand out to, turned by the actor's Rotation and moved to its Location (scale 1).
constexpr uint32_t kStaticMesh = 0x118, kBoxMin = 0x2C, kBoxMax = 0x38;
constexpr float kUnrealToRadians = 3.14159265f / 32768.0f;

uint32_t g_keypad = 0, g_clicks = 0;
int g_x = -1, g_y = -1;

// the keypad Sam is at, 0 for none
uint32_t KeypadOf(uint32_t pc) {
    uint32_t state = pc ? unreal::StateOf(pc) : 0;
    if (state != unreal::Name("s_KeyPadInteract") && state != unreal::Name("s_KeyPadInteractHanged")) return 0;
    uint32_t offset = unreal::Offset(pc, unreal::Name("Interaction"));
    uint32_t interaction = offset ? R32(pc + offset) : 0;
    if (!interaction || !unreal::IsA(interaction, unreal::Name("EKeyPadInteraction"))) return 0;
    return R32(interaction + kMyKeyPad);
}

// the handler takes keys only while the keypad is in use (not showing granted / denied)
bool InUse(uint32_t keypad) { return keypad && unreal::StateOf(keypad) == unreal::Name("s_Use"); }

void Select(X86Context& ctx, uint32_t keypad, int button, bool push) {
    W32(keypad + kSelectedButton, uint32_t(button));
    if (push) {
        ctx.ecx = keypad;
        CallGuest(ctx, kKeyPushed, {});
    }
    ctx.ecx = keypad;
    CallGuest(ctx, kGlowSelected, {});
}

// the key top's corners in window pixels, in order around it; false when the button cannot be placed
bool KeyTop(uint32_t button, float left, float top, float vw, float vh, float px[4], float py[4]) {
    uint32_t location = unreal::Offset(button, unreal::Name("Location"));
    uint32_t rotation = unreal::Offset(button, unreal::Name("Rotation"));
    uint32_t mesh = R32(button + kStaticMesh);
    if (!location || !rotation || !mesh) return false;
    auto angle = [&](uint32_t i) { return float(int32_t(R32(button + rotation + i * 4))) * kUnrealToRadians; };
    float p = angle(0), yaw = angle(1), r = angle(2);
    float sp = std::sin(p), cp = std::cos(p), sy = std::sin(yaw), cy = std::cos(yaw), sr = std::sin(r), cr = std::cos(r);
    // FRotationMatrix's rows (UE2): local x y z to world
    const float axes[3][3] = {{cp * cy, cp * sy, sp},
                              {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
                              {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
    float lo[3], hi[3];
    for (uint32_t a = 0; a < 3; a++) lo[a] = GF(mesh + kBoxMin + a * 4), hi[a] = GF(mesh + kBoxMax + a * 4);
    const float corners[4][2] = {{lo[1], lo[2]}, {hi[1], lo[2]}, {hi[1], hi[2]}, {lo[1], hi[2]}};
    for (uint32_t c = 0; c < 4; c++) {
        const float local[3] = {hi[0], corners[c][0], corners[c][1]};
        float world[3], nx, ny;
        for (uint32_t k = 0; k < 3; k++) {
            world[k] = GF(button + location + k * 4);
            for (uint32_t a = 0; a < 3; a++) world[k] += local[a] * axes[a][k];
        }
        if (!game::ProjectModel(world, nx, ny)) return false;
        px[c] = left + (nx + 1.0f) * 0.5f * vw;
        py[c] = top + (1.0f - ny) * 0.5f * vh;
    }
    return true;
}

// inside a convex quad: the point is on the same side of all four edges
bool Inside(const float px[4], const float py[4], float x, float y) {
    bool neg = false, pos = false;
    for (uint32_t c = 0; c < 4; c++) {
        uint32_t n = (c + 1) % 4;
        float cross = (px[n] - px[c]) * (y - py[c]) - (py[n] - py[c]) * (x - px[c]);
        neg |= cross < 0.0f;
        pos |= cross > 0.0f;
    }
    return !(neg && pos);
}

// the button under the window point, -1 for none. The view fills the shown frame, in the middle of the window
int ButtonAt(uint32_t keypad, int x, int y, int w, int h) {
    uint32_t count = std::min(R32(keypad + kButtonCount), kMaxButtons), data = R32(keypad + kButtons);
    float aspect = app::Aspect();
    float vw = std::min(float(w), h * aspect), vh = vw / aspect, left = (w - vw) / 2, top = (h - vh) / 2;
    for (uint32_t i = 0; data && i < count; i++) {
        uint32_t button = R32(data + i * 4);
        float px[4], py[4];
        if (button && KeyTop(button, left, top, vw, vh, px, py) && Inside(px, py, float(x), float(y))) return int(i);
    }
    return -1;
}

void Mouse(X86Context& ctx, uint32_t keypad) {
    input::SetPointer(keypad != 0);
    bool entered = keypad != g_keypad;
    g_keypad = keypad;
    int x, y, w, h;
    bool cursor = input::Cursor(x, y, w, h) && w > 0 && h > 0;
    uint32_t clicks = input::Clicks();
    // a keypad starts from where the cursor rests, only movement or a click after it acts
    bool clicked = clicks != g_clicks && !entered, moved = cursor && (x != g_x || y != g_y) && !entered;
    g_clicks = clicks;
    if (cursor) g_x = x, g_y = y;
    if (!keypad || !cursor || !input::KeyboardActive() || (!entered && !moved && !clicked)) return;
    int button = ButtonAt(keypad, x, y, w, h);
    if (!InUse(keypad)) return;
    if (clicked && button >= 0) Select(ctx, keypad, button, true);
    else if (moved && button != int32_t(R32(keypad + kSelectedButton))) Select(ctx, keypad, button, false);
}

}  // namespace

namespace game {

void KeypadKeys(X86Context& ctx, uint32_t viewport, bool menu, bool net) {
    int button = input::TakeKeypadButton();
    // a synced session's lockstep carries the pad, not what the game side does on its own
    uint32_t keypad = menu || net ? 0 : KeypadOf(R32(viewport + kViewportActor));
    if (button >= 0 && InUse(keypad)) Select(ctx, keypad, button, true);
    Mouse(ctx, keypad);
}

}  // namespace game

// KeyPushed (thiscall keypad): indexes KeyButtons with SelectedButton unchecked
X86_EXTERN_FUNC(__imp__sub_0014AE47);
X86_FUNC(sub_0014AE47) {
    int32_t selected = int32_t(R32(ctx.ecx + kSelectedButton));
    if (selected < 0 || uint32_t(selected) >= R32(ctx.ecx + kButtonCount)) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_0014AE47(ctx, base);
}

// EKeyPadInteraction::KeyEvent (thiscall interaction; FString* alias, action, float delta, int key, bool auto; ret
// 0x14). Its grid moves from -1 go nowhere but Down (to 3); from none selected a move press starts on 5 instead.
X86_EXTERN_FUNC(__imp__sub_001661F2);
X86_FUNC(sub_001661F2) {
    static constexpr std::u16string_view kMoves[] = {u"AnalogUp",   u"MoveForward", u"AnalogDown",  u"MoveBackward",
                                                     u"AnalogLeft", u"StrafeLeft",  u"AnalogRight", u"StrafeRight"};
    constexpr uint32_t kPress = 1, kCentre = 4;
    uint32_t keypad = R32(ctx.ecx + kMyKeyPad), alias = R32(ctx.esp + 4);
    // FString: +0 characters, +4 count with the terminator
    uint32_t text = alias ? R32(alias) : 0, length = alias ? R32(alias + 4) : 0;
    std::u16string name;
    for (uint32_t i = 0; text && i + 1 < length && i < 32; i++) name += char16_t(R16(text + i * 2));
    if (keypad && int32_t(R32(keypad + kSelectedButton)) < 0 && R32(ctx.esp + 8) == kPress && !R32(ctx.esp + 20) &&
        InUse(keypad) && std::find(std::begin(kMoves), std::end(kMoves), name) != std::end(kMoves)) {
        uint32_t interaction = ctx.ecx;
        Select(ctx, keypad, kCentre, false);
        ctx.ecx = interaction;
        ctx.eax = 1;
        ctx.esp += 0x18;
        return;
    }
    __imp__sub_001661F2(ctx, base);
}
