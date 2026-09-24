#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
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

// Texture inputs. Under heap-bindless (issue #691) these become heap
// lookups keyed by the SAME slot numbers the bindful branch declares, so the two
// variants cannot disagree about which texture is which — and the shader BODY
// below is unchanged between them. Inert without OLO_BINDLESS; the engine only
// defines it on the raw-GLSL compile route.
#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#define u_SSAOTexture OLO_HEAP_TEX_2D(20) // TEX_SSAO
#define u_DepthTexture OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_Texture;
layout(binding = 20) uniform sampler2D u_SSAOTexture;
layout(binding = 19) uniform sampler2D u_DepthTexture;
#endif

// SSAO Apply — Modulates scene color by the SSAO occlusion factor.
// Uses depth-aware bilateral upsampling to prevent half-res AO from bleeding
// across depth discontinuities (which causes blurry dark halos).

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Scene HDR color (ping-pong source)

// Blurred SSAO result (R channel = AO value, 0 = full occlusion, 1 = no occlusion)
// This texture is at HALF resolution.

// Full-res scene depth for bilateral upsampling

// SSAO UBO (binding 9) — we read intensity and debug flag from here
layout(std140, binding = 9) uniform SSAOUBO
{
    float u_Radius;
    float u_Bias;
    float u_Intensity;
    int   u_Samples;

    int   u_ScreenWidth;
    int   u_ScreenHeight;
    int   u_DebugView;
    float _pad1;

    mat4  u_Projection;
    mat4  u_InverseProjection;
};

// The upsample and the strength mix live in the shared include (issue
// #1336), because DeferredLighting now applies the SAME value to the ambient
// term alone. This pass is the FORWARD paths' consumer; see the include's
// header for why the two differ.
#define OLO_SSAO_TAP_DEPTH(uv) texture(u_DepthTexture, (uv)).r
#include "include/ScreenSpaceAOSampling.glsl"

void main()
{
    vec3 sceneColor = texture(u_Texture, v_TexCoord).rgb;

    float ao = oloSampleScreenSpaceAO(u_SSAOTexture, v_TexCoord, u_Projection[2][2], u_Projection[3][2]);

    if (u_DebugView != 0)
    {
        o_Color = vec4(vec3(ao), 1.0);
        return;
    }

    // Mix between full color and AO-modulated color based on intensity
    vec3 result = sceneColor * oloScreenSpaceAOVisibility(ao, u_Intensity);

    o_Color = vec4(result, 1.0);
}
