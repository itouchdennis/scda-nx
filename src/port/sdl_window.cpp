// The port's main thread, as app/main.cpp is on Windows: the SDL2 window and its event loop, which feeds the keyboard,
// mouse and focus into the Win32 view input.cpp reads (port/win32_input.h) and keeps the controllers (port/pads.h).
// The game runs on its own guest threads from kernel::Boot; this thread only pumps events until the player quits.
#include <SDL.h>
#ifdef SCDA_SDL_VULKAN
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "audio/output.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "port/pads.h"
#include "port/port.h"
#include "port/win32_input.h"

namespace dispatch {
bool Load(const std::filesystem::path& dll);
}

// the Switch's Vulkan renderer opens no SDL window: SDL's would put an EGL surface on the default NWindow, which
// Vulkan presents to itself (switch_platform.cpp). Input and sound need none
#if defined(SCDA_VK) && defined(SCDA_SWITCH)
#define SCDA_NO_WINDOW
#endif

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_focus{false};
std::atomic<bool> g_quit{false};
std::atomic<int> g_cursor_x{0}, g_cursor_y{0}, g_width{1280}, g_height{720};
std::atomic<uint32_t> g_mouse_buttons{0};  // VK_LBUTTON .. VK_XBUTTON2 as bits
std::atomic<int> g_want_relative{-1};       // ClipCursor's ask, -1 none waiting
Uint32 g_apply_event = 0;                   // ApplyDisplay from another thread

// SDL scan codes by virtual key, the US layout's keys (the bindings name keys by their US position)
struct KeyMap {
    int vk;
    SDL_Scancode scan;
};
constexpr KeyMap kKeys[] = {
    {VK_BACK, SDL_SCANCODE_BACKSPACE}, {VK_TAB, SDL_SCANCODE_TAB}, {VK_RETURN, SDL_SCANCODE_RETURN},
    {VK_PAUSE, SDL_SCANCODE_PAUSE}, {VK_CAPITAL, SDL_SCANCODE_CAPSLOCK}, {VK_ESCAPE, SDL_SCANCODE_ESCAPE},
    {VK_SPACE, SDL_SCANCODE_SPACE}, {VK_PRIOR, SDL_SCANCODE_PAGEUP}, {VK_NEXT, SDL_SCANCODE_PAGEDOWN},
    {VK_END, SDL_SCANCODE_END}, {VK_HOME, SDL_SCANCODE_HOME}, {VK_LEFT, SDL_SCANCODE_LEFT}, {VK_UP, SDL_SCANCODE_UP},
    {VK_RIGHT, SDL_SCANCODE_RIGHT}, {VK_DOWN, SDL_SCANCODE_DOWN}, {VK_SNAPSHOT, SDL_SCANCODE_PRINTSCREEN},
    {VK_INSERT, SDL_SCANCODE_INSERT}, {VK_DELETE, SDL_SCANCODE_DELETE},
    {'0', SDL_SCANCODE_0}, {'1', SDL_SCANCODE_1}, {'2', SDL_SCANCODE_2}, {'3', SDL_SCANCODE_3}, {'4', SDL_SCANCODE_4},
    {'5', SDL_SCANCODE_5}, {'6', SDL_SCANCODE_6}, {'7', SDL_SCANCODE_7}, {'8', SDL_SCANCODE_8}, {'9', SDL_SCANCODE_9},
    {'A', SDL_SCANCODE_A}, {'B', SDL_SCANCODE_B}, {'C', SDL_SCANCODE_C}, {'D', SDL_SCANCODE_D}, {'E', SDL_SCANCODE_E},
    {'F', SDL_SCANCODE_F}, {'G', SDL_SCANCODE_G}, {'H', SDL_SCANCODE_H}, {'I', SDL_SCANCODE_I}, {'J', SDL_SCANCODE_J},
    {'K', SDL_SCANCODE_K}, {'L', SDL_SCANCODE_L}, {'M', SDL_SCANCODE_M}, {'N', SDL_SCANCODE_N}, {'O', SDL_SCANCODE_O},
    {'P', SDL_SCANCODE_P}, {'Q', SDL_SCANCODE_Q}, {'R', SDL_SCANCODE_R}, {'S', SDL_SCANCODE_S}, {'T', SDL_SCANCODE_T},
    {'U', SDL_SCANCODE_U}, {'V', SDL_SCANCODE_V}, {'W', SDL_SCANCODE_W}, {'X', SDL_SCANCODE_X}, {'Y', SDL_SCANCODE_Y},
    {'Z', SDL_SCANCODE_Z}, {VK_LWIN, SDL_SCANCODE_LGUI}, {VK_RWIN, SDL_SCANCODE_RGUI}, {VK_APPS, SDL_SCANCODE_APPLICATION},
    {VK_NUMPAD0, SDL_SCANCODE_KP_0}, {VK_NUMPAD1, SDL_SCANCODE_KP_1}, {VK_NUMPAD2, SDL_SCANCODE_KP_2},
    {VK_NUMPAD3, SDL_SCANCODE_KP_3}, {VK_NUMPAD4, SDL_SCANCODE_KP_4}, {VK_NUMPAD5, SDL_SCANCODE_KP_5},
    {VK_NUMPAD6, SDL_SCANCODE_KP_6}, {VK_NUMPAD7, SDL_SCANCODE_KP_7}, {VK_NUMPAD8, SDL_SCANCODE_KP_8},
    {VK_NUMPAD9, SDL_SCANCODE_KP_9}, {VK_MULTIPLY, SDL_SCANCODE_KP_MULTIPLY}, {VK_ADD, SDL_SCANCODE_KP_PLUS},
    {VK_SUBTRACT, SDL_SCANCODE_KP_MINUS}, {VK_DECIMAL, SDL_SCANCODE_KP_PERIOD}, {VK_DIVIDE, SDL_SCANCODE_KP_DIVIDE},
    {VK_F1, SDL_SCANCODE_F1}, {VK_F2, SDL_SCANCODE_F2}, {VK_F3, SDL_SCANCODE_F3}, {VK_F4, SDL_SCANCODE_F4},
    {VK_F5, SDL_SCANCODE_F5}, {VK_F6, SDL_SCANCODE_F6}, {VK_F7, SDL_SCANCODE_F7}, {VK_F8, SDL_SCANCODE_F8},
    {VK_F9, SDL_SCANCODE_F9}, {VK_F10, SDL_SCANCODE_F10}, {VK_F11, SDL_SCANCODE_F11}, {VK_F12, SDL_SCANCODE_F12},
    {VK_NUMLOCK, SDL_SCANCODE_NUMLOCKCLEAR}, {VK_SCROLL, SDL_SCANCODE_SCROLLLOCK},
    {VK_LSHIFT, SDL_SCANCODE_LSHIFT}, {VK_RSHIFT, SDL_SCANCODE_RSHIFT}, {VK_LCONTROL, SDL_SCANCODE_LCTRL},
    {VK_RCONTROL, SDL_SCANCODE_RCTRL}, {VK_LMENU, SDL_SCANCODE_LALT}, {VK_RMENU, SDL_SCANCODE_RALT},
    {VK_OEM_1, SDL_SCANCODE_SEMICOLON}, {VK_OEM_PLUS, SDL_SCANCODE_EQUALS}, {VK_OEM_COMMA, SDL_SCANCODE_COMMA},
    {VK_OEM_MINUS, SDL_SCANCODE_MINUS}, {VK_OEM_PERIOD, SDL_SCANCODE_PERIOD}, {VK_OEM_2, SDL_SCANCODE_SLASH},
    {VK_OEM_3, SDL_SCANCODE_GRAVE}, {VK_OEM_4, SDL_SCANCODE_LEFTBRACKET}, {VK_OEM_5, SDL_SCANCODE_BACKSLASH},
    {VK_OEM_6, SDL_SCANCODE_RIGHTBRACKET}, {VK_OEM_7, SDL_SCANCODE_APOSTROPHE},
};

SDL_Scancode ScanOf(int vk) {
    for (const KeyMap& k : kKeys)
        if (k.vk == vk) return k.scan;
    return SDL_SCANCODE_UNKNOWN;
}

int VkOf(SDL_Scancode scan) {
    for (const KeyMap& k : kKeys)
        if (k.scan == scan) return k.vk;
    return 0;
}

// a key went down: the settings panel, hide HUD and texture replacement binds, as main.cpp's HostKey
bool HostKey(int vk, bool repeat) {
    if (repeat || app::ConsoleOpen()) return false;
    if (input::HostKey(input::KeyEvent::SettingsPanel, vk)) app::ToggleMenu();
    else if (!app::MenuOpen() && input::HostKey(input::KeyEvent::HideHud, vk)) app::g_hide_hud = !app::g_hide_hud;
    else if (!app::MenuOpen() && input::HostKey(input::KeyEvent::ToggleTextures, vk)) app::ToggleTextureReplacement();
    else return false;
    return true;
}

void OnKey(const SDL_KeyboardEvent& k) {
    int vk = VkOf(k.keysym.scancode);
    if (!vk) return;
    bool repeat = k.repeat != 0;
    if (HostKey(vk, repeat)) return;
    if (vk == VK_ESCAPE && app::MenuOpen() && !app::Capturing()) app::ToggleMenu();
    else if (vk == VK_ESCAPE && app::ConsoleOpen()) app::ToggleConsole();
    if (input::ConsoleKey(vk) && !app::MenuOpen() && !repeat) app::ToggleConsole();
}

// what input.cpp and the menus take for the window: the SDL window, or a stand-in where there is none
HWND Handle() {
#ifdef SCDA_NO_WINDOW
    static int stand_in;
    return &stand_in;
#else
    return g_window;
#endif
}

// the window's size in pixels, as the renderer sees it
void Drawable(int& w, int& h) {
#if defined(SCDA_NO_WINDOW)
    // 720p unless width and height ask for another, as ApplyNow sets the display mode with a window
    const app::Settings& s = app::Get();
    w = s.width && s.height ? int(s.width) : 1280;
    h = s.width && s.height ? int(s.height) : 720;
#elif defined(SCDA_SDL_VULKAN)
    SDL_Vulkan_GetDrawableSize(g_window, &w, &h);
#elif defined(SCDA_GL)
    SDL_GL_GetDrawableSize(g_window, &w, &h);
#else
    SDL_GetWindowSize(g_window, &w, &h);
#endif
}

// UTF-8 into the UTF-16 units WM_CHAR would bring
void OnText(const char* s) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        for (; more && (*p & 0xC0) == 0x80; more--) c = c << 6 | (*p++ & 0x3F);
        if (c >= 0x10000) {
            input::OnChar(0xD800 + ((c - 0x10000) >> 10));
            input::OnChar(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            input::OnChar(c);
        }
    }
}

void RawMouse(LONG dx, LONG dy, int wheel) {
    RAWINPUT ri{};
    ri.header.dwType = RIM_TYPEMOUSE;
    ri.data.mouse.lLastX = dx;
    ri.data.mouse.lLastY = dy;
    if (wheel) {
        ri.data.mouse.usButtonFlags = RI_MOUSE_WHEEL;
        ri.data.mouse.usButtonData = USHORT(SHORT(wheel * WHEEL_DELTA));
    }
    input::OnRawInput(&ri);
}

uint32_t ButtonBit(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return 1u << VK_LBUTTON;
        case SDL_BUTTON_RIGHT: return 1u << VK_RBUTTON;
        case SDL_BUTTON_MIDDLE: return 1u << VK_MBUTTON;
        case SDL_BUTTON_X1: return 1u << VK_XBUTTON1;
        case SDL_BUTTON_X2: return 1u << VK_XBUTTON2;
        default: return 0;
    }
}

void ApplyNow() {
    const app::Settings& s = app::Get();
#if defined(SCDA_NO_WINDOW)
    (void)s;
#elif defined(SCDA_SWITCH)
    // 720p unless width and height ask for another: docked the console would draw the frame at 1080p
    SDL_DisplayMode mode{};
    SDL_GetDesktopDisplayMode(0, &mode);
    mode.w = s.width && s.height ? int(s.width) : 1280;
    mode.h = s.width && s.height ? int(s.height) : 720;
    SDL_SetWindowDisplayMode(g_window, &mode);
    SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN);
#else
    if (s.display_mode == app::DisplayMode::Windowed) {
        app::Mode out = app::Output();
        SDL_SetWindowFullscreen(g_window, 0);
        SDL_SetWindowSize(g_window, int(out.width), int(out.height));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
#endif
    int w = 0, h = 0;
    Drawable(w, h);
    LOGI("window: frame %dx%d", w, h);
    g_width = w;
    g_height = h;
    app::g_display_applied++;
}

void HandleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT: g_quit = true; break;
        case SDL_KEYDOWN: OnKey(e.key); break;
        case SDL_TEXTINPUT: OnText(e.text.text); break;
        case SDL_MOUSEMOTION:
            g_cursor_x = e.motion.x;
            g_cursor_y = e.motion.y;
            if (e.motion.xrel || e.motion.yrel) RawMouse(e.motion.xrel, e.motion.yrel, 0);
            break;
        case SDL_MOUSEWHEEL:
            if (e.wheel.y) RawMouse(0, 0, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
            g_mouse_buttons |= ButtonBit(e.button.button);
            if (e.button.button == SDL_BUTTON_LEFT) input::OnPress();
            break;
        case SDL_MOUSEBUTTONUP:
            g_mouse_buttons &= ~ButtonBit(e.button.button);
            if (e.button.button == SDL_BUTTON_LEFT) input::OnClick();
            break;
        case SDL_CONTROLLERDEVICEADDED: port::PadAdded(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: port::PadRemoved(e.cdevice.which); break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_focus = true;
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                g_focus = false;
                g_mouse_buttons = 0;
                input::ReleaseCursor();
            }
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                int w = 0, h = 0;
                Drawable(w, h);
                g_width = w;
                g_height = h;
                app::g_display_applied++;
            }
            break;
        default:
            if (e.type == g_apply_event) ApplyNow();
            break;
    }
}

// debug builds: every 15 s where each guest thread is, the return addresses on its stack (a hang shows its waits)
void Watchdog() {
    if (!logx::Verbose()) return;
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    if (std::chrono::steady_clock::now() < next) return;
    next += std::chrono::seconds(15);
    for (auto& t : kernel::Threads()) {
        if (!t->ctx) continue;
        LOGI("watchdog: guest thread %X, last indirect call %08X", t->id, t->ctx->last_icall);
        kernel::LogGuestStack(*t->ctx, 48);
    }
}

}  // namespace

// the Win32 calls input.cpp makes, answered from what the event loop keeps
SHORT GetAsyncKeyState(int vk) {
    if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) return (g_mouse_buttons >> vk & 1) ? SHORT(0x8000) : 0;
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    auto down = [&](SDL_Scancode s) { return s != SDL_SCANCODE_UNKNOWN && keys[s]; };
    bool held;
    switch (vk) {
        case VK_SHIFT: held = down(SDL_SCANCODE_LSHIFT) || down(SDL_SCANCODE_RSHIFT); break;
        case VK_CONTROL: held = down(SDL_SCANCODE_LCTRL) || down(SDL_SCANCODE_RCTRL); break;
        case VK_MENU: held = down(SDL_SCANCODE_LALT) || down(SDL_SCANCODE_RALT); break;
        default: held = down(ScanOf(vk)); break;
    }
    return held ? SHORT(0x8000) : 0;
}

HWND GetForegroundWindow() { return g_focus ? Handle() : nullptr; }

BOOL GetCursorPos(POINT* p) {
    p->x = g_cursor_x;
    p->y = g_cursor_y;
    return 1;
}

BOOL GetClientRect(HWND, RECT* r) {
    *r = {0, 0, g_width, g_height};
    return 1;
}

BOOL ClipCursor(const RECT* r) {
    g_want_relative = r ? 1 : 0;
    return 1;
}

UINT GetRawInputData(HRAWINPUT handle, UINT, void* data, UINT* size, UINT) {
    if (!handle || *size < sizeof(RAWINPUT)) return UINT(-1);
    *static_cast<RAWINPUT*>(data) = *static_cast<const RAWINPUT*>(handle);
    return sizeof(RAWINPUT);
}

namespace app {

std::filesystem::path ExeDir() { return port::HomeDir(); }

int Instance() {
    static const int n = [] {
        size_t at = port::CommandLine().find("--instance");
        return at == std::string::npos ? 0 : std::clamp(std::atoi(port::CommandLine().c_str() + at + 10), 0, 8);
    }();
    return n;
}

void* Window() { return Handle(); }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {
    SDL_Event e{};
    e.type = g_apply_event;
    SDL_PushEvent(&e);
}

// the window is up from the start: no splash or handover holds it back
void OpenGameWindow(bool apply) {
    if (apply) ApplyDisplay();
}
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
void* MenuCursor() { return nullptr; }

// the code is linked in: nothing to build or load beyond its tables
bool PrepareGameCode(const GameFiles&) { return dispatch::Load({}); }

void Pace() {
#ifdef SCDA_SWITCH
    port::KeepClocks(Get().cpu_clock);
#endif
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) cap = port::RefreshRate();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

// in the background the game and its sound hold still, as on Windows
void WaitWhileUnfocused() {
#ifndef SCDA_SWITCH
    auto should = [] { return Get().pause_on_focus_loss && !input::NetSession() && !g_focus && g_window; };
    if (!should()) return;
    LOGI("focus lost: paused");
    audio::SetPaused(true);
    kernel::HoldGuestTime(true);
    while (should() && !g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    kernel::HoldGuestTime(false);
    audio::SetPaused(false);
    LOGI("focus back: resumed");
#endif
}

}  // namespace app

namespace port {

#ifdef __SWITCH__
// the console's own mode is the 720p frame ApplyNow sets (docked SDL reports 1080p), so width and height 0 mean 720p
uint32_t ScreenWidth() { return 1280; }
uint32_t ScreenHeight() { return 720; }
#else
uint32_t ScreenWidth() {
    SDL_DisplayMode m{};
    return SDL_GetDesktopDisplayMode(0, &m) == 0 ? uint32_t(m.w) : 1280;
}
uint32_t ScreenHeight() {
    SDL_DisplayMode m{};
    return SDL_GetDesktopDisplayMode(0, &m) == 0 ? uint32_t(m.h) : 720;
}
#endif
uint32_t RefreshRate() {
    SDL_DisplayMode m{};
    int display = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
    return SDL_GetCurrentDisplayMode(std::max(display, 0), &m) == 0 && m.refresh_rate > 1 ? uint32_t(m.refresh_rate) : 0;
}

void DrawableSize(uint32_t& w, uint32_t& h) {
    w = uint32_t(std::max<int>(g_width, 1));
    h = uint32_t(std::max<int>(g_height, 1));
}

#ifdef SCDA_SDL_VULKAN
std::vector<const char*> VulkanInstanceExtensions() {
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(g_window, &n, nullptr);
    std::vector<const char*> out(n);
    SDL_Vulkan_GetInstanceExtensions(g_window, &n, out.data());
    return out;
}

bool CreateVulkanSurface(void* instance, uint64_t* surface) {
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(instance), &s)) {
        LOGE("vk: SDL made no surface: %s", SDL_GetError());
        return false;
    }
    *surface = uint64_t(s);
    return true;
}
#elif !defined(SCDA_SWITCH)
std::vector<const char*> VulkanInstanceExtensions() { return {}; }
bool CreateVulkanSurface(void*, uint64_t*) { return false; }
#endif
#ifndef SCDA_SWITCH
bool SurfaceSizeDiffers(uint32_t, uint32_t) { return false; }
void SetSurfaceSize(uint32_t, uint32_t) {}
#endif

void RequestQuit() {
    g_quit = true;
    SDL_Event e{};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

}  // namespace port

int main(int argc, char** argv) {
    bool logging = app::LoggingWanted();
    bool profiling = app::ProfilingWanted();
    logx::SetLogging(logging, profiling);
    port::SetBootLog(logging);
#ifdef SCDA_SWITCH
    port::SetProfiling(profiling);
#endif
    port::Init(argc, argv);
    std::string log = app::Instance() > 1 ? "scda-" + std::to_string(app::Instance()) : "scda";
    logx::Init((app::ExeDir() / "logs").string(), log, port::Relaunched());
    port::Breadcrumb(("log open: " + logx::Path()).c_str());
    app::Load();
    port::Breadcrumb("settings loaded");
    if (port::Relaunched() && !logx::Verbose()) LOGR("Relaunched");
    else app::LogSystem();

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        port::Fatal("Splinter Cell: Double Agent", std::string("SDL failed to start: ") + SDL_GetError());
        return 1;
    }
    g_apply_event = SDL_RegisterEvents(1);
    port::Breadcrumb("SDL up");

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!app::PrepareGameCode(game)) return 1;
    port::Breadcrumb("game found");

    app::Mode out = app::Output();
    std::string title = "Tom Clancy's Splinter Cell: Double Agent";
    if (app::Instance()) title += " [" + std::to_string(app::Instance()) + "]";
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef SCDA_SDL_VULKAN
    flags |= SDL_WINDOW_VULKAN;
#endif
#ifdef SCDA_GL
    // the OpenGL renderer (gpu_gl.cpp) makes its context on this window: 4.3 core, no depth of its own
    flags |= SDL_WINDOW_OPENGL;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
#endif
#ifdef SCDA_NO_WINDOW
    (void)flags;
    ApplyNow();
    g_focus = true;
#else
    g_window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, int(out.width),
                                int(out.height), flags);
    if (!g_window) {
        port::Fatal("Splinter Cell: Double Agent", std::string("The window could not be opened: ") + SDL_GetError());
        return 1;
    }
    port::Breadcrumb("window open");
    ApplyNow();
    g_focus = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
#ifndef SCDA_SWITCH
    // typed characters for name entry; on the Switch SDL opens the system keyboard for this, so not there
    SDL_StartTextInput();
#endif

    input::Init(Handle());
    input::LoadBindings();
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    port::Breadcrumb("boot");
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        port::Fatal("Splinter Cell: Double Agent", "The game failed to start, see the log in the logs folder.");
        return 1;
    }

    port::Breadcrumb("booted, event loop");
    logx::Flush();
    while (!g_quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 2)) {
            HandleEvent(e);
            while (SDL_PollEvent(&e)) HandleEvent(e);
        }
        if (int want = g_want_relative.exchange(-1); want >= 0 && g_focus)
            SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
        port::UpdatePads();
        Watchdog();
#ifdef __SWITCH__
        port::ServiceRelaunch();
#endif
    }
    gpu::SavePipelineCache();
    logx::Flush();
    kernel::Shutdown(0);
}
