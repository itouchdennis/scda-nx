// What the port leaves out for now: the headset (voice.cpp and the Opus voice link, Windows SDL3 audio capture) and
// gyro aiming (gyro.cpp reads SDL3's sensors). Each answers as the Windows build does with no device: no
// microphones, no streams, no turn.
#include "audio/audio.h"
#include "core/gyro.h"

namespace audio {

std::vector<std::string> Microphones() { return {}; }
VoiceStream* OpenMicrophone(const std::string&, uint32_t, uint32_t) { return nullptr; }
VoiceStream* OpenSpeaker(uint32_t, uint32_t) { return nullptr; }
uint32_t Available(VoiceStream*) { return 0; }
uint32_t Read(VoiceStream*, void*, uint32_t) { return 0; }
uint32_t Queued(VoiceStream*) { return 0; }
void Write(VoiceStream*, const void*, uint32_t) {}
void Close(VoiceStream*) {}

void StartVoiceLink(const std::string&) {}
void StopVoiceLink() {}
void VoiceSent(uint32_t) {}
bool TakeVoiceLinkPacket(uint32_t, const uint8_t*, int) { return false; }
bool VoiceLinkHeard() { return false; }
void SetMicOpen(bool) {}

}  // namespace audio

namespace gyro {

Turn Take(const Settings&) { return {}; }
void StartCalibration() {}
bool Calibrating() { return false; }

}  // namespace gyro
