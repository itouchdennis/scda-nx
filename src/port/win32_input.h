#pragma once
// The part of Win32 that core/input.cpp reads the keyboard and mouse through, for builds without Windows. The keys keep
// their virtual key codes (the bindings and scda.toml name them so); port/win32_input.cpp answers from SDL's state:
// GetAsyncKeyState from the keyboard and mouse buttons, the cursor in window pixels, raw mouse motion as RAWINPUT the
// event loop builds from SDL's relative motion and wheel. Clipping the cursor is SDL's relative mouse mode. The layout
// is always the US one, which is how the bindings name their keys anyway.
#include <cstdint>
#include <strings.h>

#include <chrono>
#include <thread>

using BYTE = uint8_t;
using WORD = uint16_t;
using SHORT = int16_t;
using USHORT = uint16_t;
using UINT = unsigned;
using DWORD = uint32_t;
using LONG = int32_t;
using ULONG = uint32_t;
using BOOL = int;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using HWND = void*;
using HKL = void*;
using HRAWINPUT = void*;

struct POINT {
    LONG x, y;
};
struct RECT {
    LONG left, top, right, bottom;
};

// XInput
struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
constexpr WORD XINPUT_GAMEPAD_DPAD_UP = 0x0001;
constexpr WORD XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
constexpr WORD XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
constexpr WORD XINPUT_GAMEPAD_START = 0x0010;
constexpr WORD XINPUT_GAMEPAD_BACK = 0x0020;
constexpr WORD XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
constexpr WORD XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
constexpr WORD XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD XINPUT_GAMEPAD_A = 0x1000;
constexpr WORD XINPUT_GAMEPAD_B = 0x2000;
constexpr WORD XINPUT_GAMEPAD_X = 0x4000;
constexpr WORD XINPUT_GAMEPAD_Y = 0x8000;
constexpr BYTE XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;

// virtual key codes
enum : int {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D,
    VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7,
    VK_NUMPAD8, VK_NUMPAD9, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E,
    VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2,
    VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_BROWSER_BACK = 0xA6, VK_LAUNCH_APP2 = 0xB7,
    VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE,
    VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
};
constexpr UINT MAPVK_VK_TO_VSC = 0, MAPVK_VK_TO_CHAR = 2, MAPVK_VSC_TO_VK_EX = 3;

// window messages input.cpp posts to refresh the cursor
constexpr UINT WM_SETCURSOR = 0x0020, WM_MOUSEMOVE = 0x0200;
constexpr LPARAM HTCLIENT = 1;
constexpr int WHEEL_DELTA = 120;
inline LPARAM MAKELPARAM(int lo, int hi) { return LPARAM(uint32_t(uint16_t(lo)) | uint32_t(uint16_t(hi)) << 16); }

// raw input, as the event loop fills it
constexpr DWORD RIM_TYPEMOUSE = 0;
constexpr UINT RID_INPUT = 0x10000003;
constexpr USHORT RI_MOUSE_WHEEL = 0x0400;
constexpr USHORT MOUSE_MOVE_ABSOLUTE = 0x01;
struct RAWINPUTHEADER {
    DWORD dwType;
    DWORD dwSize;
};
struct RAWMOUSE {
    USHORT usFlags;
    USHORT usButtonFlags;
    USHORT usButtonData;
    LONG lLastX;
    LONG lLastY;
};
struct RAWINPUT {
    RAWINPUTHEADER header;
    union {
        RAWMOUSE mouse;
    } data;
};
struct RAWINPUTDEVICE {
    USHORT usUsagePage, usUsage;
    DWORD dwFlags;
    HWND hwndTarget;
};

SHORT GetAsyncKeyState(int vk);
HWND GetForegroundWindow();  // the SDL window while it has the input focus
inline HKL GetKeyboardLayout(DWORD) { return nullptr; }
inline DWORD GetWindowThreadProcessId(HWND, DWORD*) { return 0; }
inline UINT MapVirtualKeyExW(UINT, UINT, HKL) { return 0; }  // the US layout: every key is where its name says
inline BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) { return 1; }
BOOL GetCursorPos(POINT* p);  // in window pixels: ScreenToClient leaves it alone
inline BOOL ScreenToClient(HWND, POINT*) { return 1; }
BOOL GetClientRect(HWND, RECT* r);
inline int MapWindowPoints(HWND, HWND, POINT*, UINT) { return 0; }
inline BOOL GetClipCursor(RECT*) { return 0; }
inline BOOL EqualRect(const RECT*, const RECT*) { return 0; }
BOOL ClipCursor(const RECT* r);  // null frees the mouse, a rect captures it (relative mode)
inline BOOL RegisterRawInputDevices(const RAWINPUTDEVICE*, UINT, UINT) { return 1; }
// handle: the RAWINPUT the event loop passed to input::OnRawInput
UINT GetRawInputData(HRAWINPUT handle, UINT command, void* data, UINT* size, UINT header_size);
#ifndef SCDA_SLEEP_DEFINED
#define SCDA_SLEEP_DEFINED
inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
