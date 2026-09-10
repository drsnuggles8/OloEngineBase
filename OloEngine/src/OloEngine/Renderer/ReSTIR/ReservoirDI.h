#pragma once

// =============================================================================
// ReservoirDI.h — the ReSTIR DI reservoir, its measure convention, and the
// three pieces of arithmetic that decide whether the estimator is right.
// Issue #1140 (#979 Phase 3).
//
// THE RULE THIS FILE EXISTS TO ENFORCE: a resampled estimator that is WRONG
// still looks plausible. Noise reads as noise, and a missing Jacobian or a
// mis-normalised MIS weight reads as "slightly darker contact shadows" — which
// nobody reports as a bug. So the arithmetic lives here, backend-neutral and
// GPU-free, and is pinned by ReSTIRDIContractTest on a machine with no device.
// Reservoir.glsl is its GLSL twin, function for function. Two different seams
// hold them together and it takes both: ReSTIRDIContractTest scans the GLSL
// SOURCE for the shared constants on a machine with no device, and
// ReSTIRDIReservoirGpuParityTest drives the ENCODING through a real RGBA32F
// target on one that has a device — because the failure that actually shipped
// here was not an arithmetic disagreement. It was a STORE that destroyed the
// value while the arithmetic on both sides was right.
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
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace OloEngine::ReSTIR
{
    // -------------------------------------------------------------------------
    // Tolerances, named once
    // -------------------------------------------------------------------------

    // The smallest emitter-side cosine a shift is allowed to divide by. Below
    // it the reuse is rejected rather than scaled: an edge-on source puts a
    // vanishing cosine in the Jacobian's DENOMINATOR, and the resulting firefly
    // is then kept alive for a hundred frames by temporal reuse.
    inline constexpr f32 kMinimumShiftCosine = 1.0e-4f;

    // The default cap on M for temporal reuse. Capping is what bounds how long
    // a stale sample can survive a change in the scene; without it a pixel that
    // has been still for a thousand frames ignores the light that just turned
    // on. 20x the initial candidate count is the literature's rule of thumb and
    // is a TUNABLE, not a constant of nature — ReSTIRDISettings owns it.
    inline constexpr f32 kDefaultTemporalMCap = 20.0f;

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
        const auto finite3 = [](const glm::vec3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        return std::isfinite(r.TargetPdf) && std::isfinite(r.WeightSum) && std::isfinite(r.M) &&
               std::isfinite(r.W) && finite3(r.Sample.Position) && finite3(r.Sample.Normal) &&
               finite3(r.Sample.Radiance);
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
    // Streaming RIS
    // -------------------------------------------------------------------------

    // Add one candidate. `weight` is the RIS weight pHat(x) / p(x) for an
    // initial candidate, or m * pHat(x) * W for a reservoir being merged.
    // `xi` is a fresh uniform in [0,1).
    //
    // The test is written `xi * WeightSum < weight` rather than
    // `xi < weight / WeightSum` for one reason that matters: the first form is
    // FALSE when WeightSum is zero, which is exactly the "nothing has been
    // added yet and this candidate is worthless" case, while the second divides
    // by zero and selects on a NaN comparison whose result is
    // implementation-defined. GLSL has the same trap, and Reservoir.glsl spells
    // it the same way.
    //
    // Returns whether the candidate replaced the incumbent — the caller needs
    // it to carry along any per-sample bookkeeping the reservoir does not hold.
    inline bool ReservoirUpdate(Reservoir& r, const LightSample& sample, f32 weight, f32 targetPdf, f32 xi)
    {
        // A non-finite or negative weight is DROPPED, not clamped: clamping it
        // would fold a garbage candidate into M and quietly lower every later
        // candidate's chance of being picked.
        if (!std::isfinite(weight) || weight < 0.0f || !std::isfinite(targetPdf) || targetPdf < 0.0f)
            return false;

        r.M += 1.0f;
        r.WeightSum += weight;
        if (xi * r.WeightSum < weight)
        {
            r.Sample = sample;
            r.TargetPdf = targetPdf;
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // Normalisation — the two bias modes
    // -------------------------------------------------------------------------

    enum class BiasMode : u32
    {
        // 1/M. Cheap, and biased whenever the combined reservoirs do not share
        // a domain. The bias is a DARKENING at exactly the pixels where reuse
        // helps most, so it is not a uniform offset an exposure tweak could
        // hide.
        Biased = 0,
        // The generalised balance heuristic (Talbot MIS). Unbiased, and costs
        // one target-function evaluation per (neighbour, selected sample) pair,
        // which is the real price of the mode rather than a constant factor.
        UnbiasedMIS = 1,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(BiasMode mode)
    {
        switch (mode)
        {
            case BiasMode::Biased:
                return "biased (1/M)";
            case BiasMode::UnbiasedMIS:
                return "unbiased (MIS-weighted)";
            case BiasMode::Count:
                break;
        }
        return "unknown";
    }

    // W = weightSum / (normaliser * targetPdf).
    //
    // `normaliser` is M under 1/M and 1 under a MIS-weighted combine, where the
    // per-candidate MIS weights already sum to one inside weightSum. Passing it
    // in rather than branching on the mode is what lets the contract test drive
    // both arms through one function and catch a normaliser applied twice.
    [[nodiscard]] inline f32 ComputeContributionWeight(f32 weightSum, f32 normaliser, f32 targetPdf)
    {
        if (!(weightSum > 0.0f) || !(normaliser > 0.0f) || !(targetPdf > 0.0f))
            return 0.0f;
        const f32 w = weightSum / (normaliser * targetPdf);
        return std::isfinite(w) ? w : 0.0f;
    }

    // Finish a reservoir built by streaming RIS over independent candidates
    // drawn from a source pdf at ONE pixel. Always 1/M: those candidates DO
    // share a domain, so 1/M is the correct unbiased normaliser and the bias
    // mode does not apply. That distinction is why this is its own function.
    inline void FinalizeInitialCandidates(Reservoir& r)
    {
        r.W = ComputeContributionWeight(r.WeightSum, r.M, r.TargetPdf);
    }

    // -------------------------------------------------------------------------
    // The shift, and its Jacobian
    // -------------------------------------------------------------------------

    // The reconnection shift's Jacobian, converting a sample whose density was
    // expressed in SOLID ANGLE at `sourceShadingPoint` into a density in solid
    // angle at `destShadingPoint`, through the fixed emitter point.
    //
    //     J = ( cos(phiDest) / cos(phiSource) ) * ( dSourceSq / dDestSq )
    //
    // where phi is the angle at the EMITTER between its normal and the vector
    // to the shading point, and d is the distance from the shading point to the
    // emitter point. Both cosines are taken at the emitter end — taking them at
    // the shading end is the plausible-looking mistake, and it is wrong because
    // the shading-point cosine belongs to the integrand, not to the measure.
    //
    // Returns 0 for a degenerate configuration (the source saw the emitter
    // edge-on, or the destination coincides with it). Zero means REJECT this
    // reuse; it never means "no change", which 1 would.
    [[nodiscard]] inline f32 ShiftJacobian(const LightSample& sample, const glm::vec3& destShadingPoint,
                                           const glm::vec3& sourceShadingPoint)
    {
        // A delta light is the same point in a discrete measure from both
        // pixels. There is nothing to convert.
        if (IsDeltaLight(sample.Kind))
            return 1.0f;

        const glm::vec3 toDest = destShadingPoint - sample.Position;
        const glm::vec3 toSource = sourceShadingPoint - sample.Position;
        const f32 destDistanceSq = glm::dot(toDest, toDest);
        const f32 sourceDistanceSq = glm::dot(toSource, toSource);
        if (!(destDistanceSq > 0.0f) || !(sourceDistanceSq > 0.0f))
            return 0.0f;

        const f32 normalLengthSq = glm::dot(sample.Normal, sample.Normal);
        if (!(normalLengthSq > 0.0f))
            return 0.0f;
        const glm::vec3 n = sample.Normal / std::sqrt(normalLengthSq);

        const f32 cosDest = std::abs(glm::dot(n, toDest / std::sqrt(destDistanceSq)));
        const f32 cosSource = std::abs(glm::dot(n, toSource / std::sqrt(sourceDistanceSq)));
        if (!(cosSource > kMinimumShiftCosine))
            return 0.0f;

        const f32 jacobian = (cosDest / cosSource) * (sourceDistanceSq / destDistanceSq);
        return std::isfinite(jacobian) && jacobian >= 0.0f ? jacobian : 0.0f;
    }

    // Carry a reservoir's contribution weight through the shift.
    //
    // THE JACOBIAN MULTIPLIES W. IT DOES NOT DIVIDE THE TARGET FUNCTION. Those
    // two are not the same thing — they differ by a factor of J — and both
    // produce a plausible image, so the derivation is written out here rather
    // than left to be re-derived at each call site:
    //
    //   A reservoir's W behaves as 1/p(y) in the measure its source density was
    //   expressed in. An emitter point sampled uniformly by AREA has the same
    //   area density at every pixel, so the two pixels' SOLID-ANGLE densities
    //   differ only by geometry:
    //
    //       p_source(y) = pArea(y) * dSourceSq / cosSource
    //       p_dest(y)   = pArea(y) * dDestSq   / cosDest
    //
    //   hence
    //
    //       1/p_dest = (1/p_source) * (cosDest/cosSource) * (dSourceSq/dDestSq)
    //                = W_source * ShiftJacobian(sample, dest, source)
    //
    // So a merged candidate's RIS weight is  m * pHat_dest(y) * ShiftedW, with
    // pHat_dest LEFT ALONE. Dividing pHat by J instead is off by J^2 in the
    // weight and by J in the final estimate — a smooth geometric brightness
    // error, which is exactly the shape nobody reports.
    //
    // AND m CARRIES THE CONFIDENCE WEIGHT. Under 1/M normalisation m_i is M_i,
    // not 1: the reservoir stands for M_i candidates and the summed M in the
    // denominator is what divides them back out. Using 1 there darkens the image
    // by the average M — a factor of 8 at eight candidates per reservoir, and
    // more once the temporal cap raises M. Under the MIS-weighted mode m_i is the
    // balance heuristic, whose numerator ALREADY contains M_i, so multiplying by
    // M_i again double-counts it. That asymmetry is why the two modes cannot
    // share one literal, and it is what ReSTIRDIOracleTest measures.
    [[nodiscard]] inline f32 ShiftedContributionWeight(f32 sourceW, f32 jacobian)
    {
        if (!(sourceW > 0.0f) || !(jacobian > 0.0f))
            return 0.0f;
        const f32 w = sourceW * jacobian;
        return std::isfinite(w) ? w : 0.0f;
    }

    // The area-to-solid-angle conversion, used to state the measure identity the
    // contract test pins: a density in area measure at the emitter is
    // areaPdf * dSq / cos in solid angle at the shading point. Written once,
    // here, so the shader and the test cannot each grow their own copy.
    [[nodiscard]] inline f32 AreaPdfToSolidAnglePdf(f32 areaPdf, const LightSample& sample,
                                                    const glm::vec3& shadingPoint)
    {
        if (IsDeltaLight(sample.Kind))
            return areaPdf;
        if (!(areaPdf > 0.0f))
            return 0.0f;
        const glm::vec3 toShading = shadingPoint - sample.Position;
        const f32 distanceSq = glm::dot(toShading, toShading);
        const f32 normalLengthSq = glm::dot(sample.Normal, sample.Normal);
        if (!(distanceSq > 0.0f) || !(normalLengthSq > 0.0f))
            return 0.0f;
        const f32 cosLight = std::abs(
            glm::dot(sample.Normal / std::sqrt(normalLengthSq), toShading / std::sqrt(distanceSq)));
        if (!(cosLight > kMinimumShiftCosine))
            return 0.0f;
        const f32 pdf = areaPdf * distanceSq / cosLight;
        return std::isfinite(pdf) ? pdf : 0.0f;
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
    // WEIGHT (ShiftedContributionWeight above), never on the target function.

    // The per-candidate MIS weight for the generalised balance heuristic:
    //
    //     m_i(y) = M_i * pHat_i(y) / sum_j M_j * pHat_j(y)
    //
    // `targetPdfMatrix[i * count + j]` is pHat_i(y_j) — candidate i's target
    // function evaluated on candidate j's sample. The matrix form is
    // deliberate: it is what a CPU test can build exactly, and it makes the
    // symmetry the balance heuristic depends on visible instead of implicit.
    //
    // Returns 0 when the denominator vanishes, which happens only when no
    // reservoir could have produced that sample — a state the caller must treat
    // as "reject", never as "weight 1".
    [[nodiscard]] inline f32 BalanceHeuristicMISWeight(u32 selectedIndex, u32 sampleOwnerIndex,
                                                       const f32* targetPdfMatrix, const f32* confidenceM,
                                                       u32 count)
    {
        if (targetPdfMatrix == nullptr || confidenceM == nullptr || count == 0u || selectedIndex >= count ||
            sampleOwnerIndex >= count)
        {
            return 0.0f;
        }
        f32 denominator = 0.0f;
        for (u32 j = 0; j < count; ++j)
        {
            const f32 term = confidenceM[j] * targetPdfMatrix[j * count + sampleOwnerIndex];
            if (std::isfinite(term) && term > 0.0f)
                denominator += term;
        }
        if (!(denominator > 0.0f))
            return 0.0f;
        const f32 numerator =
            confidenceM[selectedIndex] * targetPdfMatrix[selectedIndex * count + sampleOwnerIndex];
        if (!std::isfinite(numerator) || !(numerator > 0.0f))
            return 0.0f;
        const f32 m = numerator / denominator;
        return std::isfinite(m) ? std::clamp(m, 0.0f, 1.0f) : 0.0f;
    }

    // Finish a reservoir built by merging OTHER reservoirs.
    //
    //   Biased      — normaliser is the summed M. Correct only when every
    //                 merged reservoir could have produced the surviving
    //                 sample, which spatial neighbours generally could not.
    //   UnbiasedMIS — normaliser is 1, because the caller already folded each
    //                 candidate's MIS weight into its update weight. Passing a
    //                 summed M here as well is the "normalised twice" bug the
    //                 shared ComputeContributionWeight exists to make visible.
    inline void FinalizeCombined(Reservoir& r, BiasMode mode, f32 summedM)
    {
        const f32 normaliser = (mode == BiasMode::UnbiasedMIS) ? 1.0f : summedM;
        r.M = summedM;
        r.W = ComputeContributionWeight(r.WeightSum, normaliser, r.TargetPdf);
    }

    // Cap M after a temporal merge. Rescaling WeightSum with it is what keeps W
    // unchanged across the cap — capping M alone would silently multiply the
    // pixel's radiance by (cap / M), which brightens exactly the pixels that
    // have been stable longest.
    inline void ApplyTemporalMCap(Reservoir& r, f32 cap)
    {
        if (!(cap > 0.0f) || !(r.M > cap))
            return;
        const f32 scale = cap / r.M;
        r.WeightSum *= scale;
        r.M = cap;
    }

    // -------------------------------------------------------------------------
    // THE GPU LAYOUT'S ENCODING, AND WHY IT IS ARITHMETIC RATHER THAN BITS
    // -------------------------------------------------------------------------
    //
    // Reservoirs live in screen-space RGBA32F RENDER TARGETS, not storage
    // buffers: there is no free SSBO binding left (79 taken of an 80 floor). So
    // every integer field has to survive a trip through a float lane.
    //
    // THE OBVIOUS ENCODING IS BROKEN. Reinterpreting `kind | index << 3` as a
    // float makes a DENORMAL — 401 becomes 5.6e-43 — and a GPU is permitted to
    // flush denormals to zero. This one does. The measured result was every
    // reservoir reading back as kind None, the resolve writing black, and every
    // counter still reporting the tier active, with nothing in the log. That was
    // layout v1.
    //
    // So each integer is stored as its own NUMERIC value. An f32 is exact on
    // every integer to 2^24, far above what these fields need, and no value in
    // the encoding is ever denormal.
    //
    // These are the C++ twins of OloPackReservoirIdentity /
    // OloUnpackReservoirIdentity / OloPackReservoirNormal /
    // OloUnpackReservoirNormal in Reservoir.glsl, and
    // ReSTIRDIReservoirGpuParityTest drives both sides with the same inputs
    // THROUGH a real RGBA32F target — which is the only arrangement that can
    // catch the v1 failure, because it is the storage round-trip that loses the
    // value, not the arithmetic.
    inline constexpr u32 kReservoirKindBits = 3u;
    inline constexpr u32 kReservoirKindMask = 7u;
    // 12 bits per octahedral axis: ~0.05% of angular resolution, which the
    // Jacobian's cosine does not notice. The largest encoded value is
    // 4095*4096 + 4095 = 16'777'215 — exactly 2^24 - 1, the LAST integer an f32
    // still counts by ones, with NO headroom at all. This comment used to say
    // 16'773'120 and call it comfortable; that arithmetic slip is what made a
    // `+ 0.5` round look safe here. See RoundReservoirLaneToInteger.
    inline constexpr f32 kReservoirOctScale = 4095.0f;
    inline constexpr f32 kReservoirOctStride = 4096.0f;
    // A delta light has no emitter normal. -1 is outside the encoding's range,
    // so it cannot collide with a real value the way 0 would — 0 is a legitimate
    // encoded normal.
    inline constexpr f32 kReservoirNoNormal = -1.0f;

    // ROUNDING A LANE THAT IS ALREADY AN EXACT INTEGER. The GLSL twin's
    // OloReservoirRoundToInteger, and its comment is the long version.
    //
    // In one line: `+ 0.5` then truncate is the obvious defensive round and it
    // is WRONG at or above 2^23, where an f32 has no fractional part left. The
    // add lands exactly halfway between two representable integers and rounds to
    // EVEN, flipping the low bit — the sample KIND in the identity lane, and the
    // octahedral y in the normal lane, where 4095 carries into x and the decoded
    // normal jumps to the far side of the octahedron. Both lanes reach 2^24 - 1
    // by design, so this is most of the domain rather than a corner of it.
    inline constexpr f32 kReservoirExactIntegerLimit = 8388608.0f; // 2^23

    [[nodiscard]] inline f32 RoundReservoirLaneToInteger(f32 packed)
    {
        const f32 v = std::max(packed, 0.0f);
        return (v < kReservoirExactIntegerLimit) ? std::floor(v + 0.5f) : v;
    }

    [[nodiscard]] inline f32 PackReservoirIdentity(u32 kind, u32 lightIndex)
    {
        return static_cast<f32>((kind & kReservoirKindMask) | (lightIndex << kReservoirKindBits));
    }

    inline void UnpackReservoirIdentity(f32 packed, u32& kind, u32& lightIndex)
    {
        // Rounded, not truncated: the value went through a render target and
        // back, and a slightly-off lane must land on the same integer rather
        // than one below it. See RoundReservoirLaneToInteger for why that round
        // is not a plain `+ 0.5`.
        const u32 bits = static_cast<u32>(RoundReservoirLaneToInteger(packed));
        kind = bits & kReservoirKindMask;
        lightIndex = bits >> kReservoirKindBits;
    }

    [[nodiscard]] inline f32 PackReservoirNormal(glm::vec3 n)
    {
        const f32 lengthSq = glm::dot(n, n);
        if (!(lengthSq > 0.0f))
            return kReservoirNoNormal;
        n *= 1.0f / std::sqrt(lengthSq);
        glm::vec2 p = glm::vec2(n.x, n.y) * (1.0f / (std::abs(n.x) + std::abs(n.y) + std::abs(n.z)));
        if (n.z < 0.0f)
        {
            p = glm::vec2(1.0f - std::abs(p.y), 1.0f - std::abs(p.x)) *
                glm::vec2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
        }
        const glm::vec2 q(std::floor(std::clamp(p.x * 0.5f + 0.5f, 0.0f, 1.0f) * kReservoirOctScale + 0.5f),
                          std::floor(std::clamp(p.y * 0.5f + 0.5f, 0.0f, 1.0f) * kReservoirOctScale + 0.5f));
        return q.x * kReservoirOctStride + q.y;
    }

    [[nodiscard]] inline glm::vec3 UnpackReservoirNormal(f32 packed)
    {
        if (packed < 0.0f)
            return glm::vec3(0.0f);
        const f32 v = RoundReservoirLaneToInteger(packed);
        const f32 qx = std::floor(v / kReservoirOctStride);
        const f32 qy = v - qx * kReservoirOctStride;
        const glm::vec2 p = (glm::vec2(qx, qy) / kReservoirOctScale) * 2.0f - 1.0f;
        glm::vec3 n(p.x, p.y, 1.0f - std::abs(p.x) - std::abs(p.y));
        const f32 t = std::max(-n.z, 0.0f);
        n.x += (n.x >= 0.0f) ? -t : t;
        n.y += (n.y >= 0.0f) ? -t : t;
        const f32 lengthSq = glm::dot(n, n);
        return (lengthSq > 0.0f) ? n * (1.0f / std::sqrt(lengthSq)) : glm::vec3(0.0f);
    }

} // namespace OloEngine::ReSTIR
