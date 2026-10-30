// the Communicator's hooks in sc4_adv.xbe (xdk/communicator.cpp)
#include "xdk/communicator.h"
#include "xdk/hle.h"

using namespace kernel;

namespace dsound {
void MuteForVoiceLink(uint32_t stream);
}  // namespace dsound

namespace {

constexpr communicator::Types kTypes{0x312890, 0x31289C};
constexpr uint32_t kTalkerStream = 0x18;  // talker: its DirectSound stream

}  // namespace

// XGetDevices (type), XGetDeviceChanges (type, insertions, removals) and the game's own read of a record
X86_EXTERN_FUNC(__imp__sub_0031394B);
X86_FUNC(sub_0031394B) {
    communicator::Refresh(ctx, kTypes);
    __imp__sub_0031394B(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0031396D);
X86_FUNC(sub_0031396D) {
    communicator::Refresh(ctx, kTypes);
    __imp__sub_0031396D(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0031390A);
X86_FUNC(sub_0031390A) {
    communicator::Refresh(ctx, kTypes);
    __imp__sub_0031390A(ctx, base);
}

// XVoiceCreateMediaObjectEx
X86_EXTERN_FUNC(__imp__sub_0031AF6D);
X86_FUNC(sub_0031AF6D) {
    if (!communicator::Create(ctx, kTypes)) __imp__sub_0031AF6D(ctx, base);
}

// A talker's playback, thiscall: the game plays the voice through a DirectSound stream of its own (8 kHz mono) kept
// at this +0x18; it goes quiet while the voice link plays, as the headphone does.
X86_EXTERN_FUNC(__imp__sub_0021C9F4);
X86_FUNC(sub_0021C9F4) {
    uint32_t self = ctx.ecx;
    __imp__sub_0021C9F4(ctx, base);
    if (uint32_t stream = R32(self + kTalkerStream)) {
        dsound::MuteForVoiceLink(stream);
        LOG_FIRST(4, "communicator: talker speaker stream %08X", stream);
    }
}
