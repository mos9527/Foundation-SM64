// Foundation-renderer host for sm64ex (scaffold).
//
// The Foundation/Vulkan/GPUScene setup below mirrors Examples/Renderer/SM64/
// SM64.cpp (the libsm64 demo) so it is known-good against the current
// Foundation API; the only structural change is that we *statically link* the
// sm64ex host library (src/pc/host) and feed it our own GfxRenderingAPI /
// GfxWindowManagerAPI backends, instead of dlopen-ing libsm64.
//
// Implemented here:
//   - Full Foundation init (SDL3 + Vulkan + GPUScene + Rasterizer)   [from Example]
//   - GfxWindowManagerAPI over Foundation's bundled SDL3               [complete]
//   - GfxRenderingAPI that *captures* the triangle stream gfx_pc emits
//     (texture / shader / state bookkeeping + a staging vertex buffer)
//
// TODO (the actual "full port", i.e. route B from the original analysis):
//   - draw_triangles -> GPUScene geometry rebuild: split static level geometry
//     (cache/upload once by display-list hash) from dynamic actor geometry
//     (per-frame update / BLAS refit), parse the real gfx_pc VBO stride, and
//     map N64 combiner/material -> Foundation PBR (GSMaterial), baking SHADE.
//   - Per-frame GPUScene update + CommitScene with real materials / GSLight.
//   - Host audio backend + controller input mapping (SDL gamepad/keyboard).

#include <SDL3/SDL.h>

#include "sm64ex_host.h"          // sm64ex host embedding interface (PUBLIC include)
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"

#include <Core/Allocator.hpp>     // GLOBAL_ALLOC
#include <Renderer/Renderer.hpp>
#include <Renderer/GPUScene.hpp>
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

// Orbit camera ported from Examples/Renderer/SM64/SM64.cpp's FMarioCamera.
// yaw/radius/height -> position + forward, then RH-reverse-Z view/proj matrices.
struct FMarioCamera
{
    float3 center{};
    float yaw = 0.0f;
    float radius = 1000.0f;
    float height = 200.0f;
    float fovY = radians(45.0f);
    float zNear = 1.0f;
    float aspect = 1.0f;
    mat4 view{};
    mat4 proj{};
    float3 position{};
    float3 forward{};

    void RefreshMatrices()
    {
        position = center + float3(radius * cosf(yaw), height, radius * sinf(yaw));
        forward = normalize(center - position);
        const float3 backward = -forward; // camera +Z (outward)
        const float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), backward));
        const float3 up = cross(backward, right);
        const quat rot = quat_cast(mat3{right, up, backward});
        view = viewMatrixRHReverseZ(position, rot);
        proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
    }
};
static FMarioCamera g_camera;

// ---------------------------------------------------------------------------
// Captured game state  (TODO: push into GPUScene)
// ---------------------------------------------------------------------------
static std::vector<float>                         g_staging_vbo;  // raw floats from gfx_pc
static size_t                                     g_captured_tris = 0;
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_textures; // id -> rgba32
static std::unordered_map<uint32_t, struct ShaderProgram*> g_shaders; // id -> dummy sentinel
static uint32_t g_cur_tex[2] = {0, 0};

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
static void ra_unload_shader(struct ShaderProgram*) {}
static void ra_load_shader(struct ShaderProgram*) {}
static struct ShaderProgram* ra_create_and_load_new_shader(uint32_t id) {
    // We do not compile N64 combiner GLSL; keep a stable dummy keyed by id so
    // lookup_shader returns something non-null.
    auto it = g_shaders.find(id);
    if (it != g_shaders.end())
        return it->second;
    struct ShaderProgram* p = (struct ShaderProgram*)(uintptr_t)(id | 0x1u);
    g_shaders[id] = p;
    return p;
}
static struct ShaderProgram* ra_lookup_shader(uint32_t id) {
    auto it = g_shaders.find(id);
    return it == g_shaders.end() ? nullptr : it->second;
}
static void ra_shader_get_info(struct ShaderProgram*, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = 3;            // position + vertex color + uv
    used_textures[0] = true;
    used_textures[1] = false;
}
static uint32_t ra_new_texture(void) {
    static uint32_t next = 1;
    return next++;
}
static void ra_select_texture(int tile, uint32_t id) {
    if (tile >= 0 && tile < 2)
        g_cur_tex[tile] = id;
}
static void ra_upload_texture(const uint8_t* rgba32, int w, int h) {
    // TODO: associate with g_cur_tex[0] and upload to GPUScene as the
    //       GSMaterial baseColorTexture (by rgba32 data hash for de-dup).
    std::vector<uint8_t> buf(rgba32, rgba32 + (size_t)w * (size_t)h * 4);
    g_textures[g_cur_tex[0]] = std::move(buf);
}
static void ra_set_sampler_parameters(int, bool, uint32_t, uint32_t) {}
static void ra_set_depth_test(bool) {}
static void ra_set_depth_mask(bool) {}
static void ra_set_zmode_decal(bool) {}
static void ra_set_viewport(int, int, int, int) {}
static void ra_set_scissor(int, int, int, int) {}
static void ra_set_use_alpha(bool) {}
static void ra_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    // TODO (the core of the port): parse the interleaved VBO emitted by gfx_pc
    // -- verify stride against gfx_pc.c (expect pos[3] col[4] uv[2], optional
    // normal[3]) -- and push into GPUScene, splitting static level geometry
    // (cache/upload once, keyed by display-list hash) from dynamic actor
    // geometry (per-frame update). For the scaffold we only stage the raw
    // floats and count triangles.
    (void)buf_vbo_len;
    g_staging_vbo.insert(g_staging_vbo.end(), buf_vbo, buf_vbo + buf_vbo_len);
    g_captured_tris += num_tris;
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

static void CommitScene(GPUScene& gpu, GeometryHandle mario) {
    auto tables = gpu.BeginScene(2, 2, 2);
    tables.materials[0] = GSMaterial{};
    tables.materials[0].baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
    tables.materials[0].metallicFactor = 0.0f;
    tables.materials[0].roughnessFactor = 0.75f;
    tables.materials[0].ior = 1.5f;
    tables.materials[1] = tables.materials[0];
    tables.instances[0] = GSInstance{
        .transform = float3(0.0f, 0.0f, 0.0f),
        .rotation = quat(0.0f, 0.0f, 0.0f, 1.0f),
        .scale = float3(1.0f, 1.0f, 1.0f),
        .materialIndex = 1,
        .resourceIndex = mario.index,
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
    desc.dynamicGeometryBudget = 1024u * 1024u;
    desc.dynamicStagingBudget = 1024u * 1024u;
    desc.instanceBudget = 8;
    desc.materialBudget = 8;
    desc.lightBudget = 8;
    desc.geometryBudget = 8;
    desc.tlasInstanceBudget = 8;
    GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);

    constexpr uint32_t kMaxTris = 1u << 16;
    constexpr uint32_t kMaxVerts = kMaxTris * 3u;
    constexpr uint32_t kMaxIdx = kMaxVerts;
    GeometryHandle mario{};
    (void)gpu.Allocate(kMaxVerts, kMaxIdx, mario, false);

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

    g_cfg.cullFlags = CullFlagsBits::Frustum | CullFlagsBits::Backface;
    if (!ctx.device->GetCapabilities().raytracingInline)
        g_cfg.viewFlags &= ~ViewFlagsBits::EnableRasterRTShadows;

    // Camera defaults: SM64 Mario lives around y~1000.
    g_camera.radius = 1000.0f;
    g_camera.center = float3(0.0f, 300.0f, 0.0f);

    // Build the render graph once up front (the resize path only rebuilds it).
    // This also fills g_cfg.renderExtent used for the camera aspect ratio.
    RebuildGraph(ctx, gpu);

    uint64_t t0 = SDL_GetTicksNS();
    while (g_running) {
        uint64_t t1 = SDL_GetTicksNS();
        float dt = static_cast<float>(t1 - t0) / 1e9f;
        t0 = t1;

        Examples_BeginFrameInput(g_input);
        if (Examples_PollEvents(g_window, ctx, g_input))
            break;

        if (g_input.wantResizeOrRebuild) {
            g_input.wantResizeOrRebuild = false;
            RebuildGraph(ctx, gpu);
        }

        // 30 Hz game tick; our GfxRenderingAPI::draw_triangles captures the
        // triangle stream (TODO: route into GPUScene).
        sm64ex_host_frame();

        // TODO: parse g_staging_vbo, split static/dynamic, upload to GPUScene.
        g_staging_vbo.clear();
        g_captured_tris = 0;

        // Orbit with the arrow keys (ported from SM64.cpp's GatherInputs).
        float camYawDelta = 0.0f;
        const bool* kbd = SDL_GetKeyboardState(NULL);
        if (kbd[SDL_SCANCODE_LEFT])  camYawDelta -= 1.0f;
        if (kbd[SDL_SCANCODE_RIGHT]) camYawDelta += 1.0f;
        g_camera.yaw += camYawDelta * dt * 2.0f;
        if (camYawDelta != 0.0f)
            g_ubo.ptAccumulatedFrames = 0u;
        g_camera.center = float3(0.0f, 300.0f, 0.0f);
        g_camera.aspect = static_cast<float>(g_cfg.renderExtent.x) /
                          static_cast<float>(g_cfg.renderExtent.y);
        g_camera.RefreshMatrices();
        UpdateRendererCameraUBO(g_ubo, ctx.renderer->GetFrame(), g_camera.view, g_camera.proj);
        g_ubo.zNear = g_camera.zNear;
        g_ubo.projPlanes = planeSymmetric(g_camera.proj);
        g_ubo.camPosition = float4(g_camera.position, 0.0f);
        g_ubo.camDirection = float4(g_camera.forward, 0.0f);
        g_ubo.dbgViewFlags = g_cfg.viewFlags;
        g_ubo.dbgMaterialFlags = g_cfg.materialFlags;

        CommitScene(gpu, mario);

        if (Examples_RendererSwitchButton(g_input, g_renderer))
            g_input.wantResizeOrRebuild = true;

        Examples_NewFrame(g_window, ctx);
    }

    sm64ex_host_deinit();
    Examples_DestroyVulkan(g_window, ctx);
    wm_shutdown();
    return 0;
}
