// the Communicator's hooks in default.xbe (xdk/communicator.cpp)
#include "xdk/communicator.h"
#include "xdk/hle.h"

using namespace kernel;

namespace dsound {
void MuteForVoiceLink(uint32_t stream);
}  // namespace dsound

namespace dispatch {
uint32_t KernelThunkAddress(uint32_t ordinal);
}  // namespace dispatch

namespace {

constexpr communicator::Types kTypes{0x4EC630, 0x4EC63C};

// the session's network lock: sub_00255732 the current session (cdecl, null without one), its [+8] +0x30 a holder
// of the critical section; the byte at 0x6AB0F8 turns the network layer's locking off
constexpr uint32_t kSession = 0x255732, kNoLocks = 0x6AB0F8;
constexpr uint32_t kEnterCriticalSection = 277, kLeaveCriticalSection = 294;  // xboxkrnl ordinals

}  // namespace

// XGetDevices (type), XGetDeviceChanges (type, insertions, removals) and the game's own read of a record
X86_EXTERN_FUNC(__imp__sub_004EDA95);
X86_FUNC(sub_004EDA95) {
    communicator::Refresh(ctx, kTypes);
    __imp__sub_004EDA95(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_004EDAB7);
X86_FUNC(sub_004EDAB7) {
    communicator::Refresh(ctx, kTypes);
    __imp__sub_004EDAB7(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_004EDA54);
X86_FUNC(sub_004EDA54) {
    communicator::Refresh(ctx, kTypes);
    __imp__sub_004EDA54(ctx, base);
}

// XVoiceCreateMediaObjectEx
X86_EXTERN_FUNC(__imp__sub_004F4EE3);
X86_FUNC(sub_004F4EE3) {
    if (!communicator::Create(ctx, kTypes)) __imp__sub_004F4EE3(ctx, base);
}

// A talker's playback, thiscall (holder, ?, DSSTREAMDESC): besides the headphone, the game plays the voice through a
// DirectSound stream of its own, kept at holder +0x14; it goes quiet while the voice link plays, as the headphone does.
X86_EXTERN_FUNC(__imp__sub_00413230);
X86_FUNC(sub_00413230) {
    uint32_t holder = R32(ctx.esp + 4);
    __imp__sub_00413230(ctx, base);
    if (uint32_t stream = R32(holder + 0x14)) {
        dsound::MuteForVoiceLink(stream);
        LOG_FIRST(4, "communicator: talker speaker stream %08X", stream);
    }
}

// The voice task's tick (thiscall) holds the voice locks and, for each packet it receives or sends, takes the network
// lock; a network thread handing the voice code a session event (a player joining) holds the network lock and takes
// the voice lock. The console's one core never ran them side by side, here they deadlock: the tick takes the network
// lock first, so both go network, then voice.
X86_EXTERN_FUNC(__imp__sub_0029888F);
X86_FUNC(sub_0029888F) {
    uint32_t self = ctx.ecx;
    uint32_t session = G<uint8_t>(kNoLocks) ? 0 : CallGuest(ctx, kSession, {});
    uint32_t net = session ? R32(session + 8) : 0;
    uint32_t cs = net ? R32(net + 0x30) : 0;
    if (cs) CallGuest(ctx, dispatch::KernelThunkAddress(kEnterCriticalSection), {cs});
    ctx.ecx = self;
    __imp__sub_0029888F(ctx, base);
    if (cs) {
        uint32_t result = ctx.eax;
        CallGuest(ctx, dispatch::KernelThunkAddress(kLeaveCriticalSection), {cs});
        ctx.eax = result;
    }
}
