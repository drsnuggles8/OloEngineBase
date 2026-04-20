// =============================================================================
// PerfRegressionTests.cpp
//
// Layer-6 (Performance Regression, doc section 6) microbenchmarks. Wronski's
// "microbenchmarks with controlled inputs" philosophy — isolate individual
// post-process passes on fixed-size synthetic inputs and measure GPU time
// via GL timestamp queries, then compare against a dev-PC baseline stored
// in `perf_baselines.txt` (tracked in git, rebased via the
// `OLOENGINE_PERF_REBASE=1` env var).
//
// Regression policy:
//   - Measured <= baseline  : PASS silently (improvements are welcome)
//   - Measured up to 1.5x   : PASS (noise band; these are sub-10 µs passes)
//   - Measured 1.5x .. 2.5x : ADD_FAILURE as a WARN (surfaces as a test
//                             failure in Debug so regressions show up in CI
//                             output, but the number itself is a warning)
//   - Measured > 2.5x       : hard FAIL
//
// We aggregate 20 measurement iterations (after 5 warmup draws) and take
// the MINIMUM of the samples, not the median. For pure GPU microbenchmarks
// in the low-µs range, thermal throttling and OS scheduling jitter only
// ever *lengthen* timings; the fastest run is the cleanest signal.
//
// These tests deliberately target a single developer workstation — not a CI
// runner. Different hardware will produce wildly different numbers, and
// until a cross-machine perf harness is stood up (non-goal for this PR),
// the baselines live next to the tests and are rebased by the engine's
// maintainer on known-good commits.
//
// Currently measured:
//   1. Tone map pass GPU time at 512×512 RGBA8 (Reinhard)
//   2. Bloom threshold pass GPU time at 512×512
//   3. Bloom downsample pass GPU time at 512×512 (13-tap box)
//   4. Bloom upsample pass GPU time at 512×512 (9-tap tent)
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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Regression thresholds relative to baseline. Improvements never fail.
        // Thresholds are loose because these microbenchmarks run in the low
        // microsecond range where driver scheduling + GPU idle-ramp add
        // nontrivial variance even after 20 samples-min aggregation.
        constexpr f32 kPerfWarnRatio = 1.50f;
        constexpr f32 kPerfFailRatio = 2.50f;

        // Absolute ceiling to catch pathological regressions even when no
        // baseline is present (still enforced alongside the ratio check).
        constexpr u64 kSanityCeilingNs = 100 * 1000 * 1000ull; // 100 ms

        static bool PerfShouldRebase()
        {
            const char* env = std::getenv("OLOENGINE_PERF_REBASE");
            if (!env)
                return false;
            std::string s(env);
            for (auto& c : s)
                c = static_cast<char>(std::tolower(c));
            return !(s == "0" || s == "false" || s.empty());
        }

        // Locate the perf_baselines.txt file. We try a small list of
        // candidate paths to be robust against varying working directories
        // (tests are typically run from the repo root via the VS Code task,
        // but IDE test runners often run from the build output dir).
        static fs::path PerfBaselinePath()
        {
            const fs::path relFromRoot = fs::path("OloEngine") / "tests" / "Rendering" / "PropertyTests" / "perf_baselines.txt";

            // Walk up from CWD looking for a match. Covers:
            //   - run from repo root (cwd/OloEngine/tests/...)
            //   - run from build/OloEngine/tests/Debug (walk up 4 levels)
            fs::path cwd = fs::current_path();
            for (int i = 0; i < 8; ++i)
            {
                fs::path candidate = cwd / relFromRoot;
                if (fs::exists(candidate))
                    return candidate;
                if (!cwd.has_parent_path() || cwd == cwd.parent_path())
                    break;
                cwd = cwd.parent_path();
            }
            // Fallback: next to this translation unit at build time.
            return fs::path(__FILE__).parent_path() / "perf_baselines.txt";
        }

        static std::unordered_map<std::string, u64>& PerfBaselineCache()
        {
            static std::unordered_map<std::string, u64> cache;
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                fs::path path = PerfBaselinePath();
                std::ifstream in(path);
                if (in.is_open())
                {
                    std::string line;
                    while (std::getline(in, line))
                    {
                        // Strip comments and whitespace.
                        auto hash = line.find('#');
                        if (hash != std::string::npos)
                            line.erase(hash);
                        std::istringstream ls(line);
                        std::string key;
                        u64 value = 0;
                        if (ls >> key >> value)
                            cache[key] = value;
                    }
                }
            }
            return cache;
        }

        // Writes the current baseline map back to disk, preserving the
        // comment header (recreated from scratch since the file format is
        // append-only simple).
        static void WritePerfBaselines(const std::unordered_map<std::string, u64>& baselines)
        {
            fs::path path = PerfBaselinePath();
            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open())
                return;
            out << "# Performance baselines for Layer-6 microbenchmarks.\n"
                << "#\n"
                << "# Format: one benchmark per line, \"<key> <minimum_ns>\".\n"
                << "# Lines starting with '#' are comments.\n"
                << "#\n"
                << "# Captured on the development machine \xe2\x80\x94 NOT a CI runner. These baselines\n"
                << "# are authoritative for regression detection on the same hardware; numbers\n"
                << "# will differ (sometimes by an order of magnitude) on other GPUs.\n"
                << "#\n"
                << "# To rebase (after confirming the change is expected):\n"
                << "#   set OLOENGINE_PERF_REBASE=1\n"
                << "#   run-tests-debug\n"
                << "# The test harness will rewrite this file with fresh numbers from the\n"
                << "# current run and pass unconditionally while the env var is set.\n"
                << "#\n"
                << "# Regression policy (see PerfRegressionTests.cpp):\n"
                << "#   - Measured >= 2.5x baseline  -> FAIL\n"
                << "#   - Measured >= 1.5x baseline  -> WARN (ADD_FAILURE; still reported as a\n"
                << "#                                         failure, but classified as a warn\n"
                << "#                                         in the message text)\n"
                << "#   - Faster than baseline       -> PASS, never fail on improvements\n"
                << "#\n"
                << "# Metric: minimum of 20 GL_TIME_ELAPSED samples after 5 warmup draws.\n"
                << "# The minimum (not median) is used because driver scheduling + GPU\n"
                << "# idle-ramp can only add time; the fastest run is the cleanest signal.\n\n";
            // Sort for stability.
            std::vector<std::string> keys;
            keys.reserve(baselines.size());
            for (const auto& [k, _] : baselines)
                keys.push_back(k);
            std::sort(keys.begin(), keys.end());
            for (const auto& k : keys)
                out << k << " " << baselines.at(k) << "\n";
        }

        // Runs the regression check + reports diagnostics via gtest. Handles
        // rebase and "no baseline" cases uniformly.
        //
        // `name` must match a key in perf_baselines.txt.
        static void CheckPerfRegression(const std::string& name, u64 measuredNs)
        {
            auto& cache = PerfBaselineCache();

            if (PerfShouldRebase())
            {
                cache[name] = measuredNs;
                WritePerfBaselines(cache);
                ::testing::Test::RecordProperty(name + "_ns_median", std::to_string(measuredNs));
                ::testing::Test::RecordProperty(name + "_rebased", "1");
                return;
            }

            ::testing::Test::RecordProperty(name + "_ns_median", std::to_string(measuredNs));

            // Sanity ceiling always applies.
            EXPECT_LT(measuredNs, kSanityCeilingNs)
                << name << " took " << measuredNs << " ns (> " << kSanityCeilingNs
                << " ns sanity ceiling — something is catastrophically wrong)";

            auto it = cache.find(name);
            if (it == cache.end())
            {
                // Missing baseline = gentle reminder; don't fail.
                ADD_FAILURE() << "No baseline for '" << name
                              << "'. Measured " << measuredNs
                              << " ns. Rebase via OLOENGINE_PERF_REBASE=1.";
                return;
            }

            const u64 baseline = it->second;
            if (baseline == 0)
                return; // placeholder; skip comparison

            const f32 ratio = static_cast<f32>(measuredNs) / static_cast<f32>(baseline);
            ::testing::Test::RecordProperty(name + "_ratio_to_baseline",
                                            std::to_string(ratio));

            if (ratio >= kPerfFailRatio)
            {
                FAIL() << name << " PERF REGRESSION: " << measuredNs
                       << " ns vs baseline " << baseline << " ns ("
                       << ratio << "x; threshold " << kPerfFailRatio << "x)";
            }
            else if (ratio >= kPerfWarnRatio)
            {
                ADD_FAILURE() << name << " perf warning: " << measuredNs
                              << " ns vs baseline " << baseline << " ns ("
                              << ratio << "x; warn threshold "
                              << kPerfWarnRatio << "x)";
            }
        }

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
        // Use minimum across iterations: for pure GPU microbenchmarks the
        // minimum is the most stable metric (thermal/scheduling jitter only
        // ever lengthens timings; the "fastest run" is the purest signal).
        const u64 minNs = samples.front();

        CheckPerfRegression("tone_map_512x512", minNs);

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
            constexpr u32 kWarmup = 5;
            constexpr u32 kMeasure = 20;

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
            // Minimum across iterations — see rationale in ToneMap test.
            return samples.front();
        }
    } // namespace

    // Bloom extract (threshold) pass: luminance extract + soft-knee multiply.
    // Should be fastest of the bloom chain — no cross-pixel sampling.
    TEST(PerfRegressionTest, BloomThresholdPassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassNs(
            "assets/shaders/PostProcess_BloomThreshold.glsl", 512, 512);
        CheckPerfRegression("bloom_threshold_512x512", median);
    }

    // Bloom downsample pass: 13-tap box filter. Expected cost roughly 13×
    // the tone-map pass (1 tap each), minus ALU overhead.
    TEST(PerfRegressionTest, BloomDownsamplePassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassNs(
            "assets/shaders/PostProcess_BloomDownsample.glsl", 512, 512);
        CheckPerfRegression("bloom_downsample_512x512", median);
    }

    // Bloom upsample pass: 9-tap tent filter. Classic Call-of-Duty style.
    TEST(PerfRegressionTest, BloomUpsamplePassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassNs(
            "assets/shaders/PostProcess_BloomUpsample.glsl", 512, 512);
        CheckPerfRegression("bloom_upsample_512x512", median);
    }
} // namespace OloEngine::Tests
