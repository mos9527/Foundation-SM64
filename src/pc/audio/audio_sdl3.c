// SDL3 audio backend for the Foundation host, ported from audio_sdl2.c but
// using the SDL3 audio-stream API (the same approach SM64.cpp takes).
//
// Unlike the SDL2 build this is compiled for the Foundation host, which has
// already initialised SDL3 (video + gamepad) on its own, so this backend only
// opens an SDL3 audio stream and feeds it the game's 32 kHz stereo S16 buffers.
#ifdef AAPI_SDL3

#include <stdio.h>

#include <SDL3/SDL.h>

#include "audio_api.h"

static SDL_AudioStream *stream;

static bool audio_sdl_init(void) {
    // The host already brings up SDL with VIDEO | GAMEPAD | AUDIO (mirroring
    // SM64.cpp's single SDL_Init call). We only need to make sure the audio
    // subsystem is present; this is additive and safe to call repeatedly.
    // Unlike SDL2/SDL1 backends we do NOT treat a non-zero return here as fatal:
    // SDL3's SDL_OpenAudioDeviceStream auto-initialises the audio subsystem, and
    // a lone SDL_Init(AUDIO) failure was silently dropping all sound. The real
    // gate is whether the stream below actually opens.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[sdl3-audio] SDL_InitSubSystem(AUDIO) failed (non-fatal): %s\n",
                SDL_GetError());
    }

    // 32000 Hz, native-endian signed 16-bit, stereo -- matches what the game's
    // synthesizer produces (see produce_one_frame() in sm64ex_host.c).
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 32000;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (stream == NULL) {
        fprintf(stderr, "[sdl3-audio] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        fprintf(stderr, "[sdl3-audio] SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
    }
    fprintf(stderr, "[sdl3-audio] stream opened OK (freq=%d ch=%d)\n", spec.freq, spec.channels);
    return true;
}

static int audio_sdl_buffered(void) {
    if (stream == NULL)
        return 0;
    return (int)(SDL_GetAudioStreamQueued(stream) / 4);
}

static int audio_sdl_get_desired_buffered(void) {
    return 1100;
}

static void audio_sdl_play(const uint8_t *buf, size_t len) {
    if (stream == NULL)
        return;
    if (audio_sdl_buffered() >= 6000) {
        return; // consumer is keeping up; don't flood the queue
    }
    if (!SDL_PutAudioStreamData(stream, buf, (int)len)) {
        fprintf(stderr, "[sdl3-audio] SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        return;
    }
    // Diagnostic: confirm the game is actually producing non-silent audio.
    static int s_log = 0;
    if (s_log < 8) {
        const int16_t *p = (const int16_t *)buf;
        int n = (int)(len / 2);
        int peak = 0;
        for (int i = 0; i < n; ++i) {
            int v = p[i] < 0 ? -p[i] : p[i];
            if (v > peak) peak = v;
        }
        fprintf(stderr, "[sdl3-audio] play #%d len=%zu peak=%d queued=%d\n",
                s_log, len, peak, audio_sdl_buffered());
        ++s_log;
    }
}

static void audio_sdl_shutdown(void) {
    if (stream != NULL) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

struct AudioAPI audio_sdl3 = {
    audio_sdl_init,
    audio_sdl_buffered,
    audio_sdl_get_desired_buffered,
    audio_sdl_play,
    audio_sdl_shutdown
};

#endif // AAPI_SDL3
