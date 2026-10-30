// The Xbox Communicator on the keyboard player's port, backed by the host's audio: the XPP device records say a
// headset is plugged in, and XVoiceCreateMediaObjectEx hands the game a microphone and a headphone of ours. The
// encoder, the voice packets and voice detection stay the game's own. Each image hooks its own copies of the XDK
// functions with its addresses (game/communicator.cpp, versus/communicator.cpp).
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/input.h"
#include "kernel/memory.h"
#include "xdk/communicator.h"
#include "xdk/hle.h"

using namespace kernel;

namespace dispatch {
uint32_t HostThunk(X86Func* fn);
}  // namespace dispatch


namespace {

// XMEDIAPACKET: +0 buffer, +4 max size, +8 completed size pointer, +0xC status pointer, +0x10 event or context
constexpr uint32_t kPending = 0x8000000A, kFlushed = 0x80004004, kNotImpl = 0x80004001;
constexpr uint32_t kAcceptInput = 1, kAcceptOutput = 2;  // XMO_STATUSF
// the object is its vtable pointer, then the vtable: XMediaObject's 7 methods and a few spare
constexpr uint32_t kSlots = 10, kObjectSize = 4 + kSlots * 4;
// recorded sound kept waiting for the game, and played sound queued ahead of the device
constexpr uint32_t kMaxLagMs = 200, kAheadMs = 60;
// the Opus voice link beside the game's 8 kHz voice; off leaves the game's own voice alone
constexpr bool kVoiceLink = true;
// push to talk keeps the microphone open this long after its key is let go
constexpr int64_t kTalkHoldMs = 300;

struct Packet {
    uint32_t buffer, size, completed, status, context;
};

struct Media {
    bool microphone = false, link = false;
    uint32_t port = 0, max_packets = 1, callback = 0, context = 0;
    uint32_t rate = 16000, channels = 1, align = 2;
    int refs = 1;
    audio::VoiceStream* stream = nullptr;
    // without a device: silence recorded and sound dropped at the format's pace
    double budget = 0;
    std::chrono::steady_clock::time_point last;
    std::deque<Packet> packets;
};

struct Done {
    uint32_t callback, context, packet_context, status;
};

std::recursive_mutex g_lock;     // the objects and their packets
std::recursive_mutex g_deliver;  // held while callbacks run: a Release waits for them, none comes after it
std::map<uint32_t, Media> g_media;

uint32_t BytesPerMs(const Media& m) { return std::max<uint32_t>(m.rate * m.align / 1000, 1); }

void Complete(const Media& m, const Packet& p, uint32_t status, uint32_t bytes, std::vector<Done>& done) {
    if (p.completed) W32(p.completed, bytes);
    if (p.status) W32(p.status, status);
    if (m.callback) done.push_back({m.callback, m.context, p.context, status});
    else if (p.context)
        if (ObjectRef e = FromHandle(p.context)) SetEvent(*e);
}

void Deliver(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void Flush(Media& m, std::vector<Done>& done) {
    for (const Packet& p : m.packets) Complete(m, p, kFlushed, 0, done);
    m.packets.clear();
}

void Step(Media& m, std::vector<Done>& done, bool open) {
    auto now = std::chrono::steady_clock::now();
    if (!m.stream) {
        double ms = std::chrono::duration<double, std::milli>(now - m.last).count();
        m.budget = std::min(m.budget + ms * BytesPerMs(m), double(kMaxLagMs * BytesPerMs(m)));
    }
    m.last = now;
    if (m.microphone) {
        if (m.stream) {
            uint32_t have = audio::Available(m.stream), keep = kMaxLagMs * BytesPerMs(m);
            static std::vector<uint8_t> drop;
            if (have > keep) {
                drop.resize(have - keep);
                audio::Read(m.stream, drop.data(), uint32_t(drop.size()));
            }
        }
        while (!m.packets.empty()) {
            Packet p = m.packets.front();
            if (m.stream ? audio::Available(m.stream) < p.size : m.budget < p.size) break;
            if (m.stream) {
                audio::Read(m.stream, HostPtr(p.buffer), p.size);
                // closed, the game hears silence: nothing to send, nothing for the guards
                if (!open) std::memset(HostPtr(p.buffer), 0, p.size);
            } else {
                std::memset(HostPtr(p.buffer), 0, p.size);
                m.budget -= p.size;
            }
            m.packets.pop_front();
            Complete(m, p, 0, p.size, done);
        }
        return;
    }
    while (!m.packets.empty()) {
        Packet p = m.packets.front();
        if (m.stream ? audio::Queued(m.stream) >= kAheadMs * BytesPerMs(m) : m.budget < p.size) break;
        if (m.stream) {
            // the voice link's copy plays instead; silence keeps the pace
            static const std::vector<uint8_t> silence(0x10000);
            bool linked = audio::VoiceLinkHeard() && p.size <= silence.size();
            audio::Write(m.stream, linked ? silence.data() : HostPtr(p.buffer), p.size);
        } else {
            m.budget -= p.size;
        }
        m.packets.pop_front();
        Complete(m, p, 0, p.size, done);
    }
}

bool MicOpen() {
    static int64_t until = 0;  // the pump thread's
    if (!app::Get().push_to_talk) return true;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (input::EventKeyHeld(input::KeyEvent::PushToTalk)) until = now + kTalkHoldMs;
    return now < until;
}

// the console's USB driver completes packets on its own, the title's callbacks run at DPC level
void Pump(X86Context& ctx) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard deliver(g_deliver);
        std::vector<Done> done;
        {
            std::lock_guard lock(g_lock);
            bool open = MicOpen();
            audio::SetMicOpen(open);
            for (auto& [obj, m] : g_media) Step(m, done, open);
        }
        Deliver(ctx, done);
    }
}

Media* Find(uint32_t obj) {
    auto it = g_media.find(obj);
    return it == g_media.end() ? nullptr : &it->second;
}

uint32_t Arg(X86Context& ctx, int i) { return R32(ctx.esp + 4 + 4 * uint32_t(i)); }

void AddRef(X86Context& __restrict ctx, uint8_t*) {
    std::lock_guard lock(g_lock);
    Media* m = Find(Arg(ctx, 0));
    Return(ctx, m ? uint32_t(++m->refs) : 0, 1);
}

void Release(X86Context& __restrict ctx, uint8_t*) {
    uint32_t obj = Arg(ctx, 0), refs = 0;
    std::vector<Done> done;
    {
        std::lock_guard deliver(g_deliver);
        std::lock_guard lock(g_lock);
        if (Media* m = Find(obj); m && (refs = uint32_t(--m->refs)) == 0) {
            Flush(*m, done);
            if (m->stream) audio::Close(m->stream);
            if (m->link) audio::StopVoiceLink();
            LOGI("communicator: %s on port %u released", m->microphone ? "microphone" : "headphone", m->port);
            g_media.erase(obj);
            mem::FreeSystem(obj);
        }
    }
    Deliver(ctx, done);
    Return(ctx, refs, 1);
}

void GetInfo(X86Context& __restrict ctx, uint8_t*) {
    std::lock_guard lock(g_lock);
    Media* m = Find(Arg(ctx, 0));
    // XMEDIAINFO: flags, input size, output size, max lookahead
    if (uint32_t info = Arg(ctx, 1); m && info) {
        W32(info, 0);
        W32(info + 4, m->align);
        W32(info + 8, m->align);
        W32(info + 12, 0);
    }
    Return(ctx, 0, 2);
}

void GetStatus(X86Context& __restrict ctx, uint8_t*) {
    std::lock_guard lock(g_lock);
    Media* m = Find(Arg(ctx, 0));
    if (uint32_t status = Arg(ctx, 1); m && status)
        W32(status, m->packets.size() < m->max_packets ? (m->microphone ? kAcceptOutput : kAcceptInput) : 0);
    Return(ctx, 0, 2);
}

// (in, out): the microphone fills out, the headphone plays in
void Process(X86Context& __restrict ctx, uint8_t*) {
    std::lock_guard lock(g_lock);
    Media* m = Find(Arg(ctx, 0));
    uint32_t xp = m ? Arg(ctx, m->microphone ? 2 : 1) : 0;
    if (!xp) {
        Return(ctx, 0x80070057, 3);  // E_INVALIDARG
        return;
    }
    Packet p{R32(xp), R32(xp + 4), R32(xp + 8), R32(xp + 12), R32(xp + 16)};
    p.size -= p.size % m->align;
    if (p.completed) W32(p.completed, 0);
    if (p.status) W32(p.status, kPending);
    m->packets.push_back(p);
    Return(ctx, 0, 3);
}

void Discontinuity(X86Context& __restrict ctx, uint8_t*) { Return(ctx, 0, 1); }

void FlushMethod(X86Context& __restrict ctx, uint8_t*) {
    std::vector<Done> done;
    {
        std::lock_guard deliver(g_deliver);
        std::lock_guard lock(g_lock);
        if (Media* m = Find(Arg(ctx, 0))) Flush(*m, done);
    }
    Deliver(ctx, done);
    Return(ctx, 0, 1);
}

void Unknown(X86Context& __restrict ctx, uint8_t*) {
    LOG_FIRST(8, "communicator: a method past Flush was called (return address %08X)", R32(ctx.esp));
    Return(ctx, kNotImpl, 1);
}

void Present(uint32_t type, bool microphone) {
    int port = input::KeyboardPort();
    uint32_t cur = port >= 0 ? 1u << port : 0, old = R32(type);
    if (cur == old) return;
    W32(type + 4, R32(type + 4) | (cur ^ old));
    W32(type, cur);
    if (microphone) LOGI("communicator: headset on port %d", port);
}


}  // namespace

namespace communicator {

void Refresh(X86Context& ctx, const Types& types) {
    uint32_t type = R32(ctx.esp + 4);
    if (type == types.microphone || type == types.headphone) Present(type, type == types.microphone);
}

bool Create(X86Context& ctx, const Types& types) {
    uint32_t type = R32(ctx.esp + 4);
    if (type != types.microphone && type != types.headphone) return false;
    static std::once_flag pump;
    std::call_once(pump, [] { StartHostThread("communicator", Pump); });
    Media m;
    m.microphone = type == types.microphone;
    m.port = R32(ctx.esp + 8);
    m.max_packets = std::max<uint32_t>(R32(ctx.esp + 12), 1);
    // WAVEFORMATEX: tag, channels, rate, bytes a second, block align, bits
    if (uint32_t wfx = R32(ctx.esp + 16)) {
        m.channels = std::max<uint16_t>(R16(wfx + 2), 1);
        m.rate = std::max<uint32_t>(R32(wfx + 4), 1000);
        m.align = std::max<uint16_t>(R16(wfx + 12), 1);
        if (R16(wfx) != 1 || R16(wfx + 14) != 16) LOGE("communicator: format tag %u, %u bits", R16(wfx), R16(wfx + 14));
    }
    m.callback = R32(ctx.esp + 20);
    m.context = R32(ctx.esp + 24);
    std::string device;
    {
        std::lock_guard lock(app::UiLock());
        device = app::Get().microphone;
    }
    m.link = kVoiceLink && m.microphone;
    if (m.link) audio::StartVoiceLink(device);
    m.stream = m.microphone ? audio::OpenMicrophone(device, m.rate, m.channels)
                            : audio::OpenSpeaker(m.rate, m.channels);
    m.last = std::chrono::steady_clock::now();
    LOGI("communicator: %s on port %u, %u Hz, %u ch, %u packets, callback %08X%s", m.microphone ? "microphone" : "headphone",
         m.port, m.rate, m.channels, m.max_packets, m.callback, m.stream ? "" : ", no device: silent");
    uint32_t obj = mem::AllocSystem(kObjectSize);
    static X86Func* const kMethods[] = {AddRef, Release, GetInfo, GetStatus, Process, Discontinuity, FlushMethod};
    for (uint32_t i = 0; i < kSlots; i++)
        W32(obj + 4 + i * 4, dispatch::HostThunk(i < std::size(kMethods) ? kMethods[i] : Unknown));
    W32(obj, obj + 4);
    {
        std::lock_guard lock(g_lock);
        g_media[obj] = std::move(m);
    }
    if (uint32_t out = R32(ctx.esp + 28)) W32(out, obj);
    Return(ctx, 0, 7);
    return true;
}

}  // namespace communicator
