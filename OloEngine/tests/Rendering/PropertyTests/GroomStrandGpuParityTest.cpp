#include "OloEnginePCH.h"

// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GroomStrandGpuParityTest.cpp — pins GroomCoverage's C++ arithmetic against the
// REAL COMPILED include/GroomStrandCommon.glsl (issue #1246).
//
// THE CONTRACT THIS FILE DEFENDS
// -------------------------------
// The criterion-1 comparison is computed on the CPU. The coverage error every
// capture is judged against, the mode selection recorded in
// docs/analysis/groom-strand-visibility-1246.md, and the "stochastic alpha is
// unbiased" claim all come out of GroomCoverage::StochasticHash and the widened
// alpha — not out of the GPU. That is only meaningful while the two agree.
//
// A hand-mirrored formula drifts. Someone tightens the hash's avalanche, or
// changes the one-pixel floor, and the C++ side keeps predicting the OLD sample
// pattern. Nothing downstream can detect it: the measured numbers stay
// plausible, the pictures stay plausible, and every stochastic figure in the
// analysis quietly becomes a description of something that is not on screen.
//
// So: render GroomStrandHashParityProbe.glsl — which calls the production
// helpers directly — over an integer grid, read it back, and evaluate the C++
// twins on the identical grid. Any disagreement fails here with the offending
// pixel named.
//
// WHY EXACT EQUALITY IS THE RIGHT BAR HERE, unlike the BRDF parity sibling.
// These are not floating-point estimators: the hash is integer arithmetic whose
// wraparound is defined identically in C++ and GLSL, and its result is a 24-bit
// integer scaled by an exact power of two, so both sides land on the same
// 2^-24 grid. The widened alpha is one divide by a constant and a clamp. There
// is no evaluation-order freedom for a compiler to spend, so "close enough"
// would be hiding something rather than tolerating it.
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
#include "OloEngine/Groom/GroomCoverage.h"
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
        // 64 wide so the widened-alpha sweep (x / 64) crosses the half-pixel
        // floor inside the grid rather than only approaching it.
        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        // The production seed, and the only one the probe uses. Stated here so
        // a change on either side is a change to a named constant rather than
        // to a literal buried in two files.
        constexpr u32 kSeed = 1246;

        struct HashProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            HashProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F: the hash lands on a 2^-24 grid, and a half-float
                // readback would quantise away exactly the low-bit drift this
                // test exists to catch.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/GroomStrandHashParityProbe.glsl");
            }

            void Draw()
            {
                GLStateGuard guard("GroomStrandGpuParity::Draw", GLStateGuard::Policy::Restore);
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

        // The C++ twins of the probe's two derived values. The widened alpha
        // and the raster half width live inside GroomCoverage.cpp's anonymous
        // namespace, so they are restated here from the SAME constant the
        // header documents — if that floor ever moves, this restatement is
        // what makes the move visible instead of silently agreeing.
        constexpr f32 kMinHalfWidthPixels = 0.5f;

        [[nodiscard]] f32 WidenedAlpha(f32 trueHalfWidth)
        {
            return std::clamp(trueHalfWidth / kMinHalfWidthPixels, 0.0f, 1.0f);
        }

        [[nodiscard]] f32 RasterHalfWidth(f32 trueHalfWidth)
        {
            return std::max(trueHalfWidth, kMinHalfWidthPixels);
        }
    } // namespace

    TEST(GroomStrandGpuParity, CppHashMatchesTheCompiledShaderExactly)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HashProbeHarness harness;
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        u32 mismatches = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;

                // The probe's parameterisation, restated. It must match
                // GroomStrandHashParityProbe.glsl exactly — that is the whole
                // comparison, so it is written out rather than factored away.
                const u32 frame = x % 7u;
                const u32 segmentId = y * 13u + x;

                const f32 expected = GroomCoverage::StochasticHash(x, y, frame, segmentId, kSeed);
                const f32 actual = pixels[i];

                if (actual != expected && mismatches < 8u)
                {
                    ++mismatches;
                    ADD_FAILURE() << "hash drift at pixel (" << x << ", " << y << "): shader " << actual
                                  << " vs C++ " << expected << " (frame " << frame << ", segment " << segmentId
                                  << ", seed " << kSeed << ")";
                }
                else if (actual != expected)
                {
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(mismatches, 0u) << mismatches << " of " << (kWidth * kHeight)
                                  << " pixels disagree; the C++ coverage model and the shader are hashing "
                                     "different sample sets, so every stochastic number in "
                                     "docs/analysis/groom-strand-visibility-1246.md describes a pattern that is "
                                     "not on screen";
    }

    TEST(GroomStrandGpuParity, CppWidenedAlphaAndRasterWidthMatchTheCompiledShader)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HashProbeHarness harness;
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                const f32 trueHalfWidth = static_cast<f32>(x) / 64.0f;

                EXPECT_FLOAT_EQ(pixels[i + 1], WidenedAlpha(trueHalfWidth))
                    << "widened alpha drift at x = " << x << " (half width " << trueHalfWidth << ")";
                EXPECT_FLOAT_EQ(pixels[i + 2], RasterHalfWidth(trueHalfWidth))
                    << "raster half width drift at x = " << x;
            }
        }
    }

    // A grid that never exercised the interesting range would pass both tests
    // above while proving nothing — the same pairing the BRDF parity sibling
    // uses, and for the same reason.
    TEST(GroomStrandGpuParity, TheProbeGridActuallySweepsTheInterestingRange)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HashProbeHarness harness;
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        f32 minHash = 1.0f;
        f32 maxHash = 0.0f;
        u32 belowFloor = 0;
        u32 atOrAboveFloor = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                minHash = std::min(minHash, pixels[i]);
                maxHash = std::max(maxHash, pixels[i]);
                if (pixels[i + 1] < 1.0f)
                {
                    ++belowFloor;
                }
                else
                {
                    ++atOrAboveFloor;
                }
            }
        }

        // The hash must span most of [0,1) — a constant or a collapsed range
        // would make the equality tests above compare two identical fields.
        EXPECT_LT(minHash, 0.02f) << "the probe's hash never approaches 0";
        EXPECT_GT(maxHash, 0.98f) << "the probe's hash never approaches 1";
        // And the alpha sweep must cross the one-pixel floor in both
        // directions, or the clamp knee — the part most likely to be mirrored
        // wrong — is never evaluated.
        EXPECT_GT(belowFloor, 0u) << "no probe pixel is below the half-pixel floor";
        EXPECT_GT(atOrAboveFloor, 0u) << "no probe pixel is at or above the half-pixel floor";
    }
} // namespace OloEngine::Tests
