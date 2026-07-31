#ifndef SM64EX_HOST_H
#define SM64EX_HOST_H

// ---------------------------------------------------------------------------
// Embedding interface for sm64ex.
//
// The upstream PC port picks its window / rendering / audio backend at compile
// time in pc_main.c and owns the process (main() plus an infinite
// wm_api->main_loop). When sm64ex is built as a library (SM64_BACKEND=host)
// that file is replaced by sm64ex_host.c, which keeps every global and helper
// the game expects but lets the embedder supply the backends and drive the
// frame loop instead.
//
// Typical use:
//
//     struct SM64ExHostConfig cfg = {0};
//     cfg.wm_api        = &my_window_manager;   // required
//     cfg.rendering_api = &my_renderer;         // required
//     cfg.audio_api     = &my_audio;            // optional
//     sm64ex_host_init(&cfg);
//     while (running) sm64ex_host_frame();      // 30 Hz game tick
//     sm64ex_host_deinit();
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;
struct AudioAPI;

// Snapshot of player-1 pad state after the latest sm64ex_host_frame().
// buttonDown / buttonPressed use the N64 CONT_* masks (see SM64EX_BTN_*).
struct SM64ExHostInput {
    uint16_t buttonDown;
    uint16_t buttonPressed;
    int16_t stickX;      // raw stick, roughly [-128, 127]
    int16_t stickY;
    int16_t extStickX;   // C-stick / right stick when present
    int16_t extStickY;
};

// Same bit layout as include/PR/os_cont.h CONT_* / A_BUTTON etc.
enum {
    SM64EX_BTN_A      = 0x8000,
    SM64EX_BTN_B      = 0x4000,
    SM64EX_BTN_Z      = 0x2000,
    SM64EX_BTN_START  = 0x1000,
    SM64EX_BTN_D_UP   = 0x0800,
    SM64EX_BTN_D_DOWN = 0x0400,
    SM64EX_BTN_D_LEFT = 0x0200,
    SM64EX_BTN_D_RIGHT= 0x0100,
    SM64EX_BTN_L      = 0x0020,
    SM64EX_BTN_R      = 0x0010,
    SM64EX_BTN_C_UP   = 0x0008,
    SM64EX_BTN_C_DOWN = 0x0004,
    SM64EX_BTN_C_LEFT = 0x0002,
    SM64EX_BTN_C_RIGHT= 0x0001,
};

struct SM64ExHostConfig {
    // Required. Same vtables the built-in backends implement; see
    // src/pc/gfx/gfx_rendering_api.h and gfx_window_manager_api.h.
    struct GfxWindowManagerAPI *wm_api;
    struct GfxRenderingAPI *rendering_api;

    // Optional. NULL selects the built-in null audio backend.
    struct AudioAPI *audio_api;

    // Optional. NULL/0 selects the upstream default.
    const char *window_title;
    const char *game_dir;   // defaults to FS_BASEDIR
    const char *user_path;  // defaults to sys_user_path()
    size_t pool_size;       // defaults to DEFAULT_POOL_SIZE
};

// Runs everything main_func() did up to (but not including) the main loop:
// filesystem, config, main pool, gfx_init, audio and the initial
// thread5_game_loop pass. Returns false if the configuration is incomplete.
bool sm64ex_host_init(const struct SM64ExHostConfig *config);

// One 30 Hz game tick: produces a display list (delivered through
// GfxRenderingAPI) and one audio buffer. Equivalent to the callback the
// upstream backends hand to wm_api->main_loop().
void sm64ex_host_frame(void);

// Same as the game's own shutdown path (config save, controllers, audio, gfx).
void sm64ex_host_deinit(void);

bool sm64ex_host_is_inited(void);

// Fills *out with gPlayer1Controller's current state (keyboard + gamepad merged).
// Safe to call when not inited: writes zeros.
void sm64ex_host_get_input(struct SM64ExHostInput *out);

#ifdef __cplusplus
}
#endif

#endif // SM64EX_HOST_H
