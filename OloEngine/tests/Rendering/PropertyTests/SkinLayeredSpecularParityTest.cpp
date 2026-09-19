// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// SkinLayeredSpecularParityTest.cpp — the GLSL layered response IS the CPU one.
// Issue #1243.
//
// WHY THIS EXISTS BESIDE SkinLayeredSpecularTest. That test pins the CPU maths
// and its convexity argument. This one pins that the SHADER computes the same
// maths — and the two together are what make the arrangement
// Renderer/SkinLayeredSpecular.h describes actually load-bearing:
//
//   "EVERY PHYSICAL DECISION IN THIS FEATURE IS MADE HERE, ON THE CPU ... the
//    shader is handed two lobe widths, a mixture weight and a detail strength."
//
// A specification nothing is compared against is a comment. The probe
// (assets/shaders/tests/ShaderUnit_SkinLayeredSpecular.glsl) CALLS the
// production functions rather than transcribing them, so agreement here is
// agreement for all six of their real callers: PBR_MultiLight{,_Skinned}.glsl,
// include/ForwardPlusCommon.glsl, PBR_GBuffer{,_Skinned}.glsl and
// include/DeferredLightingShared.glsl.
//
// THE ASSERTION THAT MATTERS MOST is CaseFilterParity: the variance filter
// compared against SkinFilteredAlpha to a float tolerance, at an oblique
// derivative pair. Every other filter case is a PROPERTY (monotone here,
// clamped there), and a property can hold while the number is wrong by a factor
// of a thousand — which is exactly what a `2 *` dropped from the kernel, or an
// alpha/roughness confusion, would look like.
//
// THE SECOND MOST IMPORTANT is CaseLayeredDiffuseUntouched. That is the issue's
// FIRST acceptance criterion as a number: the layered specular must not erase
// the diffusion underneath, and the diffusion pass is fed from exactly the
// `Diffuse` half this case compares.
//
// Skips cleanly without a GL 4.6 context, like every probe fixture here.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The probe's column layout — mirrored from the shader's header, where
        // each case is named. An enum so a mismatch between the two is a
        // compile-visible name rather than a magic index.
        enum ProbeCase : u32
        {
            CaseFilterParity = 0,
            CaseFilterNeverSharpens = 1,
            CaseZeroStrengthExact = 2,
            CaseKernelClamped = 3,
            CaseMixIsConvex = 4,
            CaseZeroMixExact = 5,
            CaseFullMixIsBroad = 6,
            CaseBroadNeverNarrower = 7,
            CaseLayeredDiffuseUntouched = 8,
            CaseLayeredSpecularMoves = 9,
            CaseDetailZeroIsIdentity = 10,
            CaseDetailMinusOneIsCoarse = 11,
            CaseDetailPositiveDeepens = 12,
            CaseLobeGateWrongVersion = 13,
            CaseLobeGateRightVersion = 14,
            CaseCount
        };

        // THE FIXTURE, mirrored from the shader's literals.
        // TheProbeFixtureMatchesTheProfileItClaimsToBe below asserts that the
        // lane literals really are what the named profile packs — so the mirror
        // cannot drift silently, which is the standing risk with a hand-copied
        // fixture.
        constexpr f32 kAlpha = 0.16f;
        constexpr f32 kRoughness = 0.4f;
        constexpr f32 kVarianceStrength = 0.5f;

        constexpr glm::vec3 kdNdx{ 0.12f, -0.05f, 0.03f };
        constexpr glm::vec3 kdNdy{ -0.04f, 0.09f, -0.07f };

        constexpr f32 kLobeMix = 0.35f;
        constexpr f32 kLobeRoughnessScale = 2.0f;

        // THE TOLERANCE IS A PROPERTY OF THE FORMAT, not a number chosen until
        // the test passed, and the distinction matters because a parity test
        // whose tolerance was tuned to fit is not a parity test.
        //
        // The probe writes RGBA16F, which is a HALF float: 11 bits of mantissa,
        // so the representable spacing near 1 is 2^-11 ~= 4.9e-4. `kLobeMix`
        // 0.35 is not representable at all — the nearest half is 0.34985, which
        // is 1.46e-4 away, and that is exactly the discrepancy the first run of
        // this test reported. Nothing in the shader is wrong at that scale.
        //
        // RGBA16F rather than RGBA32F is itself deliberate: the harness comment
        // explains that several cases carry SIGNED differences, and a format
        // that clamps them would turn a real failure into a silent pass. Half is
        // the cheapest format that does not.
        //
        // SCALED BY THE MAGNITUDE, because half's spacing is RELATIVE: the
        // mixture cases below compare values near 8, where the quantum is eight
        // times the one near 1 — and the specular cases compare values near
        // 0.01, where it is a hundred times SMALLER.
        //
        // THE FLOOR IS 1e-5 AND NOT 1e-3, which is the whole point. The first
        // version of this helper floored at 1e-3 absolute, which is fine for a
        // value near 1 and absurd for one near 0.01 — there it is ten percent of
        // the quantity being compared. That made the layered-vs-unlayered
        // specular case (whose values ARE near 0.01) unable to assert anything:
        // a real 3% difference sat an order of magnitude below the "tolerance".
        // Surfaced by CodeRabbit on PR #1316, which pointed out that the same
        // case's EXPECT_NE would pass on a one-ULP step; tightening the
        // comparison as suggested is what exposed the floor underneath it.
        //
        // 1e-5 is still comfortably above the half spacing for every value these
        // cases produce, and far below any difference that means something.
        [[nodiscard]] f32 HalfTolerance(f32 magnitude)
        {
            constexpr f32 kHalfRelative = 1.0e-3f; // ~2x the 2^-11 spacing, for the
                                                   // two roundings a compare involves
            return std::max(1.0e-5f, std::abs(magnitude) * kHalfRelative);
        }

        struct ProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                // RGBA16F, not RGBA8. Several cases carry SIGNED differences
                // (the detail-normal cases return `detailed - reference`), which
                // an 8-bit target would clamp to 0 and turn into a silent pass —
                // the worst possible failure for a case whose whole job is to
                // notice a difference.
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("SkinLayeredSpecularProbe::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutputRgbaFloat(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), m_Width, m_Height, out);
            }
        };
    } // namespace

    // THE MIRROR CHECK, and it runs on the CPU alone so it cannot be skipped by
    // a headless machine. The probe hard-codes its lane rather than taking a
    // uniform (so a failure cannot be a bad bind); the price is that the
    // literals could drift from any real profile, at which point the GPU/CPU
    // comparison below would be comparing two answers to a question nobody
    // asks. This pins the literals to a profile.
    TEST(SkinLayeredSpecularParity, TheProbeFixtureMatchesTheProfileItClaimsToBe)
    {
        SkinProfileParameters profile{};
        profile.EvaluationModel = SkinEvaluationModel::LayeredSpecular;
        profile.Specular.LobeMix = kLobeMix;
        profile.Specular.LobeRoughnessScale = kLobeRoughnessScale;
        profile.Specular.NormalVarianceStrength = kVarianceStrength;
        ASSERT_TRUE(profile.Sanitize()) << "the probe fixture fell outside the authored bounds";

        const glm::vec4 lane = SkinSpecularLane(profile);
        EXPECT_NEAR(lane.x, kLobeMix, 1.0e-6f)
            << "ShaderUnit_SkinLayeredSpecular.glsl's kLane no longer matches what a real profile packs";
        EXPECT_NEAR(lane.y, kLobeRoughnessScale, 1.0e-6f);
        EXPECT_NEAR(lane.z, kVarianceStrength, 1.0e-6f);

        // And the probe's alpha really is the square of its roughness — the one
        // unit relationship this feature converts across, and the one an
        // alpha/roughness confusion would break in both files at once.
        EXPECT_NEAR(kAlpha, kRoughness * kRoughness, 1.0e-6f)
            << "the probe's kAlpha and kRoughness are no longer the same surface";
    }

    TEST(SkinLayeredSpecularParity, TheProductionShaderComputesTheCpuMaths)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ProbeHarness harness(static_cast<u32>(CaseCount), 1u,
                             "assets/shaders/tests/ShaderUnit_SkinLayeredSpecular.glsl");

        // THE SHADER COMPILED, ASKED BEFORE ANYTHING IS DRAWN. Shader::Create
        // hands back an OpenGLShader even when its status is Failed, and Bind()
        // on a non-Ready program returns without binding while Draw() carries on
        // regardless. Without this the probe would report whatever the no-shader
        // draw left in the attachment — a zero that looks exactly like a term
        // correctly returning zero.
        ASSERT_TRUE(harness.m_Shader) << "the probe shader was not created at all";
        ASSERT_TRUE(harness.m_Shader->IsReady())
            << "assets/shaders/tests/ShaderUnit_SkinLayeredSpecular.glsl did not compile — check OloEngine.log for "
               "the compiler diagnostic. Every assertion below would otherwise measure a frame no shader wrote.";

        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(CaseCount) * 4u);

        const auto texel = [&](ProbeCase c)
        {
            const std::size_t base = static_cast<std::size_t>(c) * 4u;
            return glm::vec4(pixels[base + 0u], pixels[base + 1u], pixels[base + 2u], pixels[base + 3u]);
        };

        // A readback is an external float boundary — validate before anything is
        // compared, or a NaN would make every EXPECT below quietly false rather
        // than reporting that the draw failed.
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(pixels[i]))
                << "probe texel " << i << " is non-finite — the draw or the readback failed, which makes nothing "
                                          "after this measurable.";
        }

        // ── The variance filter ─────────────────────────────────────────────

        // THE EQUALITY. Everything else about the filter is a property.
        const f32 cpuFiltered = SkinFilteredAlpha(kAlpha, glm::dot(kdNdx, kdNdx), glm::dot(kdNdy, kdNdy),
                                                  kVarianceStrength);
        EXPECT_NEAR(texel(CaseFilterParity).r, cpuFiltered, HalfTolerance(cpuFiltered))
            << "THE SHADER'S VARIANCE FILTER IS NOT THE CPU'S. Renderer/SkinLayeredSpecular.h calls itself the "
               "specification for this expression and include/SkinLayeredSpecular.glsl claims to transcribe it "
               "operation for operation. A dropped `2 *`, an alpha/roughness confusion or a reassociated sqrt all "
               "land here, and none of them is visible in a frame.";

        const glm::vec4 sharpens = texel(CaseFilterNeverSharpens);
        EXPECT_GE(sharpens.r, sharpens.g - 1.0e-6f)
            << "THE FILTER SHARPENED. It is a variance filter: the pixel's normal spread ADDS to the lobe's. A "
               "filter that narrows the lobe cannot remove aliasing — it manufactures it, and the surface will "
               "sparkle MORE at distance than it did unfiltered.";

        const glm::vec4 zeroStrength = texel(CaseZeroStrengthExact);
        EXPECT_FLOAT_EQ(zeroStrength.r, zeroStrength.g)
            << "A VARIANCE STRENGTH OF ZERO CHANGED THE ROUGHNESS. That value is the A/B control arm the sparkle "
               "evidence is measured against; if it is not an exact identity, 'filtering off' is a third variant "
               "rather than the baseline and every comparison built on it is meaningless.";

        const glm::vec4 clamped = texel(CaseKernelClamped);
        EXPECT_NEAR(clamped.r, clamped.g, HalfTolerance(clamped.g))
            << "THE KERNEL CLAMP IS NOT BINDING. Without it a silhouette pixel — where the shading normal swings "
               "most of a hemisphere between neighbours — widens without bound and the surface goes fully rough "
               "along every outline, which reads as a bright halo around everything in the scene.";
        EXPECT_LT(clamped.r, 1.0f);

        // ── The convex mixture ──────────────────────────────────────────────

        // Convexity, channel by channel, against the CPU's own mixture: the two
        // lobes differ in sign of (broad - narrow) across the three channels on
        // purpose, so a spelling that only works when broad > narrow fails here.
        const glm::vec3 narrow{ 2.0f, 3.0f, 4.0f };
        const glm::vec3 broad{ 8.0f, 1.0f, 4.0f };
        const glm::vec3 cpuMixed = SkinSpecularMix(narrow, broad, 0.25f);
        const glm::vec4 gpuMixed = texel(CaseMixIsConvex);
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(gpuMixed[c], cpuMixed[c], HalfTolerance(cpuMixed[c]))
                << "channel " << c
                << ": THE SHADER'S MIXTURE IS NOT THE CPU'S. The additive spelling (`narrow + w * broad`) agrees "
                   "with the convex one at w = 0 and nowhere else, and it is the spelling that quietly brightens "
                   "every head in the game.";
            EXPECT_LE(gpuMixed[c], std::max(narrow[c], broad[c]) + HalfTolerance(std::max(narrow[c], broad[c])))
                << "channel " << c << ": the mixture exceeded BOTH its lobes — energy out of nothing.";
            EXPECT_GE(gpuMixed[c], std::min(narrow[c], broad[c]) - HalfTolerance(std::min(narrow[c], broad[c])))
                << "channel " << c << ": the mixture fell below both its lobes — negative radiance is one step away.";
        }

        const glm::vec4 zeroMix = texel(CaseZeroMixExact);
        EXPECT_FLOAT_EQ(zeroMix.r, zeroMix.g)
            << "A LOBE MIX OF ZERO MOVED THE SPECULAR. That is the A/B control the fourth acceptance criterion's "
               "comparison is built on, and it has to be the pre-#1243 frame bit for bit — otherwise a golden "
               "image cannot tell a regression from the control arm.";
        EXPECT_FLOAT_EQ(zeroMix.b, 0.0f) << "the green channel of the zero-mix case also has to be exact";

        const glm::vec4 fullMix = texel(CaseFullMixIsBroad);
        EXPECT_NEAR(fullMix.r, fullMix.g, HalfTolerance(fullMix.g))
            << "a mix of 1 did not give the broad lobe — the two arguments are swapped, which case 5 alone would "
               "not have caught.";

        const glm::vec4 broadCase = texel(CaseBroadNeverNarrower);
        EXPECT_NEAR(broadCase.r, kRoughness * kLobeRoughnessScale, HalfTolerance(kRoughness * kLobeRoughnessScale));
        EXPECT_GT(broadCase.r, broadCase.g)
            << "the BROAD lobe is not broader than the narrow one — every comment downstream reasons about which "
               "of the two is which, so this is a naming failure as much as a numeric one.";
        EXPECT_LE(broadCase.b, 1.0f)
            << "the broad roughness left the NDF's domain; the clamp in oloSkinBroadRoughness is missing.";

        // ── The layered closure ─────────────────────────────────────────────

        const glm::vec4 diffuse = texel(CaseLayeredDiffuseUntouched);
        EXPECT_FLOAT_EQ(diffuse.r, diffuse.g)
            << "THE LAYERED CLOSURE MOVED THE DIFFUSE HALF. This is issue #1243's FIRST acceptance criterion as a "
               "number: the layered specular must not erase the diffusion underneath, and #1241's diffusion pass "
               "is fed from exactly this value. A second lobe that leaks into the diffuse half would blur an "
               "amount of energy that never went through the surface.";

        const glm::vec4 specular = texel(CaseLayeredSpecularMoves);
        // A MAGNITUDE THRESHOLD AND NOT EXPECT_NE. Both values ride in one
        // RGBA16F texel, so each is quantised to a half before it is read back;
        // EXPECT_NE would be satisfied by a one-ULP step that means nothing, and
        // this case exists precisely to rule out "the mixture is inert". The
        // threshold is the same format-derived one every other comparison here
        // uses. (CodeRabbit, PR #1316.)
        EXPECT_GT(std::abs(specular.r - specular.g), HalfTolerance(specular.g))
            << "the layered specular is IDENTICAL to the unlayered one, so the diffuse assertion above passed "
               "because the whole mixture is inert rather than because it is correctly confined.";
        // And it lies between the two lobes it mixes, measured on the GPU's own
        // two arms rather than against a CPU reconstruction of them.
        EXPECT_LE(specular.r, std::max(specular.g, specular.b) + HalfTolerance(std::max(specular.g, specular.b)));
        EXPECT_GE(specular.r, std::min(specular.g, specular.b) - HalfTolerance(std::min(specular.g, specular.b)));

        // ── The expression-driven detail band ───────────────────────────────

        const glm::vec4 detailZero = texel(CaseDetailZeroIsIdentity);
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_FLOAT_EQ(detailZero[c], 0.0f)
                << "channel " << c
                << ": A DETAIL STRENGTH OF ZERO CHANGED THE NORMAL. Zero is the neutral default every existing "
                   "skin material will load with, so this is the difference between 'the feature is opt-in' and "
                   "'every head in every scene just moved'.";
        }

        const glm::vec4 detailOff = texel(CaseDetailMinusOneIsCoarse);
        for (int c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(detailOff[c], 0.0f, HalfTolerance(1.0f))
                << "channel " << c
                << ": a detail strength of -1 did not return the COARSE normal. That is the exact 'detail off' arm "
                   "the fourth acceptance criterion's AOV comparison subtracts against; 'smoother' is not good "
                   "enough, because then the difference image contains the error as well as the signal.";
        }

        const glm::vec4 deepen = texel(CaseDetailPositiveDeepens);
        EXPECT_GT(deepen.r, deepen.g)
            << "a POSITIVE detail strength did not push the normal further from the coarse one than the authored "
               "normal already is — which is what 'deepen the pores' means as an inequality. The sign of the "
               "difference term is inverted.";

        // ── The version gate ────────────────────────────────────────────────

        const glm::vec4 wrongVersion = texel(CaseLobeGateWrongVersion);
        EXPECT_FLOAT_EQ(wrongVersion.r, 0.0f)
            << "A PROFILE BELOW TRANSPORT VERSION 3 ACQUIRED A SECOND LOBE. The version exists so a profile stays "
               "where its author left it; a head authored against #1242 must not change appearance because this "
               "build shipped.";
        EXPECT_FLOAT_EQ(wrongVersion.g, 1.0f) << "the neutral lobe scale must be 1, not 0";
        EXPECT_FLOAT_EQ(wrongVersion.b, 0.0f)
            << "a NON-SKIN material acquired a lobe — the kind test is missing, and every terrain highlight in the "
               "scene is now being mixed with somebody's skin profile.";

        const glm::vec4 rightVersion = texel(CaseLobeGateRightVersion);
        EXPECT_NEAR(rightVersion.r, kLobeMix, HalfTolerance(kLobeMix))
            << "a version-3 skin pixel did NOT get its authored lobe, so the gate assertions above are passing by "
               "being uniformly closed rather than by discriminating.";
        EXPECT_NEAR(rightVersion.g, kLobeRoughnessScale, HalfTolerance(kLobeRoughnessScale));
    }

} // namespace OloEngine::Tests
