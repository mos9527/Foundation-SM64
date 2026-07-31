#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Render-pass tags forwarded from the game (via gDPNoOpTag in the display list)
// to GfxRenderingAPI.set_render_layer. Lets a backend partition draws into e.g.
// a skybox / game-geometry / UI split without relying on projection heuristics.
#define FOUNDATION_PASS_SKYBOX 1u
#define FOUNDATION_PASS_GAME   2u
#define FOUNDATION_PASS_UI     3u

struct ShaderProgram;

struct GfxRenderingAPI {
    bool (*z_is_from_0_to_1)(void);
    void (*unload_shader)(struct ShaderProgram *old_prg);
    void (*load_shader)(struct ShaderProgram *new_prg);
    struct ShaderProgram *(*create_and_load_new_shader)(uint32_t shader_id);
    struct ShaderProgram *(*lookup_shader)(uint32_t shader_id);
    void (*shader_get_info)(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]);
    uint32_t (*new_texture)(void);
    void (*select_texture)(int tile, uint32_t texture_id);
    void (*upload_texture)(const uint8_t *rgba32_buf, int width, int height);
    void (*set_sampler_parameters)(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt);
    void (*set_depth_test)(bool depth_test);
    void (*set_depth_mask)(bool z_upd);
    void (*set_zmode_decal)(bool zmode_decal);
    void (*set_viewport)(int x, int y, int width, int height);
    void (*set_scissor)(int x, int y, int width, int height);
    void (*set_use_alpha)(bool use_alpha);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    void (*init)(void);
    void (*on_resize)(void);
    void (*start_frame)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
    void (*shutdown)(void);

    // Optional; takes precedence over draw_triangles when set. Backends that
    // reconstruct 3D geometry (rather than drawing gfx_pc's clip-space output)
    // additionally get modelview-space positions -- 9 floats per triangle, or
    // NULL for screen-space batches -- plus the projection gfx_pc baked into
    // buf_vbo.
    void (*draw_triangles_3d)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris,
                              const float *buf_pos3d, const float projection[4][4]);

    // Optional. Forwarded render-pass marker from the game (gDPNoOpTag). Backends
    // that partition draws (e.g. skybox / game / UI) set this; others leave it NULL
    // and the markers are ignored. gfx_pc flushes the current batch before calling it.
    void (*set_render_layer)(uint32_t tag);
};

#endif
