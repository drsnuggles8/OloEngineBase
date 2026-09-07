// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ClosureV2SampleGpuParityTest.cpp — device-pins `closureV2SampleBRDF`, the
// SAMPLE third of the v2 closure's Evaluate / Sample / Pdf triple, against
// its C++ twin PBRClosureBSDF.h's BSDF::Sample (issue #1055).
//
// ClosureV2GpuParityTest pins Evaluate and Pdf. Until #1055 the sampler was
// compile-covered only — PBRCommon.glsl's own note asked the first consumer to
// "extend the parity probe with a sampled-tuple channel". The GPU path tracer
// draws EVERY scatter direction through this function, and a path tracer
// whose sampling routine is unpinned is not an oracle: a wrong Sample twin
// converges beautifully to the wrong image.
//
// Renders assets/shaders/tests/PbrClosureV2SampleProbe.glsl over a
// (roughness x packed lobe-select / metallic) grid with a fixed off-axis
// view, reads back the sampled direction, the mixture density and the
// evaluated value, and diffs each against BSDF::Sample on the identical grid.
//
// The two twins spell a failed draw differently — the C++ returns false, the
// GLSL returns Pdf == 0 with a placeholder direction — so the comparison
// maps the two conventions onto each other rather than comparing the
// placeholder.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        // Must match METALLIC_STEPS in PbrClosureV2SampleProbe.glsl.
        constexpr u32 kMetallicSteps = 4;

        constexpr u32 kWidth = 128;  // roughness axis
        constexpr u32 kHeight = 128; // packed (metallic band, lobe select) axis

        struct SampleProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            SampleProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F, not 16F: a half-float readback would blur the
                // comparison to ~1e-3 relative and hide exactly the kind of
                // small constant-factor drift this test exists to catch.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/PbrClosureV2SampleProbe.glsl");
            }

            [[nodiscard]] bool Draw()
            {
                if (m_OutputFB == nullptr || m_Shader == nullptr)
                {
                    ADD_FAILURE() << (m_OutputFB == nullptr ? "sample probe framebuffer was not created"
                                                            : "PbrClosureV2SampleProbe.glsl failed to load/compile");
                    return false;
                }

                GLStateGuard guard("ClosureV2SampleGpuParity::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                const GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
                ::glDrawBuffers(2, drawBuffers);
                ::glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
                return true;
            }

            void ReadDirection(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            }
            void ReadValue(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(1), kWidth, kHeight, out);
            }
        };

        // The C++ side of the identical grid. Mirrors the shader's decode
        // exactly, pixel-centre offsets included, so the two evaluate the SAME
        // parameters rather than two grids that merely span the same ranges.
        struct GridPoint
        {
            f32 Roughness = 0.0f;
            f32 Metallic = 0.0f;
            f32 LobeXi = 0.0f;
            glm::vec2 Xi{ 0.0f };
        };

        [[nodiscard]] f32 Fract(f32 x)
        {
            return x - std::floor(x);
        }

        [[nodiscard]] GridPoint DecodeGridPoint(u32 x, u32 y)
        {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);

            GridPoint point;
            point.Roughness = std::clamp(u, 0.0f, 1.0f);

            const f32 scaled = std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(kMetallicSteps);
            const f32 band = std::min(std::floor(scaled), static_cast<f32>(kMetallicSteps - 1));
            const f32 withinBand = scaled - band;

            point.Metallic = band / static_cast<f32>(kMetallicSteps - 1);
            point.LobeXi = withinBand;
            point.Xi = glm::vec2(Fract(u * 7.0f + v * 3.0f) * 0.98f + 0.01f, Fract(v * 11.0f + u * 5.0f) * 0.98f + 0.01f);
            return point;
        }

        [[nodiscard]] ReferenceMaterial MakeV2Material(const GridPoint& point)
        {
            ReferenceMaterial material;
            material.BaseColor = glm::vec3(0.9f, 0.6f, 0.3f);
            material.Metallic = point.Metallic;
            material.Roughness = point.Roughness;
            material.Model = PBRModel::ClosureV2;
            return material;
        }

        const glm::vec3 kN(0.0f, 0.0f, 1.0f);
        const glm::vec3 kV = glm::normalize(glm::vec3(0.4f, 0.3f, 1.0f));

        // Same tolerance model as the Evaluate / Pdf parity tests: relative
        // with an absolute floor, so a value near zero is not held to a
        // relative bar that fp32 evaluation order cannot meet.
        constexpr f32 kRelativeTolerance = 0.01f;
        constexpr f32 kAbsoluteFloor = 1e-4f;
        // Directions are unit vectors; the sampler's trig and basis are exact
        // to a few ulps on both sides, so the bar is absolute and tight. The
        // one legitimate source of a larger gap is the shape sample Xi itself
        // rounding differently through the two fract()s — a 1e-7 relative
        // change of Xi moves a sampled direction by about that much.
        constexpr f32 kDirectionTolerance = 2e-4f;

        [[nodiscard]] bool RelativeMismatch(f32 expected, f32 actual, f32 conditioningBand = 0.0f)
        {
            const f32 difference = std::abs(actual - expected);
            const f32 scale = std::max({ std::abs(expected), std::abs(actual), kAbsoluteFloor });
            return (difference / scale) > kRelativeTolerance && difference > kAbsoluteFloor &&
                   difference > conditioningBand;
        }

        // How far the density and the specular value move when the half
        // vector's cosine N.H is off by a few fp32 ulps of 1.0 — which is what
        // `normalize(V + L)` rounding differently on the two sides does. Near
        // the v2 roughness floor the GGX lobe is peaked enough (alpha^2 ~
        // 2.6e-6) that a 5e-7 change of N.H moves D by tens of percent: an
        // ill-conditioned function of an fp32 input, not a drift between the
        // twins. Measured on the cosine rather than as a rotation of L,
        // because at the peak a tangential rotation of L barely moves N.H at
        // all while the arithmetic error lands on it directly. The band is
        // the measured sensitivity, so a mismatch inside it cannot be
        // distinguished from rounding by ANY implementation.
        constexpr f32 kCosinePerturbation = 8.0f * 5.9604645e-8f; // 8 ulps of 1.0

        struct ConditioningBands
        {
            f32 Pdf = 0.0f;
            f32 Value = 0.0f;
        };

        [[nodiscard]] ConditioningBands ConditioningBandsAt(const ReferenceMaterial& material, const glm::vec3& L)
        {
            const glm::vec3 H = glm::normalize(kV + L);
            const f32 nDotH = std::clamp(glm::dot(kN, H), 0.0f, 1.0f);
            const f32 nDotV = glm::dot(kN, kV);
            const f32 nDotL = std::max(glm::dot(kN, L), 0.0f);
            const f32 r = ClosureV2Roughness(material.Roughness);
            const f32 alpha = r * r;

            const f32 centre = DistributionGGXSamplingDensity(nDotH, r);
            const f32 up = DistributionGGXSamplingDensity(std::min(nDotH + kCosinePerturbation, 1.0f), r);
            const f32 down = DistributionGGXSamplingDensity(std::max(nDotH - kCosinePerturbation, 0.0f), r);
            const f32 dBand = std::max(std::abs(up - centre), std::abs(down - centre));

            const f32 pSpecular = SpecularLobeProbability(material.BaseColor, material.Metallic);
            const f32 g1V = 1.0f / (1.0f + GgxSmithLambda(nDotV, alpha));
            const f32 vis = VisibilitySmithGGXCorrelated(nDotV, nDotL, r);
            const glm::vec3 f0 = glm::mix(glm::vec3(kDefaultDielectricF0), material.BaseColor, material.Metallic);
            const glm::vec3 fresnel = FresnelSchlick(std::max(glm::dot(H, kV), 0.0f), f0);
            const f32 fresnelMax = std::max({ fresnel.x, fresnel.y, fresnel.z });

            ConditioningBands bands;
            bands.Pdf = pSpecular * g1V * dBand / (4.0f * std::max(nDotV, 1e-6f));
            bands.Value = dBand * vis * fresnelMax;
            return bands;
        }
    } // namespace

    TEST(ClosureV2SampleGpuParity, V2SampleMatchesCppTwin)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SampleProbeHarness harness;
        ASSERT_TRUE(harness.Draw());
        std::vector<f32> gpuDirection;
        std::vector<f32> gpuValue;
        harness.ReadDirection(gpuDirection);
        harness.ReadValue(gpuValue);
        ASSERT_EQ(gpuDirection.size(), static_cast<sizet>(kWidth) * kHeight * 4);
        ASSERT_EQ(gpuValue.size(), gpuDirection.size());

        u32 directionMismatches = 0;
        u32 pdfMismatches = 0;
        u32 valueMismatches = 0;
        u32 conventionMismatches = 0;
        u32 validSamples = 0;
        u32 specularDraws = 0;
        u32 illConditionedTexels = 0;
        f32 worstDirectionGap = 0.0f;
        GridPoint worstPoint{};

        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet base = (static_cast<sizet>(y) * kWidth + x) * 4;
                const GridPoint point = DecodeGridPoint(x, y);
                const ReferenceMaterial material = MakeV2Material(point);

                BSDF::BSDFSample expected;
                const bool cpuValid = BSDF::Sample(material, kN, kV, point.LobeXi, point.Xi, expected);
                const glm::vec3 gpuL(gpuDirection[base + 0], gpuDirection[base + 1], gpuDirection[base + 2]);
                const f32 gpuPdf = gpuDirection[base + 3];
                const glm::vec3 gpuVal(gpuValue[base + 0], gpuValue[base + 1], gpuValue[base + 2]);

                for (sizet c = 0; c < 4; ++c)
                {
                    ASSERT_TRUE(std::isfinite(gpuDirection[base + c])) << "non-finite direction/pdf at " << x << "," << y;
                    ASSERT_TRUE(std::isfinite(gpuValue[base + c])) << "non-finite value at " << x << "," << y;
                }

                // The two failure conventions must agree on WHICH draws fail.
                const bool gpuValid = gpuPdf > 0.0f;
                if (cpuValid != gpuValid)
                {
                    ++conventionMismatches;
                    continue;
                }
                if (!cpuValid)
                    continue;

                ++validSamples;
                if (point.LobeXi < BSDF::SpecularProbability(material))
                    ++specularDraws;

                const f32 directionGap = glm::length(gpuL - expected.Direction);
                if (directionGap > kDirectionTolerance)
                {
                    ++directionMismatches;
                    if (directionGap > worstDirectionGap)
                    {
                        worstDirectionGap = directionGap;
                        worstPoint = point;
                    }
                }

                // The density and value are compared AT THE GPU'S OWN
                // DIRECTION, which is what Sample() reports for its draw: the
                // direction gap is bounded separately above, and a near-mirror
                // lobe is peaked enough that a direction inside that bound
                // still moves the density by more than the relative
                // tolerance. Pdf(L) == Sample's Pdf is already pinned on the
                // CPU side (ClosureV2ConsistencyTest); this pins the GLSL
                // density and evaluation at the sampled point.
                const f32 pdfAtGpuL = BSDF::Pdf(material, kN, kV, gpuL);
                const glm::vec3 valueAtGpuL = BSDF::Evaluate(material, kN, kV, gpuL);
                const ConditioningBands bands = ConditioningBandsAt(material, gpuL);
                const f32 pdfBand = bands.Pdf;
                const f32 valueBand = bands.Value;
                if (pdfBand > kRelativeTolerance * std::max(pdfAtGpuL, kAbsoluteFloor))
                    ++illConditionedTexels;
                if (RelativeMismatch(pdfAtGpuL, gpuPdf, pdfBand))
                {
                    if (pdfMismatches == 0)
                        std::cout << "[parity] first pdf mismatch at roughness " << point.Roughness << " metallic "
                                  << point.Metallic << " lobeXi " << point.LobeXi << ": cpu " << pdfAtGpuL << " gpu "
                                  << gpuPdf << "\n";
                    ++pdfMismatches;
                }
                // The value's conditioning is measured on the red channel and
                // applied to all three: the specular term is the same D for
                // every channel and it is the only ill-conditioned factor.
                if (RelativeMismatch(valueAtGpuL.x, gpuVal.x, valueBand) ||
                    RelativeMismatch(valueAtGpuL.y, gpuVal.y, valueBand) ||
                    RelativeMismatch(valueAtGpuL.z, gpuVal.z, valueBand))
                {
                    if (valueMismatches == 0)
                        std::cout << "[parity] first value mismatch at roughness " << point.Roughness << " metallic "
                                  << point.Metallic << " lobeXi " << point.LobeXi << ": cpu " << valueAtGpuL.x << " gpu "
                                  << gpuVal.x << "\n";
                    ++valueMismatches;
                }
            }
        }

        EXPECT_EQ(conventionMismatches, 0u) << "C++ Sample returned false where GLSL reported Pdf > 0, or vice versa";
        EXPECT_EQ(directionMismatches, 0u)
            << "sampled DIRECTION drifted from BSDF::Sample; worst gap " << worstDirectionGap << " at roughness "
            << worstPoint.Roughness << ", metallic " << worstPoint.Metallic << ", lobeXi " << worstPoint.LobeXi;
        EXPECT_EQ(pdfMismatches, 0u) << "mixture Pdf drifted from BSDF::Sample's";
        EXPECT_EQ(valueMismatches, 0u) << "sampled Value drifted from BSDF::Sample's";
        // The conditioning escape must stay the exception: it exists for the
        // near-mirror corner of the grid, and a grid where most texels needed
        // it would be comparing nothing.
        std::cout << "[parity] ill-conditioned texels (density moves > 1% under an 8-ulp change of N.H): "
                  << illConditionedTexels << " / " << (kWidth * kHeight) << "\n";
        EXPECT_LT(illConditionedTexels, (kWidth * kHeight) / 10u);

        // Non-vacuity: a cleared target (a shader that did not compile) has
        // Pdf == 0 everywhere and would pass every comparison above as "both
        // failed". Both lobes must actually be drawn.
        const u32 texels = kWidth * kHeight;
        EXPECT_GT(validSamples, texels / 2u) << "most draws of an above-horizon view must succeed";
        EXPECT_GT(specularDraws, texels / 20u) << "the VNDF lobe must be exercised, not only the cosine lobe";
        EXPECT_GT(validSamples - specularDraws, texels / 20u) << "the cosine lobe must be exercised too";
    }
} // namespace OloEngine::Tests
