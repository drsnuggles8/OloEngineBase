#type vertex
#version 460 core

#ifdef OLO_VULKAN
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

// =============================================================================
// RayTracedShadowResolve.glsl — draw B of RayTracedShadowPass. Issue #1056.
//
// Temporal accumulation of the one-sample-per-pixel visibility signal draw A
// produced. This is what turns one stochastic ray into a smooth penumbra: the
// trace is deliberately cheap and noisy, and the denoiser is where the quality
// comes from.
//
// IT DENOISES ON THE SUBSTRATE THAT ALREADY EXISTS, not a bespoke one.
// include/SurfaceHistory.glsl answers "is last frame's sample the same
// surface?" and include/TemporalResolve.glsl answers "how much of it do I
// keep?" — the same two files SSGI's and SSR's resolves use, so a fix to
// either reaches all three. In particular the hit-distance lane that
// SurfaceHistoryRecord reserved "for a future ray hit" is now used for what it
// was reserved for: a shadow ray's blocker distance separates two samples that
// agree on depth and normal but disagree about WHICH occluder they found,
// which is exactly a shadow silhouette sweeping across a flat wall.
// OLO_SURFACE_TEST_HIT_DISTANCE is the test for it, and this is its first
// consumer.
//
// OUTPUTS
//   0  RGBA16F  the resolved visibility, one channel per ray-traced light.
//   1  RGBA16F  x = first moment, y = second moment (of the summary below),
//               z = this frame's positive view depth, w = this frame's blocker
//               distance summary.
//
// ONE SCALAR MOMENT PAIR FOR FOUR CHANNELS, stated because it is a real
// simplification and not an oversight: the moments drive the spatial filter's
// RADIUS, and all four channels of a pixel describe the SAME surface, so they
// disocclude together and converge together. Per-channel variance would need
// two more attachments to answer a question whose answer is the same four
// times. The summary is the mean over the ACTIVE channels; a light whose
// penumbra is still noisy while its neighbours have converged therefore gets a
// slightly narrower filter than it wants, which is the trade.
//
// The view depth in z is the same trick SSGI's resolve uses and exists for the
// same reason: there is no depth HISTORY buffer, and a fixed tolerance on
// device depth would be centimetres near the camera and kilometres far.
// =============================================================================

layout(location = 0) out vec4 o_Visibility;
layout(location = 1) out vec4 o_Moments;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_StochasticSignal OLO_HEAP_TEX_2D(0)
#define u_History OLO_HEAP_TEX_2D(1)
#define u_SurfaceHistory OLO_HEAP_TEX_2D(2)
#define u_RayTracedShadowMomentsHistory OLO_HEAP_TEX_2D(3)
#define u_RayTracedShadowHitDistance OLO_HEAP_TEX_2D(4)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)  // TEX_POSTPROCESS_DEPTH
#define u_GBufferNormal OLO_HEAP_TEX_2D(44) // TEX_GBUFFER_NORMAL
#define u_GVelocity OLO_HEAP_TEX_2D(46)     // TEX_GBUFFER_VELOCITY
#else
layout(binding = 0) uniform sampler2D u_StochasticSignal; // this frame's raw per-light visibility
layout(binding = 1) uniform sampler2D u_History;          // last frame's resolved visibility
layout(binding = 2) uniform sampler2D u_SurfaceHistory;   // last frame's oct normal + roughness + AO
layout(binding = 3) uniform sampler2D u_RayTracedShadowMomentsHistory; // last frame's moments + view depth + blocker distance
layout(binding = 4) uniform sampler2D u_RayTracedShadowHitDistance;    // this frame's per-channel blocker distance
layout(binding = 19) uniform sampler2D u_DepthTexture;    // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal;   // current oct normal + roughness + AO
layout(binding = 46) uniform sampler2D u_GVelocity;       // G-Buffer RT3: current-minus-previous UV motion
#endif

#include "include/SkyDepth.glsl"
#include "include/SurfaceHistory.glsl"
#include "include/TemporalResolve.glsl"

// The SAME std140 block RayTracedShadow.glsl declares — one upload feeds all
// three draws of the pass. Mirrored on the CPU by
// UBOStructures::RayTracingShadowUBO.
layout(std140, binding = 65) uniform RayTracingShadowParams
{
    mat4 u_InvView;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_LightVectors[4];
    vec4 u_LightShapes[4];
    uvec4 u_TlasAddressAndCounts;
    vec4 u_RayParams;
    vec4 u_ScreenParams;
    vec4 u_TemporalParams; // x = feedback, y = hasVelocity, z = historyUsable, w = clipGamma
    vec4 u_FilterParams;
};

// Relative view-depth tolerance for the disocclusion test. The same 5% SSGI
// uses, and for the same reason: loose enough that a grazing surface's own
// reprojection error does not read as a disocclusion, tight enough that a
// silhouette does.
const float RT_SHADOW_DEPTH_TOLERANCE = 0.05;

// Blocker distances jitter frame to frame because the rays are jittered inside
// the light's cone, so the hit-distance test has to tolerate the penumbra's own
// spread. 25% is wide enough that a converged penumbra keeps its history and
// narrow enough that a NEW occluder — which changes the distance by a lot more
// than the cone's width — rejects it.
const float RT_SHADOW_HIT_DISTANCE_TOLERANCE = 0.25;

// Visibility is binary per ray, so its accumulated estimate stops improving
// fast. Capping the effective history length is what keeps a moving occluder's
// shadow from trailing behind it.
const float RT_SHADOW_MIN_MOMENT_ALPHA = 1.0 / 32.0;

vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

float ViewDepthFromDevice(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_InvProjection * ndc;
    return -(view.z / view.w);
}

// Mean over the ACTIVE channels only. Averaging the inactive ones — which are
// pinned at 1.0 — would bias the summary toward "lit" in proportion to how many
// lights are unassigned, so a single ray-traced light in a noisy penumbra would
// look three-quarters converged and get almost no spatial filtering.
float ActiveMean(vec4 values, uint channelCount)
{
    if (channelCount == 0u)
        return 0.0;
    float total = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        if (i >= channelCount)
            break;
        total += values[i];
    }
    return total / float(channelCount);
}

OloSurfaceHistoryRecord MakeShadowSurface(float viewDepth, vec4 packedSurface, vec2 motion, float blockerDistance)
{
    OloSurfaceHistoryRecord result;
    result.LinearDepth = viewDepth;
    result.GeometricNormal = OctDecode(packedSurface.xy);
    result.ShadingNormal = result.GeometricNormal;
    result.Roughness = packedSurface.z;
    result.MaterialClass = 0u;
    result.Motion = motion;
    result.Instance = uvec2(0xffffffffu, 0u);
    result.Primitive = uvec2(0xffffffffu, 0u);
    result.Material = uvec2(0xffffffffu, 0u);
    // A zero blocker distance means "nothing occluded this pixel", which is a
    // DIFFERENT state from "an occluder at 0 m" — the flag is what carries that
    // distinction, and OloEvaluateSurfaceHistory rejects a pair whose flags
    // disagree. That rejection is the useful one: it fires exactly when a pixel
    // crosses into or out of a shadow.
    result.Flags = blockerDistance > 0.0 ? OLO_SURFACE_FLAG_HAS_HIT_DISTANCE : 0u;
    result.HitDistance = blockerDistance;
    result.PrimitiveLocalIndex = 0xffffffffu;
    return result;
}

// Reproject using the CLOSEST surface in the 3x3 neighbourhood so a foreground
// occluder drags its own history rather than the background's. Depth comes from
// the scene depth buffer rather than the signal's alpha, because every channel
// of the signal here is a light's visibility.
vec2 DilatedVelocityUV(vec2 uv, vec2 texel)
{
    vec2 bestUV = uv;
    float bestDepth = 1.0e30;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 sampleUV = uv + vec2(float(x), float(y)) * texel;
            float d = texture(u_DepthTexture, sampleUV).r;
            if (oloDepthIsSky(d))
                continue;
            float viewDepth = ViewDepthFromDevice(sampleUV, d);
            if (viewDepth > 0.0 && viewDepth < bestDepth)
            {
                bestDepth = viewDepth;
                bestUV = sampleUV;
            }
        }
    }
    return bestUV;
}

void main()
{
    vec2 uv = v_TexCoord;
    vec2 texel = u_ScreenParams.zw;
    vec4 current = texture(u_StochasticSignal, uv);
    uint channelCount = min(u_TlasAddressAndCounts.z, 4u);

    float deviceDepth = texture(u_DepthTexture, uv).r;

    // The sky is lit by definition and has no surface to accumulate against.
    // Writing it explicitly rather than letting it fall through the resolve
    // stops a reprojected sky sample from dragging a wall's history onto it.
    if (oloDepthIsSky(deviceDepth))
    {
        o_Visibility = vec4(1.0);
        o_Moments = vec4(1.0, 1.0, 0.0, 0.0);
        return;
    }

    float currentViewDepth = ViewDepthFromDevice(uv, deviceDepth);
    float currentBlocker = ActiveMean(texture(u_RayTracedShadowHitDistance, uv), channelCount);

    bool historyAvailable = u_TemporalParams.z >= 0.5;
    vec2 velocity = vec2(0.0);
    if (historyAvailable && u_TemporalParams.y > 0.5)
        velocity = texture(u_GVelocity, DilatedVelocityUV(uv, texel)).rg;

    vec2 prevUV = uv - velocity;
    bool historySampleAvailable = historyAvailable && OloTemporalHistoryUVValid(prevUV);
    vec4 history = historySampleAvailable ? texture(u_History, prevUV) : current;
    vec4 previousMoments = historySampleAvailable ? texture(u_RayTracedShadowMomentsHistory, prevUV) : vec4(0.0);

    vec4 currentSurfacePacked = texture(u_GBufferNormal, uv);
    vec4 previousSurfacePacked = historySampleAvailable ? texture(u_SurfaceHistory, prevUV) : currentSurfacePacked;

    OloSurfaceHistoryRecord currentSurface =
        MakeShadowSurface(currentViewDepth, currentSurfacePacked, velocity, currentBlocker);
    OloSurfaceHistoryRecord previousSurface =
        MakeShadowSurface(historySampleAvailable ? previousMoments.z : currentViewDepth, previousSurfacePacked,
                          vec2(0.0), historySampleAvailable ? previousMoments.w : currentBlocker);

    OloSurfaceHistorySettings validitySettings;
    validitySettings.TestMask = OLO_SURFACE_TEST_GEOMETRIC_NORMAL |
                                OLO_SURFACE_TEST_SHADING_NORMAL |
                                OLO_SURFACE_TEST_MOTION |
                                OLO_SURFACE_TEST_HIT_DISTANCE;
    validitySettings.RelativeDepthThreshold = RT_SHADOW_DEPTH_TOLERANCE;
    validitySettings.GeometricNormalCosineThreshold = 0.85;
    validitySettings.ShadingNormalCosineThreshold = 0.75;
    validitySettings.RoughnessThreshold = 0.15;
    validitySettings.MotionThresholdPixels = 64.0;
    validitySettings.RelativeHitDistanceThreshold = RT_SHADOW_HIT_DISTANCE_TOLERANCE;
    validitySettings.PixelSize = texel;
    uint rejectionReasons =
        OloEvaluateSurfaceHistory(currentSurface, previousSurface, prevUV, historyAvailable, validitySettings);

    // The hit-distance rejection is treated as a PARTIAL loss of confidence
    // rather than a full reset. Its two triggers — the pixel crossed a shadow
    // silhouette, or found a different occluder — are exactly the moments a
    // full reset would replace a smooth penumbra with one frame of raw
    // one-sample noise, which reads as a sparkling shadow edge. Every OTHER
    // rejection reason is a genuine disocclusion and does reset to zero.
    uint hardRejections = rejectionReasons & ~OLO_SURFACE_REJECT_HIT_DISTANCE;
    float confidence = hardRejections != 0u
                           ? 0.0
                           : ((rejectionReasons & OLO_SURFACE_REJECT_HIT_DISTANCE) != 0u ? 0.35 : 1.0);
    bool historyAccepted = rejectionReasons == OLO_SURFACE_REJECT_NONE;

    vec2 velocityPixels = velocity / max(texel, vec2(1.0e-8));
    float feedback = OloTemporalMotionFeedback(u_TemporalParams.x, velocityPixels, 1.0, 5.0, 0.5);

    // Per-channel scalar resolve. A vec3 YCoCg clip would be meaningless here —
    // the four channels are four independent scalars, not a colour — so each
    // gets its own neighbourhood statistics and its own clip. The gather is
    // written out rather than reusing OLO_TEMPORAL_GATHER_3X3, which is defined
    // for an rgb signal.
    vec4 resolved = current;
    for (uint channel = 0u; channel < 4u; ++channel)
    {
        if (channel >= channelCount)
        {
            resolved[channel] = 1.0;
            continue;
        }

        OloTemporalScalarStats stats;
        stats.MinC = 1.0e30;
        stats.MaxC = -1.0e30;
        float sum = 0.0;
        float sumSquares = 0.0;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                float s = texture(u_StochasticSignal, uv + vec2(float(x), float(y)) * texel)[channel];
                stats.MinC = min(stats.MinC, s);
                stats.MaxC = max(stats.MaxC, s);
                sum += s;
                sumSquares += s * s;
            }
        }
        stats.Mean = sum / 9.0;
        stats.StdDev = sqrt(max(sumSquares / 9.0 - stats.Mean * stats.Mean, 0.0));

        float clampedHistory = OloTemporalClipHistoryScalar(history[channel], stats, u_TemporalParams.w);
        resolved[channel] =
            clamp(OloTemporalBlendScalar(current[channel], clampedHistory, feedback, confidence), 0.0, 1.0);
    }

    // The moment summary, accumulated the same exponential way the resolve is,
    // so a disocclusion resets the variance estimate as well as the signal —
    // otherwise the spatial filter would be told a freshly disoccluded pixel is
    // converged and would leave its noise alone, which is the one place the
    // noise is worst.
    float currentSummary = ActiveMean(current, channelCount);
    float momentAlpha = historyAccepted ? max(1.0 - feedback, RT_SHADOW_MIN_MOMENT_ALPHA) : 1.0;
    float previousFirst = historyAccepted ? previousMoments.x : currentSummary;
    float previousSecond = historyAccepted ? previousMoments.y : currentSummary * currentSummary;

    o_Visibility = resolved;
    o_Moments = vec4(mix(previousFirst, currentSummary, momentAlpha),
                     mix(previousSecond, currentSummary * currentSummary, momentAlpha),
                     currentViewDepth,
                     currentBlocker);
}
