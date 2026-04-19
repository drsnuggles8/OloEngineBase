// =============================================================================
// PerfRegressionTests.cpp
//
// Layer-6 (Performance Regression, doc section 6) skeleton. Wronski's
// "microbenchmarks with controlled inputs" philosophy — isolate individual
// post-process passes on fixed-size synthetic inputs and measure GPU time
// via GL timestamp queries. No checked-in baselines yet: these tests assert
// only that *timing queries work* and that the measured time is within a
// sanity range (non-zero, below a generous ceiling).
//
// Rationale: the baseline JSON management described in doc §6 is a separate
// feature; shipping it prematurely would bake in numbers from the developer
// machine that fail on every other box. This file establishes the
// infrastructure — shader compile, dispatch, timing — so future work can
// add baselines and CI budget checks.
//
// Currently measured:
//   1. Tone map pass GPU time at 512×512 RGBA8 (Reinhard)
//   2. Bloom threshold pass GPU time at 512×512
//   3. Bloom downsample pass GPU time at 512×512 (13-tap box)
//   4. Bloom upsample pass GPU time at 512×512 (9-tap tent)
//
// Expected additions (per the doc table):
//   - FXAA, shadow generation, PBR lighting at varied resolutions.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // RAII wrapper around a pair of GL_TIME_ELAPSED queries (start/stop
        // timestamps). Measures wall-clock GPU time between Begin() and End().
        class GpuTimer
        {
          public:
            GpuTimer()
            {
                ::glGenQueries(1, &m_Query);
            }
            ~GpuTimer()
            {
                if (m_Query)
                    ::glDeleteQueries(1, &m_Query);
            }
            GpuTimer(const GpuTimer&) = delete;
            GpuTimer& operator=(const GpuTimer&) = delete;

            void Begin() const
            {
                ::glBeginQuery(GL_TIME_ELAPSED, m_Query);
            }
            void End() const
            {
                ::glEndQuery(GL_TIME_ELAPSED);
            }

            // Blocks until the query resolves. Returns elapsed nanoseconds.
            u64 GetElapsedNs() const
            {
                GLint available = 0;
                while (!available)
                    ::glGetQueryObjectiv(m_Query, GL_QUERY_RESULT_AVAILABLE, &available);
                GLuint64 ns = 0;
                ::glGetQueryObjectui64v(m_Query, GL_QUERY_RESULT, &ns);
                return static_cast<u64>(ns);
            }

          private:
            GLuint m_Query = 0;
        };
    } // namespace

    // Run the tone-map pass over a uniform input and confirm that GPU timing
    // infrastructure works end-to-end. Reports the median over N runs. This
    // is the skeleton for future per-pass microbenchmarks.
    TEST(PerfRegressionTest, ToneMapPassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 512;
        constexpr u32 kHeight = 512;
        constexpr u32 kWarmup = 3;
        constexpr u32 kMeasure = 10;

        FramebufferSpecification spec{};
        spec.Width = kWidth;
        spec.Height = kHeight;
        spec.Attachments = { FramebufferTextureFormat::RGBA8 };
        auto fb = Framebuffer::Create(spec);

        auto shader = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");
        ASSERT_TRUE(shader);

        PostProcessUBOData uboData{};
        uboData.TonemapOperator = 1;
        uboData.Exposure = 1.0f;
        uboData.Gamma = 2.2f;
        uboData.InverseScreenWidth = 1.0f / kWidth;
        uboData.InverseScreenHeight = 1.0f / kHeight;
        uboData.TexelSizeX = uboData.InverseScreenWidth;
        uboData.TexelSizeY = uboData.InverseScreenHeight;
        auto ubo = UniformBuffer::Create(PostProcessUBOData::GetSize(), 7);
        ubo->SetData(&uboData, PostProcessUBOData::GetSize());

        const u32 inputTex = CreateUniformFloatTexture2D(kWidth, kHeight, 0.5f, 0.5f, 0.5f, 1.0f);

        FullscreenPass pass;

        auto DrawOnce = [&]()
        {
            fb->Bind();
            ::glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
            ::glDisable(GL_BLEND);
            ::glDisable(GL_DEPTH_TEST);
            ::glDisable(GL_CULL_FACE);
            shader->Bind();
            pass.Draw(inputTex);
            fb->Unbind();
        };

        // Warm up — first frame always hits driver JIT.
        for (u32 i = 0; i < kWarmup; ++i)
            DrawOnce();
        ::glFinish();

        std::vector<u64> samples;
        samples.reserve(kMeasure);
        for (u32 i = 0; i < kMeasure; ++i)
        {
            GpuTimer timer;
            timer.Begin();
            DrawOnce();
            timer.End();
            samples.push_back(timer.GetElapsedNs());
        }

        std::sort(samples.begin(), samples.end());
        const u64 median = samples[samples.size() / 2];

        // Sanity bounds: must be non-zero (query is working) and must be
        // under 100 ms (otherwise something is very wrong — a fullscreen
        // tone map at 512×512 takes microseconds on any real GPU).
        EXPECT_GT(median, 0u) << "GL_TIME_ELAPSED query returned 0 ns — driver support?";
        EXPECT_LT(median, 100 * 1000 * 1000ull) << "tone map at 512^2 took >100 ms; bail";

        RecordProperty("tone_map_512x512_ns_median", std::to_string(median));

        ::glDeleteTextures(1, &inputTex);
    }

    // -------------------------------------------------------------------------
    // Shared microbenchmark helper: time a single-shader fullscreen pass over
    // a uniform RGBA32F input. Returns the median GPU time in nanoseconds.
    // -------------------------------------------------------------------------
    namespace
    {
        u64 MeasureFullscreenPassNs(const char* shaderPath, u32 width, u32 height)
        {
            constexpr u32 kWarmup = 3;
            constexpr u32 kMeasure = 10;

            FramebufferSpecification spec{};
            spec.Width = width;
            spec.Height = height;
            spec.Attachments = { FramebufferTextureFormat::RGBA8 };
            auto fb = Framebuffer::Create(spec);

            auto shader = Shader::Create(shaderPath);
            if (!shader)
                return 0;

            PostProcessUBOData uboData{};
            uboData.TonemapOperator = 1;
            uboData.Exposure = 1.0f;
            uboData.Gamma = 2.2f;
            uboData.BloomThreshold = 1.0f;
            uboData.BloomIntensity = 0.5f;
            uboData.InverseScreenWidth = 1.0f / static_cast<f32>(width);
            uboData.InverseScreenHeight = 1.0f / static_cast<f32>(height);
            uboData.TexelSizeX = uboData.InverseScreenWidth;
            uboData.TexelSizeY = uboData.InverseScreenHeight;
            auto ubo = UniformBuffer::Create(PostProcessUBOData::GetSize(), 7);
            ubo->SetData(&uboData, PostProcessUBOData::GetSize());

            const u32 inputTex = CreateUniformFloatTexture2D(width, height, 0.5f, 0.5f, 0.5f, 1.0f);

            FullscreenPass pass;

            auto DrawOnce = [&]()
            {
                fb->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                shader->Bind();
                pass.Draw(inputTex);
                fb->Unbind();
            };

            for (u32 i = 0; i < kWarmup; ++i)
                DrawOnce();
            ::glFinish();

            std::vector<u64> samples;
            samples.reserve(kMeasure);
            for (u32 i = 0; i < kMeasure; ++i)
            {
                GpuTimer timer;
                timer.Begin();
                DrawOnce();
                timer.End();
                samples.push_back(timer.GetElapsedNs());
            }

            ::glDeleteTextures(1, &inputTex);

            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        }
    } // namespace

    // Bloom extract (threshold) pass: luminance extract + soft-knee multiply.
    // Should be fastest of the bloom chain — no cross-pixel sampling.
    TEST(PerfRegressionTest, BloomThresholdPassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassNs(
            "assets/shaders/PostProcess_BloomThreshold.glsl", 512, 512);
        EXPECT_GT(median, 0u) << "bloom threshold: timing query returned 0 ns";
        EXPECT_LT(median, 100 * 1000 * 1000ull) << "bloom threshold >100 ms at 512^2";
        RecordProperty("bloom_threshold_512x512_ns_median", std::to_string(median));
    }

    // Bloom downsample pass: 13-tap box filter. Expected cost roughly 13×
    // the tone-map pass (1 tap each), minus ALU overhead.
    TEST(PerfRegressionTest, BloomDownsamplePassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassNs(
            "assets/shaders/PostProcess_BloomDownsample.glsl", 512, 512);
        EXPECT_GT(median, 0u) << "bloom downsample: timing query returned 0 ns";
        EXPECT_LT(median, 100 * 1000 * 1000ull) << "bloom downsample >100 ms at 512^2";
        RecordProperty("bloom_downsample_512x512_ns_median", std::to_string(median));
    }

    // Bloom upsample pass: 9-tap tent filter. Classic Call-of-Duty style.
    TEST(PerfRegressionTest, BloomUpsamplePassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassNs(
            "assets/shaders/PostProcess_BloomUpsample.glsl", 512, 512);
        EXPECT_GT(median, 0u) << "bloom upsample: timing query returned 0 ns";
        EXPECT_LT(median, 100 * 1000 * 1000ull) << "bloom upsample >100 ms at 512^2";
        RecordProperty("bloom_upsample_512x512_ns_median", std::to_string(median));
    }
} // namespace OloEngine::Tests
