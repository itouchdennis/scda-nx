// The pad's black button brings up a picker whose release (sub_001BE71C) sends the player controller
// WhistleEvent (left slot) or SidekickCommand (right). The whistle and sidekick keys send them at once, behind the
// checks the HUD greys the whistle slot with (sub_0017D90E). With a human teammate the controller is the co-op one,
// which has no SidekickCommand; the key runs its CoopAction exec there, which the black button's binding runs first.
#include "core/input.h"
#include "game/console.h"
#include "game/unreal.h"
#include "xdk/hle.h"

using namespace kernel;
namespace unreal = game::unreal;

namespace {

constexpr uint32_t kViewportActor = 0x34;
// thiscall on the controller: eventWhistleEvent(), eventSidekickCommand(), eventCanAccessWhistle() -> bool,
// eventIsScriptedEvent() -> bool
constexpr uint32_t kWhistleEvent = 0x1BE05B, kSidekickCommand = 0x1BE037;
constexpr uint32_t kCanAccessWhistle = 0x17D002, kIsScriptedEvent = 0x19A304;
// controller: +0x3F8 set while it can act, +0x38C bit 0x20 and +0x510 bit 8 block the whistle
constexpr uint32_t kCanAct = 0x3F8, kBlock1 = 0x38C, kBlock2 = 0x510;

uint32_t Call(X86Context& ctx, uint32_t fn, uint32_t pc) {
    ctx.ecx = pc;
    return CallGuest(ctx, fn, {});
}

}  // namespace

namespace game {

void WhistleKeys(X86Context& ctx, uint32_t viewport) {
    bool whistle = input::TakeEvent(input::KeyEvent::Whistle);
    bool command = input::TakeEvent(input::KeyEvent::SidekickCommand);
    uint32_t pc = R32(viewport + kViewportActor);
    if ((!whistle && !command) || !pc || !R32(pc + kCanAct) || Call(ctx, kIsScriptedEvent, pc)) return;
    if (whistle && Call(ctx, kCanAccessWhistle, pc) && !(G<uint8_t>(pc + kBlock1) & 0x20) &&
        !(G<uint8_t>(pc + kBlock2) & 0x08))
        Call(ctx, kWhistleEvent, pc);
    // only the sidekick controller has the event, and looking it up on another is fatal
    if (command && unreal::IsA(pc, unreal::Name("ESidekickPlayerController"))) Call(ctx, kSidekickCommand, pc);
    else if (command && unreal::IsA(pc, unreal::Name("ECoopPlayerController"))) QueueCommand("CoopAction", Route::Player);
}

}  // namespace game
