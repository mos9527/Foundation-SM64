// Foundation-renderer host for sm64ex (scaffold).
//
// The Foundation/Vulkan/GPUScene setup below mirrors Examples/Renderer/SM64/
// SM64.cpp (the libsm64 demo) so it is known-good against the current
// Foundation API; the only structural change is that we *statically link* the
// sm64ex host library (src/pc/host) and feed it our own GfxRenderingAPI /
// GfxWindowManagerAPI backends, instead of dlopen-ing libsm64.
//
// gfx_pc hands backends clip-space vertices, which carry no usable 3D geometry.
// We instead take draw_triangles_3d (see gfx_rendering_api.h) to get
// modelview-space positions, so what lands in GPUScene is the frame's geometry in
// the game's *view space*: the camera therefore sits at the origin looking down
// -Z, with the projection derived from the game's own matrix.
//
// Implemented here:
//   - Full Foundation init (SDL3 + Vulkan + GPUScene + Rasterizer)   [from Example]
//   - GfxWindowManagerAPI over Foundation's bundled SDL3               [complete]
//   - Per-frame capture of perspective draws into dynamic GPUScene geometry
//   - N64 textures -> GPUScene textures (GSMaterial::baseColorTexture), and
//     per-material geometry buckets so one GSInstance == one material
//   - Separate capture of screen-space / ortho draws into a UI batch list
//   - FOV from sFOVState (via sm64ex_host_get_fov_degrees)
//   - Keyboard input via controller_keyboard (SDL3 scancodes -> DOS scancodes,
//     same mapping as gfx_sdl2.c), fed through the WM keyboard callbacks
//
// Material mapping notes:
//   A GSInstance carries exactly one GSMaterial, so the captured frame cannot be
//   a single geometry any more: draws are bucketed by (texture, shade color) and
//   each bucket owns its own dynamic geometry + instance. Buckets are recycled
//   every frame; a bucket that overflows chains into a fresh one.

#include <SDL3/SDL.h>

#include "sm64ex_host.h"          // sm64ex host embedding interface (PUBLIC include)
#include "configfile.h"           // configSkipIntro, configKey*, MAX_BINDS
#include "gfx/gfx_screen_config.h" // DESIRED_SCREEN_WIDTH/HEIGHT for options-menu reset
#include "audio/audio_sdl3.h"     // audio_api (SDL3 backend)
#include "controller/controller_sdl3_keyboard.h"  // keyboard input backend + vk names
#include "gfx/gfx_cc.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"

#include <Core/Allocator.hpp>     // GLOBAL_ALLOC
#include <Renderer/Renderer.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Mesh.hpp>
#include <Renderer/Rasterizer.hpp>
#include <Renderer/Texture.hpp>
#include <Math/Math.hpp>
#include <Math/ModelViewProjection.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <Examples.hpp>
#include <Renderer/Examples.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Foundation globals
// ---------------------------------------------------------------------------
static SDL_Window*       g_window  = nullptr;
static bool              g_running = true;

static ExampleInputState g_input;
static ExampleFpsCounter g_fps;
static RendererUBO       g_ubo;
static RendererConfig    g_cfg;
static RendererOutputs   g_outputs;
static ExampleRenderer   g_renderer = ExampleRenderer::RealtimePT;

// The captured game geometry is already in the game's view space, so the camera
// sits at the origin. FOV comes from sFOVState each frame; aspect from our
// swapchain (widescreen without gfx_pc's clip-space fixup).
struct FViewSpaceCamera
{
    float fovY = radians(60.0f);
    float zNear = 1.0f;
    float aspect = 1.0f;
    mat4 view{};
    mat4 proj{};
    static constexpr float3 kPosition{0.0f, 0.0f, 0.0f};
    static constexpr float3 kForward{0.0f, 0.0f, -1.0f};

    void RefreshMatrices()
    {
        view = viewMatrixRHReverseZ(kPosition, quat(0.0f, 0.0f, 0.0f, 1.0f));
        proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
    }
};
static FViewSpaceCamera g_camera;
static float g_gameFovY = radians(60.0f);

// gfx_pc leaves `struct ShaderProgram` to the backend. We compile no GLSL, but
// gfx_sp_tri1 drives texture setup and the VBO layout off shader_get_info, so
// the decoded combiner features have to be real.
struct ShaderProgram {
    uint32_t shader_id;
    CCFeatures cc;
    uint8_t num_floats;
};

// ---------------------------------------------------------------------------
// Captured game / UI state
// ---------------------------------------------------------------------------
// Game draws: perspective + modelview -> view-space soup for GPUScene.
// UI draws: screen-space / ortho (HUD, fill rects, text) kept as clip-space
// triangles for a dedicated overlay pass (not committed yet).
static constexpr uint32_t kMaxTris = 32768u;
static constexpr uint32_t kMaxUiTris = 4096u;

// One GSInstance references exactly one GSMaterial, so the frame is split into
// material buckets: each bucket owns a dynamic geometry of kBucketVerts and is
// committed as its own instance. Buckets are handed out in order every frame
// and recycled, so bucket N is a different material from frame to frame.
static constexpr uint32_t kMaxBuckets = 1024;
static constexpr uint32_t kBucketVerts = 3072u;
static constexpr uint32_t kInvalidBucket = ~0u;
static constexpr uint32_t kNoTexture = ~0u;

struct Bucket {
    GeometryHandle geo{};
    uint32_t texIndex = kNoTexture;   // GPUScene bindless texture index
    float4 color = float4(1.0f);      // baked shade / prim color
    uint32_t vertexCount = 0;         // filled this frame
    uint32_t dirtyCount = 0;          // vertices left non-degenerate on the GPU
    bool indicesUploaded = false;
};
static Bucket g_buckets[kMaxBuckets];
static std::vector<FQVertex> g_bucket_verts;      // kMaxBuckets * kBucketVerts
static std::vector<uint32_t> g_indices;           // shared identity index buffer
static std::unordered_map<uint64_t, uint32_t> g_bucket_by_key;  // material -> bucket
static std::unordered_map<uint32_t, uint32_t> g_bucket_by_tex;  // texture  -> bucket
static uint32_t g_live_buckets = 0;
static uint32_t g_captured_tris = 0;
static bool g_dropped_tris = false;

// Packed to 44 bytes (11 floats) with no trailing padding so the GPU vertex
// stride matches exactly. texId is the GPUScene bindless texture index; < 0
// means the quad is untextured and the fragment shader outputs the flat color.
struct UiVertex {
    float px, py, pz, pw;   // clip-space xyzw
    float u, v;             // texcoord
    float r, g, b, a;       // color
    float texId;
};
static_assert(sizeof(UiVertex) == 44, "UI vertex stride must be 44 bytes");
struct UiBatch {
    uint32_t firstVertex;
    uint32_t vertexCount;
    uint32_t textureId;   // 0 = untextured
    uint32_t shaderId;
    bool useAlpha;
};
static std::vector<UiVertex> g_ui_verts;
static std::vector<UiBatch> g_ui_batches;
static uint32_t g_ui_tris = 0;
static bool g_dropped_ui = false;
static bool g_use_alpha = false;

// [Foundation] Skybox capture. The skybox is ortho/screen-space, like the UI
// overlay, but kept in its own buffer so it can be drawn as a dedicated pass
// behind the game geometry.
static std::vector<UiVertex> g_skybox_verts;
static std::vector<UiBatch> g_skybox_batches;
static uint32_t g_skybox_tris = 0;
static bool g_dropped_skybox = false;

// Overlay vertex buffers (recreated on each RebuildGraph; the renderer owns the
// underlying memory). Both are Upload-heap buffers bound as vertex buffers and
// refilled per-frame from the captured screen-space soups.
static constexpr size_t kUiVboCapacity = 1u << 17;  // 131072 verts
static constexpr size_t kUiVboCapacityBytes = kUiVboCapacity * sizeof(UiVertex);
static ResourceHandle g_ui_vbo{};
static ResourceHandle g_skybox_vbo{};
// One global sampler for the overlay passes; N64 tile clamping is already baked
// into the UVs at capture time, so a plain repeat/linear sampler is enough.
static ResourceHandle g_overlay_sampler{};

// gfx_pc hands out its own texture ids and re-uploads onto a recycled id when
// its cache wraps, so the GPUScene texture is keyed by content hash and the id
// map is just a redirection into that pool. Uploaded textures are pinned and
// never released: unique N64 texture content is bounded and tiny.
static std::unordered_map<uint32_t, TextureHandle> g_tex_by_id;
static std::unordered_map<uint64_t, TextureHandle> g_tex_by_hash;
static bool g_dropped_textures = false;

static std::unordered_map<uint32_t, ShaderProgram> g_shaders;         // id -> decoded combiner
static const ShaderProgram* g_cur_shader = nullptr;
static uint32_t g_cur_tex[2] = {0, 0};
static int g_cur_tile = 0;

// N64 tile wrap modes (gbi.h): clamping is emulated by clamping the UVs, since
// Foundation samples every bindless texture through one global repeat sampler.
static constexpr uint32_t kTxClamp = 0x2;
static bool g_clamp_s[2] = {false, false};
static bool g_clamp_t[2] = {false, false};

static GPUScene* g_gpu = nullptr;

static const FQVertex kDegenerateVertex = [] {
    FVertex v{};
    v.normal = float3(0.0f, 0.0f, 1.0f);
    return FQVertex::Pack(v);
}();

// ===========================================================================
// GfxWindowManagerAPI  (Foundation's bundled SDL3)
// ===========================================================================
// Keyboard input is handled by controller_sdl3_keyboard.c, which translates
// SDL3 scancodes to the DOS scancodes controller_keyboard expects and feeds
// them in. The scancode inversion table and per-event dispatch live there;
// see keyboard_sdl3_event() / keyboard_sdl3_set_callbacks() in that file.

// SDL event hook for Examples_PollEvents: forward each event to the SDL3
// gamepad backend (connect/disconnect) and the SDL3 keyboard backend (which
// translates the scancode and feeds controller_keyboard). Called once per SDL
// event on the host thread.
extern "C" void controller_sdl3_event(SDL_Event* event);
static void OnSdlEvent(SDL_Event* event) {
    controller_sdl3_event(event);   // gamepad add/remove; ignores other events
    keyboard_sdl3_event(event);     // keyboard: translate + dispatch to controller_keyboard
}

static void wm_init(const char* title) {
    // Init AUDIO together with VIDEO/GAMEPAD (one call), matching SM64.cpp's
    // SDL_Init(VIDEO | GAMEPAD | AUDIO). Initialising the audio subsystem
    // separately later can fail on some setups, which silently disabled sound.
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
    keyboard_sdl3_init();
    // The window is created by main() (before Foundation/Vulkan init). The host
    // calls init() again during sm64ex_host_init, after the window exists, so
    // only create it here if it is not already up.
    (void)title; // FOUNDATION_APPLICATION_TITLE() requires a string literal, not a var
    if (!g_window)
        g_window = SDL_CreateWindow(title, 1280, 720,
                                    Examples_SDLWindowFlagsVulkan);
}
static void wm_set_keyboard_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up,
                                      void (*on_all_keys_up)(void)) {
    // sm64ex_host_init() wires controller_keyboard to these during boot; hand them
    // to the SDL3 keyboard backend so OnSdlEvent() can forward translated presses.
    keyboard_sdl3_set_callbacks(on_key_down, on_key_up, on_all_keys_up);
}
static void wm_main_loop(void (*)(void)) {
    // Unused: sm64ex_host drives its own frame loop (see main()).
}
static void wm_get_dimensions(uint32_t* w, uint32_t* h) {
    int iw = 0, ih = 0;
    SDL_GetWindowSize(g_window, &iw, &ih);
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
}
static void wm_handle_events(void) {
    // SDL events are pumped by Examples_PollEvents() in the frame loop; this
    // hook is intentionally a no-op to avoid double-consuming the queue.
    // TODO: route keyboard/gamepad -> controller_api here.
}
static bool wm_start_frame(void) {
    // Options menu (EXT_OPTIONS_MENU) flips these when the user hits Apply /
    // Reset Window under Display. Honour them here since we own the SDL window.
    if (configWindow.settings_changed) {
        configWindow.settings_changed = false;
        if (configWindow.reset) {
            configWindow.reset = false;
            configWindow.fullscreen = false;
            configWindow.x = WAPI_WIN_CENTERPOS;
            configWindow.y = WAPI_WIN_CENTERPOS;
            configWindow.w = DESIRED_SCREEN_WIDTH;
            configWindow.h = DESIRED_SCREEN_HEIGHT;
            SDL_SetWindowFullscreen(g_window, 0);
            SDL_SetWindowSize(g_window, (int)configWindow.w, (int)configWindow.h);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        } else {
            const bool isFs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (configWindow.fullscreen != isFs)
                SDL_SetWindowFullscreen(g_window,
                                        configWindow.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
            if (!configWindow.fullscreen) {
                SDL_SetWindowSize(g_window, (int)configWindow.w, (int)configWindow.h);
                const int xpos = (configWindow.x == WAPI_WIN_CENTERPOS)
                                     ? SDL_WINDOWPOS_CENTERED : (int)configWindow.x;
                const int ypos = (configWindow.y == WAPI_WIN_CENTERPOS)
                                     ? SDL_WINDOWPOS_CENTERED : (int)configWindow.y;
                SDL_SetWindowPosition(g_window, xpos, ypos);
            }
        }
    }
    return true;
}
static void wm_swap_buffers_begin(void) {}
static void wm_swap_buffers_end(void) {}      // Foundation swaps internally.
static double wm_get_time(void) {
    return (double)SDL_GetTicksNS() / 1e9;
}
static void wm_shutdown(void) {
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}
static void wm_on_resize(void) {
    // Triggered on viewport change; the frame loop checks
    // input.wantResizeOrRebuild and rebuilds the renderer graph.
}

static struct GfxWindowManagerAPI gfx_window_manager_api = {
    wm_init,
    wm_set_keyboard_callbacks,
    wm_main_loop,
    wm_get_dimensions,
    wm_handle_events,
    wm_start_frame,
    wm_swap_buffers_begin,
    wm_swap_buffers_end,
    wm_get_time,
    wm_shutdown,
};

// ===========================================================================
// GfxRenderingAPI  (captures the triangle stream -> GPUScene; material map TODO)
// ===========================================================================
static bool ra_z_is_0to1(void) { return true; }
static void ra_unload_shader(struct ShaderProgram* old_prg) {
    if (g_cur_shader == old_prg)
        g_cur_shader = nullptr;
}
static void ra_load_shader(struct ShaderProgram* new_prg) { g_cur_shader = new_prg; }
static struct ShaderProgram* ra_create_and_load_new_shader(uint32_t id) {
    auto [it, inserted] = g_shaders.try_emplace(id);
    ShaderProgram& prg = it->second;
    if (inserted) {
        prg.shader_id = id;
        gfx_cc_get_features(id, &prg.cc);
        // Mirrors what gfx_sp_tri1 writes per vertex.
        prg.num_floats = 4;
        if (prg.cc.used_textures[0] || prg.cc.used_textures[1])
            prg.num_floats += 2;
        if (prg.cc.opt_fog)
            prg.num_floats += 4;
        prg.num_floats += static_cast<uint8_t>(prg.cc.num_inputs * (prg.cc.opt_alpha ? 4 : 3));
    }
    g_cur_shader = &prg;
    return &prg;
}
static struct ShaderProgram* ra_lookup_shader(uint32_t id) {
    auto it = g_shaders.find(id);
    return it == g_shaders.end() ? nullptr : &it->second;
}
static void ra_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    // Must be exact: gfx_pc dereferences rendering_state.textures[i] for every
    // tile reported here, and indexes shader_input_mapping by num_inputs.
    *num_inputs = static_cast<uint8_t>(prg->cc.num_inputs);
    used_textures[0] = prg->cc.used_textures[0];
    used_textures[1] = prg->cc.used_textures[1];
}
static uint32_t ra_new_texture(void) {
    static uint32_t next = 1;
    return next++;
}
static void ra_select_texture(int tile, uint32_t id) {
    if (tile >= 0 && tile < 2) {
        g_cur_tex[tile] = id;
        g_cur_tile = tile;
    }
}
static uint64_t HashTexture(const uint8_t* data, size_t size, int w, int h) {
    uint64_t hash = 1469598103934665603ull;   // FNV-1a
    auto mix = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    for (size_t i = 0; i < size; ++i)
        mix(data[i]);
    mix((uint8_t)w);
    mix((uint8_t)h);
    return hash;
}

static void ra_upload_texture(const uint8_t* rgba32, int w, int h) {
    // gfx_pc uploads into whichever tile it selected last. This runs on the host
    // thread between frames (inside sm64ex_host_frame), so a blocking GPUScene
    // upload is safe here.
    const uint32_t id = g_cur_tex[g_cur_tile];
    if (!g_gpu || !rgba32 || w <= 0 || h <= 0)
        return;

    const size_t size = (size_t)w * (size_t)h * 4u;
    const uint64_t hash = HashTexture(rgba32, size, w, h);
    if (auto it = g_tex_by_hash.find(hash); it != g_tex_by_hash.end()) {
        g_tex_by_id[id] = it->second;   // same pixels as an already resident texture
        return;
    }

    FTexture texture(GLOBAL_ALLOC);
    texture.Initialize(RHIResourceFormat::R8G8B8A8Srgb, RHITextureDimension::E2D,
                       (uint32_t)w, (uint32_t)h);
    texture.bytes.assign(rgba32, rgba32 + size);

    TextureHandle handle{};
    if (g_gpu->Upload(texture, handle, "sm64 texture", /*pinned=*/true) != GPUScene::Result::Ready ||
        !handle.IsValid()) {
        g_dropped_textures = true;
        g_tex_by_id[id] = TextureHandle{};
        return;
    }
    g_tex_by_hash.emplace(hash, handle);
    g_tex_by_id[id] = handle;
}
static void ra_set_sampler_parameters(int tile, bool /*linearFilter*/, uint32_t cms, uint32_t cmt) {
    // TODO: point filtering and mirrored wrap need per-material samplers.
    if (tile >= 0 && tile < 2) {
        g_clamp_s[tile] = (cms & kTxClamp) != 0;
        g_clamp_t[tile] = (cmt & kTxClamp) != 0;
    }
}
static void ra_set_depth_test(bool) {}
static void ra_set_depth_mask(bool) {}

// N64 decal surfaces (shadows, paintings, floor markings) are authored exactly
// coplanar with what they sit on and rely on this bias to win the depth test.
// Must clear FP16 vertex quantization (relative step 2^-11) to survive
// FQVertex::Pack; the rest is headroom.
static constexpr float kDecalDepthBias = 2e-3f;
static bool g_zmode_decal = false;
static void ra_set_zmode_decal(bool zmode_decal) { g_zmode_decal = zmode_decal; }
static void ra_set_viewport(int, int, int, int) {}
static void ra_set_scissor(int, int, int, int) {}
static void ra_set_use_alpha(bool use_alpha) { g_use_alpha = use_alpha; }
// Never reached: gfx_pc prefers draw_triangles_3d whenever it is set.
static void ra_draw_triangles(float[], size_t, size_t) {}

// Perspective (game) vs screen-space/ortho (UI). pos3d is only filled for
// vertices that went through a modelview; HUD rectangles set is_3d=false so
// pos3d is NULL. Ortho world draws still have pos3d but projection[2][3]==0.
// [Foundation] Current render pass, forwarded from the game via gDPNoOpTag ->
// set_render_layer. Lets us split the single gfx_pc draw stream into
// skybox / game-geometry / UI zones instead of guessing from the projection.
static uint32_t g_current_pass = FOUNDATION_PASS_GAME;
static uint64_t g_pass_tris[4] = {0, 0, 0, 0}; // indexed by FOUNDATION_PASS_* tag
static uint32_t g_last_pass_log = 0;

static void ra_set_render_layer(uint32_t tag) {
    if (tag < FOUNDATION_PASS_SKYBOX || tag > FOUNDATION_PASS_UI)
        return;
    if (tag != g_current_pass) {
        // Throttled summary so we can confirm the three zones are distinct.
        const uint32_t now = SDL_GetTicks();
        if (now - g_last_pass_log > 1000) {
            std::fprintf(stderr,
                "[foundation] passes (last ~1s): skybox=%llu game=%llu ui=%llu tris\n",
                (unsigned long long)g_pass_tris[FOUNDATION_PASS_SKYBOX],
                (unsigned long long)g_pass_tris[FOUNDATION_PASS_GAME],
                (unsigned long long)g_pass_tris[FOUNDATION_PASS_UI]);
            g_pass_tris[FOUNDATION_PASS_SKYBOX] = 0;
            g_pass_tris[FOUNDATION_PASS_GAME] = 0;
            g_pass_tris[FOUNDATION_PASS_UI] = 0;
            g_last_pass_log = now;
        }
    }
    g_current_pass = tag;
}

static bool IsGamePerspectiveDraw(const float* pos3d, const float projection[4][4]) {
    return pos3d != nullptr && std::fabs(projection[2][3]) > 1e-6f;
}

// Shared screen-space capture used by both the skybox and UI/overlay passes.
// Both are ortho (no perspective), so vertices are stored as clip-space
// (px,py,pz,pw) and drawn as screen-space overlays later.
static void CaptureScreenSpaceBatch(std::vector<UiVertex>& verts, std::vector<UiBatch>& batches,
                                    uint32_t& triCount, bool& dropped, uint32_t maxTris,
                                    float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    const uint32_t stride = g_cur_shader->num_floats;
    if (buf_vbo_len != num_tris * 3u * stride)
        return;

    const bool textured = g_cur_shader->cc.used_textures[0] || g_cur_shader->cc.used_textures[1];
    const bool fog = g_cur_shader->cc.opt_fog;
    const bool alpha = g_cur_shader->cc.opt_alpha;
    const int num_inputs = g_cur_shader->cc.num_inputs;

    // pos[4] + optional uv[2] + optional fog[4] + inputs
    uint32_t uvOff = 4;
    uint32_t colorOff = 4 + (textured ? 2u : 0u) + (fog ? 4u : 0u);

    UiBatch batch{};
    batch.firstVertex = static_cast<uint32_t>(verts.size());
    batch.textureId = textured ? g_cur_tex[0] : 0u;
    batch.shaderId = g_cur_shader->shader_id;
    batch.useAlpha = g_use_alpha || alpha;

    // Resolve the GPUScene bindless texture index for this batch (g_cur_tex[0]
    // is the gfx_pc texture id; g_tex_by_id maps it to a GPUScene TextureHandle
    // whose .index IS the bindless pool slot). Untextured quads get -1.
    float bindlessId = -1.0f;
    if (textured) {
        auto it = g_tex_by_id.find((uint32_t)g_cur_tex[0]);
        if (it != g_tex_by_id.end() && it->second.IsValid())
            bindlessId = (float)it->second.index;
    }

    for (size_t t = 0; t < num_tris; ++t) {
        if (triCount >= maxTris) {
            dropped = true;
            break;
        }
        for (uint32_t v = 0; v < 3; ++v) {
            const float* vtx = buf_vbo + (t * 3u + v) * stride;
            UiVertex u{};
            u.px = vtx[0]; u.py = vtx[1]; u.pz = vtx[2]; u.pw = vtx[3];
            if (textured) {
                u.u = vtx[uvOff]; u.v = vtx[uvOff + 1];
                if (u.u < 0) u.u += 1.0f;
                if (u.v < 0) u.v += 1.0f;
            }
            if (num_inputs > 0) {
                u.r = vtx[colorOff + 0]; u.g = vtx[colorOff + 1]; u.b = vtx[colorOff + 2];
                u.a = alpha ? vtx[colorOff + 3] : 1.0f;
            } else {
                u.r = u.g = u.b = u.a = 1.0f;
            }
            u.texId = bindlessId;
            verts.push_back(u);
        }
        ++triCount;
        batch.vertexCount += 3;
    }
    if (batch.vertexCount > 0)
        batches.push_back(batch);
}

static void CaptureUiBatch(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    CaptureScreenSpaceBatch(g_ui_verts, g_ui_batches, g_ui_tris, g_dropped_ui, kMaxUiTris,
                            buf_vbo, buf_vbo_len, num_tris);
}

// [Foundation] Skybox is ortho/screen-space like the UI overlay, but kept in its
// own buffer so it can be drawn as a separate pass behind the game geometry.
static void CaptureSkyboxBatch(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    CaptureScreenSpaceBatch(g_skybox_verts, g_skybox_batches, g_skybox_tris, g_dropped_skybox, kMaxUiTris,
                            buf_vbo, buf_vbo_len, num_tris);
}

// ---------------------------------------------------------------------------
// Material bucketing
// ---------------------------------------------------------------------------
// FQVertex has no color channel, so the N64 shade/prim color has to live in the
// material. Colors are quantized to 5 bits per channel to keep the number of
// distinct buckets down for gouraud-shaded geometry.
static uint32_t QuantizeColor(const float4& c) {
    auto q = [](float v) -> uint32_t {
        const float clamped = std::clamp(v, 0.0f, 1.0f);
        return (uint32_t)(clamped * 31.0f + 0.5f);
    };
    return (q(c.x) << 15) | (q(c.y) << 10) | (q(c.z) << 5) | q(c.w);
}
static float4 DequantizeColor(uint32_t q) {
    return float4(((q >> 15) & 31u) / 31.0f, ((q >> 10) & 31u) / 31.0f,
                  ((q >> 5) & 31u) / 31.0f, (q & 31u) / 31.0f);
}

static uint32_t OpenBucket(uint32_t texIndex, const float4& color) {
    const uint32_t idx = g_live_buckets++;
    Bucket& b = g_buckets[idx];
    b.texIndex = texIndex;
    b.color = color;
    g_bucket_by_tex[texIndex] = idx;
    return idx;
}

// Returns a bucket with room for one more triangle, or kInvalidBucket.
static uint32_t AcquireBucket(uint32_t texIndex, uint32_t quantColor) {
    const uint64_t key = ((uint64_t)texIndex << 32) | quantColor;
    const float4 color = DequantizeColor(quantColor);

    if (auto it = g_bucket_by_key.find(key); it != g_bucket_by_key.end()) {
        if (g_buckets[it->second].vertexCount + 3u <= kBucketVerts)
            return it->second;
        // Bucket is full: chain a fresh one for the same material.
        if (g_live_buckets < kMaxBuckets) {
            it->second = OpenBucket(texIndex, color);
            return it->second;
        }
    } else if (g_live_buckets < kMaxBuckets) {
        const uint32_t idx = OpenBucket(texIndex, color);
        g_bucket_by_key.emplace(key, idx);
        return idx;
    }

    // Out of buckets: fall back to any bucket with the same texture rather than
    // dropping the geometry - only the baked color ends up slightly off.
    if (auto it = g_bucket_by_tex.find(texIndex); it != g_bucket_by_tex.end()) {
        if (g_buckets[it->second].vertexCount + 3u <= kBucketVerts)
            return it->second;
    }
    return kInvalidBucket;
}

static void CaptureGameBatch(float buf_vbo[], size_t buf_vbo_len, size_t num_tris, const float* pos3d) {
    const uint32_t stride = g_cur_shader->num_floats;
    if (buf_vbo_len != num_tris * 3u * stride) {
        std::fprintf(stderr, "game draw: stride mismatch (shader %08x expects %u/vtx, got %zu)\n",
                     g_cur_shader->shader_id, stride, buf_vbo_len / (num_tris * 3u));
        return;
    }
    const CCFeatures& cc = g_cur_shader->cc;
    const int tile = cc.used_textures[0] ? 0 : (cc.used_textures[1] ? 1 : -1);
    const bool textured = tile >= 0;
    const bool alpha = cc.opt_alpha;
    const int num_inputs = cc.num_inputs;
    const uint32_t colorOff = 4u + (textured ? 2u : 0u) + (cc.opt_fog ? 4u : 0u);

    uint32_t texIndex = kNoTexture;
    if (textured) {
        // gfx_pc always writes tile 0's UVs at vtx[4..5], whichever tile is used.
        if (auto it = g_tex_by_id.find(g_cur_tex[tile]); it != g_tex_by_id.end() && it->second.IsValid())
            texIndex = it->second.index;
    }

    // Uniformly scaling a view-space position is a pure depth offset: the camera
    // sits at the origin, so the perspective divide cancels the scale in x/y and
    // only NDC z moves. Shrinking pulls the decal toward the camera.
    const float decalScale = g_zmode_decal ? (1.0f - kDecalDepthBias) : 1.0f;

    for (size_t t = 0; t < num_tris; ++t) {
        if (g_captured_tris >= kMaxTris) {
            g_dropped_tris = true;
            return;
        }
        const float* p = pos3d + t * 9u;
        const float3 tri[3] = {float3(p[0], p[1], p[2]) * decalScale,
                               float3(p[3], p[4], p[5]) * decalScale,
                               float3(p[6], p[7], p[8]) * decalScale};
        const float3 n = cross(tri[1] - tri[0], tri[2] - tri[0]);
        const float nlen = length(n);
        if (nlen < 1e-8f)
            continue;

        // The combiner modulates the texture by shade/prim/env; approximate that
        // with the triangle's average input color baked into baseColorFactor.
        float4 color(1.0f);
        if (num_inputs > 0) {
            color = float4(0.0f);
            for (uint32_t v = 0; v < 3; ++v) {
                const float* vtx = buf_vbo + (t * 3u + v) * stride;
                color += float4(vtx[colorOff + 0], vtx[colorOff + 1], vtx[colorOff + 2],
                                alpha ? vtx[colorOff + 3] : 1.0f);
            }
            color /= 3.0f;
        }

        const uint32_t bucketIdx = AcquireBucket(texIndex, QuantizeColor(color));
        if (bucketIdx == kInvalidBucket) {
            g_dropped_tris = true;
            return;
        }
        Bucket& bucket = g_buckets[bucketIdx];
        FQVertex* out = &g_bucket_verts[(size_t)bucketIdx * kBucketVerts + bucket.vertexCount];

        for (uint32_t v = 0; v < 3; ++v) {
            FVertex fv{};
            fv.position = tri[v];
            fv.normal = n / nlen;
            if (textured) {
                const float* vtx = buf_vbo + (t * 3u + v) * stride;
                fv.uv = float2(vtx[4], vtx[5]);
            }
            out[v] = FQVertex::Pack(fv);
        }
        bucket.vertexCount += 3u;
        ++g_captured_tris;
    }
}

static void ra_draw_triangles_3d(float buf_vbo[], size_t buf_vbo_len, size_t num_tris, const float* pos3d,
                                 const float projection[4][4]) {
    if (!g_cur_shader || num_tris == 0)
        return;

    // Partition into skybox / game-geometry / UI by the explicit pass tag the
    // game forwarded (skybox and UI are both ortho/screen-space and cannot be
    // told apart by projection alone, which is why we tag them upstream).
    const bool isGame3D = (g_current_pass == FOUNDATION_PASS_GAME) && IsGamePerspectiveDraw(pos3d, projection);
    if (isGame3D) {
        // projection == rsp.P_matrix, built from node->fov via guPerspective:
        // projection[1][1] = 1 / tan(fovY/2). Decode it so the camera matches the
        // game exactly, independent of sFOVState.
        if (projection[1][1] > 1e-6f)
            g_gameFovY = 2.0f * atanf(1.0f / projection[1][1]);
        CaptureGameBatch(buf_vbo, buf_vbo_len, num_tris, pos3d);
        g_pass_tris[FOUNDATION_PASS_GAME] += num_tris;
    } else if (g_current_pass == FOUNDATION_PASS_SKYBOX) {
        // Skybox is ortho/screen-space; keep it in its own buffer so it can be
        // drawn as a dedicated pass behind the game geometry.
        CaptureSkyboxBatch(buf_vbo, buf_vbo_len, num_tris);
        g_pass_tris[FOUNDATION_PASS_SKYBOX] += num_tris;
    } else {
        // UI / HUD overlay (ortho / screen-space).
        CaptureUiBatch(buf_vbo, buf_vbo_len, num_tris);
        g_pass_tris[FOUNDATION_PASS_UI] += num_tris;
    }
}
static void ra_init(void) {}
static void ra_on_resize(void) {}
static void ra_start_frame(void) {}
static void ra_end_frame(void) {}
static void ra_finish_render(void) {}
static void ra_shutdown(void) {}

static struct GfxRenderingAPI gfx_rendering_api = {
    ra_z_is_0to1,
    ra_unload_shader,
    ra_load_shader,
    ra_create_and_load_new_shader,
    ra_lookup_shader,
    ra_shader_get_info,
    ra_new_texture,
    ra_select_texture,
    ra_upload_texture,
    ra_set_sampler_parameters,
    ra_set_depth_test,
    ra_set_depth_mask,
    ra_set_zmode_decal,
    ra_set_viewport,
    ra_set_scissor,
    ra_set_use_alpha,
    ra_draw_triangles,
    ra_init,
    ra_on_resize,
    ra_start_frame,
    ra_end_frame,
    ra_finish_render,
    ra_shutdown,
    ra_draw_triangles_3d,
    ra_set_render_layer,
};

// ===========================================================================
// Screen-space overlay passes (skybox / UI)
// ===========================================================================
// Skybox and HUD both reach us as ortho triangles that gfx_pc already
// transformed into N64/OpenGL clip space, so they share a single pipeline: one
// interleaved UiVertex stream sampling the GPUScene bindless texture pool. Only
// depth handling and blending differ, so the pass is parameterised rather than
// duplicated.
//
// Both passes depth-test against the scene depth the rasterizer produced. The
// skybox composites onto the lit HDR AOV (so it gets tonemapped with the scene),
// while the HUD draws straight to the backbuffer after tonemapping - keeping the
// game's own palette instead of running it through the tonemap curve.
static constexpr uint32_t kOverlayZKeep = 0u;  // remap the game's vertex z
static constexpr uint32_t kOverlayZFar = 1u;   // pin to the reverse-Z far plane

struct OverlayPushConstants {
    uint32_t zMode;
    float alphaRef;
};

struct RasterizedPassDesc {
    const char* name = "Overlay";
    ResourceHandle colorTarget{};  // post-lighting AOV we composite onto (ignored if toBackbuffer)
    RHIResourceFormat colorFormat = RHIResourceFormat::R16G16B16A16SignedFloat;
    bool toBackbuffer = false;     // draw onto the swapchain backbuffer instead of colorTarget
    ResourceHandle sceneDepth{};   // scene depth, tested against but never written
    RHIExtent2D extent{};
    ResourceHandle vertexBuffer{};
    ResourceHandle sampler{};
    const std::vector<UiVertex>* vertices = nullptr;
    uint32_t zMode = kOverlayZKeep;
    bool blending = false;
    float alphaRef = 1.0f / 255.0f;
    size_t priority = 0;
};

static PassHandle CreateRasterizedPass(Renderer* renderer, const RendererResources& gpuRes,
                                       const RasterizedPassDesc& desc) {
    using PSD = RHIPipelineState::PipelineStateDesc;
    const auto shaderPath =
        renderer->GetApplication()->ResolveRelativePathBase("Data/Shaders/SM64Overlay.spv");

    return renderer->CreatePass(
        desc.name, RHIDeviceQueueType::Graphics, desc.priority,
        [desc, gpuRes, shaderPath](PassHandle self, Renderer* r) {
            const auto blending = desc.blending ? PSD::Attachment::Blending::GetAlphaBlending()
                                                : PSD::Attachment::Blending::GetNoBlending();
            if (desc.toBackbuffer) {
                // HUD: composite onto the tonemapped backbuffer in the game's own
                // colour space, so the tonemap curve never touches the HUD palette.
                r->BindBackbufferRTV(self, blending);
            } else {
                r->BindTextureRTV(self, desc.colorTarget,
                                  {.format = desc.colorFormat,
                                   .range = RHITextureSubresourceRange::Create(
                                       RHITextureAspectFlagBits::Color)},
                                  blending);
            }
            // Read-only DSV: z-test on, z-write off for both overlay passes.
            r->BindTextureDSV(self, desc.sceneDepth,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(
                                   RHITextureAspectFlagBits::Depth)},
                              /*readOnly=*/true);
            r->BindBufferCopyDst(self, desc.vertexBuffer);
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", shaderPath);
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", shaderPath);
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0,
                                sizeof(OverlayPushConstants));
            r->BindDescriptorSetRead(self, "gTextures2D",
                                     gpuRes.textures2D->GetDescriptorSetLayout());
            r->BindTextureSampler(self, desc.sampler, "textureSampler");
            r->BindVertexInput(
                self, {.bindings = {{{sizeof(UiVertex), false}}},
                       .attributes = {{
                           {.location = 0,
                            .offset = offsetof(UiVertex, px),
                            .format = RHIResourceFormat::R32G32B32A32SignedFloat},
                           {.location = 1,
                            .offset = offsetof(UiVertex, u),
                            .format = RHIResourceFormat::R32G32SignedFloat},
                           {.location = 2,
                            .offset = offsetof(UiVertex, r),
                            .format = RHIResourceFormat::R32G32B32A32SignedFloat},
                           {.location = 3,
                            .offset = offsetof(UiVertex, texId),
                            .format = RHIResourceFormat::R32SignedFloat},
                       }}});
            // gfx_pc has already applied the game's own culling, and the ortho
            // layers are authored two-sided, so leave winding alone.
            // Reverse-Z GreaterEqual: a skybox pinned to z = 0 still passes on
            // background pixels (depth cleared to 0) but loses to any geometry.
            r->PassSetRasterizerFlags(self, {.cullMode = PSD::Rasterizer::CullNone},
                                      {.depthFormat = RHIResourceFormat::D32SignedFloat,
                                       .depthTest = true,
                                       .depthWrite = false,
                                       .depthCompareOp = PSD::DepthStencil::GreaterEqual});
        },
        [desc, gpuRes](PassHandle self, Renderer* r, RHICommandList* cmd) {
            auto* vbo = r->DerefResource(desc.vertexBuffer).Get<RHIBuffer*>();
            const size_t vertexCount =
                std::min(desc.vertices->size(), kUiVboCapacityBytes / sizeof(UiVertex));
            const size_t vboOffset = static_cast<size_t>(r->GetSync()) * kUiVboCapacityBytes;
            if (vertexCount > 0) {
                std::memcpy(vbo->Map<char>() + vboOffset, desc.vertices->data(),
                            vertexCount * sizeof(UiVertex));
                vbo->Flush(vboOffset, vertexCount * sizeof(UiVertex));
            }

            // Load both attachments: we composite on top of the lit frame and
            // reuse the depth the rasterizer already resolved.
            r->CmdBeginGraphics(self, cmd, desc.extent, {{{RHIAttachmentLoadOp::Load}}},
                                {RHIAttachmentLoadOp::Load, {0.0f, 0u}});
            r->CmdSetPipeline(self, cmd);
            if (vertexCount > 0) {
                // flipY: gfx_pc emits GL-style clip space (+Y up), so let the
                // viewport do the flip instead of touching the game matrices.
                cmd->SetViewport(0, 0, (float)desc.extent.x, (float)desc.extent.y, 0.0f, 1.0f,
                                 /*flipY=*/true);
                cmd->SetScissor(0, 0, desc.extent.x, desc.extent.y);
                r->CmdBindDescriptorSet(self, cmd, "gTextures2D",
                                        gpuRes.textures2D->GetDescriptorSet());
                const OverlayPushConstants pc{desc.zMode, desc.alphaRef};
                r->CmdSetPushConstant(
                    self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, pc);
                cmd->BindVertexBuffer(0, {{vbo}}, {{vboOffset}});
                cmd->Draw((uint32_t)vertexCount);
            }
            cmd->EndGraphics();
        });
}

// ===========================================================================
// Foundation render-graph helpers (mirrored from SM64.cpp)
// ===========================================================================
static void RebuildGraph(ExampleVulkanContext& ctx, GPUScene& gpu) {
    Examples_ResetRenderer(ctx, RendererDesc{});
    ctx.renderer->BeginSetup();
    g_cfg.renderExtent = ctx.renderer->GetSwapchainExtent();
    g_cfg.cullFlags &= ~CullFlagsBits::Backface;
    g_ubo.ptMaxBounces = 2u;
    g_cfg.textureAnisoEnable = false;
    g_cfg.textureTrilinear = false;
    auto resources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
    BuildGPUSceneHostUpdatePass(ctx.renderer.get(), resources);
    Example_BuildExampleRenderer(g_renderer, ctx.renderer.get(), &g_ubo, resources, g_cfg, g_outputs);

    // Screen-space layers. The skybox composites onto the lit HDR AOV so it gets
    // tonemapped with the scene; the HUD draws straight to the backbuffer after
    // tonemapping so its palette is untouched. Both still depth-test against the
    // scene depth the rasterizer resolved (read-only DSV, z-write off).
    const RHIBufferDesc overlayVboDesc{
        .resource = {.heap = RHIDeviceHeapType::Upload,
                     .hostAccess = RHIResourceHostAccess::WriteOnly},
        .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
        .size = kUiVboCapacityBytes * ctx.renderer->GetFrameSwaps()};
    g_skybox_vbo = ctx.renderer->CreateResource("SM64 Skybox VBO", overlayVboDesc);
    g_ui_vbo = ctx.renderer->CreateResource("SM64 UI VBO", overlayVboDesc);
    g_overlay_sampler = ctx.renderer->CreateSampler({});

    RasterizedPassDesc overlayBase{};
    overlayBase.colorTarget = g_outputs.diffuse;
    overlayBase.colorFormat = g_outputs.aovFormat;
    overlayBase.sceneDepth = g_outputs.depth;
    overlayBase.extent = g_outputs.extent;
    overlayBase.sampler = g_overlay_sampler;

    RasterizedPassDesc skyboxDesc = overlayBase;
    skyboxDesc.name = "SM64 Skybox";
    skyboxDesc.vertexBuffer = g_skybox_vbo;
    skyboxDesc.vertices = &g_skybox_verts;
    // The skybox has no meaningful depth of its own - force it onto the far
    // plane so any rasterized geometry wins the depth test.
    skyboxDesc.zMode = kOverlayZFar;
    skyboxDesc.blending = false;
    CreateRasterizedPass(ctx.renderer.get(), resources, skyboxDesc);

    // Tonemap the lit HDR AOV (skybox included) into the backbuffer *before* the
    // HUD pass, so the HUD composites onto the already-display-encoded image.
    Examples_BuildTonemappingPass(ctx.renderer.get(), g_outputs, true);

    RasterizedPassDesc uiDesc = overlayBase;
    uiDesc.name = "SM64 UI";
    uiDesc.vertexBuffer = g_ui_vbo;
    uiDesc.vertices = &g_ui_verts;
    // The HUD keeps the depth its own ortho matrix produced (dialog boxes and
    // the pause menu rely on the game's own layering), blends over the frame,
    // and is emitted to the swapchain backbuffer (past the tonemap stage).
    uiDesc.zMode = kOverlayZKeep;
    uiDesc.blending = true;
    uiDesc.toBackbuffer = true;
    CreateRasterizedPass(ctx.renderer.get(), resources, uiDesc);
    RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text",
                                                 Examples_HudLines(g_input));
    ctx.renderer->EndSetup();
}

// Lit HUD token: green via Examples_Push/PopColor while `active`.
static void HudLitText(ExampleInputState& input, StringView text, bool active, bool sameLine = true)
{
    if (active)
        Examples_PushColor(input, 0, 255, 0);
    Examples_Text(input, text);
    if (active)
        Examples_PopColor(input);
    if (sameLine)
        Examples_SameLine(input, 0);
}

#ifndef VK_INVALID
#define VK_INVALID 0xFFFFu
#endif
#ifndef VK_BASE_SDL_GAMEPAD
#define VK_BASE_SDL_GAMEPAD 0x1000u
#endif

// First configured bind for an action — prefer a keyboard VK so the HUD matches
// what players type; fall back to whatever is bound (gamepad/mouse).
static unsigned int FirstBind(const unsigned int binds[MAX_BINDS])
{
    for (int i = 0; i < MAX_BINDS; ++i) {
        if (binds[i] != VK_INVALID && binds[i] < VK_BASE_SDL_GAMEPAD)
            return binds[i];
    }
    for (int i = 0; i < MAX_BINDS; ++i) {
        if (binds[i] != VK_INVALID)
            return binds[i];
    }
    return VK_INVALID;
}

static const char* BindLabel(const unsigned int binds[MAX_BINDS])
{
    return keyboard_sdl3_vk_name(FirstBind(binds));
}

// One controls row driven by sm64ex_host_get_input() (merged keyboard + gamepad).
// Labels come from configKey* so they track the Options > Controls remaps.
// On the title screen (levelNum == 1) the whole row blinks red/white at 1 Hz.
static void HudControlsRow(ExampleInputState& input, SM64ExHostInput const& pad, int16_t levelNum)
{
    constexpr int16_t kStickDeadzone = 8;
    const bool stickLeft = pad.stickX <= -kStickDeadzone;
    const bool stickRight = pad.stickX >= kStickDeadzone;
    const bool stickDown = pad.stickY <= -kStickDeadzone;
    const bool stickUp = pad.stickY >= kStickDeadzone;
    auto btn = [&](uint16_t mask) { return (pad.buttonDown & mask) != 0; };

    const bool titleBlink = (levelNum == 1);
    const bool blink = std::fmod(Examples_GetTime(), 1.0f) < 0.5f;
    if (titleBlink) {
        if (blink)
            Examples_PushColor(input, 255, 0, 0);
        else
            Examples_PushColor(input, 255, 255, 255);
    }

    HudLitText(input, BindLabel(configKeyStickUp), stickUp);
    HudLitText(input, BindLabel(configKeyStickLeft), stickLeft);
    HudLitText(input, BindLabel(configKeyStickDown), stickDown);
    HudLitText(input, BindLabel(configKeyStickRight), stickRight);
    Examples_Text(input, " move | ");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyA), btn(SM64EX_BTN_A));
    Examples_Text(input, " jump | ");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyB), btn(SM64EX_BTN_B));
    Examples_Text(input, " action | ");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyZ), btn(SM64EX_BTN_Z));
    Examples_Text(input, " crouch | ");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyCUp), btn(SM64EX_BTN_C_UP));
    HudLitText(input, BindLabel(configKeyCLeft), btn(SM64EX_BTN_C_LEFT));
    HudLitText(input, BindLabel(configKeyCDown), btn(SM64EX_BTN_C_DOWN));
    HudLitText(input, BindLabel(configKeyCRight), btn(SM64EX_BTN_C_RIGHT));
    Examples_Text(input, " cam | ");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyStart), btn(SM64EX_BTN_START));
    Examples_Text(input, " Start | ");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyL), btn(SM64EX_BTN_L));
    Examples_Text(input, "/");
    Examples_SameLine(input, 0);
    HudLitText(input, BindLabel(configKeyR), btn(SM64EX_BTN_R), /*sameLine=*/false);

    if (titleBlink)
        Examples_PopColor(input);
}

static const char* PlayModeLabel(int16_t mode)
{
    switch (mode) {
    case SM64EX_PLAY_MODE_NORMAL:        return "normal";
    case SM64EX_PLAY_MODE_PAUSED:        return "paused";
    case SM64EX_PLAY_MODE_CHANGE_AREA:   return "change-area";
    case SM64EX_PLAY_MODE_CHANGE_LEVEL:  return "change-level";
    case SM64EX_PLAY_MODE_FRAME_ADVANCE: return "frame-advance";
    default:                             return "unknown";
    }
}

static void HudGameStateRows(ExampleInputState& input, SM64ExHostGameState const& st)
{
    // Health wedges are the upper byte of the raw health word (0x0880 == 8).
    const int wedges = (st.health >> 8) & 0xFF;
    Examples_Text(input,
                  Format("lvl {} area {} course {} act {} | {} | file {}",
                         st.levelNum, st.areaIndex, st.courseNum, st.actNum,
                         PlayModeLabel(st.playMode), st.saveFileNum));
    Examples_Text(input,
                  Format("pos {:.0f},{:.0f},{:.0f} vel {:.1f},{:.1f},{:.1f} fwd {:.1f} yaw {} act {:08X}",
                         st.posX, st.posY, st.posZ, st.velX, st.velY, st.velZ, st.forwardVel,
                         st.faceYaw, st.action));
    Examples_Text(input,
                  Format("HP {}/8 lives {} coins {} stars {} keys {} | hud L{} C{} S{} W{} T{}",
                         wedges, st.lives, st.coins, st.stars, st.keys, st.hudLives, st.hudCoins,
                         st.hudStars, st.hudWedges, st.hudTimer));
}

// EndScene resolves every slot BeginScene handed out, so the counts must match
// what we actually fill in: one instance + one material per live material bucket.
static void CommitScene(GPUScene& gpu, uint32_t liveBuckets) {
    auto tables = gpu.BeginScene(liveBuckets, liveBuckets, 2);
    for (uint32_t i = 0; i < liveBuckets; ++i) {
        const Bucket& bucket = g_buckets[i];
        GSMaterial material{};
        material.baseColorFactor = bucket.color;
        material.metallicFactor = 0.0f;
        material.roughnessFactor = 0.75f;
        material.ior = 1.5f;
        if (bucket.texIndex != kNoTexture)
            material.baseColorTexture = bucket.texIndex;
        tables.materials[i] = material;
        tables.instances[i] = GSInstance{
            .transform = float3(0.0f, 0.0f, 0.0f),
            .rotation = quat(0.0f, 0.0f, 0.0f, 1.0f),
            .scale = float3(1.0f, 1.0f, 1.0f),
            .materialIndex = i,
            .resourceIndex = bucket.geo.index,
            .type = kGSInstanceTypeMesh,
        };
    }
    tables.lights[0] = GSLight{.flags = kGSLightTypeEnvironment, .color = float3(1.0f, 1.0f, 1.0f), .power = 0.25f};
    tables.lights[1] = GSLight{.flags = kGSLightTypeDirectional | to_integer(GSLightFlagsBits::UseShadow),
                               .color = float3(1.0f, 0.96f, 0.9f),
                               .power = 1.0f,
                               .direction = float3(0.10f, -1.0f, 0.0f),
                               .params = float4(.05f, 0.0f, 0.0f, 0.0f)};
    gpu.EndScene(tables);
    gpu.UpdateUBO(g_ubo);
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
    g_window = SDL_CreateWindow("Super Mario 64 EX (Foundation) - INTERNAL EVALUATION BUILD", 1280, 720,
                                Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(g_window, argc, argv, RendererDesc{});

    // GPUScene init (mirrors SM64.cpp).
    GPUSceneDesc desc{};
    const uint32_t framesInFlight = std::max(ctx.renderer->GetFrameSwaps(), 1u);
    const uint32_t gpuSceneRingFrameSlack = framesInFlight + 1u;
    desc.primitiveBudget = 1024u * 1024u;
    desc.dynamicGeometryBudget = 128 * 1024u * 1024u;
    desc.dynamicStagingBudget = 128 * 1024u * 1024u;
    desc.dynamicStagingFramesInFlight = framesInFlight;
    // One instance/material/geometry per material bucket, plus headroom for the
    // per-frame instance & material rings.
    desc.instanceBudget = kMaxBuckets * gpuSceneRingFrameSlack;
    desc.materialBudget = kMaxBuckets * gpuSceneRingFrameSlack;
    desc.lightBudget = 2u * gpuSceneRingFrameSlack;
    desc.geometryBudget = kMaxBuckets;
    desc.tlasInstanceBudget = kMaxBuckets * gpuSceneRingFrameSlack;
    desc.texturesBudget = 4096;
    GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);
    g_gpu = &gpu;

    for (Bucket& bucket : g_buckets)
        CHECK(gpu.Allocate(kBucketVerts, kBucketVerts, bucket.geo, /*isGpu=*/false) == GPUScene::Result::Ready);

    // UpdateDynamicGeometryCPU requires the full allocation every time, so each
    // bucket keeps kBucketVerts vertices with the unused tail degenerated.
    g_bucket_verts.assign((size_t)kMaxBuckets * kBucketVerts, kDegenerateVertex);
    g_indices.resize(kBucketVerts);
    for (uint32_t i = 0; i < kBucketVerts; ++i)
        g_indices[i] = i;

    // ---- sm64ex host init (replaces libsm64 dlopen) ----
    struct SM64ExHostConfig host_cfg = {0};
    host_cfg.rendering_api = &gfx_rendering_api;
    host_cfg.wm_api = &gfx_window_manager_api;
    host_cfg.audio_api = &audio_sdl3;   // SDL3 audio backend (32 kHz stereo S16)
    host_cfg.window_title = "sm64ex Foundation";
    if (!sm64ex_host_init(&host_cfg)) {
        std::fprintf(stderr, "sm64ex_host_init failed\n");
        Examples_DestroyVulkan(g_window, ctx);
        return 1;
    }

    // Foundation port: skip the intro cutscene / castle jingle by default.
    // Mirrors the original port's --skip-intro CLI flag without needing args.
    configSkipIntro = true;

    // sm64ex never enables backface culling, so its geometry is authored
    // double-sided; culling it here punches holes in the level.
    g_cfg.cullFlags = CullFlagsBits::Frustum;
    if (!ctx.device->GetCapabilities().raytracingInline)
        g_cfg.viewFlags &= ~ViewFlagsBits::EnableRasterRTShadows;

    // Build the render graph once up front (the resize path only rebuilds it).
    // This also fills g_cfg.renderExtent used for the camera aspect ratio.
    RebuildGraph(ctx, gpu);

    const String deviceName = ctx.device->QueryDeviceString();

    // Fixed refresh
    static constexpr uint64_t kTargetFps = 30;
    static constexpr uint64_t kFrameNs = 1'000'000'000ull / kTargetFps;
    uint64_t nextFrameNs = SDL_GetTicksNS();

    while (g_running) {
        Examples_BeginFrameInput(g_input);
        if (Examples_PollEvents(g_window, ctx, g_input, nullptr, OnSdlEvent))
            break;

        if (g_input.wantResizeOrRebuild) {
            g_input.wantResizeOrRebuild = false;
            RebuildGraph(ctx, gpu);
        }

        // Fixed tick; captures split into game (GPUScene) and UI (overlay buffer).
        g_captured_tris = 0;
        g_dropped_tris = false;
        g_live_buckets = 0;
        g_bucket_by_key.clear();
        g_bucket_by_tex.clear();
        for (Bucket& bucket : g_buckets)
            bucket.vertexCount = 0;
        g_ui_verts.clear();
        g_ui_batches.clear();
        g_ui_tris = 0;
        g_dropped_ui = false;
        g_skybox_verts.clear();
        g_skybox_batches.clear();
        g_skybox_tris = 0;
        g_dropped_skybox = false;
        sm64ex_host_frame();

        // Camera FOV decoded from the game projection matrix (authoritative;
        // tracks modes / cutscenes / shake via rsp.P_matrix). Set in
        // ra_draw_triangles_3d each frame, so no sFOVState dependency.
        g_camera.fovY = g_gameFovY;

        // Always keep one bucket live so BeginScene never gets a zero count.
        const uint32_t liveBuckets = std::max(g_live_buckets, 1u);

        gpu.BeginDynamicUpdate();
        for (uint32_t i = 0; i < liveBuckets; ++i) {
            Bucket& bucket = g_buckets[i];
            FQVertex* verts = &g_bucket_verts[(size_t)i * kBucketVerts];
            // Degenerate whatever this slot drew when it was last used, so stale
            // triangles stop rendering.
            for (uint32_t v = bucket.vertexCount; v < bucket.dirtyCount; ++v)
                verts[v] = kDegenerateVertex;
            bucket.dirtyCount = bucket.vertexCount;

            gpu.UpdateDynamicMeshCPU(bucket.geo, Span<const FQVertex>{verts, kBucketVerts},
                                     bucket.indicesUploaded
                                         // Identity topology, so it only ever needs uploading once.
                                         ? Span<const uint32_t>{}
                                         : Span<const uint32_t>{g_indices.data(), g_indices.size()});
            bucket.indicesUploaded = true;
        }
        gpu.EndDynamicUpdate();

        g_camera.aspect = static_cast<float>(g_cfg.renderExtent.x) /
                          static_cast<float>(g_cfg.renderExtent.y);
        g_camera.RefreshMatrices();
        UpdateRendererCameraUBO(g_ubo, ctx.renderer->GetFrame(), g_camera.view, g_camera.proj);
        g_ubo.zNear = g_camera.zNear;
        g_ubo.projPlanes = planeSymmetric(g_camera.proj);
        g_ubo.camPosition = float4(FViewSpaceCamera::kPosition, 0.0f);
        g_ubo.camDirection = float4(FViewSpaceCamera::kForward, 0.0f);
        g_ubo.dbgViewFlags = g_cfg.viewFlags;
        g_ubo.dbgMaterialFlags = g_cfg.materialFlags;

        CommitScene(gpu, liveBuckets);

        SM64ExHostInput pad{};
        sm64ex_host_get_input(&pad);
        SM64ExHostGameState game{};
        sm64ex_host_get_game_state(&game);

        Examples_PushScale(g_input, 1);
        Examples_Text(g_input, Format("{:.0f} FPS | game {} tris | skybox {} | ui {} tris ({} batches)",
                                      g_fps.Update(), g_captured_tris, g_skybox_tris, g_ui_tris, g_ui_batches.size()));
        Examples_Text(g_input, Format("FOV {:.1f} deg | frame {} | {}/{} material buckets | {} textures",
                                      degrees(g_camera.fovY), ctx.renderer->GetFrame(), g_live_buckets,
                                      kMaxBuckets, g_tex_by_hash.size()));
        size_t gpuMemoryUsed = 0;
        size_t gpuMemoryBudget = 0;
        ctx.device->QueryBudget(RHIDeviceHeapType::Local, gpuMemoryUsed, gpuMemoryBudget);
        constexpr double kBytesPerMiB = 1024.0 * 1024.0;
        Examples_Text(g_input, Format("GPU memory {:.1f} / {:.1f} MiB",
                                      gpuMemoryUsed / kBytesPerMiB, gpuMemoryBudget / kBytesPerMiB));
        HudGameStateRows(g_input, game);
        HudControlsRow(g_input, pad, game.levelNum);
        Examples_Text(g_input, deviceName);
        if (g_dropped_tris)
            Examples_Text(g_input, Format("dropped game triangles ({} tri / {} bucket budget)", kMaxTris,
                                          kMaxBuckets));
        if (g_dropped_textures)
            Examples_Text(g_input, Format("dropped textures past the {} budget", 4096));
        if (g_dropped_ui)
            Examples_Text(g_input, Format("dropped UI triangles past the {} budget", kMaxUiTris));
        if (g_dropped_skybox)
            Examples_Text(g_input, Format("dropped skybox triangles past the {} budget", kMaxUiTris));
        // The skybox / UI passes upload g_skybox_verts and g_ui_verts straight
        // from their record callbacks, so the soups have to stay alive until
        // Examples_NewFrame below has recorded and submitted the graph.
        if (Examples_RendererSwitchButton(g_input, g_renderer))
            g_input.wantResizeOrRebuild = true;
        if (g_renderer == ExampleRenderer::ProgressivePT)
            g_renderer = ExampleRenderer::Raster; // Wrap back since we don't want *that* here...

        Examples_NewFrame(g_window, ctx);

        // Pace after present so swap cost is included (same as gfx_sdl2).
        nextFrameNs += kFrameNs;
        const uint64_t now = SDL_GetTicksNS();
        if (now < nextFrameNs)
            SDL_DelayNS(nextFrameNs - now);
        else
            nextFrameNs = now; // fell behind; resync rather than catch up in a burst
    }

    sm64ex_host_deinit();
    g_gpu = nullptr;   // no texture uploads past this point
    Examples_DestroyVulkan(g_window, ctx);
    wm_shutdown();
    return 0;
}
