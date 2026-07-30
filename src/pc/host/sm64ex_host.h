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

#ifdef __cplusplus
extern "C" {
#endif

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;
struct AudioAPI;

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

#ifdef __cplusplus
}
#endif

#endif // SM64EX_HOST_H
