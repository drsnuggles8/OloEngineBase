// The display encode (issue #1353): composition -> swapchain. Runs once per
// presented frame in an HDR output mode, after the frame and the UI have been
// composed into the context's RGBA16F composition image (VulkanContext). In
// SDR there is no composition image and this pass never runs.
#type vertex
#version 460 core

#ifdef OLO_VULKAN
// Vertex pulling, as FullscreenBlit.glsl (binding 57, OloVertexPull).
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
#include "include/DisplayEncode.glsl"

#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#else
layout(binding = 0) uniform sampler2D u_Texture;
#endif

// PostProcessUBO (binding 7), the display-output tail of PostProcessUBOData.
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
    float u_DitherAmplitude;

    float u_TexelSizeX;
    float u_TexelSizeY;
    float u_Near;
    float u_Far;

    int   u_DisplayMode;
    float u_DisplayPaperWhiteNits;
    float u_DisplayUIWhiteNits;
    float u_DisplayPeakNits;

    float u_DisplayMinNits;
    float _ppDisplayPad0;
    float _ppDisplayPad1;
    float _ppDisplayPad2;
};

void main()
{
    // One composition texel per swapchain pixel: the two images share an
    // extent, so a fetch is exact where a filtered sample would blend.
    vec3 composition = texelFetch(u_Texture, ivec2(gl_FragCoord.xy), 0).rgb;
    o_Color = vec4(oloEncodeCompositionForDisplay(composition, u_DisplayMode, u_Gamma, u_DisplayUIWhiteNits,
                                                  u_DisplayPeakNits),
                   1.0);
}
