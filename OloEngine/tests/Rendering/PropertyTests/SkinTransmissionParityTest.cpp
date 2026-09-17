// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// SkinTransmissionParityTest.cpp — the GLSL transmission term IS the CPU one.
// Issue #1242.
//
// WHY THIS EXISTS BESIDE SkinTransmissionTest. That test pins the CPU maths and
// its three-premise energy argument. This one pins that the SHADER computes the
// same maths — and the two together are what make the arrangement
// Renderer/SkinTransmission.h describes actually load-bearing:
//
//   "THIS FUNCTION IS THE SPECIFICATION AND ALSO A RUNTIME PATH ... the GLSL in
//    include/SkinTransmission.glsl is a transcription of this expression in
//    this order."
//
// A specification nothing is compared against is a comment. The probe
// (assets/shaders/tests/ShaderUnit_SkinTransmission.glsl) CALLS the production
// functions rather than transcribing them, so agreement here is agreement for
// every one of their five real callers: PBR_MultiLight{,_Skinned}.glsl,
// PBR_GBuffer{,_Skinned}.glsl and DeferredLightingShared.glsl.
//
// THE ONE ASSERTION THAT MATTERS MOST is CaseCpuParity: an oblique, partly
// shadowed configuration compared against EvaluateSkinTransmissionLanes to a
// float tolerance. Every other case is a PROPERTY (zero here, monotone there),
// and a property can hold while the number is wrong by a factor of a thousand —
// which is exactly the unit slip this feature's maths is most exposed to.
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
#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinTransmission.h"

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
            CaseBacklitLit = 0,
            CaseBacklitShadowed = 1,
            CaseFrontLit = 2,
            CaseTangent = 3,
            CaseZeroThickness = 4,
            CaseThickerTransmitsLess = 5,
            CaseRedOutlivesBlue = 6,
            CaseShadowNormalBacklit = 7,
            CaseShadowNormalFrontlit = 8,
            CaseGBufferLaneRoundTrip = 9,
            CaseGBufferLaneDefersToLightmap = 10,
            CaseCpuParity = 11,
            CaseCount
        };

        // THE FIXTURE, mirrored from the shader's literals. Every value here has
        // a twin in ShaderUnit_SkinTransmission.glsl, and
        // TheProbeFixtureMatchesTheProfileItClaimsToBe below asserts that the
        // lane literals really are what the named profile packs — so the mirror
        // cannot drift silently, which is the standing risk with a hand-copied
        // fixture.
        constexpr glm::vec4 kScatterLane{ 0.90f, 0.55f, 0.40f, 0.7f };
        constexpr glm::vec4 kScalingLane{ 2.0f, 1.1f, 0.8f, 4.0f };

        constexpr glm::vec3 kNormal{ 0.0f, 0.0f, 1.0f };
        constexpr glm::vec3 kView{ 0.0f, 0.0f, 1.0f };
        constexpr glm::vec3 kLightBehind{ 0.0f, 0.0f, -1.0f };
        constexpr glm::vec3 kLightInFront{ 0.0f, 0.0f, 1.0f };

        constexpr glm::vec3 kAlbedo{ 0.62f, 0.48f, 0.42f };
        constexpr glm::vec3 kRadiance{ 3.0f, 3.0f, 3.0f };
        constexpr f32 kThicknessMM = 2.0f;

        struct SkinProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            SkinProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                // RGBA16F, not RGBA8: the term is linear HDR radiance, the
                // normal dots are SIGNED, and the G-Buffer lane case carries a
                // thickness in MILLIMETRES that can reach 2000. An 8-bit target
                // would clamp the -1 the shadow-normal case looks for to 0 and
                // turn it into a silent pass, and would quantise the parity
                // case to nothing.
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("SkinProbeHarness::Draw", GLStateGuard::Policy::Restore);
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
    TEST(SkinTransmissionParity, TheProbeFixtureMatchesTheProfileItClaimsToBe)
    {
        SkinProfileParameters profile{};
        profile.EvaluationModel = SkinEvaluationModel::ThicknessTransmission;
        profile.ScatterColor = glm::vec3(0.90f, 0.55f, 0.40f);
        profile.Transmission.Strength = 1.0f;
        profile.Transmission.Anisotropy = 0.7f;
        profile.Transmission.Power = 4.0f;
        // The radii that make SkinBurleyScalingMM come out at the shader's
        // kScaling — solved backwards from the shape fit rather than guessed, so
        // this test says what the probe's `d` means instead of asserting a
        // number nobody can derive.
        const glm::vec3 shape{ SkinBurleyShapeFromAlbedo(profile.ScatterColor.x),
                               SkinBurleyShapeFromAlbedo(profile.ScatterColor.y),
                               SkinBurleyShapeFromAlbedo(profile.ScatterColor.z) };
        profile.ScatterRadiusMM = glm::vec3(kScalingLane) * shape;
        ASSERT_TRUE(profile.Sanitize()) << "the derived radii fell outside the authored bounds";

        const glm::vec4 scatter = SkinTransmissionScatterLane(profile);
        const glm::vec4 scaling = SkinTransmissionScalingLane(profile);

        for (int c = 0; c < 4; ++c)
        {
            EXPECT_NEAR(scatter[c], kScatterLane[c], 1.0e-5f)
                << "lane component " << c
                << ": ShaderUnit_SkinTransmission.glsl's kScatter no longer matches what a real profile packs";
            EXPECT_NEAR(scaling[c], kScalingLane[c], 1.0e-4f)
                << "lane component " << c
                << ": ShaderUnit_SkinTransmission.glsl's kScaling no longer matches what a real profile packs";
        }
    }

    TEST(SkinTransmissionParity, TheProductionShaderComputesTheCpuTerm)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SkinProbeHarness harness(static_cast<u32>(CaseCount), 1u,
                                 "assets/shaders/tests/ShaderUnit_SkinTransmission.glsl");

        // THE SHADER COMPILED, ASKED BEFORE ANYTHING IS DRAWN. Shader::Create
        // hands back an OpenGLShader even when FinalizeGL set its status to
        // Failed, and Bind() on a non-Ready program returns without binding
        // while Draw() carries on regardless. Without this the probe would
        // report whatever the no-shader draw left in the attachment — a zero
        // that looks exactly like a term correctly returning zero — and the
        // failure would read as a shading bug rather than as "the probe did not
        // compile". Same trap as Texture2D::Create; see
        // docs/agent-rules/notes-renderer.md.
        ASSERT_TRUE(harness.m_Shader) << "the probe shader was not created at all";
        ASSERT_TRUE(harness.m_Shader->IsReady())
            << "assets/shaders/tests/ShaderUnit_SkinTransmission.glsl did not compile — check OloEngine.log for "
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

        // ── Premise 1: disjoint incident directions ─────────────────────────

        const glm::vec4 backlit = texel(CaseBacklitLit);
        EXPECT_GT(backlit.r, 0.0f)
            << "a backlit thin region transmits nothing at all — the lobe is dead in the shader, and a head against "
               "the sun will read exactly as it did before #1242.";

        EXPECT_FLOAT_EQ(texel(CaseFrontLit).r, 0.0f)
            << "FRONT LIGHTING TRANSMITTED. This is premise 1 of the energy argument: the reflected diffuse lobe "
               "already counts every light on the near side, so transmitting as well double-counts it — and the "
               "frame it produces is the uniformly emissive head #1242's second criterion forbids.";

        EXPECT_FLOAT_EQ(texel(CaseTangent).r, 0.0f)
            << "an exactly tangent light transmitted — a `< 0` test where a `<= 0` one was meant, which puts a "
               "bright rim on every silhouette.";

        // ── The shared visibility gate ──────────────────────────────────────

        const glm::vec4 shadowed = texel(CaseBacklitShadowed);
        EXPECT_FLOAT_EQ(shadowed.r, 0.0f)
            << "TRANSMISSION SURVIVED A VISIBILITY OF ZERO. #1242's second criterion rules this out by name: the "
               "term is not an unshadowed constant, so an ear behind a raised hand must stop glowing. (The lit arm "
               "of the same geometry measured " << shadowed.g << ".)";
        EXPECT_GT(shadowed.g, 0.0f)
            << "the LIT arm of the shadow case is also zero, so the assertion above passed on a dead lobe rather "
               "than on a working gate.";

        // ── The conservative fallback ───────────────────────────────────────

        EXPECT_FLOAT_EQ(texel(CaseZeroThickness).r, 0.0f)
            << "a ZERO thickness transmitted. The shader is reading it as 'infinitely thin, so exp(0) = 1, so fully "
               "transparent' instead of as 'no volume authored' — which is the uniformly emissive head.";

        // ── The profile's own physics ───────────────────────────────────────

        const glm::vec4 thicknessPair = texel(CaseThickerTransmitsLess);
        EXPECT_LT(thicknessPair.g, thicknessPair.r)
            << "four times the thickness transmitted at least as much — a sign slip in the exponent, which renders "
               "a head whose THICKEST parts glow most.";

        const glm::vec4 transmittance = texel(CaseRedOutlivesBlue);
        EXPECT_GT(transmittance.r, transmittance.g);
        EXPECT_GT(transmittance.g, transmittance.b)
            << "the per-channel mean free paths are not reaching the transmittance, so the glow is grey — which is "
               "the difference between skin and wax, and is carried by the profile rather than by a tint.";

        // ── The shadow normal ───────────────────────────────────────────────

        const glm::vec4 shadowNormalBacklit = texel(CaseShadowNormalBacklit);
        EXPECT_GT(shadowNormalBacklit.r, 0.0f)
            << "oloSkinShadowNormal did not return the LIT-SIDE normal for a backlit pixel, so the cascade's "
               "receiver offset walks the shadow sample into the head's own depth and the term goes black. The "
               "symptom is 'backlit ears do not glow' and the cause is the bias sign.";
        EXPECT_FLOAT_EQ(shadowNormalBacklit.g, -1.0f) << "the backlit shadow normal should be exactly -N";

        const glm::vec4 shadowNormalLit = texel(CaseShadowNormalFrontlit);
        EXPECT_FLOAT_EQ(shadowNormalLit.r, 1.0f)
            << "oloSkinShadowNormal changed the normal for a FRONT-lit pixel. That breaks the property that lets "
               "ONE shadow lookup serve both lobes, and it silently retunes the reflected lobe's bias on every "
               "skin surface in the scene.";

        // ── The deferred thickness lane ─────────────────────────────────────

        const glm::vec4 roundTrip = texel(CaseGBufferLaneRoundTrip);
        EXPECT_NEAR(roundTrip.r, kThicknessMM, 1.0e-3f)
            << "the RT5 thickness lane did not round-trip: packed " << kThicknessMM << " mm, read back "
            << roundTrip.r << " mm. The whole deferred path's per-pixel thickness goes through this pair.";
        EXPECT_FLOAT_EQ(roundTrip.g, 0.0f)
            << "the packer wrote a non-zero COVERAGE, which makes the ambient ladder read this pixel as lightmapped "
               "and take its indirect light from a thickness.";

        const glm::vec4 deferred = texel(CaseGBufferLaneDefersToLightmap);
        EXPECT_FLOAT_EQ(deferred.r, 0.0f)
            << "a LIGHTMAPPED pixel reported a thickness — the reader is about to treat baked irradiance as "
               "millimetres, which is a bright transmission fringe along every such edge.";
        EXPECT_FLOAT_EQ(deferred.g, 0.25f)
            << "the packer OVERWROTE a lightmapped pixel's irradiance. That trades a visible lighting regression "
               "for a subtle transmission gain — the wrong way round.";
        EXPECT_FLOAT_EQ(deferred.a, 1.0f) << "the lightmapped pixel's coverage must survive untouched";

        // ── THE TRANSCRIPTION ITSELF ────────────────────────────────────────

        // The one equality in this file. Every assertion above is a property
        // that would still hold if the shader were off by a constant factor —
        // including a factor of 1000, which is precisely the unit slip this
        // feature is most exposed to.
        //
        // The configuration mirrors the probe's CaseCpuParity: oblique and
        // partly shadowed, because an axis-aligned case makes the lobe's `pow`
        // exactly 1 or 0 (passing with the anisotropy term missing) and a
        // visibility of 1 passes with the multiply dropped.
        const glm::vec3 lightDir = glm::normalize(glm::vec3(0.35f, 0.20f, -0.85f));
        const glm::vec3 view = glm::normalize(glm::vec3(0.10f, -0.15f, 1.0f));
        const glm::vec3 expected = EvaluateSkinTransmissionLanes(kNormal, view, lightDir, kRadiance, kAlbedo, 0.6f,
                                                                  kThicknessMM, kScatterLane, kScalingLane);
        const glm::vec4 measured = texel(CaseCpuParity);

        ASSERT_GT(expected.r, 0.0f)
            << "the CPU expectation is zero, so this comparison would pass against a shader that computes nothing. "
               "Fix the fixture, not the tolerance.";

        // THE TOLERANCE IS THE TARGET FORMAT'S, not a taste number.
        //
        // The probe writes to RGBA16F, so every measured value has already been
        // rounded to a HALF float — 10 explicit mantissa bits, i.e. a relative
        // spacing of 2^-11 = 4.9e-4. The first run of this test failed at a 1e-4
        // relative tolerance with the shader reporting 0.2427978515625 against
        // the CPU's 0.24285490810871124: that measured value is EXACTLY
        // representable as a half, and the gap is under half the half-float
        // spacing at that magnitude. It was the framebuffer, not the maths.
        //
        // Two ULPs of half precision covers that rounding plus the shader's own
        // accumulated chain, and is still three orders of magnitude tighter than
        // any transcription error this test exists to catch: a dropped factor, a
        // reassociated product or a millimetre/metre slip all move the answer by
        // percent or by 1000x, not by 1e-3.
        //
        // Deliberately NOT fixed by widening until green — the number below is
        // derived from the attachment format, and if the probe's target ever
        // becomes RGBA32F this should tighten rather than stay.
        constexpr f32 kHalfFloatRelativeUlp = 4.883e-4f; // 2^-11
        constexpr f32 kToleranceUlps = 2.0f;

        for (int c = 0; c < 3; ++c)
        {
            const f32 tolerance = std::max(kToleranceUlps * kHalfFloatRelativeUlp * std::abs(expected[c]), 1.0e-6f);
            EXPECT_NEAR(measured[c], expected[c], tolerance)
                << "channel " << c << ": the shader computes " << measured[c] << " where "
                << "EvaluateSkinTransmissionLanes computes " << expected[c]
                << ". include/SkinTransmission.glsl is no longer a faithful transcription of "
                   "Renderer/SkinTransmission.h — check the multiply ORDER before checking the maths, since the "
                   "two are documented to be operation-for-operation identical.";
        }
    }

} // namespace OloEngine::Tests
