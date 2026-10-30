// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <optional>
#include <vector>


#include "app/app.h"
#include "audio/audio.h"
#include "audio/output.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;

// mix bins (DSMIXBIN) the host mixes by name
constexpr uint32_t kBinCentre = 2;
constexpr uint32_t kBinSub = 3;
constexpr uint32_t kBinCrosstalkFrontLeft = 6;
constexpr uint32_t kBinCrosstalkFrontRight = 7;
constexpr uint32_t kBinCrosstalkBackLeft = 8;
constexpr uint32_t kBinCrosstalkBackRight = 9;
constexpr uint32_t kBinFirstSend = 10;  // effect sends from here up
constexpr uint32_t kBinI3dl2 = 10;
constexpr uint32_t kSends = 1u << kBinI3dl2;  // the effects image mixes these into its reverb (state X:0x1540)
constexpr uint32_t kLanes = kBinFirstSend;     // the mixer's lanes are bins 0 to 9, speakers then crosstalk

constexpr uint32_t kEarLength = 64;  // past the longest interaural delay, 31 samples

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0;
    uint32_t owned = 0;  // the buffer's own memory, made with it: freed by SetBufferData and Release
    bool playing = false, looping = false, paused = false;
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    bool fed = false;  // had packets the last frame: running dry while still active is an underrun
    uint32_t dry = 0, last_size = 0;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    bool link_voice = false;  // the game's speaker copy of a talker's voice: silent while the voice link plays it
    bool is3d = false;
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    float lf = 1, hf = 1;  // I3DL2 direct path: lows and highs (I3DL2Source)
    float lp[2] = {};      // its low pass, per channel
    float room = 1;        // I3DL2 reverb send of a 3D voice (lRoom)
    float gain[2][kLanes] = {};  // the last block's levels, the next block ramps from them
    bool has_gain = false;
    // a mono 3D voice's interaural delay in samples and its recent samples
    float itd = 0;
    float ear[kEarLength] = {};
    uint32_t ear_pos = 0;
    // ADPCM block cache
    int64_t block = -1;
    int16_t decoded[65 * 8];
};

// a 3D voice as the library's 3D pass sees it (0x4CE757 into the slot builder 0x4D45CB), as gains
struct Place {
    bool on = false;
    float att = 1;      // every bin
    float front = 1;    // crosstalk front pair
    float back = 0;     // crosstalk back pair, silent on a stereo console
    float centre = 1;   // centre bin
    float azimuth = 0;
    float elevation = 0;
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;
bool g_headphones = false;  // IDirectSound_EnableHeadphones
// IDirectSound_SetMixBinHeadroom, a step halves the bin. The library starts bins 0..30 at 1 and 31 at 0
// (0x4D032A..0x4D033B), the game sets 0..9 to 0 (0x19390)
uint8_t g_bin_headroom[32] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                              1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};

// the crossover between I3DL2's lows and highs, a one pole low pass at 1.5 kHz on the 48 kHz mix: 1 - e^(-2 pi 1500 / 48000)
constexpr float kLowPass = 0.178f;

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

// Xbox ADPCM plays 64 frames per 36 byte block (header sample + 63 nibbles, the last nibble is dropped), not
// MS IMA's 65: counting 65 stretches every ADPCM sound by 1/64.
constexpr uint32_t kAdpcmFrames = 64;

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * kAdpcmFrames;
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / kAdpcmFrames * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, 65 frames decoded, kAdpcmFrames played
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / kAdpcmFrames) * 36 * v.channels;
        if (at != v.block) {
            DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded);
            v.block = at;
        }
        return v.decoded[(f % kAdpcmFrames) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// The XDK's I3DL2 reverb as its DSP program runs it: a 24 kHz core, delays in samples at that rate
struct Reverb {
    struct Pole {
        float a = 0, b = 0, z = 0;
        float Run(float x) { return z = a * z + b * x + 1e-20f; }  // the offset keeps silence out of denormals
    };
    struct Line {
        std::vector<float> s;
        uint32_t w = 0;
        // a delay of the whole line reads its oldest sample
        float At(uint32_t delay) const { return s[(w + s.size() - delay % s.size()) % s.size()]; }
        void Put(float x) {
            s[w] = x;
            w = (w + 1) % uint32_t(s.size());
        }
    };
    struct Params {
        uint32_t refl, tap[4], in[4], chain[8], comb[4], echo;
        float early[4], late, diff[8], rest, cross;
        Pole in_lp, chain_lp[3], loop_lp[4], echo_lp[2];
    };
    static constexpr int kLen[14] = {6000, 6000, 6000, 6000, 680, 612, 640, 720, 1392, 1684, 2044, 2476, 24000, 24000};
    Line line[14];
    Params now, next;
    bool on = false, pending = false;
    float fade = 1, held[4] = {};
    uint32_t tick = 0;

    Reverb() {
        for (int i = 0; i < 14; i++) line[i].s.assign(kLen[i], 0.0f);
    }
    static float Lin(float mb) { return std::pow(10.0f, mb / 2000.0f); }
    // y = a * y1 + b * x: mb millibels at 0 Hz, hf more at ref Hz
    static Pole Design(int mb, int hf, float ref, float rate) {
        Pole p;
        if (mb <= -10000) return p;
        // 1.0 is past the DSP's fractions and comes out as -1
        p.b = mb ? Lin(float(mb)) : -1;
        if (hf >= 0) return p;
        p.b = Lin(float(mb));
        float g = Lin(2.0f * hf), c = std::cos(6.2831853f * ref / rate);
        p.a = (1 - g * c - std::sqrt(2 * g * (1 - c) - g * g * (1 - c * c))) / (1 - g);
        p.b *= 1 - p.a;
        return p;
    }
    // DSI3DL2LISTENER as 12 floats, the library's own conversion
    void Set(const float* p) {
        static const float kGain[4] = {0.57f, 0.52f, 0.54f, 0.6f}, kTap[4] = {0.1052f, 0.0673f, 0.1737f, 0.2404f};
        static const float kComb[4] = {0.2469f, 0.2209f, 0.2311f, 0.2608f};
        static const float kCombBase[4] = {0.003651f, 0.003515f, 0.003673f, 0.003946f};
        static const float kChain[8] = {0.040023f, 0.288186f, 0.603764f, 0.851905f,
                                        0.021224f, 0.37542f,  0.623583f, 0.759229f};
        static const float kSign[8] = {1, -1, -1, 1, 1, 1, -1, -1};
        static const int kInput[4] = {0, 58, 134, 232};
        auto al = [](int n) { return (n + 1) & ~3; };
        // taps are read in blocks of 4 samples, 16 back at least
        auto tap = [](int n) { return uint32_t(std::max((n + 3) & ~3, 16)); };
        float decay = std::max(p[3], 0.1f), ratio = std::max(p[4], 0.1f), d = std::clamp(p[9] * 0.01f, 0.0f, 1.0f);
        float sc = std::clamp((2 - p[10] * 0.01f) * (decay - 1) * 5, 1.0f, 60.0f);
        float f = sc >= 2.5f ? 1.0f : (sc - 0.8f) * 0.5882353f;
        // a loop of n samples loses 60 dB in the decay time, high frequencies sooner by the ratio
        auto loop = [&](int n) {
            float x = n / 24000.0f / decay * -6000.0f;
            int lf = int(x), hf = int(ratio < 1 ? x / ratio : x);
            return Design(lf, hf - lf, p[11], 24000);
        };
        Params q;
        q.in_lp = Design(0, int(p[1]), p[11], 48000);
        int refl = al(int(p[6] * 24000)), rev = int(p[8] * 24000), echo = int(f * 24000), prev = 0;
        q.refl = tap(refl);
        q.late = Lin(p[7] + p[0]);
        for (int k = 0; k < 4; k++) {
            q.early[k] = Lin(p[5] + p[0]) * kGain[k];
            q.tap[k] = tap(refl + al(int(rev * kTap[k])));
            q.in[k] = tap(refl + al(rev) + kInput[k]);
            q.comb[k] = tap(std::min(al(int((p[8] * kComb[k] + kCombBase[k]) * 24000)), kLen[4 + k] - 1));
            q.loop_lp[k] = loop(kLen[8 + k]);
            // echo times count from the input, a stage of the chain holds 250 ms
            int a = al(int(f * kChain[k] * 24000)), b = al(int(f * kChain[4 + k] * 24000)), m = std::max(a, b) - prev;
            float r = m > 5999 ? 5999.0f / m : 0;
            q.chain[k * 2] = tap(r ? al(int((a - prev) * r)) : a - prev);
            q.chain[k * 2 + 1] = tap(r ? al(int((b - prev) * r)) : b - prev);
            prev = b;
        }
        q.echo = tap(std::min(al(echo), 23999));
        for (int k = 0; k < 3; k++) q.chain_lp[k] = loop(echo >> 2);
        q.echo_lp[0] = q.echo_lp[1] = loop(echo);
        for (int k = 0; k < 8; k++) q.diff[k] = d * kSign[k];
        q.rest = d > 0 ? 1 - d : -1;
        q.cross = 0.2f * d;
        // a change fades out, swaps, fades in
        (on ? next : now) = q;
        pending = on;
        on = true;
    }
    // one 24 kHz sample in, front left, front right, back left, back right out
    void Frame(float x, float o[4]) {
        static const float kFeed[4] = {-0.4f, 0.4f, -0.4f, -0.4f};
        Params& q = now;
        Line* l = line;
        float t = l[0].At(q.refl), e[4], in[4], c[8], u[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            e[k] = t + l[0].At(q.tap[k]) + kFeed[k] * l[4 + k].At(q.comb[k]);
            in[k] = l[0].At(q.in[k]);
            c[k * 2] = l[k].At(q.chain[k * 2]);
            c[k * 2 + 1] = l[k].At(q.chain[k * 2 + 1]);
        }
        // the echoes, front and back
        float sa = c[1] + c[2] + c[4] + c[7], sb = c[0] + c[3] + c[5] + c[6];
        float ea = q.rest * sa + l[12].At(q.echo), eb = q.rest * sb + l[13].At(q.echo);
        for (int j = 0; j < 4; j++)
            sum += u[j] = l[8 + j].At(0) + q.diff[j * 2] * in[j & 1 ? 2 : 0] + q.diff[j * 2 + 1] * in[j & 1 ? 3 : 1];
        for (int j = 0; j < 4; j++) {
            o[j] = q.early[j] * e[j] + q.late * u[j] + q.late * 0.5f * (j < 2 ? ea : eb);
            l[4 + j].Put(e[j]);
            l[8 + j].Put(q.loop_lp[j].Run(u[j] - 0.5f * sum + q.cross * (sa + sb)));
        }
        l[0].Put(x);
        for (int k = 0; k < 3; k++) l[k + 1].Put(q.chain_lp[k].Run(c[k * 2 + 1]));
        l[12].Put(q.echo_lp[0].Run(ea));
        l[13].Put(q.echo_lp[1].Run(eb));
    }
    // adds the reverb of the 48 kHz send to the crosstalk bins, as the image's reverb writes them
    void Run(const float* send, float* out, uint32_t frames) {
        static const uint32_t kOut[4] = {kBinCrosstalkFrontLeft, kBinCrosstalkFrontRight, kBinCrosstalkBackLeft,
                                         kBinCrosstalkBackRight};
        for (uint32_t i = 0; on && i < frames; i++) {
            float x = now.in_lp.Run(send[i] * 0.5f), o[4];
            if (!(tick++ & 1)) {
                if (pending && (fade -= 1 / 128.0f) <= 0) {
                    now = next;
                    pending = false;
                    fade = 0;
                } else if (!pending) {
                    fade = std::min(fade + 1 / 128.0f, 1.0f);
                }
                Frame(x, o);
                for (int k = 0; k < 4; k++) held[k] = o[k] * fade;
            }
            for (int k = 0; k < 4; k++) out[i * kLanes + kOut[k]] += held[k];
        }
    }
} g_reverb;

// dsstdfx effect 4 on a pair of crosstalk bins: each side adds the other side's output of 11 samples back through a
// one pole filter (image coefficients 0x7645A1 and 0xF88313). Headphones turn it off.
struct CrossFeed {
    static constexpr uint32_t kDelay = 11;
    static constexpr float kPole = 0.924f;
    static constexpr float kFeed = -0.0585f;
    float s[2] = {};
    float past[2][kDelay] = {};
    uint32_t at = 0;
    void Run(float& left, float& right) {
        // the offset keeps silence out of denormals
        s[0] = kPole * s[0] + kFeed * past[1][at] + 1e-20f;
        s[1] = kPole * s[1] + kFeed * past[0][at] + 1e-20f;
        left += s[0];
        right += s[1];
        past[0][at] = left;
        past[1][at] = right;
        at = (at + 1) % kDelay;
    }
};
CrossFeed g_front_feed;
CrossFeed g_back_feed;
bool g_cross_feed = false;  // the title runs the effects image the effect was read from (UsesOwnImage)

// The library's full HRTF tables (DirectSoundUseFullHRTF): a filter number per azimuth step by elevation step, and
// the filters, 31 taps and the interaural delay in the last byte, the right ear's filter after the left's
constexpr uint32_t kHrtfIndex = SYM(DSound_HrtfIndex);
constexpr uint32_t kHrtfFilters = SYM(DSound_HrtfFilters);
constexpr uint32_t kHrtfFilterBytes = 32;
constexpr uint32_t kHrtfDelayByte = 31;
constexpr int kHrtfAzimuths = 61;     // 0 to 180 degrees in 3
constexpr int kHrtfElevations = 31;   // -90 to 90 degrees in 6
constexpr int kHrtfAzimuthStep = 3;
constexpr int kHrtfElevationStep = 6;
// the azimuth steps widen towards the poles: 3 up to 30 degrees of elevation, 6 up to 60, 12 beyond, none at 90
constexpr int kHrtfMidStep = 6;
constexpr int kHrtfPoleStep = 12;
constexpr int kHrtfLowBand = 30;
constexpr int kHrtfMidBand = 60;
constexpr int kHrtfPole = 90;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Degrees from straight ahead, plus to the right. The library's octant estimate (0x4D15A1), not atan2: it picks
// the HRTF entry and the interaural delay.
float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = Dot(d, front);
    float side = Dot(d, right);
    float a = std::fabs(ahead);
    float s = std::fabs(side);
    float deg = 0;
    if (a > s) deg = 45 * s / a;
    else if (s > 0) deg = 90 - 45 * a / s;
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

// degrees above the horizontal, the same estimate (0x4D15A1)
float Elevation(const float d[3], const float front[3], const float right[3], const float top[3]) {
    float flat = std::hypot(Dot(d, front), Dot(d, right));
    float up = Dot(d, top);
    float u = std::fabs(up);
    float deg = 0;
    if (flat > u) deg = 45 * u / flat;
    else if (u > 0) deg = 90 - 45 * flat / u;
    return up < 0 ? -deg : deg;
}

// The library's filter pick (0x4D1B70). With speakers behind the listener a sound behind takes the entry of its
// mirror in front (0x4D4D6C).
uint32_t HrtfEntry(float azimuth, float elevation, bool surround) {
    int half = kHrtfElevationStep / 2;
    int el = int(elevation < 0 ? elevation - half : elevation + half) / kHrtfElevationStep * kHrtfElevationStep;
    float az = std::fabs(azimuth);
    int step = kHrtfAzimuthStep;
    if (std::abs(el) > kHrtfLowBand) step = kHrtfMidStep;
    if (std::abs(el) > kHrtfMidBand) step = kHrtfPoleStep;
    int deg = int(az + step / 2.0f) / step * step;
    if (std::abs(el) == kHrtfPole) deg = 0;
    if (surround && deg > 90) deg = 180 - deg;
    int a = deg / kHrtfAzimuthStep;
    int e = (el + kHrtfPole) / kHrtfElevationStep;
    if (a < 0 || a >= kHrtfAzimuths) a = 0;
    if (e < 0 || e >= kHrtfElevations) e = 0;
    return static_cast<const uint16_t*>(HostPtr(kHrtfIndex))[a * kHrtfElevations + e];
}

// Interaural delay in samples, above 0 the left ear hears later: the last byte of the far ear's filter (0x4D4D14)
float Itd(float azimuth, float elevation, bool surround) {
    if (!(std::fabs(azimuth) <= 180) || !(std::fabs(elevation) <= 90)) return 0;
    const uint8_t* filters = static_cast<const uint8_t*>(HostPtr(kHrtfFilters));
    float delay = filters[HrtfEntry(azimuth, elevation, surround) * kHrtfFilterBytes + kHrtfDelayByte];
    return azimuth < 0 ? -delay : delay;
}

// A voice whose fifth bin is the centre (0x4CF25D) shares the front with it (0x4D1A55) by whole degrees below 45:
// tables 0x4D9250 and 0x4D9308 are 0.7071 cos^2(2a) and 0.8409 cos(2a)
void ShareCentre(Place& at) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr int kCentreLimit = 45;
    int az = int(std::fabs(at.azimuth));
    int el = int(std::fabs(at.elevation));
    if (az >= kCentreLimit || el >= kCentreLimit) {
        at.centre = 0;
        return;
    }
    float both = std::cos(2 * az * kRadiansPerDegree) * std::cos(2 * el * kRadiansPerDegree);
    at.centre *= 0.7071f * both;
    at.front *= 1 - 0.5f * both * both;
}

// Share of a 3D voice for the crosstalk pair behind, by power (0x4D1986): none to 45 degrees, half at 90, all from
// 135, drawn towards half and half within half a unit of the head
float BackShare(float azimuth, float elevation, float dist) {
    float a = std::fabs(azimuth) / 90;
    float e = std::fabs(elevation) / 90;
    float back = std::clamp(0.5f - (1 - a) * (1 - e), 0.0f, 1.0f);
    if (dist < 0.5f) back = ((back - 0.5f) * dist + 1) * 0.5f;
    return back;
}

// Where a 3D voice sits for the library's 3D pass (0x4D005C): distance attenuation and direction
Place Locate(const Voice& v) {
    Place at;
    if (!v.is3d) return at;
    at.on = true;
    float d[3] = {v.p[0] - g_listener[0], v.p[1] - g_listener[1], v.p[2] - g_listener[2]};
    float dist = std::sqrt(Dot(d, d));
    if (dist > v.min_dist) at.att = v.min_dist / std::min(dist, v.max_dist);
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    at.azimuth = Azimuth(d, g_front, right);
    at.elevation = Elevation(d, g_front, right, g_top);
    // surround without headphones splits the crosstalk pairs (0x4CF3F7), a stereo console plays the front alone
    if (audio::IsSurround() && !g_headphones) {
        float back = BackShare(at.azimuth, at.elevation, dist);
        at.front = std::sqrt(1 - back);
        at.back = std::sqrt(back);
    }
    // the centre takes the front's share too
    at.centre = at.front;
    if (v.nbins >= 5 && v.bin[4] == kBinCentre) ShareCentre(at);
    return at;
}

// The library's level of a 3D voice in a bin (0x4D45CB): the crosstalk pairs and the centre take their shares,
// every bin takes the distance attenuation. Left and right come from the HRTF, both bins of a pair get the same.
float BinScale(const Place& at, uint32_t bin) {
    if (!at.on) return 1.0f;
    if (bin == kBinCentre) return at.att * at.centre;
    if (bin == kBinCrosstalkFrontLeft || bin == kBinCrosstalkFrontRight) return at.att * at.front;
    if (bin == kBinCrosstalkBackLeft || bin == kBinCrosstalkBackRight) return at.att * at.back;
    return at.att;
}

// bins 0 to 5 are the speakers, the image's crosstalk effect adds 6 to 9 into the front and back pairs (AddCrosstalk)
static_assert(kBinSub == audio::Sub && kBinCentre == audio::Centre && audio::BackRight == 5);
static_assert(audio::kChannels == kBinCrosstalkFrontLeft);

// a slot onto the speakers, effect sends on none. A voice heard through effect sends alone plays on the front pair.
void AddToSpeakers(float g[kLanes], uint32_t b, float x, bool speakers) {
    if (!speakers) {
        g[audio::FrontLeft] = std::max(g[audio::FrontLeft], x);
        g[audio::FrontRight] = std::max(g[audio::FrontRight], x);
        return;
    }
    if (b >= kBinFirstSend) return;
    g[b] = std::max(g[b], x);
}

// gain of voice channel c on each speaker, w its share for the reverb. A hardware voice holds two channels and owns
// a run of slots, which alternate between them.
void Levels(const Voice& v, const Place& at, float g[2][kLanes], float w[2]) {
    std::memset(g, 0, 2 * kLanes * sizeof(float));
    w[0] = w[1] = 0;
    if (v.link_voice && audio::VoiceLinkHeard()) return;
    static const uint8_t kFront[2] = {0, 1};
    const uint8_t* bin = v.nbins ? v.bin : kFront;
    uint32_t n = v.nbins ? v.nbins : 2, voices = (std::max<uint32_t>(v.channels, 1) - 1) / 2 + 1;
    uint32_t per = std::max<uint32_t>(n / voices, 2), step = v.channels == 1 ? 1 : 2;
    bool speakers = false;
    // muted speaker bins do not count: such a voice is heard through its effect send
    for (uint32_t s = 0; s < n; s++)
        speakers |= bin[s] < kBinFirstSend && bin[s] != kBinSub && v.bin_mb[bin[s]] > -10000;
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t s = c; c < v.channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            float x = Db(v.bin_mb[b]) * v.volume * v.headroom * BinScale(at, b);
            if (kSends >> b & 1) {
                if (at.on) x *= v.room;
                w[c] += std::ldexp(x, -g_bin_headroom[b]);
                continue;
            }
            if (speakers && b >= kBinFirstSend) continue;  // other effect sends
            AddToSpeakers(g[c], b, x, speakers);
        }
    // the game can place a sound at NaN, every frame, that voice alone goes silent, where its
    // NaN gain would turn the whole mix to NaN, which plays as silence
    for (int c = 0; c < 2; c++) {
        for (uint32_t k = 0; k < kLanes; k++)
            if (!std::isfinite(g[c][k])) g[c][k] = 0;
        if (!std::isfinite(w[c])) w[c] = 0;
    }
}

// 3D levels follow the listener once a game frame: stepped per block, a fast turn clicks
void UpdateRamp(Voice& v, const float g[2][kLanes], uint32_t frames, float dg[2][kLanes]) {
    if (!v.has_gain) std::memcpy(v.gain, g, sizeof(v.gain));
    v.has_gain = true;
    for (int c = 0; c < 2; c++)
        for (uint32_t k = 0; k < kLanes; k++) dg[c][k] = (g[c][k] - v.gain[c][k]) / float(frames);
}

// the 3D voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos % kEarLength] = x;
    v.ear_pos++;
}
float EarAt(const Voice& v, float delay) {
    float back = std::max(delay, 0.0f);
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    float now = v.ear[(v.ear_pos - 1 - whole) % kEarLength];
    float before = v.ear[(v.ear_pos - 2 - whole) % kEarLength];
    return now + (before - now) * part;
}

// a stream goes quiet: a started one loses its pause
void Deactivate(Voice& v) {
    if (v.active) v.active = v.paused = v.ending = false;
}

// where a frame and the one after it are read from, for interpolation
struct Cursor {
    uint32_t data;
    uint32_t f;
    uint32_t next_data;
    uint32_t next_f;
};

// the stream's frame at its position, none once its packets ran out
std::optional<Cursor> StreamFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos -= FramesOf(v, pk.size);
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        // the headset's 8 kHz streams play to no device and run dry all the time
        if (v.fed && v.active && !v.ending && v.rate != 8000 && v.dry++ % 100 == 0) {
            LOGI("dsound: stream %08X ran dry %u times (%u packets at most of %u bytes, %u Hz, %u ch, tag %X)",
                 v.obj, v.dry, v.max_packets, v.last_size, v.rate, v.channels, v.tag);
        }
        v.fed = false;
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return std::nullopt;
    }
    v.fed = true;
    Packet& pk = v.packets.front();
    Cursor at = {pk.data, f, pk.data, f};
    if (f + 1 < FramesOf(v, pk.size)) {
        at.next_f = f + 1;
    } else if (v.packets.size() > 1) {
        at.next_data = v.packets[1].data;
        at.next_f = 0;
    }
    return at;
}

// the buffer's frame at its position, none once it stopped
std::optional<Cursor> BufferFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    uint32_t end = FramesOf(v, v.bytes);
    if (v.looping && v.loop_len) end = FramesOf(v, v.loop_start + v.loop_len);
    uint32_t start = 0;
    if (v.loop_len) start = FramesOf(v, v.loop_start);
    if (f >= end) {
        if (!v.looping || !end) {
            v.playing = false;
            v.pos = 0;
            return std::nullopt;
        }
        v.pos -= end - start;
        f = uint32_t(v.pos);
    }
    if (!v.data) return std::nullopt;
    Cursor at = {v.data, f, v.data, f};
    if (f + 1 < end) {
        at.next_f = f + 1;
    } else if (v.looping && end) {
        at.next_f = start;
    }
    return at;
}

// the voice's channels at its position, interpolated, through its I3DL2 direct path filter
void ReadFrame(Voice& v, const Cursor& at, float x[2]) {
    float t = float(v.pos - at.f);
    for (int c = 0; c < 2; c++) {
        float now = Sample(v, at.data, at.f, c);
        x[c] = now + (Sample(v, at.next_data, at.next_f, c) - now) * t;
    }
    if (v.lf == 1 && v.hf == 1) return;
    for (int c = 0; c < 2; c++) {
        v.lp[c] += kLowPass * (x[c] - v.lp[c]);
        x[c] = v.lp[c] * v.lf + (x[c] - v.lp[c]) * v.hf;
    }
}

// A mono 3D voice leaves the HRTF as two ears on the left and right speakers, the far one delayed. The centre and
// sub are not HRTF slots (0x4D45CB) and take it as it is.
void EarFrame(Voice& v, float x, float frame[kLanes]) {
    PushEar(v, x);
    float left = EarAt(v, v.itd);
    float right = EarAt(v, -v.itd);
    frame[audio::FrontLeft] = left;
    frame[audio::BackLeft] = left;
    frame[audio::FrontRight] = right;
    frame[audio::BackRight] = right;
    frame[kBinCrosstalkFrontLeft] = left;
    frame[kBinCrosstalkBackLeft] = left;
    frame[kBinCrosstalkFrontRight] = right;
    frame[kBinCrosstalkBackRight] = right;
    frame[audio::Centre] = x;
    frame[audio::Sub] = x;
}

// adds the voice to the lane frames and its reverb share to the send
void MixVoice(Voice& v, float* out, float* send, uint32_t frames) {
    Place at = Locate(v);
    float g[2][kLanes];
    float w[2];
    Levels(v, at, g, w);
    bool ears = at.on && v.channels == 1;
    float itd = Itd(at.azimuth, at.elevation, audio::IsSurround());
    if (!v.has_gain) v.itd = itd;
    float itd_step = (itd - v.itd) / float(frames);
    float dg[2][kLanes];
    UpdateRamp(v, g, frames, dg);
    double rate = double(v.freq ? v.freq : v.rate) / audio::kRate;
    for (uint32_t i = 0; i < frames; i++) {
        std::optional<Cursor> cursor = v.stream ? StreamFrame(v) : BufferFrame(v);
        if (!cursor) break;
        float x[2];
        ReadFrame(v, *cursor, x);
        send[i] += x[0] * w[0] + x[1] * w[1];  // the send leaves before the HRTF
        float ear[kLanes];
        if (ears) {
            v.itd += itd_step;
            EarFrame(v, x[0], ear);
        }
        float* o = out + i * kLanes;
        for (uint32_t k = 0; k < kLanes; k++) {
            v.gain[0][k] += dg[0][k];
            v.gain[1][k] += dg[1][k];
            if (ears) o[k] += ear[k] * v.gain[0][k];
            else o[k] += x[0] * v.gain[0][k] + x[1] * v.gain[1][k];
        }
        v.pos += rate;
    }
    std::memcpy(v.gain, g, sizeof(g));
    v.itd = itd;
}

// the speaker frames from the lane frames: the crosstalk bins through the image's crosstalk effect onto the front
// and back pairs
void AddCrosstalk(const float* lanes, float* out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        const float* m = lanes + i * kLanes;
        float* o = out + i * audio::kChannels;
        float front_left = m[kBinCrosstalkFrontLeft];
        float front_right = m[kBinCrosstalkFrontRight];
        float back_left = m[kBinCrosstalkBackLeft];
        float back_right = m[kBinCrosstalkBackRight];
        if (g_cross_feed && !g_headphones) {
            g_front_feed.Run(front_left, front_right);
            g_back_feed.Run(back_left, back_right);
        }
        std::memcpy(o, m, audio::kChannels * sizeof(float));
        o[audio::FrontLeft] += front_left;
        o[audio::FrontRight] += front_right;
        o[audio::BackLeft] += back_left;
        o[audio::BackRight] += back_right;
    }
}

// the device clips once, after the fold to stereo sums the speakers
void Render(float* out, uint32_t frames) {
    // the output thread alone renders
    static std::vector<float> lanes;
    static std::vector<float> send;
    lanes.assign(frames * kLanes, 0.0f);
    send.assign(frames, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) {
        if (v.paused || (!v.stream && !v.playing)) continue;
        MixVoice(v, lanes.data(), send.data(), frames);
    }
    if (app::Get().reverb) g_reverb.Run(send.data(), lanes.data(), frames);
    AddCrosstalk(lanes.data(), out, frames);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// DSI3DL2BUFFER: lDirect, lDirectHF, lRoom, lRoomHF (mB), flRoomRolloffFactor, Obstruction {lHFLevel, flLFRatio},
// Occlusion {lHFLevel, flLFRatio}. The direct path as I3DL2 levels it: obstruction and occlusion take lHFLevel off
// the highs and lHFLevel times flLFRatio off the lows. The game's sound occluders (closed doors, walls) come this
// way: muffled and quieter. lRoom sets the reverb send.
void I3DL2Source(Voice& v, uint32_t src) {
    if (!src) return;
    int32_t direct = int32_t(R32(src)), direct_hf = int32_t(R32(src + 4));
    int32_t obstruction = int32_t(R32(src + 20)), occlusion = int32_t(R32(src + 28));
    float obstruction_lf = F32(R32(src + 24)), occlusion_lf = F32(R32(src + 32));
    v.lf = Db(int32_t(float(direct) + float(obstruction) * obstruction_lf + float(occlusion) * occlusion_lf));
    v.hf = Db(direct + direct_hf + obstruction + occlusion);
    v.room = Db(int32_t(R32(src + 8)));
}

}  // namespace

// device

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
// DSSPEAKER as the library keeps it (0x4CDB34): the dashboard's XC_AUDIO_FLAGS
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    constexpr uint32_t kStereo = 0;
    constexpr uint32_t kMono = 1;  // DSSPEAKER_MONO
    constexpr uint32_t kSurroundAc3 = 0x2 | 0x10000;  // DSSPEAKER_SURROUND | DSSPEAKER_ENABLE_AC3
    uint32_t config = kStereo;
    if (audio::IsMono()) config = kMono;
    if (audio::IsSurround()) config = kSurroundAc3;
    W32(a[1], config);
    return 0;
}
HLE_STDCALL(IDirectSound_SetEffectData, 6) { return 0; }
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_headphones = a[1] != 0;
    return 0;
}
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] < 32) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    float p[12];
    for (int i = 0; i < 12; i++) {
        uint32_t v = R32(a[1] + i * 4);
        p[i] = i < 2 || i == 5 || i == 7 ? float(int32_t(v)) : F32(v);
    }
    LOG_FIRST(32, "dsound: listener room %.0f reflections %.0f after %.3f s, reverb %.0f after %.3f s, decay %.2f s",
              p[0], p[5], p[6], p[7], p[8], p[3]);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_reverb.Set(p);
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}

// the image is the XBE's own dsstdfx section, which the disc's dsstdfx.bin repeats byte for byte
bool UsesOwnImage(const std::vector<uint8_t>& img) {
    for (auto& sec : g_image->sections) {
        if (sec.name != "dsstdfx") continue;
        return !img.empty() && img.size() <= sec.vsize && std::memcmp(img.data(), HostPtr(sec.va), img.size()) == 0;
    }
    return false;
}

// Effects run on the console's DSP, only the crosstalk is reproduced (AddCrosstalk). The descriptor keeps the
// image's effect list so the title finds its indices, every segment points at zeroed memory.
HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = 0x818 + size_t(u32(0x804) + u32(0x80C)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name.c_str(), count);
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_cross_feed = UsesOwnImage(img);
    }
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    if (a[3]) W32(a[3], desc);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // versus makes each sound's buffer at the sound's size, then hands it the sound's own data
    if (v.owned != a[1]) {
        if (v.owned) mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);  // a size with no data
    v.bytes = v.loop_len = a[2];
    v.loop_start = 0;
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(Play, 4) {
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.bytes ? (p + BytesOf(v, kAdpcmFrames * 4)) % v.bytes : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) {
    I3DL2Source(v, a[1]);
    return 0;
}

namespace dsound {

void MuteForVoiceLink(uint32_t stream) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (Voice* v = Find(stream)) v->link_voice = true;
}

}  // namespace dsound

// streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t desc = a[0];
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj, kStreamVtable);
    W32(obj + 4, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;
    v.headroom = DefaultHeadroom(flags);
    W32(a[1], obj);
    return 0;
}

#define STREAM(name, nargs)                                      \
    static uint32_t str_##name(Voice& v, const Args& a);         \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {              \
        uint32_t r;                                              \
        std::vector<Done> flushed;                               \
        {                                                        \
            std::lock_guard<std::recursive_mutex> lock(g_lock);  \
            Voice* v = Find(a[0]);                               \
            r = v ? str_##name(*v, a) : 0x80004005u;             \
            flushed.swap(t_flushed);                             \
        }                                                        \
        Run(ctx, flushed);                                       \
        return r;                                                \
    }                                                            \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    v.block = -1;
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
STREAM(GetStatus, 2) {
    W32(a[1], v.packets.size() < v.max_packets ? 1u : 0u);
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    v.last_size = pk.size;
    if (v.tag == 0x69 && pk.size % (36u * v.channels))
        LOG_FIRST(16, "dsound: stream %08X takes an ADPCM packet of %u bytes, not whole blocks", v.obj, pk.size);
    v.active = true;
    v.ending = false;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
STREAM(FlushEx, 4) {
    if (a[3] && !v.active) return 0;
    FlushStream(v);
    return 0;
}
STREAM(Pause, 2) {
    v.paused = a[1] == 1;
    return 0;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) {
    I3DL2Source(v, a[1]);
    return 0;
}
