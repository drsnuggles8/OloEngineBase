// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ReferenceBRDFGpuParityTest.cpp — pins the offline reference path tracer's C++
// BRDF against the REAL COMPILED PBRCommon.glsl (issue #709).
//
// THE CONTRACT THIS FILE DEFENDS
// -------------------------------
// The reference tracer's value proposition is: "a divergence between raster and
// reference IS the bug". That sentence is only true while both shade with the
// same BRDF. The C++ mirror in ReferenceBRDF.h is a hand transcription, and a
// hand transcription drifts — someone edits `cookTorranceBRDF` (a Fresnel
// tweak, a different G term, an energy-compensation factor) and the reference
// keeps integrating the OLD formula. Nothing downstream can detect that: the
// reference still converges beautifully, and every GI comparison against it
// keeps "passing" while measuring against a renderer that no longer exists.
//
// So: render PbrBrdfParityProbe.glsl — which calls the production
// `cookTorranceBRDF` directly — over a (roughness x metallic x NdotL) grid,
// read it back, and evaluate the C++ port on the identical grid. Any
// disagreement beyond fp32 evaluation-order noise fails here, immediately,
// with the offending parameters named.
//
// This is the shaderpipe sibling of ReferenceBRDFTest (which pins the port's
// mathematical properties headlessly). Neither subsumes the other: this test
// cannot tell you the PDF's Jacobian is inverted (the GLSL has no PDF), and
// that one cannot tell you the two implementations have diverged.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
//
// Classification: shaderpipe (compiles + runs a production shader, compares
// against CPU math).
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        // Must match METALLIC_STEPS in PbrBrdfParityProbe.glsl. A mismatch
        // would silently compare different metallic values on the two sides —
        // so the test asserts the decoded band count against the shader's own
        // constant by construction (both are 4 and both are named here).
        constexpr u32 kMetallicSteps = 4;

        constexpr u32 kWidth = 128;  // roughness axis
        constexpr u32 kHeight = 128; // packed (metallic band, NdotL) axis

        // Minimal fullscreen-draw harness, mirroring PbrPropertyTests'
        // PbrProbeHarness. Deliberately a local copy rather than a shared
        // header: this file must be able to state exactly what GL state its
        // comparison ran under.
        struct ParityProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ParityProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F, not 16F: a half-float readback would blur the
                // comparison to ~1e-3 relative and hide exactly the kind of
                // small constant-factor drift this test exists to catch.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/PbrBrdfParityProbe.glsl");
            }

            void Draw()
            {
                GLStateGuard guard("ReferenceBRDFGpuParity::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutput(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            }
        };

        // The C++ side of the identical grid. Mirrors the shader's decode
        // exactly — including the pixel-centre offsets, which is what keeps the
        // two evaluating the SAME parameter values rather than two grids that
        // merely span the same ranges.
        struct GridPoint
        {
            f32 Roughness = 0.0f;
            f32 Metallic = 0.0f;
            glm::vec3 L{ 0.0f };
        };

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

            const f32 theta = withinBand * (kPi * 0.5f);
            point.L = glm::vec3(std::sin(theta), 0.0f, std::cos(theta));
            return point;
        }
    } // namespace

    TEST(ReferenceBRDFGpuParity, CppMirrorMatchesCompiledPbrCommon)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ParityProbeHarness harness;
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 v(0.0f, 0.0f, 1.0f);
        const glm::vec3 albedo(0.9f, 0.6f, 0.3f);

        // Tolerance model: the two implementations run the SAME arithmetic in
        // a different order on different hardware, so exact equality is not
        // available. A relative tolerance with an absolute floor is the right
        // shape — the BRDF spans several orders of magnitude across this grid
        // (a near-mirror highlight vs a grazing dielectric), so a pure absolute
        // bound would be meaninglessly loose at the top and impossible at the
        // bottom.
        //
        // 1% relative is roughly two orders of magnitude tighter than any
        // structural change to the BRDF: a swapped G term moves values by tens
        // of percent, a dropped 1/pi by ~3x, a Fresnel evaluated at the wrong
        // vector by a factor that grows with angle. It is loose enough that
        // fp32 reassociation and a driver's fast-math `inversesqrt` cannot
        // trip it.
        constexpr f32 kRelativeTolerance = 0.01f;
        constexpr f32 kAbsoluteFloor = 1e-4f;

        u32 mismatches = 0;
        f32 worstRelative = 0.0f;
        u32 worstX = 0;
        u32 worstY = 0;

        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const GridPoint point = DecodeGridPoint(x, y);
                const glm::vec3 expected =
                    CookTorranceBRDF(n, v, point.L, albedo, point.Metallic, point.Roughness);

                const sizet base = (static_cast<sizet>(y) * kWidth + x) * 4;
                const glm::vec3 actual(pixels[base + 0], pixels[base + 1], pixels[base + 2]);

                for (glm::length_t channel = 0; channel < 3; ++channel)
                {
                    ASSERT_TRUE(std::isfinite(actual[channel]))
                        << "GPU produced a non-finite BRDF at (" << x << ", " << y << ")";

                    const f32 difference = std::abs(actual[channel] - expected[channel]);
                    const f32 scale = std::max({ std::abs(expected[channel]), std::abs(actual[channel]), kAbsoluteFloor });
                    const f32 relative = difference / scale;
                    if (relative > worstRelative)
                    {
                        worstRelative = relative;
                        worstX = x;
                        worstY = y;
                    }
                    if (relative > kRelativeTolerance && difference > kAbsoluteFloor)
                        ++mismatches;
                }
            }
        }

        const GridPoint worstPoint = DecodeGridPoint(worstX, worstY);
        const glm::vec3 worstExpected =
            CookTorranceBRDF(n, v, worstPoint.L, albedo, worstPoint.Metallic, worstPoint.Roughness);
        const sizet worstBase = (static_cast<sizet>(worstY) * kWidth + worstX) * 4;

        EXPECT_EQ(mismatches, 0u)
            << mismatches << " of " << (kWidth * kHeight * 3) << " samples disagree.\n"
            << "Worst at roughness = " << worstPoint.Roughness << ", metallic = " << worstPoint.Metallic
            << ", NdotL = " << worstPoint.L.z << " (relative " << worstRelative << ")\n"
            << "  GLSL cookTorranceBRDF = (" << pixels[worstBase + 0] << ", " << pixels[worstBase + 1] << ", "
            << pixels[worstBase + 2] << ")\n"
            << "  C++  CookTorranceBRDF = (" << worstExpected.x << ", " << worstExpected.y << ", "
            << worstExpected.z << ")\n"
            << "PBRCommon.glsl and Renderer/PathTracing/ReferenceBRDF.h have DRIFTED. Until they are\n"
            << "reconciled the offline reference path tracer is validating the renderer against a\n"
            << "BRDF the renderer no longer uses — every comparison it makes is meaningless.";
    }

    // The probe must actually exercise the interesting parts of the domain; a
    // grid that came back all-zero (a shader that failed to compile and left a
    // cleared target) would make the parity test above pass vacuously.
    TEST(ReferenceBRDFGpuParity, ProbeGridCoversANonTrivialRange)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ParityProbeHarness harness;
        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        f32 minimum = std::numeric_limits<f32>::max();
        f32 maximum = 0.0f;
        for (sizet i = 0; i < static_cast<sizet>(kWidth) * kHeight; ++i)
        {
            for (u32 channel = 0; channel < 3; ++channel)
            {
                const f32 value = pixels[i * 4 + channel];
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }

        // Not `>= 0`: `kD * albedo / pi + specular` computes kD as `1 - F`, and
        // at grazing angles F rounds a hair above 1 in fp32, so the GPU
        // legitimately returns values like -2e-9. That is rounding, not a
        // negative BRDF — the bound is set where a real sign error (a flipped G
        // term, an inverted Fresnel) would land, orders of magnitude away.
        EXPECT_GE(minimum, -1e-5f) << "the BRDF returned a meaningfully negative value";
        EXPECT_GT(maximum, 0.5f) << "the probe target is uniformly near zero — the shader almost certainly "
                                    "failed to compile, which would make the parity test above pass vacuously";
    }
} // namespace OloEngine::Tests
