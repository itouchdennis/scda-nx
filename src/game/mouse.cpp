// The mouse on the game side, after UXboxViewport::UpdateInput on the game thread.
// Play: raw counts go to UEngine::InputEvent as IK_MouseX and IK_MouseY axis events, as the PC viewport sends
// them; the game's ini binds them to aMouseX and aMouseY.
// Menus: the Magma page on top of the input stack. The cursor focuses the widget under it the way the neighbour
// navigation does and in a list selects the item under it; a click presses A, or scrolls on a list's arrows.
// The wheel steps up and down as the stick does, a notch a step; in play it steps the focused interaction list.
// On the OPSAT map the mouse turns the map while the right button is held: the map turns by aTurn and aLookUp.
// In a synced session the lockstep carries only the pad, so there the wheel taps D-pad up and down, in menus and
// the focused interaction list.
#include <algorithm>
#include <chrono>
#include <iterator>

#include "app/app.h"
#include "core/input.h"
#include "game/console.h"
#include "game/magma.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
void UpdateView(uint32_t viewport);               // views.cpp
void WhistleKeys(X86Context& ctx, uint32_t viewport);  // whistle.cpp
void KeypadKeys(X86Context& ctx, uint32_t viewport, bool menu, bool net);  // keypad.cpp
void GoalsTab(X86Context& ctx, uint32_t tab);  // opsat.cpp
void QuickSaveKeys(X86Context& ctx, uint32_t viewport, bool menu);  // quick_save.cpp
void MenuTick(X86Context& ctx);     // options_menu.cpp
void MenuPlace(X86Context& ctx);    // options_menu.cpp
bool BlankRow(uint32_t list, int row);  // options_menu.cpp
void PromptsTick(X86Context& ctx);  // prompts.cpp
void NameEntryTick(X86Context& ctx, uint32_t top);  // name_entry.cpp
bool InteractionListFocused(uint32_t player);  // interaction_list.cpp, player 0 any
}

namespace {

// the pages of opsat.mgm whose names are known, but P_VideoData: its videos skip on pause (Start), which is
// Escape there
constexpr uint32_t kOpsatPages[] = {
    game::magma::Id("P_Opsat"),    game::magma::Id("P_Goals"),     game::magma::Id("P_Goals_details"),
    game::magma::Id("P_Map"),      game::magma::Id("P_Notes"),     game::magma::Id("P_DataFile"),
    game::magma::Id("P_Datas"),    game::magma::Id("P_Equipment"), game::magma::Id("P_Communications"),
    game::magma::Id("P_PDA"),      game::magma::Id("P_Trust"),     game::magma::Id("P_Videos"),
};

// no menus to the mouse, which B leaves, and Escape does as on SCCT PC: a computer's file list (computer.mgm) and
// the hacking device's help and legend (hud.mgm, name not recovered)
constexpr uint32_t kComputer = game::magma::Id("P_Computer");
constexpr uint32_t kHacking = 0xE62F87FD;
constexpr uint32_t kMap = game::magma::Id("P_Map");

// engine: UXboxViewport::UpdateInput takes the synced input when +0xC8 and +0xCC or +0xDC are set
constexpr uint32_t kNetA = 0xC8, kNetB = 0xCC, kNetC = 0xDC;
// engine +0xD4: this machine's player number in a synced session, 1 the host, 2 the client
constexpr uint32_t kLocalPlayer = 0xD4;
constexpr uint32_t kMouseX = 0xE4, kMouseY = 0xE5, kJoyA = 0xCA, kAnalogUp = 0xC4, kAnalogDown = 0xC5;
constexpr uint32_t kPress = 1, kRelease = 3, kAxis = 4;

// manager: +0x3C contexts, +0x40 count; context: +0x14 draw stack, +0x18 input stack, +0x1C id;
// stack: +4 begin, +8 end of {page, flags}; screen: +0x84 width, +0x86 height
constexpr uint32_t kMagma = 0x6A6638, kMagmaScreen = 0x6A6668;
// a player's menus go to its own context (co-op's second player: id 2): viewport +0x70 menus interaction, its +0x3C
// menus manager, its +0x0C context id (sub_0028FD7E)
constexpr uint32_t kMenusInteraction = 0x70, kMenusManager = 0x3C, kManagerContext = 0x0C, kContextId = 0x1C;
// page: +8 instances begin, +0xC end, +0x14 id, +0x1C layer (100 HUD, the base default), +0x24 focused; instance: +8 widget, +0x18 bit 0 visible, +0x4C flags (2 enabled, 8 focused),
// vtable +0xC Focus, +0x10 Unfocus; widget: vtable +0xC type, +0x10 GetBounds(short rect[4] left right top bottom)
// list: +0x3C up arrow, +0x50 down arrow, +0x7C up arrow rect, +0x8C down arrow rect, +0x9C selection
constexpr uint32_t kListType = 10;
constexpr uint32_t kHudLayer = 100;
// held open in play and worked with the move keys: the mouse keeps looking around
constexpr uint32_t kQuickInventory = game::magma::Id("P_QuickInventory_New");
// the message box, among others "Controller Disconnected ... press START": up while a player has lost its port,
// which only a Start press on a free port claims back, and "choose another controller" when split screen starts
constexpr uint32_t kMsgBox = game::magma::Id("P_MsgBox");
constexpr uint32_t kVideoData = game::magma::Id("P_VideoData");
// first viewport: +0x1EC player 1's port, +0x1F0 player 2's, -1 for none; any viewport: +0x1F4 the port it reads;
// client +0x34 viewport count
constexpr uint32_t kPlayer1Port = 0x1EC, kPlayer2Port = 0x1F0, kReadPort = 0x1F4, kViewportCount = 0x34;
uint32_t g_keyboard_player = 0;  // split screen: the player on the keyboard's port, else 0
// P_Goals' tab buttons Primary, Opposing, Opportunity (names not recovered): not focusable, the pad steps them with
// Left / Right, so a click on one goes to it
constexpr uint32_t kGoals = game::magma::Id("P_Goals");
constexpr uint32_t kGoalsTabs[] = {0xDFFFD221, 0xCE9F9174, 0xF2D85AF3};
// a hidden button that takes a page's buttons (mission end and review, loadout); the pad cannot move focus off it,
// a click anywhere presses A on it
constexpr uint32_t kEventsCatcher = game::magma::Id("B_EventsCatcher");
constexpr uint32_t kListHitTest = 0x2C1190, kListSelect = 0x2C2040, kPageSetFocus = 0x2C5370;
// one press is held, then let go, each longer than a co-op step at the game's own 22 Hz
constexpr uint16_t kDpadUp = 1 << 0, kDpadDown = 1 << 1;
constexpr uint64_t kWheelTapMs = 50;
constexpr auto kWheelTapGap = std::chrono::milliseconds(2 * kWheelTapMs);

int g_pad_wheel = 0;  // notches not pressed yet, up positive
std::chrono::steady_clock::time_point g_pad_wheel_next;

struct Game {
    X86Context& ctx;
    uint32_t engine, viewport, scratch;  // scratch: point at +0, rect at +8
};

uint32_t Call(X86Context& ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    ctx.ecx = self;
    return CallGuest(ctx, fn, args);
}

uint32_t Virtual(X86Context& ctx, uint32_t self, uint32_t offset, std::initializer_list<uint32_t> args = {}) {
    return Call(ctx, R32(R32(self) + offset), self, args);
}

void Event(Game& g, uint32_t key, uint32_t action, float delta) {
    uint32_t d;
    std::memcpy(&d, &delta, 4);
    Call(g.ctx, R32(R32(g.engine) + 0x78), g.engine, {g.viewport, key, action, d});
}

void Tap(Game& g, uint32_t key) {
    Event(g, key, kPress, 1.0f);
    Event(g, key, kRelease, 1.0f);
}

// the top of the viewport's own context's input stack, else of the first context that has one
uint32_t InputPage(uint32_t viewport) {
    uint32_t m = R32(kMagma);
    uint32_t list = m ? R32(m + 0x3C) : 0, count = m ? R32(m + 0x40) : 0;
    if (!list || count > 16) return 0;
    uint32_t interaction = R32(viewport + kMenusInteraction);
    uint32_t manager = interaction ? R32(interaction + kMenusManager) : 0;
    uint32_t id = g_keyboard_player ? g_keyboard_player : manager ? R32(manager + kManagerContext) : 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t context = R32(list + i * 4);
        uint32_t stack = context ? R32(context + 0x18) : 0;
        uint32_t begin = stack ? R32(stack + 4) : 0, end = stack ? R32(stack + 8) : 0;
        if (!begin || end <= begin || end - begin > 8 * 64) continue;
        if (R32(context + kContextId) == id) return R32(end - 8);
        if (!first) first = R32(end - 8);
    }
    return first;
}

// a hidden widget can be enabled too: Load Game keeps its save list, taller than the mission list, under it
bool Focusable(Game& g, uint32_t instance) {
    if (!(G<uint8_t>(instance + 0x18) & 1) || !(G<uint8_t>(instance + 0x4C) & 2) || !R32(instance + 8)) return false;
    switch (Virtual(g.ctx, R32(instance + 8), 0xC)) {
        case 2: case 3: case 4: case 9: case 10: case 12: return true;
        default: return false;
    }
}

bool Inside(Game& g, uint32_t rect) {
    int16_t x = int16_t(R16(g.scratch)), y = int16_t(R16(g.scratch + 2));
    int16_t l = int16_t(R16(rect)), r = int16_t(R16(rect + 2)), t = int16_t(R16(rect + 4)), b = int16_t(R16(rect + 6));
    return l < r && t < b && x >= l && x <= r && y >= t && y <= b;
}

// a click on one of P_Goals' tabs
bool GoalsTabClick(Game& g, uint32_t page) {
    uint32_t begin = R32(page + 8), end = R32(page + 0xC);
    if (!begin || end < begin || end - begin > 4 * 512) return false;
    for (uint32_t it = begin; it < end; it += 4) {
        uint32_t instance = R32(it);
        const uint32_t* tab = instance ? std::ranges::find(kGoalsTabs, R32(instance + 4)) : std::end(kGoalsTabs);
        if (tab == std::end(kGoalsTabs) || !(G<uint8_t>(instance + 0x18) & 1) || !R32(instance + 8)) continue;
        Virtual(g.ctx, R32(instance + 8), 0x10, {g.scratch + 8});
        if (!Inside(g, g.scratch + 8)) continue;
        game::GoalsTab(g.ctx, uint32_t(tab - std::begin(kGoalsTabs)));
        return true;
    }
    return false;
}

uint32_t g_page = 0;
uint32_t g_clicks = 0;
uint32_t g_point = 0xFFFFFFFF;

// window cursor to page space: the frame is drawn in the middle of the window, menu pages 4:3 in its middle
bool Point(uint32_t& point) {
    int x, y, w, h;
    if (!input::Cursor(x, y, w, h) || w <= 0 || h <= 0) return false;
    uint32_t screen = R32(kMagmaScreen);
    float sw = screen ? float(int16_t(R16(screen + 0x84))) : 0, sh = screen ? float(int16_t(R16(screen + 0x86))) : 0;
    if (sw <= 0 || sh <= 0) sw = 640, sh = 480;
    float aspect = app::Aspect(), k = app::Narrow();
    float vw = std::min(float(w), h * aspect), vh = vw / aspect;
    float fx = ((x - (w - vw) / 2) / vw - (1 - k) / 2) / k;
    int px = int(fx * sw), py = int((y - (h - vh) / 2) / vh * sh);
    point = uint32_t(uint16_t(int16_t(px))) | uint32_t(uint16_t(int16_t(py))) << 16;
    LOG_FIRST(1, "mouse: menu space %gx%g", sw, sh);
    return true;
}

// true while a menu takes the input. The cursor acts only when act: in a synced session the lockstep carries the pad,
// and a focus moved, a list row picked or a key tapped here changes the menus on this machine alone (a click opening
// Objectives here only, the next B closing the OPSAT on the other machine, dropped the session).
bool Menu(Game& g, bool act) {
    uint32_t top = InputPage(g.viewport);
    if (top && R32(top + 0x14) == kQuickInventory) return false;
    // the HUD stays on the input stack through play with nothing focused, its widgets are not a menu. P_Trust and
    // P_Videos are OPSAT pages on the HUD layer: menus all the same, P_Trust with nothing to point at
    uint32_t layer = top ? R32(top + 0x1C) : 0, focus = top ? R32(top + 0x24) : 0;
    bool opsat = top && std::ranges::find(kOpsatPages, R32(top + 0x14)) != std::end(kOpsatPages);
    uint32_t page = (layer == kHudLayer && !opsat) || !focus ? 0 : top;
    uint32_t begin = page ? R32(page + 8) : 0, end = page ? R32(page + 0xC) : 0;
    if (end < begin || end - begin > 4 * 512) begin = end = 0;
    uint32_t focusable = 0, hovered = 0, focused = 0;
    uint32_t point = g_point, clicks = input::Clicks();
    // a new page starts from where the cursor rests, only movement or a click after it acts
    bool fresh = top != g_page;
    bool moved = Point(point) && point != g_point && !fresh && act, clicked = clicks != g_clicks && !fresh && act;
    g_point = point;
    g_clicks = clicks;
    W32(g.scratch, point);
    if (clicked && top && R32(top + 0x14) == kGoals && GoalsTabClick(g, top)) return true;
    for (uint32_t it = begin; it < end; it += 4) {
        uint32_t instance = R32(it);
        if (!instance || !Focusable(g, instance)) continue;
        focusable++;
        if (G<uint8_t>(instance + 0x4C) & 8) focused = instance;
        if (!moved && !clicked) continue;
        Virtual(g.ctx, R32(instance + 8), 0x10, {g.scratch + 8});
        if (Inside(g, g.scratch + 8) && (!hovered || (G<uint8_t>(instance + 0x4C) & 8))) hovered = instance;
    }
    if (fresh) {
        g_page = top;
        LOGI("mouse: input page %08X id %08X layer %u focus %08X, %u focusable, %s", top, top ? R32(top + 0x14) : 0,
             layer, focus, focusable, page ? "menu" : "not a menu");
    }
    if (!focusable) return page && opsat;
    if (focused && R32(focused + 4) == kEventsCatcher) {
        if (clicked) Tap(g, kJoyA);
        return true;
    }
    if (!hovered) return true;
    if (hovered != focused) Call(g.ctx, kPageSetFocus, page, {hovered});
    uint32_t widget = R32(hovered + 8);
    if (Virtual(g.ctx, widget, 0xC) != kListType) {
        if (clicked) Tap(g, kJoyA);
        return true;
    }
    int16_t hit = int16_t(Call(g.ctx, kListHitTest, widget, {g.scratch}));
    if (game::BlankRow(widget, hit)) hit = -1;
    if (hit >= 0 && hit != int16_t(R16(widget + 0x9C))) {
        Call(g.ctx, kListSelect, widget, {uint32_t(hit), 0, 1});
        Virtual(g.ctx, hovered, 0xC);
    }
    if (!clicked) return true;
    if (R32(widget + 0x3C) && Inside(g, widget + 0x7C)) Tap(g, kAnalogUp);
    else if (R32(widget + 0x50) && Inside(g, widget + 0x8C)) Tap(g, kAnalogDown);
    else if (hit >= 0) Tap(g, kJoyA);
    return true;
}

void PadWheel(int notches) {
    if (notches * g_pad_wheel < 0) g_pad_wheel = 0;
    g_pad_wheel += notches;
    auto now = std::chrono::steady_clock::now();
    if (!g_pad_wheel || now < g_pad_wheel_next) return;
    input::TapButtons(g_pad_wheel > 0 ? kDpadUp : kDpadDown, kWheelTapMs);
    g_pad_wheel += g_pad_wheel > 0 ? -1 : 1;
    g_pad_wheel_next = now + kWheelTapGap;
}

// The controller boxes, sub_001853BE thiscall(lost, show, choice) on the menus manager: message 0x30 / 0x31
// "reconnect the controller", 0x32 "choose another controller", each answered by Start on a free port. The manager's
// message list (+0x24: +0x30 entries, +0x34 count, +0x3D shown) has the one up last; an entry's +0 is its id.
uint32_t g_messages = 0;

// Split screen: the keyboard and mouse act in the update of the viewport reading the keyboard's port and on that
// player's menus (context id = player number); with one viewport, or the keyboard on no player yet, the first's
uint32_t KeyboardViewport(uint32_t client, uint32_t first) {
    uint32_t count = R32(client + kViewportCount), list = R32(client + 0x30);
    int32_t port = input::KeyboardPort();
    g_keyboard_player = 0;
    if (count < 2 || count > 4 || !first || port < 0) return first;
    g_keyboard_player = int32_t(R32(first + kPlayer1Port)) == port   ? 1
                        : int32_t(R32(first + kPlayer2Port)) == port ? 2
                                                                     : 0;
    for (uint32_t i = 0; g_keyboard_player && i < count; i++)
        if (uint32_t v = R32(list + i * 4); v && int32_t(R32(v + kReadPort)) == port) return v;
    g_keyboard_player = 0;
    return first;
}

bool ControllerBox() {
    uint32_t count = g_messages ? R32(g_messages + 0x34) : 0;
    if (!count || count > 64 || !G<uint8_t>(g_messages + 0x3D)) return false;
    uint32_t entry = R32(R32(g_messages + 0x30) + (count - 1) * 4);
    uint32_t id = entry ? R32(entry) : 0;
    return id >= 0x30 && id <= 0x32;
}

}  // namespace

namespace game {
uint32_t KeyboardPlayer() { return g_keyboard_player; }
}

X86_EXTERN_FUNC(__imp__sub_001853BE);
X86_FUNC(sub_001853BE) {
    g_messages = ctx.ecx + 0x24;
    __imp__sub_001853BE(ctx, base);
}

// viewport: +0x18 client; client: +0x2C engine, +0x30 viewports; engine vtable +0x78 InputEvent(viewport, key, action, delta)
X86_EXTERN_FUNC(__imp__sub_0004B525);
X86_FUNC(sub_0004B525) {
    uint32_t viewport = ctx.ecx;
    __imp__sub_0004B525(ctx, base);
    uint32_t client = R32(viewport + 0x18);
    uint32_t engine = client ? R32(client + 0x2C) : 0;
    uint32_t first = client ? R32(R32(client + 0x30)) : 0;
    if (!engine || viewport != KeyboardViewport(client, first)) return;
    static uint32_t logged = ~0u;
    if (g_keyboard_player != logged) {
        logged = g_keyboard_player;
        uint32_t interaction = R32(viewport + kMenusInteraction);
        uint32_t manager = interaction ? R32(interaction + kMenusManager) : 0;
        LOGI("split: keyboard player %u, viewport %08X (first %08X), its menus context %u", g_keyboard_player, viewport,
             first, manager ? R32(manager + kManagerContext) : 0);
    }
    // guest calls go below a scratch block under the caller's stack
    uint32_t esp = ctx.esp;
    Game g{ctx, engine, viewport, (esp - 0x40) & ~0xFu};
    ctx.esp = g.scratch - 0x10;
    game::MenuPlace(ctx);
    bool net = (R32(engine + kNetA) && R32(engine + kNetB)) || R32(engine + kNetC);
    bool menu = Menu(g, !net);
    input::SetMenu(menu);
    bool list = !menu && game::InteractionListFocused(net ? R32(engine + kLocalPlayer) : 0);
    input::SetInteractionList(list);
    // a synced session's lockstep carries the pad, not these taps
    int wheel = input::TakeMenuWheel();
    if (!net || !(menu || list)) g_pad_wheel = 0;
    else PadWheel(wheel);
    for (int n = wheel; (menu || list) && !net && n; n += n > 0 ? -1 : 1)
        Tap(g, n > 0 ? kAnalogUp : kAnalogDown);
    uint32_t top = InputPage(viewport);
    uint32_t page = top ? R32(top + 0x14) : 0;
    bool reconnect = page == kMsgBox && (int32_t(R32(first + kPlayer1Port)) == -1 ||
                                         (R32(client + kViewportCount) > 1 && int32_t(R32(first + kPlayer2Port)) == -1) ||
                                         ControllerBox());
    input::SetTitleScreen(page == game::magma::Id("P_Start") || reconnect);
    input::SetPauseMenu(page == game::magma::Id("P_Pause") || reconnect);
    input::SetQuickInventory(page == kQuickInventory);
    input::SetMap(page == kMap);
    input::SetOpsat(page && (std::ranges::find(kOpsatPages, page) != std::end(kOpsatPages) || page == kComputer ||
                             page == kHacking));
    input::SetTrainingVideo(page == kVideoData);
    game::UpdateView(viewport);
    game::MenuTick(ctx);
    game::PromptsTick(ctx);
    game::NameEntryTick(ctx, top);
    input::SetNetSession(net);
    float dx, dy;
    // in a synced session the mouse is recorded with the pad (coop_mouse.cpp); the axis command stores its value,
    // so each axis gets one summed event per update
    if (!net && input::TakeMouse(dx, dy) && (!menu || page == kMap)) {
        if (dx != 0.0f) Event(g, kMouseX, kAxis, dx);
        if (dy != 0.0f) Event(g, kMouseY, kAxis, dy);
    }
    if (!net) game::WhistleKeys(ctx, viewport);
    game::KeypadKeys(ctx, viewport, menu, net);
    game::QuickSaveKeys(ctx, viewport, menu);
    if (input::TakeEvent(input::KeyEvent::Reload) && !menu) game::QueueCommand("Reload", game::Route::Player);
    // exec Interact calls ProcessInteract straight, past the interaction list: s_Carry's puts the body down
    if (input::TakeEvent(input::KeyEvent::DropBody) && !menu) game::QueueCommand("Interact", game::Route::Player);
    game::RunCommands(ctx, engine, viewport);
    ctx.esp = esp;
}
