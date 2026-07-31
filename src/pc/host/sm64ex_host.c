// ---------------------------------------------------------------------------
// Library entry point for sm64ex (SM64_BACKEND=host).
//
// This is pc_main.c with three changes:
//   * main() and the infinite wm_api->main_loop() are gone -- the embedder
//     drives the frame loop;
//   * the compile-time #if chain that selects a window/rendering/audio backend
//     is replaced by pointers the embedder passes in;
//   * the CLI option overrides become fields of SM64ExHostConfig.
//
// Everything else -- the globals the game links against, produce_one_frame(),
// send_display_list(), game_deinit()/game_exit() -- is kept verbatim so the
// rest of the code base is untouched.
// ---------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

// Relative to src/pc/, where pc_main.c used to live.
#include "../gfx/gfx_pc.h"
#include "../gfx/gfx_window_manager_api.h"

#include "../audio/audio_api.h"
#include "../audio/audio_null.h"

#include "../pc_main.h"
#include "../configfile.h"
#include "../controller/controller_api.h"
#include "../controller/controller_keyboard.h"
#include "../fs/fs.h"
#include "../platform.h"

#include "game/game_init.h"
#include "game/main.h"
#include "game/thread6.h"

#include "sm64ex_host.h"

// --- globals the rest of the game expects from pc_main.c -------------------

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

s32 gRumblePakPfs;
struct RumbleData gRumbleDataQueue[3];
struct StructSH8031D9B0 gCurrRumbleSettings;

static struct AudioAPI *audio_api;
static struct GfxWindowManagerAPI *wm_api;
static struct GfxRenderingAPI *rendering_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler,
                        UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {
}

static bool inited = false;

#include "game/display.h" // for gGlobalTimer
void send_display_list(struct SPTask *spTask) {
    if (!inited) return;
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 656
#define SAMPLES_LOW 640
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

void produce_one_frame(void) {
    gfx_start_frame();

    const f32 master_mod = (f32)configMasterVolume / 127.0f;
    set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * master_mod);
    set_sequence_player_volume(SEQ_PLAYER_SFX, (f32)configSfxVolume / 127.0f * master_mod);
    set_sequence_player_volume(SEQ_PLAYER_ENV, (f32)configEnvVolume / 127.0f * master_mod);

    game_loop_one_iteration();
    thread6_rumble_loop(NULL);

    int samples_left = audio_api->buffered();
    u32 num_audio_samples = samples_left < audio_api->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
    s16 audio_buffer[SAMPLES_HIGH * 2 * 2];
    for (int i = 0; i < 2; i++) {
        create_next_audio_buffer(audio_buffer + i * (num_audio_samples * 2), num_audio_samples);
    }

    audio_api->play((u8 *)audio_buffer, 2 * num_audio_samples * 4);

    gfx_end_frame();
}

void audio_shutdown(void) {
    if (audio_api) {
        if (audio_api->shutdown) audio_api->shutdown();
        audio_api = NULL;
    }
}

void game_deinit(void) {
    if (!inited) return;
    configfile_save(configfile_name());
    controller_shutdown();
    audio_shutdown();
    gfx_shutdown();
    inited = false;
}

void game_exit(void) {
    game_deinit();
    exit(0);
}

// --- embedding interface ---------------------------------------------------

bool sm64ex_host_init(const struct SM64ExHostConfig *config) {
    if (inited) return true;

    if (config == NULL || config->wm_api == NULL || config->rendering_api == NULL) {
        fprintf(stderr, "sm64ex_host_init: wm_api and rendering_api are required\n");
        return false;
    }

    wm_api = config->wm_api;
    rendering_api = config->rendering_api;

    const char *gamedir = config->game_dir ? config->game_dir : FS_BASEDIR;
    const char *userpath = config->user_path ? config->user_path : sys_user_path();
    fs_init(sys_ropaths, gamedir, userpath);

    configfile_load(configfile_name());

    const size_t poolsize = config->pool_size ? config->pool_size : DEFAULT_POOL_SIZE;
    u64 *pool = malloc(poolsize);
    if (!pool) {
        sys_fatal("Could not alloc %u bytes for main pool.\n", (unsigned)poolsize);
        return false;
    }
    main_pool_init(pool, pool + poolsize / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    const char *title = config->window_title ? config->window_title
                                             : "Super Mario 64 EX";
    gfx_init(wm_api, rendering_api, title);
    if (wm_api->set_keyboard_callbacks) {
        wm_api->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up,
                                       keyboard_on_all_keys_up);
    }

    audio_api = config->audio_api;
    if (audio_api && audio_api->init && !audio_api->init())
        audio_api = NULL;
    if (audio_api == NULL)
        audio_api = &audio_null;

    audio_init();
    sound_init();

    thread5_game_loop(NULL);

    inited = true;
    return true;
}

void sm64ex_host_frame(void) {
    if (!inited) return;
    produce_one_frame();
}

void sm64ex_host_deinit(void) {
    game_deinit();
}

bool sm64ex_host_is_inited(void) {
    return inited;
}

void sm64ex_host_get_input(struct SM64ExHostInput *out) {
    if (!out)
        return;
    if (!inited || !gPlayer1Controller) {
        out->buttonDown = 0;
        out->buttonPressed = 0;
        out->stickX = 0;
        out->stickY = 0;
        out->extStickX = 0;
        out->extStickY = 0;
        return;
    }
    out->buttonDown = gPlayer1Controller->buttonDown;
    out->buttonPressed = gPlayer1Controller->buttonPressed;
    out->stickX = gPlayer1Controller->rawStickX;
    out->stickY = gPlayer1Controller->rawStickY;
    out->extStickX = gPlayer1Controller->extStickX;
    out->extStickY = gPlayer1Controller->extStickY;
}
