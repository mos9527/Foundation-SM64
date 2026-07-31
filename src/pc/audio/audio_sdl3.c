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
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL audio init error: %s\n", SDL_GetError());
        return false;
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
        fprintf(stderr, "SDL_OpenAudioDeviceStream error: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
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
    if (audio_sdl_buffered() < 6000) {
        // Don't flood the audio buffer if the consumer is falling behind.
        SDL_PutAudioStreamData(stream, buf, (int)len);
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
