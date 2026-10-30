// The wideband voice link beside the game's own voice. While the game sends VDP (game data with its 8 kHz Voxware
// voice) to a peer, a 16 kHz Opus copy of the same microphone goes to that peer over the same socket, and the peer
// plays it in place of the game's. The game's path stays whole, so voice activation, muting and guards hearing the
// player are still the game's. Versus is client and server: a client's game only sends to the host, so whoever
// receives a talker's own packets passes them on to its other peers, marked relayed, and relayed ones go no further.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <opus.h>
#include <SDL3/SDL.h>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace xnet {
bool SendVoiceLink(uint32_t address, const void* data, int len);  // xdk/xnet.cpp
}  // namespace xnet

namespace audio {

namespace {

constexpr int kRate = 16000, kFrame = 320;  // 20 ms
constexpr int kBitrate = 16000, kComplexity = 10;
constexpr char kMagic[4] = {'S', 'C', 'D', 'V'};
constexpr uint8_t kVersion = 2;
constexpr size_t kHeader = 12;  // magic, version, flags, sequence, talker
constexpr uint8_t kRelayed = 1;
constexpr size_t kMaxPayload = 1276;
// a peer the game sent voice to this recently is sent ours; ours heard this recently mutes the game's
constexpr int64_t kSendHoldMs = 300, kHeardHoldMs = 400, kTalkerTimeoutMs = 10000;
// jitter buffer: playback starts at kTargetFrames, a gap is concealed for at most kMaxConcealed frames, a deeper queue
// than kMaxQueued drops its oldest
constexpr int kTargetFrames = 2, kMaxConcealed = 3, kMaxQueued = 10;
constexpr size_t kMaxIncoming = 256;
const SDL_AudioSpec kSpec{SDL_AUDIO_S16, 1, kRate};

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// fed by the worker, drained by SDL's playback thread
struct Talker {
    SDL_AudioStream* stream = nullptr;
    OpusDecoder* decoder = nullptr;
    std::mutex lock;
    std::map<uint16_t, std::vector<uint8_t>> queue;  // by sequence, wrapping
    bool playing = false;
    uint16_t next = 0;
    int concealed = 0;
    int64_t last = 0;
};

std::mutex g_lock;  // starting and stopping
int g_users = 0;
std::thread g_worker;
std::atomic<bool> g_stop{false};
std::atomic<int64_t> g_heard{0};
std::atomic<bool> g_running{false};
std::atomic<bool> g_mic_open{true};
std::atomic<uint32_t> g_self{0};  // this talker's id, new each start
// packets the game's sockets took for the link, with the address they came from
struct Incoming {
    uint32_t from;
    std::vector<uint8_t> bytes;
};
std::mutex g_in_lock;
std::deque<Incoming> g_in;
std::mutex g_peers_lock;
std::map<uint32_t, int64_t> g_peers;  // address -> the game's last voice send to it

uint32_t Talker32(const uint8_t* b) { return uint32_t(b[8] | b[9] << 8 | b[10] << 16 | uint32_t(b[11]) << 24); }

// the peers the game sent VDP to lately
std::vector<uint32_t> Peers(int64_t now) {
    std::vector<uint32_t> out;
    std::lock_guard lock(g_peers_lock);
    for (const auto& [address, at] : g_peers)
        if (now - at < kSendHoldMs) out.push_back(address);
    return out;
}

bool Before(uint16_t a, uint16_t b) { return int16_t(a - b) < 0; }

uint16_t Oldest(const Talker& t) {
    uint16_t oldest = t.queue.begin()->first;
    for (const auto& [sequence, packet] : t.queue)
        if (Before(sequence, oldest)) oldest = sequence;
    return oldest;
}

// one frame of this talker's voice, false while they are silent
bool Pull(Talker& t, int16_t (&pcm)[kFrame]) {
    std::lock_guard lock(t.lock);
    if (!t.playing) {
        if (int(t.queue.size()) < kTargetFrames) return false;
        t.playing = true;
        t.next = Oldest(t);
        t.concealed = 0;
    }
    while (int(t.queue.size()) > kMaxQueued) {
        uint16_t oldest = Oldest(t);
        t.queue.erase(oldest);
        t.next = uint16_t(oldest + 1);
    }
    // a gap longer than concealment covers is skipped, not filled
    if (!t.queue.empty()) {
        uint16_t oldest = Oldest(t);
        if (int16_t(oldest - t.next) > kMaxConcealed) t.next = oldest;
    }
    if (auto it = t.queue.find(t.next); it != t.queue.end()) {
        int n = opus_decode(t.decoder, it->second.data(), opus_int32(it->second.size()), pcm, kFrame, 0);
        t.queue.erase(it);
        t.next++;
        t.concealed = 0;
        if (n != kFrame) std::fill(std::begin(pcm), std::end(pcm), int16_t(0));
        return true;
    }
    if (t.queue.empty() && t.concealed >= kMaxConcealed) {
        t.playing = false;
        return false;
    }
    opus_decode(t.decoder, nullptr, 0, pcm, kFrame, 0);
    t.next++;
    t.concealed++;
    return true;
}

void SDLCALL Feed(void* user, SDL_AudioStream* stream, int additional, int) {
    auto& t = *static_cast<Talker*>(user);
    int16_t pcm[kFrame];
    for (int fed = 0; fed < additional; fed += int(sizeof(pcm))) {
        if (!Pull(t, pcm)) return;
        SDL_PutAudioStreamData(stream, pcm, sizeof(pcm));
    }
}

// destroying the stream waits out a callback in progress
void Destroy(Talker* t) {
    SDL_DestroyAudioStream(t->stream);
    opus_decoder_destroy(t->decoder);
    delete t;
}

std::string Ip(uint32_t a) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    return s;
}

void Run(std::string microphone) {
    VoiceStream* capture = OpenMicrophone(microphone, kRate, 1);
    int error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (encoder) {
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kBitrate));
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(kComplexity));
        opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    } else {
        LOGE("voice link: no Opus encoder: %s", opus_strerror(error));
    }
    SDL_AudioDeviceID playback = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (playback) SDL_ResumeAudioDevice(playback);
    else LOGE("voice link: no playback device: %s", SDL_GetError());
    std::map<uint32_t, Talker*> talkers;  // by talker id
    uint16_t sequence = 0;
    int16_t pcm[kFrame];
    uint8_t packet[kHeader + kMaxPayload];
    std::memcpy(packet, kMagic, 4);
    packet[4] = kVersion;
    packet[5] = 0;
    uint32_t self = g_self;
    for (int i = 0; i < 4; i++) packet[8 + i] = uint8_t(self >> (8 * i));
    std::deque<Incoming> in;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        int64_t now = NowMs();
        {
            std::lock_guard lock(g_in_lock);
            in.swap(g_in);
        }
        for (; !in.empty(); in.pop_front()) {
            Incoming& packet_in = in.front();
            std::vector<uint8_t>& b = packet_in.bytes;
            uint32_t address = packet_in.from, talker = Talker32(b.data());
            if (talker == self) continue;
            if (!(b[5] & kRelayed)) {
                b[5] |= kRelayed;
                for (uint32_t peer : Peers(now))
                    if (peer != address) xnet::SendVoiceLink(peer, b.data(), int(b.size()));
            }
            if (!playback) continue;
            Talker*& t = talkers[talker];
            if (!t) {
                auto made = new Talker;
                made->decoder = opus_decoder_create(kRate, 1, &error);
                made->stream = made->decoder ? SDL_CreateAudioStream(&kSpec, &kSpec) : nullptr;
                if (!made->stream || !SDL_SetAudioStreamGetCallback(made->stream, Feed, made) ||
                    !SDL_BindAudioStream(playback, made->stream)) {
                    LOGE("voice link: no playback for %s: %s", Ip(address).c_str(), SDL_GetError());
                    if (made->stream) SDL_DestroyAudioStream(made->stream);
                    if (made->decoder) opus_decoder_destroy(made->decoder);
                    delete made;
                    talkers.erase(talker);
                    continue;
                }
                t = made;
                LOGI("voice link: hearing talker %08X from %s", talker, Ip(address).c_str());
            }
            uint16_t seq = uint16_t(b[6] | b[7] << 8);
            std::lock_guard lock(t->lock);
            t->last = now;
            g_heard = now;
            if (t->playing && Before(seq, t->next)) continue;
            t->queue[seq].assign(b.begin() + kHeader, b.end());
        }
        for (auto it = talkers.begin(); it != talkers.end();) {
            int64_t last;
            {
                std::lock_guard lock(it->second->lock);
                last = it->second->last;
            }
            if (now - last < kTalkerTimeoutMs) {
                ++it;
                continue;
            }
            LOGI("voice link: talker %08X silent, dropped", it->first);
            Destroy(it->second);
            it = talkers.erase(it);
        }
        // every frame is encoded, sent or not, so the encoder never jumps
        while (capture && encoder && Available(capture) >= sizeof(pcm)) {
            Read(capture, pcm, sizeof(pcm));
            // silence rather than nothing, so the peer keeps the link's voice and never falls back to the game's
            if (!g_mic_open) std::fill(std::begin(pcm), std::end(pcm), int16_t(0));
            int bytes = opus_encode(encoder, pcm, kFrame, packet + kHeader, opus_int32(kMaxPayload));
            uint16_t seq = sequence++;
            if (bytes <= 0) continue;
            packet[6] = uint8_t(seq);
            packet[7] = uint8_t(seq >> 8);
            for (uint32_t address : Peers(now)) xnet::SendVoiceLink(address, packet, int(kHeader) + bytes);
        }
    }
    for (auto& [key, t] : talkers) Destroy(t);
    if (playback) SDL_CloseAudioDevice(playback);
    if (encoder) opus_encoder_destroy(encoder);
    if (capture) Close(capture);
    LOGI("voice link: stopped");
}

}  // namespace

void StartVoiceLink(const std::string& microphone) {
    std::lock_guard lock(g_lock);
    if (g_users++) return;
    g_stop = false;
    g_heard = 0;
    g_running = true;
    g_self = std::random_device{}() | 1;
    g_worker = std::thread(Run, microphone);
    LOGI("voice link: started");
}

void StopVoiceLink() {
    std::lock_guard lock(g_lock);
    if (g_users == 0 || --g_users) return;
    g_stop = true;
    g_running = false;
    if (g_worker.joinable()) g_worker.join();
    {
        std::lock_guard in(g_in_lock);
        g_in.clear();
    }
    g_heard = 0;
    std::lock_guard peers(g_peers_lock);
    g_peers.clear();
}

void VoiceSent(uint32_t address) {
    std::lock_guard lock(g_peers_lock);
    auto [it, added] = g_peers.insert_or_assign(address, NowMs());
    if (added) LOGI("voice link: the game talks to %s", Ip(address).c_str());
}

bool TakeVoiceLinkPacket(uint32_t address, const uint8_t* data, int bytes) {
    if (bytes <= int(kHeader) || std::memcmp(data, kMagic, 4)) return false;
    if (data[4] != kVersion) {
        LOG_FIRST(4, "voice link: dropped a version %u packet from %s, both players need the same build", data[4], Ip(address).c_str());
        return true;
    }
    if (!g_running) return true;
    std::lock_guard lock(g_in_lock);
    if (g_in.size() < kMaxIncoming) g_in.push_back({address, std::vector<uint8_t>(data, data + bytes)});
    return true;
}

void SetMicOpen(bool open) { g_mic_open = open; }

bool VoiceLinkHeard() {
    int64_t heard = g_heard;
    return heard && NowMs() - heard < kHeardHoldMs;
}

}  // namespace audio
