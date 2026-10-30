// Co-op is Quazal Net-Z's SyncSim, a fixed-step lockstep. The session init sub_0025480C caps the step frequency at
// 22 Hz and puts a 50 ms floor under the input delay; both are options here. Both machines need the same values:
// the session runs at the lowest peer's rate, and different floors commit inputs on different steps. So the host's
// go to the client beside the game's own traffic (xdk/xnet.cpp), and the client uses them in place of its own.
// The session init runs before the session is joined, when neither the role nor the host's values may be known
// yet, so each step puts the session's rate into the SyncSim again; it reads the ceiling live. The floor stays as the
// init set it: changed mid-session on one side, the peers fall out of step and the session drops.
// A session that steps slower than its rate runs in slow motion (each step still advances 1/rate s), and the game
// lowers the rate only for slow frames, not for steps waiting on the peer's input. So the ceiling drops to the steps the
// session reaches, and tries back up to the setting after a while.
// The main loop (sub_0003F950) holds every frame of a synced session to 1/22 s on its own as well, in 7 ms sleeps;
// with a raised rate that limit goes and the lockstep alone paces the frames.
#include <algorithm>
#include <chrono>

#include "app/app.h"
#include "core/input.h"
#include "xdk/hle.h"

using namespace kernel;

namespace xnet {
void SetCoopHost(bool (*host)(uint32_t& rate, uint32_t& floor));
bool CoopHostValues(uint32_t& rate, uint32_t& floor);
}  // namespace xnet

namespace {

// SyncSim setters, thiscall(sim, value), each called once, by the session init: +0x84 the step frequency this
// machine advertises (Hz), +0x8C the input delay floor (ms)
constexpr uint32_t kMaxRate = 250;  // a step is at least 4 whole ms
constexpr uint32_t kFrameLimit = 0x4F64D0;  // double, seconds; read only at 0003F9F2
constexpr double kStockFrameLimit = double(1.0f / 22.0f);
constexpr uint32_t kMaxDelayFloor = 200;
constexpr uint32_t kSyncSim = 0x672974;  // pointer
constexpr uint32_t kCeiling = 0x84;
constexpr uint32_t kStockRate = 22;
constexpr uint32_t kSteps = 0x5C, kRate = 0x7C;  // steps taken; the session's rate, Hz
constexpr uint32_t kAutoBottom = 30;
constexpr uint32_t kAutoHold = 5, kAutoMaxHold = 160;  // seconds on target before trying higher
constexpr uint32_t kAutoWarmup = 3, kAutoBehind = 2;    // seconds: unjudged after a start or load; behind to lower
// GEngine; +0xD4 this machine's player number in a synced session, 1 the host, 2 the client
constexpr uint32_t kEngine = 0x5ED158, kLocalPlayer = 0xD4;

uint32_t Player() {
    uint32_t engine = R32(kEngine);
    return engine ? R32(engine + kLocalPlayer) : 0;
}

// this machine's settings: rate 0 is the game's
void Own(uint32_t& rate, uint32_t& floor) {
    rate = std::min(app::Get().coop_tick_rate, kMaxRate);
    floor = std::min(app::Get().coop_input_delay_floor, kMaxDelayFloor);
}

// the session's: the host's when this machine has them and does not host
bool Session(uint32_t& rate, uint32_t& floor) {
    uint32_t r, f;
    if (Player() == 1 || !xnet::CoopHostValues(r, f)) {
        Own(rate, floor);
        return false;
    }
    rate = std::min(r, kMaxRate);
    floor = std::min(f, kMaxDelayFloor);
    return true;
}

// the ceiling for a set rate, once a second from the steps the last second took
uint32_t Ceiling(uint32_t sim, uint32_t top) {
    static uint32_t set = 0, ceiling = 0, hold = kAutoHold, good = 0, last = 0, warmup = 0, behind = 0;
    static bool raised = false;
    static std::chrono::steady_clock::time_point start;
    auto now = std::chrono::steady_clock::now();
    uint32_t steps = R32(sim + kSteps);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (steps < last || top != set) set = ceiling = top, hold = kAutoHold, good = 0, raised = false;  // new session or setting
    if (steps < last || ms > 2000) {  // a new session, or a load: its first seconds step unevenly
        start = now, last = steps, warmup = kAutoWarmup, behind = 0;
        return ceiling;
    }
    if (ms < 1000) return ceiling;
    uint32_t reached = uint32_t((steps - last) * 1000 / ms);
    start = now, last = steps;
    if (warmup) {
        warmup--;
    } else if (reached + reached / 10 < R32(sim + kRate)) {
        if (++behind < kAutoBehind) return ceiling;
        if (raised) hold = std::min(hold * 2, kAutoMaxHold);
        ceiling = std::max(reached, kAutoBottom);
        raised = false, good = 0, behind = 0;
    } else {
        behind = 0;
        if (++good >= hold && ceiling < top) {
            ceiling = std::min(ceiling + ceiling / 5, top);
            raised = true, good = 0;
        } else if (good >= 3) {
            raised = false;
        }
    }
    return ceiling;
}

void SetFrameLimit(uint32_t rate) { G<double>(kFrameLimit) = rate ? 0.0 : kStockFrameLimit; }

// xnet's pump thread: the values to send while this machine hosts a synced session
bool Hosting(uint32_t& rate, uint32_t& floor) {
    if (!input::NetSession() || Player() != 1) return false;
    Own(rate, floor);
    return true;
}

struct Register {
    Register() { xnet::SetCoopHost(Hosting); }
} g_register;

// each step: the session's rate into the SyncSim, as its setter would put it
void Apply() {
    uint32_t sim = R32(kSyncSim);
    if (!sim || !Player()) return;
    uint32_t rate, floor;
    bool theirs = Session(rate, floor);
    uint32_t ceiling = rate ? Ceiling(sim, rate) : kStockRate;
    if (R32(sim + kCeiling) == ceiling) return;
    W32(sim + kCeiling, ceiling);
    SetFrameLimit(rate);
    LOGR("Co-op player %u: tick rate %u Hz of %u (%s)", Player(), ceiling, rate ? rate : kStockRate,
         theirs ? "the host's" : "own");
}

}  // namespace

// the step frequency setter (SyncSim +0x84)
X86_EXTERN_FUNC(__imp__sub_0040A7A0);
X86_FUNC(sub_0040A7A0) {
    uint32_t rate, floor;
    bool theirs = Session(rate, floor);
    if (rate) W32(ctx.esp + 4, rate);
    SetFrameLimit(rate);
    LOGR("Co-op session: tick rate %u Hz, input delay floor %u ms (%s)", R32(ctx.esp + 4), floor, theirs ? "the host's" : "own");
    __imp__sub_0040A7A0(ctx, base);
}

// the input delay floor setter (SyncSim +0x8C)
X86_EXTERN_FUNC(__imp__sub_0040A740);
X86_FUNC(sub_0040A740) {
    uint32_t rate, floor;
    Session(rate, floor);
    W32(ctx.esp + 4, floor);
    __imp__sub_0040A740(ctx, base);
}

// The step delta. The game returns 1/rate s (rate = SyncSim +0x7C, left in eax), but each step waits
// floor(1000/rate) whole ms (sub_0040C039), so game time ran fast: 1% at 22 Hz, 6% at 60. Return the real step.
X86_EXTERN_FUNC(__imp__sub_002535D6);
X86_FUNC(sub_002535D6) {
    __imp__sub_002535D6(ctx, base);
    Apply();
    if (ctx.eax) X87_ST(0) = double(1000 / ctx.eax) / 1000.0;
}
