#pragma once
#include <cstdint>

#include "cpu/x86_context.h"

// the Xbox Communicator (communicator.cpp); each image hooks its own copies of the XDK functions and passes its
// addresses
namespace communicator {

// the image's XPP device type records: CurrentConnected, ChangeConnected, PreviousConnected
struct Types {
    uint32_t microphone, headphone;
};

// a call that reads device records, the type first on the stack (XGetDevices, XGetDeviceChanges and the like):
// before the original runs, the headset records follow the keyboard's port
void Refresh(X86Context& ctx, const Types& types);
// XVoiceCreateMediaObjectEx (type, port, max packets, format, callback, context, out): false for a type no headset
// has, then the original runs
bool Create(X86Context& ctx, const Types& types);

}  // namespace communicator
