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
#else
layout(binding = 0) uniform sampler2D u_Texture;
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

void main()
{
    // Direction from center for radial offset
    vec2 center = vec2(0.5);
    vec2 dir = v_TexCoord - center;

    float offset = u_ChromaticAberrationIntensity;

    // Offset R and B channels radially; G stays centered
    float r = texture(u_Texture, v_TexCoord + dir * offset).r;
    float g = texture(u_Texture, v_TexCoord).g;
    float b = texture(u_Texture, v_TexCoord - dir * offset).b;

    o_Color = vec4(r, g, b, 1.0);
}
