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

// SSR PRE-BLUR (issue #708, stage 2) — draw B of SSRRenderPass.
//
// The same guided Poisson-disc filter the SSGI pre-blur runs, with ONE
// difference that is the whole story of porting a diffuse denoiser to specular:
// the radius comes from ROUGHNESS, not from noise.
//
// A mirror carries the sharpest detail in the frame. Filtering it by how noisy
// it looks would blur exactly the reflections that must stay sharp, because a
// reflected high-contrast edge is legitimately high-variance. What sets the
// right filter width for a reflection is the width of the specular lobe the
// single VNDF sample was drawn from, and that is roughness — so a near-mirror
// gets radius 0 and is left completely alone. See OloDenoiseRoughnessRadius.
//
// It is still worth running for the rough half of the roughness range, which is
// where #902 raised SSRMaxRoughness to 0.8 and where one VNDF sample per pixel
// is at its noisiest.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_StochasticSignal OLO_HEAP_TEX_2D(0)
#define u_Guide OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_StochasticSignal; // rgb = reflection delta, a = view depth
layout(binding = 1) uniform sampler2D u_Guide;            // rg = oct world normal, b = roughness, a = AO
#endif

#include "include/SpatialDenoise.glsl"

// The SAME std140 block PostProcess_SSR.glsl declares (SSRUBOData).
layout(std140, binding = 38) uniform SSRParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;        // x = DebugView, y = FrameIndex, zw = pad
    vec4 u_HZBParams;
    vec4 u_TemporalParams;
    vec4 u_DenoiseParams; // x = PreBlurRadius (px), y = unused, z = PostBlurMaxRadius, w = unused
    vec4 u_DenoiseGuide;  // x = PlaneTolerance, y = NormalPower, z = RoughnessKnee, w = MaxRoughness (the trace's cutoff)
};

void main()
{
    ivec2 screenSize = ivec2(max(u_ScreenParams.xy, vec2(1.0)));
    ivec2 center = clamp(ivec2(gl_FragCoord.xy), ivec2(0), screenSize - 1);

    vec4 signalCenter = texelFetch(u_StochasticSignal, center, 0);
    vec4 guideCenter = texelFetch(u_Guide, center, 0);
    float centerDepth = signalCenter.a;

    float radius = OloDenoiseRoughnessRadius(guideCenter.z, u_DenoiseParams.x, u_DenoiseGuide.z);
    if (OloDenoiseIsSky(centerDepth) || !(radius > 0.0))
    {
        o_Color = signalCenter;
        return;
    }

    vec3 centerNormalWS = OloDenoiseOctDecode(guideCenter.xy);
    vec3 centerNormalVS = normalize(mat3(u_View) * centerNormalWS);
    vec2 centerUV = (vec2(center) + 0.5) * u_ScreenParams.zw;
    vec3 centerPositionVS = OloDenoiseViewPosition(u_InvProjection, centerUV, centerDepth);

    float planeTolerance = u_DenoiseGuide.x;
    float normalPower = u_DenoiseGuide.y;
    float maxRoughness = u_DenoiseGuide.w;
    uint frameIndex = uint(max(u_Flags.y, 0.0));
    mat2 rotation = OloDenoiseDiscRotation(frameIndex, 0u);

    vec3 accumulated = signalCenter.rgb;
    float weightSum = 1.0;

    for (int i = 0; i < OLO_DENOISE_POISSON_COUNT; ++i)
    {
        vec2 offset = rotation * OLO_DENOISE_POISSON_8[i] * radius;
        ivec2 tap = center + OloDenoiseTapOffset(offset);
        if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, screenSize)))
            continue;

        vec4 signalTap = texelFetch(u_StochasticSignal, tap, 0);
        if (OloDenoiseIsSky(signalTap.a))
            continue;

        vec4 guideTap = texelFetch(u_Guide, tap, 0);
        vec2 tapUV = (vec2(tap) + 0.5) * u_ScreenParams.zw;
        vec3 tapPositionVS = OloDenoiseViewPosition(u_InvProjection, tapUV, signalTap.a);

        // The roughness term is what a purely geometric filter gets wrong here:
        // the trace early-outs to a ZERO delta above its roughness cutoff while
        // still writing a valid depth and normal, so without this a rough patch
        // beside a polished one averages in zeros and leaves a dark band along
        // the seam. See OloDenoiseSpecularTapWeight.
        float weight = OloDenoisePlaneWeight(centerPositionVS, centerNormalVS, tapPositionVS,
                                             planeTolerance, centerDepth) *
                       OloDenoiseNormalWeight(centerNormalWS, OloDenoiseOctDecode(guideTap.xy), normalPower) *
                       OloDenoiseSpecularTapWeight(guideCenter.z, guideTap.z, maxRoughness);
        if (!(weight > 0.0))
            continue;

        accumulated += weight * signalTap.rgb;
        weightSum += weight;
    }

    // NOT clamped to non-negative: the SSR signal is a signed DELTA, and a
    // reflection darker than the surface it replaces is a legitimate negative.
    // Alpha stays the CENTRE pixel's own view depth, never a filtered average —
    // it is a geometric fact about this pixel that the resolve reprojects
    // against next frame.
    o_Color = vec4(accumulated / weightSum, centerDepth);
}
