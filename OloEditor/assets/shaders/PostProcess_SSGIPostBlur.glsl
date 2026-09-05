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

// SSGI POST-BLUR (issue #708, stage 4) — draw D of SSGIRenderPass.
//
// The cleanup filter, run AFTER the temporal resolve, with a radius that varies
// per pixel: wide where the estimate is still noisy, narrow where it converged.
// Because the image reaching it has already been pre-blurred and temporally
// integrated, this filter can be small — that is the whole payoff of the
// spatial → temporal → spatial ordering, against a temporal-then-spatial design
// which needs one expensive wide filter to hide everything at once.
//
// TWO SIGNALS DRIVE THE RADIUS, and the wider of the two wins:
//
//   * the accumulated luminance VARIANCE the resolve wrote into
//     MomentsSecond.a — relative to the mean, so exposure cannot move it;
//   * the accumulated HISTORY LENGTH in MomentsFirst.a — a freshly disoccluded
//     pixel has no history for its variance estimate to be trustworthy, and its
//     true error is at its worst precisely there.
//
// `max` and not a product: either reason alone is sufficient grounds to widen,
// and multiplying them would let a converged-but-noisy region cancel against a
// fresh-but-flat one. See OloDenoisePostBlurRadius.
//
// Widening HERE rather than in the resolve is deliberate — the post-blur's
// output is not what gets accumulated, so a temporary widening on disocclusion
// hides the noise without polluting the history with an over-smoothed value
// that would then take the full history length to wash out.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_ResolvedSignal OLO_HEAP_TEX_2D(0)
#define u_Guide OLO_HEAP_TEX_2D(1)
#define u_MomentsFirst OLO_HEAP_TEX_2D(2)
#define u_MomentsSecond OLO_HEAP_TEX_2D(3)
#else
layout(binding = 0) uniform sampler2D u_ResolvedSignal; // rgb = resolved indirect diffuse, a = view depth
layout(binding = 1) uniform sampler2D u_Guide;          // rg = oct world normal, b = roughness, a = AO
layout(binding = 2) uniform sampler2D u_MomentsFirst;   // rgb = first moments, a = history length
layout(binding = 3) uniform sampler2D u_MomentsSecond;  // rgb = second moments, a = luminance variance
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

const vec3 LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

void main()
{
    ivec2 traceSize = ivec2(max(u_TraceParams.xy, vec2(1.0)));
    ivec2 center = clamp(ivec2(gl_FragCoord.xy), ivec2(0), traceSize - 1);

    vec4 resolvedCenter = texelFetch(u_ResolvedSignal, center, 0);
    float centerDepth = resolvedCenter.a;

    float maxRadius = u_DenoiseParams.z;
    if (OloDenoiseIsSky(centerDepth) || !(maxRadius > 0.0))
    {
        o_Color = resolvedCenter;
        return;
    }

    // SMOOTH THE GUIDE BEFORE USING IT, over a 3x3 neighbourhood. This is the
    // guard denoisinator.md's objection to variance guiding calls for: a radius
    // read from a single noisy variance texel varies pixel-to-pixel, so
    // neighbouring pixels filter at different widths and the result is
    // low-frequency mottling — noise traded for blotches rather than removed.
    // It was visible on a flat lit surface in the first version of this shader.
    //
    // Variance takes the MEAN (an average of nine estimates of the same
    // quantity is a better estimate) and history length takes the MINIMUM
    // (a pixel next to a disocclusion should widen with it, not be pulled
    // narrow by its converged neighbours — the conservative direction).
    float historyLength = 1.0e30;
    float variance = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 tap = clamp(center + ivec2(x, y), ivec2(0), traceSize - 1);
            historyLength = min(historyLength, texelFetch(u_MomentsFirst, tap, 0).a);
            variance += texelFetch(u_MomentsSecond, tap, 0).a;
        }
    }
    variance *= 1.0 / 9.0;
    float mean = dot(max(resolvedCenter.rgb, vec3(0.0)), LUMINANCE_WEIGHTS);
    float radius = OloDenoisePostBlurRadius(variance, mean, historyLength,
                                            u_DenoiseParams.y, maxRadius,
                                            u_DenoiseParams.w, u_DenoiseGuide.z);
    if (!(radius > 0.0))
    {
        o_Color = resolvedCenter;
        return;
    }

    vec4 guideCenter = texelFetch(u_Guide, center, 0);
    vec3 centerNormalWS = OloDenoiseOctDecode(guideCenter.xy);
    vec3 centerNormalVS = normalize(mat3(u_View) * centerNormalWS);
    vec2 centerUV = (vec2(center) + 0.5) * u_TraceParams.zw;
    vec3 centerPositionVS = OloDenoiseViewPosition(u_InvProjection, centerUV, centerDepth);

    float planeTolerance = u_DenoiseGuide.x;
    float normalPower = u_DenoiseGuide.y;
    uint frameIndex = uint(max(u_Flags.y, 0.0));
    // Stage 1, so the post-blur's disc is rotated to a different angle than the
    // pre-blur's in the same frame — filtering twice through the same eight
    // directions would deepen that pattern instead of covering the disc.
    mat2 rotation = OloDenoiseDiscRotation(frameIndex, 1u);

    vec3 accumulated = resolvedCenter.rgb;
    float weightSum = 1.0;

    for (int i = 0; i < OLO_DENOISE_POISSON_COUNT; ++i)
    {
        vec2 offset = rotation * OLO_DENOISE_POISSON_8[i] * radius;
        ivec2 tap = center + OloDenoiseTapOffset(offset);
        if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, traceSize)))
            continue;

        vec4 resolvedTap = texelFetch(u_ResolvedSignal, tap, 0);
        if (OloDenoiseIsSky(resolvedTap.a))
            continue;

        vec4 guideTap = texelFetch(u_Guide, tap, 0);
        vec2 tapUV = (vec2(tap) + 0.5) * u_TraceParams.zw;
        vec3 tapPositionVS = OloDenoiseViewPosition(u_InvProjection, tapUV, resolvedTap.a);

        float weight = OloDenoisePlaneWeight(centerPositionVS, centerNormalVS, tapPositionVS,
                                             planeTolerance, centerDepth) *
                       OloDenoiseNormalWeight(centerNormalWS, OloDenoiseOctDecode(guideTap.xy), normalPower);
        if (!(weight > 0.0))
            continue;

        accumulated += weight * resolvedTap.rgb;
        weightSum += weight;
    }

    // Indirect diffuse is a non-negative radiance; alpha carries this pixel's
    // own view depth on to the guided upscale, which needs it to reject
    // half-resolution taps that belong to a different surface.
    o_Color = vec4(max(accumulated / weightSum, vec3(0.0)), centerDepth);
}
