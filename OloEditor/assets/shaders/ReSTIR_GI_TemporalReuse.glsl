#type vertex
#version 460 core

#ifdef OLO_VULKAN
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
// ReSTIR GI, draw B of four: TEMPORAL REUSE. Issue #1169 (#979 Phase 3).
//
// Merge last frame's reservoir at this pixel's reprojected position into this
// frame's, gated on the #976 history-validity layer AND on the sample's AGE,
// with a real reconnection Jacobian, then cap M.
//
// WHY THIS DRAW COMPUTES A JACOBIAN WHERE ReSTIR_DI_TemporalReuse.glsl IS
// LICENSED TO USE 1. DI's argument is that the #976 validity test establishes
// that last frame's shading point IS this frame's, so the shift is the identity.
// The test is the same here and the conclusion is not, for one reason: DI's
// Jacobian divides by the distance to an EMITTER, metres away, and a sub-pixel
// reprojection error does not move it. GI's divides by the distance to a SAMPLE
// VERTEX, which can be centimetres away — dDestSq is in the denominator — so the
// same sub-pixel error is a real factor (design note §6.3). The previous shading
// point is reconstructed from the reprojected UV, the view depth in the surface
// history plane, and u_PrevInvView / u_PrevInvProjection, which exist in the
// parameter block for exactly this and are read on every frame that reuses
// history.
//
// AND WHY THERE IS AN AGE CAP AS WELL AS AN M CAP (design note §10). #976 looks
// only at the RECEIVING surface. A sample whose own vertex has moved, or whose
// vertex has been re-lit, passes it every single frame — the receiver did not
// change. The M cap bounds how much of the pixel's estimate such a sample can
// still claim; the AGE cap bounds whether it survives at all. They are not the
// same bound and neither substitutes for the other.
//
// WHAT MAKES MOTION WORK RATHER THAN SMEAR, the three distinct events #979 asks
// about:
//
//   * A CAMERA CUT invalidates the whole history through the temporal-history
//     registry, so this draw sees no previous reservoir and the frame is one
//     bounce per pixel plus spatial reuse. Noisy, and correct.
//   * DISOCCLUSION is per-pixel: the reprojected UV lands on a different
//     surface, #976 rejects it, and that pixel restarts while its neighbours
//     keep their history. The spatial draw refills it from the survivors.
//   * A LIGHT OR OBJECT MOVING under a still camera passes #976 and is bounded
//     by the two caps above. That is the case the age cap exists for.
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
#include "include/ReSTIRGISceneAccess.glsl"

// The #976 record as this tier fills it. The G-Buffer gives no per-pixel
// instance / primitive / material identity, so those tests are switched OFF and
// the record says so through the settings mask rather than by passing invalid
// handles — an invalid handle would trip IdentityUnavailable and reject every
// pixel, which is a whole tier silently disabled. Depth, both normals, roughness
// and motion are the tests this tier actually has data for, the same set
// RayTracedShadowResolve.glsl and the DI tier use and for the same reason.
OloSurfaceHistoryRecord MakeReSTIRGISurface(vec4 packed, vec2 motion)
{
    OloSurfaceHistoryRecord result;
    result.LinearDepth = packed.w;
    result.GeometricNormal = OloGBufferOctDecode(packed.xy);
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

// LAST FRAME'S SHADING POINT, in THIS frame's render-relative space.
//
// Three things have to line up and each is a way to be silently wrong:
//   * the PREVIOUS inverse projection, not this frame's — a moving camera
//     changes it, and using the current one makes the reconstructed point drift
//     with the motion rather than stand still;
//   * the surface history plane's LINEAR view depth, which is what draw A wrote
//     there, so the ray is scaled to z = -1 before being multiplied by it rather
//     than being unprojected from an NDC depth nobody stored;
//   * u_PrevOriginDelta, because the render origin can SNAP between frames
//     (issue #429). It is zero until the grid snaps, which is why leaving it out
//     looks correct in every test scene near the world origin and misses by
//     exactly the origin 1024 m out.
vec3 PreviousShadingPoint(vec2 prevUV, float prevViewDepth)
{
    vec4 clip = vec4(prevUV * 2.0 - 1.0, 0.0, 1.0);
    vec4 unprojected = u_PrevInvProjection * clip;
    vec3 onRay = unprojected.xyz / unprojected.w;
    // Scaled so that z == -1, then stretched to the stored linear depth.
    vec3 viewRay = onRay / max(-onRay.z, 1.0e-6);
    vec3 viewPos = viewRay * prevViewDepth;
    return (u_PrevInvView * vec4(viewPos, 1.0)).xyz + u_PrevOriginDelta.xyz;
}

void main()
{
    const vec4 current0 = texture(u_Reservoir0, v_TexCoord);
    const vec4 current1 = texture(u_Reservoir1, v_TexCoord);
    const vec4 current2 = texture(u_Reservoir2, v_TexCoord);
    OloGIReservoir reservoir = OloUnpackGIReservoir(current0, current1, current2);
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
    const bool temporalRequested = (u_EmissiveTable.w & OLO_RESTIR_GI_FLAG_TEMPORAL_REUSE) != 0u;

    if (!shadeable || !temporalRequested)
    {
        // Pass draw A's reservoir through untouched. A pass-through is the right
        // stand-down: the chain still produces a valid reservoir, one frame's
        // worth of samples, and the tier is noisier rather than wrong.
        OloPackGIReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }

    const vec4 packedNormal = texture(u_GBufferNormal, v_TexCoord);
    const vec4 packedAlbedo = texture(u_GBufferAlbedo, v_TexCoord);
    const OloGBufferSurface surface =
        OloLoadGBufferSurface(v_TexCoord, depth, packedNormal, packedAlbedo, oloGBufferFlagsPbrModel(gbFlags));
    const vec3 viewDirection = normalize((u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz - surface.Position);

    const vec2 velocity = texture(u_GVelocity, v_TexCoord).xy;
    const vec2 prevUV = v_TexCoord - velocity;

    const bool historyAvailable = (u_EmissiveTable.w & OLO_RESTIR_GI_FLAG_HISTORY_VALID) != 0u;
    const bool historySampleAvailable = historyAvailable && OloTemporalHistoryUVValid(prevUV);

    const vec4 currentSurfacePacked = vec4(packedNormal.xy, packedNormal.z, surface.ViewDepth);
    const vec4 previousSurfacePacked =
        historySampleAvailable ? texture(u_HistorySurface, prevUV) : currentSurfacePacked;

    OloSurfaceHistorySettings validity;
    validity.TestMask = OLO_SURFACE_TEST_GEOMETRIC_NORMAL | OLO_SURFACE_TEST_SHADING_NORMAL |
                        OLO_SURFACE_TEST_ROUGHNESS | OLO_SURFACE_TEST_MOTION;
    validity.RelativeDepthThreshold = 0.05;
    validity.GeometricNormalCosineThreshold = 0.9;
    validity.ShadingNormalCosineThreshold = 0.9;
    // LOOSER than DI's 0.1, and that is not a slip. A DI reservoir's sample was
    // chosen against a target function that contains the surface's specular
    // lobe, so a roughness change moves it sharply. A GI reservoir carries a
    // one-bounce VERTEX, which is a property of the geometry around the pixel
    // rather than of its material: the same vertex is still the right sample for
    // a surface whose roughness drifted, and rejecting it would throw away
    // history for a change the estimate barely notices.
    validity.RoughnessThreshold = 0.2;
    validity.MotionThresholdPixels = 64.0;
    validity.RelativeHitDistanceThreshold = 0.1;
    validity.PixelSize = u_ScreenParams.zw;

    const uint rejections = OloEvaluateSurfaceHistory(MakeReSTIRGISurface(currentSurfacePacked, velocity),
                                                      MakeReSTIRGISurface(previousSurfacePacked, velocity),
                                                      prevUV, historyAvailable, validity);
    o_Validity = vec4(float(rejections), rejections == OLO_SURFACE_REJECT_NONE ? 1.0 : 0.0, 0.0, 1.0);

    if (rejections != OLO_SURFACE_REJECT_NONE || !historySampleAvailable)
    {
        reservoir.Diagnostics |= OLO_RESERVOIR_DIAG_TEMPORAL_REJECTED;
        OloPackGIReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }

    OloGIReservoir history = OloUnpackGIReservoir(texture(u_HistoryReservoir0, prevUV),
                                                 texture(u_HistoryReservoir1, prevUV),
                                                 texture(u_HistoryReservoir2, prevUV));

    // THE AGE CAP (design note §10). Applied to the age the sample WOULD have
    // after this merge, so a sample never survives one frame past the cap.
    const uint maxSampleAge = uint(max(u_GIParams.y, 1.0));
    history.Sample.Age = OloGIAdvanceSampleAge(history.Sample.Age);
    if (!OloGISampleAgeAcceptable(history.Sample.Age, maxSampleAge))
    {
        reservoir.Diagnostics |= OLO_GI_DIAG_AGE_EXPIRED | OLO_RESERVOIR_DIAG_TEMPORAL_REJECTED;
        OloPackGIReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }

    // THE SHIFT, from last frame's shading point to this one through the fixed
    // sample vertex. See the header for why this is not DI's exact 1.
    const vec3 previousPoint = PreviousShadingPoint(prevUV, previousSurfacePacked.w);
    const float minReconnection = u_GIParams.x;
    float historyJacobian = 0.0;
    if (OloGIReconnectionInDomain(history.Sample, surface.Position, surface.ShadingNormal, minReconnection))
        historyJacobian = OloGIShiftJacobian(history.Sample, surface.Position, previousPoint);

    // The same CONDITIONING bound the spatial draw applies, and it belongs here
    // too even though a well-reprojected pixel's J is ~1: a disocclusion the #976
    // test happens to accept, or a reprojection that lands a pixel off, is exactly
    // the case that produces a large J - and this is the stage that feeds it back.
    if (!OloGIShiftJacobianAcceptable(historyJacobian))
    {
        // Out of domain, or a degenerate configuration. REJECTED, never scaled:
        // a shift that quietly returns something for an out-of-domain input is
        // how light leaks through a wall with a perfectly smooth falloff.
        reservoir.Diagnostics |= OLO_GI_DIAG_DOMAIN_REJECTED | OLO_RESERVOIR_DIAG_TEMPORAL_REJECTED;
        OloPackGIReservoir(reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }
    const float historyShiftedW = OloShiftedContributionWeight(history.W, historyJacobian);

    // Both reservoirs are re-evaluated against THIS pixel's target function.
    // Re-evaluating rather than trusting the stored TargetPdf is the whole point:
    // the stored value was measured against last frame's surface, and using it
    // would weight a sample by how good it was for a surface that no longer
    // exists.
    const float currentTarget = OloGITargetPdf(surface, reservoir.Sample, viewDirection);
    const float historyTarget = OloGITargetPdf(surface, history.Sample, viewDirection);

    OloGIReservoir merged = OloMakeEmptyGIReservoir();
    merged.Diagnostics = reservoir.Diagnostics | OLO_RESERVOIR_DIAG_TEMPORAL_ACCEPTED;

    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    OloPathSampler pathSampler = oloPtMakeSampler(
        oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), u_TlasAddressAndFrame.w ^ 0x5bf03635u),
        u_TlasAddressAndFrame.w);

    // w_i = m_i * pHat_dest(y_i) * (W_i * J_i), with pHat LEFT ALONE and the
    // JACOBIAN ON THE CONTRIBUTION WEIGHT. The two are not the same thing — they
    // differ by a factor of J — and both produce a plausible image; the
    // derivation is in ReservoirCore.h's ShiftedContributionWeight and design
    // note §4.2.
    //
    // m_i CARRIES THE CONFIDENCE WEIGHT. Under 1/M it is M_i — the reservoir
    // stands for M_i candidates, and FinalizeCombined's summed-M denominator is
    // what divides them back out. Using 1 here darkens the frame by the average
    // M. Under the MIS-weighted mode the numerator ALREADY contains M_i, so
    // multiplying by M_i again double-counts it and the normaliser is 1 instead.
    const bool unbiased = u_ResamplingCounts.w == OLO_RESTIR_BIAS_MODE_UNBIASED_MIS;
    float mCurrent = reservoir.M;
    float mHistory = history.M;
    if (unbiased)
    {
        // Both reservoirs live on the SAME surface (that is what the validity
        // test established), so pHat_current and pHat_history are the same
        // function and the heuristic reduces to the M-weighted split below.
        // Writing it out rather than hard-coding 0.5 keeps the two modes'
        // arithmetic in one shape, so a later change to the target function
        // cannot silently make only one of them right.
        const float denomCurrent = reservoir.M * currentTarget + history.M * currentTarget;
        const float denomHistory = reservoir.M * historyTarget + history.M * historyTarget;
        mCurrent = denomCurrent > 0.0 ? (reservoir.M * currentTarget) / denomCurrent : 0.0;
        mHistory = denomHistory > 0.0 ? (history.M * historyTarget) / denomHistory : 0.0;
    }

    // The centre's own shift is the identity, so its J is exactly 1 and its
    // weight passes through unscaled.
    OloGIReservoirUpdate(merged, reservoir.Sample, mCurrent * currentTarget * reservoir.W, currentTarget,
                         oloPtGet1D(pathSampler));
    OloGIReservoirUpdate(merged, history.Sample, mHistory * historyTarget * historyShiftedW, historyTarget,
                         oloPtGet1D(pathSampler));

    // OloGIReservoirUpdate counted two candidates; the merged confidence is the
    // SUM of the two reservoirs' M, not 2.
    OloGIReservoirFinalizeCombined(merged, u_ResamplingCounts.w, reservoir.M + history.M);
    OloGIReservoirApplyMCap(merged, u_ReuseParams.x);
    if (OloGISampleIsDistant(merged.Sample.Kind))
        merged.Diagnostics |= OLO_GI_DIAG_ENVIRONMENT;

    OloPackGIReservoir(merged, o_Reservoir0, o_Reservoir1, o_Reservoir2);
}
