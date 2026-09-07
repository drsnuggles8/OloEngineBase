// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// PathTracerSamplerParityTest.cpp — pins include/PathTracerSampler.glsl, the
// GLSL twin of Renderer/PathTracing/PathSampler.h, against the C++ sampler
// ON THE DEVICE, value for value (issue #1055).
//
// The GPU path tracer is comparable with the CPU reference term by term only
// because the two draw the SAME random numbers for the same (pixel, sample,
// dimension). That claim is checkable to the bit: the sampler is integer
// arithmetic modulo 2^32 plus one exact float conversion, so the tolerance
// here is ZERO. A single differing bit fails the test and names the texel.
//
// Renders assets/shaders/tests/PathTracerSamplerProbe.glsl, which dumps the
// first six dimensions of the sequence for a (sample index x pixel-seed row)
// grid, and compares against PathSampler on the identical grid.
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
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    using OloEngine::PathTracing::MakePixelSeed;
    using OloEngine::PathTracing::PathSampler;

    namespace
    {
        // Must match GLOBAL_SEED / SEED_COLUMN in PathTracerSamplerProbe.glsl.
        constexpr u32 kGlobalSeed = 0x9e3779b9u;
        constexpr u32 kSeedColumn = 7u;

        constexpr u32 kWidth = 256; // sample index axis
        constexpr u32 kHeight = 64; // pixel-seed row axis

        struct SamplerProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            SamplerProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/PathTracerSamplerProbe.glsl");
            }

            [[nodiscard]] bool Draw()
            {
                if (m_OutputFB == nullptr || m_Shader == nullptr)
                {
                    ADD_FAILURE() << (m_OutputFB == nullptr ? "sampler probe framebuffer was not created"
                                                            : "PathTracerSamplerProbe.glsl failed to load/compile");
                    return false;
                }

                GLStateGuard guard("PathTracerSamplerParity::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
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

            void ReadOutput(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            }
        };
    } // namespace

    TEST(PathTracerSamplerParity, GlslSamplerIsBitIdenticalToTheCppSampler)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SamplerProbeHarness harness;
        ASSERT_TRUE(harness.Draw());
        std::vector<f32> gpu;
        harness.ReadOutput(gpu);
        ASSERT_EQ(gpu.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        u32 mismatches = 0;
        u32 firstX = 0, firstY = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                // glGetTextureImage returns texture row 0 first, which is the
                // row gl_FragCoord.y == 0 wrote — no flip on this side either.
                const sizet base = (static_cast<sizet>(y) * kWidth + x) * 4;
                const u32 pixelSeed = MakePixelSeed(y, kSeedColumn, kGlobalSeed);
                PathSampler sampler(pixelSeed, x);
                const glm::vec2 first2D = sampler.Get2D();
                const f32 first1D = sampler.Get1D();
                const glm::vec2 second2D = sampler.Get2D();
                const f32 second1D = sampler.Get1D();
                // The pack is exact: both terms are below 1 and the multiply
                // by 4 is a power of two.
                const f32 packed = second2D.x + second1D * 4.0f;

                const bool equal = gpu[base + 0] == first2D.x && gpu[base + 1] == first2D.y &&
                                   gpu[base + 2] == first1D && gpu[base + 3] == packed;
                if (!equal)
                {
                    if (mismatches == 0)
                    {
                        firstX = x;
                        firstY = y;
                    }
                    ++mismatches;
                }
            }
        }

        EXPECT_EQ(mismatches, 0u) << "GLSL sampler drifted from PathSampler.h at " << mismatches
                                  << " texel(s); first at sample " << firstX << ", seed row " << firstY;
    }

    TEST(PathTracerSamplerParity, ProbeGridIsNotTrivial)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        SamplerProbeHarness harness;
        ASSERT_TRUE(harness.Draw());
        std::vector<f32> gpu;
        harness.ReadOutput(gpu);
        ASSERT_EQ(gpu.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        // A shader that failed to compile leaves a cleared target, and a
        // sampler that returned a constant would pass a comparison against a
        // C++ twin with the same bug. Neither is a unit-interval spread.
        f32 minimum = 1.0f;
        f32 maximum = 0.0f;
        for (sizet i = 0; i < gpu.size(); i += 4)
        {
            minimum = std::min({ minimum, gpu[i + 0], gpu[i + 1], gpu[i + 2] });
            maximum = std::max({ maximum, gpu[i + 0], gpu[i + 1], gpu[i + 2] });
            EXPECT_GE(gpu[i + 0], 0.0f);
            EXPECT_LT(gpu[i + 0], 1.0f) << "the 24-bit truncation keeps every draw strictly below 1";
        }
        EXPECT_LT(minimum, 0.05f);
        EXPECT_GT(maximum, 0.95f);
    }
} // namespace OloEngine::Tests
