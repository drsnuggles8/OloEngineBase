// OLO_TEST_LAYER: L2
// =============================================================================
// LightingSignalContractGpuTest.cpp — the production composition and ambient
// ladder, against independently derived answers (issue #1336).
//
// The probe (assets/shaders/tests/ShaderUnit_LightingSignalContract.glsl)
// CALLS the functions every raster path composes a lit surface with. The
// expected values here are NOT read off those functions: they are what the
// physics says the answer must be —
//
//   - a Lambertian surface under a uniform sky of radiance L reflects
//     kD * albedo * L, whether the ladder was handed the irradiance cube's
//     E / pi, the lightmap's E or the probe bake's radiance projection;
//   - a composed pixel is the plain sum of its terms, with AO applied to the
//     ambient term and nothing else.
//
// So a units slip (the pi #1336 found on the lightmap and DDGI rungs) or an
// occlusion applied to a term it does not own shows up as a number here rather
// than as a subjective "the room looks bright". Each claim was checked to fail
// against a deliberately broken implementation; see the PR for the record.
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
#include "OloEngine/Renderer/SphericalHarmonics.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The probe's column layout, named in the shader's header.
        enum ProbeCase : u32
        {
            CaseLadderLightmapRung = 0,
            CaseLadderIblRung = 1,
            CaseLadderShRung = 2,
            CaseShCpuParity = 3,
            CaseEmissionNotOccluded = 4,
            CaseDirectNotOccluded = 5,
            CaseTracedIndirectNotOccluded = 6,
            CaseTransmissionNotOccluded = 7,
            CaseSuperposition = 8,
            CaseCount
        };

        // THE FIXTURE, mirrored literal for literal from the probe.
        constexpr glm::vec3 kAlbedo{ 0.8f, 0.5f, 0.2f };
        constexpr f32 kRoughness = 0.5f;
        constexpr glm::vec3 kUniformRadiance{ 1.2f, 0.9f, 0.6f };

        constexpr glm::vec3 kDirectDiffuse{ 0.30f, 0.20f, 0.10f };
        constexpr glm::vec3 kDirectSpecular{ 0.05f, 0.06f, 0.07f };
        constexpr glm::vec3 kAmbientDiffuse{ 0.11f, 0.12f, 0.13f };
        constexpr glm::vec3 kAmbientSpecular{ 0.02f, 0.03f, 0.04f };
        constexpr glm::vec3 kTracedIndirect{ 0.21f, 0.17f, 0.09f };
        constexpr glm::vec3 kUnsplitDirect{ 0.07f, 0.05f, 0.03f };
        constexpr glm::vec3 kTransmitted{ 0.015f, 0.025f, 0.035f };
        constexpr glm::vec3 kEmissive{ 1.5f, 0.25f, 0.75f };

        // The Lambertian answer, written from the physics rather than from
        // PBRCommon: a dielectric (F0 = 0.04) keeps (1 - F) of the energy for
        // the body lobe, F being Schlick's roughness-aware fit at the view angle
        // (the split-sum convention every ladder rung shares); the body lobe
        // then reflects albedo * L of a uniform field.
        [[nodiscard]] glm::vec3 ExpectedUniformSkyDiffuse()
        {
            const glm::vec3 view = glm::normalize(glm::vec3(0.3f, 0.0f, 1.0f));
            const f32 cosTheta = view.z;
            constexpr f32 kF0 = 0.04f;
            const f32 fresnel = kF0 + (std::max(1.0f - kRoughness, kF0) - kF0) * std::pow(1.0f - cosTheta, 5.0f);
            return (1.0f - fresnel) * kAlbedo * kUniformRadiance;
        }

        struct ContractProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ContractProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                // RGBA32F: the superposition case is compared at float
                // precision, and a half-float target would quantise the
                // difference the test exists to see.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("ContractProbeHarness::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                ::glClear(GL_COLOR_BUFFER_BIT);
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

        void ExpectVecNear(const glm::vec3& actual, const glm::vec3& expected, f32 relativeTolerance,
                           const char* what)
        {
            for (int c = 0; c < 3; ++c)
            {
                const f32 tolerance = relativeTolerance * std::max(1.0f, std::abs(expected[c]));
                EXPECT_NEAR(actual[c], expected[c], tolerance) << what << ", channel " << c;
            }
        }
    } // namespace

    TEST(LightingSignalContractGpu, ProductionCompositionAndLadderMatchIndependentAnswers)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ContractProbeHarness harness(static_cast<u32>(CaseCount), 1u,
                                     "assets/shaders/tests/ShaderUnit_LightingSignalContract.glsl");
        ASSERT_TRUE(harness.m_Shader) << "the probe shader was not created at all";
        ASSERT_TRUE(harness.m_Shader->IsReady())
            << "ShaderUnit_LightingSignalContract.glsl did not compile — check OloEngine.log. Every assertion below "
               "would otherwise measure a frame no shader wrote.";

        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(CaseCount) * 4u);
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(pixels[i])) << "probe texel " << i << " is non-finite";
        }
        const auto texel = [&](ProbeCase c)
        {
            const std::size_t base = static_cast<std::size_t>(c) * 4u;
            EXPECT_EQ(pixels[base + 3u], 1.0f) << "column " << static_cast<u32>(c) << " was never written";
            return glm::vec3(pixels[base + 0u], pixels[base + 1u], pixels[base + 2u]);
        };

        // ── Three routes to the ladder, one quantity ────────────────────────
        const glm::vec3 expectedDiffuse = ExpectedUniformSkyDiffuse();
        ExpectVecNear(texel(CaseLadderIblRung), expectedDiffuse, 1e-5f,
                      "the IBL rung (irradiance cube stores E/pi) does not reflect kD * albedo * L");
        ExpectVecNear(texel(CaseLadderLightmapRung), expectedDiffuse, 1e-5f,
                      "THE LIGHTMAP RUNG IS OFF: full irradiance E entered the ladder without the one E -> E/pi "
                      "conversion, so a lightmapped surface reads pi times brighter than the same surface under the "
                      "same sky on the IBL rung (issue #1336)");
        ExpectVecNear(texel(CaseLadderShRung), expectedDiffuse, 1e-4f,
                      "THE BAKED-SH RUNG IS OFF: the probe sampler must hand the ladder full irradiance E (the cosine "
                      "lobe applied per band), the same unit the DDGI atlas it is blended with returns");

        // ── The SH evaluator is the CPU one ─────────────────────────────────
        {
            SHCoefficients sh{};
            sh.Coefficients[0] = { 1.00f, 0.80f, 0.60f };
            sh.Coefficients[1] = { 0.30f, -0.10f, 0.05f };
            sh.Coefficients[2] = { 0.40f, 0.35f, 0.10f };
            sh.Coefficients[3] = { -0.20f, 0.15f, 0.25f };
            sh.Coefficients[4] = { 0.05f, -0.04f, 0.03f };
            sh.Coefficients[5] = { 0.02f, 0.06f, -0.05f };
            sh.Coefficients[6] = { -0.08f, 0.02f, 0.04f };
            sh.Coefficients[7] = { 0.03f, -0.02f, 0.01f };
            sh.Coefficients[8] = { 0.06f, 0.01f, -0.03f };
            const glm::vec3 cpu =
                SHBasis::EvaluateCosineConvolvedIrradiance(sh, glm::normalize(glm::vec3(0.3f, -0.5f, 0.8f)));
            ASSERT_GT(cpu.r, 0.0f) << "fixture degenerate: the CPU evaluator clamped to zero";
            ExpectVecNear(texel(CaseShCpuParity), cpu, 1e-5f,
                          "evaluateSHCosineIrradiance and SHBasis::EvaluateCosineConvolvedIrradiance disagree");
        }

        // ── Occlusion touches the ambient term and nothing else ─────────────
        ExpectVecNear(texel(CaseEmissionNotOccluded), kEmissive, 1e-6f,
                      "emission was darkened by the ambient visibility — AO applied to a term it does not own");
        ExpectVecNear(texel(CaseDirectNotOccluded), kDirectDiffuse + kDirectSpecular + kUnsplitDirect, 1e-6f,
                      "direct light was darkened by the ambient visibility; a light's own shadow is its visibility");
        ExpectVecNear(texel(CaseTracedIndirectNotOccluded), kTracedIndirect, 1e-6f,
                      "a traced tier's indirect was darkened by AO — it traced its own occlusion, and multiplying a "
                      "second estimate of the same visibility darkens every corner twice");
        ExpectVecNear(texel(CaseTransmissionNotOccluded), kTransmitted, 1e-6f,
                      "transmission was darkened by the ambient visibility");

        // ── The composed pixel is the sum of its terms ──────────────────────
        const f32 visibility = 0.37f;
        const glm::vec3 expectedSum = kDirectDiffuse + kDirectSpecular + visibility * (kAmbientDiffuse + kAmbientSpecular) +
                                      kTracedIndirect + kUnsplitDirect + kTransmitted + kEmissive;
        ExpectVecNear(texel(CaseSuperposition), expectedSum, 1e-5f,
                      "the composed pixel is not the sum of its terms — a term is counted twice or dropped");
    }
} // namespace OloEngine::Tests
