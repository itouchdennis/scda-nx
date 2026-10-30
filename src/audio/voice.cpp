// headset audio through SDL: the recording devices and 16-bit PCM voice streams
#include <algorithm>
#include <mutex>

#include <SDL3/SDL.h>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

bool Start() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!ok) LOGE("audio: SDL audio failed to start, no headset audio: %s", SDL_GetError());
    });
    return ok;
}

SDL_AudioStream* Sdl(VoiceStream* s) { return reinterpret_cast<SDL_AudioStream*>(s); }

VoiceStream* Open(SDL_AudioDeviceID device, uint32_t rate, uint32_t channels) {
    SDL_AudioSpec spec{SDL_AUDIO_S16, int(channels), int(rate)};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(device, &spec, nullptr, nullptr);
    if (!s) {
        LOGE("audio: voice stream failed to open: %s", SDL_GetError());
        return nullptr;
    }
    SDL_ResumeAudioStreamDevice(s);
    return reinterpret_cast<VoiceStream*>(s);
}

}  // namespace

std::vector<std::string> Microphones() {
    std::vector<std::string> out;
    if (!Start()) return out;
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
        for (int i = 0; i < count; i++)
            if (const char* name = SDL_GetAudioDeviceName(ids[i])) out.emplace_back(name);
        SDL_free(ids);
    }
    return out;
}

VoiceStream* OpenMicrophone(const std::string& name, uint32_t rate, uint32_t channels) {
    if (!Start()) return nullptr;
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
        for (int i = 0; i < count; i++)
            if (const char* n = SDL_GetAudioDeviceName(ids[i]); n && !name.empty() && name == n) device = ids[i];
        SDL_free(ids);
    }
    LOGI("audio: microphone %s", device == SDL_AUDIO_DEVICE_DEFAULT_RECORDING ? "system default" : name.c_str());
    return Open(device, rate, channels);
}

VoiceStream* OpenSpeaker(uint32_t rate, uint32_t channels) {
    return Start() ? Open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, rate, channels) : nullptr;
}

uint32_t Available(VoiceStream* s) { return uint32_t(std::max(SDL_GetAudioStreamAvailable(Sdl(s)), 0)); }

uint32_t Read(VoiceStream* s, void* out, uint32_t bytes) {
    return uint32_t(std::max(SDL_GetAudioStreamData(Sdl(s), out, int(bytes)), 0));
}

uint32_t Queued(VoiceStream* s) { return uint32_t(std::max(SDL_GetAudioStreamQueued(Sdl(s)), 0)); }

void Write(VoiceStream* s, const void* data, uint32_t bytes) { SDL_PutAudioStreamData(Sdl(s), data, int(bytes)); }

void Close(VoiceStream* s) { SDL_DestroyAudioStream(Sdl(s)); }

}  // namespace audio
