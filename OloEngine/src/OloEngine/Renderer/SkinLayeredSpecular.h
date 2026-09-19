#pragma once

// =============================================================================
// SkinLayeredSpecular.h — the layered surface response #1243 adds on top of
// #1231's split, #1241's diffusion and #1242's transmission.
//
// EVERY PHYSICAL DECISION IN THIS FEATURE IS MADE HERE, ON THE CPU, for the
// reason SkinDiffusion.h and SkinTransmission.h both state and this feature
// needs more than either of them: the numbers here were CHOSEN BY MEASUREMENT,
// and a constant that came out of an experiment is worthless if the experiment
// cannot be pointed at the line that uses it. The shader is handed two lobe
// widths, a mixture weight and a detail strength; it knows nothing about
// variance estimators, footprints or morph weights.
//
// THE MEASUREMENT. experiments/skin-specular-reference/compare_lobes.py builds a
// patch of skin micro-geometry — furrows, pores, a patchy lipid film — far below
// the pixel, integrates the true aggregate response of a pixel footprint over
// it, and scores candidate models against it. Every claim below that says "the
// measurement says" is a row of that script's output, recorded in
// docs/guides/skin-layered-specular.md. Three results drive the whole design:
//
//   1. A FIXED AUTHORED ROUGHNESS FAILS AT DISTANCE, AND FILTERING IS WHAT
//      FIXES IT. Relative RMS against the integrated truth, for one authored
//      roughness tuned at the finest footprint: 10% at a 16 um footprint, still
//      11% at 125 um, then 55% at 250 um and 138% at 500 um. The variance
//      filter takes that 500 um figure to 27%. This is the structural win, it
//      needs no fitting, and it is the reason the filter is not optional.
//
//   2. A ROUGHNESS MIP IS NOT A FILTER — IT IS WORSE THAN NOT FILTERING.
//      Averaging the roughness map over the footprint and stopping there
//      measured 149% at 500 um, ABOVE the fixed value's 138%. It lowers the
//      roughness toward the smooth regions' value while the normal variance
//      that should have raised it is discarded, so it moves the answer the
//      wrong way twice over. Half of this filter is not optional.
//
//   3. THE SECOND LOBE IS A REAL IMPROVEMENT, AND THE COMPARISON IS NOT FREE.
//      The fitted convex mixture scores 0.4% to 8% across the whole footprint
//      range, against the shipping estimator's 13% to 72%. But the mixture is a
//      THREE-PARAMETER FIT (both lobe widths and the weight) and the estimator
//      has none, so that gap is an UPPER BOUND on what layering buys rather
//      than a like-for-like score. What the fit says without ambiguity is WHERE
//      the second lobe earns its place: its weight is near zero at the finest
//      footprints and settles around 0.55-0.60 once a pixel straddles regions
//      of different roughness. That is the physical claim "layered specular"
//      makes, and it is why the default is 0 and the field is authored.
//
//   4. THE SPARKLE NUMBER. Pushing the camera in over a drifting footprint, the
//      mean frame-to-frame change of a point-sampled normal is 2.3x the change
//      in the true aggregate — the aggregate's own change being real motion
//      under the pixel, and therefore the floor. A roughness mip alone is 1.3x,
//      the ideal estimator 1.1x. The filter below is 1.0x: it lands ON the
//      floor. That ratio is the second acceptance criterion as a number.
//
// =============================================================================
// WHY THE VARIANCE IS MEASURED IN SCREEN SPACE AND NOT OFF THE NORMAL MAP
// =============================================================================
//
// The textbook answer is Toksvig (2005): mip the normal map WITHOUT
// renormalizing, and the length of the averaged normal is the variance that was
// lost. It is the better estimator over most of the measured range — 8% against
// 28% at a 31 um footprint, and 21% against 52% at 62 um — and this engine
// cannot use it. (At the COARSEST footprints the screen-space estimator wins
// instead, 27% against 82%, because it also sees the geometric curvature that
// dominates there and Toksvig sees only the map.)
//
// include/PBRCommon.glsl's `decodeTangentNormal` reconstructs
// z = sqrt(1 - x^2 - y^2) from the sampled xy rather than sampling the blue
// channel. It has to: a two-channel BC5 normal map has no blue channel, and
// sampling it yields z = -1 and an inverted normal (#440). Reconstruction
// renormalizes. Whatever the mip chain averaged, what leaves that function is
// unit length at every mip, so |N| is 1.0 always and the Toksvig factor is 1.0
// always — a filter that compiles, runs, costs ALU and does nothing.
//
// THAT IS A TRAP WORTH NAMING because it is invisible: the code looks right, the
// maths is right, and the answer is identically the unfiltered one. Anyone
// reaching for Toksvig here has to change the decode first, which changes every
// normal-mapped surface in the engine, which is not this issue.
//
// So the estimator below is Tokuyoshi & Kaplanyan's (2019): the variance of the
// FINAL shading normal across the pixel, from its screen-space derivatives.
// Two ALU instructions, available on every path that has a normal, and it
// responds to geometric curvature and to LOD as well as to the normal map —
// none of which Toksvig sees at all.
//
// ITS WEAKNESS, MEASURED AND NOT HIDDEN. A screen-space estimator measures the
// variation BETWEEN pixels; the quantity that belongs in the roughness is the
// variation WITHIN one. Those coincide only when the normal map is resolved at
// about a texel per pixel. Magnify past that — a close-up head, which is this
// feature's whole subject — and between-pixel variation is detail the filter
// should have kept; shrink past it and one pixel hides variance no neighbour
// difference can see. Measured, the estimator at the paper's default strength
// misses the true hemispherical energy by up to 19% in BOTH directions across
// the range (0.82x at a 125 um footprint, 1.19x at 500 um).
//
// The fitted strength across the measured range runs 0.05 at every fine
// footprint, then 0.15 and 0.75 at the two coarsest — a factor of fifteen. One
// constant cannot serve that, which is why `NormalVarianceStrength` is an
// AUTHORED FIELD and not a #define, and why its documentation says what it is
// trading rather than offering a good default and hoping. Refitting it moves
// the estimator from 13-72% to 4-32%, which is most of the gap to Toksvig.
//
// =============================================================================
// ENERGY: WHY A SECOND LOBE CANNOT BRIGHTEN A HEAD
// =============================================================================
//
// The first acceptance criterion asks for consistent energy, and a second
// specular lobe is the classic way to lose it: add a broad lobe beside the
// narrow one and every skin surface in the game gets brighter, which reads as
// "the new skin shader looks better" right up until someone checks a furnace
// test. So the mixture here is CONVEX and not additive:
//
//     specular = (1 - w) * GGX(narrow) + w * GGX(broad)
//
// The directional albedo of that is (1 - w) E_narrow + w E_broad, a convex
// combination of two numbers each of which is the directional albedo of a
// single GGX lobe. It is therefore bounded above by max(E_narrow, E_broad) for
// every w in [0, 1] — it cannot exceed what ONE lobe of either width would have
// returned, whatever the author sets. The bound needs no parameter range to
// hold and no test to discover; it is a property of the expression, and
// SkinSpecularMix below is the only place the expression is written.
//
// At w = 0 the result is bit-identical to a single lobe at the narrow width, so
// the LOBE half of the A/B control is exact rather than approximate.
//
// That is a claim about the mixture and nothing else. The narrow lobe still
// shades at the FILTERED roughness, so w = 0 alone does not reproduce the
// transport-version-2 frame — that needs NormalVarianceStrength and both detail
// fields at zero as well, which is what the neutral-identity arm of
// SkinLayeredSpecularEvidenceTest authors. Worth spelling out because "lobe mix
// zero means the old frame" is the intuitive reading and it is wrong.
//
// WHAT THE MEASUREMENT SAYS ABOUT w. The fitted mixture weight runs 0.05-0.10
// at the finest footprints and 0.55-0.60 at the coarsest — that is, the second
// lobe earns its place as a pixel starts straddling regions of DIFFERENT
// roughness (an oily plateau beside a dry one), and does very little when it
// does not. That is the physical claim "layered specular" makes, and it is why
// the default is 0: an author turns it up for the surfaces and distances where
// it is true.
//
// =============================================================================
// THE DETAIL BLEND AND ITS HISTORY
// =============================================================================
//
// The third acceptance criterion has two halves, and the second is the one that
// is easy to fail silently: the expression-driven detail must "blend
// deterministically with existing morph/animation state AND EMIT VALID HISTORY
// CHANGES". A shading term that moves without the renderer knowing it moved is
// a term a temporal upscaler will smear, and the smear looks like a bad
// upscaler rather than like a bad material.
//
// The engine already owns that decision. #1227 gave MorphTargetComponent an
// `AppliedWeights` vector — the weights the surface CURRENTLY ON THE GPU was
// built from — and Scene::OnUpdateRuntime rejects deformation history whenever
// it differs from `PrevAppliedWeights`. So this feature does not invent a
// second history mechanism. It derives its detail weight FROM `AppliedWeights`,
// and inherits the existing one exactly:
//
//     the detail weight changed
//       => AppliedWeights changed          (it is a pure function of them)
//       => history was already rejected    (Scene.cpp's `finish` lambda)
//
// DERIVING IT FROM `Weights` INSTEAD WOULD HAVE BROKEN THAT, and it is the
// obvious thing to reach for: `Weights` is the authored map, it is what a
// script sets, and it is one member away. But it is the weights the surface
// WILL have, not the ones it HAS — the morph pass has not run yet — so the
// detail would lead the geometry by a frame, and in the frame where a script
// sets a weight and the surface has not yet moved, the shading would change
// with no history rejection behind it. That is precisely the invalid history
// change the criterion forbids, and nothing about it is visible in a still.
// SkinExpressionDetailWeight therefore takes a span of APPLIED weights and the
// call site is the only place that could get this wrong.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <glm/glm.hpp>

#include <span>

namespace OloEngine
{

    // -------------------------------------------------------------------------
    // The variance filter
    // -------------------------------------------------------------------------

    // The upper bound on how much variance one pixel may add to the lobe,
    // expressed in the same alpha-squared units the variance is added in.
    //
    // NOT A TASTE BOUND. Without it, a silhouette pixel — where the shading
    // normal swings most of a hemisphere between one pixel and the next — adds
    // an unbounded amount and the surface goes to fully rough along every
    // outline, which is a bright halo rather than a soft one. 0.18 is Tokuyoshi
    // & Kaplanyan's published clamp and corresponds to roughly the widest
    // widening that still reads as the same material.
    inline constexpr f32 kSkinVarianceKernelClamp = 0.18f;

    // GGX alpha widened by the screen-space variance of the shading normal.
    //
    //   alpha             the surface's own alpha (roughness^2), > 0
    //   dNdxLengthSq      |dN/dx|^2, from the screen-space derivative of the
    //   dNdyLengthSq      |dN/dy|^2   FINAL shading normal — after the normal
    //                                 map, not the vertex normal
    //   varianceStrength  the profile's NormalVarianceStrength, sigma^2
    //
    // Returns an alpha in (0, 1], never smaller than `alpha`: the filter only
    // ever ROUGHENS. That direction is not an accident of the formula and it is
    // worth stating, because the other direction is a sharpening filter and a
    // sharpening filter cannot remove aliasing, it manufactures it.
    //
    //     kernel  = min(2 * sigma^2 * (|dN/dx|^2 + |dN/dy|^2), clamp)
    //     alpha'  = sqrt(alpha^2 + kernel)
    //
    // THE VARIANCES ADD, and that is the whole reason this composes with an
    // authored roughness map rather than fighting it. GGX's alpha^2 IS a slope
    // variance; the pixel's normal spread is another one; two independent
    // variances of the same quantity add. So a rough authored material and a
    // high-variance pixel do not have to be reconciled by a mix or a max — the
    // sum is the physical answer, and it degrades to each of them when the other
    // is zero.
    //
    // THIS FUNCTION IS THE SPECIFICATION AND ALSO A RUNTIME PATH: the CPU uses
    // it to pin the maths, and include/SkinLayeredSpecular.glsl's
    // oloSkinFilteredAlpha is a transcription of this expression in this order.
    // SkinLayeredSpecularParityTest drives the shader against it.
    //
    // Non-finite inputs are the caller's problem for the same reason they are in
    // SkinTransmission.h's EvaluateSkinTransmissionLanes: a guarded CPU path
    // compared against an unguarded shader is not a parity test. The authored
    // side is guarded by Sanitize; the per-pixel side is a derivative of a
    // normalized vector and cannot be non-finite without the normal already
    // being so.
    [[nodiscard]] f32 SkinFilteredAlpha(f32 alpha, f32 dNdxLengthSq, f32 dNdyLengthSq,
                                        f32 varianceStrength) noexcept;

    // -------------------------------------------------------------------------
    // The two lobes
    // -------------------------------------------------------------------------

    // The lobe pair a profile asks for, at one pixel's already-filtered
    // PERCEPTUAL ROUGHNESS.
    //
    // ROUGHNESS AND NOT ALPHA, which is the one unit decision in this struct and
    // is made this way because roughness is what a material authors and what
    // every closure in include/PBRCommon.glsl takes. Alpha appears in exactly
    // one place in this feature — SkinFilteredAlpha, where the variance addition
    // genuinely happens in alpha-squared space — and is converted back
    // immediately. Keeping the lobes in roughness means the shader needs no
    // square root between the author's slider and its effect.
    struct SkinSpecularLobePair
    {
        // Roughness of the sharp lobe — the lipid film. This is the filtered
        // roughness unchanged: the narrow lobe IS the surface's own response,
        // and the mixture adds a broad companion rather than narrowing the
        // original. Narrowing it would have made w = 0 stop being the identity,
        // and with it the A/B control the acceptance criteria ask for.
        f32 NarrowRoughness = 0.0f;

        // Roughness of the broad lobe — the dry, scattering stratum corneum.
        // `NarrowRoughness * LobeRoughnessScale`, clamped to 1.
        f32 BroadRoughness = 0.0f;

        // The fraction of the specular that comes from the broad lobe, [0, 1].
        // The mixture is convex in this, so it is a redistribution and never an
        // addition.
        f32 BroadWeight = 0.0f;
    };

    [[nodiscard]] SkinSpecularLobePair SkinSpecularLobesFor(f32 filteredRoughness,
                                                            const SkinSpecularParameters& parameters) noexcept;

    // The convex mixture, and the ONE place it is written.
    //
    // Exposed as a named function rather than inlined at its two call sites
    // because the energy argument above is an argument about THIS EXPRESSION,
    // and an argument about an expression written in two places is an argument
    // about one of them. SkinLayeredSpecularTest sweeps it.
    [[nodiscard]] f32 SkinSpecularMix(f32 narrow, f32 broad, f32 broadWeight) noexcept;
    [[nodiscard]] glm::vec3 SkinSpecularMix(const glm::vec3& narrow, const glm::vec3& broad,
                                            f32 broadWeight) noexcept;

    // -------------------------------------------------------------------------
    // Expression-driven detail
    // -------------------------------------------------------------------------

    // How far from neutral this face is, [0, 1], from the morph weights the
    // surface CURRENTLY ON THE GPU was built from.
    //
    // `appliedWeights` is MorphTargetComponent::AppliedWeights and must not be
    // its `Weights` map — see THE DETAIL BLEND AND ITS HISTORY at the top of
    // this file for why that substitution is a history bug rather than a style
    // preference.
    //
    // THE SUM OF MAGNITUDES, CLAMPED. Deliberately the simplest total that has
    // the three properties this has to have:
    //
    //   DETERMINISTIC AND ORDER-FREE — the same set of weights gives the same
    //     number whatever order the targets were evaluated in, so two frames
    //     that reached the same expression by different routes shade the same.
    //     A max() would have this too; a running accumulator would not.
    //   EXACTLY ZERO AT NEUTRAL — an empty or all-zero weight vector returns
    //     0.0, so a head that is not emoting shades bit-identically to one with
    //     no expression detail authored at all. That is what makes the feature's
    //     cost and its risk both opt-in.
    //   CONTINUOUS — no thresholds, so the detail cannot pop. A discontinuity
    //     here would be a shading change with no geometric change behind it,
    //     which is the one thing the history mechanism cannot express.
    //
    // WHAT IT DELIBERATELY IS NOT: a per-region wrinkle mask. Real wrinkles
    // appear where skin COMPRESSES, which is a per-target, per-region fact and
    // needs an authored map per morph target. That is a facial-rig authoring
    // tool, which issue #1243's scope boundary explicitly excludes and #1245
    // owns. This is one scalar for the whole surface, and the guide says so in
    // the same words so that nobody discovers the limitation by shipping a face.
    //
    // Non-finite weights are SKIPPED rather than propagated: MorphTargetComponent
    // rejects those at the setter, so one reaching here means a path that
    // bypassed it, and a NaN detail strength would take the whole material's
    // specular with it.
    [[nodiscard]] f32 SkinExpressionDetailWeight(std::span<const f32> appliedWeights) noexcept;

    // The per-draw pore-band gain: the profile's base plus its expression gain
    // times the weight above, clamped into the authored bound.
    //
    // Unitless, [kMinSkinDetailStrength, kMaxSkinDetailStrength]. It scales the
    // DIFFERENCE between the normal map at the fragment's mip and the same map
    // two mips coarser — the band a skin normal map carries its pores in. 0
    // leaves the authored normal untouched and -1 removes the band entirely.
    // See oloSkinDetailTangentNormal in include/SkinLayeredSpecular.glsl for why
    // the band is taken out of the existing map rather than out of a second one.
    [[nodiscard]] f32 SkinDetailStrength(const SkinSpecularParameters& parameters,
                                         f32 expressionWeight) noexcept;

    // -------------------------------------------------------------------------
    // The vec4 lane the GPU is handed
    // -------------------------------------------------------------------------

    // A skin profile's layered specular reaches both shading paths as ONE vec4 —
    // the forward path through the material UBO, the deferred path through the
    // per-frame profile table — for the reason SkinTransmissionScatterLane
    // states: two paths that are handed the same bytes cannot disagree about
    // what is in them.
    //
    //   x = LobeMix, w                  [0, 1]
    //   y = LobeRoughnessScale, s       [1, kMaxSkinLobeRoughnessScale]
    //   z = NormalVarianceStrength      [0, kMaxSkinNormalVarianceStrength]
    //   w = 0, reserved
    //
    // ONLY x AND y ARE READ BY THE DEFERRED LIGHTING PASS, and that is not a
    // waste. z is consumed where the normal is built — the G-Buffer writer on
    // the deferred path, the lit shader on the forward ones — so on deferred it
    // has already been spent by the time the lighting pass reads the lane, and
    // the filtered roughness is sitting in the G-Buffer. Shipping the same four
    // numbers to both places anyway keeps ONE packing function, which is the
    // whole point; a lane that meant different things in the two tables would be
    // the trap this arrangement exists to avoid.
    //
    // A ZERO-FILLED LANE IS NEUTRAL: w = 0 makes the mixture the narrow lobe
    // alone and z = 0 disables the filter, so an unclaimed or stale slot loses
    // the effect rather than acquiring someone else's.
    [[nodiscard]] glm::vec4 SkinSpecularLane(const SkinProfileParameters& parameters) noexcept;

    // Whether a profile's transport version evaluates the layered response at
    // all. The ONE place that test is spelled, so the submission path, the
    // deferred table and the editor cannot disagree about it.
    //
    // A `==` and not a `>=`, matching oloApplySkinProfile's version branch and
    // for its reason: a version this code has no arm for must apply NOTHING
    // rather than guess that a later transport meant the same thing by these
    // fields.
    [[nodiscard]] constexpr bool SkinEvaluatesLayeredSpecular(SkinEvaluationModel model) noexcept
    {
        return model == SkinEvaluationModel::LayeredSpecular || model == SkinEvaluationModel::OralSurface ||
               model == SkinEvaluationModel::OcularSurface;
    }

} // namespace OloEngine
