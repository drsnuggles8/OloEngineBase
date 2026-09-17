// OLO_TEST_LAYER: L1
// =============================================================================
// SkinTransmissionTest.cpp — the energy bookkeeping and the unit chain of the
// thin-region transmission term. Issue #1242.
//
// WHY THIS TEST SHAPE. The issue's third acceptance criterion is that
// transmission and diffusion must not double-count the same energy, and the
// failure mode of getting it wrong is the expensive kind: a head that looks
// FINE from the front and uniformly emissive from behind. No screenshot review
// reliably catches "slightly too much light in the ear", and by the time the
// frame is wrong the cause is three files away.
//
// So the argument in Renderer/SkinTransmission.h is not tested as a whole —
// each of its three PREMISES is tested separately, because each can break on
// its own and each breaks differently:
//
//   1. DISJOINT INCIDENT DIRECTIONS. The lobe is EXACTLY zero for every light
//      on the near side, so no photon is counted by both transports. This is
//      the premise a "wrap" or "half-Lambert" term would quietly break, and it
//      is the one that would still look plausible.
//   2. THE LOBE IS BOUNDED BY 1. So the term can never exceed the incident
//      radiance. This is what a NORMALISED phase function would break, at its
//      peak only — i.e. at one viewing angle, in one frame.
//   3. THE UNIT CHAIN. Millimetres all the way, with ThicknessScale in it
//      exactly once. A unit slip here is a factor of 1000 and is the single
//      most likely defect in the whole feature.
//
// Plus the fourth criterion's conservative fallback: a missing thickness
// transmits NOTHING rather than everything, which is the sign of the inequality
// that separates "unfinished" from "wrong".
//
// Classification: L1 (pure CPU maths, no GL context, no renderer, no scene).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinTransmission.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A sanitized transmitting profile. Every test starts from this and
        // moves ONE thing, so a failure names the parameter that caused it.
        [[nodiscard]] SkinProfileParameters TransmittingProfile()
        {
            SkinProfileParameters p{};
            p.EvaluationModel = SkinEvaluationModel::ThicknessTransmission;
            EXPECT_TRUE(p.Sanitize()) << "the default profile needed correcting — a bound moved";
            return p;
        }

        // The geometric configuration of a BACKLIT pixel: the surface faces the
        // viewer, the light is behind it.
        constexpr glm::vec3 kNormalTowardViewer{ 0.0f, 0.0f, 1.0f };
        constexpr glm::vec3 kViewTowardViewer{ 0.0f, 0.0f, 1.0f };
        // `lightDir` points FROM the surface TOWARD the light (oloLightSample's
        // convention), so a light behind the surface points AWAY from the eye.
        constexpr glm::vec3 kLightBehind{ 0.0f, 0.0f, -1.0f };
        constexpr glm::vec3 kLightInFront{ 0.0f, 0.0f, 1.0f };

        // A parameter sweep wide enough to be a claim about the whole authored
        // range rather than about one profile. Deliberately includes both bounds
        // of every field: a clamp that is off by one side shows up here.
        [[nodiscard]] std::vector<SkinProfileParameters> SweepProfiles()
        {
            std::vector<SkinProfileParameters> out;
            for (const f32 albedo : { 0.0f, 0.05f, 0.5f, 0.85f, 1.0f })
            {
                for (const f32 radius : { kMinSkinScatterRadiusMM, 0.55f, 1.55f, 31.0f, kMaxSkinScatterRadiusMM })
                {
                    for (const f32 strength : { kMinSkinTransmissionStrength, 0.5f, kMaxSkinTransmissionStrength })
                    {
                        for (const f32 anisotropy : { kMinSkinTransmissionAnisotropy, 0.7f, kMaxSkinTransmissionAnisotropy })
                        {
                            for (const f32 power : { kMinSkinTransmissionPower, 4.0f, kMaxSkinTransmissionPower })
                            {
                                SkinProfileParameters p{};
                                p.EvaluationModel = SkinEvaluationModel::ThicknessTransmission;
                                p.ScatterColor = glm::vec3(albedo);
                                p.ScatterRadiusMM = glm::vec3(radius);
                                p.Transmission.Strength = strength;
                                p.Transmission.Anisotropy = anisotropy;
                                p.Transmission.Power = power;
                                (void)p.Sanitize();
                                out.push_back(p);
                            }
                        }
                    }
                }
            }
            return out;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // PREMISE 1 — the two transports draw from disjoint incident directions
    // -------------------------------------------------------------------------

    // THE LOAD-BEARING TEST OF THE WHOLE FEATURE. If this fails, the energy
    // argument in Renderer/SkinTransmission.h is void and the head can glow
    // while it is also being lit — which is the plausible-but-wrong frame.
    TEST(SkinTransmission, TheLobeIsExactlyZeroForEveryLightOnTheNearSide)
    {
        const SkinProfileParameters profile = TransmittingProfile();

        // Every direction in the near hemisphere, plus the tangent ring.
        //
        // THE ASSERTION BRANCHES ON THE ACTUAL dot(N, L), NOT ON THE NOMINAL
        // ANGLE, and that is not a weakening — it is what makes the claim
        // checkable at all. `std::cos(glm::radians(90.0f))` is -4.4e-8, not 0,
        // so the vector built at a nominal 90 degrees lands a hair on the FAR
        // side and the lobe is correctly ~1e-8 rather than exactly 0. Asserting
        // zero there would be asserting that `clamp(-dot(N, L), 0, 1)` rounds
        // its input, which it does not and should not.
        //
        // So the sweep asks the question premise 1 actually makes: for every
        // direction genuinely in the NEAR hemisphere the lobe is EXACTLY zero,
        // and the straddling ray is required to be negligible rather than
        // exactly zero. `oloSkinTransmissionLobe`'s tangent case is pinned
        // exactly instead, by ShaderUnit_SkinTransmission.glsl's TANGENT column,
        // where the direction is the literal (1, 0, 0) and the dot is a true 0.
        for (i32 degrees = 0; degrees <= 90; ++degrees)
        {
            const f32 radians = glm::radians(static_cast<f32>(degrees));
            const glm::vec3 lightDir{ std::sin(radians), 0.0f, std::cos(radians) };
            const f32 lobe = SkinTransmissionLobe(kNormalTowardViewer, kViewTowardViewer, lightDir,
                                                  profile.Transmission);

            if (const f32 ndotl = glm::dot(kNormalTowardViewer, lightDir); ndotl >= 0.0f)
            {
                EXPECT_EQ(lobe, 0.0f)
                    << "a light " << degrees
                    << " degrees off the surface normal (dot(N, L) = " << ndotl
                    << ") is on the LIT side, where the reflected diffuse lobe already counts it — transmitting as "
                       "well is the double-count #1242 forbids";
            }
            else
            {
                // Within one float epsilon of tangency. A term that returned
                // anything visible here would rim every silhouette.
                EXPECT_LE(lobe, 1.0e-6f)
                    << "a light " << degrees << " degrees off the normal landed a hair past tangency (dot(N, L) = "
                    << ndotl << ") and transmitted " << lobe
                    << " — negligible is fine, visible is a silhouette rim";
            }
        }
    }

    // The same claim through the whole term rather than through the lobe alone,
    // so an added constant or an offset anywhere in the product is caught too.
    TEST(SkinTransmission, TheTermVanishesUnderFrontLighting)
    {
        const SkinProfileParameters profile = TransmittingProfile();
        const f32 thickness = SkinThicknessBaseMM(0.002f, profile.ThicknessScale);
        ASSERT_GT(thickness, 0.0f) << "the fixture must author a real thickness or this proves nothing";

        const glm::vec3 frontLit = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightInFront,
                                                            glm::vec3(10.0f), glm::vec3(0.8f), 1.0f, thickness,
                                                            profile);
        EXPECT_EQ(frontLit, glm::vec3(0.0f)) << "front lighting must transmit nothing";

        // And the control: the SAME call with the light moved behind must be
        // non-zero, or the test above would pass on a term that never fires.
        const glm::vec3 backLit = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                           glm::vec3(10.0f), glm::vec3(0.8f), 1.0f, thickness,
                                                           profile);
        EXPECT_GT(backLit.r, 0.0f) << "backlighting must transmit SOMETHING, or the vanishing test above is vacuous";
    }

    // -------------------------------------------------------------------------
    // PREMISE 2 — the lobe is bounded by 1, so the term cannot add energy
    // -------------------------------------------------------------------------

    TEST(SkinTransmission, TheLobeNeverExceedsOneOverTheWholeSphereAndParameterRange)
    {
        for (const SkinProfileParameters& profile : SweepProfiles())
        {
            for (i32 theta = 0; theta <= 180; theta += 5)
            {
                for (i32 phi = 0; phi < 360; phi += 15)
                {
                    const f32 t = glm::radians(static_cast<f32>(theta));
                    const f32 p = glm::radians(static_cast<f32>(phi));
                    const glm::vec3 lightDir{ std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t) };

                    const f32 lobe = SkinTransmissionLobe(kNormalTowardViewer, kViewTowardViewer, lightDir,
                                                          profile.Transmission);
                    ASSERT_TRUE(std::isfinite(lobe));
                    ASSERT_GE(lobe, 0.0f);
                    ASSERT_LE(lobe, 1.0f) << "a lobe above 1 lets a grazing view add energy — which is what a "
                                             "NORMALISED phase function would do at its peak";
                }
            }
        }
    }

    // THE CRITERION ITSELF, swept: diffusion + transmission never exceed the
    // incident energy, for any sanitized profile and any thickness.
    TEST(SkinTransmission, TheJointEnergyBoundNeverExceedsTheIncidentRadiance)
    {
        constexpr glm::vec3 kWhiteAlbedo{ 1.0f, 1.0f, 1.0f };

        for (const SkinProfileParameters& profile : SweepProfiles())
        {
            for (const f32 thicknessMM : { 0.0f, 0.001f, 0.5f, 2.0f, 12.0f, 200.0f, kMaxSkinThicknessMM,
                                           kMaxSkinThicknessMM * 10.0f })
            {
                const glm::vec3 bound = SkinTransmissionEnergyBound(thicknessMM, kWhiteAlbedo, profile);
                for (int c = 0; c < 3; ++c)
                {
                    ASSERT_TRUE(std::isfinite(bound[c]));
                    ASSERT_GE(bound[c], 0.0f);
                    // A WHITE albedo is the worst case, and it makes the bound
                    // exactly 1 rather than merely under it: the diffuse half of
                    // a white Lambertian surface returns all of the incident
                    // energy. So `<= 1` here is tight, not slack.
                    ASSERT_LE(bound[c], 1.0f)
                        << "thickness " << thicknessMM << " mm, channel " << c
                        << ": the diffuse and transmitted transports together exceed the incident radiance";
                }
            }
        }
    }

    // The bound is a MAXIMUM over geometry, so the evaluated term must never
    // beat it. This is what ties the bound to the thing that actually shades:
    // a bound nothing is measured against is a comment.
    TEST(SkinTransmission, TheEvaluatedTermNeverExceedsItsOwnEnergyBound)
    {
        constexpr glm::vec3 kAlbedo{ 0.85f, 0.62f, 0.55f };
        constexpr glm::vec3 kRadiance{ 1.0f, 1.0f, 1.0f };

        for (const SkinProfileParameters& profile : SweepProfiles())
        {
            const glm::vec3 bound = SkinTransmissionEnergyBound(2.0f, kAlbedo, profile);

            for (i32 theta = 91; theta <= 180; theta += 7)
            {
                for (i32 phi = 0; phi < 360; phi += 30)
                {
                    const f32 t = glm::radians(static_cast<f32>(theta));
                    const f32 p = glm::radians(static_cast<f32>(phi));
                    const glm::vec3 lightDir{ std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t) };

                    const glm::vec3 term = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, lightDir,
                                                                     kRadiance, kAlbedo, 1.0f, 2.0f, profile);
                    for (int c = 0; c < 3; ++c)
                    {
                        ASSERT_TRUE(std::isfinite(term[c]));
                        ASSERT_LE(term[c], bound[c] + 1.0e-6f)
                            << "the evaluated term exceeds the bound the energy criterion is asserted against";
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // PREMISE 3 — the unit chain
    // -------------------------------------------------------------------------

    // THE FACTOR-OF-1000 TEST. `ThicknessScale` defaults to 1000 precisely
    // because one world unit is one metre and the radii are in millimetres, so a
    // 2 mm ear is authored as a thicknessFactor of 0.002.
    TEST(SkinTransmission, TheDefaultThicknessScaleIsTheMetreToMillimetreIdentity)
    {
        const SkinProfileParameters profile = TransmittingProfile();
        ASSERT_FLOAT_EQ(profile.ThicknessScale, 1000.0f)
            << "the default scale is the m -> mm identity; if it moved, this whole chain means something else";

        // 2 mm of ear, authored in metres.
        EXPECT_FLOAT_EQ(SkinThicknessBaseMM(0.002f, profile.ThicknessScale), 2.0f);
        // And a whole 20 cm head.
        EXPECT_FLOAT_EQ(SkinThicknessBaseMM(0.2f, profile.ThicknessScale), 200.0f);
    }

    // The two spellings of the chain must agree, or the GPU (which is handed the
    // BASE and multiplies by the map itself) and the CPU would disagree.
    TEST(SkinTransmission, TheBaseThicknessTimesTheMapSampleIsTheFullChain)
    {
        for (const f32 scale : { 1.0f, 100.0f, 1000.0f, kMaxSkinThicknessScale })
        {
            for (const f32 metres : { 0.0005f, 0.002f, 0.05f })
            {
                for (const f32 sample : { 0.0f, 0.25f, 0.5f, 1.0f })
                {
                    const f32 full = SkinThicknessMM(metres, sample, scale);
                    const f32 viaBase = std::min(SkinThicknessBaseMM(metres, scale) * sample, kMaxSkinThicknessMM);
                    EXPECT_NEAR(full, viaBase, 1.0e-3f)
                        << "scale " << scale << ", " << metres << " m, sample " << sample
                        << ": the shader multiplies the BASE by the map sample, so the two spellings must agree";
                }
            }
        }
    }

    // THE SHARED PHYSICS. The transmittance must use the SAME Burley scaling the
    // diffusion kernel is built from — that sharing is the whole of this
    // feature's answer to "are these two features two opinions about one
    // surface?", and it is only a real answer if it is actually the same number.
    TEST(SkinTransmission, TheTransmittanceUsesTheSameBurleyScalingAsTheDiffusionKernel)
    {
        SkinProfileParameters profile = TransmittingProfile();
        profile.ScatterRadiusMM = glm::vec3(1.55f, 0.80f, 0.55f);
        profile.ScatterColor = glm::vec3(0.85f, 0.55f, 0.45f);
        ASSERT_TRUE(profile.Sanitize());

        const glm::vec3 d = SkinBurleyScalingMM(profile);
        const glm::vec3 scalingLane = glm::vec3(SkinTransmissionScalingLane(profile));
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_FLOAT_EQ(scalingLane[c], d[c])
                << "channel " << c << ": the lane handed to the GPU is not the diffusion kernel's own scaling";
        }

        // And the transmittance at one mean free path is ScatterColor / e,
        // which pins the Beer-Lambert form rather than merely its monotonicity.
        for (int c = 0; c < 3; ++c)
        {
            const glm::vec3 transmittance = SkinTransmittance(d[c], profile);
            EXPECT_NEAR(transmittance[c], profile.ScatterColor[c] * std::exp(-1.0f), 1.0e-4f)
                << "channel " << c << ": at one scaling length the survival must be exactly 1/e";
        }
    }

    // RED MUST OUTLIVE BLUE. This is the reason a backlit ear reads as red
    // rather than as a white glow, and it is carried entirely by the authored
    // per-channel radii — not by a tint, which this feature deliberately does
    // not have.
    TEST(SkinTransmission, RedTransmitsFurtherThanBlueThroughTheSameThickness)
    {
        SkinProfileParameters profile = TransmittingProfile();
        profile.ScatterRadiusMM = glm::vec3(1.55f, 0.80f, 0.55f);
        profile.ScatterColor = glm::vec3(0.85f, 0.55f, 0.45f);
        ASSERT_TRUE(profile.Sanitize());

        // At a thickness comparable to the radii, where the exponentials differ
        // most; far beyond them everything is zero and the claim is vacuous.
        const glm::vec3 transmittance = SkinTransmittance(2.0f, profile);
        EXPECT_GT(transmittance.r, transmittance.g);
        EXPECT_GT(transmittance.g, transmittance.b);
    }

    // Monotone in thickness, per channel. A thicker region transmits less —
    // which sounds too obvious to test, except that it is what a sign slip in
    // the exponent would break, and a sign slip there renders a head whose
    // THICKEST parts glow most.
    TEST(SkinTransmission, ThickerRegionsTransmitStrictlyLess)
    {
        const SkinProfileParameters profile = TransmittingProfile();

        f32 previousRed = 2.0f;
        for (const f32 thicknessMM : { 0.1f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f })
        {
            const glm::vec3 transmittance = SkinTransmittance(thicknessMM, profile);
            ASSERT_LT(transmittance.r, previousRed)
                << "thickness " << thicknessMM << " mm transmits at least as much as a thinner region";
            previousRed = transmittance.r;
        }
    }

    // -------------------------------------------------------------------------
    // The conservative fallback — and the SIGN of it
    // -------------------------------------------------------------------------

    // THE UNIFORMLY EMISSIVE HEAD, tested directly. A zero thickness has two
    // readings and only one of them is safe: "no volume authored" (transmit
    // nothing) and "infinitely thin" (exp(0) = 1, transmit everything). The
    // second is the failure the issue's second criterion names by name.
    TEST(SkinTransmission, AMissingThicknessTransmitsNothingRatherThanEverything)
    {
        const SkinProfileParameters profile = TransmittingProfile();

        EXPECT_EQ(SkinTransmittance(kSkinThicknessMissing, profile), glm::vec3(0.0f))
            << "a zero thickness must read as 'no volume authored', not as 'infinitely thin'";
        EXPECT_EQ(SkinTransmittance(0.0f, profile), glm::vec3(0.0f));

        const glm::vec3 term = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                         glm::vec3(50.0f), glm::vec3(1.0f), 1.0f,
                                                         kSkinThicknessMissing, profile);
        EXPECT_EQ(term, glm::vec3(0.0f))
            << "with no authored thickness the whole term must vanish — a bright backlight must not produce a glow";
    }

    // Occlusion has to reach the term, or the issue's second criterion ("and
    // occlusion") is unmet. Tested as a proportionality rather than as a
    // threshold, because the shared visibility factor is a multiply.
    TEST(SkinTransmission, OcclusionScalesTheTermProportionally)
    {
        const SkinProfileParameters profile = TransmittingProfile();
        const f32 thickness = 2.0f;

        const glm::vec3 unshadowed = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                               glm::vec3(4.0f), glm::vec3(0.8f), 1.0f, thickness,
                                                               profile);
        ASSERT_GT(unshadowed.r, 0.0f);

        const glm::vec3 half = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                         glm::vec3(4.0f), glm::vec3(0.8f), 0.5f, thickness, profile);
        EXPECT_NEAR(half.r, unshadowed.r * 0.5f, 1.0e-5f);

        const glm::vec3 occluded = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                             glm::vec3(4.0f), glm::vec3(0.8f), 0.0f, thickness,
                                                             profile);
        EXPECT_EQ(occluded, glm::vec3(0.0f)) << "a fully shadowed thin region must not glow";
    }

    // -------------------------------------------------------------------------
    // The transport version gate (ADR 0024)
    // -------------------------------------------------------------------------

    // A profile authored against an older transport must NOT acquire this term.
    // That is the whole reason SkinEvaluationModel exists, and the regression it
    // prevents is "upgrading the engine restated every scene".
    TEST(SkinTransmission, OlderTransportVersionsDoNotTransmit)
    {
        for (const SkinEvaluationModel model :
             { SkinEvaluationModel::DiffuseSpecularSplit, SkinEvaluationModel::ScreenSpaceDiffusion })
        {
            SkinProfileParameters profile{};
            profile.EvaluationModel = model;
            profile.Transmission.Strength = 1.0f;
            ASSERT_TRUE(profile.Sanitize());

            const glm::vec3 term = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                             glm::vec3(10.0f), glm::vec3(1.0f), 1.0f, 2.0f, profile);
            EXPECT_EQ(term, glm::vec3(0.0f))
                << ToString(model) << " must not transmit — turning transmission on is an authoring act per profile";

            // And the energy bound agrees, so the bound cannot pass a profile
            // the evaluator would have transmitted for.
            const glm::vec3 bound = SkinTransmissionEnergyBound(2.0f, glm::vec3(0.5f), profile);
            EXPECT_EQ(bound, glm::vec3(0.5f)) << ToString(model) << ": the bound must be the diffuse half alone";
        }
    }

    TEST(SkinTransmission, ZeroStrengthDisablesTheTermEvenAtVersionTwo)
    {
        SkinProfileParameters profile = TransmittingProfile();
        profile.Transmission.Strength = 0.0f;
        ASSERT_TRUE(profile.Sanitize());

        const glm::vec3 term = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                         glm::vec3(10.0f), glm::vec3(1.0f), 1.0f, 2.0f, profile);
        EXPECT_EQ(term, glm::vec3(0.0f));
    }

    // -------------------------------------------------------------------------
    // Version 2 still DIFFUSES — the regression #1242 actually caused
    // -------------------------------------------------------------------------

    // A VERSION-2 PROFILE MUST STILL GET A REAL DIFFUSION KERNEL, because
    // version 2 is "everything version 1 does, PLUS transmission"
    // (Renderer/SkinProfile.h). This is a one-line test for a bug that shipped
    // in this very issue's first draft and cost an evidence-test investigation:
    // the SHADER was taught to hand a version-2 pixel's diffuse half to the
    // diffusion pass, while BuildSkinDiffusionKernel still answered with an
    // IDENTITY kernel for anything that was not exactly version 1. The pass then
    // blurred by nothing and added `blur(aux) - aux == 0`.
    //
    // The symptom was the worst kind: a head that gained backlit ears and lost
    // its soft terminator in the same authoring click, which reads as "the new
    // feature broke subsurface scattering" and points at the wrong file.
    //
    // Asserted against version 1's kernel rather than merely "not identity", so
    // it also catches a version-2 kernel that is built but built DIFFERENTLY —
    // the two versions share every scattering parameter, so their kernels must
    // be identical.
    TEST(SkinTransmission, VersionTwoGetsTheSameRealDiffusionKernelAsVersionOne)
    {
        SkinProfileParameters diffusing{};
        diffusing.EvaluationModel = SkinEvaluationModel::ScreenSpaceDiffusion;
        diffusing.ScatterRadiusMM = glm::vec3(31.0f, 16.0f, 11.0f);
        ASSERT_TRUE(diffusing.Sanitize());

        SkinProfileParameters transmitting = diffusing;
        transmitting.EvaluationModel = SkinEvaluationModel::ThicknessTransmission;
        ASSERT_TRUE(transmitting.Sanitize());

        const SkinDiffusionKernel v1 = BuildSkinDiffusionKernel(diffusing, SkinDiffusionQuality::High);
        const SkinDiffusionKernel v2 = BuildSkinDiffusionKernel(transmitting, SkinDiffusionQuality::High);

        ASSERT_FALSE(v1.IsIdentity()) << "the version-1 fixture does not diffuse, so this test proves nothing";
        EXPECT_FALSE(v2.IsIdentity())
            << "a VERSION-2 profile got an IDENTITY diffusion kernel. Version 2 is version 1 plus transmission, so "
               "enabling transmission has just silently turned subsurface scattering OFF.";

        EXPECT_FLOAT_EQ(v2.SupportRadiusMM, v1.SupportRadiusMM);
        ASSERT_EQ(v2.TapCount, v1.TapCount);
        for (u32 tap = 0; tap < v1.TapCount; ++tap)
        {
            for (int c = 0; c < 4; ++c)
            {
                EXPECT_FLOAT_EQ(v2.Taps[tap][c], v1.Taps[tap][c])
                    << "tap " << tap << " component " << c
                    << ": the two versions share every scattering parameter, so their kernels must be identical";
            }
        }

        // And version 0 must STILL get an identity, or the version gate has been
        // widened into "everything diffuses" and ADR 0024's whole point is lost.
        SkinProfileParameters legacy = diffusing;
        legacy.EvaluationModel = SkinEvaluationModel::DiffuseSpecularSplit;
        ASSERT_TRUE(legacy.Sanitize());
        EXPECT_TRUE(BuildSkinDiffusionKernel(legacy, SkinDiffusionQuality::High).IsIdentity())
            << "a version-0 profile must not be diffused — turning diffusion on is an authoring act per profile";
    }

    // -------------------------------------------------------------------------
    // Validation — every float from an asset is checked, per CLAUDE.md
    // -------------------------------------------------------------------------

    TEST(SkinTransmission, SanitizeClampsAndReplacesEveryTransmissionField)
    {
        const SkinTransmissionParameters defaults{};

        SkinTransmissionParameters nonFinite{};
        nonFinite.Strength = std::numeric_limits<f32>::quiet_NaN();
        nonFinite.Anisotropy = std::numeric_limits<f32>::infinity();
        nonFinite.Power = -std::numeric_limits<f32>::infinity();
        EXPECT_FALSE(nonFinite.Sanitize()) << "a non-finite field must be REPORTED as corrected, not silently fixed";
        // Replaced with the DEFAULT, not clamped: NaN compares false against
        // every bound, so std::clamp would pass it straight through.
        EXPECT_FLOAT_EQ(nonFinite.Strength, defaults.Strength);
        EXPECT_FLOAT_EQ(nonFinite.Anisotropy, defaults.Anisotropy);
        EXPECT_FLOAT_EQ(nonFinite.Power, defaults.Power);

        SkinTransmissionParameters outOfRange{};
        outOfRange.Strength = 12.0f; // above 1 would break the energy bound
        outOfRange.Anisotropy = -3.0f;
        outOfRange.Power = 4096.0f;
        EXPECT_FALSE(outOfRange.Sanitize());
        EXPECT_FLOAT_EQ(outOfRange.Strength, kMaxSkinTransmissionStrength);
        EXPECT_FLOAT_EQ(outOfRange.Anisotropy, kMinSkinTransmissionAnisotropy);
        EXPECT_FLOAT_EQ(outOfRange.Power, kMaxSkinTransmissionPower);
    }

    // A corrupt texture or a corrupt asset must not become a NaN pixel. Every
    // entry point is fed rubbish, because "the caller validated it" is the
    // assumption that stops being true the moment a second caller appears.
    TEST(SkinTransmission, NonFiniteInputsProduceZeroRatherThanNaN)
    {
        const SkinProfileParameters profile = TransmittingProfile();
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();

        EXPECT_EQ(SkinThicknessMM(nan, 1.0f, 1000.0f), kSkinThicknessMissing);
        EXPECT_EQ(SkinThicknessMM(0.002f, nan, 1000.0f), kSkinThicknessMissing);
        EXPECT_EQ(SkinThicknessMM(0.002f, 1.0f, nan), kSkinThicknessMissing);
        EXPECT_EQ(SkinThicknessMM(inf, 1.0f, 1000.0f), kSkinThicknessMissing);

        EXPECT_EQ(SkinTransmittance(nan, profile), glm::vec3(0.0f));
        EXPECT_EQ(SkinTransmittance(-1.0f, profile), glm::vec3(0.0f));

        EXPECT_EQ(SkinTransmissionLobe(glm::vec3(nan), kViewTowardViewer, kLightBehind, profile.Transmission), 0.0f);
        EXPECT_EQ(SkinTransmissionLobe(kNormalTowardViewer, glm::vec3(inf), kLightBehind, profile.Transmission), 0.0f);

        const glm::vec3 nanRadiance = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                                glm::vec3(nan), glm::vec3(0.8f), 1.0f, 2.0f, profile);
        EXPECT_EQ(nanRadiance, glm::vec3(0.0f));

        const glm::vec3 nanVisibility = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, kLightBehind,
                                                                  glm::vec3(4.0f), glm::vec3(0.8f), nan, 2.0f, profile);
        EXPECT_EQ(nanVisibility, glm::vec3(0.0f));
    }

    // An absurd thickness is CLAMPED rather than allowed to underflow the
    // exponential into a denormal, and it is monotone up to the clamp — so a
    // corrupt map cannot make a region transmit MORE.
    TEST(SkinTransmission, AnAbsurdThicknessIsClampedAndStillTransmitsNothing)
    {
        const SkinProfileParameters profile = TransmittingProfile();

        const glm::vec3 atCeiling = SkinTransmittance(kMaxSkinThicknessMM, profile);
        const glm::vec3 beyond = SkinTransmittance(kMaxSkinThicknessMM * 1000.0f, profile);
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_TRUE(std::isfinite(beyond[c]));
            EXPECT_FLOAT_EQ(beyond[c], atCeiling[c]) << "channel " << c << ": past the ceiling the answer must not move";
            EXPECT_NEAR(atCeiling[c], 0.0f, 1.0e-6f) << "two metres of skin must transmit nothing measurable";
        }
    }

    // -------------------------------------------------------------------------
    // The lane packing the GPU is handed
    // -------------------------------------------------------------------------

    // The lanes are what BOTH shading paths read, so a packing slip is a
    // per-path divergence waiting to happen — and the lane form is also what
    // the shader-parity test drives.
    TEST(SkinTransmission, TheLanesCarryThePremultipliedAlbedoAndTheAuthoredLobe)
    {
        SkinProfileParameters profile = TransmittingProfile();
        profile.ScatterColor = glm::vec3(0.8f, 0.6f, 0.4f);
        profile.Transmission.Strength = 0.5f;
        profile.Transmission.Anisotropy = 0.25f;
        profile.Transmission.Power = 8.0f;
        ASSERT_TRUE(profile.Sanitize());

        const glm::vec4 scatter = SkinTransmissionScatterLane(profile);
        EXPECT_FLOAT_EQ(scatter.x, 0.8f * 0.5f);
        EXPECT_FLOAT_EQ(scatter.y, 0.6f * 0.5f);
        EXPECT_FLOAT_EQ(scatter.z, 0.4f * 0.5f);
        EXPECT_FLOAT_EQ(scatter.w, 0.25f) << "the anisotropy rides the scatter lane's w";

        const glm::vec4 scaling = SkinTransmissionScalingLane(profile);
        EXPECT_FLOAT_EQ(scaling.w, 8.0f) << "the power rides the scaling lane's w";
        for (int c = 0; c < 3; ++c)
            EXPECT_GT(scaling[c], 0.0f) << "the scaling lane must be strictly positive or the shader divides by zero";
    }

    // The lane-based evaluator and the profile-based one must agree — they are
    // the same expression, and the parity test compares the SHADER against the
    // lane form, so a divergence here would make that test prove nothing.
    TEST(SkinTransmission, TheLaneEvaluatorAgreesWithTheProfileEvaluator)
    {
        constexpr glm::vec3 kAlbedo{ 0.8f, 0.6f, 0.5f };
        constexpr glm::vec3 kRadiance{ 3.0f, 3.0f, 3.0f };

        for (const SkinProfileParameters& profile : SweepProfiles())
        {
            for (i32 theta = 95; theta <= 180; theta += 15)
            {
                const f32 t = glm::radians(static_cast<f32>(theta));
                const glm::vec3 lightDir{ std::sin(t), 0.0f, std::cos(t) };

                const glm::vec3 viaProfile = EvaluateSkinTransmission(kNormalTowardViewer, kViewTowardViewer, lightDir,
                                                                       kRadiance, kAlbedo, 0.75f, 2.0f, profile);
                const glm::vec3 viaLanes = EvaluateSkinTransmissionLanes(
                    kNormalTowardViewer, kViewTowardViewer, lightDir, kRadiance, kAlbedo, 0.75f, 2.0f,
                    SkinTransmissionScatterLane(profile), SkinTransmissionScalingLane(profile));

                // Version 0/1 profiles are filtered by the profile evaluator and
                // not by the lane one, which has no version to look at — the
                // lanes are packed unconditionally on the deferred path and the
                // SHADER holds that gate. So compare only where the profile
                // evaluator is meant to produce something.
                if (profile.EvaluationModel != SkinEvaluationModel::ThicknessTransmission)
                    continue;

                for (int c = 0; c < 3; ++c)
                    ASSERT_FLOAT_EQ(viaProfile[c], viaLanes[c]);
            }
        }
    }

} // namespace OloEngine::Tests
