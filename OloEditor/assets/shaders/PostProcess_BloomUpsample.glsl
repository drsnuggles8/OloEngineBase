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

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). The BODY is
// byte-identical between the two variants — only the declaration moves, and it
// names the same binding number the pass binds with.
#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#else
layout(binding = 0) uniform sampler2D u_Texture;
#endif

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

// 9-tap tent filter upsample
void main()
{
    vec2 uv = v_TexCoord;
    vec2 d = vec2(u_TexelSizeX, u_TexelSizeY);

    // 3x3 tent filter
    vec3 result = vec3(0.0);
    result += texture(u_Texture, uv + vec2(-1.0, -1.0) * d).rgb * 1.0;
    result += texture(u_Texture, uv + vec2( 0.0, -1.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv + vec2( 1.0, -1.0) * d).rgb * 1.0;
    result += texture(u_Texture, uv + vec2(-1.0,  0.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv).rgb * 4.0;
    result += texture(u_Texture, uv + vec2( 1.0,  0.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv + vec2(-1.0,  1.0) * d).rgb * 1.0;
    result += texture(u_Texture, uv + vec2( 0.0,  1.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv + vec2( 1.0,  1.0) * d).rgb * 1.0;
    result /= 16.0;

    o_Color = vec4(result, 1.0);
}
