#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED — binding 57
// is the engine-wide vertex-pull binding.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float Data[];
} u_VertexPull;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    const uint base = uint(gl_VertexIndex) * 5u;
    const vec3 position = vec3(u_VertexPull.Data[base + 0u], u_VertexPull.Data[base + 1u],
                              u_VertexPull.Data[base + 2u]);
    v_TexCoord = vec2(u_VertexPull.Data[base + 3u], u_VertexPull.Data[base + 4u]);
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
// ReSTIR DI, draw D of four: RESOLVE. Issue #1140 (#979 Phase 3).
//
// Turn the final reservoir into the direct-lighting radiance the deferred
// lighting pass consumes in place of its own punctual and area-light loop, and
// expose every intermediate the estimator used.
//
// WHY THE VISIBILITY RAY IS TRACED AGAIN HERE. The target function is
// unshadowed, so temporal and spatial reuse can hand this pixel a sample that
// draw A's ray already rejected at a NEIGHBOURING pixel — or one whose
// occluder has moved since. One ray on the surviving sample, at the pixel that
// will actually shade with it, is the only thing that makes the resolved image
// respect geometry. It is the same ray budget as the initial draw: one per pixel.
//
// WHY THE AOVs ARE NOT OPTIONAL. #979's non-goal is explicit — "do not use
// 'path traced' to mean one noisy sample plus an opaque denoiser". A tier whose
// reservoir lineage (M, W), history verdict, variance and bias-clamp state
// cannot be looked at is exactly that. Every one of those is a debug view here,
// selected by u_EstimatorParams.w, and the RADIANCE view is just the first of
// them rather than a privileged mode.
// =============================================================================

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require

// The direct-lighting radiance the deferred lighting pass samples. RGBA16F;
// alpha is 1 where this tier produced a value and 0 where it did not, so the
// consumer can tell "black because nothing lights this pixel" from "no value" —
// the distinction a single black texel cannot carry.
layout(location = 0) out vec4 o_Radiance;
// Variance moments of the resolved luminance: x = first, y = second. Fed back
// through the ReSTIRDI Moments history planes so the Variance debug view shows
// a converging quantity rather than one frame's noise.
layout(location = 1) out vec4 o_Moments;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Reservoir0 OLO_HEAP_TEX_2D(0)
#define u_Reservoir1 OLO_HEAP_TEX_2D(1)
#define u_Reservoir2 OLO_HEAP_TEX_2D(2)
#define u_RawCandidate OLO_HEAP_TEX_2D(3)
#define u_Validity OLO_HEAP_TEX_2D(4)
#define u_MomentsHistory OLO_HEAP_TEX_2D(5)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45)
#else
layout(binding = 0) uniform sampler2D u_Reservoir0;      // after temporal + spatial reuse
layout(binding = 1) uniform sampler2D u_Reservoir1;
layout(binding = 2) uniform sampler2D u_Reservoir2;
layout(binding = 3) uniform sampler2D u_RawCandidate;    // draw A's un-reused signal
layout(binding = 4) uniform sampler2D u_Validity;        // draw B's #976 rejection mask
layout(binding = 5) uniform sampler2D u_MomentsHistory;  // last frame's resolved moments
layout(binding = 19) uniform sampler2D u_DepthTexture;
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;
layout(binding = 44) uniform sampler2D u_GBufferNormal;
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
#endif

#include "include/SkyDepth.glsl"
#include "include/PBRCommon.glsl"
// OloTemporalMoments / OloAccumulateTemporalMoments and the #976 rejection bits.
#include "include/SurfaceHistory.glsl"
#include "include/ReSTIRDISceneAccess.glsl"

// A scalar or a vector rendered as a debug colour. Kept in one place so every
// view scales the same way and a screenshot of one is comparable with a
// screenshot of another.
vec3 DebugRamp(float value)
{
    const float v = clamp(value, 0.0, 1.0);
    return vec3(v, v * v, v * v * v);
}

void main()
{
    // Alpha 0 means "this tier produced nothing here". A sky pixel, an unlit
    // pixel and a stood-down frame all land here, and the consumer falls back
    // to its own lighting rather than multiplying by a black texel.
    o_Radiance = vec4(0.0);
    o_Moments = vec4(0.0);

    const float depth = texture(u_DepthTexture, v_TexCoord).r;
    const vec4 packedEmissive = texture(u_GBufferEmissive, v_TexCoord);
    const int gbFlags = oloDecodeGBufferFlags(packedEmissive.a);
    if (oloDepthIsSky(depth) || oloGBufferFlagsAreUnlit(gbFlags))
        return;

    const vec4 packedNormal = texture(u_GBufferNormal, v_TexCoord);
    const vec4 packedAlbedo = texture(u_GBufferAlbedo, v_TexCoord);
    const OloReSTIRSurface surface =
        OloReSTIRLoadSurface(v_TexCoord, depth, packedNormal, packedAlbedo, oloGBufferFlagsPbrModel(gbFlags));
    const vec3 viewDirection = normalize((u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz - surface.Position);

    OloReservoir reservoir = OloUnpackReservoir(texture(u_Reservoir0, v_TexCoord),
                                               texture(u_Reservoir1, v_TexCoord),
                                               texture(u_Reservoir2, v_TexCoord));

    vec3 radiance = vec3(0.0);
    bool visible = false;
    if (!OloReservoirIsEmpty(reservoir) && reservoir.W > 0.0)
    {
        // The VisibilityReuse setting is honoured HERE too, not just in the
        // initial draw. It used to be ignored here, which made "visibility reuse
        // off" mean "one ray instead of two" rather than "no rays" — so the
        // switch could not do the one job it exists for, isolating the ray cost
        // from the resampling when profiling, and could not be used to A/B this
        // tier against a clustered path whose lights are CastShadows:false.
        const bool traceVisibility = (u_EmissiveTable.w & OLO_RESTIR_FLAG_VISIBILITY_REUSE) != 0u;
        visible = !traceVisibility ||
                  OloReSTIRSampleVisible(surface, reservoir.Sample, u_ReuseParams.z, u_ReuseParams.w);
        if (visible)
        {
            radiance = OloReSTIRUnshadowedContribution(surface, reservoir.Sample, viewDirection) * reservoir.W;
        }
        else
        {
            reservoir.Diagnostics |= OLO_RESERVOIR_DIAG_VISIBILITY_KILLED;
        }
    }

    // The firefly clamp. It is a BIAS and it is off by default: any run that
    // claims to match the oracle must leave it off, and the diagnostics lane
    // records where it fired so a clamped frame is never mistaken for a
    // converged one.
    const float clampLimit = u_EstimatorParams.z;
    if (clampLimit > 0.0)
    {
        const float peak = max(max(radiance.r, radiance.g), radiance.b);
        if (peak > clampLimit)
        {
            radiance *= clampLimit / peak;
            reservoir.Diagnostics |= OLO_RESERVOIR_DIAG_RADIANCE_CLAMPED;
        }
    }

    if (!OloReservoirFinite(radiance))
        radiance = vec3(0.0);

    // ---- moments, for the Variance view ----------------------------------
    const float luminance = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    const vec4 previousMoments = texture(u_MomentsHistory, v_TexCoord);
    // MOMENTS_VALID, not HISTORY_VALID: the moments plane accumulates whenever
    // it is bound, whether or not the RESERVOIRS are being reused. Unit 5 falls
    // back to the raw-candidate target when the plane is absent, so this flag is
    // also what stops the accumulator reading that target's blue channel as a
    // history length.
    const bool momentsUsable = (u_EmissiveTable.w & OLO_RESTIR_FLAG_MOMENTS_VALID) != 0u &&
                               OloReservoirFinite(previousMoments.x) && OloReservoirFinite(previousMoments.y) &&
                               previousMoments.z > 0.0;
    OloTemporalMoments moments;
    moments.First = momentsUsable ? vec4(previousMoments.x) : vec4(luminance);
    moments.Second = momentsUsable ? vec4(previousMoments.y) : vec4(luminance * luminance);
    moments.HistoryLength = momentsUsable ? previousMoments.z : 0.0;
    const OloTemporalMoments accumulated =
        OloAccumulateTemporalMoments(vec4(luminance), moments, momentsUsable, 255.0);
    o_Moments = vec4(accumulated.First.x, accumulated.Second.x, accumulated.HistoryLength, 1.0);
    const float variance = max(accumulated.Second.x - accumulated.First.x * accumulated.First.x, 0.0);

    // ---- what lands on screen --------------------------------------------
    const int view = int(u_EstimatorParams.w + 0.5);
    if (view == OLO_RESTIR_VIEW_RAW_CANDIDATE)
    {
        o_Radiance = vec4(texture(u_RawCandidate, v_TexCoord).rgb, 1.0);
        return;
    }
    if (view == OLO_RESTIR_VIEW_HISTORY_VALIDITY)
    {
        const vec4 validity = texture(u_Validity, v_TexCoord);
        const uint rejections = uint(validity.x + 0.5);
        // Green where the history was accepted; the rejection bits colour the
        // rest, so "why did this pixel restart" is answerable from the picture.
        const vec3 colour = rejections == OLO_SURFACE_REJECT_NONE
                                ? vec3(0.0, 1.0, 0.0)
                                : vec3(float((rejections >> 3u) & 7u) / 7.0,
                                       float((rejections >> 6u) & 7u) / 7.0,
                                       float((rejections >> 9u) & 7u) / 7.0);
        o_Radiance = vec4(colour, 1.0);
        return;
    }
    if (view == OLO_RESTIR_VIEW_VARIANCE)
    {
        o_Radiance = vec4(DebugRamp(sqrt(variance) * 8.0), 1.0);
        return;
    }
    if (view == OLO_RESTIR_VIEW_RESERVOIR_M)
    {
        // Scaled by the cap, so a full bar means "this pixel is at the cap" —
        // an absolute scale would make the view depend on the setting.
        o_Radiance = vec4(DebugRamp(reservoir.M / max(u_ReuseParams.x, 1.0)), 1.0);
        return;
    }
    if (view == OLO_RESTIR_VIEW_RESERVOIR_W)
    {
        o_Radiance = vec4(DebugRamp(reservoir.W), 1.0);
        return;
    }
    if (view == OLO_RESTIR_VIEW_SAMPLE_KIND)
    {
        vec3 colour = vec3(0.0);
        if (reservoir.Sample.Kind == OLO_LIGHT_SAMPLE_PUNCTUAL)
            colour = vec3(1.0, 0.4, 0.1);
        else if (reservoir.Sample.Kind == OLO_LIGHT_SAMPLE_DIRECTIONAL)
            colour = vec3(1.0, 1.0, 0.5);
        else if (reservoir.Sample.Kind == OLO_LIGHT_SAMPLE_SPHERE_AREA)
            colour = vec3(0.2, 0.6, 1.0);
        else if (reservoir.Sample.Kind == OLO_LIGHT_SAMPLE_EMISSIVE_TRIANGLE)
            colour = vec3(0.3, 1.0, 0.4);
        o_Radiance = vec4(colour, 1.0);
        return;
    }
    if (view == OLO_RESTIR_VIEW_BIAS_CLAMP)
    {
        // Red where the Jacobian rejected a neighbour, green where the firefly
        // clamp fired, blue where the visibility ray killed the survivor: the
        // three places this tier deliberately departs from the raw estimate.
        o_Radiance = vec4((reservoir.Diagnostics & OLO_RESERVOIR_DIAG_JACOBIAN_REJECTED) != 0u ? 1.0 : 0.0,
                          (reservoir.Diagnostics & OLO_RESERVOIR_DIAG_RADIANCE_CLAMPED) != 0u ? 1.0 : 0.0,
                          (reservoir.Diagnostics & OLO_RESERVOIR_DIAG_VISIBILITY_KILLED) != 0u ? 1.0 : 0.0, 1.0);
        return;
    }

    o_Radiance = vec4(radiance, 1.0);
}
