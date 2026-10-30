// Mixer output through SDL2, as audio/xaudio2.cpp does it on Windows: a thread renders 10 ms buffers and queues them,
// queueing deeper when the device starves. The Switch and most Linux setups play stereo: the 5.1 mix folds down as the
// console's DOLBY image does, or to mono; the surround setting opens a 5.1 device where there is one.
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>

#include "audio/output.h"
#include "core/log.h"
#include "port/port.h"

namespace audio {

namespace {

constexpr float kCentreFold = 0.707f;
constexpr float kBackFold = 0.707f;
// the fold adds up to 2.4 times a channel's level, and loud passages of the game's own mix pass 1.0: scaled back,
// then a limiter (2 ms look-ahead, ~100 ms release) keeps the peaks under full scale. A plain clamp cut them off,
// which crackled on the bass.
constexpr float kFoldGain = 1.0f / (1.0f + kCentreFold);
constexpr float kCeiling = 0.97f;
constexpr uint32_t kLookAhead = 96;               // 2 ms at 48 kHz
constexpr float kAttack = 5.0f / kLookAhead;      // down within the look-ahead
constexpr float kRelease = 1.0f / (0.1f * 48000);  // up over ~100 ms
float g_limit = 1.0f;  // the limiter's gain, the pump thread's alone
// what the limiter did over the last 10 s, logged when it did anything
struct LimitStats {
    float peak = 0.0f;
    float low = 1.0f;
    uint32_t limited = 0;
    uint32_t frames = 0;
} g_stats;

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_paused{false};

// the 5.1 frames into the device's channels: 6 as they are, else the stereo (or mono) fold
void Fold(const float* in, float* out, uint32_t frames, int channels, bool mono) {
    if (channels == int(kChannels)) {
        std::copy(in, in + frames * kChannels, out);
    } else {
        float* o = out;
        for (uint32_t i = 0; i < frames; i++, in += kChannels) {
            float l = (in[FrontLeft] + kCentreFold * in[Centre] + kBackFold * in[BackLeft]) * kFoldGain;
            float r = (in[FrontRight] + kCentreFold * in[Centre] + kBackFold * in[BackRight]) * kFoldGain;
            if (mono || channels == 1) l = r = (l + r) * 0.5f;
            *o++ = l;
            if (channels > 1) *o++ = r;
        }
    }
    // A limiter with 2 ms of look-ahead: the gain slides down before a peak arrives and back up over ~100 ms. The
    // gain jumping once a 10 ms buffer clicked on loud ambience and rattled the bass.
    static std::vector<float> delay;
    static std::vector<float> need;
    static uint32_t at = 0;
    const uint32_t ch = uint32_t(channels);
    if (delay.size() != kLookAhead * ch) {
        delay.assign(kLookAhead * ch, 0.0f);
        need.assign(kLookAhead, 1.0f);
        at = 0;
    }
    // SCDA_AUDIO_DUMP=file: the fold before the limiter as raw float frames, to measure the mix
    static FILE* dump = [] {
        const char* p = std::getenv("SCDA_AUDIO_DUMP");
        return p && *p ? std::fopen(p, "ab") : nullptr;  // appended across relaunches
    }();
    if (dump && std::fwrite(out, sizeof(float), size_t(frames) * channels, dump)) std::fflush(dump);
    for (uint32_t f = 0; f < frames; f++) {
        float* frame = out + f * ch;
        float peak = 0.0f;
        for (uint32_t c = 0; c < ch; c++) peak = std::max(peak, std::fabs(frame[c]));
        g_stats.peak = std::max(g_stats.peak, peak);
        g_stats.low = std::min(g_stats.low, g_limit);
        g_stats.limited += g_limit < 0.99f;
        g_stats.frames++;
        need[at] = peak > kCeiling ? kCeiling / peak : 1.0f;
        float low = 1.0f;
        for (float x : need) low = std::min(low, x);
        g_limit += (low - g_limit) * (low < g_limit ? kAttack : kRelease);
        for (uint32_t c = 0; c < ch; c++) {
            float x = delay[at * ch + c];
            delay[at * ch + c] = frame[c];
            frame[c] = std::clamp(x * g_limit, -1.0f, 1.0f);
        }
        at = (at + 1) % kLookAhead;
    }
    if (g_stats.frames >= 10 * kRate) {
        if (g_stats.limited) {
            LOGI("audio: the mix peaked at %.2f, the limiter held it %.0f%% of the time, down to %.2f", g_stats.peak,
                 100.0 * g_stats.limited / g_stats.frames, g_stats.low);
        }
        g_stats = {};
    }
}

SDL_AudioDeviceID Open(int channels, SDL_AudioSpec& got) {
    SDL_AudioSpec want{};
    want.freq = int(kRate);
    want.format = AUDIO_F32SYS;
    want.channels = uint8_t(channels);
#ifdef __SWITCH__
    // SDL's Switch driver plays two buffers of this many frames and sees one finish only every 5 ms audio renderer
    // frame: at 512 (10.7 ms) its thread had ~5 ms to refill, a busy core made it late and the gap clicked hard on
    // steady bass (ambience, menu sounds). 2048 leaves it ~37 ms.
    want.samples = 2048;
#else
    want.samples = 512;
#endif
    return SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
}

void Pump(std::function<void(float*, uint32_t)> render, SDL_AudioDeviceID dev, int channels) {
    port::NameThread("audio");
    // above the busy guest threads, which on the Switch kept it waiting at the default priority until they blocked
    // (voices played drawn out, crackles): time critical is 0x2B there (switch_platform.cpp)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
    constexpr uint32_t kFrames = kRate / 100;
    constexpr uint32_t kMinQueued = 5;  // 3 still crackled on the Switch
    constexpr uint32_t kMaxQueued = 10;
    std::vector<float> mix(kFrames * kChannels), out(kFrames * kChannels);
    uint32_t depth = kMinQueued;
    const uint32_t buffer = kFrames * uint32_t(channels) * sizeof(float);
    bool stopped = false;
    SDL_PauseAudioDevice(dev, 0);
    for (uint64_t n = 0;;) {
        if (g_paused != stopped) {
            stopped = !stopped;
            SDL_PauseAudioDevice(dev, stopped);
        }
        if (stopped) {
            SDL_Delay(10);
            continue;
        }
        uint32_t queued = SDL_GetQueuedAudioSize(dev) / buffer;
        // a starved device crackles: queue one buffer more each time, up to kMaxQueued
        if (n && !queued && depth < kMaxQueued) {
            depth++;
            LOGI("audio: output starved, queueing %u ms", depth * 10);
        }
        if (queued >= depth) {
            SDL_Delay(2);
            continue;
        }
        render(mix.data(), kFrames);
        g_mono = g_mode == kOutputMono;
        Fold(mix.data(), out.data(), kFrames, channels, g_mono);
        SDL_QueueAudio(dev, out.data(), buffer);
        n++;
    }
}

}  // namespace

bool IsSurround() { return g_surround; }
bool IsMono() { return g_mono; }
// the device opens once at boot with the setting then, the fold follows a change at once
void SetOutputMode(uint32_t mode) { g_mode = mode; }
void SetPaused(bool paused) { g_paused = paused; }

void StartOutput(std::function<void(float*, uint32_t)> render) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOGE("audio: SDL audio failed to start, no sound: %s", SDL_GetError());
        return;
    }
    // automatic is stereo here: SDL2 cannot tell a 5.1 default device from one that only takes 5.1 streams
    bool surround = g_mode == kOutputSurround;
    SDL_AudioSpec got{};
    SDL_AudioDeviceID dev = surround ? Open(int(kChannels), got) : 0;
    if (!dev || got.channels != kChannels) {
        if (dev) SDL_CloseAudioDevice(dev);
        surround = false;
        dev = Open(2, got);
    }
    if (!dev || got.format != AUDIO_F32SYS || got.freq != int(kRate)) {
        LOGE("audio: no %u Hz float output (%s), no sound", kRate, dev ? "other format" : SDL_GetError());
        if (dev) SDL_CloseAudioDevice(dev);
        return;
    }
    g_surround = surround;
    g_mono = g_mode == kOutputMono;
    LOGI("audio: SDL %s, %d channels", SDL_GetCurrentAudioDriver(), got.channels);
    std::thread(Pump, std::move(render), dev, int(got.channels)).detach();
}

}  // namespace audio
