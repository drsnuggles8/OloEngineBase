#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). The BODY is
// byte-identical between the two variants — only the declaration moves, and it
// names the same binding number the pass binds with.
#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_BloomColor OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_SceneColor;  // Scene HDR color
layout(binding = 1) uniform sampler2D u_BloomColor;  // Bloom result
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

void main()
{
    vec3 sceneColor = texture(u_SceneColor, v_TexCoord).rgb;
    vec3 bloomColor = texture(u_BloomColor, v_TexCoord).rgb;

    // Additive blend with intensity control
    vec3 result = sceneColor + bloomColor * u_BloomIntensity;

    o_Color = vec4(result, 1.0);
}
