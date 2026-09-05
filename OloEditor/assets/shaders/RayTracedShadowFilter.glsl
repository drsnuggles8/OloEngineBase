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
// RayTracedShadowFilter.glsl — draw C of RayTracedShadowPass. Issue #1056.
//
// The variance-guided spatial filter, and the only output anything outside the
// pass reads: RayTracedShadowMask, one channel per ray-traced light, 1 = lit.
//
// WHY VARIANCE GUIDES IT. Temporal accumulation converges a static pixel in a
// few dozen frames and leaves a freshly disoccluded one with a single
// stochastic sample. A fixed-radius blur would then over-smooth the converged
// interior of a penumbra (destroying the contact hardening the ray tracing
// exists to produce) while under-smoothing the disoccluded edge (which is the
// one that sparkles). Driving the radius by the accumulated variance the
// resolve wrote spends the filter where the noise actually is, and shrinks it
// to nothing where the estimate has converged — so a converged contact shadow
// stays as sharp as the geometry says it is.
//
// THE EDGE STOP IS DEPTH AND NORMAL, NOT THE SIGNAL. Weighting by how similar
// the neighbour's VISIBILITY is would be self-fulfilling: a noisy pixel's
// neighbours look dissimilar precisely because they are noisy, so the filter
// would refuse to fix the pixels that need it most. Weighting by the surface
// instead means the filter never crosses a silhouette or a crease, and inside
// one surface it averages freely — which is what a shadow needs, because a
// penumbra is a smooth function ON a surface.
// =============================================================================

layout(location = 0) out vec4 o_Mask;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_RayTracedShadowResolved OLO_HEAP_TEX_2D(0)
#define u_RayTracedShadowMoments OLO_HEAP_TEX_2D(1)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)  // TEX_POSTPROCESS_DEPTH
#define u_GBufferNormal OLO_HEAP_TEX_2D(44) // TEX_GBUFFER_NORMAL
#else
layout(binding = 0) uniform sampler2D u_RayTracedShadowResolved; // draw B's temporally resolved visibility
layout(binding = 1) uniform sampler2D u_RayTracedShadowMoments;  // draw B's moments + view depth + blocker distance
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // oct world normal + roughness + AO
#endif

#include "include/SkyDepth.glsl"

// The SAME std140 block RayTracedShadow.glsl declares.
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
    vec4 u_TemporalParams;
    vec4 u_FilterParams; // x = spatialRadiusPixels, y = spatialEnabled, zw reserved
};

// Fixed tap count, variable SPACING. A variable tap count would branch
// divergently across a wavefront for no quality gain; spreading a fixed 5x5
// over a wider footprint is how a variance-guided filter widens.
const int RT_SHADOW_FILTER_HALF_TAPS = 2;

// Visibility is a value in [0, 1] whose per-ray variance is bounded by 0.25
// (a Bernoulli variable's maximum). Normalising against that maximum is what
// makes the radius scale meaningful rather than scene-dependent.
const float RT_SHADOW_MAX_VARIANCE = 0.25;

// Depth edge stop: how far, as a fraction of the centre pixel's view depth, a
// neighbour may be before it stops contributing.
const float RT_SHADOW_DEPTH_SIGMA = 0.02;

vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main()
{
    vec2 uv = v_TexCoord;
    vec4 resolved = texture(u_RayTracedShadowResolved, uv);

    // Everything that is not a filterable opaque surface passes through
    // unchanged, and passes through at whatever the resolve decided — including
    // the sky's explicit 1.0. Guarding here rather than clamping at the end
    // means a disabled filter and an unfilterable pixel take the same path.
    float deviceDepth = texture(u_DepthTexture, uv).r;
    uint channelCount = min(u_TlasAddressAndCounts.z, 4u);
    if (u_FilterParams.y < 0.5 || channelCount == 0u || oloDepthIsSky(deviceDepth))
    {
        o_Mask = resolved;
        return;
    }

    vec4 moments = texture(u_RayTracedShadowMoments, uv);
    float variance = max(moments.y - moments.x * moments.x, 0.0);
    float noise = clamp(variance / RT_SHADOW_MAX_VARIANCE, 0.0, 1.0);

    // sqrt, not a linear ramp: variance is a squared quantity, so its square
    // root is the one that scales like a distance in the signal — a linear ramp
    // leaves a nearly-converged pixel with a filter far narrower than the noise
    // it still carries.
    float radiusPixels = max(u_FilterParams.x, 0.0) * sqrt(noise);
    if (radiusPixels <= 0.5)
    {
        o_Mask = resolved;
        return;
    }

    vec2 texel = u_ScreenParams.zw;
    vec2 step = texel * (radiusPixels / float(RT_SHADOW_FILTER_HALF_TAPS));

    float centreViewDepth = moments.z;
    vec3 centreNormal = OctDecode(texture(u_GBufferNormal, uv).xy);

    vec4 total = vec4(0.0);
    float weightSum = 0.0;
    for (int y = -RT_SHADOW_FILTER_HALF_TAPS; y <= RT_SHADOW_FILTER_HALF_TAPS; ++y)
    {
        for (int x = -RT_SHADOW_FILTER_HALF_TAPS; x <= RT_SHADOW_FILTER_HALF_TAPS; ++x)
        {
            vec2 sampleUV = uv + vec2(float(x), float(y)) * step;
            if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
                continue;

            float sampleDepth = texture(u_DepthTexture, sampleUV).r;
            if (oloDepthIsSky(sampleDepth))
                continue;

            // The neighbour's view depth is read from the RESOLVE's own
            // moments rather than reconstructed from the depth buffer: the
            // resolve already paid for that reconstruction, and reading it back
            // guarantees the two stages agree about what "this pixel's depth"
            // means even if the reconstruction changes.
            vec4 sampleMoments = texture(u_RayTracedShadowMoments, sampleUV);
            float depthDifference = abs(sampleMoments.z - centreViewDepth);
            float depthWeight =
                exp(-depthDifference / max(RT_SHADOW_DEPTH_SIGMA * max(centreViewDepth, 1.0e-3), 1.0e-6));

            vec3 sampleNormal = OctDecode(texture(u_GBufferNormal, sampleUV).xy);
            float normalWeight = pow(max(dot(centreNormal, sampleNormal), 0.0), 16.0);

            // A Gaussian in the SPACING, not in raw pixels: the taps are
            // already spread by the radius, so the spatial term only shapes
            // their relative contribution and stays the same at every radius.
            float distanceSquared = float(x * x + y * y);
            float spatialWeight = exp(-distanceSquared / (2.0 * float(RT_SHADOW_FILTER_HALF_TAPS)));

            float weight = depthWeight * normalWeight * spatialWeight;
            total += texture(u_RayTracedShadowResolved, sampleUV) * weight;
            weightSum += weight;
        }
    }

    // A zero weight sum can only happen if every tap including the centre was
    // rejected, which the centre's own weight of 1 makes impossible — but
    // guarding is cheaper than a NaN that then propagates into the lit frame
    // through the multiply in DeferredLightingShared.glsl.
    o_Mask = weightSum > 0.0 ? clamp(total / weightSum, 0.0, 1.0) : resolved;
}
