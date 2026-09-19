#pragma once

// =============================================================================
// SkinOcularSurface.h — the cornea, the iris behind it and the tear line, for
// the reference head's eyes. Issue #1244.
//
// THE MATHS LIVES HERE AND THE SHADER TRANSCRIBES IT, which is the arrangement
// Renderer/SkinTransmission.h, Renderer/SkinLayeredSpecular.h and
// Renderer/SkinOralSurface.h established and the reason all three state: a
// physical decision that only exists inside a `.glsl` file cannot be unit
// tested, cannot be reasoned about by anything that does not have a GL context,
// and drifts silently between the three lighting paths.
// include/SkinOcularSurface.glsl is a transcription of this file, in the same
// order, operation for operation, and SkinOcularSurfaceParityTest drives the
// two against each other.
//
// -----------------------------------------------------------------------------
// WHAT AN EYE IS THAT SKIN IS NOT
// -----------------------------------------------------------------------------
//
// ONE THING, and everything below follows from it: an eye's visible colour is
// BEHIND A LENS. The iris sits about 2.5 mm under a transparent dome of index
// 1.336, so a viewer never sees the iris where it is — they see it refracted,
// displaced, and magnified about 1.13x. Every other surface in this engine,
// skin included, shows its albedo where its albedo is.
//
// That is the whole feature. A painted iris — one sampled at the surface point
// — is wrong by **55% of the iris radius** at a 60-degree view, measured in
// experiments/eye-cornea-reference/compare_refraction.py against a two-surface
// ray trace through the clinical geometry. It is wrong by ZERO at a head-on
// view, which is exactly why a static front-on capture cannot tell the two
// models apart and why issue #1244's third acceptance criterion is about
// MOTION.
//
// -----------------------------------------------------------------------------
// WHY THIS IS A SKIN TRANSPORT VERSION AND NOT A NEW MaterialKind
// -----------------------------------------------------------------------------
//
// Because an eye is not one surface, it is three, and two of them are already
// skin. The SCLERA is collagen that scatters — it is white the way a fingernail
// is white, not the way chalk is — so it wants version 1's Burley diffusion.
// The TEAR FILM is a thin water layer in front of wet tissue, which is
// version 4's wet coat with a different index (tears 1.337, saliva 1.330 — an
// F0 of 0.0208 against 0.0201, a 3% difference; question 7 of the reference).
// Only the CORNEA is new, and what it adds is a refraction, not a closure.
//
// So version 5 adds ONE term to version 4 and reuses four. A `MaterialKind::Eye`
// would have had to re-implement the diffusion, the transmission, the layered
// specular and the coat to get back to where version 4 already is. See
// docs/adr/0024-material-kind-is-not-the-closure-version.md.
//
// -----------------------------------------------------------------------------
// WHERE IT RUNS, AND WHY THAT IS NOT WHERE #1245 RUNS
// -----------------------------------------------------------------------------
//
// IN THE MATERIAL STAGE, NOT THE LIGHTING STAGE. The wet coat of #1245 is a
// second BRDF: it needs a light direction, so it runs per light, in all three
// lighting paths, and the deferred path needed a whole per-frame table to reach
// it. The corneal refraction is not a BRDF at all — it changes WHICH POINT OF
// THE SURFACE YOU ARE LOOKING AT, which is a property of the view alone.
//
// So it runs once, in the fragment shader that resolves the material:
// PBR_MultiLight{,_Skinned}.glsl on the forward and clustered paths, and
// PBR_GBuffer{,_Skinned}.glsl on the deferred one. The deferred lighting pass
// never learns that eyes exist — it is handed an albedo and a normal that
// already have the cornea in them.
//
// THREE CONSEQUENCES, all of them load-bearing:
//
//   * The three render paths agree BY CONSTRUCTION rather than by three
//     matching edits. Issue #1244's fourth criterion asks for cost and quality
//     across the intended paths; the answer is "identical, because it is the
//     same code at the same stage", which is a structural answer rather than a
//     measured coincidence.
//   * There is no G-Buffer lane and no deferred profile table entry. The
//     G-Buffer flags lane that #1288 is the receipt for is not touched.
//   * LAYER SORTING CANNOT GO WRONG, which is #1244's second criterion in the
//     words it uses. Cornea, iris and tear film are not three depth-sorted
//     surfaces here; they are three terms evaluated in a fixed order at one
//     surface — refract, then iris response, then the surface closure, then the
//     coat. The order is a property of where the code is, and
//     SkinOcularSurfaceTest pins it.
//
// -----------------------------------------------------------------------------
// THE OPTICAL AXIS COMES FROM THE ENTITY TRANSFORM, AND THAT IS THE LEFT/RIGHT
// CONVENTION
// -----------------------------------------------------------------------------
//
// An eye needs to know which way it is looking, and that is a property of the
// ENTITY, not of the profile: two eyes in one head share every optical constant
// and differ only in where they point.
//
// So the axis is the entity transform's +Z in world space — `u_Model[2].xyz`,
// which every fragment stage that shades skin already has through
// include/InstanceBlock.glsl. Nothing is plumbed, no material is copied per
// draw, and no lane carries it.
//
// THAT IS THE PREDICTABLE LEFT/RIGHT CONVENTION issue #1244's first acceptance
// criterion asks for, and it is worth stating in the terms the criterion uses:
//
//     BOTH EYES NAME THE SAME `.oloskin`. There is no left profile and no right
//     profile, no mirrored asset and no `IsLeftEye` flag. The eyes differ by
//     their transforms' rotation and by nothing else, so a gaze change is a
//     transform change and an iris colour change is a one-file edit.
//
// A flag would have been the obvious alternative and it is worse in a specific
// way rather than merely inelegant: it puts a per-entity fact in a per-asset
// place, so the day an author wants a third eye looking somewhere else they
// need a third profile that differs from the other two in one bool.
//
// THE CONVENTION HAS ONE REQUIREMENT, AND IT IS STATED RATHER THAN ENFORCED PER
// PIXEL: the eye mesh must be a UNIFORMLY SCALED sphere. The shader reads the
// globe's radial direction out of the interpolated normal, and a non-uniform
// scale makes the normal stop being radial — the eye-local frame is then wrong
// everywhere and the iris shears.
//
// NOT CHECKED IN THE SHADER, deliberately. The test would be three column
// lengths of u_Model compared per pixel, to reach a conclusion that is constant
// over the whole draw; and having reached it there is nothing useful to do —
// falling back to a painted iris would hide the authoring error behind a
// plausible frame, which is the failure mode this repo's `no-silent-fallbacks`
// rule exists to stop. So the requirement is documented here and in
// docs/guides/eye-cornea-iris.md, and the shipped scene obeys it.
//
// AND A SECOND REQUIREMENT THAT FOLLOWS FROM THE SAME LINE OF CODE: EACH EYE
// MUST BE ITS OWN ENTITY. `u_Model` is the DRAW's transform, so on a skinned
// head whose eyes are submeshes of one skinned mesh, both eyes would be handed
// the character's forward axis while their normals came from the bone
// deformation — every eye in the scene refracting about the head's axis rather
// than its own, which reads as two eyes that will not converge.
//
// This is a real limit of reading the axis from the model matrix, and the
// alternative — resolving a per-eye BONE transform in the fragment stage —
// would need the bone index routed through a flat varying on four shaders and a
// palette lookup per pixel, to serve an asset shape this repo does not have.
// The eye is a sphere primitive in every scene here and in
// Assets/Scenes/Eyes.olo, so the per-entity form is also the natural one; it is
// written down rather than assumed, because the failure is silent and the fix
// (split the eyes out) is trivial once you know.
//
// -----------------------------------------------------------------------------
// WHY ONE REFRACTING SURFACE, AT THE AQUEOUS INDEX, WITH A BENT NORMAL
// -----------------------------------------------------------------------------
//
// Three decisions, and all three were made against
// experiments/eye-cornea-reference/compare_refraction.py BEFORE this file
// existed — which is what issue #1244's third acceptance criterion asks for and
// the reason the experiment is committed beside the implementation. The ground
// truth there is a two-surface trace (air -> stroma 1.376 -> aqueous 1.336)
// through clinical radii, and it is validated against an EXTERNAL number first:
// it reproduces the literature's 1.13x entrance-pupil magnification as 1.123x,
// by two independent methods that agree to 0.001 mm.
//
// ONE SURFACE, NOT TWO. Worst-case error over a 0-60 degree sweep, as a
// fraction of the iris radius:
//
//     painted (no offset)          54.7%     <- the thing being replaced
//     march, no refraction         43.3%     <- a transparent shell, no shader
//     one surface, n = 1.376        2.0%
//     one surface, n = 1.3375       0.1%
//     one surface, n = 1.336        0.1%     <- what ships
//
// AT THE AQUEOUS INDEX AND NOT THE STROMAL ONE, which is the decision in that
// table worth defending because 1.376 is the number an author would look up for
// "cornea". The cornea is 0.55 mm of n 1.376 sitting on 3 mm of n 1.336, and
// the ray spends almost all of its path in the aqueous: the index that matters
// is the one it ENDS in, not the one it passes through. Using the stromal index
// is twenty times worse than using the aqueous one, and it is wrong in the
// direction that looks fine — it under-refracts, so the eye reads slightly
// painted.
//
// AND WITH THE NORMAL BENT. The engine's eye is a SPHERE primitive, so the only
// normal the mesh supplies is the globe's, and the globe is much flatter than
// the corneal dome it stands in for. Same sweep, same units:
//
//     sphere mesh, globe normal    10.2%
//     sphere mesh, bent normal      7.5%     <- what ships
//
// The bend is one line — see SkinCornealNormal — and it is the difference
// between recovering 81% and 86% of what the painted arm gets wrong.
//
// THE 7.5% RESIDUAL IS STATED RATHER THAN HIDDEN, and it is an ASSET
// limitation, not a shader one. A sphere has no corneal bulge, so the ray
// enters 1.1 mm behind where it should; that is an ENTRY-POINT error and no
// amount of parameter authoring reaches an entry point. Question 4b of the
// reference sweeps the authored iris depth to prove it: the curve is
// flat-bottomed at ~7% and never approaches zero. An eye mesh with real corneal
// geometry sets EyeGeometry.CorneaRadiusMM equal to EyeRadiusMM — no bend — and
// lands at the 0.1% row.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <glm/glm.hpp>

namespace OloEngine
{

    // The index of refraction of air, the medium the eye is seen through. Named
    // rather than spelled 1.0 inline so SkinCorneaEta reads as the ratio it is.
    inline constexpr f32 kSkinOcularAmbientIor = 1.0f;

    // @brief The refraction ratio the shader's `refract()` takes, for a cornea
    //        of index `ior` entered from air.
    //
    //     eta = n_air / n_cornea
    //
    // THE AUTHORED FIELD IS THE IOR AND THE LANE CARRIES ETA, for the reason
    // SkinOralCoatF0 gives about F0: the IOR is the number that exists in a
    // reference table and eta is the number the shader wants. The conversion
    // happens once, here, on the CPU, so no shader has a second opinion about
    // which way round the ratio goes — and that is a real hazard rather than a
    // stylistic one, because 1/1.336 and 1.336 both produce a refracted ray and
    // only one of them bends the right way.
    [[nodiscard]] f32 SkinCorneaEta(f32 ior) noexcept;

    // @brief The corneal dome's normal at a point whose GLOBE normal is
    //        `globeNormal`, for an eye whose dome is `curvatureRatio` times
    //        steeper than its globe.
    //
    //     sin(phi) = curvatureRatio * sin(theta)
    //
    // where theta is the globe normal's angle from the optical axis and phi is
    // the corneal normal's. DERIVED, not fitted: a point on the globe at polar
    // angle theta has lateral radius R_globe * sin(theta), and the point of the
    // corneal cap at that SAME lateral radius has its normal at phi with
    // sin(phi) = (R_globe / R_cornea) * sin(theta). So the ratio is exactly
    // EyeRadiusMM / CorneaRadiusMM and SkinOcularCorneaLane derives it there.
    //
    // A ratio of 1 returns `globeNormal` unchanged, which is both the neutral
    // case and the correct answer for a mesh that already carries real corneal
    // geometry — its interpolated normal IS the corneal normal and bending it
    // again would be applying the same curvature twice.
    //
    // CLAMPED AT sin(phi) = 1, which is a real surface and not a numerical
    // guard: beyond it the corneal cap has ended. Inside the limbus of a
    // clinical eye the product never exceeds 0.75, so the clamp is unreachable
    // on a correctly authored profile and exists for the one that is not.
    [[nodiscard]] glm::vec3 SkinCornealNormal(const glm::vec3& globeNormal, const glm::vec3& axis,
                                              f32 curvatureRatio) noexcept;

    // @brief Snell's law in the exact form GLSL's `refract()` takes.
    //
    // `incident` points ALONG the ray, INTO the surface — so a caller holding a
    // view vector V that points from the surface toward the eye passes `-V`.
    // Getting that sign wrong produces a refracted ray that is plausible,
    // continuous and pointing out of the eye, which shades as a painted iris
    // with extra steps.
    //
    // Returns false on TOTAL INTERNAL REFLECTION, where no refracted direction
    // exists. The out parameter is left untouched in that case rather than set
    // to zero: a zero direction marches nowhere and lands on the entry point,
    // which is indistinguishable from a painted eye and is precisely the silent
    // failure this whole file argues against. The caller reports it.
    [[nodiscard]] bool SkinOcularRefract(const glm::vec3& incident, const glm::vec3& normal, f32 eta,
                                         glm::vec3& refracted) noexcept;

    // @brief Where a refracted ray lands on the iris plane, in EYE RADII.
    //
    // The whole march is scale-free and that is deliberate. Working in units of
    // the eye's own radius means the shader never needs the eye's world size,
    // never needs its centre, and cannot be broken by a scene authored in
    // centimetres. The entry point is the globe surface point, which on a unit
    // sphere IS the normal — so `globeNormal` is the position as well as the
    // direction, and no vertex data beyond the normal is required.
    //
    // `irisPlaneDepth` is the axial distance from the eye's APEX to the iris
    // plane, in eye radii, so the plane sits at axial coordinate
    // `1 - irisPlaneDepth`.
    //
    // Returns false when the refracted ray does not travel inward — it cannot
    // reach the plane, and extrapolating it backwards would land the iris in
    // front of the eye.
    [[nodiscard]] bool SkinIrisPlaneHit(const glm::vec3& globeNormal, const glm::vec3& axis,
                                        const glm::vec3& refracted, f32 irisPlaneDepth,
                                        glm::vec3& hit) noexcept;

    // @brief How much of the iris disc this disc coordinate is inside: 1 well
    //        within the iris, 0 at and beyond its edge.
    //
    //     1 - smoothstep(1 - band, 1, radial)
    //
    // The ONE definition of "where the iris stops", used by the tint and
    // sharing its boundary with SkinIrisLimbalRing — see SkinOcularTintLane for
    // why those two are not allowed to disagree.
    [[nodiscard]] f32 SkinIrisDiscMask(f32 radial, f32 band) noexcept;

    // @brief The iris disc coordinate of a point on the iris plane: its
    //        distance from the optical axis, divided by the iris radius.
    //
    // 0 at the pupil centre, 1 at the iris edge. UNITLESS on purpose — it is
    // the coordinate every response below is a function of, and expressing the
    // limbal ring's width in the same unit is what lets an author move the iris
    // radius without re-tuning the ring.
    [[nodiscard]] f32 SkinIrisRadialCoordinate(const glm::vec3& irisPlanePoint, const glm::vec3& axis,
                                               f32 irisRadius) noexcept;

    // @brief How much of the iris albedo survives the limbal ring at disc
    //        coordinate `radial`.
    //
    //     1 - strength * smoothstep(1 - 2*band, 1 - band, radial)
    //
    // THE BAND SITS ONE BAND-WIDTH INSIDE THE IRIS EDGE, not flush against it,
    // and that placement is what keeps the limbus continuous. SkinIrisDiscMask
    // fades the WHOLE iris response out over [1 - band, 1] so the sclera beyond
    // it is untouched; a ring that peaked at radial 1 would be peaking exactly
    // where that fade has reached zero, and the two would fight — leaving a
    // hard step at the limbus of about 2x the albedo, one pixel wide, all the
    // way around the iris. That is the crawling ring the pupil's soft edge and
    // the dish's edge ramp both go out of their way to avoid.
    //
    // THE LIMBAL RING IS NOT DECORATION. The iris root passes under the corneal
    // limbus, where the anterior chamber is shallowest and the sclera overhangs
    // it, so the outermost iris is genuinely in shadow. It reads as a dark ring
    // and its WIDTH IN THE FRAME CHANGES WITH VIEW ANGLE, because it is being
    // seen through the same refraction the rest of the iris is — which is one of
    // the two cues that separate a refracting eye from a painted one at a glance
    // (the other is the pupil's parallax).
    //
    // A strength of 0 returns exactly 1 for every input, so a profile that did
    // not author a ring is bit-identical to one that has no ring code.
    //
    // `band` is the derived ring width in disc coordinates, floored at
    // kMinSkinIrisEdgeBand — see kMinSkinLimbalRingWidthRatio for why a zero
    // there is a NaN on the GPU and not merely a degenerate ring.
    [[nodiscard]] f32 SkinIrisLimbalRing(f32 radial, f32 band, f32 strength) noexcept;

    // @brief How much of the iris albedo survives the pupil at disc coordinate
    //        `radial`.
    //
    // The pupil is an APERTURE, not a pigment: it is dark because it is a hole
    // into an absorbing chamber, which is why `darkening` defaults to 1 (fully
    // black) rather than to 0. An author lifts it off 1 to fake the faint red
    // return a flash photograph gets, not to choose a pupil colour.
    //
    // `pupilRadial` is the pupil radius in the same disc coordinate — the CPU
    // has already divided the authored millimetres by the iris radius, so this
    // function compares two numbers in one unit.
    [[nodiscard]] f32 SkinIrisPupilMask(f32 radial, f32 pupilRadial, f32 darkening) noexcept;

    // @brief The shading normal on the iris, tilted by its concavity.
    //
    // A real iris is not a flat disc: it is a shallow dish, deepest at the
    // pupil margin where it rests against the lens, so its surface normal
    // turns outward as the disc coordinate falls toward the pupil. That
    // tilt is what makes an iris catch a key light across one side and go dark
    // across the other — the DEPTH RESPONSE issue #1244's second criterion asks
    // for, and the part of "not a flat painted eye" that the parallax alone does
    // not supply.
    //
    // APPLIED TO THE SHADING NORMAL AND NOT TO THE ALBEDO, which is why it costs
    // nothing extra on the deferred path: the G-Buffer already carries a normal,
    // so the tilt reaches the lighting pass through a channel that was always
    // there. A baked-in albedo gradient would have looked similar at one light
    // position and stayed put when the light moved.
    //
    // A concavity of 0 returns `normal` UNCHANGED — not renormalized, unchanged
    // — so a profile that did not author it is bit-identical.
    [[nodiscard]] glm::vec3 SkinIrisShadingNormal(const glm::vec3& normal, const glm::vec3& axis,
                                                  const glm::vec3& irisPlanePoint, f32 irisRadius,
                                                  f32 concavity) noexcept;

    // -------------------------------------------------------------------------
    // The one entry point, and what it reports
    // -------------------------------------------------------------------------

    // Why an eye pixel did not get the full model.
    //
    // NOT A RUNTIME COUNTER, and that is worth stating plainly because the
    // sibling features have one (SkinTransmissionFallbackReason is reported
    // through SkinProfileTable::ReportTransmissionFallback) and a reader will
    // look for the equivalent here. There is none, for a specific reason rather
    // than an oversight: EVERY ONE OF THESE IS UNREACHABLE FROM A SANITIZED
    // PROFILE.
    //
    //   * TotalInternalReflection needs eta > 1. SkinCorneaEta returns
    //     1 / clamp(ior, 1, 2.5), so eta is in (0.4, 1] and
    //     k = 1 - eta^2 (1 - cos^2) >= cos^2 >= 0 always.
    //   * RayDoesNotReachIris needs a refracted ray that does not travel
    //     inward, which a ray bent TOWARD the normal of an outward-facing
    //     surface cannot be.
    //   * NonUniformEyeScale is an authoring shape, not a value — see the
    //     convention note at the top; nothing in a profile can express it.
    //
    // So the gate that actually prevents these is
    // SkinProfileParameters::Sanitize, which every reader routes through, and
    // SkinOcularSurfaceTest asserts its three cross-field rules directly. What
    // this enum is FOR is the CPU model's own answer to "why is this pixel not
    // refracting?" — a named reason a test can assert against rather than a
    // plausible-looking albedo. AnInvertedIndexIsReportedRatherThanSilentlyPainted
    // drives it with a hand-corrupted lane, which is the only way to get there.
    //
    // The shader has no equivalent and does not need one: it early-outs on the
    // same conditions, and a condition that cannot occur needs no diagnostic.
    enum class SkinOcularFallbackReason : u8
    {
        // The pixel is outside the limbus. NOT AN ERROR and not counted as one
        // — it is sclera, and sclera is version-4 skin. Present in this enum so
        // that "why is this pixel not refracting" has an answer for every pixel.
        OutsideLimbus = 0,
        // The refraction total-internal-reflected. Physically impossible
        // entering a denser medium from air, so reaching this means the lane
        // carries an eta above 1 — an inverted IOR conversion.
        TotalInternalReflection = 1,
        // The refracted ray does not travel inward, so it cannot reach the iris
        // plane. Reachable with a corrupt axis or a back-facing normal.
        RayDoesNotReachIris = 2,
        // The eye entity's transform is not uniformly scaled, so the
        // interpolated normal is not the globe's radial direction and the whole
        // eye-local frame is wrong. See the convention note at the top.
        NonUniformEyeScale = 3,
        Count
    };

    // What one eye pixel resolved to. A struct rather than out-parameters
    // because the shader's transcription returns the same four things and the
    // parity test compares them field by field.
    struct SkinOcularResult
    {
        // The albedo after the iris response. Equal to the input where the
        // pixel is sclera or the profile is neutral.
        glm::vec3 Albedo{ 0.0f };
        // The shading normal after the iris concavity tilt. Equal to the input
        // wherever the tilt does not apply.
        glm::vec3 Normal{ 0.0f };
        // The iris disc coordinate this pixel resolved to: 0 at the pupil
        // centre, 1 at the iris edge, and NEGATIVE where the pixel is not on
        // the iris at all. A sentinel rather than a separate bool because it is
        // the quantity every evidence test measures and a test that has to read
        // two fields to know whether the third is meaningful gets it wrong once.
        f32 IrisRadial{ -1.0f };
        // Whether the full refracted model applied. False means `Reason` says
        // why, and it is the field the renderer counts.
        bool Refracted{ false };
        SkinOcularFallbackReason Reason{ SkinOcularFallbackReason::OutsideLimbus };
    };

    // @brief Resolve one eye pixel: refract through the cornea, find the iris
    //        point behind it, and apply the iris response.
    //
    // The ONE place the order of the three terms is written on this side, and
    // the order IS the layer sorting: refract first (it decides which iris point
    // you are looking at), then the limbal ring and the pupil (they are
    // properties of that point), then the concavity tilt (it is a property of
    // that point too). Nothing here touches the specular, the coat or the
    // transmission — those are version 4's and they run after, unchanged.
    //
    // `view` points FROM the surface TOWARD the eye, which is the convention
    // every other function in this engine's shading uses; the incident
    // direction handed to SkinOcularRefract is its negation, once, here.
    [[nodiscard]] SkinOcularResult ApplySkinOcularSurface(const glm::vec3& albedo, const glm::vec3& normal,
                                                          const glm::vec3& view, const glm::vec3& axis,
                                                          const glm::vec4& corneaLane,
                                                          const glm::vec4& irisLane,
                                                          const glm::vec4& responseLane,
                                                          const glm::vec4& tintLane) noexcept;

    // -------------------------------------------------------------------------
    // The lanes
    // -------------------------------------------------------------------------

    // @brief The corneal geometry, packed once for every path.
    //
    //   x = eta               (DERIVED from CorneaIor — see SkinCorneaEta)
    //   y = curvature ratio   (DERIVED: EyeRadiusMM / CorneaRadiusMM)
    //   z = iris plane depth  (DERIVED: IrisPlaneDepthMM / EyeRadiusMM)
    //   w = limbus cosine     (DERIVED: cos(asin(IrisRadiusMM / EyeRadiusMM)))
    //
    // EVERY COMPONENT IS DERIVED, which is the point and not a coincidence. The
    // authored fields are five clinical lengths and one index — numbers an
    // author can look up in Bennett & Rabbetts and check against a real eye —
    // and the four ratios a shader wants are computed here, once, where
    // SkinOcularSurfaceTest can look at them. An author who had to type 0.7485
    // for the reciprocal of 1.336 would be typing a number they cannot check.
    //
    // A ZERO-FILLED LANE IS INERT rather than neutral, and the difference
    // matters: eta 0 refracts to nothing and the limbus cosine 0 puts the limbus
    // at the equator. The master switch is `irisLane.w` (OcularStrength), which
    // is ALSO zero in an unclaimed slot, and that is what makes a stale lane
    // shade as version-4 skin rather than as a broken eye.
    [[nodiscard]] glm::vec4 SkinOcularCorneaLane(const SkinProfileParameters& parameters) noexcept;

    // @brief The iris disc, packed once for every path.
    //
    //   x = iris radius       (DERIVED: IrisRadiusMM / EyeRadiusMM)
    //   y = pupil radial      (DERIVED: PupilRadiusMM / IrisRadiusMM)
    //   z = limbal ring width (DERIVED: LimbalRingWidthMM / IrisRadiusMM)
    //   w = OcularStrength    — THE MASTER SWITCH, authored, default 0
    //
    // `w` IS THE ONE FIELD THAT TURNS THE FEATURE ON, and it defaults to 0 for
    // the reason CoatStrength and LobeMix do: a profile moved to transport
    // version 5 shades EXACTLY as it did at version 4 until an author asks for
    // an eye. The neutral-identity arm SkinOcularSurfaceEvidenceTest captures is
    // that statement, and it is bit-identical rather than merely similar.
    [[nodiscard]] glm::vec4 SkinOcularIrisLane(const SkinProfileParameters& parameters) noexcept;

    // @brief The iris response, packed once for every path.
    //
    //   x = LimbalRingStrength   y = PupilDarkening
    //   z = IrisConcavity        w = RefractionStrength
    //
    // `w` IS THE QUALITY LADDER, and it is the honest answer to issue #1244's
    // fourth acceptance criterion. 1 is the full refracted model; 0 is the
    // painted arm — the iris still has a pupil, a limbal ring and a concavity
    // tilt, it simply stops moving under the cornea. Intermediate values
    // interpolate the iris-plane landing point, so the ladder is continuous and
    // a scene can cross it without a pop.
    //
    // IT IS AUTHORED AND NEVER CHOSEN BY THE RENDERER. A path that cannot
    // afford the full model says so and is counted
    // (SkinOcularFallbackReason); it does not quietly move this number, because
    // an effect that silently degrades is one nobody ever notices is missing.
    // It defaults to 1: once an author has turned the eye on, the refraction is
    // the entire reason the eye exists.
    [[nodiscard]] glm::vec4 SkinOcularResponseLane(const SkinProfileParameters& parameters) noexcept;

    // @brief The iris tint, packed once for every path.
    //
    //   xyz = IrisColor (LINEAR Rec.709)
    //   w   = iris edge band (DERIVED: LimbalRingWidthMM / IrisRadiusMM, floored)
    //
    // A FOURTH LANE FOR THREE NUMBERS, which is worth defending because the
    // obvious alternative was to squeeze the colour into a spare-looking
    // component elsewhere. There are none: all twelve components of the three
    // lanes above carry a value, and std140 will not pack a vec3 into another
    // vec4's tail. The `w` is the edge band rather than an explicit pad, so the
    // lane costs nothing it does not use.
    //
    // THE EDGE BAND IS DERIVED FROM THE RING WIDTH because the iris/sclera
    // boundary and the limbal ring are THE SAME BOUNDARY — the limbus. Two
    // authored widths could have disagreed about where an eye stops being an
    // iris, and a reader would have had no way to tell which one was wrong.
    //
    // WHITE IS NEUTRAL: the colour is MULTIPLIED into the albedo, so an
    // unclaimed or stale slot — which is all-zero, not white — makes the iris
    // BLACK rather than untinted. That is safe only because `irisLane.w` gates
    // the whole block, and it is called out here for the same reason the cornea
    // lane's comment calls out its own inert zero.
    [[nodiscard]] glm::vec4 SkinOcularTintLane(const SkinProfileParameters& parameters) noexcept;

    // @brief Whether a profile's transport version evaluates the ocular terms
    //        at all. The ONE place that test is spelled on this side.
    //
    // An `==` and not a `>=`, matching every version branch that came before it
    // and for their reason: a version this code has no arm for must apply
    // NOTHING rather than guess that a later transport meant the same thing by
    // these fields.
    [[nodiscard]] constexpr bool SkinEvaluatesOcularSurface(SkinEvaluationModel model) noexcept
    {
        return model == SkinEvaluationModel::OcularSurface;
    }

} // namespace OloEngine
