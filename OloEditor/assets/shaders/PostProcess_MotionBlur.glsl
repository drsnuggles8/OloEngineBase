#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead. OLO_VULKAN is
// defined only on the Vulkan shaderc route; the GL branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

// Texture inputs. Under heap-bindless (issue #691 Phase 3) these become heap
// lookups keyed by the SAME slot numbers the bindful branch declares, so the two
// variants cannot disagree about which texture is which — and the shader BODY
// below is unchanged between them. Inert without OLO_BINDLESS; the engine only
// defines it on the raw-GLSL compile route.
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#define u_Velocity OLO_HEAP_TEX_2D(2)
#define u_DepthTexture OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_Texture;      // Scene color (HDR)
layout(binding = 2) uniform sampler2D u_Velocity;     // Per-pixel screen-space velocity (RG16F, curr-prev in UV) — Deferred G-Buffer RT3 / Forward scene attachment 3
layout(binding = 19) uniform sampler2D u_DepthTexture;  // Scene depth
#endif

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;


layout(std140, binding = 7) uniform PostProcessUBO
{
    int   u_TonemapOperator;
    float u_Exposure;
    float u_Gamma;
    float u_BloomThreshold;

    float u_BloomIntensity;
    float u_VignetteIntensity;
    float u_VignetteSmoothness;
    float u_ChromaticAberrationIntensity;

    float u_DOFFocusDistance;
    float u_DOFFocusRange;
    float u_DOFBokehRadius;
    float u_MotionBlurStrength;

    int   u_MotionBlurSamples;
    float u_InverseScreenWidth;
    float u_InverseScreenHeight;
    float _padding0;

    float u_TexelSizeX;
    float u_TexelSizeY;
    float u_Near;
    float u_Far;
};

// Motion blur matrices (UBO binding 8) — camera-only velocity reconstruction
// for background pixels and the no-velocity-buffer fallback.
layout(std140, binding = 8) uniform MotionBlurUBO
{
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// Motion-blur per-pass flags (UBO binding 42). x = hasVelocityTexture (0/1):
// when 1, geometry pixels read full per-object+camera motion from u_Velocity;
// when 0 (no velocity buffer for this path), every pixel falls back to the
// camera-only reconstruction below.
layout(std140, binding = 42) uniform MotionBlurParams
{
    vec4 u_MB_Params;
};
#define u_HasVelocityTexture (int(u_MB_Params.x))

// Camera-only velocity from depth + previous view-projection. Covers
// background/sky pixels (which geometry never writes into the velocity
// buffer) and the Forward fallback when no velocity buffer exists.
vec2 ReconstructCameraVelocity(vec2 uv, float depth)
{
    vec4 ndcPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos = u_InverseViewProjection * ndcPos;
    worldPos /= worldPos.w;
    vec4 prevClipPos = u_PrevViewProjection * worldPos;
    if (prevClipPos.w <= 0.0001)
        return vec2(0.0);
    vec2 prevUV = (prevClipPos.xy / prevClipPos.w) * 0.5 + 0.5;
    return uv - prevUV; // current - prev (matches the sign convention of the velocity buffer)
}

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // Per-pixel velocity (curr.xy/w - prev.xy/w, halved to UV units) already
    // encodes full camera + object motion, so geometry pixels need no extra
    // reconstruction. Background pixels (depth == far) are never written into
    // the velocity buffer, so reconstruct camera motion there to keep the sky
    // streaking under camera movement just like the legacy camera-only path.
    vec2 velocity;
    if (u_HasVelocityTexture != 0 && depth < 1.0)
        velocity = texture(u_Velocity, v_TexCoord).rg;
    else
        velocity = ReconstructCameraVelocity(v_TexCoord, depth);

    velocity *= u_MotionBlurStrength;

    // Clamp velocity to prevent excessive blur
    float speed = length(velocity / vec2(u_InverseScreenWidth, u_InverseScreenHeight));
    float maxSpeed = 40.0; // max pixels of blur
    if (speed > maxSpeed)
    {
        velocity *= maxSpeed / speed;
    }

    // Accumulate samples along the velocity vector
    vec3 result = texture(u_Texture, v_TexCoord).rgb;
    float totalWeight = 1.0;

    int numSamples = u_MotionBlurSamples;
    for (int i = 1; i < numSamples; i++)
    {
        float t = float(i) / float(numSamples - 1) - 0.5; // [-0.5, 0.5]
        vec2 sampleUV = v_TexCoord + velocity * t;
        sampleUV = clamp(sampleUV, vec2(0.0), vec2(1.0));
        result += texture(u_Texture, sampleUV).rgb;
        totalWeight += 1.0;
    }

    result /= totalWeight;
    o_Color = vec4(result, 1.0);
}
