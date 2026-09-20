#pragma once

// =============================================================================
// SkinProfile.h — the authored parameter set a skin material points at, and the
// separately versioned algorithm that evaluates it. Issue #1231.
//
// WHY THE PROFILE IS AN ASSET AND NOT FIELDS ON THE MATERIAL. A head, its ears,
// its lips and its eyelids are different materials with different albedo, normal
// and roughness maps, and the same skin. Duplicating six scattering scalars into
// each of them makes "make the skin less waxy" a six-file edit whose sixth file
// is always missed. One asset, many materials, is also what lets the deferred
// path carry a per-pixel PROFILE IDENTITY in a few bits instead of a parameter
// block per pixel.
//
// WHY THE EVALUATION MODEL LIVES HERE AND IS NOT `MaterialKind`. The issue is
// explicit that the KIND of a material must stay distinct from the VERSION of
// the algorithm that evaluates it, and the engine already learned this once:
// `PBRModel` (#975) exists because correcting a BRDF must not silently restate
// every scene. `MaterialKind::Skin` says "this is skin" forever;
// `SkinEvaluationModel` says "this profile was authored against transport
// version N", and #1241's diffusion lands as version 1 without touching a
// single material's kind. See
// docs/adr/0024-material-kind-is-not-the-closure-version.md.
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

namespace OloEngine
{

    // The versioned skin transport algorithm. Append, never renumber — the
    // number is on disk in every .oloskin file and in every scene that names
    // one.
    enum class SkinEvaluationModel : u8
    {
        // What #1231 ships: the diffuse irradiance and the surface specular are
        // evaluated and kept as SEPARATE terms all the way to the composite,
        // then summed. Numerically this equals the single combined term the
        // engine shaded before, which is the point — it is the seam #1241
        // needs, delivered without restating a pixel.
        DiffuseSpecularSplit = 0,

        // What #1241 ships: everything version 0 does, plus the DIFFUSE half is
        // diffused across the surface in screen space by a Burley normalized
        // diffusion kernel built from ScatterColor and ScatterRadiusMM, while
        // the specular half is recombined untouched. See
        // Renderer/SkinDiffusion.h for the maths and
        // docs/guides/skin-diffusion.md for its limits.
        //
        // A profile stays at version 0 until an author moves it. That is the
        // whole reason the version exists: turning diffusion on is an authoring
        // act per profile, not a renderer setting that restates every scene —
        // the renderer setting only decides whether the pass RUNS.
        ScreenSpaceDiffusion = 1,

        // What #1242 ships: everything version 1 does, plus a THIN-REGION
        // TRANSMISSION term — light entering the far face of a thin region and
        // leaving toward the viewer, attenuated through the authored thickness
        // by the same per-channel mean free paths the diffusion kernel uses.
        // See Renderer/SkinTransmission.h for the maths, the unit chain and the
        // energy argument, and docs/guides/skin-transmission.md for the
        // authoring path and its limits.
        //
        // A VERSION, NOT A FLAG, for the reason version 1 is one: a profile
        // stays where its author left it. Turning transmission on is an
        // authoring act per profile — a head authored against version 1 does not
        // start glowing through its ears because a renderer setting moved, and
        // the renderer setting only decides whether the term is EVALUATED.
        ThicknessTransmission = 2,

        // What #1243 ships: everything version 2 does, plus a LAYERED SURFACE
        // RESPONSE — a convex mixture of two GGX lobes in place of the single
        // one, a roughness filtered by the measured screen-space variance of the
        // shading normal, and a detail normal whose strength is driven by the
        // entity's expression. See Renderer/SkinLayeredSpecular.h for the maths
        // and the energy argument, docs/guides/skin-layered-specular.md for the
        // measured comparison the model was chosen on.
        //
        // IT TOUCHES THE SPECULAR HALF AND NOTHING ELSE, which is the whole
        // reason it can be added at all: #1231 split the two, so a second lobe
        // and a filtered roughness reach the specular without the diffusion
        // underneath (version 1) or the transmission beside it (version 2) being
        // able to see them. The issue's first acceptance criterion — "do not
        // erase the underlying diffusion" — is therefore a property of where the
        // code is, not of a value anyone tuned.
        //
        // A VERSION, NOT A FLAG, for the third time and the same reason: a
        // profile stays where its author left it. Its defaults are also NEUTRAL
        // (LobeMix 0, DetailStrength 0, ExpressionDetailGain 0), so moving a
        // profile to version 3 changes exactly one thing — the roughness becomes
        // variance-filtered — and the author opts into the rest one field at a
        // time.
        LayeredSpecular = 3,

        // What #1245 ships: everything version 3 does, plus the two terms an
        // ORAL SURFACE needs and skin does not — a WET COAT (a thin dielectric
        // film in front of the tissue, which takes energy from the response
        // underneath rather than adding to it) and a CAVITY WEIGHT (which gates
        // the thin-region transmission of version 2 by the material's own
        // occlusion, so the inside of a closed mouth stops glowing). See
        // Renderer/SkinOralSurface.h for both and for the energy argument, and
        // docs/guides/skin-oral-surfaces.md for the authoring path.
        //
        // NOT A SEPARATE MATERIAL KIND, and the distinction is the one
        // docs/adr/0024 draws. Lips, gums and a tongue ARE skin: they scatter
        // with the same Burley kernel, transmit through the same thickness and
        // shade with the same layered specular. What version 4 adds is a
        // FILM IN FRONT of all of that. Teeth are the interesting case and they
        // are handled by AUTHORING, not by a branch: an enamel profile is a
        // version-4 profile with a short achromatic scattering radius, a higher
        // coat IOR (1.63 against saliva's 1.33) and its own slot — a genuinely
        // different response, reached without a second code path that would
        // then have to be kept in step with this one.
        //
        // A VERSION, NOT A FLAG, for the fourth time and the same reason: a
        // profile stays where its author left it. Its defaults are NEUTRAL
        // (CoatStrength 0, CavityOcclusion 0), so moving a profile to version 4
        // changes NOTHING until a field is authored — the neutral-identity arm
        // SkinOralSurfaceEvidenceTest captures.
        OralSurface = 4,

        // What #1244 ships: everything version 4 does, plus the one term an EYE
        // needs and no other surface on a head does — a CORNEAL REFRACTION.
        // The iris sits ~2.5 mm behind a transparent dome of index 1.336, so a
        // viewer never sees it where it is; version 5 refracts the view ray at
        // that dome, finds the iris point behind it, and applies the iris's own
        // depth response (a limbal ring, a pupil and a dish tilt) at THAT point.
        // See Renderer/SkinOcularSurface.h for the maths and the measured
        // comparison the approximation was chosen on, and
        // docs/guides/eye-cornea-iris.md for the authoring path.
        //
        // AN EYE IS THREE SURFACES AND TWO OF THEM ARE ALREADY SKIN, which is
        // why this is a version and not a MaterialKind. The SCLERA scatters
        // (version 1's Burley kernel); the TEAR FILM is version 4's wet coat
        // with the index of tears instead of saliva — an F0 of 0.0208 against
        // 0.0201, a 3% difference, which is the measured argument for reusing
        // the coat rather than minting a second one. ONLY THE CORNEA IS NEW.
        //
        // IT RUNS IN THE MATERIAL STAGE, NOT THE LIGHTING STAGE, which is the
        // structural difference from every version before it: a refraction
        // changes WHICH POINT you are looking at, not how it reflects, so it
        // resolves once in the G-Buffer / forward fragment shader and the
        // deferred lighting pass never learns that eyes exist. Three
        // consequences — the paths agree by construction, no G-Buffer lane is
        // touched, and the cornea/iris/tear ordering cannot sort wrongly
        // because it is code order and not depth order.
        //
        // A VERSION, NOT A FLAG, for the fifth time and the same reason: a
        // profile stays where its author left it. Its master switch
        // (OcularStrength) defaults to 0, so moving a profile to version 5
        // changes NOTHING until a field is authored — the neutral-identity arm
        // SkinOcularSurfaceEvidenceTest captures.
        OcularSurface = 5,

        // What #1368 ships: everything version 5 does, but the diffusion of
        // version 1 is evaluated with an ISOTROPIC GATHER instead of two
        // separable passes, against a REFITTED transport profile.
        //
        // THE TWO CHANGES ARE ONE VERSION BECAUSE THEY ARE COUPLED, and that
        // coupling is the measured finding #1368 exists for. #1255 found two
        // errors in the version-1 pass — the Burley searchlight fit running
        // narrow above a diffuse albedo of 0.7, and a support radius sized to
        // the fit rather than to the transport — and #1361 measured what
        // repairing each was worth. Separately: nothing. Repairing only the
        // projection leaves the fit's error exposed and lands FURTHER from
        // transport than version 1 does; repairing only the fit leaves the
        // separable projection exposed and does the same. The two errors have
        // opposite signs and partially cancel in version 1, so either one alone
        // is a regression and the pair together is a 3-4x improvement.
        //
        // Shipping them as two versions would therefore have shipped a version
        // nobody should author against. They are one.
        //
        // WHAT CHANGES, precisely:
        //   * the kernel becomes a 25-tap golden-angle disc evaluated in ONE
        //     pass, rather than 17 taps along each of two axes. It is not a
        //     cost increase, it is a cost CUT: 25 fetches against 34. The count
        //     is kSkinGatherTapCount, which records why 25 and not more.
        //   * the profile becomes a generalised two-exponential whose mixture
        //     weight, rate ratio and scaling correction are
        //     kSkinGatherMixtureWeight, kSkinGatherRateRatio and
        //     SkinGatherScalingCorrection in Renderer/SkinDiffusion.h. Burley's
        //     (0.25, 3, 1) is the version-1 special case.
        //
        // A VERSION, NOT A RENDERER SETTING, for the sixth time and for a
        // sharper reason than the five before it: this one changes the SHAPE of
        // a profile that authors have already tuned against. A head authored at
        // version 1 keeps the exact pixels it was signed off with, and moving it
        // to 6 is a deliberate act with a visible result — which is the whole of
        // docs/adr/0024-material-kind-is-not-the-closure-version.md.
        //
        // IT IS NOT NEUTRAL ON ARRIVAL, unlike versions 3, 4 and 5. There is no
        // field to author: moving a profile here changes its look immediately,
        // by design, because the change IS the correction. That makes it the
        // first version whose evidence test cannot be a neutral-identity A/B.
        IsotropicGather = 6,

        Count
    };

    inline constexpr i32 kSkinEvaluationModelCount = static_cast<i32>(SkinEvaluationModel::Count);

    [[nodiscard]] constexpr std::string_view ToString(SkinEvaluationModel model)
    {
        switch (model)
        {
            case SkinEvaluationModel::DiffuseSpecularSplit:
                return "DiffuseSpecularSplit";
            case SkinEvaluationModel::ScreenSpaceDiffusion:
                return "ScreenSpaceDiffusion";
            case SkinEvaluationModel::ThicknessTransmission:
                return "ThicknessTransmission";
            case SkinEvaluationModel::LayeredSpecular:
                return "LayeredSpecular";
            case SkinEvaluationModel::OralSurface:
                return "OralSurface";
            case SkinEvaluationModel::OcularSurface:
                return "OcularSurface";
            case SkinEvaluationModel::IsotropicGather:
                return "IsotropicGather";
            case SkinEvaluationModel::Count:
                break;
        }
        return "DiffuseSpecularSplit";
    }

    [[nodiscard]] inline constexpr bool IsValidSkinEvaluationModel(i32 value) noexcept
    {
        return value >= 0 && value < kSkinEvaluationModelCount;
    }

    // -------------------------------------------------------------------------
    // Parameter bounds
    // -------------------------------------------------------------------------
    // Every bound is a REJECTION bound, not a taste bound: a value outside it
    // either makes the transport non-finite or is authoring corruption. They are
    // named here so the asset, the YAML reader, the save-game reader and the
    // editor inspector all clamp to the same numbers instead of three of them
    // agreeing and the fourth drifting.

    // Per-channel mean free path, in MILLIMETRES. Human dermis sits around
    // 0.5-2 mm in red and well under 0.2 mm in blue. Zero would divide by zero
    // in any diffusion profile; the upper bound is "a head is not 20 cm of
    // translucent jelly".
    inline constexpr f32 kMinSkinScatterRadiusMM = 1.0e-3f;
    inline constexpr f32 kMaxSkinScatterRadiusMM = 200.0f;

    // Scales the authored thickness (Material::GetThicknessFactor, which is in
    // metres per glTF KHR_materials_volume) into the millimetre space the radii
    // are expressed in. 1000 is the identity conversion m -> mm; an author
    // scales away from it to exaggerate or damp transmission.
    inline constexpr f32 kMinSkinThicknessScale = 0.0f;
    inline constexpr f32 kMaxSkinThicknessScale = 1.0e4f;

    // -------------------------------------------------------------------------
    // Transmission bounds (issue #1242)
    // -------------------------------------------------------------------------

    // Scales the whole transmitted term. 0 disables it for this profile even at
    // transport version 2; 1 is the physical answer. Above 1 would break the
    // energy bound premise 3 of Renderer/SkinTransmission.h's argument rests on,
    // so the ceiling is 1 and is NOT a taste bound — it is the bound
    // SkinTransmissionTest asserts against.
    inline constexpr f32 kMinSkinTransmissionStrength = 0.0f;
    inline constexpr f32 kMaxSkinTransmissionStrength = 1.0f;

    // The forward bias of the exit lobe, `g` in SkinTransmissionLobe. 0 is an
    // isotropic exit (the volume has fully forgotten which way the light came
    // in); 1 is fully view-dependent. Unitless.
    inline constexpr f32 kMinSkinTransmissionAnisotropy = 0.0f;
    inline constexpr f32 kMaxSkinTransmissionAnisotropy = 1.0f;

    // The sharpness of the forward part of the exit lobe, `P` in
    // SkinTransmissionLobe. Below 1 the pow() is a root and the lobe stops being
    // monotone in the useful direction; above 64 it is a specular spike, which a
    // diffuse exit is not.
    inline constexpr f32 kMinSkinTransmissionPower = 1.0f;
    inline constexpr f32 kMaxSkinTransmissionPower = 64.0f;

    // The largest authored thickness, MILLIMETRES, the transmission term will
    // evaluate. Past it the transmittance is below the quantisation of an
    // RGBA16F target for any plausible radiance and the exp() is wasted work;
    // more usefully, it bounds what a corrupt thickness map can ask for. A head
    // is not two metres thick.
    inline constexpr f32 kMaxSkinThicknessMM = 2.0e3f;

    // -------------------------------------------------------------------------
    // Layered specular bounds (issue #1243)
    // -------------------------------------------------------------------------

    // `w` — the fraction of the specular carried by the BROAD lobe. The ceiling
    // is 1 and is not a taste bound: the mixture is convex, and a weight outside
    // [0, 1] is what makes a convex combination stop being one. Past 1 the
    // narrow lobe's coefficient goes negative and the surface can return
    // negative radiance, which is the energy failure the whole arrangement
    // exists to make impossible. Pinned by SkinLayeredSpecularTest.
    inline constexpr f32 kMinSkinLobeMix = 0.0f;
    inline constexpr f32 kMaxSkinLobeMix = 1.0f;

    // `s` — how much rougher the broad lobe is than the narrow one, as a
    // multiplier on PERCEPTUAL ROUGHNESS and not on alpha. The two differ by a
    // square and the distinction is not pedantry: the reference experiment
    // fitted ALPHA ratios of 1.3 to 4.0, which are roughness ratios of 1.14 to
    // 2.0, and an author who reads the experiment and types its number into this
    // field gets a lobe four times wider than the one that was measured.
    //
    // Below 1 the "broad" lobe is the narrower of the two and the two names are
    // lies, which matters because every comment downstream reasons about which
    // is which. The ceiling is where the product has saturated at roughness 1
    // for any skin an author would type, so a larger number is the same lobe
    // with a more alarming label.
    inline constexpr f32 kMinSkinLobeRoughnessScale = 1.0f;
    inline constexpr f32 kMaxSkinLobeRoughnessScale = 4.0f;

    // `sigma^2` — the screen-space variance strength. 0 disables the filter
    // entirely, which is NOT a taste setting: it is the A/B control arm the
    // issue's acceptance criteria are demonstrated against, and it is what makes
    // "filtering off" an exact comparison rather than an approximate one.
    //
    // The ceiling is where the kernel clamp (kSkinVarianceKernelClamp) binds for
    // essentially every pixel, so a larger value is a constant maximum widening
    // wearing a variable's clothes. The measured useful range is 0.05 to 1.5
    // depending on how magnified the surface is — see
    // Renderer/SkinLayeredSpecular.h, which is also where the reason this is
    // authored rather than fixed is argued.
    inline constexpr f32 kMinSkinNormalVarianceStrength = 0.0f;
    inline constexpr f32 kMaxSkinNormalVarianceStrength = 4.0f;

    // The EXTRA gain on the normal map's high-frequency band — its pores and
    // fine furrows — over and above what the map already carries. Unitless.
    //
    // THE SIGNED RANGE IS THE POINT. 0 is neutral and returns the authored
    // normal untouched; positive deepens the pores; and -1 subtracts the band
    // entirely, returning the coarse normal. That -1 is not a curiosity, it is
    // the EXACT "detail off" arm the issue's fourth acceptance criterion is
    // demonstrated against — a control that removes precisely the signal the
    // feature adds, rather than a second authored value that looks similar.
    //
    // The ceiling is where the reconstructed detail has swung the normal into
    // the tangent plane for all but the flattest texels and the surface stops
    // having an orientation. Both ends are rejection bounds.
    inline constexpr f32 kMinSkinDetailStrength = -1.0f;
    inline constexpr f32 kMaxSkinDetailStrength = 4.0f;

    // -------------------------------------------------------------------------
    // Oral surface bounds (issue #1245)
    // -------------------------------------------------------------------------

    // How much of the wet film is present. 0 is a dry surface and disables the
    // coat entirely — the neutral default, and the A/B control arm the "wet
    // specular stays distinct from diffusion" criterion is demonstrated against.
    //
    // The ceiling is 1 and is NOT a taste bound: the coat's Fresnel and the
    // attenuation it leaves behind are a PARTITION of the incident energy
    // (Renderer/SkinOralSurface.h), and a strength above 1 makes the attenuation
    // negative — a surface that returns negative radiance under its own
    // highlight. Pinned by SkinOralSurfaceTest.
    inline constexpr f32 kMinSkinOralCoatStrength = 0.0f;
    inline constexpr f32 kMaxSkinOralCoatStrength = 1.0f;

    // The film's own perceptual roughness. A saliva film is smooth — the
    // measured useful range is 0.05 to 0.2 — but the floor here is a DOMAIN
    // bound rather than that taste: below it the GGX lobe is narrower than a
    // pixel at any sane magnification and the highlight becomes a sparkling
    // aliased dot that no amount of variance filtering recovers, because the
    // filter runs on the SURFACE normal and the coat is evaluated after it.
    //
    // The ceiling is 1 because above it alpha = roughness^2 leaves the NDF's
    // normalizable domain, which is the same ceiling every roughness in this
    // engine carries.
    inline constexpr f32 kMinSkinOralCoatRoughness = 0.01f;
    inline constexpr f32 kMaxSkinOralCoatRoughness = 1.0f;

    // The film's index of refraction, seen from air. Saliva is 1.33 (it is
    // essentially water); enamel is 1.63. The floor is 1 — the index of air
    // itself, which gives F0 = 0 and a coat that reflects nothing — and is
    // therefore a second, redundant way to say "dry"; below it the Fresnel
    // formula's ratio changes sign and squares back to a plausible-looking
    // number for a physically impossible medium.
    //
    // The ceiling is above diamond (2.42) and far above anything in a mouth, so
    // it bounds authoring corruption rather than expressing taste.
    inline constexpr f32 kMinSkinOralCoatIor = 1.0f;
    inline constexpr f32 kMaxSkinOralCoatIor = 2.5f;

    // How much of the material's own occlusion is spent on the thin-region
    // transmission term. 0 leaves that term exactly as issue #1242 shipped it —
    // the neutral default, and the arm the "no glowing interiors" A/B is
    // measured against; 1 hands it the AO in full.
    //
    // Both ends are the ends of a mix weight, so neither is a taste bound.
    inline constexpr f32 kMinSkinOralCavityOcclusion = 0.0f;
    inline constexpr f32 kMaxSkinOralCavityOcclusion = 1.0f;

    // -------------------------------------------------------------------------
    // The eye (issue #1244). Transport version 5.
    //
    // FIVE CLINICAL LENGTHS, IN MILLIMETRES, AND ONE INDEX. Every ratio the
    // shader actually wants — the refraction eta, the corneal curvature ratio,
    // the iris-plane depth in eye radii, the limbus cosine, the pupil and ring
    // radii in disc coordinates — is DERIVED from these on the CPU by the three
    // lane packers in Renderer/SkinOcularSurface.h.
    //
    // That direction is the whole authoring argument and it is the one
    // SkinOralCoatF0 made one version earlier: 12.0 mm, 7.8 mm and 5.85 mm are
    // numbers an author can find in Bennett & Rabbetts and check against a real
    // eye; 0.7485, 1.5385 and 0.8724 are numbers nobody can check against
    // anything. The defaults below are the schematic eye of
    // experiments/eye-cornea-reference/compare_refraction.py, which is itself
    // validated against the literature's entrance-pupil magnification before
    // any of these numbers were chosen.
    // -------------------------------------------------------------------------

    // The eyeball's radius. The reference length EVERY other ocular length is
    // divided by, which is why it is authored rather than assumed 1: an eye
    // authored in a scene whose unit is the centimetre still has a 12 mm globe,
    // and the ratios that reach the GPU are unaffected by either.
    //
    // The bounds span an infant eye (16 mm axial, ~8 mm radius) to a severely
    // myopic adult one, so they bound authoring corruption rather than taste.
    inline constexpr f32 kMinSkinEyeRadiusMM = 4.0f;
    inline constexpr f32 kMaxSkinEyeRadiusMM = 20.0f;

    // The anterior cornea's radius of curvature. 7.8 mm is the clinical mean.
    //
    // ITS ONLY JOB IS THE CURVATURE RATIO EyeRadiusMM / CorneaRadiusMM, which is
    // how much steeper the corneal dome is than the sphere the mesh actually
    // supplies. SETTING IT EQUAL TO EyeRadiusMM IS THE DOCUMENTED WAY TO SAY
    // "this mesh already has corneal geometry": the ratio becomes exactly 1 and
    // SkinCornealNormal returns the interpolated normal untouched.
    //
    // The floor is well below the steepest keratoconic cornea and the ceiling is
    // above any human globe; a ratio below 1 would be a cornea FLATTER than the
    // globe, which is not an eye, and kMinSkinCorneaCurvatureRatio refuses it.
    inline constexpr f32 kMinSkinCorneaRadiusMM = 3.0f;
    inline constexpr f32 kMaxSkinCorneaRadiusMM = 20.0f;

    // The derived curvature ratio's bounds, applied after the division so a
    // corrupt pair of radii cannot produce an inf or a bend that inverts the
    // normal. 1 is the neutral end, exactly (see SkinCornealNormal).
    inline constexpr f32 kMinSkinCorneaCurvatureRatio = 1.0f;
    inline constexpr f32 kMaxSkinCorneaCurvatureRatio = 4.0f;

    // The visible iris's radius — half the horizontal visible iris diameter,
    // 11.7 mm in the clinical mean, so 5.85. ALSO SETS THE LIMBUS: the boundary
    // between cornea and sclera is where the iris ends, so one authored length
    // decides both where the refraction applies and where the iris runs out.
    // Two fields could have disagreed; one cannot.
    inline constexpr f32 kMinSkinIrisRadiusMM = 1.0f;
    inline constexpr f32 kMaxSkinIrisRadiusMM = 12.0f;

    // The pupil's radius. 2.0 mm is a mesopic pupil; the bounds are the
    // physiological range, fully constricted to fully dilated.
    inline constexpr f32 kMinSkinPupilRadiusMM = 0.3f;
    inline constexpr f32 kMaxSkinPupilRadiusMM = 5.0f;

    // The derived pupil radius in DISC COORDINATES (pupil / iris). The ceiling
    // is below 1 because a pupil that filled the iris would leave no iris at
    // all, and the floor keeps the soft edge band from collapsing to nothing.
    inline constexpr f32 kMinSkinPupilRadialRatio = 0.02f;
    inline constexpr f32 kMaxSkinPupilRadialRatio = 0.95f;

    // How wide the pupil's soft edge is, as a fraction of the pupil radius.
    // FIXED RATHER THAN AUTHORED: it is an anti-aliasing measure, not a look —
    // the real pupil margin is a few tens of microns and would be a hard step at
    // any render resolution, which crawls under a temporal upscaler. Expressed
    // as a fraction so a constricted pupil does not become all edge.
    inline constexpr f32 kSkinPupilEdgeBandFraction = 0.06f;

    // The axial distance from the eye's APEX to the iris plane.
    //
    // 2.48 mm IS THE DEFAULT AND IT IS NOT THE CLINICAL ANTERIOR CHAMBER DEPTH,
    // which is 3.6 mm, and the difference is the one number in this block that
    // needs its own defence. The clinical figure is measured from the CORNEAL
    // apex, and a sphere-primitive eye has no corneal bulge: its apex sits
    // 1.12 mm behind where a real one would. 3.6 - 1.12 = 2.48 mm, DERIVED from
    // the schematic eye rather than fitted to a frame — question 4b of the
    // optical reference sweeps this value and finds a flat-bottomed curve
    // around it, so the derived number costs 0.7 points of accuracy against the
    // best fitted one and is worth it for being re-derivable.
    //
    // An eye mesh WITH corneal geometry authors the clinical 3.6 here, together
    // with CorneaRadiusMM == EyeRadiusMM above.
    inline constexpr f32 kMinSkinIrisPlaneDepthMM = 0.2f;
    inline constexpr f32 kMaxSkinIrisPlaneDepthMM = 8.0f;

    // The derived iris-plane depth in EYE RADII. The ceiling is below 1 because
    // a plane at or past the eye's centre is behind the equator, where no
    // refracted ray from the cornea reaches it.
    inline constexpr f32 kMinSkinIrisPlaneDepthRatio = 0.01f;
    inline constexpr f32 kMaxSkinIrisPlaneDepthRatio = 0.9f;

    // The cornea's index of refraction, seen from air.
    //
    // 1.336 — THE AQUEOUS HUMOUR — AND NOT 1.376, THE CORNEAL STROMA, and this
    // is the trap in the whole feature. 1.376 is the number an author looks up
    // for "cornea" and it is twenty times worse: the ray spends 0.55 mm in the
    // stroma and 3 mm in the aqueous behind it, so the index that decides where
    // it lands is the one it ENDS in. Measured, in the optical reference: 0.1%
    // of the iris radius against 2.0%. The error is also in the direction that
    // looks plausible — it under-refracts, so the eye reads slightly painted.
    //
    // The floor is 1, the index of air, which refracts nothing and is a second
    // way to say "no cornea"; below it the eta inverts and the ray bends the
    // wrong way. The ceiling is above diamond and far above anything in an eye.
    inline constexpr f32 kMinSkinCorneaIor = 1.0f;
    inline constexpr f32 kMaxSkinCorneaIor = 2.5f;

    // The width of the band over which the IRIS DISC fades into the sclera, as
    // a fraction of the iris radius. NOT AUTHORED: it is the same boundary the
    // limbal ring is drawn on, so it is derived from LimbalRingWidthMM with a
    // floor — a ring of width 0 would otherwise give the iris a hard edge, and
    // a hard edge at the limbus aliases into a crawling circle under any
    // temporal upscaler.
    //
    // The floor is small enough to still read as a limbus rather than as a
    // gradient, and large enough to span more than one pixel at any resolution
    // a head is rendered at.
    inline constexpr f32 kMinSkinIrisEdgeBand = 0.02f;

    // The limbal ring's width, measured inward from the iris edge. 0.7 mm is
    // the visible band on a mid-tone iris; the ceiling is the iris radius
    // itself, where the "ring" has become the whole iris.
    inline constexpr f32 kMinSkinLimbalRingWidthMM = 0.0f;
    inline constexpr f32 kMaxSkinLimbalRingWidthMM = 12.0f;

    // The derived ring width in DISC COORDINATES (width / iris radius).
    //
    // ITS FLOOR IS kMinSkinIrisEdgeBand AND NOT ZERO, which is a correctness
    // bound rather than a taste one. The ring is a smoothstep over
    // [1 - 2w, 1 - w] and the iris edge is a smoothstep over [1 - w, 1]; at
    // w == 0 both collapse to smoothstep(1, 1, x), which is a DIVISION BY THE
    // SPAN and is undefined in GLSL. The CPU's SmoothStep guards it explicitly
    // and the shader's builtin does not, so a zero here is a NaN albedo on the
    // GPU and a clean 0-or-1 on the CPU — a parity divergence that only appears
    // for an author who set the ring's width to 0 while leaving its strength up.
    //
    // A zero-WIDTH ring is meaningless anyway: "no ring" is what
    // LimbalRingStrength 0 says, exactly and by a cheaper path.
    inline constexpr f32 kMinSkinLimbalRingWidthRatio = kMinSkinIrisEdgeBand;
    inline constexpr f32 kMaxSkinLimbalRingWidthRatio = 0.45f;

    // How much albedo the limbal ring takes at its darkest. 1 is fully black,
    // which is a legal ring rather than a corrupt one — a very dark iris under
    // a shallow chamber genuinely reads that way — so the ceiling is not a
    // taste bound. 0 is the neutral default and returns exactly 1.
    inline constexpr f32 kMinSkinLimbalRingStrength = 0.0f;
    inline constexpr f32 kMaxSkinLimbalRingStrength = 1.0f;

    // How far the iris dish tilts its shading normal, as a tangent added to the
    // corneal normal before renormalizing.
    //
    // The ceiling is 0.5 and it IS a domain bound rather than taste: the tilt is
    // added to a unit normal, so at 0.5 the shading normal has turned about 26
    // degrees off the surface it belongs to, which is already more than the
    // corneal normal's own variation across the whole iris. Beyond it the
    // "perturbation" would be the dominant term and the specular highlight —
    // which must stay where the cornea put it — would start to slide.
    inline constexpr f32 kMinSkinIrisConcavity = 0.0f;
    inline constexpr f32 kMaxSkinIrisConcavity = 0.5f;

    // @brief The ocular half of a skin profile's authored parameters (#1244).
    //
    // A nested aggregate for the reason SkinOralParameters is one: every
    // function in Renderer/SkinOcularSurface.h that needs only the geometry can
    // take THIS, and the derivation of the four corneal ratios reads as a
    // property of five lengths instead of of twenty-nine fields. Serialized as
    // part of the profile and sanitized by the profile's own Sanitize() —
    // SkinProfile.h promises ONE validation gate and this struct does not open
    // a fourth.
    struct SkinOcularParameters
    {
        // THE MASTER SWITCH. How much of the ocular model applies. Meaningful
        // only at transport version 5 (SkinEvaluationModel::OcularSurface); the
        // version branch, not this field, is what stops an older profile
        // acquiring an eye.
        //
        // DEFAULT 0 — no eye, and bit-identical to the version-4 frame. That is
        // the fourth time this file has made that choice and it is the same
        // choice: a profile moved to version 5 changes NOTHING until an author
        // asks for an eye. The neutral-identity arm
        // SkinOcularSurfaceEvidenceTest captures is that sentence.
        f32 OcularStrength = 0.0f;

        // How much of the CORNEAL REFRACTION applies, once the eye is on. The
        // quality ladder for issue #1244's fourth acceptance criterion: 1 is
        // the full refracted model, 0 is a painted iris that still has a pupil,
        // a limbal ring and a dish tilt. Continuous in between — the iris-plane
        // landing point is interpolated, so crossing the ladder does not pop.
        //
        // DEFAULT 1, the opposite polarity to OcularStrength above, and the
        // difference is deliberate: an author who has turned an eye on wants the
        // refraction, which is the entire reason the eye is a feature rather
        // than a texture. Turning it DOWN is the deliberate act.
        //
        // AUTHORED, NEVER CHOSEN BY THE RENDERER. See the lane comment in
        // Renderer/SkinOcularSurface.h: an effect that silently degrades is one
        // nobody notices is missing.
        f32 RefractionStrength = 1.0f;

        // The five clinical lengths, MILLIMETRES, and the one index. Defaults
        // are the schematic eye — real values rather than neutral ones, for the
        // reason SkinOralParameters::CoatIor defaults to saliva: these are not
        // effects an author opts into but MATERIAL CONSTANTS, and a profile that
        // had an eye and no eye radius would have to invent one.
        f32 EyeRadiusMM = 12.0f;
        f32 CorneaRadiusMM = 7.8f;
        f32 IrisRadiusMM = 5.85f;
        f32 PupilRadiusMM = 2.0f;
        f32 IrisPlaneDepthMM = 2.48f;
        f32 CorneaIor = 1.336f;

        // The limbal ring. WIDTH is a shape and defaults to a real 0.7 mm;
        // STRENGTH is the effect and defaults to 0, so the ring costs nothing
        // and shows nothing until it is authored.
        f32 LimbalRingWidthMM = 0.7f;
        f32 LimbalRingStrength = 0.0f;

        // How dark the pupil is. 1 — fully black — is the DEFAULT rather than
        // the neutral 0, because a pupil is an aperture into an absorbing
        // chamber and a profile with an eye and a bright pupil is not a look,
        // it is a missing field. It is still gated by OcularStrength, so a
        // neutral profile is unaffected.
        f32 PupilDarkening = 1.0f;

        // The iris dish's tilt. The effect, so 0.
        f32 IrisConcavity = 0.0f;

        // The iris's own colour, LINEAR Rec.709, [0,1]. MULTIPLIED into the
        // material's albedo inside the iris disc and faded out at the limbus,
        // so the sclera keeps whatever the material authored.
        //
        // WHITE IS THE DEFAULT AND IT IS THE NEUTRAL ONE: multiplying by 1
        // changes nothing, so a profile that has not authored an iris colour
        // shades exactly as it did. That is the same property every other
        // default in this struct has, reached here by the colour's identity
        // rather than by a zero.
        //
        // A MULTIPLY AND NOT A REPLACE, which is what makes white neutral and
        // is also the physically sensible direction: an iris is PIGMENTED
        // TISSUE seen through the same chamber the sclera is, so it is darker
        // and more saturated than the surface around it, never brighter. An
        // author who wants a pale blue iris on a warm sclera writes the
        // TRANSMITTANCE of that pigment here, which is what they would measure.
        //
        // WITHOUT THIS THE EYE IS A SPHERE WITH A DOT. The limbal ring and the
        // pupil alone leave the iris exactly the colour of the sclera, which
        // reads worse than the "flat painted eye" issue #1244's second
        // acceptance criterion forbids — and that is how the first evidence
        // capture of this feature came out.
        glm::vec3 IrisColor{ 1.0f, 1.0f, 1.0f };

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected.
        // Called by SkinProfileParameters::Sanitize, never on its own.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinOcularParameters& other) const noexcept;
    };

    // @brief The oral-surface half of a skin profile's authored parameters
    //        (issue #1245).
    //
    // A nested aggregate for the reason SkinTransmissionParameters and
    // SkinSpecularParameters are ones: every function in
    // Renderer/SkinOralSurface.h that needs only the coat can take THIS, and the
    // energy partition reads as a property of two numbers instead of of
    // seventeen. Serialized as part of the profile and sanitized by the
    // profile's own Sanitize() — SkinProfile.h promises ONE validation gate and
    // this struct does not open a third.
    struct SkinOralParameters
    {
        // How much wet film is present. Meaningful only at transport version 4
        // (SkinEvaluationModel::OralSurface); the version branch, not this
        // field, is what stops an older profile acquiring a coat.
        //
        // DEFAULT 0 — dry, and bit-identical to the version-3 frame. The
        // measured fit wants ~0.35 for lips and a parted mouth's inner
        // surfaces, ~0.6 for a tongue and ~0.25 for enamel, so there is no one
        // right number and the neutral one is the honest default.
        f32 CoatStrength = 0.0f;

        // The film's perceptual roughness. 0.1 is a wet-but-not-mirror film and
        // is only consulted when CoatStrength is non-zero, so it is a starting
        // point for authoring rather than a value that does anything on its own.
        f32 CoatRoughness = 0.1f;

        // The film's index of refraction. Defaults to SALIVA rather than to the
        // neutral 1.0, because unlike CoatStrength this is not an effect an
        // author opts into but a MATERIAL CONSTANT: a profile that had a coat
        // and no index would have to invent one. Authoring an enamel profile
        // means moving this to 1.63, which is the single field that makes teeth
        // shade differently from the mucosa beside them.
        f32 CoatIor = 1.33f;

        // How much of the material's occlusion gates the transmitted term.
        //
        // DEFAULT 0, the opposite polarity to NormalVarianceStrength one struct
        // up, and the difference is deliberate: the variance filter is a
        // CORRECTION every version-3 profile wants, while this one changes what
        // a head that already looked right does with its ears. A head moved from
        // version 3 to version 4 must not start occluding its own backlit rim
        // because the author wanted a wet lip.
        f32 CavityOcclusion = 0.0f;

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected.
        // Called by SkinProfileParameters::Sanitize, never on its own.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinOralParameters& other) const noexcept;
    };

    // @brief The layered-specular half of a skin profile's authored parameters
    //        (issue #1243).
    //
    // A nested aggregate for the reason SkinTransmissionParameters is one: every
    // function in Renderer/SkinLayeredSpecular.h that needs only the lobe shape
    // can take THIS, and the convexity argument reads as a property of two
    // numbers instead of of thirteen. Serialized as part of the profile and
    // sanitized by the profile's own Sanitize() — SkinProfile.h promises ONE
    // validation gate and this struct does not open a second.
    struct SkinSpecularParameters
    {
        // `w`. Meaningful only at transport version 3
        // (SkinEvaluationModel::LayeredSpecular); the version branch, not this
        // field, is what stops an older profile acquiring a second lobe.
        //
        // DEFAULT 0 — the single-lobe answer. NOT, on its own, the version-2
        // frame: it removes the broad lobe and nothing else, and the narrow lobe
        // still shades at the FILTERED roughness. Reproducing version 2 needs
        // all four of this, NormalVarianceStrength, DetailStrength and
        // ExpressionDetailGain at zero — which is the arm
        // SkinLayeredSpecularEvidenceTest's neutral-identity A/B authors.
        //
        // The measured fit wants ~0.05 at close range and ~0.6 once a pixel
        // straddles regions of different roughness, so there is no one right
        // number and the neutral one is the honest default.
        f32 LobeMix = 0.0f;

        // `s`. 3.0 is the middle of the measured fit's range and is only
        // consulted when LobeMix is non-zero, so it is a starting point for
        // authoring rather than a value that does anything on its own.
        f32 LobeRoughnessScale = 3.0f;

        // `sigma^2`. Defaults to the published 0.5 rather than to 0: unlike the
        // two above, this one is not an effect an author opts into but a
        // CORRECTION, and a version-3 profile that filtered nothing would ship
        // the sparkle the version exists to remove. Turning it off is the
        // deliberate act, which is the opposite polarity to LobeMix and is why
        // the two defaults differ.
        f32 NormalVarianceStrength = 0.5f;

        // Extra pore-band gain at a NEUTRAL expression. 0 leaves the authored
        // normal map exactly as it is.
        f32 DetailStrength = 0.0f;

        // Extra pore-band gain added at a FULLY EXPRESSED face, scaled by
        // SkinExpressionDetailWeight's [0, 1] and summed with DetailStrength.
        // The sum is clamped into the bound by SkinDetailStrength, so the two
        // fields cannot combine into a value neither of them could hold.
        f32 ExpressionDetailGain = 0.0f;

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected.
        // Called by SkinProfileParameters::Sanitize, never on its own.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinSpecularParameters& other) const noexcept;
    };

    // @brief The transmission half of a skin profile's authored parameters
    //        (issue #1242).
    //
    // A nested aggregate rather than three more fields on SkinProfileParameters
    // so that every function in Renderer/SkinTransmission.h that needs only the
    // lobe shape can take THIS, and the energy bound reads as a property of
    // three numbers instead of of eight. It is still serialized as part of the
    // profile and sanitized by the profile's own Sanitize() — SkinProfile.h
    // promises ONE validation gate and this struct does not open a second.
    struct SkinTransmissionParameters
    {
        // Scales the whole term. Meaningful only at transport version 2
        // (SkinEvaluationModel::ThicknessTransmission); the version branch, not
        // this field, is what stops an older profile acquiring transmission.
        f32 Strength = 1.0f;

        // `g` — how much of the exit lobe is view-dependent.
        f32 Anisotropy = 0.7f;

        // `P` — the sharpness of the view-dependent part.
        f32 Power = 4.0f;

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected.
        // Called by SkinProfileParameters::Sanitize, never on its own.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinTransmissionParameters& other) const noexcept;
    };

    // @brief The authored values themselves — a plain aggregate, deliberately
    //        separate from the Asset that owns them.
    //
    // Separate because the renderer needs to COPY a profile's parameters (into a
    // per-frame slot table, into a test fixture, into a save-game record) and
    // `Asset` derives from `RefCounted`, whose atomic refcount makes the type
    // non-copyable. Splitting the data out is also what lets the finite-
    // validation live in one function that the asset, both deserializers and the
    // editor all call.
    //
    // UNITS AND COLOUR SPACE ARE PART OF THE CONTRACT, not a comment: every
    // colour below is LINEAR (Rec.709 primaries, no transfer function applied),
    // because that is the space the whole lighting stack works in, and every
    // length is MILLIMETRES, because a scattering radius authored in metres is a
    // number no human recognises as wrong. The editor inspector labels them the
    // same way and SkinProfileSerializer writes the same names to YAML.
    struct SkinProfileParameters
    {
        SkinEvaluationModel EvaluationModel = SkinEvaluationModel::DiffuseSpecularSplit;

        // Transport albedo per channel. LINEAR Rec.709, unitless, [0,1]. The
        // fraction of light entering the surface that leaves it again rather
        // than being absorbed — red survives, blue does not, which is what makes
        // skin read as skin. The defaults are a mid-tone dermis: a starting
        // point for authoring, not a measurement.
        glm::vec3 ScatterColor{ 0.85f, 0.55f, 0.45f };

        // Per-channel mean free path, MILLIMETRES.
        glm::vec3 ScatterRadiusMM{ 1.55f, 0.80f, 0.55f };

        // Multiplier taking the material's authored thickness (metres) into the
        // millimetre space of the radii above. Unitless; 1000 is the identity.
        f32 ThicknessScale = 1000.0f;

        // Multiplies the SURFACE specular term of every material using this
        // profile. LINEAR Rec.709, unitless, [0,1]. This is the knob that keeps
        // highlights crisp while the diffuse half is blurred, and it is only
        // meaningful BECAUSE the two outputs are separate — a combined term
        // cannot be tinted without tinting the scattering with it.
        glm::vec3 SpecularTint{ 1.0f, 1.0f, 1.0f };

        // The thin-region transmission lobe (issue #1242). Read only at
        // transport version 2; see SkinTransmissionParameters above and
        // Renderer/SkinTransmission.h.
        SkinTransmissionParameters Transmission{};

        // The layered surface response (issue #1243). Read only at transport
        // version 3; see SkinSpecularParameters above and
        // Renderer/SkinLayeredSpecular.h.
        SkinSpecularParameters Specular{};

        // The wet coat and the cavity weight (issue #1245). Read only at
        // transport version 4; see SkinOralParameters above and
        // Renderer/SkinOralSurface.h.
        SkinOralParameters Oral{};

        // The cornea, the iris behind it and the tear line (issue #1244). Read
        // only at transport version 5; see SkinOcularParameters above and
        // Renderer/SkinOcularSurface.h.
        SkinOcularParameters Ocular{};

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected, so
        // callers can log the one case that matters.
        //
        // This is the ONLY validation gate: the YAML reader, the asset-pack
        // reader, the save-game reader and the editor all route through it, so a
        // value cannot reach the GPU by a path that forgot to check it.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinProfileParameters& other) const noexcept;
    };

    // @brief Authored skin scattering parameters, shared by every material that
    //        names this asset.
    class SkinProfile : public Asset
    {
      public:
        SkinProfile() = default;
        // Sanitizes, exactly as SetParameters does. Assigning m_Parameters
        // directly here would have been the one way into this class that skips
        // the validation gate — and SkinProfileTable::Resolve copies whatever it
        // finds without re-checking, so a non-finite value constructed this way
        // would reach the material UBO.
        explicit SkinProfile(const SkinProfileParameters& parameters)
        {
            (void)SetParameters(parameters);
        }
        ~SkinProfile() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::SkinProfile;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // Authoring label. Not an identity — the AssetHandle is.
        [[nodiscard]] const std::string& GetName() const noexcept
        {
            return m_Name;
        }
        void SetName(std::string name)
        {
            m_Name = std::move(name);
        }

        [[nodiscard]] const SkinProfileParameters& GetParameters() const noexcept
        {
            return m_Parameters;
        }

        // Assigns and sanitizes in one step. Returns false when a value had to
        // be corrected, so the caller can report it rather than shipping a
        // silently altered profile.
        bool SetParameters(const SkinProfileParameters& parameters);

        // Mutable access for the editor inspector, which edits fields in place
        // through ImGui and calls Sanitize() itself once the widget is done.
        [[nodiscard]] SkinProfileParameters& GetParametersForEdit() noexcept
        {
            return m_Parameters;
        }

        // The parameters a material gets when its handle names nothing loadable.
        // A real, finite, obviously-neutral set rather than a null pointer the
        // shader would have to test for: the caller LOGS and COUNTS the
        // substitution (SkinProfileFallbackReason) so a missing asset is loud,
        // and the frame still renders a head instead of a black hole.
        [[nodiscard]] static SkinProfileParameters DefaultParameters()
        {
            return SkinProfileParameters{};
        }

      private:
        std::string m_Name = "Skin";
        SkinProfileParameters m_Parameters{};
    };

    // -------------------------------------------------------------------------
    // Resolution — and why it is not a silent default
    // -------------------------------------------------------------------------

    // Why a material asked for a profile and did not get the one it named.
    // Mirrors the ShadowTechnique pattern (#1056): a path that cannot do its job
    // says so loudly and countably, because the alternative — quietly shading a
    // head with a neutral profile — looks exactly like a correct frame.
    enum class SkinProfileFallbackReason : u8
    {
        None = 0,       ///< The named profile resolved. No fallback.
        NoHandle,       ///< MaterialKind::Skin with no profile assigned at all.
        AssetMissing,   ///< The handle names an asset the manager cannot load.
        WrongAssetType, ///< The handle resolves to something that is not a SkinProfile.
        SlotBudgetFull, ///< More distinct profiles in one frame than the G-Buffer lane can name.
        MaterialNotPBR, ///< MaterialKind::Skin on a legacy (Phong) material, which has no skin transport.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(SkinProfileFallbackReason reason)
    {
        switch (reason)
        {
            case SkinProfileFallbackReason::None:
                return "None";
            case SkinProfileFallbackReason::NoHandle:
                return "NoHandle";
            case SkinProfileFallbackReason::AssetMissing:
                return "AssetMissing";
            case SkinProfileFallbackReason::WrongAssetType:
                return "WrongAssetType";
            case SkinProfileFallbackReason::SlotBudgetFull:
                return "SlotBudgetFull";
            case SkinProfileFallbackReason::MaterialNotPBR:
                return "MaterialNotPBR";
            case SkinProfileFallbackReason::Count:
                break;
        }
        return "None";
    }

    // -------------------------------------------------------------------------
    // Why a material asked for transmission and did not get it
    // -------------------------------------------------------------------------

    // The house rule on silent fallbacks, applied to the two ways a skin
    // material can fail to transmit. Counted and logged like
    // SkinProfileFallbackReason, and for the same reason: a head that quietly
    // stopped transmitting looks exactly like a head that never should have.
    enum class SkinTransmissionFallbackReason : u8
    {
        None = 0,

        // MaterialKind::Skin with a version-2 profile and NO authored thickness
        // — no thicknessFactor and no thickness map. The conservative answer is
        // no transmission, because the other reading of a zero thickness is
        // "infinitely thin", which renders the uniformly emissive head the
        // issue's second criterion forbids. Documented in
        // docs/guides/skin-transmission.md.
        NoThickness,

        // A thickness map that could not be loaded. Distinct from NoThickness:
        // the author DID author one, so the scalar factor is used alone and the
        // per-pixel variation — the ear — is the thing that went missing.
        ThicknessMapMissing,

        // MaterialKind::Skin with KHR_materials_transmission's TransmissionFactor
        // also raised. Two transmission closures on one surface IS the
        // double-count the fourth criterion forbids, arriving by the authoring
        // path rather than by the maths: skin's term wins, the refractive one is
        // dropped for this material, and the author is told.
        RefractiveTransmissionConflict,

        // The DEFERRED path cannot carry this pixel's thickness because the
        // surface is LIGHTMAPPED and RT5 is holding its baked irradiance (see
        // oloSkinPackGBufferThickness in include/SkinTransmission.glsl).
        //
        // On that path the term does NOT FIRE — the reader returns 0 mm rather
        // than falling back to the material's scalar, because the scalar never
        // reaches the deferred lighting pass: that channel is the thickness's
        // only route there. Forward and Forward+ sample the material directly
        // and are unaffected, which is why this is worth telling apart from
        // ThicknessMapMissing: the fix is to unlightmap the head or use a
        // forward path, not to repair an asset.
        //
        // Raised at SUBMISSION (Renderer3DMeshSubmission.cpp), the only site
        // that can see both the material and the draw's lightmap region, and
        // raised on every path — the condition is a property of the ASSET, so a
        // scene that later switches to Deferred would lose the effect silently
        // unless it had been counted beforehand.
        DeferredThicknessLaneUnavailable,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(SkinTransmissionFallbackReason reason)
    {
        switch (reason)
        {
            case SkinTransmissionFallbackReason::None:
                return "None";
            case SkinTransmissionFallbackReason::NoThickness:
                return "NoThickness";
            case SkinTransmissionFallbackReason::ThicknessMapMissing:
                return "ThicknessMapMissing";
            case SkinTransmissionFallbackReason::RefractiveTransmissionConflict:
                return "RefractiveTransmissionConflict";
            case SkinTransmissionFallbackReason::DeferredThicknessLaneUnavailable:
                return "DeferredThicknessLaneUnavailable";
            case SkinTransmissionFallbackReason::Count:
                break;
        }
        return "None";
    }

    inline constexpr i32 kSkinTransmissionFallbackReasonCount =
        static_cast<i32>(SkinTransmissionFallbackReason::Count);

    // -------------------------------------------------------------------------
    // The per-frame slot table
    // -------------------------------------------------------------------------

    // A skin profile reaches the DEFERRED lighting pass as a small integer in
    // the G-Buffer RT2 flags lane, not as an AssetHandle — 64 bits per pixel is
    // not something a G-Buffer can carry, and the lane has three bits to spare
    // (see oloEncodeGBufferPbrFlags in include/PBRCommon.glsl). So the renderer
    // assigns each DISTINCT profile used in a frame a slot in
    // [0, kMaxSkinProfileSlots).
    //
    // kSkinProfileSlotNone is the "this pixel names no profile" code and is what
    // every non-skin surface writes. It is the all-ones pattern of the field on
    // purpose: a reader must never confuse "nothing written here" with
    // "profile 0".
    inline constexpr u32 kSkinProfileSlotBits = 3;
    inline constexpr u32 kSkinProfileSlotNone = (1u << kSkinProfileSlotBits) - 1u; // 7
    inline constexpr u32 kMaxSkinProfileSlots = kSkinProfileSlotNone;              // slots 0..6

} // namespace OloEngine
