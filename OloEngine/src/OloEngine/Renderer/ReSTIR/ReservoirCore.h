#pragma once

// =============================================================================
// ReservoirCore.h — the part of a ReSTIR reservoir that does not know what the
// sample IS. Extracted from ReservoirDI.h for issue #1169 (ReSTIR GI).
//
// THE TEST FOR WHAT BELONGS HERE, and it is the only one: a piece belongs in
// this file when it contains no reference to what the sample is. Streaming RIS
// does not care whether the survivor is a point on an emitter or a one-bounce
// hit vertex. Neither does the normaliser, the balance heuristic, the M cap, or
// the arithmetic that gets an integer through a float lane.
//
// WHAT DELIBERATELY DOES NOT BELONG HERE. The target function, candidate
// generation, visibility, and the DEGENERATE ARM of the shift — a delta light
// for DI, an environment sample for GI. Those are the parts the two domains
// disagree about, and folding them together would mean a `kind` enum that spans
// two GPU layouts. See docs/design/restir-gi-reconnection-shift.md §8.
//
// -----------------------------------------------------------------------------
// THE GEOMETRIC CORE OF THE SHIFT IS SHARED, AND THAT IS DERIVED RATHER THAN
// NOTICED
// -----------------------------------------------------------------------------
//
// DI reuses a point on an emitter; GI reuses a one-bounce hit vertex. Both
// shifts hold ONE VERTEX FIXED and re-express a solid-angle density at a MOVED
// SHADING POINT, and neither derivation looks at what the vertex is:
//
//     p_w(vertex, x) = p_A(vertex) * || vertex - x ||^2 / |n_vertex . dir|
//
// so the change of measure between two shading points through the same vertex is
//
//     J = ( cosPhiDest / cosPhiSource ) * ( dSourceSq / dDestSq )
//
// with BOTH cosines taken at the VERTEX. Taking them at the shading points is
// the plausible-looking mistake: the shading-point cosine belongs to the
// integrand, not to the measure.
//
// ReconnectionJacobian() below is that expression and nothing else. It has no
// notion of a delta light or of an environment sample, because those are
// statements about a DOMAIN's degenerate case and each domain's wrapper makes
// its own. ReservoirDI.h::ShiftJacobian and ReservoirGI.h::GIShiftJacobian are
// those wrappers.
//
// THE JACOBIAN MULTIPLIES W. IT DOES NOT DIVIDE THE TARGET FUNCTION. The
// derivation is in ShiftedContributionWeight below and in the design note's §4.2;
// it is restated at every layer because the identity that pins the TERM does not
// constrain its PLACEMENT, and that is the mistake that actually shipped once.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace OloEngine::ReSTIR
{
    // -------------------------------------------------------------------------
    // Tolerances, named once for every domain
    // -------------------------------------------------------------------------

    // The smallest vertex-side cosine a shift is allowed to divide by. Below it
    // the reuse is REJECTED rather than scaled: a vertex seen edge-on from the
    // source puts a vanishing cosine in the Jacobian's DENOMINATOR, and the
    // resulting firefly is then kept alive for a hundred frames by temporal
    // reuse.
    inline constexpr f32 kMinimumShiftCosine = 1.0e-4f;

    // The default cap on M for temporal reuse. Capping is what bounds how long a
    // stale sample can survive a change in the scene; without it a pixel that has
    // been still for a thousand frames ignores the light that just turned on.
    // 20x the initial candidate count is the literature's rule of thumb and is a
    // TUNABLE, not a constant of nature — each domain's settings struct owns it.
    inline constexpr f32 kDefaultTemporalMCap = 20.0f;

    // -------------------------------------------------------------------------
    // Normalisation — the two bias modes
    // -------------------------------------------------------------------------

    enum class BiasMode : u32
    {
        // 1/M. Cheap, and biased whenever the combined reservoirs do not share a
        // domain. The bias is a DARKENING at exactly the pixels where reuse helps
        // most, so it is not a uniform offset an exposure tweak could hide.
        Biased = 0,
        // The generalised balance heuristic (Talbot MIS). Unbiased, and costs one
        // target-function evaluation per (neighbour, selected sample) pair, which
        // is the real price of the mode rather than a constant factor.
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

    [[nodiscard]] inline bool IsFiniteVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // W = weightSum / (normaliser * targetPdf).
    //
    // `normaliser` is M under 1/M and 1 under a MIS-weighted combine, where the
    // per-candidate MIS weights already sum to one inside weightSum. Passing it
    // in rather than branching on the mode is what lets a test drive both arms
    // through one function and catch a normaliser applied twice.
    [[nodiscard]] inline f32 ComputeContributionWeight(f32 weightSum, f32 normaliser, f32 targetPdf)
    {
        if (!(weightSum > 0.0f) || !(normaliser > 0.0f) || !(targetPdf > 0.0f))
            return 0.0f;
        const f32 w = weightSum / (normaliser * targetPdf);
        return std::isfinite(w) ? w : 0.0f;
    }

    // The per-candidate MIS weight for the generalised balance heuristic:
    //
    //     m_i(y) = M_i * pHat_i(y) / sum_j M_j * pHat_j(y)
    //
    // `targetPdfMatrix[i * count + j]` is pHat_i(y_j) — candidate i's target
    // function evaluated on candidate j's sample. The matrix form is deliberate:
    // it is what a CPU test can build exactly, and it makes the symmetry the
    // balance heuristic depends on visible instead of implicit.
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

    // -------------------------------------------------------------------------
    // Streaming RIS, and the two finalisers
    // -------------------------------------------------------------------------
    //
    // Templated on the reservoir and sample types rather than duplicated per
    // domain: the arithmetic below is identical for a light sample and for a
    // one-bounce vertex, and the ONE line that is easy to get wrong — the
    // selection test — must not exist twice.

    // Add one candidate. `weight` is the RIS weight pHat(x) / p(x) for an initial
    // candidate, or m * pHat(x) * W for a reservoir being merged. `xi` is a fresh
    // uniform in [0,1).
    //
    // The test is written `xi * WeightSum < weight` rather than
    // `xi < weight / WeightSum` for one reason that matters: the first form is
    // FALSE when WeightSum is zero, which is exactly the "nothing has been added
    // yet and this candidate is worthless" case, while the second divides by zero
    // and selects on a NaN comparison whose result is implementation-defined.
    // GLSL has the same trap, and ReservoirCore.glsl spells it the same way.
    //
    // Returns whether the candidate replaced the incumbent — the caller needs it
    // to carry along any per-sample bookkeeping the reservoir does not hold.
    template <typename ReservoirT, typename SampleT>
    inline bool ReservoirUpdate(ReservoirT& r, const SampleT& sample, f32 weight, f32 targetPdf, f32 xi)
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

    // Finish a reservoir built by streaming RIS over independent candidates drawn
    // from a source pdf at ONE pixel. Always 1/M: those candidates DO share a
    // domain, so 1/M is the correct unbiased normaliser and the bias mode does
    // not apply. That distinction is why this is its own function.
    template <typename ReservoirT>
    inline void FinalizeInitialCandidates(ReservoirT& r)
    {
        r.W = ComputeContributionWeight(r.WeightSum, r.M, r.TargetPdf);
    }

    // Finish a reservoir built by merging OTHER reservoirs.
    //
    //   Biased      — normaliser is the summed M. Correct only when every merged
    //                 reservoir could have produced the surviving sample, which
    //                 spatial neighbours generally could not.
    //   UnbiasedMIS — normaliser is 1, because the caller already folded each
    //                 candidate's MIS weight into its update weight. Passing a
    //                 summed M here as well is the "normalised twice" bug the
    //                 shared ComputeContributionWeight exists to make visible.
    template <typename ReservoirT>
    inline void FinalizeCombined(ReservoirT& r, BiasMode mode, f32 summedM)
    {
        const f32 normaliser = (mode == BiasMode::UnbiasedMIS) ? 1.0f : summedM;
        r.M = summedM;
        r.W = ComputeContributionWeight(r.WeightSum, normaliser, r.TargetPdf);
    }

    // Cap M after a temporal merge. Rescaling WeightSum with it is what keeps W
    // unchanged across the cap — capping M alone would silently multiply the
    // pixel's radiance by (cap / M), which brightens exactly the pixels that have
    // been stable longest.
    template <typename ReservoirT>
    inline void ApplyTemporalMCap(ReservoirT& r, f32 cap)
    {
        if (!(cap > 0.0f) || !(r.M > cap))
            return;
        const f32 scale = cap / r.M;
        r.WeightSum *= scale;
        r.M = cap;
    }

    // -------------------------------------------------------------------------
    // The shift's geometric core
    // -------------------------------------------------------------------------

    // The reconnection shift's Jacobian for a FIXED VERTEX seen from two shading
    // points, converting a density expressed in solid angle at `sourceShadingPoint`
    // into one in solid angle at `destShadingPoint`:
    //
    //     J = ( cos(phiDest) / cos(phiSource) ) * ( dSourceSq / dDestSq )
    //
    // where phi is the angle AT THE VERTEX between its normal and the vector to
    // the shading point, and d is the distance from the shading point to the
    // vertex.
    //
    // Returns 0 for a degenerate configuration (the source saw the vertex
    // edge-on, or a shading point coincides with it). Zero means REJECT this
    // reuse; it never means "no change", which 1 would.
    //
    // IT HAS NO DEGENERATE-CASE ARM. A delta light (DI) and an environment sample
    // (GI) both shift with J = 1, but for different reasons in different measures,
    // and each domain's wrapper decides that before calling here.
    [[nodiscard]] inline f32 ReconnectionJacobian(const glm::vec3& vertexPosition,
                                                  const glm::vec3& vertexNormal,
                                                  const glm::vec3& destShadingPoint,
                                                  const glm::vec3& sourceShadingPoint)
    {
        const glm::vec3 toDest = destShadingPoint - vertexPosition;
        const glm::vec3 toSource = sourceShadingPoint - vertexPosition;
        const f32 destDistanceSq = glm::dot(toDest, toDest);
        const f32 sourceDistanceSq = glm::dot(toSource, toSource);
        if (!(destDistanceSq > 0.0f) || !(sourceDistanceSq > 0.0f))
            return 0.0f;

        const f32 normalLengthSq = glm::dot(vertexNormal, vertexNormal);
        if (!(normalLengthSq > 0.0f))
            return 0.0f;
        const glm::vec3 n = vertexNormal / std::sqrt(normalLengthSq);

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
    //   expressed in. A vertex has the same AREA density at every shading point,
    //   so two shading points' SOLID-ANGLE densities differ only by geometry:
    //
    //       p_source(y) = pArea(y) * dSourceSq / cosSource
    //       p_dest(y)   = pArea(y) * dDestSq   / cosDest
    //
    //   hence
    //
    //       1/p_dest = (1/p_source) * (cosDest/cosSource) * (dSourceSq/dDestSq)
    //                = W_source * ReconnectionJacobian(...)
    //
    // So a merged candidate's RIS weight is  m * pHat_dest(y) * ShiftedW, with
    // pHat_dest LEFT ALONE. Dividing pHat by J instead is off by J^2 in the
    // weight and by J in the final estimate — a smooth geometric brightness
    // error, which is exactly the shape nobody reports.
    //
    // AND m CARRIES THE CONFIDENCE WEIGHT. Under 1/M normalisation m_i is M_i,
    // not 1: the reservoir stands for M_i candidates and the summed M in the
    // denominator is what divides them back out. Using 1 there darkens the image
    // by the average M. Under the MIS-weighted mode m_i is the balance heuristic,
    // whose numerator ALREADY contains M_i, so multiplying by M_i again
    // double-counts it. That asymmetry is why the two modes cannot share one
    // literal.
    [[nodiscard]] inline f32 ShiftedContributionWeight(f32 sourceW, f32 jacobian)
    {
        if (!(sourceW > 0.0f) || !(jacobian > 0.0f))
            return 0.0f;
        const f32 w = sourceW * jacobian;
        return std::isfinite(w) ? w : 0.0f;
    }

    // The area-to-solid-angle conversion, used to state the measure identity the
    // contract tests pin: a density in area measure at a vertex is
    // areaPdf * dSq / cos in solid angle at the shading point. Written once, here,
    // so no shader and no test can grow its own copy.
    //
    // No delta / environment arm, for the same reason ReconnectionJacobian has
    // none.
    [[nodiscard]] inline f32 SolidAnglePdfFromAreaPdf(f32 areaPdf, const glm::vec3& vertexPosition,
                                                      const glm::vec3& vertexNormal,
                                                      const glm::vec3& shadingPoint)
    {
        if (!(areaPdf > 0.0f))
            return 0.0f;
        const glm::vec3 toShading = shadingPoint - vertexPosition;
        const f32 distanceSq = glm::dot(toShading, toShading);
        const f32 normalLengthSq = glm::dot(vertexNormal, vertexNormal);
        if (!(distanceSq > 0.0f) || !(normalLengthSq > 0.0f))
            return 0.0f;
        const f32 cosVertex =
            std::abs(glm::dot(vertexNormal / std::sqrt(normalLengthSq), toShading / std::sqrt(distanceSq)));
        if (!(cosVertex > kMinimumShiftCosine))
            return 0.0f;
        const f32 pdf = areaPdf * distanceSq / cosVertex;
        return std::isfinite(pdf) ? pdf : 0.0f;
    }

    // -------------------------------------------------------------------------
    // THE GPU LAYOUT'S ENCODING, AND WHY IT IS ARITHMETIC RATHER THAN BITS
    // -------------------------------------------------------------------------
    //
    // Reservoirs live in screen-space RGBA32F RENDER TARGETS, not storage
    // buffers: there is no free SSBO binding left. So every integer field has to
    // survive a trip through a float lane.
    //
    // THE OBVIOUS ENCODING IS BROKEN. Reinterpreting `kind | index << 3` as a
    // float makes a DENORMAL — 401 becomes 5.6e-43 — and a GPU is permitted to
    // flush denormals to zero. This one does. The measured result in #1140 was
    // every reservoir reading back as kind None, the resolve writing black, and
    // every counter still reporting the tier active, with nothing in the log.
    //
    // So each integer is stored as its own NUMERIC value. An f32 is exact on
    // every integer to 2^24, far above what these fields need, and no value in
    // the encoding is ever denormal.
    inline constexpr u32 kReservoirKindBits = 3u;
    inline constexpr u32 kReservoirKindMask = 7u;
    // 12 bits per octahedral axis: ~0.05% of angular resolution, which the
    // Jacobian's cosine does not notice. The largest encoded value is
    // 4095*4096 + 4095 = 16'777'215 — exactly 2^24 - 1, the LAST integer an f32
    // still counts by ones, with NO headroom at all.
    inline constexpr f32 kReservoirOctScale = 4095.0f;
    inline constexpr f32 kReservoirOctStride = 4096.0f;
    // A sample with no meaningful vertex normal (a delta light for DI, an
    // environment direction for GI) stores this. -1 is outside the encoding's
    // range, so it cannot collide with a real value the way 0 would — 0 is a
    // legitimate encoded normal.
    inline constexpr f32 kReservoirNoNormal = -1.0f;

    // ROUNDING A LANE THAT IS ALREADY AN EXACT INTEGER.
    //
    // In one line: `+ 0.5` then truncate is the obvious defensive round and it is
    // WRONG at or above 2^23, where an f32 has no fractional part left. The add
    // lands exactly halfway between two representable integers and rounds to
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

    // The largest payload index the identity lane can carry alongside a kind.
    // The index is shifted up by kReservoirKindBits and the result must stay
    // inside the range an f32 counts by ones, so the ceiling is 2^24 / 8 - 1.
    //
    // PAST IT THE ENCODING ALIASES SILENTLY: index and index + 2^21 pack to the
    // same lane, and the reservoir comes back naming something else entirely with
    // a perfectly plausible payload. Each domain's pass REFUSES to publish a
    // table it cannot address rather than encoding wrapped indices.
    inline constexpr u32 kMaxEncodablePayloadIndex = (1u << 21) - 1u;

    [[nodiscard]] inline bool IsEncodablePayloadIndex(u32 index)
    {
        return index <= kMaxEncodablePayloadIndex;
    }

    [[nodiscard]] inline f32 PackReservoirIdentity(u32 kind, u32 payloadIndex)
    {
        return static_cast<f32>((kind & kReservoirKindMask) | (payloadIndex << kReservoirKindBits));
    }

    inline void UnpackReservoirIdentity(f32 packed, u32& kind, u32& payloadIndex)
    {
        // Rounded, not truncated: the value went through a render target and
        // back, and a slightly-off lane must land on the same integer rather than
        // one below it. See RoundReservoirLaneToInteger for why that round is not
        // a plain `+ 0.5`.
        const u32 bits = static_cast<u32>(RoundReservoirLaneToInteger(packed));
        kind = bits & kReservoirKindMask;
        payloadIndex = bits >> kReservoirKindBits;
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
