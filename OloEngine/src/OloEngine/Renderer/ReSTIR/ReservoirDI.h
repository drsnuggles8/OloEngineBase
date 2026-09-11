#pragma once

// =============================================================================
// ReservoirDI.h — the ReSTIR DI reservoir: what its sample IS, and the two
// pieces of arithmetic that depend on that. Issue #1140 (#979 Phase 3).
//
// THE DOMAIN-NEUTRAL HALF MOVED TO ReservoirCore.h (issue #1169). Streaming RIS,
// the two normalisers, the balance heuristic, the M cap, the geometric core of
// the shift and the float-lane encoding do not know what the sample is, and
// ReSTIR GI needs every one of them. What is left here is what a LIGHT SAMPLE
// makes true: the sample kinds, the delta-light arm of the shift, and the
// packed DI plane layout. Nothing this file used to export was renamed —
// ReservoirCore.h is in the same namespace and every old call site still
// resolves. docs/design/restir-gi-reconnection-shift.md §8 states the split rule.
//
// THE RULE THIS FILE EXISTS TO ENFORCE: a resampled estimator that is WRONG
// still looks plausible. Noise reads as noise, and a missing Jacobian or a
// mis-normalised MIS weight reads as "slightly darker contact shadows" — which
// nobody reports as a bug. So the arithmetic lives here and in the core,
// backend-neutral and GPU-free, and is pinned by ReSTIRDIContractTest on a
// machine with no device. Reservoir.glsl is its GLSL twin, function for
// function. Two different seams hold them together and it takes both:
// ReSTIRDIContractTest scans the GLSL SOURCE for the shared constants on a
// machine with no device, and ReSTIRDIReservoirGpuParityTest drives the
// ENCODING through a real RGBA32F target on one that has a device — because the
// failure that actually shipped here was not an arithmetic disagreement. It was
// a STORE that destroyed the value while the arithmetic on both sides was right.
//
// -----------------------------------------------------------------------------
// THE MEASURE CONVENTION, WHICH IS THE WHOLE JACOBIAN QUESTION
// -----------------------------------------------------------------------------
//
// A reservoir stores a POINT ON AN EMITTER (area measure), never a direction.
// That is deliberate and it is what makes reuse across pixels meaningful at
// all: a direction is only interpretable relative to the shading point that
// drew it, so a neighbour's direction is a different sample, while a
// neighbour's emitter POINT is the same sample seen from somewhere else.
//
// The TARGET FUNCTION, however, is evaluated in SOLID-ANGLE measure at the
// shading point that owns the reservoir:
//
//     targetPdf(x) = || f(x) * L(x) * cos(thetaShading) ||
//
// because that is the integrand the renderer actually integrates, and using
// the integrand as the target function is what makes the resampling worth
// doing. Two measures therefore meet inside every reuse, and the conversion
// between them is the shift Jacobian in ShiftJacobian() below. Getting it
// wrong is invisible: it biases the estimate by a smooth factor that varies
// with geometry, which is indistinguishable from "the lighting looks a bit
// off" in any still frame.
//
// THE INVARIANT THAT PINS IT. Applying the Jacobian and evaluating in
// solid-angle measure must give the SAME estimator as evaluating in area
// measure with no Jacobian at all. That identity — not a hand-picked number —
// is what ReSTIRDIContractTest asserts, for randomised geometry. It is the one
// check that fails when the cosine is taken at the wrong end or the distances
// are the wrong way up, which are the two mistakes this term invites.
//
// A DELTA LIGHT HAS NO AREA and therefore no Jacobian: the sample is the same
// point in space seen from both pixels, in a discrete measure that no shift can
// change. ShiftJacobian() returns exactly 1 for it, and that is a fact about
// the measure, not a shortcut.
//
// -----------------------------------------------------------------------------
// WHY BOTH BIAS MODES SHIP, AND BOTH ARE INSPECTABLE
// -----------------------------------------------------------------------------
//
// The 1/M normalisation is biased whenever the reservoirs being combined do not
// all have the same domain — which spatial neighbours never do, because a
// neighbour facing away from a light could not have drawn the sample being
// reused. The bias darkens exactly the regions where reuse helps most. It is
// also cheaper and, at high M, small. Both modes are therefore selectable and
// both are COUNTED, so "which one produced this frame" is never a guess.
// ReservoirCore.h owns BiasMode and the two finalisers.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirCore.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace OloEngine::ReSTIR
{
    // kMinimumShiftCosine and kDefaultTemporalMCap live in ReservoirCore.h; they
    // are properties of a reservoir shift and of temporal reuse, not of a light.

    // -------------------------------------------------------------------------
    // The versioned GPU layout
    // -------------------------------------------------------------------------

    // Bumped whenever the packing in Reservoir.glsl changes shape. The history
    // registry keys its planes on it, so a bump invalidates last frame's
    // reservoirs instead of reinterpreting them — reading a v1 reservoir as v2
    // is not a crash, it is a plausible wrong image, which is the failure this
    // whole file is organised against.
    // v2: every integer field is stored NUMERICALLY rather than as a float
    // bit pattern. v1 reinterpreted small integers, which are DENORMALS as
    // floats, and the GPU flushed them to zero — see Reservoir.glsl.
    inline constexpr u32 kReservoirLayoutVersion = 2u;

    // Which family the stored sample came from. The value is PACKED INTO THE
    // GPU LAYOUT, so append only — never renumber.
    enum class LightSampleKind : u32
    {
        None = 0,             ///< An empty reservoir. Not an error; the normal state before any candidate.
        Punctual = 1,         ///< Point / spot. A delta in position: no area, no Jacobian.
        Directional = 2,      ///< A delta in DIRECTION. Position stores the direction TOWARD the light.
        SphereArea = 3,       ///< A point on a sphere emitter's surface. Cone-sampled in SOLID ANGLE at the source.
        EmissiveTriangle = 4, ///< A point on an emissive triangle. Sampled uniformly by AREA.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(LightSampleKind kind)
    {
        switch (kind)
        {
            case LightSampleKind::None:
                return "none";
            case LightSampleKind::Punctual:
                return "punctual";
            case LightSampleKind::Directional:
                return "directional";
            case LightSampleKind::SphereArea:
                return "sphere area";
            case LightSampleKind::EmissiveTriangle:
                return "emissive triangle";
            case LightSampleKind::Count:
                break;
        }
        return "unknown";
    }

    // A sample with no area to reconnect through: the shift is the identity and
    // the Jacobian is exactly 1. Spelled as a predicate rather than tested
    // inline so the two places that must agree (here and Reservoir.glsl) read
    // the same question.
    //
    // This is DI's DEGENERATE ARM of the shift, and it is the reason
    // ShiftJacobian is a wrapper rather than ReconnectionJacobian itself: GI's
    // degenerate arm is an environment sample, which is a different question in
    // a different measure reaching the same answer.
    [[nodiscard]] constexpr bool IsDeltaLight(LightSampleKind kind)
    {
        return kind == LightSampleKind::Punctual || kind == LightSampleKind::Directional;
    }

    // -------------------------------------------------------------------------
    // The sample
    // -------------------------------------------------------------------------

    // One light sample, in the form every pixel can re-evaluate it in: a POINT
    // (or, for a directional light, a direction), the emitter normal there, and
    // the radiance leaving it. Radiance is CARRIED rather than re-fetched
    // because re-fetching needs the emitter's texture and UV, which a
    // neighbouring pixel reusing the sample has no reason to have loaded — and
    // because a re-fetch that disagreed with the fetch at selection time would
    // silently break the estimator's target-pdf / weight bookkeeping.
    struct LightSample
    {
        LightSampleKind Kind = LightSampleKind::None;
        // GPU Scene light slot for Punctual/Directional/SphereArea; the
        // emissive-table triangle index for EmissiveTriangle. Carried so the
        // debug views can colour by lineage and so a retired light can be
        // detected rather than sampled.
        u32 LightIndex = 0;

        // World (RENDER-RELATIVE, issue #429) point on the emitter. For
        // Directional this is the unit direction TOWARD the light, not a point:
        // the two readings are distinguished by Kind and never by inspecting
        // the vector, which is the mistake RayTracedShadowLightRequest records.
        glm::vec3 Position{ 0.0f };
        // Outward emitter normal at Position. Zero for a delta light, where it
        // has no meaning and is never read.
        glm::vec3 Normal{ 0.0f };
        // Radiance leaving Position toward the shading point that selected it,
        // including the emitter's own texture and, for a punctual light, its
        // distance attenuation and spot cone. Pre-multiplied so a reusing pixel
        // needs no light record at all.
        glm::vec3 Radiance{ 0.0f };

        [[nodiscard]] auto operator==(const LightSample&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // The reservoir
    // -------------------------------------------------------------------------

    // Weighted reservoir sampling state. The five members are the ones the
    // literature names, kept in the literature's order so a reader can check
    // them against the paper:
    //
    //   Sample     y        — the surviving candidate
    //   TargetPdf  pHat(y)  — at the pixel that owns this reservoir
    //   WeightSum  wSum     — the running sum of candidate weights, TRANSIENT
    //   M                   — the confidence weight (a candidate count, kept as
    //                         f32 because the temporal cap makes it fractional)
    //   W                   — the unbiased contribution weight, what shading uses
    //
    // W is stored rather than derived at read time even though it equals
    // wSum / (M * pHat) under 1/M normalisation, because the MIS-weighted
    // mode's normaliser is NOT M and the two would diverge. One field, one
    // meaning.
    //
    // The member NAMES are the interface ReservoirCore.h's templates bind to.
    // ReservoirGI.h's reservoir spells them identically for that reason.
    struct Reservoir
    {
        LightSample Sample{};
        f32 TargetPdf = 0.0f;
        f32 WeightSum = 0.0f;
        f32 M = 0.0f;
        f32 W = 0.0f;

        [[nodiscard]] constexpr bool IsEmpty() const
        {
            return Sample.Kind == LightSampleKind::None || !(M > 0.0f);
        }

        void Reset()
        {
            *this = Reservoir{};
        }

        [[nodiscard]] auto operator==(const Reservoir&) const -> bool = default;
    };

    // Every float in a reservoir finite. Applied wherever a reservoir is READ
    // BACK from a texture, because a NaN that reaches WeightSum poisons every
    // later combine at that pixel and then spreads through spatial reuse — one
    // bad pixel becomes a growing blot, which reads as a renderer bug of some
    // entirely different kind.
    [[nodiscard]] inline bool IsFinite(const Reservoir& r)
    {
        return std::isfinite(r.TargetPdf) && std::isfinite(r.WeightSum) && std::isfinite(r.M) &&
               std::isfinite(r.W) && IsFiniteVec3(r.Sample.Position) && IsFiniteVec3(r.Sample.Normal) &&
               IsFiniteVec3(r.Sample.Radiance);
    }

    // A reservoir that cannot be trusted becomes EMPTY, never clamped. Clamping
    // a garbage reservoir keeps its M, and M is a claim about how many
    // candidates were seen — a false one suppresses every future candidate at
    // that pixel, so the corruption becomes permanent instead of transient.
    [[nodiscard]] inline Reservoir SanitizeReservoir(const Reservoir& in)
    {
        if (!IsFinite(in))
            return Reservoir{};
        if (std::to_underlying(in.Sample.Kind) >= std::to_underlying(LightSampleKind::Count))
            return Reservoir{};
        if (in.TargetPdf < 0.0f || in.WeightSum < 0.0f || in.M < 0.0f || in.W < 0.0f)
            return Reservoir{};
        return in;
    }

    // -------------------------------------------------------------------------
    // The shift, and its Jacobian — DI's wrapper around the geometric core
    // -------------------------------------------------------------------------

    // The reconnection shift's Jacobian, converting a sample whose density was
    // expressed in SOLID ANGLE at `sourceShadingPoint` into a density in solid
    // angle at `destShadingPoint`, through the fixed emitter point.
    //
    //     J = ( cos(phiDest) / cos(phiSource) ) * ( dSourceSq / dDestSq )
    //
    // where phi is the angle at the EMITTER between its normal and the vector to
    // the shading point, and d is the distance from the shading point to the
    // emitter point. Both cosines are taken at the emitter end — taking them at
    // the shading end is the plausible-looking mistake, and it is wrong because
    // the shading-point cosine belongs to the integrand, not to the measure.
    // ReconnectionJacobian in ReservoirCore.h is that expression.
    //
    // WHAT THIS WRAPPER ADDS, and the only thing it adds: the DELTA-LIGHT ARM.
    // A delta light is the same point in a discrete measure from both pixels, so
    // there is nothing to convert. That is a statement about DI's sample kinds,
    // which is exactly why it cannot live in the core.
    //
    // Returns 0 for a degenerate configuration (the source saw the emitter
    // edge-on, or the destination coincides with it). Zero means REJECT this
    // reuse; it never means "no change", which 1 would.
    [[nodiscard]] inline f32 ShiftJacobian(const LightSample& sample, const glm::vec3& destShadingPoint,
                                           const glm::vec3& sourceShadingPoint)
    {
        if (IsDeltaLight(sample.Kind))
            return 1.0f;
        return ReconnectionJacobian(sample.Position, sample.Normal, destShadingPoint, sourceShadingPoint);
    }

    // The area-to-solid-angle conversion, used to state the measure identity the
    // contract test pins: a density in area measure at the emitter is
    // areaPdf * dSq / cos in solid angle at the shading point.
    //
    // The delta arm is this wrapper's, for the same reason as above: a delta
    // light's "area pdf" is already a probability and passes through unchanged.
    [[nodiscard]] inline f32 AreaPdfToSolidAnglePdf(f32 areaPdf, const LightSample& sample,
                                                    const glm::vec3& shadingPoint)
    {
        if (IsDeltaLight(sample.Kind))
            return areaPdf;
        return SolidAnglePdfFromAreaPdf(areaPdf, sample.Position, sample.Normal, shadingPoint);
    }

    // -------------------------------------------------------------------------
    // Combining reservoirs
    // -------------------------------------------------------------------------

    // WHAT A COMBINE NEEDS, AND WHY IT IS NOT A STRUCT HERE. Merging reservoir i
    // into a destination needs three things the reservoir does not carry: the
    // shading point it came from (for the Jacobian), its target function
    // re-evaluated AT THE DESTINATION, and the shift Jacobian between the two.
    // The caller supplies all three because only it has the destination's BSDF
    // and normal — which is what keeps this file free of the closure and testable
    // without one — and it assembles them however suits it: the shader needs an
    // ARRAY of them (ReSTIR_DI_SpatialReuse.glsl's OloSpatialCandidate), and a
    // caller merging two reservoirs needs three locals. A struct here would have
    // served neither and would be a type nothing instantiates.
    //
    // The one rule that binds every caller: the Jacobian goes on the CONTRIBUTION
    // WEIGHT (ShiftedContributionWeight in ReservoirCore.h), never on the target
    // function.
    //
    // ReservoirUpdate, FinalizeInitialCandidates, FinalizeCombined,
    // ApplyTemporalMCap and BalanceHeuristicMISWeight are ReservoirCore.h's, as
    // templates over the reservoir and sample types. Calls that named them
    // before this split still resolve unchanged.

    // -------------------------------------------------------------------------
    // The packed DI plane layout
    // -------------------------------------------------------------------------
    //
    // The ENCODING — the numeric packing, the octahedral normal, and the round
    // that must not be `+ 0.5` — is ReservoirCore.h's, because none of it knows
    // what the sample is. What is DI's is the ceiling below, because what the
    // identity lane's payload MEANS is a light index.

    // The largest light index the identity lane can carry. Punctual and sphere
    // lights cannot reach it (OLO_LIGHT_MAX_SLOTS is 256), but an emissive
    // triangle index is a triangle count, and nothing about a two-million-
    // triangle emissive set is absurd. So the pass REFUSES to publish a table it
    // cannot address rather than encoding wrapped indices — see its use in
    // ReSTIRDIPass.
    inline constexpr u32 kMaxEncodableLightIndex = kMaxEncodablePayloadIndex;

    [[nodiscard]] inline bool IsEncodableLightIndex(u32 lightIndex)
    {
        return IsEncodablePayloadIndex(lightIndex);
    }

} // namespace OloEngine::ReSTIR
