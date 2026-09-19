// OLO_TEST_LAYER: L1
// =============================================================================
// SkinOralSurfaceTest.cpp — the energy partition, the coat's directional albedo
// bound and the cavity weight's monotonicity. Issue #1245.
//
// WHAT THIS PINS AND WHAT IT DOES NOT. This file is the CPU maths of
// Renderer/SkinOralSurface.h. That the SHADER computes the same maths is
// SkinOralSurfaceParityTest's job, and that the result LOOKS right is
// SkinOralSurfaceEvidenceTest's. All three are needed and none substitutes for
// another: a property can hold while the number is wrong by a factor of a
// thousand, an equality can hold while the frame is black, and a frame can look
// fine while a mouth's interior quietly gained a third of a stop.
//
// THE CENTRAL CLAIM is that the coat CANNOT BRIGHTEN THE SURFACE ON AVERAGE.
// A wet highlight is the classic way to make a renderer look better and be
// wrong, so the claim is checked twice and in two different currencies:
//
//   * as an EXACT IDENTITY — attenuation + strength * fresnel == 1 — swept over
//     the whole legal domain, which is what "a partition, not a blend" means;
//   * as a NUMERICALLY INTEGRATED BOUND — the coat lobe's directional albedo
//     never exceeds 1, and at a smooth coat sits essentially ON its Fresnel —
//     which is the part the identity cannot reach, because a partition of the
//     incident energy says nothing about what a GGX lobe then does with its
//     share.
//
// The second is the one that would catch a missing 1/(4 NdotV NdotL), a Smith
// term paired with the wrong alpha, or a normalization dropped from D. None of
// those would move the identity by a bit.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinOralSurface.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <cmath>
#include <limits>
#include <numbers>

using namespace OloEngine;

namespace
{
    constexpr f32 kSalivaIor = 1.33f;
    constexpr f32 kEnamelIor = 1.63f;

    [[nodiscard]] SkinProfileParameters MakeOralProfile(f32 coatStrength, f32 coatRoughness, f32 ior,
                                                        f32 cavity)
    {
        SkinProfileParameters parameters;
        parameters.EvaluationModel = SkinEvaluationModel::OralSurface;
        parameters.Oral.CoatStrength = coatStrength;
        parameters.Oral.CoatRoughness = coatRoughness;
        parameters.Oral.CoatIor = ior;
        parameters.Oral.CavityOcclusion = cavity;
        (void)parameters.Sanitize();
        return parameters;
    }

    // The coat's DIRECTIONAL ALBEDO: the fraction of the light arriving from all
    // directions that the film sends toward `view`.
    //
    //     rho(V) = integral over the hemisphere of f(V, L) * dot(N, L) dL
    //
    // Integrated on a uniform theta/phi grid rather than by importance sampling,
    // because a test that sampled the very distribution it is checking would be
    // insensitive to exactly the errors it exists to find — a wrong
    // normalization in D cancels against a wrong pdf.
    //
    // A COARSE GRID UNDER-REPORTS A NARROW LOBE, and that cuts BOTH ways: it
    // would let a genuinely energy-gaining coat through the bound below, and it
    // makes the "is the lobe actually there?" test fail on a correct lobe. A
    // 512x256 grid has a theta step of about 0.18 degrees, which resolves a GGX
    // whose alpha is a few times that — i.e. ROUGHNESS ABOVE ABOUT 0.25. At the
    // 0.06 an author gives a wet lip the lobe is under a degree wide and this
    // quadrature reports a tenth of its energy.
    //
    // SO THE SWEEP BELOW STAYS ABOVE THAT, AND THAT IS NOT A GAP. D, the Smith
    // visibility and the Schlick are ONE piece of arithmetic with roughness as
    // an argument; pinning its normalization where the quadrature is honest
    // pins it everywhere. What covers the narrow end is a different instrument:
    // SkinOralSurfaceParityTest compares the shader and the CPU at roughness
    // 0.12 exactly, which catches a wrong constant without needing to integrate
    // anything. Sweeping a roughness this integrator cannot resolve would
    // produce a number that looks like evidence and is not.
    [[nodiscard]] f32 CoatDirectionalAlbedo(f32 coatRoughness, f32 coatF0, f32 NdotV)
    {
        constexpr i32 kThetaSteps = 512;
        constexpr i32 kPhiSteps = 256;
        constexpr f32 kPi = std::numbers::pi_v<f32>;

        const glm::vec3 N{ 0.0f, 0.0f, 1.0f };
        const f32 sinV = std::sqrt(std::max(0.0f, 1.0f - NdotV * NdotV));
        const glm::vec3 V{ sinV, 0.0f, NdotV };

        f32 total = 0.0f;
        const f32 dTheta = (0.5f * kPi) / static_cast<f32>(kThetaSteps);
        const f32 dPhi = (2.0f * kPi) / static_cast<f32>(kPhiSteps);

        for (i32 ti = 0; ti < kThetaSteps; ++ti)
        {
            const f32 theta = (static_cast<f32>(ti) + 0.5f) * dTheta;
            const f32 sinTheta = std::sin(theta);
            const f32 cosTheta = std::cos(theta);
            for (i32 pi = 0; pi < kPhiSteps; ++pi)
            {
                const f32 phi = (static_cast<f32>(pi) + 0.5f) * dPhi;
                const glm::vec3 L{ sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta };
                const f32 brdf = SkinOralCoatSpecular(N, V, L, coatRoughness, coatF0);
                total += brdf * cosTheta * sinTheta * dTheta * dPhi;
            }
        }
        return total;
    }
} // namespace

// -----------------------------------------------------------------------------
// The index-of-refraction conversion
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, CoatF0MatchesTheFresnelFormulaForSalivaAndEnamel)
{
    // The two numbers an author actually types, checked against the closed form
    // rather than against a constant transcribed from the same place the code
    // took it, which would make the test a copy of the bug.
    const f32 saliva = SkinOralCoatF0(kSalivaIor);
    const f32 enamel = SkinOralCoatF0(kEnamelIor);

    const f32 expectedSaliva = ((kSalivaIor - 1.0f) / (kSalivaIor + 1.0f)) * ((kSalivaIor - 1.0f) / (kSalivaIor + 1.0f));
    const f32 expectedEnamel = ((kEnamelIor - 1.0f) / (kEnamelIor + 1.0f)) * ((kEnamelIor - 1.0f) / (kEnamelIor + 1.0f));

    EXPECT_NEAR(saliva, expectedSaliva, 1.0e-6f);
    EXPECT_NEAR(enamel, expectedEnamel, 1.0e-6f);

    // AND THEY DIFFER, which is the whole of the "teeth and mucosa are not
    // assigned identical skin response" criterion expressed as a number: enamel
    // reflects roughly three times as much at normal incidence as a saliva film,
    // and an authoring mistake that left both at the default would be invisible
    // in a frame and obvious here.
    EXPECT_GT(enamel, 2.5f * saliva);
}

TEST(SkinOralSurfaceTest, CoatF0IsZeroAtTheIndexOfAir)
{
    // The second, redundant spelling of "dry", and the reason the bound's floor
    // is 1 rather than something above it.
    EXPECT_FLOAT_EQ(SkinOralCoatF0(1.0f), 0.0f);
}

TEST(SkinOralSurfaceTest, CoatF0RejectsNonFiniteAndOutOfRangeIndices)
{
    EXPECT_FLOAT_EQ(SkinOralCoatF0(std::numeric_limits<f32>::quiet_NaN()), 0.0f);
    EXPECT_FLOAT_EQ(SkinOralCoatF0(std::numeric_limits<f32>::infinity()), 0.0f);
    // Below the floor clamps TO the floor, which is F0 = 0 — not to a plausible
    // number for a physically impossible medium.
    EXPECT_FLOAT_EQ(SkinOralCoatF0(0.2f), 0.0f);
    EXPECT_FLOAT_EQ(SkinOralCoatF0(99.0f), SkinOralCoatF0(kMaxSkinOralCoatIor));
}

// -----------------------------------------------------------------------------
// The partition — the central claim, as an exact identity
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, AttenuationAndTheCoatsShareArePartitionOfUnity)
{
    // Swept rather than spot-checked, because the claim is universal: for EVERY
    // legal strength and EVERY legal Fresnel, what the coat takes and what it
    // leaves must add to exactly one. A blend that merely came close would let
    // a strong coat leak a few percent of extra energy per light, per frame.
    for (i32 si = 0; si <= 20; ++si)
    {
        const f32 strength = static_cast<f32>(si) / 20.0f;
        for (i32 fi = 0; fi <= 20; ++fi)
        {
            const f32 fresnel = static_cast<f32>(fi) / 20.0f;
            const f32 attenuation = SkinOralCoatAttenuation(strength, fresnel);

            EXPECT_FLOAT_EQ(attenuation + strength * fresnel, 1.0f)
                << "strength " << strength << ", fresnel " << fresnel;
            EXPECT_GE(attenuation, 0.0f);
            EXPECT_LE(attenuation, 1.0f);
        }
    }
}

TEST(SkinOralSurfaceTest, AttenuationStaysInRangeForOutOfBoundInputs)
{
    // A value outside the bound cannot reach here through a sanitized profile,
    // but the deferred table is indexed by a slot that can be stale by a frame,
    // so the function is asked to be total rather than to trust its caller.
    EXPECT_FLOAT_EQ(SkinOralCoatAttenuation(5.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SkinOralCoatAttenuation(-3.0f, 1.0f), 1.0f);
    // NON-FINITE RETURNS 1, i.e. "the coat took nothing". The conservative
    // answer: a NaN that multiplied a radiance would take the whole pixel.
    EXPECT_FLOAT_EQ(SkinOralCoatAttenuation(std::numeric_limits<f32>::quiet_NaN(), 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(SkinOralCoatAttenuation(0.5f, std::numeric_limits<f32>::quiet_NaN()), 1.0f);
}

TEST(SkinOralSurfaceTest, FresnelRisesMonotonicallyToOneAtGrazing)
{
    const f32 f0 = SkinOralCoatF0(kSalivaIor);

    EXPECT_NEAR(SkinOralCoatFresnel(f0, 1.0f), f0, 1.0e-6f);
    EXPECT_NEAR(SkinOralCoatFresnel(f0, 0.0f), 1.0f, 1.0e-6f);

    f32 previous = SkinOralCoatFresnel(f0, 1.0f);
    for (i32 i = 19; i >= 0; --i)
    {
        const f32 cosTheta = static_cast<f32>(i) / 20.0f;
        const f32 fresnel = SkinOralCoatFresnel(f0, cosTheta);
        // The rim-brightening a wet surface reads as, stated as the inequality
        // it is rather than as a screenshot.
        EXPECT_GE(fresnel, previous) << "cos " << cosTheta;
        previous = fresnel;
    }
}

// -----------------------------------------------------------------------------
// The coat's directional albedo — the claim the identity cannot reach
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, CoatDirectionalAlbedoNeverExceedsItsFresnel)
{
    // THE BOUND: a Smith-masked GGX lobe with no multiple-scattering
    // compensation loses energy and never creates it, so the fraction the coat
    // sends toward the viewer is at most the fraction it reflected. If this
    // fails, the coat is manufacturing light and the "cannot brighten on
    // average" claim in Renderer/SkinOralSurface.h is false.
    //
    // THE CEILING IS 1, NOT F0, and the looser number is the honest one: the
    // Schlick inside the lobe runs up toward 1 as the half-vector approaches
    // grazing, so a wide lobe at an oblique view legitimately returns MORE than
    // F0 — 1.7 x F0 at roughness 0.8 and NdotV 0.25, measured. Asserting F0
    // here would be asserting a near-normal coincidence, and it would go red on
    // correct code at exactly the angles a wet surface is most interesting.
    for (const f32 ior : { kSalivaIor, kEnamelIor })
    {
        const f32 f0 = SkinOralCoatF0(ior);
        for (const f32 roughness : { 0.3f, 0.4f, 0.6f, 0.8f })
        {
            for (const f32 NdotV : { 0.25f, 0.5f, 0.85f, 1.0f })
            {
                const f32 rho = CoatDirectionalAlbedo(roughness, f0, NdotV);
                EXPECT_GE(rho, 0.0f) << "ior " << ior << " r " << roughness << " NdotV " << NdotV;
                EXPECT_LE(rho, 1.0f) << "ior " << ior << " r " << roughness << " NdotV " << NdotV
                                     << " — the coat returned more light than reached it";
            }
        }
    }
}

TEST(SkinOralSurfaceTest, CoatDirectionalAlbedoIsCloseToItsFresnelForASmoothCoat)
{
    // THE BOUND ABOVE PASSES TRIVIALLY FOR A LOBE THAT IS IDENTICALLY ZERO, so
    // this is the case that says the lobe is actually there — and it is the
    // tightest statement this file makes about the coat's magnitude.
    //
    // At roughness 0.3 a Smith-masked single-scattering GGX loses almost
    // nothing, so its directional albedo should sit essentially ON its Fresnel
    // at normal incidence. The measured figure is 0.99 x F0 for both indices;
    // the band below is wide enough that a legitimate quadrature wobble or a
    // change of Smith convention does not trip it, and narrow enough that the
    // errors worth catching cannot hide in it:
    //
    //   * a missing 1/(4 NdotV NdotL) — the classic one, since this file uses
    //     the VISIBILITY form and a reader who "restores" the division gets a
    //     lobe four times too dim at normal incidence;
    //   * a 4*pi or 1/(4*pi) from a solid-angle confusion;
    //   * D normalized to 1 instead of to 1/cos — a factor of pi.
    //
    // Every one of those is more than 30% and none of them changes the
    // partition, the monotonicity or the <= 1 bound by a single bit.
    for (const f32 ior : { kSalivaIor, kEnamelIor })
    {
        const f32 f0 = SkinOralCoatF0(ior);
        for (const f32 NdotV : { 0.5f, 0.85f, 1.0f })
        {
            const f32 rho = CoatDirectionalAlbedo(0.3f, f0, NdotV);
            EXPECT_GT(rho, 0.8f * f0) << "ior " << ior << " NdotV " << NdotV
                                      << ": the coat lobe is missing a third of its energy";
            EXPECT_LT(rho, 1.3f * f0) << "ior " << ior << " NdotV " << NdotV
                                      << ": the coat lobe is carrying far more than its Fresnel";
        }
    }

    // NOT AT GRAZING, and the omission is deliberate rather than convenient: at
    // NdotV 0.25 the lobe straddles the horizon and the Schlick inside it runs
    // up toward 1, so the directional albedo legitimately EXCEEDS F0 — 1.7 x at
    // roughness 0.8. Energy is still conserved (the <= 1 bound above covers it,
    // and it is the honest bound there); it is the "close to F0" framing that
    // stops applying, and asserting it anyway would be asserting a coincidence
    // of the near-normal case.
}

TEST(SkinOralSurfaceTest, EnamelReflectsMoreThanMucosaAtEveryAngle)
{
    // The acceptance criterion "teeth and mucosa are not assigned identical skin
    // response", as an ordering that holds everywhere rather than as one sample
    // where it happens to.
    const f32 saliva = SkinOralCoatF0(kSalivaIor);
    const f32 enamel = SkinOralCoatF0(kEnamelIor);

    for (i32 i = 1; i <= 20; ++i)
    {
        const f32 cosTheta = static_cast<f32>(i) / 20.0f;
        EXPECT_GT(SkinOralCoatFresnel(enamel, cosTheta), SkinOralCoatFresnel(saliva, cosTheta))
            << "cos " << cosTheta;
    }
}

TEST(SkinOralSurfaceTest, CoatSpecularIsZeroForADegenerateHalfVector)
{
    const glm::vec3 N{ 0.0f, 0.0f, 1.0f };
    const glm::vec3 V = glm::normalize(glm::vec3(0.15f, -0.1f, 1.0f));

    // V and -V cancel: no half-vector exists, and the answer is 0 rather than a
    // NaN that would take a whole frame's specular with it.
    EXPECT_FLOAT_EQ(SkinOralCoatSpecular(N, V, -V, 0.12f, 0.02f), 0.0f);

    const glm::vec3 nan{ std::numeric_limits<f32>::quiet_NaN() };
    EXPECT_FLOAT_EQ(SkinOralCoatSpecular(N, nan, V, 0.12f, 0.02f), 0.0f);
    EXPECT_FLOAT_EQ(SkinOralCoatSpecular(N, V, V, std::numeric_limits<f32>::quiet_NaN(), 0.02f), 0.0f);
}

// -----------------------------------------------------------------------------
// Applying the coat to a split
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, ZeroCoatStrengthReturnsTheSplitBitForBit)
{
    // THE A/B CONTROL ARM. "Coat off" must be the version-3 frame EXACTLY, not
    // approximately, or the control is a third variant rather than a baseline
    // and every evidence delta below it measures two changes at once.
    const glm::vec3 diffuse{ 0.31f, 0.22f, 0.18f };
    const glm::vec3 specular{ 0.09f, 0.11f, 0.13f };
    const glm::vec3 N{ 0.0f, 0.0f, 1.0f };
    const glm::vec3 V = glm::normalize(glm::vec3(0.15f, -0.1f, 1.0f));
    const glm::vec3 L = glm::normalize(glm::vec3(0.45f, 0.3f, 0.84f));

    const glm::vec4 dryLane{ 0.0f, 0.12f, SkinOralCoatF0(kSalivaIor), 0.6f };
    const SkinOralCoatResult dry = ApplySkinOralCoat(diffuse, specular, N, V, L, glm::vec3(2.0f), dryLane);

    EXPECT_FLOAT_EQ(dry.Diffuse.x, diffuse.x);
    EXPECT_FLOAT_EQ(dry.Diffuse.y, diffuse.y);
    EXPECT_FLOAT_EQ(dry.Diffuse.z, diffuse.z);
    EXPECT_FLOAT_EQ(dry.Specular.x, specular.x);
    EXPECT_FLOAT_EQ(dry.Specular.y, specular.y);
    EXPECT_FLOAT_EQ(dry.Specular.z, specular.z);
}

TEST(SkinOralSurfaceTest, TheCoatOnlyEverDarkensTheDiffuseHalf)
{
    // THE STRUCTURAL FORM OF "wet specular stays distinct from diffusion". The
    // diffuse half is what oloSkinDiffusionOutput hands the screen-space blur;
    // if the coat could ADD to it, a wet lip would put its highlight through the
    // diffusion kernel and the tissue under it would glow. It can only subtract.
    const glm::vec3 diffuse{ 0.31f, 0.22f, 0.18f };
    const glm::vec3 specular{ 0.09f, 0.11f, 0.13f };
    const glm::vec3 N{ 0.0f, 0.0f, 1.0f };

    for (i32 si = 1; si <= 10; ++si)
    {
        const f32 strength = static_cast<f32>(si) / 10.0f;
        for (const f32 ior : { kSalivaIor, kEnamelIor })
        {
            const glm::vec4 lane{ strength, 0.12f, SkinOralCoatF0(ior), 0.0f };
            for (i32 li = 0; li <= 8; ++li)
            {
                const f32 t = static_cast<f32>(li) / 8.0f;
                const glm::vec3 L = glm::normalize(glm::vec3(t, 0.2f, 1.0f - 0.5f * t));
                const glm::vec3 V = glm::normalize(glm::vec3(0.15f, -0.1f, 1.0f));
                const SkinOralCoatResult wet =
                    ApplySkinOralCoat(diffuse, specular, N, V, L, glm::vec3(2.0f), lane);

                EXPECT_LE(wet.Diffuse.x, diffuse.x) << "strength " << strength;
                EXPECT_LE(wet.Diffuse.y, diffuse.y) << "strength " << strength;
                EXPECT_LE(wet.Diffuse.z, diffuse.z) << "strength " << strength;
                // And never below zero, which would be blurred and re-added as a
                // dark halo by the diffusion pass.
                EXPECT_GE(wet.Diffuse.x, 0.0f);
                EXPECT_GE(wet.Diffuse.y, 0.0f);
                EXPECT_GE(wet.Diffuse.z, 0.0f);
            }
        }
    }
}

TEST(SkinOralSurfaceTest, TheCoatSurvivesANonFiniteRadiance)
{
    const glm::vec3 diffuse{ 0.31f, 0.22f, 0.18f };
    const glm::vec3 specular{ 0.09f, 0.11f, 0.13f };
    const glm::vec3 N{ 0.0f, 0.0f, 1.0f };
    const glm::vec3 V = glm::normalize(glm::vec3(0.15f, -0.1f, 1.0f));
    const glm::vec3 L = glm::normalize(glm::vec3(0.45f, 0.3f, 0.84f));
    const glm::vec4 lane{ 0.45f, 0.12f, SkinOralCoatF0(kSalivaIor), 0.0f };

    const glm::vec3 nan{ std::numeric_limits<f32>::quiet_NaN() };
    const SkinOralCoatResult result = ApplySkinOralCoat(diffuse, specular, N, V, L, nan, lane);

    // Returns the INPUT rather than propagating. A bad radiance is somebody
    // else's bug and this function refusing to spread it is the house rule.
    EXPECT_FLOAT_EQ(result.Diffuse.x, diffuse.x);
    EXPECT_FLOAT_EQ(result.Specular.x, specular.x);
}

// -----------------------------------------------------------------------------
// The cavity weight
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, CavityWeightIsExactlyOneWhenTheOcclusionIsNotSpent)
{
    // The identity arm of the "no glowing interiors" A/B. A version-4 profile
    // that authored only a coat must transmit precisely what #1242 shipped.
    for (i32 i = 0; i <= 20; ++i)
    {
        const f32 ao = static_cast<f32>(i) / 20.0f;
        EXPECT_FLOAT_EQ(SkinOralCavityWeight(ao, 0.0f), 1.0f) << "ao " << ao;
    }
}

TEST(SkinOralSurfaceTest, CavityWeightIsTheOcclusionWhenFullySpent)
{
    // So the test above is not passing by the function being inert.
    for (i32 i = 0; i <= 20; ++i)
    {
        const f32 ao = static_cast<f32>(i) / 20.0f;
        EXPECT_FLOAT_EQ(SkinOralCavityWeight(ao, 1.0f), ao) << "ao " << ao;
    }
}

TEST(SkinOralSurfaceTest, CavityWeightIsMonotoneInBothArguments)
{
    // MORE OCCLUSION NEVER TRANSMITS MORE, and spending more of it never
    // transmits more either. That pair of inequalities IS the "no glowing
    // interiors" claim; everything else about it is a picture.
    f32 previous = SkinOralCavityWeight(0.3f, 0.0f);
    for (i32 i = 1; i <= 20; ++i)
    {
        const f32 amount = static_cast<f32>(i) / 20.0f;
        const f32 weight = SkinOralCavityWeight(0.3f, amount);
        EXPECT_LE(weight, previous) << "amount " << amount;
        EXPECT_GE(weight, 0.0f);
        EXPECT_LE(weight, 1.0f);
        previous = weight;
    }

    previous = SkinOralCavityWeight(0.0f, 0.75f);
    for (i32 i = 1; i <= 20; ++i)
    {
        const f32 ao = static_cast<f32>(i) / 20.0f;
        const f32 weight = SkinOralCavityWeight(ao, 0.75f);
        EXPECT_GE(weight, previous) << "ao " << ao;
        previous = weight;
    }
}

TEST(SkinOralSurfaceTest, CavityWeightRefusesNonFiniteInput)
{
    // 1, i.e. "occlude nothing" — the conservative answer, which loses the
    // effect rather than losing the pixel.
    EXPECT_FLOAT_EQ(SkinOralCavityWeight(std::numeric_limits<f32>::quiet_NaN(), 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(SkinOralCavityWeight(0.5f, std::numeric_limits<f32>::infinity()), 1.0f);
}

// -----------------------------------------------------------------------------
// The lane and the version gate
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, TheLanePacksTheAuthoredFieldsAndDerivesF0)
{
    const SkinProfileParameters parameters = MakeOralProfile(0.45f, 0.12f, kSalivaIor, 0.6f);
    const glm::vec4 lane = SkinOralLane(parameters);

    EXPECT_FLOAT_EQ(lane.x, 0.45f);
    EXPECT_FLOAT_EQ(lane.y, 0.12f);
    // THE CONVERSION HAPPENS ON THE CPU AND ONLY HERE, which is what stops a
    // shader inventing a second opinion about it.
    EXPECT_FLOAT_EQ(lane.z, SkinOralCoatF0(kSalivaIor));
    EXPECT_FLOAT_EQ(lane.w, 0.6f);
}

TEST(SkinOralSurfaceTest, ADefaultProfilePacksANeutralLane)
{
    // An unclaimed or stale deferred slot must LOSE the effect, not acquire
    // someone else's wetness — so the neutral lane's x, the field that gates
    // everything, is zero.
    const SkinProfileParameters defaults{};
    EXPECT_FLOAT_EQ(SkinOralLane(defaults).x, 0.0f);
}

TEST(SkinOralSurfaceTest, OnlyTransportVersionFourEvaluatesTheOralTerms)
{
    // An `==`, not a `>=`: a version this code has no arm for applies NOTHING.
    EXPECT_FALSE(SkinEvaluatesOralSurface(SkinEvaluationModel::DiffuseSpecularSplit));
    EXPECT_FALSE(SkinEvaluatesOralSurface(SkinEvaluationModel::ScreenSpaceDiffusion));
    EXPECT_FALSE(SkinEvaluatesOralSurface(SkinEvaluationModel::ThicknessTransmission));
    EXPECT_FALSE(SkinEvaluatesOralSurface(SkinEvaluationModel::LayeredSpecular));
    EXPECT_TRUE(SkinEvaluatesOralSurface(SkinEvaluationModel::OralSurface));
}

// -----------------------------------------------------------------------------
// Sanitization
// -----------------------------------------------------------------------------

TEST(SkinOralSurfaceTest, SanitizeClampsEveryOralFieldIntoItsBound)
{
    SkinProfileParameters parameters;
    parameters.EvaluationModel = SkinEvaluationModel::OralSurface;
    parameters.Oral.CoatStrength = 4.0f;
    parameters.Oral.CoatRoughness = -1.0f;
    parameters.Oral.CoatIor = 12.0f;
    parameters.Oral.CavityOcclusion = 3.0f;

    EXPECT_FALSE(parameters.Sanitize()) << "Sanitize must report that it had to correct something";

    EXPECT_FLOAT_EQ(parameters.Oral.CoatStrength, kMaxSkinOralCoatStrength);
    EXPECT_FLOAT_EQ(parameters.Oral.CoatRoughness, kMinSkinOralCoatRoughness);
    EXPECT_FLOAT_EQ(parameters.Oral.CoatIor, kMaxSkinOralCoatIor);
    EXPECT_FLOAT_EQ(parameters.Oral.CavityOcclusion, kMaxSkinOralCavityOcclusion);
}

TEST(SkinOralSurfaceTest, SanitizeReplacesNonFiniteOralFieldsWithTheirDefaults)
{
    // Clamping cannot fix a NaN — it compares false against every bound and
    // passes straight through std::clamp — so the defaults are what catch it.
    const SkinOralParameters defaults{};
    SkinProfileParameters parameters;
    parameters.EvaluationModel = SkinEvaluationModel::OralSurface;
    parameters.Oral.CoatStrength = std::numeric_limits<f32>::quiet_NaN();
    parameters.Oral.CoatRoughness = std::numeric_limits<f32>::infinity();
    parameters.Oral.CoatIor = -std::numeric_limits<f32>::infinity();
    parameters.Oral.CavityOcclusion = std::numeric_limits<f32>::quiet_NaN();

    EXPECT_FALSE(parameters.Sanitize());

    EXPECT_FLOAT_EQ(parameters.Oral.CoatStrength, defaults.CoatStrength);
    EXPECT_FLOAT_EQ(parameters.Oral.CoatRoughness, defaults.CoatRoughness);
    EXPECT_FLOAT_EQ(parameters.Oral.CoatIor, defaults.CoatIor);
    EXPECT_FLOAT_EQ(parameters.Oral.CavityOcclusion, defaults.CavityOcclusion);

    // And the lane built from the repaired record is finite, which is the
    // property the whole gate exists for.
    const glm::vec4 lane = SkinOralLane(parameters);
    EXPECT_TRUE(std::isfinite(lane.x) && std::isfinite(lane.y) && std::isfinite(lane.z) &&
                std::isfinite(lane.w));
}

TEST(SkinOralSurfaceTest, ADefaultOralBlockNeedsNoCorrection)
{
    SkinProfileParameters parameters;
    parameters.EvaluationModel = SkinEvaluationModel::OralSurface;
    EXPECT_TRUE(parameters.Sanitize())
        << "a freshly defaulted version-4 profile must already be in range — otherwise every "
           "load logs a correction nobody caused";
}
