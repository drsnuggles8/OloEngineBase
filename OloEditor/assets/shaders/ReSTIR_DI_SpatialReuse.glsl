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
// ReSTIR DI, draw C of four: SPATIAL REUSE, with the shift Jacobian and both
// bias modes. Issue #1140 (#979 Phase 3).
//
// THIS IS THE DRAW WHERE BEING WRONG LOOKS RIGHT. Spatial reuse pulls samples
// from pixels that are genuinely different surface points, so two things that
// are easy to omit and invisible when omitted both bite here:
//
//   1. THE JACOBIAN. The neighbour's sample was drawn with a density expressed
//      at the NEIGHBOUR's shading point. Reusing it at this pixel without the
//      measure conversion scales the estimate by a smooth geometric factor —
//      brighter where the neighbour saw the emitter more edge-on, darker where
//      it saw it more face-on. That reads as a soft gradient across a wall,
//      which nobody files as a bug. OloReservoirShiftJacobian does the
//      conversion and REJECTS (returns 0) rather than scaling when the
//      neighbour saw the emitter edge-on, because the alternative is a firefly
//      that temporal reuse then keeps alive for as long as the M cap allows.
//
//   2. THE NORMALISATION. 1/M is only unbiased when every merged reservoir
//      COULD have produced the surviving sample. Spatial neighbours routinely
//      could not — a neighbour facing away from the light has zero target
//      function on that sample — so 1/M under-weights the survivor and the
//      image darkens exactly where reuse helps most. The MIS-weighted mode is
//      the generalised balance heuristic over the candidate set, which is
//      unbiased and costs one target-function evaluation per (neighbour,
//      candidate) pair. Both ship, both are selectable, and which one ran is
//      reported — not inferred from the settings, because the pass clamps.
//
// The balance heuristic is O(k^2) target evaluations for k candidates. That is
// the mode's real price rather than a constant factor, and it is why the
// neighbour count is clamped to OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS and why the
// default is 4 rather than 16. No RAYS are traced here: the target function is
// deliberately unshadowed, so the cost is arithmetic and texture fetches.
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

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_Reservoir0 OLO_HEAP_TEX_2D(0)
#define u_Reservoir1 OLO_HEAP_TEX_2D(1)
#define u_Reservoir2 OLO_HEAP_TEX_2D(2)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45)
#else
layout(binding = 0) uniform sampler2D u_Reservoir0; // the previous draw in the chain
layout(binding = 1) uniform sampler2D u_Reservoir1;
layout(binding = 2) uniform sampler2D u_Reservoir2;
layout(binding = 19) uniform sampler2D u_DepthTexture;
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;
layout(binding = 44) uniform sampler2D u_GBufferNormal;
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
#endif

#include "include/SkyDepth.glsl"
#include "include/PBRCommon.glsl"
#include "include/ReSTIRDISceneAccess.glsl"

// One candidate offered to the combine.
//
// It deliberately does NOT hold the neighbour's surface. An array of
// OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS + 1 surfaces is around 240 floats of
// per-invocation storage on top of the reservoirs, which spills to local memory
// and costs more than the re-fetch it saves — and the MIS loop below is already
// O(k^2) in TEXTURE FETCHES by construction, which is the cost this mode was
// documented as having. So the UV is kept and the surface is loaded again where
// it is needed, which also makes the code match its own cost model instead of
// quietly having a different one.
struct OloSpatialCandidate
{
    bool Valid;
    vec2 UV;
    OloReservoir Reservoir;
    // The neighbour's own shading point, for the Jacobian. Three floats, kept
    // because every candidate needs it and re-deriving it would mean a second
    // depth fetch and a second matrix multiply per candidate.
    vec3 ShadingPoint;
    // pHat at THIS pixel on this candidate's sample, UNSCALED. Zero means the
    // reuse was rejected.
    float TargetAtDestination;
    // The candidate's contribution weight carried through the shift: W * J. THE
    // JACOBIAN MULTIPLIES W — it does not divide the target function; see
    // OloShiftedContributionWeight for the derivation.
    float ShiftedW;
};

// The surface at one UV, or an invalid one for a sky / unlit pixel. Called both
// by the gather and by the MIS loop, so the two cannot disagree about what a
// neighbour's surface is.
OloReSTIRSurface LoadNeighbourSurface(vec2 uv)
{
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return OloReSTIRInvalidSurface();
    const float depth = texture(u_DepthTexture, uv).r;
    const vec4 packedEmissive = texture(u_GBufferEmissive, uv);
    const int gbFlags = oloDecodeGBufferFlags(packedEmissive.a);
    if (oloDepthIsSky(depth) || oloGBufferFlagsAreUnlit(gbFlags))
        return OloReSTIRInvalidSurface();
    return OloReSTIRLoadSurface(uv, depth, texture(u_GBufferNormal, uv), texture(u_GBufferAlbedo, uv),
                                oloGBufferFlagsPbrModel(gbFlags));
}

// The reservoir and surface at one UV. Valid is false for a sky or unlit pixel,
// which is a REJECTION rather than an empty reservoir: an unlit pixel has no
// surface to compute a Jacobian against.
OloSpatialCandidate LoadCandidate(vec2 uv, out OloReSTIRSurface outSurface)
{
    OloSpatialCandidate c;
    c.Valid = false;
    c.UV = uv;
    c.Reservoir = OloMakeEmptyReservoir();
    c.ShadingPoint = vec3(0.0);
    c.TargetAtDestination = 0.0;
    c.ShiftedW = 0.0;

    outSurface = LoadNeighbourSurface(uv);
    if (!outSurface.Valid)
        return c;

    c.ShadingPoint = outSurface.Position;
    c.Reservoir =
        OloUnpackReservoir(texture(u_Reservoir0, uv), texture(u_Reservoir1, uv), texture(u_Reservoir2, uv));
    c.Valid = true;
    return c;
}

// Whether a neighbour is similar enough to reuse from at all. This is the
// SPATIAL twin of the #976 temporal validity test, and it exists for the same
// reason: a neighbour across a depth discontinuity or a normal crease is not
// the same lighting environment, and reusing across one is what makes ReSTIR
// leak light around silhouettes. It is a similarity gate, not a correctness
// requirement — the Jacobian and the MIS weight handle correctness; this only
// keeps variance down and the Jacobian well-conditioned.
bool NeighbourAcceptable(OloReSTIRSurface centre, OloReSTIRSurface neighbour)
{
    if (!neighbour.Valid)
        return false;
    if (dot(centre.ShadingNormal, neighbour.ShadingNormal) < 0.9)
        return false;
    const float depthDenominator = max(max(abs(centre.ViewDepth), abs(neighbour.ViewDepth)), 1.0e-4);
    if (abs(centre.ViewDepth - neighbour.ViewDepth) / depthDenominator > 0.05)
        return false;
    return abs(centre.Roughness - neighbour.Roughness) <= 0.1;
}

void main()
{
    OloReSTIRSurface centreSurface;
    OloSpatialCandidate centre = LoadCandidate(v_TexCoord, centreSurface);
    if (!centre.Valid || (u_EmissiveTable.w & OLO_RESTIR_FLAG_SPATIAL_REUSE) == 0u)
    {
        // Pass through. As in the temporal draw, standing down leaves a valid
        // reservoir behind — noisier, never wrong.
        OloPackReservoir(centre.Reservoir, o_Reservoir0, o_Reservoir1, o_Reservoir2);
        return;
    }

    const vec3 viewDirection = normalize((u_InvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz - centreSurface.Position);
    const uint neighbourCount = min(u_ResamplingCounts.y, OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS);
    const bool unbiased = u_ResamplingCounts.w == OLO_RESTIR_BIAS_MODE_UNBIASED_MIS;

    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    // The pass index is folded into the seed so a second spatial pass draws a
    // DIFFERENT neighbour set. Without it, two passes at the same radius would
    // resample the same neighbours and the second pass would only re-normalise.
    OloPathSampler pathSampler = oloPtMakeSampler(
        oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y),
                           (u_TlasAddressAndFrame.w * 31u + u_ResamplingCounts.z) ^ 0x9e3779b9u),
        u_TlasAddressAndFrame.w);

    // ---- gather -----------------------------------------------------------
    //
    // The centre is candidate 0 and always participates: dropping it would make
    // a pixel whose neighbours are all rejected lose its own sample.
    OloSpatialCandidate candidates[OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS + 1];
    candidates[0] = centre;
    // The centre's own shift is the identity, so J is exactly 1 and its weight
    // passes through unscaled.
    candidates[0].ShiftedW = centre.Reservoir.W;
    candidates[0].TargetAtDestination =
        OloReSTIRTargetPdf(centreSurface, centre.Reservoir.Sample, viewDirection);
    uint count = 1u;

    uint jacobianRejections = 0u;
    for (uint i = 0u; i < OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS; ++i)
    {
        if (i >= neighbourCount)
            break;
        // Dimensions drawn unconditionally, before any acceptance test, so
        // neighbouring pixels do not correlate through a sampler whose
        // dimension index depends on which neighbours happened to be valid.
        const vec2 xiDisc = oloPtGet2D(pathSampler);

        // A uniform disc sample, not a fixed kernel: a fixed offset pattern
        // makes every pixel in a region reuse the same neighbours, which
        // correlates them and shows up as a repeating texture rather than as
        // noise that the temporal filter can average away.
        const float radius = u_ReuseParams.y * sqrt(clamp(xiDisc.x, 0.0, 1.0));
        const float angle = 6.28318530718 * xiDisc.y;
        const vec2 offset = vec2(cos(angle), sin(angle)) * radius * u_ScreenParams.zw;

        OloReSTIRSurface neighbourSurface;
        OloSpatialCandidate neighbour = LoadCandidate(v_TexCoord + offset, neighbourSurface);
        if (!NeighbourAcceptable(centreSurface, neighbourSurface) ||
            OloReservoirIsEmpty(neighbour.Reservoir) || !(neighbour.Reservoir.W > 0.0))
        {
            continue;
        }

        // THE JACOBIAN. From the neighbour's shading point to this one, through
        // the fixed emitter point.
        const float jacobian = OloReservoirShiftJacobian(neighbour.Reservoir.Sample, centreSurface.Position,
                                                        neighbour.ShadingPoint);
        if (!(jacobian > 0.0))
        {
            jacobianRejections += 1u;
            continue;
        }

        // pHat at THIS pixel, UNSCALED, plus the neighbour's contribution weight
        // carried through the shift. THE DIRECTION IS THE WHOLE POINT, and it is
        // easy to get backwards: W behaves as 1/p in the measure its source
        // density was expressed in, and 1/p_dest = (1/p_source) * J — so J
        // MULTIPLIES W and leaves pHat alone. Dividing pHat by J instead leaves
        // the estimate off by a factor of J, which is a smooth geometric
        // brightness error rather than anything that looks wrong.
        const float target = OloReSTIRTargetPdf(centreSurface, neighbour.Reservoir.Sample, viewDirection);
        if (!(target > 0.0))
            continue;
        const float shiftedW = OloShiftedContributionWeight(neighbour.Reservoir.W, jacobian);
        if (!(shiftedW > 0.0))
            continue;

        neighbour.ShiftedW = shiftedW;
        neighbour.TargetAtDestination = target;
        candidates[count] = neighbour;
        count += 1u;
    }

    // ---- combine ----------------------------------------------------------
    OloReservoir merged = OloMakeEmptyReservoir();
    merged.Diagnostics = centre.Reservoir.Diagnostics;
    if (count > 1u)
        merged.Diagnostics |= OLO_RESERVOIR_DIAG_SPATIAL_ACCEPTED;
    if (jacobianRejections != 0u)
        merged.Diagnostics |= OLO_RESERVOIR_DIAG_JACOBIAN_REJECTED;

    float summedM = 0.0;
    for (uint i = 0u; i < OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS + 1u; ++i)
    {
        if (i >= count)
            break;
        summedM += candidates[i].Reservoir.M;
    }

    for (uint i = 0u; i < OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS + 1u; ++i)
    {
        if (i >= count)
            break;

        // m_i, the MIS weight, and it CARRIES THE CONFIDENCE WEIGHT.
        //
        // Under 1/M it is M_i — the reservoir stands for M_i candidates and
        // FinalizeCombined's summed-M denominator divides them back out. Using 1
        // here darkens the frame by the average M (a factor of 8 at the default
        // eight initial candidates), which ReSTIRDIOracleTest measured against
        // dense quadrature. Under the balance heuristic it is
        // M_i * pHat_i(y_i) / sum_j M_j * pHat_j(y_i), whose numerator ALREADY
        // contains M_i, so multiplying by M_i again double-counts it and the
        // normaliser is 1 instead. Applying a normaliser BOTH times is the
        // classic "normalised twice" bug, which is why the two live in one place.
        float m = candidates[i].Reservoir.M;
        if (unbiased)
        {
            // pHat_j evaluated on candidate i's sample, at candidate j's OWN
            // surface — that is what makes this the balance heuristic over the
            // candidates' domains rather than a re-weighting at one pixel.
            float denominator = 0.0;
            float numerator = 0.0;
            for (uint j = 0u; j < OLO_RESTIR_MAX_SPATIAL_NEIGHBOURS + 1u; ++j)
            {
                if (j >= count)
                    break;
                // The surface is re-loaded rather than cached in the candidate
                // array: see OloSpatialCandidate's comment. This is the O(k^2)
                // fetch cost this mode is documented as having.
                const OloReSTIRSurface surfaceJ =
                    (j == 0u) ? centreSurface : LoadNeighbourSurface(candidates[j].UV);
                const float targetAtJ =
                    OloReSTIRTargetPdf(surfaceJ, candidates[i].Reservoir.Sample, viewDirection);
                if (targetAtJ > 0.0)
                    denominator += candidates[j].Reservoir.M * targetAtJ;
                if (j == i)
                    numerator = candidates[i].Reservoir.M * targetAtJ;
            }
            m = (denominator > 0.0 && numerator > 0.0) ? clamp(numerator / denominator, 0.0, 1.0) : 0.0;
        }

        // w_i = m_i * pHat_dest(y_i) * (W_i * J_i).
        OloReservoirUpdate(merged, candidates[i].Reservoir.Sample,
                           m * candidates[i].TargetAtDestination * candidates[i].ShiftedW,
                           candidates[i].TargetAtDestination, oloPtGet1D(pathSampler));
    }

    OloReservoirFinalizeCombined(merged, u_ResamplingCounts.w, summedM);
    // The cap applies here too: this reservoir is what next frame's temporal
    // draw reads as history, and spatial reuse inflates M by the neighbour
    // count. Without the cap here, one spatial pass at four neighbours would
    // multiply the effective history length by five per frame.
    OloReservoirApplyMCap(merged, u_ReuseParams.x);

    OloPackReservoir(merged, o_Reservoir0, o_Reservoir1, o_Reservoir2);
}
