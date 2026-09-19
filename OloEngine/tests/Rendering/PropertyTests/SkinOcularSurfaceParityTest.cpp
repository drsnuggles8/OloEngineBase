// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// SkinOcularSurfaceParityTest.cpp — the GLSL eye IS the CPU one. Issue #1244.
//
// WHY THIS EXISTS BESIDE SkinOcularSurfaceTest. That test pins the CPU maths
// and, crucially, its agreement with an independent two-surface ray trace. This
// one pins that the SHADER computes the same maths — and the two together are
// what make the arrangement Renderer/SkinOcularSurface.h describes actually
// load-bearing:
//
//   "THE MATHS LIVES HERE AND THE SHADER TRANSCRIBES IT ... a physical decision
//    that only exists inside a .glsl file cannot be unit tested."
//
// A specification nothing is compared against is a comment. The probe
// (assets/shaders/tests/ShaderUnit_SkinOcularSurface.glsl) CALLS the production
// functions rather than transcribing them, so agreement here is agreement for
// all four of their real callers: PBR_MultiLight{,_Skinned}.glsl and
// PBR_GBuffer{,_Skinned}.glsl. Note that the list is four MATERIAL stages and
// not three lighting paths — which is the structural point of this feature and
// the reason the deferred lighting pass appears nowhere in it.
//
// THE ASSERTION THAT MATTERS MOST is CaseApplyParity: the whole
// oloSkinOcularApply compared against ApplySkinOcularSurface for a full
// version-5 profile. Every other case is one function, and one function can be
// right while the TWELVE LANE COMPONENTS are wired into the wrong slots — which
// is exactly the failure #1288 is the receipt for, and with three lanes instead
// of one the surface area for it tripled.
//
// THE SECOND MOST IMPORTANT is CaseRefractionMovesTheIris. Every case above it
// can pass on a shader that computes a beautiful refraction and then discards
// it; that one asserts the iris actually MOVED, on the GPU, in the direction
// the cornea's magnification requires.
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
#include "OloEngine/Renderer/SkinOcularSurface.h"
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
            CaseRefractParity = 0,
            CaseRefractBendsTowardNormal = 1,
            CaseRefractReportsTir = 2,
            CaseCornealNormalParity = 3,
            CaseCornealNormalRatioOneExact = 4,
            CaseCornealNormalIsSteeper = 5,
            CaseIrisPlaneHitParity = 6,
            CaseIrisPlaneOutwardRefused = 7,
            CaseLimbalRingParity = 8,
            CaseLimbalRingZeroExact = 9,
            CasePupilParity = 10,
            CasePupilCentreIsBlack = 11,
            CaseDishTiltVanishesAtEdge = 12,
            CaseApplyParity = 13,
            CaseZeroMasterExact = 14,
            CaseScleraUntouched = 15,
            CaseRefractionMovesTheIris = 16,
            CaseCount
        };

        // THE FIXTURE, mirrored from the shader's literals.
        // TheProbeFixtureMatchesTheProfileItClaimsToBe below asserts that these
        // really are what the named profile packs — so the mirror cannot drift
        // silently, which is the standing risk with a hand-copied fixture and a
        // much larger one with twelve components than with four.
        constexpr f32 kRingStrength = 0.7f;
        constexpr f32 kPupilDarkening = 1.0f;
        constexpr f32 kConcavity = 0.3f;
        const glm::vec3 kIrisColor(0.32f, 0.46f, 0.55f);

        const glm::vec3 kAxis(0.0f, 0.0f, 1.0f);
        // 18 degrees off the optical axis, inside the 29.2-degree limbus.
        const glm::vec3 kObliqueN(0.309017f, 0.0f, 0.951057f);
        const glm::vec3 kProbeAlbedo(0.42f, 0.33f, 0.27f);

        [[nodiscard]] glm::vec3 ProbeView()
        {
            return glm::normalize(glm::vec3(0.12f, -0.08f, 1.0f));
        }

        // The profile the probe's literals describe: the shipped defaults with
        // the eye on and a ring authored.
        [[nodiscard]] SkinProfileParameters ProbeProfile()
        {
            SkinProfileParameters p{};
            p.EvaluationModel = SkinEvaluationModel::OcularSurface;
            p.Ocular.OcularStrength = 1.0f;
            p.Ocular.RefractionStrength = 1.0f;
            p.Ocular.LimbalRingStrength = kRingStrength;
            p.Ocular.PupilDarkening = kPupilDarkening;
            p.Ocular.IrisConcavity = kConcavity;
            p.Ocular.IrisColor = kIrisColor;
            return p;
        }

        // THE TOLERANCE IS A PROPERTY OF THE FORMAT, not a number chosen until
        // the test passed — the same reasoning, and the same helper, as
        // SkinOralSurfaceParityTest. The probe writes RGBA16F, whose mantissa is
        // 11 bits, so the relative spacing is about 2^-11; the factor of two
        // covers the two roundings a comparison involves, and the 1e-5 floor
        // keeps the bound meaningful for the near-zero values the difference
        // cases produce.
        [[nodiscard]] f32 HalfTolerance(f32 magnitude)
        {
            constexpr f32 kHalfRelative = 1.0e-3f;
            return std::max(1.0e-5f, std::abs(magnitude) * kHalfRelative);
        }

        struct ProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ProbeHarness(u32 width, u32 height, const char* shaderPath) : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                // RGBA16F, not RGBA8. Several cases carry SIGNED differences
                // (the corneal normal's delta from its input, the zero-master
                // albedo delta), which an 8-bit target would clamp to 0 and
                // turn into a silent pass — the worst possible failure for a
                // case whose whole job is to notice a difference. The radial
                // sentinel is negative for the same reason.
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("SkinOcularSurfaceProbe::Draw", GLStateGuard::Policy::Restore);
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
    // a headless machine. The probe hard-codes its lanes rather than taking a
    // uniform (so a failure cannot be a bad bind); the price is that the
    // literals could drift from any real profile, at which point the GPU/CPU
    // comparison below would be comparing two answers to a question nobody
    // asks. This pins the literals to a profile.
    //
    // ALL TWELVE COMPONENTS, not a sample of them. Three lanes are three
    // opportunities for a component to end up one slot over, and a fixture
    // check that spot-tests four of twelve would miss two thirds of them.
    TEST(SkinOcularSurfaceParity, TheProbeFixtureMatchesTheProfileItClaimsToBe)
    {
        SkinProfileParameters profile = ProbeProfile();
        ASSERT_TRUE(profile.Sanitize()) << "the probe fixture fell outside the authored bounds";

        const glm::vec4 cornea = SkinOcularCorneaLane(profile);
        EXPECT_NEAR(cornea.x, 1.0f / 1.336f, 1.0e-6f)
            << "ShaderUnit_SkinOcularSurface.glsl's kEta no longer matches what a real profile packs";
        EXPECT_NEAR(cornea.y, 12.0f / 7.8f, 1.0e-6f) << "kCurvatureRatio drifted";
        EXPECT_NEAR(cornea.z, 2.48f / 12.0f, 1.0e-6f) << "kIrisPlaneDepth drifted";
        EXPECT_NEAR(cornea.w, 0.8731230f, 1.0e-5f) << "kLimbusCos drifted";

        const glm::vec4 iris = SkinOcularIrisLane(profile);
        EXPECT_NEAR(iris.x, 5.85f / 12.0f, 1.0e-6f) << "kIrisRadius drifted";
        EXPECT_NEAR(iris.y, 2.0f / 5.85f, 1.0e-6f) << "kPupilRadial drifted";
        EXPECT_NEAR(iris.z, 0.7f / 5.85f, 1.0e-6f) << "kRingWidth drifted";
        EXPECT_NEAR(iris.w, 1.0f, 1.0e-6f) << "the probe's master switch is not on";

        const glm::vec4 response = SkinOcularResponseLane(profile);
        EXPECT_NEAR(response.x, kRingStrength, 1.0e-6f);
        EXPECT_NEAR(response.y, kPupilDarkening, 1.0e-6f);
        EXPECT_NEAR(response.z, kConcavity, 1.0e-6f);
        EXPECT_NEAR(response.w, 1.0f, 1.0e-6f);

        const glm::vec4 tint = SkinOcularTintLane(profile);
        EXPECT_NEAR(tint.x, kIrisColor.r, 1.0e-6f) << "kIrisColor drifted";
        EXPECT_NEAR(tint.y, kIrisColor.g, 1.0e-6f);
        EXPECT_NEAR(tint.z, kIrisColor.b, 1.0e-6f);
        EXPECT_NEAR(tint.w, 0.7f / 5.85f, 1.0e-6f) << "kIrisEdgeBand drifted";

        // And the oblique point really is inside the limbus, which every
        // refraction case silently depends on: outside it the production code
        // returns early and every case below would pass on an untouched input.
        EXPECT_GT(glm::dot(kObliqueN, kAxis), cornea.w)
            << "the probe's surface point is no longer on the cornea; every refraction case "
               "would pass by returning its input";
    }

    TEST(SkinOcularSurfaceParity, TheProductionShaderComputesTheCpuMaths)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ProbeHarness harness(static_cast<u32>(CaseCount), 1u,
                             "assets/shaders/tests/ShaderUnit_SkinOcularSurface.glsl");

        // THE SHADER COMPILED, ASKED BEFORE ANYTHING IS DRAWN. Shader::Create
        // hands back an OpenGLShader even when its status is Failed, and Bind()
        // on a non-Ready program returns without binding while Draw() carries on
        // regardless. Without this the probe would report whatever the
        // no-shader draw left in the attachment — a zero that looks exactly
        // like a term correctly evaluating to zero.
        ASSERT_TRUE(harness.m_Shader) << "the probe shader failed to load";

        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(CaseCount) * 4u);

        const auto at = [&pixels](ProbeCase c) -> glm::vec4
        {
            const sizet base = static_cast<sizet>(c) * 4u;
            return glm::vec4(pixels[base], pixels[base + 1], pixels[base + 2], pixels[base + 3]);
        };

        // Every case wrote alpha 1 (or, for the boolean cases, a defined
        // value). A column of pure zeroes means the fragment shader never ran
        // that branch, which is a different failure from a term being zero and
        // must not be mistaken for one.
        for (u32 c = 0; c < static_cast<u32>(CaseCount); ++c)
        {
            const glm::vec4 v = at(static_cast<ProbeCase>(c));
            EXPECT_TRUE(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w))
                << "case " << c << " produced a non-finite value on the GPU";
        }

        const SkinProfileParameters profile = ProbeProfile();
        const glm::vec4 corneaLane = SkinOcularCorneaLane(profile);
        const glm::vec4 irisLane = SkinOcularIrisLane(profile);
        const glm::vec4 responseLane = SkinOcularResponseLane(profile);
        const glm::vec4 tintLane = SkinOcularTintLane(profile);
        const glm::vec3 view = ProbeView();

        // --- 0: refract parity, the equality case ---------------------------
        {
            glm::vec3 expected{};
            ASSERT_TRUE(SkinOcularRefract(-view, kObliqueN, corneaLane.x, expected));
            const glm::vec4 got = at(CaseRefractParity);
            EXPECT_GT(got.w, 0.5f) << "the shader reported no refraction where the CPU found one";
            EXPECT_NEAR(got.x, expected.x, HalfTolerance(expected.x));
            EXPECT_NEAR(got.y, expected.y, HalfTolerance(expected.y));
            EXPECT_NEAR(got.z, expected.z, HalfTolerance(expected.z));
        }

        // --- 1: it bends TOWARD the normal ----------------------------------
        {
            const glm::vec4 got = at(CaseRefractBendsTowardNormal);
            EXPECT_GT(got.z, 0.5f)
                << "the refracted ray did not bend toward the normal: cos after " << got.x
                << " vs before " << got.y << ". An inverted eta produces exactly this.";
            EXPECT_GT(got.x, got.y);
        }

        // --- 2: total internal reflection is REPORTED -----------------------
        {
            const glm::vec4 got = at(CaseRefractReportsTir);
            EXPECT_LT(got.x, 0.5f) << "the shader claimed a refraction where none exists";
        }

        // --- 3, 4, 5: the corneal normal ------------------------------------
        {
            const glm::vec3 expected = SkinCornealNormal(kObliqueN, kAxis, corneaLane.y);
            const glm::vec4 got = at(CaseCornealNormalParity);
            EXPECT_NEAR(got.x, expected.x, HalfTolerance(expected.x));
            EXPECT_NEAR(got.y, expected.y, HalfTolerance(expected.y));
            EXPECT_NEAR(got.z, expected.z, HalfTolerance(expected.z));

            const glm::vec4 identity = at(CaseCornealNormalRatioOneExact);
            EXPECT_NEAR(glm::length(glm::vec3(identity)), 0.0f, 1.0e-5f)
                << "a curvature ratio of 1 no longer returns its input, so a mesh with real "
                   "corneal geometry cannot opt out of the bend";

            const glm::vec4 steeper = at(CaseCornealNormalIsSteeper);
            EXPECT_LT(steeper.x, steeper.y)
                << "the bent normal is not further from the axis than the globe normal, so the "
                   "bend is inert and case 3 is passing on a no-op";
        }

        // --- 6, 7: the march to the iris plane ------------------------------
        {
            glm::vec3 refracted{};
            ASSERT_TRUE(SkinOcularRefract(-view, SkinCornealNormal(kObliqueN, kAxis, corneaLane.y),
                                          corneaLane.x, refracted));
            glm::vec3 expected{};
            ASSERT_TRUE(SkinIrisPlaneHit(kObliqueN, kAxis, refracted, corneaLane.z, expected));

            const glm::vec4 got = at(CaseIrisPlaneHitParity);
            EXPECT_GT(got.w, 0.5f);
            EXPECT_NEAR(got.x, expected.x, HalfTolerance(expected.x));
            EXPECT_NEAR(got.y, expected.y, HalfTolerance(expected.y));
            EXPECT_NEAR(got.z, expected.z, HalfTolerance(expected.z));

            EXPECT_LT(at(CaseIrisPlaneOutwardRefused).x, 0.5f)
                << "a ray travelling away from the iris was solved for anyway, which puts the "
                   "iris in front of the eye";
        }

        // --- 8, 9: the limbal ring ------------------------------------------
        {
            // At 0.95 with a band of 0.7/5.85 = 0.1197 the ring is past its
            // inner edge (1 - 2*band = 0.76) and at its outer one (1 - band =
            // 0.88), so it is at FULL strength — which is a duller case than it
            // looks and is why the parity assertion below is paired with the
            // exactness case beside it.
            const f32 expected = SkinIrisLimbalRing(0.95f, irisLane.z, kRingStrength);
            EXPECT_NEAR(at(CaseLimbalRingParity).x, expected, HalfTolerance(expected));
            EXPECT_EQ(at(CaseLimbalRingZeroExact).x, 1.0f)
                << "a zero ring strength is no longer exactly 1 on the GPU, so the A/B control "
                   "arm is a third variant rather than a baseline";
        }

        // --- 10, 11: the pupil ------------------------------------------------
        {
            const f32 expected = SkinIrisPupilMask(irisLane.y * 0.98f, irisLane.y, kPupilDarkening);
            EXPECT_NEAR(at(CasePupilParity).x, expected, HalfTolerance(expected));

            const glm::vec4 ends = at(CasePupilCentreIsBlack);
            EXPECT_NEAR(ends.x, 0.0f, 1.0e-4f) << "the pupil centre is not black on the GPU";
            EXPECT_NEAR(ends.y, 1.0f, 1.0e-4f) << "the iris edge is not untouched on the GPU";
        }

        // --- 12: the dish tilt vanishes at the iris edge ----------------------
        {
            EXPECT_NEAR(glm::length(glm::vec3(at(CaseDishTiltVanishesAtEdge))), 0.0f, 1.0e-4f)
                << "the dish tilt no longer vanishes at the iris edge on the GPU; the limbus "
                   "will carry a hard ring";
        }

        // --- 13: THE WHOLE APPLY --------------------------------------------
        //
        // The case that fails if any of the twelve lane components is in the
        // wrong slot, which is the #1288 failure shape with three times the
        // surface area.
        {
            const SkinOcularResult expected =
                ApplySkinOcularSurface(kProbeAlbedo, kObliqueN, view, kAxis, corneaLane, irisLane,
                                       responseLane, tintLane);
            const glm::vec4 got = at(CaseApplyParity);
            EXPECT_NEAR(got.x, expected.Albedo.r, HalfTolerance(expected.Albedo.r));
            EXPECT_NEAR(got.y, expected.Albedo.g, HalfTolerance(expected.Albedo.g));
            EXPECT_NEAR(got.z, expected.Albedo.b, HalfTolerance(expected.Albedo.b));
            EXPECT_NEAR(got.w, expected.IrisRadial, HalfTolerance(expected.IrisRadial))
                << "the shader resolved a different iris point from the CPU";
        }

        // --- 14, 15: the two untouched arms ----------------------------------
        {
            const glm::vec4 neutral = at(CaseZeroMasterExact);
            EXPECT_EQ(glm::vec3(neutral), glm::vec3(0.0f))
                << "a zero master no longer returns its albedo bit for bit on the GPU";
            EXPECT_LT(neutral.w, 0.0f) << "a neutral profile reported an iris radial";

            const glm::vec4 sclera = at(CaseScleraUntouched);
            EXPECT_EQ(glm::vec3(sclera), glm::vec3(0.0f))
                << "sclera is being shaded by the ocular block; an eye's white must stay a skin term";
            EXPECT_LT(sclera.w, 0.0f);
        }

        // --- 16: THE FEATURE, on the GPU, in one texel ------------------------
        {
            const glm::vec4 got = at(CaseRefractionMovesTheIris);
            EXPECT_GT(got.z, 0.0f)
                << "the refracted iris sample is not nearer the axis than the painted one "
                   "(refracted "
                << got.x << ", painted " << got.y << "). The cornea magnifies, "
                                                     "so this sign is a physical claim.";
            EXPECT_GT(got.z, 0.01f)
                << "the refraction moves the iris by less than 1% of its radius on the GPU; "
                   "the shader is computing a refraction and then discarding it";
        }
    }

} // namespace OloEngine::Tests
