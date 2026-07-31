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
//   - Separate capture of screen-space / ortho draws into a UI batch list
//   - FOV from sFOVState (via sm64ex_host_get_fov_degrees)
//
// TODO:
//   - Split static level geometry (cache/upload once by display-list hash) from
//     dynamic actor geometry instead of rebuilding one soup per frame.
//   - Map N64 combiner/textures -> Foundation PBR (GSMaterial): needs one
//     instance per material, so today everything shares a single material.
//   - Commit g_ui_batches through a dedicated UI overlay pass.
//   - Host audio backend + controller input mapping (SDL gamepad/keyboard).

#include <SDL3/SDL.h>

#include "sm64ex_host.h"          // sm64ex host embedding interface (PUBLIC include)
#include "gfx/gfx_cc.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"

#include <Core/Allocator.hpp>     // GLOBAL_ALLOC
#include <Renderer/Renderer.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Mesh.hpp>
#include <Renderer/Rasterizer.hpp>
#include <Math/Math.hpp>
#include <Math/ModelViewProjection.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <Examples.hpp>
#include <Renderer/Examples.hpp>

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
static ExampleRenderer   g_renderer = ExampleRenderer::Raster;

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
static constexpr uint32_t kMaxVerts = kMaxTris * 3u;
static constexpr uint32_t kMaxUiTris = 4096u;

static std::vector<FQVertex> g_verts;
static std::vector<uint32_t> g_indices;
static uint32_t g_captured_tris = 0;
static uint32_t g_prev_tris = 0;
static bool g_dropped_tris = false;

struct UiVertex {
    float4 pos;   // clip-space xyzw as gfx_pc emitted it
    float2 uv;
    float4 color;
};
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

static std::unordered_map<uint32_t, std::vector<uint8_t>> g_textures; // id -> rgba32
static std::unordered_map<uint32_t, ShaderProgram> g_shaders;         // id -> decoded combiner
static const ShaderProgram* g_cur_shader = nullptr;
static uint32_t g_cur_tex[2] = {0, 0};
static int g_cur_tile = 0;

static const FQVertex kDegenerateVertex = [] {
    FVertex v{};
    v.normal = float3(0.0f, 0.0f, 1.0f);
    return FQVertex::Pack(v);
}();

// ===========================================================================
// GfxWindowManagerAPI  (Foundation's bundled SDL3)
// ===========================================================================
static void wm_init(const char* title) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    // The window is created by main() (before Foundation/Vulkan init). The host
    // calls init() again during sm64ex_host_init, after the window exists, so
    // only create it here if it is not already up.
    (void)title; // FOUNDATION_APPLICATION_TITLE() requires a string literal, not a var
    if (!g_window)
        g_window = SDL_CreateWindow(title, 1280, 720,
                                    Examples_SDLWindowFlagsVulkan);
}
static void wm_set_keyboard_callbacks(kb_callback_t, kb_callback_t, void (*)(void)) {
    // Input is pumped by Examples_PollEvents() in the host frame loop; the
    // callback hooks are unused on the host path. TODO: drive controller_api.
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
static bool wm_start_frame(void) { return true; }
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
static void ra_upload_texture(const uint8_t* rgba32, int w, int h) {
    // gfx_pc uploads into whichever tile it selected last.
    // TODO: upload to GPUScene as the GSMaterial baseColorTexture (de-dup by
    //       rgba32 data hash).
    std::vector<uint8_t> buf(rgba32, rgba32 + (size_t)w * (size_t)h * 4);
    g_textures[g_cur_tex[g_cur_tile]] = std::move(buf);
}
static void ra_set_sampler_parameters(int, bool, uint32_t, uint32_t) {}
static void ra_set_depth_test(bool) {}
static void ra_set_depth_mask(bool) {}
static void ra_set_zmode_decal(bool) {}
static void ra_set_viewport(int, int, int, int) {}
static void ra_set_scissor(int, int, int, int) {}
static void ra_set_use_alpha(bool use_alpha) { g_use_alpha = use_alpha; }
// Never reached: gfx_pc prefers draw_triangles_3d whenever it is set.
static void ra_draw_triangles(float[], size_t, size_t) {}

// Perspective (game) vs screen-space/ortho (UI). pos3d is only filled for
// vertices that went through a modelview; HUD rectangles set is_3d=false so
// pos3d is NULL. Ortho world draws still have pos3d but projection[2][3]==0.
static bool IsGamePerspectiveDraw(const float* pos3d, const float projection[4][4]) {
    return pos3d != nullptr && std::fabs(projection[2][3]) > 1e-6f;
}

static void CaptureUiBatch(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
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
    batch.firstVertex = static_cast<uint32_t>(g_ui_verts.size());
    batch.textureId = textured ? g_cur_tex[0] : 0u;
    batch.shaderId = g_cur_shader->shader_id;
    batch.useAlpha = g_use_alpha || alpha;

    for (size_t t = 0; t < num_tris; ++t) {
        if (g_ui_tris >= kMaxUiTris) {
            g_dropped_ui = true;
            break;
        }
        for (uint32_t v = 0; v < 3; ++v) {
            const float* vtx = buf_vbo + (t * 3u + v) * stride;
            UiVertex u{};
            u.pos = float4(vtx[0], vtx[1], vtx[2], vtx[3]);
            u.uv = textured ? float2(vtx[uvOff], vtx[uvOff + 1]) : float2(0.0f);
            if (num_inputs > 0) {
                u.color = float4(vtx[colorOff + 0], vtx[colorOff + 1], vtx[colorOff + 2],
                                 alpha ? vtx[colorOff + 3] : 1.0f);
            } else {
                u.color = float4(1.0f);
            }
            g_ui_verts.push_back(u);
        }
        ++g_ui_tris;
        batch.vertexCount += 3;
    }
    if (batch.vertexCount > 0)
        g_ui_batches.push_back(batch);
}

static void CaptureGameBatch(float buf_vbo[], size_t buf_vbo_len, size_t num_tris, const float* pos3d) {
    const uint32_t stride = g_cur_shader->num_floats;
    if (buf_vbo_len != num_tris * 3u * stride) {
        std::fprintf(stderr, "game draw: stride mismatch (shader %08x expects %u/vtx, got %zu)\n",
                     g_cur_shader->shader_id, stride, buf_vbo_len / (num_tris * 3u));
        return;
    }
    const bool textured = g_cur_shader->cc.used_textures[0] || g_cur_shader->cc.used_textures[1];

    for (size_t t = 0; t < num_tris; ++t) {
        if (g_captured_tris >= kMaxTris) {
            g_dropped_tris = true;
            return;
        }
        const float* p = pos3d + t * 9u;
        const float3 p0(p[0], p[1], p[2]), p1(p[3], p[4], p[5]), p2(p[6], p[7], p[8]);
        const float3 n = cross(p1 - p0, p2 - p0);
        const float nlen = length(n);
        if (nlen < 1e-8f)
            continue;

        for (uint32_t v = 0; v < 3; ++v) {
            FVertex fv{};
            fv.position = float3(p[v * 3 + 0], p[v * 3 + 1], p[v * 3 + 2]);
            fv.normal = n / nlen;
            if (textured) {
                const float* vtx = buf_vbo + (t * 3u + v) * stride;
                fv.uv = float2(vtx[4], vtx[5]);
            }
            g_verts[g_captured_tris * 3u + v] = FQVertex::Pack(fv);
        }
        ++g_captured_tris;
    }
}

static void ra_draw_triangles_3d(float buf_vbo[], size_t buf_vbo_len, size_t num_tris, const float* pos3d,
                                 const float projection[4][4]) {
    if (!g_cur_shader || num_tris == 0)
        return;

    if (IsGamePerspectiveDraw(pos3d, projection)) {
        // projection == rsp.P_matrix, built from node->fov via guPerspective:
        // projection[1][1] = 1 / tan(fovY/2). Decode it so the camera matches the
        // game exactly, independent of sFOVState.
        if (projection[1][1] > 1e-6f)
            g_gameFovY = 2.0f * atanf(1.0f / projection[1][1]);
        CaptureGameBatch(buf_vbo, buf_vbo_len, num_tris, pos3d);
    } else {
        CaptureUiBatch(buf_vbo, buf_vbo_len, num_tris);
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
};

// ===========================================================================
// Foundation render-graph helpers (mirrored from SM64.cpp)
// ===========================================================================
static void RebuildGraph(ExampleVulkanContext& ctx, GPUScene& gpu) {
    Examples_ResetRenderer(ctx, RendererDesc{});
    ctx.renderer->BeginSetup();
    g_cfg.renderExtent = ctx.renderer->GetSwapchainExtent();
    g_ubo.ptMaxBounces = 2u;
    auto resources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
    BuildGPUSceneHostUpdatePass(ctx.renderer.get(), resources);
    Example_BuildExampleRenderer(g_renderer, ctx.renderer.get(), &g_ubo, resources, g_cfg, g_outputs);
    Examples_BuildTonemappingPass(ctx.renderer.get(), g_outputs, true);
    RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text",
                                                 Examples_HudLines(g_input));
    ctx.renderer->EndSetup();
}

// EndScene resolves every slot BeginScene handed out, so the counts must match
// what we actually fill in. Unlike the libsm64 example (level + Mario) the whole
// captured frame is a single geometry here.
static void CommitScene(GPUScene& gpu, GeometryHandle captured) {
    auto tables = gpu.BeginScene(1, 1, 2);
    tables.materials[0] = GSMaterial{};
    tables.materials[0].baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
    tables.materials[0].metallicFactor = 0.0f;
    tables.materials[0].roughnessFactor = 0.75f;
    tables.materials[0].ior = 1.5f;
    tables.instances[0] = GSInstance{
        .transform = float3(0.0f, 0.0f, 0.0f),
        .rotation = quat(0.0f, 0.0f, 0.0f, 1.0f),
        .scale = float3(1.0f, 1.0f, 1.0f),
        .materialIndex = 0,
        .resourceIndex = captured.index,
        .type = kGSInstanceTypeMesh,
    };
    tables.lights[0] = GSLight{.flags = kGSLightTypeEnvironment, .color = float3(0.5f, 0.6f, 0.8f), .power = 1.0f};
    tables.lights[1] = GSLight{.flags = kGSLightTypeDirectional | to_integer(GSLightFlagsBits::UseShadow),
                               .color = float3(1.0f, 0.96f, 0.9f),
                               .power = 1.0f,
                               .direction = float3(0.0f, -1.0f, 0.0f),
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

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    g_window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("sm64ex Foundation"), 1280, 720,
                                Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(g_window, argc, argv, RendererDesc{});

    // GPUScene init (mirrors SM64.cpp).
    GPUSceneDesc desc{};
    desc.primitiveBudget = 1024u * 1024u;
    desc.dynamicGeometryBudget = 128 * 1024u * 1024u;
    desc.dynamicStagingBudget = 128 * 1024u * 1024u;
    desc.instanceBudget = 8;
    desc.materialBudget = 8;
    desc.lightBudget = 8;
    desc.geometryBudget = 8;
    desc.tlasInstanceBudget = 8;
    GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);

    GeometryHandle captured{};
    CHECK(gpu.Allocate(kMaxVerts, kMaxVerts, captured, /*isGpu=*/false) == GPUScene::Result::Ready);

    // UpdateDynamicGeometryCPU requires the full allocation every time, so the
    // vertex buffer is kept at kMaxVerts with unused triangles degenerated.
    g_verts.assign(kMaxVerts, kDegenerateVertex);
    g_indices.resize(kMaxVerts);
    for (uint32_t i = 0; i < kMaxVerts; ++i)
        g_indices[i] = i;

    // ---- sm64ex host init (replaces libsm64 dlopen) ----
    struct SM64ExHostConfig host_cfg = {0};
    host_cfg.rendering_api = &gfx_rendering_api;
    host_cfg.wm_api = &gfx_window_manager_api;
    host_cfg.audio_api = NULL;          // NULL selects the built-in null audio backend
    host_cfg.window_title = "sm64ex Foundation";
    if (!sm64ex_host_init(&host_cfg)) {
        std::fprintf(stderr, "sm64ex_host_init failed\n");
        Examples_DestroyVulkan(g_window, ctx);
        return 1;
    }

    // sm64ex never enables backface culling, so its geometry is authored
    // double-sided; culling it here punches holes in the level.
    g_cfg.cullFlags = CullFlagsBits::Frustum;
    if (!ctx.device->GetCapabilities().raytracingInline)
        g_cfg.viewFlags &= ~ViewFlagsBits::EnableRasterRTShadows;

    // Build the render graph once up front (the resize path only rebuilds it).
    // This also fills g_cfg.renderExtent used for the camera aspect ratio.
    RebuildGraph(ctx, gpu);

    // Fixed refresh
    static constexpr uint64_t kTargetFps = 30;
    static constexpr uint64_t kFrameNs = 1'000'000'000ull / kTargetFps;
    uint64_t nextFrameNs = SDL_GetTicksNS();

    while (g_running) {
        Examples_BeginFrameInput(g_input);
        if (Examples_PollEvents(g_window, ctx, g_input))
            break;

        if (g_input.wantResizeOrRebuild) {
            g_input.wantResizeOrRebuild = false;
            RebuildGraph(ctx, gpu);
        }

        // Fixed tick; captures split into game (GPUScene) and UI (overlay buffer).
        g_captured_tris = 0;
        g_dropped_tris = false;
        g_ui_verts.clear();
        g_ui_batches.clear();
        g_ui_tris = 0;
        g_dropped_ui = false;
        sm64ex_host_frame();

        // Camera FOV decoded from the game projection matrix (authoritative;
        // tracks modes / cutscenes / shake via rsp.P_matrix). Set in
        // ra_draw_triangles_3d each frame, so no sFOVState dependency.
        g_camera.fovY = g_gameFovY;

        // Degenerate whatever last frame used and this one didn't, so stale
        // triangles stop rendering.
        for (uint32_t i = g_captured_tris * 3u; i < g_prev_tris * 3u; ++i)
            g_verts[i] = kDegenerateVertex;
        g_prev_tris = g_captured_tris;

        gpu.BeginDynamicUpdate();
        gpu.UpdateDynamicMeshCPU(captured, Span<const FQVertex>{g_verts.data(), g_verts.size()},
                                     ctx.renderer->GetFrame() == 0
                                         // Identity topology, so it only ever needs uploading once.
                                         ? Span<const uint32_t>{g_indices.data(), g_indices.size()}
                                         : Span<const uint32_t>{});
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

        CommitScene(gpu, captured);

        Examples_Text(g_input, Format("sm64ex Foundation | {:.0f} FPS | game {} tris | ui {} tris ({} batches)",
                                      g_fps.Update(), g_captured_tris, g_ui_tris, g_ui_batches.size()));
        Examples_Text(g_input, Format("FOV {:.1f} deg (sFOVState) | frame {}", degrees(g_camera.fovY),
                                      ctx.renderer->GetFrame()));
        if (g_dropped_tris)
            Examples_Text(g_input, Format("dropped game triangles past the {} budget", kMaxTris));
        if (g_dropped_ui)
            Examples_Text(g_input, Format("dropped UI triangles past the {} budget", kMaxUiTris));
        // TODO: commit g_ui_batches / g_ui_verts through a dedicated UI overlay pass.
        if (Examples_RendererSwitchButton(g_input, g_renderer))
            g_input.wantResizeOrRebuild = true;

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
    Examples_DestroyVulkan(g_window, ctx);
    wm_shutdown();
    return 0;
}
