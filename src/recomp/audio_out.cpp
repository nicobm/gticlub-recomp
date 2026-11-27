// Sound output of the recompiled game: 48 kHz stereo float through an SDL audio stream.
#include <SDL3/SDL.h>
#include <cstdio>

namespace gti::recomp {
static SDL_AudioStream* g_stream = nullptr;
bool audio_out_open() {
    if (g_stream) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { std::fprintf(stderr, "[snd] SDL audio: %s\n", SDL_GetError()); return false; }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = 48000;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_stream) { std::fprintf(stderr, "[snd] no audio device: %s\n", SDL_GetError()); return false; }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}
// stereo frames waiting to be played
int audio_out_queued() { return g_stream ? SDL_GetAudioStreamQueued(g_stream) / 8 : 0; }
void audio_out_push(const float* stereo, int frames) { if (g_stream) SDL_PutAudioStreamData(g_stream, stereo, frames * 8); }
// drops what is queued (the sound was switched off in the menu)
void audio_out_clear() { if (g_stream) SDL_ClearAudioStream(g_stream); }
} // namespace gti::recomp
