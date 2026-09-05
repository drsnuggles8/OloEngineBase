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

// SSGI PRE-BLUR (issue #708, stage 2) — draw B of SSGIRenderPass.
//
// A small, depth- and normal-guided blur of the raw stochastic signal, run
// BEFORE the temporal resolve. Its job is not to produce a clean image — the
// resolve will do that — but to hand the resolve something stable enough to
// accumulate. Without it the history is being fed a one-sample-per-direction
// estimate whose neighbourhood clip box is itself noise, so the clip either
// rejects good history (banding on motion) or admits bad history (ghosting),
// and which of the two you get depends on the scene.
//
// Ordering, from denoisinator.md: cheap stochastic spatial → temporal → small
// cleanup spatial. The pre-blur is allowed to be sloppy because the resolve
// stabilises it; that is what lets the post-blur afterwards be tiny.
//
// WHAT IT MUST NOT DO is blur across a surface boundary, which is where all the
// contact detail lives. Every tap is weighted by its distance out of the centre
// pixel's tangent plane and by its normal agreement — see
// include/SpatialDenoise.glsl for both, and for why the geometry test is a
// plane distance and not a depth difference.
//
// The taps are integer texel fetches, not filtered samples: the guide packs an
// octahedral normal, and a bilinear tap on an octahedral encoding interpolates
// across the fold and produces a normal that points nowhere on the surface.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_StochasticSignal OLO_HEAP_TEX_2D(0)
#define u_Guide OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_StochasticSignal; // rgb = raw indirect diffuse, a = view depth
layout(binding = 1) uniform sampler2D u_Guide;            // rg = oct world normal, b = roughness, a = AO
#endif

#include "include/SpatialDenoise.glsl"

// The SAME std140 block PostProcess_SSGI.glsl declares (SSGIUBOData).
layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;
    vec4 u_ScreenParams;
    vec4 u_Flags;          // x = DebugView, y = FrameIndex, zw = pad
    vec4 u_TemporalParams;
    vec4 u_TraceParams;    // x = trace width, y = trace height, z = 1/width, w = 1/height
    vec4 u_DenoiseParams;  // x = PreBlurRadius (px), y = PostBlurMinRadius, z = PostBlurMaxRadius, w = VarianceKnee
    vec4 u_DenoiseGuide;   // x = PlaneTolerance, y = NormalPower, z = TargetHistoryLength, w = RayDistribution
};

void main()
{
    ivec2 traceSize = ivec2(max(u_TraceParams.xy, vec2(1.0)));
    ivec2 center = clamp(ivec2(gl_FragCoord.xy), ivec2(0), traceSize - 1);

    vec4 signalCenter = texelFetch(u_StochasticSignal, center, 0);
    vec4 guideCenter = texelFetch(u_Guide, center, 0);
    float centerDepth = signalCenter.a;

    float radius = u_DenoiseParams.x;
    // Sky has no surface to guide against and no GI to filter; a disabled
    // pre-blur (radius 0) is a straight copy. Alpha is carried through
    // unchanged on both paths because the resolve's disocclusion test reads it.
    if (OloDenoiseIsSky(centerDepth) || !(radius > 0.0))
    {
        o_Color = signalCenter;
        return;
    }

    vec3 centerNormalWS = OloDenoiseOctDecode(guideCenter.xy);
    vec3 centerNormalVS = normalize(mat3(u_View) * centerNormalWS);
    vec2 centerUV = (vec2(center) + 0.5) * u_TraceParams.zw;
    vec3 centerPositionVS = OloDenoiseViewPosition(u_InvProjection, centerUV, centerDepth);

    float planeTolerance = u_DenoiseGuide.x;
    float normalPower = u_DenoiseGuide.y;
    uint frameIndex = uint(max(u_Flags.y, 0.0));
    mat2 rotation = OloDenoiseDiscRotation(frameIndex, 0u);

    // The centre tap starts at full weight: it is the only sample guaranteed to
    // be on this exact surface, so a neighbourhood that rejects everything
    // still returns the pixel's own estimate rather than a division by zero.
    vec3 accumulated = signalCenter.rgb;
    float weightSum = 1.0;

    for (int i = 0; i < OLO_DENOISE_POISSON_COUNT; ++i)
    {
        vec2 offset = rotation * OLO_DENOISE_POISSON_8[i] * radius;
        ivec2 tap = center + OloDenoiseTapOffset(offset);
        // Out-of-bounds taps are DROPPED, not clamped. Clamping would fold the
        // whole off-screen half of the kernel onto the edge texel and weight
        // that one sample eight times, which reads as a bright rim.
        if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, traceSize)))
            continue;

        vec4 signalTap = texelFetch(u_StochasticSignal, tap, 0);
        if (OloDenoiseIsSky(signalTap.a))
            continue;

        vec4 guideTap = texelFetch(u_Guide, tap, 0);
        vec2 tapUV = (vec2(tap) + 0.5) * u_TraceParams.zw;
        vec3 tapPositionVS = OloDenoiseViewPosition(u_InvProjection, tapUV, signalTap.a);

        float weight = OloDenoisePlaneWeight(centerPositionVS, centerNormalVS, tapPositionVS,
                                             planeTolerance, centerDepth) *
                       OloDenoiseNormalWeight(centerNormalWS, OloDenoiseOctDecode(guideTap.xy), normalPower);
        if (!(weight > 0.0))
            continue;

        accumulated += weight * signalTap.rgb;
        weightSum += weight;
    }

    // Alpha stays the CENTRE pixel's own depth, never a weighted average of the
    // neighbourhood's: it is a geometric fact about this pixel that the resolve
    // reprojects against next frame, not a filtered quantity.
    o_Color = vec4(accumulated / weightSum, centerDepth);
}
