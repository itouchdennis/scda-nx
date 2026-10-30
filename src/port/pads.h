#pragma once
// Controllers for core/input.cpp, read into XInput's report. SDL2's events come to the main thread
// (port/sdl_window.cpp), which keeps each slot's latest state here; the guest's threads read it at any time.
#include <cstdint>

#include "port/win32_input.h"

namespace port {

void StartPads();  // any thread; the main thread opens the controllers SDL reports
// slot 0-3: the stick y axes up positive, the triggers 0-255; false for an empty slot
bool ReadPad(int slot, XINPUT_GAMEPAD& g);
void SetMotors(int slot, uint16_t left, uint16_t right);
// main thread, from the event loop: a controller came or went, a frame's worth of reads
void PadAdded(int device_index);
void PadRemoved(int instance_id);
void UpdatePads();

}  // namespace port
