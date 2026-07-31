// SM64 screen-space overlay shader (skybox + UI passes).
//
// gfx_pc hands us vertices that are already in the N64/OpenGL clip space its
// own matrix stack produced: +Y up and z in [-w, w]. The game matrices are kept
// as-is: the host flips Y through a negative-height viewport (SetViewport with
// flipY), so the only fixup left here is remapping z into Foundation's
// reverse-Z range (near = 1, far = 0).
//
// The pass is fed by a single interleaved vertex buffer (see UiVertex in
// sm64_foundation_main.cpp, 44 bytes/vertex) and samples straight out of the
// GPUScene bindless texture pool, so no per-batch descriptor churn is needed.

// GPUScene's bindless Texture2D pool. Must match IBindless.slang's set/binding.
[[vk::binding(0, 1)]] // [binding, set]
Texture2D<float4> gTextures2D[];

SamplerState textureSampler;

// zMode selects how SV_Position.z is produced.
static const uint kZModeKeep = 0u;  // remap the game's z into reverse-Z
static const uint kZModeFar = 1u;   // pin to the reverse-Z far plane (skybox)

struct PushConstants
{
    uint zMode;
    float alphaRef; // fragments with alpha <= alphaRef are discarded
};

[[vk::push_constant]] PushConstants pc;

struct Vertex
{
    float4 pos : SV_Position;
    float2 uv;
    float4 color;
    nointerpolation int texId;
};

[shader("vertex")]
Vertex vertMain(
    float4 pos   : POSITION,   // location 0, clip-space xyzw
    float2 uv    : TEXCOORD0,  // location 1
    float4 color : COLOR,      // location 2
    float  texId : TEXCOORD1)  // location 3, < 0 means untextured
{
    Vertex output;

    // GL clip space z in [-w, w] -> Vulkan [0, w], then reversed so that the
    // near plane maps to 1 and the far plane to 0 like the rest of the frame.
    float z = (pc.zMode == kZModeFar) ? 0.0f : (pos.w - pos.z) * 0.5f;
    // Title/Advertise
    if (pos.z >= 0.5f && pc.zMode == kZModeFar) z = 1.0f;

    output.pos = float4(pos.x, pos.y, z, pos.w);
    output.uv = uv;
    output.color = color;
    output.texId = int(texId);
    return output;
}

[shader("fragment")]
float4 fragMain(Vertex vtx) : SV_Target
{
    float4 color = vtx.color;
    if (vtx.texId >= 0)
        color *= gTextures2D[NonUniformResourceIndex(uint(vtx.texId))].Sample(textureSampler, vtx.uv);
    else
        color *= 0.25f;
    if (color.a <= pc.alphaRef)
        discard;
    return color;
}
