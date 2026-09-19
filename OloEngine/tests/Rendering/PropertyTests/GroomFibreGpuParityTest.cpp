#include "OloEnginePCH.h"

// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GroomFibreGpuParityTest.cpp — pins GroomFibreScattering's C++ arithmetic
// against the REAL COMPILED include/GroomFibreCommon.glsl (issue #1247).
//
// THE CONTRACT THIS FILE DEFENDS
// -------------------------------
// The lobe comparison is computed on the CPU. The quadrature rule was CHOSEN
// there, the energy check runs there, and every number in
// docs/analysis/groom-fibre-scattering-1247.md comes out of the C++ model — not
// out of the GPU. That is only meaningful while the two agree.
//
// A hand-mirrored model drifts. Someone tightens the Bessel series, reorders
// the attenuations, or drops the quadrature's roughness floor on one side, and
// the other side keeps predicting the old material. Nothing downstream can
// detect it, because a plausible hair shader and a correct one produce the same
// KIND of picture: the coat still looks like hair, the measured numbers still
// look reasonable, and the analysis quietly becomes a description of something
// that is not on screen.
//
// WHY A TOLERANCE AND NOT EXACT EQUALITY, unlike the #1246 hash sibling. That
// one is integer arithmetic with a defined wraparound and no evaluation-order
// freedom, so "close enough" would be hiding something. This is floating point
// with transcendentals — exp, log, sinh, asin, pow — whose last bits a driver
// is entitled to compute differently from the host's libm, and which appear
// inside a ten-term series and a division. So the bar is a RELATIVE tolerance,
// stated here as a number rather than left for a reader to infer:
//
//   kRelativeTolerance = 2e-3, with kAbsoluteFloor = 1e-6 for values so small
//   that a relative bound is meaningless.
//
// That is tight enough to catch any structural divergence — a dropped term, a
// wrong constant, a missing floor — and loose enough to survive a different
// transcendental implementation. A change that moves a value by more than 0.2 %
// is a change to the material, not to its rounding.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
//
// Classification: shaderpipe (compiles + runs a production shader include,
// compares against CPU math).
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        constexpr f32 kPi = 3.14159265358979323846f;

        constexpr f32 kRelativeTolerance = 2.0e-3f;
        constexpr f32 kAbsoluteFloor = 1.0e-6f;

        // THE PROBE'S MATERIAL, from the authored side. GroomFibreParityProbe
        // .glsl derives the same values from the same fits; building them here
        // with the production MakeGroomFibreParams rather than pasting the
        // derived numbers is deliberate, because the derivation is one of the
        // things the two sides can disagree about.
        [[nodiscard]] GroomFibreParams ProbeParams()
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Melanin;
            authored.Eumelanin = 1.3f;
            authored.Pheomelanin = 0.0f;
            authored.LongitudinalRoughness = 0.3f;
            authored.AzimuthalRoughness = 0.3f;
            authored.TiltDegrees = 2.0f;
            authored.IndexOfRefraction = kGroomFibreDefaultIOR;
            authored.Intensity = 1.0f;
            authored.HSamples = 4;
            return MakeGroomFibreParams(authored);
        }

        // The probe's grid, restated. It must match GroomFibreParityProbe.glsl
        // exactly — that IS the comparison, so it is written out rather than
        // factored away.
        struct ProbeAngles
        {
            f32 SinThetaO;
            f32 SinThetaI;
            f32 Phi;
        };

        [[nodiscard]] ProbeAngles AnglesAt(u32 x, u32 y)
        {
            ProbeAngles angles;
            angles.SinThetaO = -0.95f + (1.90f * static_cast<f32>(x) / 63.0f);
            angles.SinThetaI = -0.95f + (1.90f * static_cast<f32>(y) / 63.0f);
            angles.Phi = kPi * static_cast<f32>((x * 7u + y * 13u) % 64u) / 63.0f;
            return angles;
        }

        [[nodiscard]] bool WithinTolerance(f32 shader, f32 expected)
        {
            const f32 delta = std::abs(shader - expected);
            return delta <= std::max(kAbsoluteFloor, std::abs(expected) * kRelativeTolerance);
        }

        struct FibreProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            explicit FibreProbeHarness(const char* shaderPath)
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F: these are small radiance values, and a half-float
                // readback would quantise away exactly the drift this test
                // exists to catch.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("GroomFibreGpuParity::Draw", GLStateGuard::Policy::Restore);
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
    } // namespace

    TEST(GroomFibreGpuParity, CppLobesMatchTheCompiledShader)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FibreProbeHarness harness("assets/shaders/tests/GroomFibreParityProbe.glsl");
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        const GroomFibreParams params = ProbeParams();

        u32 mismatches = 0;
        u32 reported = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                const ProbeAngles angles = AnglesAt(x, y);
                const GroomFibreLobeSet lobes =
                    GroomFibreEvaluateFar(params, angles.SinThetaO, angles.SinThetaI, angles.Phi);

                const std::array<f32, 4> expected{ lobes[GroomFibreLobe::R].g, lobes[GroomFibreLobe::TT].g,
                                                   lobes[GroomFibreLobe::TRT].g,
                                                   lobes[GroomFibreLobe::Residual].g };
                const std::array<const char*, 4> names{ "R", "TT", "TRT", "residual" };

                for (u32 channel = 0; channel < 4; ++channel)
                {
                    if (WithinTolerance(pixels[i + channel], expected[channel]))
                    {
                        continue;
                    }
                    ++mismatches;
                    if (reported < 8u)
                    {
                        ++reported;
                        ADD_FAILURE() << names[channel] << " lobe drift at (" << x << ", " << y << "): shader "
                                      << pixels[i + channel] << " vs C++ " << expected[channel] << " (sinThetaO "
                                      << angles.SinThetaO << ", sinThetaI " << angles.SinThetaI << ", phi "
                                      << angles.Phi << ")";
                    }
                }
            }
        }

        EXPECT_EQ(mismatches, 0u) << mismatches << " of " << (kWidth * kHeight * 4u)
                                  << " lobe samples disagree; the C++ model and the shader are scattering "
                                     "differently, so every number in "
                                     "docs/analysis/groom-fibre-scattering-1247.md describes a material that is "
                                     "not on screen";
    }

    TEST(GroomFibreGpuParity, CppAmbientResponseMatchesTheCompiledShader)
    {
        // The environment half shares the attenuations with the direct half but
        // takes its own path through both sides and carries its own
        // cos(theta_o), so it needs its own comparison — and without one it
        // would be the single piece of the material no parity test covered.
        //
        // Its own PROBE, too: the lobe probe's four channels each carry one
        // lobe, so there is no channel left for it. See that probe's header for
        // why a summed channel was not an option.
        OLO_ENSURE_GPU_OR_SKIP();

        FibreProbeHarness harness("assets/shaders/tests/GroomFibreAmbientParityProbe.glsl");
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        const GroomFibreParams params = ProbeParams();

        u32 mismatches = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                const ProbeAngles angles = AnglesAt(x, y);
                const GroomFibreLobeSet ambient = GroomFibreAmbientResponse(params, angles.SinThetaO);
                const std::array<f32, 4> expected{ ambient[GroomFibreLobe::R].g, ambient[GroomFibreLobe::TT].g,
                                                   ambient[GroomFibreLobe::TRT].g,
                                                   ambient[GroomFibreLobe::Residual].g };
                const std::array<const char*, 4> names{ "R", "TT", "TRT", "residual" };
                for (u32 channel = 0; channel < 4; ++channel)
                {
                    if (WithinTolerance(pixels[i + channel], expected[channel]))
                    {
                        continue;
                    }
                    if (mismatches < 8u)
                    {
                        ADD_FAILURE() << "ambient " << names[channel] << " drift at (" << x << ", " << y
                                      << "): shader " << pixels[i + channel] << " vs C++ " << expected[channel]
                                      << " (sinThetaO " << angles.SinThetaO << ")";
                    }
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(mismatches, 0u);
    }

    // A grid that never reached the interesting part of the model would pass
    // the two tests above while proving nothing — the same pairing #1246's
    // parity sibling uses, and for the same reason.
    TEST(GroomFibreGpuParity, TheProbeGridActuallyExercisesEveryLobe)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FibreProbeHarness harness("assets/shaders/tests/GroomFibreParityProbe.glsl");
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        std::array<f32, 4> maxima{ 0.0f, 0.0f, 0.0f, 0.0f };
        u32 finite = 0;
        for (sizet i = 0; i + 3 < pixels.size(); i += 4)
        {
            for (u32 channel = 0; channel < 4; ++channel)
            {
                EXPECT_TRUE(std::isfinite(pixels[i + channel]));
                maxima[channel] = std::max(maxima[channel], pixels[i + channel]);
            }
            ++finite;
        }

        EXPECT_EQ(finite, static_cast<u32>(kWidth) * kHeight);
        // Every lobe has to actually fire somewhere on the grid, or its
        // comparison above was a comparison of two zeroes. The TT lobe is the
        // one at risk: it only reaches the eye near the forward direction, so a
        // grid that never got there would silently exempt it.
        //
        // The thresholds differ by lobe because the lobes differ in size by
        // orders of magnitude: the residual is the geometric tail of every path
        // past TRT and is around 1e-7 of the response at these angles, so
        // holding it to the R lobe's bar would fail on a correct shader. What
        // each bar has to be is ABOVE ZERO and below the lobe's real peak,
        // which is what makes a silently dropped lobe visible here.
        EXPECT_GT(maxima[0], 1.0e-4f) << "the R lobe never fires on the probe grid";
        EXPECT_GT(maxima[1], 1.0e-4f) << "the TT lobe never fires on the probe grid";
        EXPECT_GT(maxima[2], 1.0e-5f) << "the TRT lobe never fires on the probe grid";
        EXPECT_GT(maxima[3], 1.0e-9f) << "the residual lobe never fires on the probe grid";
    }
} // namespace OloEngine::Tests
