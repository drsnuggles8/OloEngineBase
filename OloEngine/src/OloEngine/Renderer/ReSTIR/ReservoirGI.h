#pragma once

// =============================================================================
// ReservoirGI.h — the ReSTIR GI reservoir: what its sample IS, and the three
// things that makes true which have no DI counterpart. Issue #1169 (#979
// Phase 3, second half).
//
// READ docs/design/restir-gi-reconnection-shift.md FIRST. It derives the
// measure, the shift and the Jacobian, and it was written before this file
// existed because #1169 requires that. The section numbers below cite it.
//
// The domain-neutral arithmetic — streaming RIS, the normalisers, the balance
// heuristic, the M cap, the GEOMETRIC CORE of the reconnection Jacobian, and
// the float-lane encoding — is ReservoirCore.h's, shared verbatim with DI.
//
// -----------------------------------------------------------------------------
// THE MEASURE CONVENTION (design note §2)
// -----------------------------------------------------------------------------
//
// A GI reservoir stores the SAMPLE VERTEX x1 — a POINT, in area measure at x1 —
// together with the vertex normal and the DIRECTION-INDEPENDENT outgoing
// radiance L_o(x1). The target function is evaluated in SOLID ANGLE at the
// shading point that owns the reservoir:
//
//     targetPdf(x1) = || f_r(x0, w, wo) * L_o(x1) * cos(theta0) ||,  w = norm(x1 - x0)
//
// A point rather than a direction for the same reason DI stores an emitter
// point: a direction is only interpretable relative to the shading point that
// drew it, so a neighbour's direction is a DIFFERENT sample, while a
// neighbour's vertex is the SAME sample seen from somewhere else.
//
// -----------------------------------------------------------------------------
// WHAT MAKES L_o(x1) DIRECTION-INDEPENDENT, WHICH IS THE WHOLE CONSTRAINT
// -----------------------------------------------------------------------------
//
// "Constrained one-bounce DIFFUSE" is not a hedge, it is the definition that
// licenses reconnection without re-tracing:
//
//     L_o(x1) = (albedo(x1) * (1 - metallic(x1)) / pi)
//               * ( E_direct(x1) + E_tail(x1) )
//
// The DIFFUSE LOBE ONLY. Evaluating the full closure at x1 toward x0 would make
// the stored radiance depend on where x0 is, and reusing it at a neighbour
// would transplant a view-dependent highlight onto a pixel that is not at that
// view — which reads as plausible indirect specular and is not.
//
// AND NO L_e(x1). The obvious term to add is the vertex's own emission, and it
// must not be there (design note §1.1): light that leaves a surface and arrives
// with NO REFLECTION in between is DIRECT lighting, so it is the direct tier's
// by definition. ReSTIR DI samples the emissive-triangle table from x0 and
// would be estimating exactly the same transport — adding L_e(x1) here
// double-counts every emissive surface in the scene EXACTLY, with the two
// estimates agreeing about the answer and the sum being twice it. An
// emissive-lit room then comes out twice as bright, which reads as "the new GI
// tier is a bit strong" and gets fixed with an intensity slider.
//
// So every path this tier estimates has at least one reflection. That is what
// makes it INDIRECT.
//
// WHAT IS LEFT OUT, THEN, is the SPECULAR LOBE AT THE BOUNCE VERTEX, and it is
// a stated non-goal rather than an approximation nobody declared: this tier estimates
// one-bounce indirect DIFFUSE. It is a DEFINITION, NOT A GATE — there is no
// roughness below which a sample is rejected, because rejecting one would leave
// a hole in the estimate, which is worse than the term it avoided. What there
// is instead is a COUNT: GlossyVertexBounces records the pixels whose bounce
// landed somewhere polished enough that the dropped lobe is a visible fraction
// of what left it, because a chrome-floored scene legitimately produces far
// less GI than a full path tracer would and "the tier looks like it is off"
// must be attributable rather than guessed at.
//
// -----------------------------------------------------------------------------
// THE THREE THINGS GI HAS AND DI DOES NOT (design note §6)
// -----------------------------------------------------------------------------
//
//   1. A VERTEX WITH A BSDF. Handled by the diffuse restriction above, counted
//      rather than gated.
//   2. A RECONNECTION SEGMENT THAT CAN BE OCCLUDED. x0' -> x1 never existed in
//      the source path, so a wall between them is invisible to the source
//      reservoir. DI's spatial reuse traces no rays at all and is right not to;
//      GI's resolve MUST trace one, or reuse leaks light through walls as a
//      smooth gradient. That ray lives in the shader, not here.
//   3. A NEAR FIELD. x1 can be centimetres from x0', and dDestSq is in the
//      Jacobian's DENOMINATOR, so J explodes and temporal reuse keeps the
//      firefly alive for as long as the M cap allows. kDefaultMinimumReconnection-
//      Distance is the guard, and it has no DI analogue because a light sample
//      metres away is DI's normal case.
//
// Every one of the three is invisible in a still frame when it is missing.
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
    // -------------------------------------------------------------------------
    // The versioned GPU layout
    // -------------------------------------------------------------------------

    // Bumped whenever the packing in include/ReservoirGI.glsl changes shape. The
    // history registry keys its planes on it, so a bump invalidates last frame's
    // reservoirs instead of reinterpreting them.
    //
    // SEPARATE FROM kReservoirLayoutVersion ON PURPOSE. The two layouts share an
    // ENCODING but not a SHAPE — the DI identity lane carries a light index and
    // the GI one carries a sample age — so a DI packing change must not
    // invalidate GI history and vice versa. One shared number would make every
    // bump invalidate twice as much as it needed to, which reads as "the tier
    // restarts for no reason".
    inline constexpr u32 kGIReservoirLayoutVersion = 1u;

    // What the sample vertex IS. The value is PACKED INTO THE GPU LAYOUT, so
    // append only — never renumber.
    enum class GISampleKind : u32
    {
        None = 0,        ///< An empty reservoir. Not an error; the normal state before any candidate.
        SurfaceHit = 1,  ///< The bounce ray hit geometry. Position is a POINT; Normal is the vertex normal.
        Environment = 2, ///< The bounce ray escaped. Position is the unit DIRECTION; Normal has no meaning.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GISampleKind kind)
    {
        switch (kind)
        {
            case GISampleKind::None:
                return "none";
            case GISampleKind::SurfaceHit:
                return "surface hit";
            case GISampleKind::Environment:
                return "environment";
            case GISampleKind::Count:
                break;
        }
        return "unknown";
    }

    // GI's DEGENERATE ARM of the shift, and it is DERIVED rather than copied
    // from DI's delta-light arm (design note §4.3).
    //
    // Let x1 = x0 + t*w and take t -> infinity. Both distances grow at the same
    // rate so dSourceSq / dDestSq -> 1, and the vertex normal opposes w from
    // both shading points so both cosines -> 1. Hence J = 1 exactly, and the
    // shift carries the DIRECTION unchanged. That is a fact about the limit, not
    // a shortcut — which is why the predicate is spelled out here rather than
    // tested inline, and why the GLSL twin asks the same question.
    [[nodiscard]] constexpr bool IsDistantSample(GISampleKind kind)
    {
        return kind == GISampleKind::Environment;
    }

    // -------------------------------------------------------------------------
    // Tolerances with no DI counterpart
    // -------------------------------------------------------------------------

    // The minimum reconnection distance, metres (design note §6.3). Below it the
    // reuse is REJECTED, not scaled: dDestSq is in the Jacobian's denominator,
    // so a vertex a centimetre from the destination multiplies the contribution
    // weight by a huge number, and temporal reuse then keeps the resulting
    // firefly alive. Exactly the concave geometry — a corner, a fold, a contact
    // shadow — that GI exists to render is where this fires.
    inline constexpr f32 kDefaultMinimumReconnectionDistance = 0.05f;

    // The roughness at or above which a bounce vertex's DROPPED SPECULAR LOBE is
    // small enough not to be worth reporting. This is a REPORTING threshold and
    // not a correctness gate — the diffuse restriction makes the stored radiance
    // direction-independent whatever the vertex's roughness is (see the header
    // comment). Below it, GlossyVertexBounces counts the pixel, so a chrome
    // scene's missing GI is attributable instead of mysterious.
    inline constexpr f32 kDefaultGlossyVertexRoughness = 0.25f;

    // The hard ceiling on a sample's age in frames. Age is the SECOND staleness
    // bound (design note §10): the M cap bounds how much of a pixel's estimate a
    // stale sample can still claim, and this bounds whether it survives at all.
    // They do different jobs because #976's validity test only looks at the
    // RECEIVING surface — a sample whose own vertex has been re-lit or moved
    // passes it every time.
    inline constexpr u32 kMaxSampleAgeFrames = 4096u;
    inline constexpr u32 kDefaultMaxSampleAge = 64u;

    // -------------------------------------------------------------------------
    // The sample
    // -------------------------------------------------------------------------

    struct GISample
    {
        GISampleKind Kind = GISampleKind::None;

        // Frames since the vertex was traced, incremented by every temporal
        // merge that keeps it. Part of the LINEAGE #1169 asks to stay
        // inspectable, and the quantity the age cap bounds. Zero on a sample
        // traced this frame.
        u32 Age = 0;

        // The sample vertex, RENDER-RELATIVE (issue #429). For an Environment
        // sample this is the unit DIRECTION the ray escaped along, not a point:
        // the two readings are distinguished by Kind and never by inspecting the
        // vector, which is the mistake RayTracedShadowLightRequest records and
        // which DI's LightSample makes the same promise about.
        glm::vec3 Position{ 0.0f };

        // The vertex normal, outward. Zero for an Environment sample, where it
        // has no meaning and is never read.
        glm::vec3 Normal{ 0.0f };

        // L_o(x1): emission plus the DIFFUSE lobe's outgoing radiance, and
        // nothing else. Direction-independent by construction — see the header.
        // Carried rather than re-derived because re-deriving it needs the
        // vertex's material, its UVs and a second NEE draw, none of which a
        // reusing pixel has any reason to have loaded, and because a re-derivation
        // that disagreed with the one at selection time would silently break the
        // estimator's target-pdf / weight bookkeeping.
        glm::vec3 Radiance{ 0.0f };

        [[nodiscard]] auto operator==(const GISample&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // The reservoir
    // -------------------------------------------------------------------------

    // The member NAMES are the interface ReservoirCore.h's templates bind to,
    // and they are deliberately identical to Reservoir's. That is not an
    // accident of style: ReservoirUpdate, FinalizeInitialCandidates,
    // FinalizeCombined and ApplyTemporalMCap are the SAME functions for both
    // domains, and a rename here would silently fork them.
    struct GIReservoir
    {
        GISample Sample{};
        f32 TargetPdf = 0.0f;
        f32 WeightSum = 0.0f;
        f32 M = 0.0f;
        f32 W = 0.0f;

        [[nodiscard]] constexpr bool IsEmpty() const
        {
            return Sample.Kind == GISampleKind::None || !(M > 0.0f);
        }

        void Reset()
        {
            *this = GIReservoir{};
        }

        [[nodiscard]] auto operator==(const GIReservoir&) const -> bool = default;
    };

    [[nodiscard]] inline bool IsFinite(const GIReservoir& r)
    {
        return std::isfinite(r.TargetPdf) && std::isfinite(r.WeightSum) && std::isfinite(r.M) &&
               std::isfinite(r.W) && IsFiniteVec3(r.Sample.Position) && IsFiniteVec3(r.Sample.Normal) &&
               IsFiniteVec3(r.Sample.Radiance);
    }

    // A reservoir that cannot be trusted becomes EMPTY, never clamped. Clamping
    // keeps M, and M is a claim about how many candidates were seen — a false one
    // suppresses every future candidate at that pixel, so a transient corruption
    // becomes permanent.
    [[nodiscard]] inline GIReservoir SanitizeGIReservoir(const GIReservoir& in)
    {
        if (!IsFinite(in))
            return GIReservoir{};
        if (std::to_underlying(in.Sample.Kind) >= std::to_underlying(GISampleKind::Count))
            return GIReservoir{};
        if (in.TargetPdf < 0.0f || in.WeightSum < 0.0f || in.M < 0.0f || in.W < 0.0f)
            return GIReservoir{};
        if (in.Sample.Age > kMaxSampleAgeFrames)
            return GIReservoir{};
        return in;
    }

    // -------------------------------------------------------------------------
    // The shift — GI's wrapper around the geometric core
    // -------------------------------------------------------------------------

    // Whether the reconnection `x0' -> x1` is inside the shift's DOMAIN at all
    // (design note §3). A shift that quietly returns something for an
    // out-of-domain input is how light leaks through a wall with a perfectly
    // smooth falloff.
    //
    // This covers the two conditions that are decidable from geometry alone. The
    // third — that x1 is actually VISIBLE from x0' — costs a ray and is the
    // shader's (design note §6.2); it cannot be asked here and pretending
    // otherwise would be worse than leaving it out.
    //
    // An Environment sample has no vertex, so the only question is whether the
    // stored direction is on the destination's upper hemisphere.
    [[nodiscard]] inline bool ReconnectionInDomain(const GISample& sample, const glm::vec3& destShadingPoint,
                                                   const glm::vec3& destNormal, f32 minimumDistance)
    {
        if (sample.Kind == GISampleKind::None)
            return false;
        if (IsDistantSample(sample.Kind))
        {
            const f32 lengthSq = glm::dot(sample.Position, sample.Position);
            if (!(lengthSq > 0.0f))
                return false;
            return glm::dot(destNormal, sample.Position / std::sqrt(lengthSq)) > 0.0f;
        }

        const glm::vec3 toVertex = sample.Position - destShadingPoint;
        const f32 distanceSq = glm::dot(toVertex, toVertex);
        const f32 minimum = std::max(minimumDistance, 0.0f);
        if (!(distanceSq > minimum * minimum))
            return false;
        return glm::dot(destNormal, toVertex / std::sqrt(distanceSq)) > 0.0f;
    }

    // The reconnection shift's Jacobian, converting a density expressed in solid
    // angle at `sourceShadingPoint` into one in solid angle at
    // `destShadingPoint`, through the fixed sample vertex:
    //
    //     J = ( cos(phiDest) / cos(phiSource) ) * ( dSourceSq / dDestSq )
    //
    // with BOTH cosines taken at the VERTEX. ReconnectionJacobian in
    // ReservoirCore.h is that expression, shared verbatim with DI because the
    // derivation never looks at what the vertex is (design note §4.1, §8).
    //
    // WHAT THIS WRAPPER ADDS: the ENVIRONMENT ARM (§4.3), and nothing else.
    //
    // Returns 0 for a degenerate configuration. Zero means REJECT this reuse; it
    // never means "no change", which 1 would.
    [[nodiscard]] inline f32 GIShiftJacobian(const GISample& sample, const glm::vec3& destShadingPoint,
                                             const glm::vec3& sourceShadingPoint)
    {
        if (IsDistantSample(sample.Kind))
            return 1.0f;
        if (sample.Kind == GISampleKind::None)
            return 0.0f;
        return ReconnectionJacobian(sample.Position, sample.Normal, destShadingPoint, sourceShadingPoint);
    }

    // The area-to-solid-angle conversion at a sample vertex, used to state the
    // measure identity the contract test pins the Jacobian against.
    //
    // An Environment sample was drawn in SOLID ANGLE already (a hemisphere
    // direction that never landed on a surface), so there is nothing to convert
    // and the density passes through — the same shape DI's delta arm has, for a
    // different reason.
    [[nodiscard]] inline f32 GIAreaPdfToSolidAnglePdf(f32 areaPdf, const GISample& sample,
                                                      const glm::vec3& shadingPoint)
    {
        if (IsDistantSample(sample.Kind))
            return areaPdf;
        return SolidAnglePdfFromAreaPdf(areaPdf, sample.Position, sample.Normal, shadingPoint);
    }

    // -------------------------------------------------------------------------
    // Age
    // -------------------------------------------------------------------------

    // Saturating, because the age lane is finite and an age that wrapped would
    // make the OLDEST samples look the freshest — which is the one failure an
    // age cap exists to prevent, arriving through the cap itself.
    [[nodiscard]] inline u32 AdvanceSampleAge(u32 age)
    {
        return (age >= kMaxSampleAgeFrames) ? kMaxSampleAgeFrames : age + 1u;
    }

    // Whether a sample is still young enough to merge. `maxAge` of 0 would mean
    // "drop everything", which is temporal reuse switched off by another name,
    // so the settings sanitizer keeps it at 1 or more and this treats 0 that way
    // too rather than silently disabling the stage.
    [[nodiscard]] inline bool SampleAgeAcceptable(u32 age, u32 maxAge)
    {
        return age <= std::max(maxAge, 1u);
    }

    // -------------------------------------------------------------------------
    // The packed GI plane layout
    // -------------------------------------------------------------------------
    //
    // Three RGBA32F planes, the same shape and the same ENCODING as DI's — the
    // numeric packing, the octahedral normal and the round that must not be
    // `+ 0.5` are all ReservoirCore.h's. What is GI's is what the identity
    // lane's payload MEANS:
    //
    //   plane 0  xyz = Sample.Position        w = Kind | Age << 3
    //   plane 1  xyz = Sample.Radiance        w = oct-packed Sample.Normal
    //   plane 2  x   = W  y = M  z = TargetPdf  w = Diagnostics
    //
    // Age rather than a light index, because a GI sample does not name anything
    // in a table — it IS the thing. kMaxSampleAgeFrames is four thousand and the
    // lane holds two million, so unlike DI's emissive-triangle index there is no
    // aliasing ceiling to defend; the saturation in AdvanceSampleAge is what
    // keeps it inside the range, and this static_assert is what keeps a later
    // raise of the cap from quietly leaving that true.
    static_assert(kMaxSampleAgeFrames <= kMaxEncodablePayloadIndex,
                  "the GI identity lane cannot carry kMaxSampleAgeFrames; raise the lane or lower the cap");

    [[nodiscard]] inline f32 PackGIIdentity(GISampleKind kind, u32 age)
    {
        return PackReservoirIdentity(std::to_underlying(kind), std::min(age, kMaxSampleAgeFrames));
    }

    inline void UnpackGIIdentity(f32 packed, GISampleKind& kind, u32& age)
    {
        u32 rawKind = 0;
        u32 rawAge = 0;
        UnpackReservoirIdentity(packed, rawKind, rawAge);
        kind = (rawKind < std::to_underlying(GISampleKind::Count)) ? static_cast<GISampleKind>(rawKind)
                                                                  : GISampleKind::None;
        age = std::min(rawAge, kMaxSampleAgeFrames);
    }

} // namespace OloEngine::ReSTIR
