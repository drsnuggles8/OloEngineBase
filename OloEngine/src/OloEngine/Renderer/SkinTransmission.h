#pragma once

// =============================================================================
// SkinTransmission.h — the thin-region transmission maths #1242 adds on top of
// #1231's split and #1241's diffusion.
//
// EVERY PHYSICAL DECISION IN THIS FEATURE IS MADE HERE, ON THE CPU, for exactly
// the reason SkinDiffusion.h states: the one number this feature is most likely
// to get wrong is a UNIT, and a unit slip inside GLSL is a thing no test can
// look at. The shader is handed per-channel transmittance and a bounded lobe
// weight; it knows nothing about millimetres, mean free paths or Burley.
//
// THE UNIT CHAIN, STATED ONCE.
//
//   Material::GetThicknessFactor()   authored, METRES (glTF KHR_materials_volume)
//   x thickness map sample [0,1]     unitless modulation, per pixel
//   -> thicknessMetres               METRES
//   x SkinProfileParameters::ThicknessScale
//   -> t                             MILLIMETRES
//   / d                              Burley scaling, MILLIMETRES (SkinDiffusion.h)
//   -> optical depth                 UNITLESS, per channel
//   exp(-opticalDepth) x ScatterColor
//   -> transmittance                 UNITLESS [0,1], per channel
//
// `ThicknessScale` IS IN THIS CHAIN, AND ONLY IN THIS CHAIN. SkinDiffusion.h
// says so in the negative — "ThicknessScale is NOT in that chain ... it is the
// transmission knob, not a second opinion about how long a millimetre is" — and
// this is the chain it was reserved for. The two features therefore share
// `ScatterColor` and `ScatterRadiusMM` and divide `ThicknessScale` between
// them: one authored transport, used twice, which is the whole of this
// feature's answer to double-counting (see ENERGY, below).
//
// =============================================================================
// ENERGY: WHY DIFFUSION AND TRANSMISSION CANNOT DOUBLE-COUNT
// =============================================================================
//
// This is the failure mode #1242 exists to avoid, and it is the expensive kind:
// getting it wrong renders a head that looks FINE from the front and uniformly
// emissive from behind. So the argument is written out, and
// SkinTransmissionTest pins each of its three premises separately.
//
//   1. THE TWO TERMS DRAW FROM DISJOINT INCIDENT DIRECTIONS. The reflected
//      diffuse lobe carries a factor saturate(dot(N, L)) and is therefore zero
//      for every light on the far side of the surface. The transmitted lobe
//      carries saturate(-dot(N, L)) and is EXACTLY zero for every light on the
//      near side — no wrap, no half-Lambert, no epsilon. A photon arriving
//      along L is counted by one term or the other, never by both.
//
//      This is the one place this feature deliberately diverges from the
//      foliage lobe it otherwise resembles (FoliageLeafProfile.h). Foliage's
//      `Wrap` parameter pushes its transmission into dot(N, L) > 0 on purpose,
//      because a leaf is a thin sheet whose two faces are the same surface.
//      Skin is not: a cheek has a near face and an ear has a far one, and
//      wrapping would put transmitted energy on top of the reflected diffuse
//      lobe over a whole hemisphere. There is no Wrap here, and its absence is
//      what makes premise 1 an equality rather than an approximation.
//
//   2. DIFFUSION REDISTRIBUTES, IT DOES NOT ADD. #1241's kernel weights sum to
//      exactly 1 per channel across its taps, by construction rather than by a
//      normalising divide (SkinDiffusion.h), and the pass adds
//      `blur(aux) - aux`. So the diffuse half's total energy over the image is
//      unchanged by diffusion, which means premise 1's bound survives it.
//
//   3. THE LOBE WEIGHT NEVER EXCEEDS 1. SkinTransmissionLobe is bounded above
//      by 1 for every input, so the transmitted radiance is at most
//      `incident x transmittance`, and transmittance is at most `ScatterColor`,
//      which Sanitize holds in [0,1].
//
// Together: for any single light, diffuse + transmitted <= incident, per
// channel, everywhere. SkinTransmissionEnergyBound computes the left-hand side
// and SkinTransmissionTest sweeps it over the profile parameter range.
//
// WHAT SHARING `ScatterColor` DOES AND DOES NOT DOUBLE-COUNT. The transport
// albedo appears in both features, and that is not a double application of the
// same factor to the same energy: in the diffusion kernel it shapes the
// SPATIAL DISTRIBUTION of a fixed amount of energy (the weights are normalised,
// so ScatterColor moves energy around and never scales the total), while here it
// scales the MAGNITUDE of a different transport. Deliberately NOT a separate
// authored "transmission tint": a second colour would let an author set a
// transmission that the surface's own absorption says is impossible, and would
// make the energy bound above unprovable from the asset.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <glm/glm.hpp>

#include <string_view>

namespace OloEngine
{

    // -------------------------------------------------------------------------
    // Thickness
    // -------------------------------------------------------------------------

    // The authored thickness in MILLIMETRES, from a material's thickness factor
    // (METRES), a thickness map sample (unitless [0,1]) and the profile's
    // ThicknessScale.
    //
    // Returns 0 — meaning "no transmission here" — for a non-finite or
    // non-positive input rather than propagating it. A zero thickness is NOT
    // "infinitely thin, therefore fully transparent", which is the reading that
    // renders the uniformly emissive head: see kSkinThicknessMissing below and
    // docs/guides/skin-transmission.md.
    [[nodiscard]] f32 SkinThicknessMM(f32 thicknessMetres, f32 mapSample, f32 thicknessScale) noexcept;

    // The PER-DRAW half of that chain: `thicknessMetres x thicknessScale`,
    // MILLIMETRES, with no map sample in it.
    //
    // This is the number that travels to the GPU — PBRMaterialUBO's
    // SkinThicknessBaseMM on the forward path, and the RT5 thickness lane on
    // the deferred one — because the unit conversion is a physical decision and
    // the shader must not own one. Multiply it by a thickness map sample and you
    // have SkinThicknessMM; SkinTransmissionTest pins that identity, which is
    // what stops the two spellings drifting into different units.
    [[nodiscard]] f32 SkinThicknessBaseMM(f32 thicknessMetres, f32 thicknessScale) noexcept;

    // The sentinel a caller passes for "this material authored no thickness at
    // all". Spelled as a named zero because the number and its meaning are
    // decided together: SkinTransmittance returns black for it, so the whole
    // term vanishes rather than saturating.
    inline constexpr f32 kSkinThicknessMissing = 0.0f;

    // -------------------------------------------------------------------------
    // Transmittance
    // -------------------------------------------------------------------------

    // The per-channel fraction of light entering the far face that reaches the
    // near one: `ScatterColor * exp(-thicknessMM / d)`, with `d` the SAME Burley
    // scaling the diffusion kernel is built from (SkinBurleyScalingMM).
    //
    // Unitless [0,1] per channel, guaranteed finite. Red survives a trip a blue
    // photon does not, which is what makes a backlit ear red rather than white,
    // and it is the authored profile — not a tint — that says so.
    //
    // `parameters` must already be sanitized; every path in goes through
    // SkinProfileTable, which only hands out sanitized parameters.
    [[nodiscard]] glm::vec3 SkinTransmittance(f32 thicknessMM, const SkinProfileParameters& parameters) noexcept;

    // -------------------------------------------------------------------------
    // The exit lobe
    // -------------------------------------------------------------------------

    // The bounded exit lobe weight, unitless [0, 1].
    //
    //   backFacing = saturate(-dot(N, L))        coupling into the FAR face
    //   forward    = saturate(dot(V, -L))        viewer looking toward the light
    //   lobe       = backFacing * mix(1, forward^P, g)
    //
    // EXACTLY ZERO FOR dot(N, L) >= 0. That is premise 1 of the energy argument
    // and the "vanishes toward front lighting" half of the issue's second
    // acceptance criterion, and it is a property of this expression rather than
    // of a threshold: saturate(-dot(N, L)) is zero on the whole near hemisphere.
    //
    // BOUNDED, NOT NORMALISED, AND THAT IS THE CHOICE. A normalised phase
    // function integrates to 1 over the sphere and therefore EXCEEDS 1 at its
    // peak — which would break premise 3 and let a grazing view add energy. The
    // `mix` form is a convex combination of two numbers in [0,1], so it cannot
    // exceed 1 for any inputs. The cost is that the forward peak is slightly
    // under-represented compared with a true Henyey-Greenstein; the benefit is
    // that "the head cannot glow brighter than the light behind it" is a fact
    // about the arithmetic rather than about the parameter values.
    [[nodiscard]] f32 SkinTransmissionLobe(const glm::vec3& normal, const glm::vec3& view,
                                           const glm::vec3& lightDir,
                                           const SkinTransmissionParameters& parameters) noexcept;

    // -------------------------------------------------------------------------
    // The two vec4 lanes the GPU is handed
    // -------------------------------------------------------------------------

    // A skin profile's transmission reaches BOTH shading paths as exactly two
    // vec4s — the forward path through the material UBO, the deferred path
    // through the per-frame profile table — so that the two cannot differ about
    // the ORDER the factors multiply in. Same arrangement, and the same reason,
    // as FoliageLeafProfileTintLane / FoliageLeafProfileLobeLane.
    //
    // `Strength` IS PRE-MULTIPLIED INTO THE SCATTER LANE. The shader never sees
    // the two apart, which means it cannot apply them in the wrong order or
    // forget one; and because ScatterColor and Strength are both held in [0,1]
    // by Sanitize, their product is too, so the energy bound is unchanged by the
    // folding.
    //
    // `d` IS COMPUTED HERE, NOT IN GLSL. SkinBurleyScalingMM is the Burley
    // albedo fit — a physical decision, which by this file's opening rule belongs
    // on the CPU where SkinTransmissionTest can look at it. The shader divides
    // by the number and knows nothing about where it came from.
    //
    //   scatter: xyz = ScatterColor * Strength (linear Rec.709, unitless [0,1])
    //            w   = Anisotropy, `g`
    //   scaling: xyz = SkinBurleyScalingMM, MILLIMETRES, strictly positive
    //            w   = Power, `P`
    [[nodiscard]] glm::vec4 SkinTransmissionScatterLane(const SkinProfileParameters& parameters) noexcept;
    [[nodiscard]] glm::vec4 SkinTransmissionScalingLane(const SkinProfileParameters& parameters) noexcept;

    // The floor the shader clamps the scaling lane against before dividing.
    // MILLIMETRES. SkinBurleyScalingMM cannot return zero for a sanitized
    // profile, so this never binds in production — it is there so that a
    // zero-filled UBO slot (an unclaimed slot, a stale frame) divides by
    // something rather than producing an infinity that becomes a NaN pixel.
    inline constexpr f32 kMinSkinTransmissionScalingMM = 1.0e-6f;

    // -------------------------------------------------------------------------
    // The whole term
    // -------------------------------------------------------------------------

    // The transmitted radiance for ONE light, linear Rec.709.
    //
    //   albedo      the surface colour the exiting light picks up, [0,1]
    //   radiance    the light's incident radiance at this point, >= 0
    //   visibility  the SHARED shadow/occlusion factor [0,1] — the same number
    //               the reflected lobe was scaled by, which is what makes
    //               occlusion modulate this term (criterion 2) rather than
    //               leaving a shadowed ear glowing
    //
    // `lightDir` points FROM the surface TOWARD the light, the convention
    // oloLightSample uses. `view` points from the surface toward the eye.
    //
    // THIS FUNCTION IS THE SPECIFICATION AND ALSO A RUNTIME PATH. Unlike
    // SkinDiffusionRadiusPixels, nothing here depends on a per-pixel quantity the
    // CPU lacks, so the GLSL in include/SkinTransmission.glsl is a transcription
    // of this expression in this order, and SkinTransmissionParityEvidenceTest
    // drives the shader against this function.
    [[nodiscard]] glm::vec3 EvaluateSkinTransmission(const glm::vec3& normal, const glm::vec3& view,
                                                     const glm::vec3& lightDir, const glm::vec3& radiance,
                                                     const glm::vec3& albedo, f32 visibility, f32 thicknessMM,
                                                     const SkinProfileParameters& parameters) noexcept;

    // The same term, taking the two GPU LANES instead of the profile — the exact
    // expression include/SkinTransmission.glsl's oloSkinTransmissionDirect
    // evaluates, in the same order, operation for operation.
    //
    // Exposed rather than left private because it is what the parity test drives:
    // comparing the shader against THIS is a test of the transcription, whereas
    // comparing it against EvaluateSkinTransmission above would also be testing
    // the lane packing, and a failure could not tell the two apart.
    //
    // Takes its inputs ALREADY RANGE-CHECKED — EvaluateSkinTransmission does
    // that and then calls here, exactly as the shader relies on its caller
    // having clamped. It performs no finiteness test of its own for the same
    // reason the shader performs none: a NaN in must be a NaN out here, or the
    // parity test would be comparing a guarded CPU path against an unguarded
    // GPU one and would pass while they disagreed.
    [[nodiscard]] glm::vec3 EvaluateSkinTransmissionLanes(const glm::vec3& normal, const glm::vec3& view,
                                                          const glm::vec3& lightDir, const glm::vec3& radiance,
                                                          const glm::vec3& albedo, f32 visibility, f32 thicknessMM,
                                                          const glm::vec4& scatterLane,
                                                          const glm::vec4& scalingLane) noexcept;

    // -------------------------------------------------------------------------
    // The energy bound
    // -------------------------------------------------------------------------

    // The largest per-channel fraction of one light's incident radiance that
    // the DIFFUSE and TRANSMITTED transports can jointly deliver at a pixel of
    // this thickness, over every geometric configuration.
    //
    // This is the quantity the fourth acceptance criterion is about, expressed
    // so a test can sweep it: it must never exceed 1 in any channel, for any
    // sanitized profile and any thickness. It is a MAXIMUM over geometry, not a
    // sample of it — premise 1 says the two terms cannot both be non-zero at
    // once, so the joint maximum is the larger of the two, and the larger is
    // whichever transport the parameters favour.
    //
    // The diffuse side enters as `albedo`, its own Lambertian bound, rather than
    // as a measured frame: diffusion preserves it (premise 2), so the bound is
    // the same before and after the pass.
    [[nodiscard]] glm::vec3 SkinTransmissionEnergyBound(f32 thicknessMM, const glm::vec3& albedo,
                                                        const SkinProfileParameters& parameters) noexcept;

} // namespace OloEngine
