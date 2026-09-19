#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

// FoliageLodTransition.h — the transition math a foliage instance climbs the
// mesh -> card -> impostor -> culled ladder by (issue #1237).
//
// GLSL twin: OloEditor/assets/shaders/include/FoliageLodTransition.glsl, which
// carries the SAME functions with the same names in camelCase. The two are
// pinned against each other by
// OloEngine/tests/Rendering/FoliageLodTransitionContractTest.cpp, which
// evaluates the C++ side and asserts the contracts the shader relies on; a
// drift shows up there and in the evidence captures, never as a compile error.
//
// ── What this file is for, stated as the image it fixes ──────────────────────
//
// Before it, every plant in a layer changed representation at the SAME
// authored distance. Walking towards a meadow you therefore see a RING sweep
// across it — every blade within one metre of the threshold swapping shape in
// the same frame — and past the impostor band a hard alpha cut-off deletes the
// far field along a line. That is the "visibly abrupt fade behavior" the issue
// names, and it is a property of every instance sharing one threshold, not of
// the threshold being in the wrong place.
//
// Three things here answer it, and each is a pure function of its arguments so
// the vertex stages, the cull kernel and the CPU tests cannot disagree:
//
//   1. **Per-instance decorrelation.** `TransitionDistance` spreads a layer's
//      single authored threshold over a band, deterministically per plant. The
//      MEAN is still the authored number — a layer's hand-over happens where
//      the author said, it just stops happening all at once.
//   2. **Hysteresis.** The same function moves the edge outward for a receding
//      viewer and inward for an approaching one, so a plant holds the
//      representation it has.
//   3. **Coverage-preserving density reduction.** `DensityKeepFraction` thins
//      the layer with distance; `CoverageCompensation` grows the survivors by
//      exactly the factor that keeps the layer's apparent coverage constant,
//      so a thinned meadow reads as the same meadow rather than as a balding
//      one.
//
// ── The anti-oscillation guarantee, and its limit ────────────────────────────
//
// `Receding` is the sign of the distance derivative (`dist >= prevDist`), not a
// stored latch: the foliage instance stream carries no persistent per-plant
// state, and the previous main-eye position the UBO already carries
// (`u_PrevMeshViewPos`) is the memory available to every stage for free.
//
// One frame of memory cannot beat a camera that oscillates with a period of
// exactly two frames — such a camera flips the representation of any plant
// whose threshold lies inside its travel, whatever the hysteresis. What
// decorrelation buys is that this is no longer the WHOLE LAYER: the plants at
// risk are the ones whose personal threshold falls inside the oscillation, so
// the flipping fraction is bounded by `2 * amplitude / TransitionSpread`. With
// the authored spread an order of magnitude above camera jitter that is a
// handful of plants rather than a ring, and the test asserts exactly that
// bound rather than claiming an absolute guarantee this design does not have.

namespace OloEngine::FoliageLod
{
    // Below this the hash axis is treated as fully consumed; guards the
    // reciprocal square root in CoverageCompensation.
    inline constexpr f32 kMinKeepFraction = 1.0f / 256.0f;

    // @brief Deterministic per-instance draw in [0, 1) from a plant's
    // terrain-local position.
    //
    // Keyed on POSITION rather than on a row index, for the reason
    // FoliageInstanceCull.comp spells out at length: compaction reorders rows
    // every frame, so anything keyed on a slot hands one plant another's
    // history the first time the frustum moves. The position is the plant's
    // identity everywhere else in the foliage stack (the wind phase, the
    // previous-frame deformation), and it is what makes this hash stable
    // across a cull, a leave-and-return and a backend switch.
    //
    // An integer avalanche over the quantised position rather than the
    // bit-reinterpreting HashPosition beside it in FoliagePlacement: that one
    // finishes with a single multiply-and-shift, and #1254 measured it
    // collapsing an 80x80 grid onto 32 distinct values. A thinning mask built
    // on 32 values deletes plants in diagonal stripes.
    [[nodiscard]] inline f32 InstanceHash(glm::vec3 terrainLocalPos)
    {
        // Quantise to a millimetre so the value is stable against the last
        // bits of a position that has been through a terrain transform.
        const auto qx = static_cast<u32>(static_cast<i32>(std::floor(terrainLocalPos.x * 1000.0f)));
        const auto qy = static_cast<u32>(static_cast<i32>(std::floor(terrainLocalPos.y * 1000.0f)));
        const auto qz = static_cast<u32>(static_cast<i32>(std::floor(terrainLocalPos.z * 1000.0f)));

        u32 h = qx * 0x8DA6B343u ^ qy * 0xD8163841u ^ qz * 0xCB1AB31Fu;
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        // 24 bits into [0, 1): exactly representable in f32, and the low bits
        // of an avalanche hash are the ones worth keeping.
        return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
    }

    // @brief This instance's own distance for a transition whose authored
    // threshold is `nominal`.
    //
    // @param nominal    the authored threshold, in world units
    // @param offset01   this instance's InstanceHash
    // @param spread     world units the per-instance offsets spread over; 0
    //                   restores the pre-#1237 behaviour exactly (every plant
    //                   on the authored number)
    // @param hysteresisOffset  the world-unit shift from HysteresisOffset; one
    //                   value for every edge of a band, so the band SLIDES
    //                   rather than changing width
    //
    // The spread is CENTRED: offsets run over [-spread/2, +spread/2], so the
    // mean transition distance is `nominal` and a layer authored for a 30 m
    // hand-over still hands over at 30 m on average. An off-centre spread
    // would quietly move every authored ladder the moment the feature was
    // switched on, which is the one thing a transition-smoothing change must
    // not do.
    [[nodiscard]] inline f32 TransitionDistance(f32 nominal, f32 offset01, f32 spread, f32 hysteresisOffset)
    {
        const f32 base = nominal + spread * (offset01 - 0.5f) + hysteresisOffset;
        // Clamped at zero: a spread wider than twice the threshold, or a
        // hysteresis offset larger than it, would otherwise hand a negative
        // distance to a smoothstep.
        return glm::max(base, 0.0f);
    }

    // @brief The hysteresis shift, in WORLD UNITS, for a band whose near edge
    // is `bandStart`: outward while the viewer retreats, inward while it
    // approaches.
    //
    // An ABSOLUTE offset, applied identically to both edges, rather than a
    // factor applied to each. A factor scales the band's WIDTH by (1 +- h) as
    // well as moving it, so a plant's hand-over would take ~17% longer walking
    // away than walking in at a hysteresis of 0.08 — a different feature from
    // the one this is, and one that contradicts the "slides bodily" guarantee
    // the band's callers rely on. Keyed on the band's near edge so the shift is
    // proportional to the distance the ladder actually works at.
    [[nodiscard]] inline f32 HysteresisOffset(f32 bandStart, bool receding, f32 hysteresis)
    {
        const f32 magnitude = glm::max(bandStart, 0.0f) * hysteresis;
        return receding ? magnitude : -magnitude;
    }

    // @brief The fraction of a layer's instances still drawn at `dist`.
    //
    // 1 up to `start`, falling smoothly to `minFraction` at `end` and holding
    // there. Smooth rather than linear because the DERIVATIVE is what the eye
    // reads at a boundary: a piecewise-linear keep fraction has a visible
    // crease at both ends, which is the same defect as the hard band this
    // replaces, moved.
    //
    // `minFraction` is a floor rather than zero: the far field is what gives a
    // hill its silhouette, and thinning it to nothing is a worse image than
    // any amount of popping. Culling stays the job of ViewDistance.
    [[nodiscard]] inline f32 DensityKeepFraction(f32 dist, f32 start, f32 end, f32 minFraction)
    {
        const f32 floorFraction = glm::clamp(minFraction, kMinKeepFraction, 1.0f);
        if (!(end > start))
            return (dist >= start) ? floorFraction : 1.0f;
        const f32 t = glm::clamp((dist - start) / (end - start), 0.0f, 1.0f);
        const f32 s = t * t * (3.0f - 2.0f * t); // smoothstep
        return glm::mix(1.0f, floorFraction, s);
    }

    // @brief The share of the hash axis a thinned instance fades out over.
    //
    // A keep fraction alone is a hard per-instance switch: the plant whose
    // hash the falling threshold crosses vanishes in one frame, which is the
    // pop this whole file exists to remove — just per plant instead of per
    // ring. `fadeFraction` is the width, in hash units, of the ramp.
    //
    // THE RAMP SITS ABOVE THE THRESHOLD, not below it: a plant is fully opaque
    // while its hash is under `keep` and fades out over `[keep, keep + width)`.
    // Below was the first attempt and it is wrong at the top of the range —
    // with the ramp under the threshold, a keep fraction of 1 (which is what
    // every distance inside DensityLodStartDistance produces) still leaves the
    // highest-hashed `width` of the layer partially transparent. The layer
    // would be thinned before the density band even began, and the contract
    // test's "inside the start distance nothing is thinned" caught it.
    [[nodiscard]] inline f32 DensityFadeWidth(f32 fadeFraction)
    {
        return glm::clamp(fadeFraction, 0.0f, 1.0f);
    }

    // @brief This instance's density fade: 1 while its hash is under the keep
    // threshold, ramping to 0 across the fade band above it, 0 beyond.
    [[nodiscard]] inline f32 DensityInstanceAlpha(f32 hash01, f32 keepFraction, f32 fadeFraction)
    {
        if (hash01 < keepFraction)
            return 1.0f;
        const f32 width = DensityFadeWidth(fadeFraction);
        if (width <= 0.0f)
            return 0.0f;
        return glm::clamp(1.0f - (hash01 - keepFraction) / width, 0.0f, 1.0f);
    }

    // @brief The keep fraction the layer EFFECTIVELY draws, counting the
    // partially-faded instances at their fade value.
    //
    // With hashes uniform on [0, 1) the expected covered fraction is the
    // integral of the alpha above: `keep` fully opaque, plus the part of the
    // ramp that fits under 1. Writing `u = min(1 - keep, width)`, that is
    // `keep + u - u^2 / (2 * width)`.
    //
    // It is NOT simply `keep`, and that is what the compensation below has to
    // invert. Compensating by `keep` alone over-grows the survivors by the
    // whole fade band, which reads as a distant layer that is slightly too
    // dense — the kind of half-right that survives a screenshot.
    [[nodiscard]] inline f32 EffectiveKeepFraction(f32 keepFraction, f32 fadeFraction)
    {
        const f32 k = glm::clamp(keepFraction, 0.0f, 1.0f);
        const f32 width = DensityFadeWidth(fadeFraction);
        if (width <= 0.0f)
            return glm::max(k, kMinKeepFraction);
        const f32 u = glm::min(1.0f - k, width);
        return glm::max(k + u - (u * u) / (2.0f * width), kMinKeepFraction);
    }

    // @brief The linear scale a surviving instance is grown by so the layer's
    // apparent coverage is the coverage it had undthinned.
    //
    // Coverage is a SUM OF AREAS, so halving the population needs each
    // survivor to cover twice the area, which is sqrt(2) in linear size. That
    // is the whole relation: `effectiveKeep * compensation^2 == 1`, and the
    // contract test asserts it over a sweep rather than at one point.
    //
    // `maxScale` caps it, and the cap is not a detail: at the density floor
    // the uncapped factor is 1/sqrt(minFraction), which for a floor of 1/64 is
    // 8x — a blade of grass drawn eight times its authored size reads as a
    // different plant, and past a point growing the survivors stops looking
    // like the same field at all. Beyond the cap the layer genuinely does thin
    // out, and that is the honest trade rather than a silently clamped one:
    // the editor shows the achieved compensation next to the cap.
    [[nodiscard]] inline f32 CoverageCompensation(f32 keepFraction, f32 fadeFraction, f32 maxScale)
    {
        const f32 effective = EffectiveKeepFraction(keepFraction, fadeFraction);
        const f32 uncapped = 1.0f / std::sqrt(effective);
        return glm::clamp(uncapped, 1.0f, glm::max(maxScale, 1.0f));
    }

    // @brief This plant's authored-mesh share at `dist`: 1 up close, 0 past
    // the band, cross-fading between.
    //
    // C++ twin of foliageMeshCoverage in FoliageInstanceGeometry.glsl. It lives
    // here rather than there because the DECORRELATED form below needs it and
    // because a CPU test is the only place the hand-over can be evaluated over
    // a whole layer at once — which is what "how many plants change shape in
    // one frame" is a count of.
    [[nodiscard]] inline f32 MeshCoverage(f32 dist, f32 bandStart, f32 bandEnd)
    {
        if (bandEnd <= 0.0f)
            return 0.0f;
        // A zero-width or inverted band would make the smoothstep undefined;
        // widen it to a millimetre so the hand-over degenerates to a hard
        // switch instead. Same guard, same constant, as the GLSL twin.
        const f32 end = glm::max(bandEnd, bandStart + 1e-3f);
        const f32 t = glm::clamp((dist - bandStart) / (end - bandStart), 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

    // @brief The same hand-over, decorrelated per instance and hysteretic.
    // Twin of foliageMeshCoverageLod. The band keeps its authored WIDTH and
    // slides bodily by this plant's own offset.
    [[nodiscard]] inline f32 MeshCoverageLod(f32 dist, f32 prevDist, f32 bandStart, f32 bandEnd, f32 offset01,
                                             f32 spread, f32 hysteresis)
    {
        if (bandEnd <= 0.0f)
            return 0.0f;
        // ONE offset for both edges — that is what makes the band slide rather
        // than stretch, and it is why HysteresisOffset is computed here from
        // the near edge instead of inside TransitionDistance from each edge.
        const f32 shift = HysteresisOffset(bandStart, dist >= prevDist, hysteresis);
        return MeshCoverage(dist, TransitionDistance(bandStart, offset01, spread, shift),
                            TransitionDistance(bandEnd, offset01, spread, shift));
    }

    // @brief Every authored density-LOD number, sanitised once so the shader
    // side and the cull side read the same thing.
    //
    // A layer arrives here from YAML, from a save game or from the editor, and
    // each of those is an untrusted float per CLAUDE.md. The cheapest place to
    // be sure a NaN never reaches a smoothstep — where it silently empties a
    // layer — is one struct every consumer builds through.
    struct Params
    {
        bool Enabled = false;
        // Resolve a partial fade stochastically in the passes that have no
        // alpha to blend — the G-Buffer and the shadow depth pass. Separate
        // from Enabled: see the GLSL twin's note on the packed bitfield.
        bool StochasticCoverage = false;
        f32 Start = 30.0f;
        f32 End = 80.0f;
        f32 MinFraction = 0.25f;
        f32 FadeFraction = 0.15f;
        f32 MaxScale = 2.0f;
        f32 TransitionSpread = 0.0f;
        f32 Hysteresis = 0.0f;

        // Identity: with this the math below is a no-op at every distance, and
        // that is what a layer authored before #1237 deserializes to.
        [[nodiscard]] static Params Identity()
        {
            return Params{};
        }
    };

    // @brief Sanitise authored numbers into something the shaders can trust.
    // Never rejects a layer — a bad value takes the neutral answer, which for
    // every field here is "this feature is off".
    // @brief The two switches packed as the float bitfield FoliageUBO's
    // LodTransition0.x carries. ONE definition, so the three UBO-fill sites
    // cannot pack it three ways.
    [[nodiscard]] inline f32 PackFlags(const Params& p)
    {
        return (p.Enabled ? 1.0f : 0.0f) + (p.StochasticCoverage ? 2.0f : 0.0f);
    }

    [[nodiscard]] inline Params Sanitise(const Params& in)
    {
        Params out;
        out.Enabled = in.Enabled;
        out.StochasticCoverage = in.StochasticCoverage;
        out.Start = std::isfinite(in.Start) ? glm::max(in.Start, 0.0f) : 30.0f;
        out.End = std::isfinite(in.End) ? glm::max(in.End, out.Start) : glm::max(80.0f, out.Start);
        out.MinFraction = std::isfinite(in.MinFraction) ? glm::clamp(in.MinFraction, kMinKeepFraction, 1.0f)
                                                        : 0.25f;
        out.FadeFraction = std::isfinite(in.FadeFraction) ? glm::clamp(in.FadeFraction, 0.0f, 1.0f) : 0.15f;
        out.MaxScale = std::isfinite(in.MaxScale) ? glm::clamp(in.MaxScale, 1.0f, 8.0f) : 2.0f;
        out.TransitionSpread = std::isfinite(in.TransitionSpread) ? glm::clamp(in.TransitionSpread, 0.0f, 500.0f)
                                                                  : 0.0f;
        out.Hysteresis = std::isfinite(in.Hysteresis) ? glm::clamp(in.Hysteresis, 0.0f, 0.5f) : 0.0f;
        return out;
    }

    // @brief The per-instance density result at one distance: what to multiply
    // the plant's alpha by, and what to multiply its size by.
    struct InstanceDensity
    {
        f32 Alpha = 1.0f;
        f32 ScaleCompensation = 1.0f;

        [[nodiscard]] bool IsCulled() const
        {
            return Alpha <= 0.0f;
        }
    };

    // @brief Evaluate the density LOD for one plant. THE function both the
    // vertex stages and FoliageInstanceCull.comp call — the cull drops a row
    // exactly when this says Alpha is 0, so a plant the cull removed is one
    // the draw would have drawn fully transparent and never one that was still
    // visible.
    [[nodiscard]] inline InstanceDensity EvaluateDensity(const Params& params, f32 hash01, f32 dist)
    {
        InstanceDensity out;
        if (!params.Enabled)
            return out;

        const f32 keep = DensityKeepFraction(dist, params.Start, params.End, params.MinFraction);
        out.Alpha = DensityInstanceAlpha(hash01, keep, params.FadeFraction);
        out.ScaleCompensation = CoverageCompensation(keep, params.FadeFraction, params.MaxScale);
        return out;
    }
} // namespace OloEngine::FoliageLod
