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
// ReSTIR DI, draw B of four: TEMPORAL REUSE. Issue #1140 (#979 Phase 3).
//
// Merge last frame's reservoir at this pixel's reprojected position into this
// frame's, gated on the #976 history-validity layer, then cap M.
//
// WHY THE JACOBIAN IS EXACTLY 1 HERE, AND WHAT LICENSES THAT. A reconnection
// shift needs a Jacobian when the two shading points differ. The #976 validity
// test is precisely the question "is last frame's sample the SAME surface
// point?" — same instance, same primitive, same material, depth and both normals
// within threshold. When it says yes, the shift is the identity and the Jacobian
// is 1; when it says no, the reuse is REJECTED rather than shifted. So the
// absence of a Jacobian in this draw is not an omission, it is what the validity
// test buys. The spatial draw, whose neighbours are genuinely different points,
// computes the real thing.
//
// WHAT MAKES MOTION WORK RATHER THAN SMEAR. #979's cross-cutting criteria ask
// for quality demonstrated DURING motion, not after accumulation, and there are
// three distinct events:
//
//   * A CAMERA CUT invalidates the whole history through the temporal-history
//     registry, so this draw sees no previous reservoir at all and the frame is
//     one RIS sample per pixel. Noisy, and correct.
//   * DISOCCLUSION is per-pixel: the reprojected UV lands on a different
//     surface, #976 rejects it, and that pixel restarts while its neighbours
//     keep their history. The spatial draw is what fills the restarted pixels
//     from the ones that survived.
//   * A LIGHT OR OBJECT MOVING under a still camera passes #976 (the receiving
//     surface did not change) and is bounded by the M CAP alone. That is the cap's
//     real job, and it is why a cap that is too high reads as lighting that lags
//     the scene by a visible fraction of a second.
// =============================================================================

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) out vec4 o_Reservoir0;
layout(location = 1) out vec4 o_Reservoir1;
layout(location = 2) out vec4 o_Reservoir2;
// The #976 rejection bitmask this pixel produced, so the HistoryValidity debug
// view shows WHY a pixel restarted rather than only that it looks noisy.
layout(location = 3) out vec4 o_Validity;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Reservoir0 OLO_HEAP_TEX_2D(0)
#define u_Reservoir1 OLO_HEAP_TEX_2D(1)
#define u_Reservoir2 OLO_HEAP_TEX_2D(2)
#define u_Surface OLO_HEAP_TEX_2D(3)
#define u_HistoryReservoir0 OLO_HEAP_TEX_2D(4)
#define u_HistoryReservoir1 OLO_HEAP_TEX_2D(5)
#define u_HistoryReservoir2 OLO_HEAP_TEX_2D(6)
#define u_HistorySurface OLO_HEAP_TEX_2D(7)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45)
#define u_GVelocity OLO_HEAP_TEX_2D(46)
#else
layout(binding = 0) uniform sampler2D u_Reservoir0;        // this frame, draw A
layout(binding = 1) uniform sampler2D u_Reservoir1;
layout(binding = 2) uniform sampler2D u_Reservoir2;
layout(binding = 3) uniform sampler2D u_Surface;           // this frame's #976 record
layout(binding = 4) uniform sampler2D u_HistoryReservoir0; // last frame, after spatial reuse
layout(binding = 5) uniform sampler2D u_HistoryReservoir1;
layout(binding = 6) uniform sampler2D u_HistoryReservoir2;
layout(binding = 7) uniform sampler2D u_HistorySurface;    // last frame's #976 record
layout(binding = 19) uniform sampler2D u_DepthTexture;
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;
layout(binding = 44) uniform sampler2D u_GBufferNormal;
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
layout(binding = 46) uniform sampler2D u_GVelocity;        // RT3: current-minus-previous UV motion
#endif

#include "include/SkyDepth.glsl"
#include "include/PBRCommon.glsl"
#include "include/SurfaceHistory.glsl"
#include "include/ReSTIRDISceneAccess.glsl"

// The #976 record as this tier fills it. The G-Buffer gives no per-pixel
// instance / primitive / material identity, so those tests are switched OFF and
// the record says so through the settings mask rather than by passing invalid
// handles — an invalid handle would trip IdentityUnavailable and reject every
// pixel, which is a whole tier silently disabled. Depth, both normals, roughness
// and motion are the tests this tier actually has data for, which is the same
// set RayTracedShadowResolve.glsl uses and for the same reason.
OloSurfaceHistoryRecord MakeReSTIRSurface(vec4 packed, vec2 motion)
{
    OloSurfaceHistoryRecord result;
    result.LinearDepth = packed.w;
    result.GeometricNormal = OloReSTIROctDecode(packed.xy);
    result.ShadingNormal = result.GeometricNormal;
    result.Roughness = packed.z;
    result.MaterialClass = 0u;
    result.Motion = motion;
    result.Instance = uvec2(0u, 1u);
    result.Primitive = uvec2(0u, 1u);
    result.Material = uvec2(0u, 1u);
    result.Flags = 0u;
    result.HitDistance = 0.0;
    result.PrimitiveLocalIndex = 0u;
    return result;
}

void main()
{
    const vec4 current0 = texture(u_Reservoir0, v_TexCoord);
    const vec4 current1 = texture(u_Reservoir1, v_TexCoord);
    const vec4 current2 = texture(u_Reservoir2, v_TexCoord);
    OloReservoir reservoir = OloUnpackReservoir(current0, current1, current2);
    // NO_HISTORY, not zero. Zero is OLO_SURFACE_REJECT_NONE, which the
    // HistoryValidity view paints green for "the history was accepted" — and a
    // draw that returns before evaluating a verdict must not report one. Every
    // early exit below leaves this value, so an unevaluated pixel reads as "no
    // history" rather than as a successful reuse.
    o_Validity = vec4(float(OLO_SURFACE_REJECT_NO_HISTORY), 0.0, 0.0, 1.0);

    const float depth = texture(u_DepthTexture, v_TexCoord).r;
    const vec4 packedEmissive = texture(u_GBufferEmissive, v_TexCoord);
    const int gbFlags = oloDecodeGBufferFlags(packedEmissive.a);
    const bool shadeable = !oloDepthIsSky(depth) && !oloGBufferFlagsAreUnlit(gbFlags);
    const bool temporalRequested = (u_EmissiveTable.w & OLO_RESTIR_FLAG_TEMPORAL_REUSE) != 0u;

    if (!shadeable || !temporalRequested)
    {
        // Pass draw A's reservoir through untouched. A pass-through is the right
        // stand-down: the chain still produces a valid reservoir, one frame's
        // worth of samples, and the tier is noisier rather than wrong.
        OloPackReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }

    const vec4 packedNormal = texture(u_GBufferNormal, v_TexCoord);
    const vec4 packedAlbedo = texture(u_GBufferAlbedo, v_TexCoord);
    const OloReSTIRSurface surface =
        OloReSTIRLoadSurface(v_TexCoord, depth, packedNormal, packedAlbedo, oloGBufferFlagsPbrModel(gbFlags));
    const vec3 viewDirection = normalize((u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz - surface.Position);

    const vec2 velocity = texture(u_GVelocity, v_TexCoord).xy;
    const vec2 prevUV = v_TexCoord - velocity;

    const bool historyAvailable = (u_EmissiveTable.w & OLO_RESTIR_FLAG_HISTORY_VALID) != 0u;
    const bool historySampleAvailable = historyAvailable && OloTemporalHistoryUVValid(prevUV);

    const vec4 currentSurfacePacked = vec4(packedNormal.xy, packedNormal.z, surface.ViewDepth);
    const vec4 previousSurfacePacked =
        historySampleAvailable ? texture(u_HistorySurface, prevUV) : currentSurfacePacked;

    OloSurfaceHistorySettings validity;
    // Only the tests this tier has data for. See MakeReSTIRSurface.
    validity.TestMask = OLO_SURFACE_TEST_GEOMETRIC_NORMAL | OLO_SURFACE_TEST_SHADING_NORMAL |
                        OLO_SURFACE_TEST_ROUGHNESS | OLO_SURFACE_TEST_MOTION;
    validity.RelativeDepthThreshold = 0.05;
    validity.GeometricNormalCosineThreshold = 0.9;
    validity.ShadingNormalCosineThreshold = 0.9;
    // Tighter than the shadow tier's 0.15: a reservoir carries a light SAMPLE,
    // and a roughness change moves the target function the sample was chosen
    // against, so a stale sample is worse here than a stale scalar visibility.
    validity.RoughnessThreshold = 0.1;
    validity.MotionThresholdPixels = 64.0;
    validity.RelativeHitDistanceThreshold = 0.1;
    validity.PixelSize = u_ScreenParams.zw;

    const uint rejections = OloEvaluateSurfaceHistory(MakeReSTIRSurface(currentSurfacePacked, velocity),
                                                      MakeReSTIRSurface(previousSurfacePacked, velocity), prevUV,
                                                      historyAvailable, validity);
    o_Validity = vec4(float(rejections), rejections == OLO_SURFACE_REJECT_NONE ? 1.0 : 0.0, 0.0, 1.0);

    if (rejections != OLO_SURFACE_REJECT_NONE || !historySampleAvailable)
    {
        reservoir.Diagnostics |= OLO_RESERVOIR_DIAG_TEMPORAL_REJECTED;
        OloPackReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }

    const OloReservoir history = OloUnpackReservoir(texture(u_HistoryReservoir0, prevUV),
                                                   texture(u_HistoryReservoir1, prevUV),
                                                   texture(u_HistoryReservoir2, prevUV));

    // Both reservoirs are re-evaluated against THIS pixel's target function.
    // Re-evaluating rather than trusting the stored TargetPdf is the whole point:
    // the stored value was measured against last frame's surface, and using it
    // would make the merge weight a sample by how good it was for a surface that
    // no longer exists.
    const float currentTarget = OloReSTIRTargetPdf(surface, reservoir.Sample, viewDirection);
    const float historyTarget = OloReSTIRTargetPdf(surface, history.Sample, viewDirection);

    OloReservoir merged = OloMakeEmptyReservoir();
    merged.Diagnostics = reservoir.Diagnostics | OLO_RESERVOIR_DIAG_TEMPORAL_ACCEPTED;

    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    OloPathSampler pathSampler = oloPtMakeSampler(
        oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), u_TlasAddressAndFrame.w ^ 0x5bf03635u),
        u_TlasAddressAndFrame.w);

    // w_i = m_i * pHat_dest(y_i) * W_i. Under 1/M the m_i are folded into the
    // normaliser instead, so they are 1 here and FinalizeCombined divides by the
    // summed M; under the MIS-weighted mode m_i is the balance heuristic over
    // the two reservoirs and the normaliser is 1. Two candidates make the
    // heuristic cheap enough to always compute, so the temporal draw pays no
    // extra target evaluation for choosing the unbiased mode — only the spatial
    // draw does.
    // m_i CARRIES THE CONFIDENCE WEIGHT. Under 1/M it is M_i — the reservoir
    // stands for M_i candidates, and FinalizeCombined's summed-M denominator is
    // what divides them back out. Using 1 here darkens the frame by the average
    // M, which is a factor of 8 at eight candidates and worse once the cap raises
    // it; ReSTIRDIOracleTest measured exactly that.
    const bool unbiased = u_ResamplingCounts.w == OLO_RESTIR_BIAS_MODE_UNBIASED_MIS;
    float mCurrent = reservoir.M;
    float mHistory = history.M;
    if (unbiased)
    {
        // pHat_j(y_i) for a two-reservoir merge. Both reservoirs live on the
        // SAME surface (that is what the validity test established), so
        // pHat_current(y) and pHat_history(y) are the same function and the
        // heuristic reduces to the M-weighted split below. Writing it out rather
        // than hard-coding 0.5 is what keeps the two modes' arithmetic in one
        // shape, so a later change to the target function cannot silently make
        // only one of them right.
        //
        // The numerator ALREADY contains M_i, so these m are the normalised
        // M-fractions and FinalizeCombined's normaliser is 1 — the 1/M arm above
        // instead keeps the raw M_i and divides by the summed M. Both reach the
        // same number here, which is the identity
        // ReSTIRDIContract.BothBiasModesAgreeOnReservoirsThatShareADomain pins.
        const float denomCurrent = reservoir.M * currentTarget + history.M * currentTarget;
        const float denomHistory = reservoir.M * historyTarget + history.M * historyTarget;
        mCurrent = denomCurrent > 0.0 ? (reservoir.M * currentTarget) / denomCurrent : 0.0;
        mHistory = denomHistory > 0.0 ? (history.M * historyTarget) / denomHistory : 0.0;
    }

    OloReservoirUpdate(merged, reservoir.Sample, mCurrent * currentTarget * reservoir.W, currentTarget,
                       oloPtGet1D(pathSampler));
    OloReservoirUpdate(merged, history.Sample, mHistory * historyTarget * history.W, historyTarget,
                       oloPtGet1D(pathSampler));

    // OloReservoirUpdate counted two candidates; the merged confidence is the
    // SUM of the two reservoirs' M, not 2.
    OloReservoirFinalizeCombined(merged, u_ResamplingCounts.w, reservoir.M + history.M);
    OloReservoirApplyMCap(merged, u_ReuseParams.x);

    OloPackReservoir(merged, o_Reservoir0, o_Reservoir1, o_Reservoir2);
}
