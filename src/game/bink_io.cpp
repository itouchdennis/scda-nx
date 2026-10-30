#include <cstdint>

#include "xdk/hle.h"

using namespace kernel;

// The game may return no data while disc reads are held for a background file copy.
// Report no work in this case to prevent the Bink IO worker from busy-spinning
// and repeatedly waking guest threads.
namespace {

constexpr uint32_t kGameReader = 0x35AA1;
constexpr uint32_t kReadsHeld = 0x5E3E24;  
constexpr uint32_t kTaskOffset = 0x20C;
constexpr uint32_t kReader = 0x120, kReadEnd = 0x12C;

}  // namespace

X86_EXTERN_FUNC(__imp__sub_004B8EC0);
X86_FUNC(sub_004B8EC0) {
    uint32_t bink = R32(ctx.esp + 4) - kTaskOffset;
    if (R32(bink + kReader) == kGameReader && (R32(kReadsHeld) || R32(bink + kReadEnd))) {
        ctx.eax = 0;
        ctx.esp += 12;  // ret 8
        return;
    }
    __imp__sub_004B8EC0(ctx, base);
}
