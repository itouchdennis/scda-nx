// Pause on focus loss: the game thread waits while the window is in the background or minimized, with its sound
// and the guest's clocks stopped. Never in a network session, where the other machines would time out.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "audio/output.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace xnet {
bool InSession();  // xdk/xnet.cpp
}  // namespace xnet

namespace app {

namespace {

constexpr DWORD kPollMs = 20;

// a hidden window, behind the splash or for a handover, never pauses: it opens from the game thread
bool ShouldPause() {
    if (!Get().pause_on_focus_loss || input::NetSession() || xnet::InSession()) return false;
    HWND hwnd = static_cast<HWND>(Window());
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    return GetForegroundWindow() != hwnd || IsIconic(hwnd);
}

}  // namespace

void WaitWhileUnfocused() {
    if (!ShouldPause()) return;
    LOGI("focus lost: paused");
    audio::SetPaused(true);
    kernel::HoldGuestTime(true);
    while (ShouldPause()) Sleep(kPollMs);
    kernel::HoldGuestTime(false);
    audio::SetPaused(false);
    LOGI("focus back: resumed");
}

}  // namespace app
