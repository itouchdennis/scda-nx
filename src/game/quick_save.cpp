// Quick save and quick load keys, in single player play. Quick save is what the pause menu's QUICK SAVE runs
// (its press handler sub_0018BB68, item 4); quick load is what LOAD GAME > quick save slot runs past its
// "progress will be lost" box.
#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// viewport +0x70 the in-game menus' interaction, its +0x3C the menus manager (vtable 0x541068), whose +4 is the
// interaction again and +0x3658 the load/save menu (CNewLoadSaveGame, made on the first pause)
constexpr uint32_t kViewportMenus = 0x70, kMenusManager = 0x3C;
constexpr uint32_t kManagerVtable = 0x541068, kManagerInteraction = 0x04, kManagerLoadSave = 0x3658;
// interaction: vtable +0x84 CanSave(), the check the pause menu shows its save items behind; sub_0023067C the
// player controller, +0xCC set in co-op
constexpr uint32_t kCanSave = 0x84, kController = 0x23067C, kCoop = 0xCC;
constexpr uint32_t kSaveMode = 0x5B5AC8;  // 2: no quick save item, SAVE GAME in its place
// load/save menu: +0x50 slot list (+0x9C selected row, short), +0x70 page mode (2 in-game load, 3-5 hide the
// quick save row, so a row is the slot after it)
constexpr uint32_t kSlotList = 0x50, kListSelection = 0x9C, kMode = 0x70, kModeLoad = 2;
constexpr uint32_t kSave = 0x18A40D;  // thiscall (slot, 0, 0)
constexpr uint32_t kLoad = 0x18A185;  // thiscall (bool confirmed), the selected row of the slot list
constexpr uint32_t kQuickSlot = 0;
// the save picture: the save writes [0x6500FC]'s captured image (vtable +0x2C) to z:\savegameimage.tmp and copies it
// into the slot's image file, so a save with nothing captured keeps the slot's old picture. The menus' save event
// (NotifyMenu 0x2F, 0x182408) captures the frame first with vtable +0x24 (width, height, -1, -1), as here.
constexpr uint32_t kCapturer = 0x6500FC, kCapture = 0x24, kCaptureSize = 0x100;

uint32_t Call(X86Context& ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    ctx.ecx = self;
    return CallGuest(ctx, fn, args);
}

}  // namespace

namespace game {

void QuickSaveKeys(X86Context& ctx, uint32_t viewport, bool menu) {
    bool save = input::TakeEvent(input::KeyEvent::QuickSave);
    bool load = input::TakeEvent(input::KeyEvent::QuickLoad);
    if ((!save && !load) || menu) return;
    uint32_t interaction = R32(viewport + kViewportMenus);
    uint32_t manager = interaction ? R32(interaction + kMenusManager) : 0;
    if (!manager || R32(manager) != kManagerVtable || R32(manager + kManagerInteraction) != interaction) return;
    uint32_t loadsave = R32(manager + kManagerLoadSave);
    uint32_t pc = Call(ctx, kController, interaction, {});
    if (!loadsave || !pc || R32(pc + kCoop) || R32(kSaveMode) == 2) {
        LOGI("quicksave: not available here");
        return;
    }
    if (save) {
        if (!Call(ctx, R32(R32(interaction) + kCanSave), interaction, {})) {
            LOGI("quicksave: saving is not allowed now");
            return;
        }
        LOGI("quicksave: saving");
        W32(loadsave + kMode, 0);
        if (uint32_t capturer = R32(kCapturer))
            Call(ctx, R32(R32(capturer) + kCapture), capturer, {kCaptureSize, kCaptureSize, ~0u, ~0u});
        Call(ctx, kSave, loadsave, {kQuickSlot, 0, 0});
    } else {
        uint32_t list = R32(loadsave + kSlotList);
        if (!list) return;
        LOGI("quicksave: loading");
        uint32_t mode = R32(loadsave + kMode);
        W32(loadsave + kMode, kModeLoad);
        W16(list + kListSelection, uint16_t(kQuickSlot));
        Call(ctx, kLoad, loadsave, {1});
        W32(loadsave + kMode, mode);
    }
}

}  // namespace game
