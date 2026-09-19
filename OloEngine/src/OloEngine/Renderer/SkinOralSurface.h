#pragma once

// =============================================================================
// SkinOralSurface.h — the wet coat and the cavity term #1245 adds on top of the
// layered surface response, for lips, gums, tongue and teeth.
//
// THE MATHS LIVES HERE AND THE SHADER TRANSCRIBES IT, which is the arrangement
// Renderer/SkinTransmission.h and Renderer/SkinLayeredSpecular.h established and
// the reason both of them state: a physical decision that only exists inside a
// `.glsl` file cannot be unit tested, cannot be reasoned about by anything that
// does not have a GL context, and drifts silently between the three lighting
// paths. include/SkinOralSurface.glsl is a transcription of this file, in the
// same order, operation for operation, and SkinOralSurfaceParityTest drives the
// two against each other.
//
// -----------------------------------------------------------------------------
// WHAT AN ORAL SURFACE IS THAT SKIN IS NOT
// -----------------------------------------------------------------------------
//
// Two things, and they are the two this file adds.
//
//   A WET FILM. Lips, gums and a tongue are covered by a thin layer of saliva,
//   and teeth by a film over enamel. That film is a SECOND, SMOOTH DIELECTRIC
//   INTERFACE in front of the tissue: it reflects a small, achromatic,
//   view-dependent fraction of the light, and it passes the rest to the surface
//   underneath. It is what makes a mouth read as wet, and it is high-frequency
//   in a way the tissue underneath never is.
//
//   AN ENCLOSING CAVITY. The inside of a mouth is a nearly closed volume. Every
//   other term in this engine's skin transport is either shadowed (the direct
//   lobes) or occluded (the ambient ladder, by the material's AO). The THIN-
//   REGION TRANSMISSION TERM of issue #1242 is neither: it is a thickness-driven
//   approximation with no visibility of its own beyond the light's shadow
//   factor, which is exactly correct for an ear held up against the sun and
//   exactly wrong for a tongue inside a closed mouth. Left alone it makes the
//   interior of a closed mouth glow — the failure #1245's second acceptance
//   criterion names in those words.
//
// -----------------------------------------------------------------------------
// WHY THE COAT TAKES ENERGY RATHER THAN ADDING IT
// -----------------------------------------------------------------------------
//
// The coat is a layer IN FRONT of the skin response, not a highlight painted on
// top of it. Light that reflects off the film never reaches the tissue, so the
// tissue's response must lose exactly what the film gained:
//
//     attenuation = 1 - strength * F_coat
//     out.Diffuse  = base.Diffuse  * attenuation
//     out.Specular = base.Specular * attenuation + coatSpecular * strength
//
// `strength * F_coat` and `attenuation` sum to one for every legal input — a
// PARTITION, not a blend — which is what SkinOralSurfaceTest pins, and what
// makes the statement "the coat cannot brighten a surface on average" checkable
// rather than aesthetic.
//
// The "on average" is load-bearing and is stated rather than hidden: a GGX lobe
// concentrates the energy it is given, so the coat CAN be brighter than what it
// replaced in the highlight's few pixels, and darker everywhere else. What is
// bounded is the hemispherical integral — the coat's directional albedo never
// exceeds UNITY, because Smith-masked GGX with no multiple-scattering
// compensation loses energy and never creates it. SkinOralSurfaceTest
// integrates the lobe numerically and asserts that bound, because the
// alternative is believing it.
//
// THE BOUND IS 1 AND NOT F0, and the looser number is the honest one. The
// Schlick inside the lobe runs up toward 1 as the half-vector approaches
// grazing, so a wide lobe at an oblique view legitimately returns MORE than F0
// — 1.7x F0 at roughness 0.8 and dot(N, V) 0.25, measured. Claiming F0 here
// would be claiming a near-normal coincidence.
//
// -----------------------------------------------------------------------------
// WHY THE CAVITY TERM GATES THE TRANSMISSION AND NOTHING ELSE
// -----------------------------------------------------------------------------
//
// Because the transmission is the only term that is not already occluded, and
// applying an occlusion twice is as wrong as applying it never.
//
//   * the direct lobes are gated by `lightVisibility` — the shadow map, the
//     cloud shadow and the ray-traced mask, multiplied into one factor;
//   * the ambient ladder is already scaled by the material's AO, at the same
//     site on all three paths;
//   * the transmitted lobe is gated by `lightVisibility` too (issue #1242's
//     second criterion) and by NOTHING ELSE. A closed mouth casts no shadow onto
//     its own interior at the resolution any shadow map here runs at, so the
//     lips stay lit from inside.
//
// So the cavity weight multiplies the transmitted lobe, and the authored
// `CavityOcclusion` says how much of the material's own AO to spend on it. 0
// leaves the term exactly as #1242 shipped it; 1 hands it the AO in full.
//
// DERIVED FROM THE MATERIAL'S AO RATHER THAN FROM A NEW MAP, for the reason
// oloSkinDetailTangentNormal takes its detail band out of the normal map that is
// already there: a second mask would need a texture slot, a heap offset, an
// importer path and an authoring convention, and it would introduce a way for
// the occlusion to disagree with the AO the ambient ladder is already using on
// the same texels. An oral cavity is the one place a head's AO map is
// unambiguous, which makes it the right signal rather than merely the available
// one.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <glm/glm.hpp>

namespace OloEngine
{

    // The index of refraction of air, the medium the coat is seen through.
    // Named rather than spelled 1.0 inline so SkinOralCoatF0's formula reads as
    // the Fresnel-at-normal-incidence expression it is.
    inline constexpr f32 kSkinOralAmbientIor = 1.0f;

    // @brief Fresnel reflectance at normal incidence for a coat of index `ior`
    //        seen from air.
    //
    //     F0 = ((ior - 1) / (ior + 1))^2
    //
    // THE AUTHORED FIELD IS THE IOR AND NOT F0, and that is the one authoring
    // decision in this file worth defending. F0 is the number the shader wants;
    // the IOR is the number that exists in a reference table. Saliva is 1.33
    // (it is essentially water), enamel is 1.63, and an author who has to type
    // 0.0201 and 0.0574 instead is typing numbers they cannot check. The
    // conversion happens once, here, on the CPU, and the lane carries the
    // result — so there is no second opinion about it in any shader.
    //
    // An `ior` of exactly 1 gives F0 = 0, which is a coat that reflects nothing:
    // the neutral end of the range, and the reason kMinSkinOralCoatIor is 1
    // rather than something above it.
    [[nodiscard]] f32 SkinOralCoatF0(f32 ior) noexcept;

    // @brief The Schlick Fresnel of the coat at `cosTheta`.
    //
    // ACHROMATIC, DELIBERATELY. A water film has no absorption worth modelling
    // over the tenth of a millimetre it is thick, so its reflectance is the same
    // in all three channels and a vec3 here would be three copies of one number
    // plus an invitation to tint it. The tissue underneath is where colour comes
    // from, and the profile already has SpecularTint and ScatterColor for it.
    //
    // `cosTheta` is dot(V, H) — the angle between the VIEW direction and the
    // HALF-VECTOR, i.e. the angle of incidence on the microfacet that actually
    // reflected this light. It is NOT dot(N, V) (the macro-surface view angle)
    // and it is NOT dot(N, H) (how far that microfacet is tilted).
    //
    // dot(N, H) IS THE ERROR THIS COMMENT USED TO PRESCRIBE, and it shipped in
    // the first version of #1245. It is wrong in a way no still frame shows:
    // near normal incidence the two agree to five decimal places, so every
    // parity case at a head-on fixture passes either way. They diverge at
    // GRAZING — at dot(N,H) 0.30 against dot(V,H) 0.97 the saliva Fresnel is
    // 0.182 against 0.020, a factor of NINE — which is exactly where a wet
    // surface is most interesting and where the coat's whole rim response is.
    //
    // Every other Fresnel in this engine takes the same quantity:
    // include/PBRCommon.glsl passes `dot(H, V)` / `VdotH` at all six of its
    // microfacet call sites, and `dot(N, V)` only to fresnelSchlickRoughness,
    // which is the IBL split-sum approximation and a different integral.
    [[nodiscard]] f32 SkinOralCoatFresnel(f32 f0, f32 cosTheta) noexcept;

    // @brief How much of the base response survives the coat.
    //
    //     1 - clamp(strength) * clamp(fresnel)
    //
    // Returns a value in [0, 1] for every input, including non-finite ones,
    // because it is multiplied into a radiance that has nowhere to put a NaN.
    [[nodiscard]] f32 SkinOralCoatAttenuation(f32 strength, f32 fresnel) noexcept;

    // @brief The coat's specular lobe for one light direction — Cook-Torrance
    //        GGX with a Smith height-correlated visibility term, achromatic.
    //
    // Returns the BRDF value, NOT the radiance: the caller multiplies by the
    // incident radiance and by dot(N, L), exactly as calculateLightContribution
    // does for the surface lobe, so the coat is gated by the same shadow factor
    // and the same light sampling as everything else.
    //
    // A SEPARATE, SMALLER GGX RATHER THAN A THIRD ENTRY IN #1243's MIXTURE,
    // and the difference is not cosmetic. The layered mixture of issue #1243 is
    // CONVEX — it redistributes the surface's own specular between two widths
    // and cannot change how much of it there is. The coat is a different
    // interface with its own Fresnel and its own index of refraction, and it
    // takes energy from the diffuse half as well as from the specular one. A
    // third lobe in a convex mixture cannot do that, and adding it there would
    // have made the "wet specular stays distinct from diffusion" criterion a
    // matter of tuning rather than of structure.
    [[nodiscard]] f32 SkinOralCoatSpecular(const glm::vec3& normal, const glm::vec3& view,
                                           const glm::vec3& lightDir, f32 coatRoughness,
                                           f32 coatF0) noexcept;

    // @brief Apply the coat to one light's already-evaluated diffuse/specular
    //        split. The ONE place the partition above is written on this side.
    //
    // `radiance` is the incident radiance already scaled by dot(N, L) — what the
    // split it is being applied to was itself scaled by — so the coat's lobe and
    // the surface's lobe are lit by the same number and the partition holds per
    // light rather than only on average over the frame.
    struct SkinOralCoatResult
    {
        glm::vec3 Diffuse{ 0.0f };
        glm::vec3 Specular{ 0.0f };
    };

    [[nodiscard]] SkinOralCoatResult ApplySkinOralCoat(const glm::vec3& baseDiffuse,
                                                       const glm::vec3& baseSpecular,
                                                       const glm::vec3& normal, const glm::vec3& view,
                                                       const glm::vec3& lightDir,
                                                       const glm::vec3& radiance,
                                                       const glm::vec4& oralLane) noexcept;

    // @brief How much of the transmitted lobe survives the cavity.
    //
    //     mix(1, clamp(occlusion), clamp(cavityOcclusion))
    //
    // `cavityOcclusion` 0 returns exactly 1 — not approximately, exactly, since
    // `1 + 0 * (ao - 1)` is 1 in IEEE 754 for every finite ao. That exactness is
    // what makes "cavity off" the bit-identical #1243 frame a golden image can
    // assert against, rather than a frame that merely looks the same.
    [[nodiscard]] f32 SkinOralCavityWeight(f32 occlusion, f32 cavityOcclusion) noexcept;

    // @brief The four numbers the GPU needs, packed once for BOTH paths.
    //
    //   x = CoatStrength      y = CoatRoughness
    //   z = CoatF0 (DERIVED from CoatIor — see SkinOralCoatF0)
    //   w = CavityOcclusion
    //
    // Packed by one function for the reason SkinSpecularLane and the two
    // transmission lanes are: the forward path reads it out of the material UBO
    // and the deferred path out of the per-frame profile table, and a lane that
    // meant different things in the two tables would be the trap the shared
    // packer exists to avoid.
    //
    // A ZERO-FILLED LANE IS NEUTRAL: x = 0 removes the coat entirely (the
    // attenuation becomes 1 and the lobe is never evaluated) and w = 0 leaves
    // the transmitted term untouched, so an unclaimed or stale slot loses the
    // effect rather than acquiring someone else's wetness.
    [[nodiscard]] glm::vec4 SkinOralLane(const SkinProfileParameters& parameters) noexcept;

    // @brief Whether a profile's transport version evaluates the oral terms at
    //        all. The ONE place that test is spelled on this side.
    //
    // A `==` and not a `>=`, matching every version branch that came before it
    // and for their reason: a version this code has no arm for must apply
    // NOTHING rather than guess that a later transport meant the same thing by
    // these fields.
    [[nodiscard]] constexpr bool SkinEvaluatesOralSurface(SkinEvaluationModel model) noexcept
    {
        return model == SkinEvaluationModel::OralSurface;
    }

} // namespace OloEngine
