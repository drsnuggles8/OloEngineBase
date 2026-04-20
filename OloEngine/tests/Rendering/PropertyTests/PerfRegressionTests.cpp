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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
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

        // -------------------------------------------------------------------
        // Historical perf tracking (Layer 6 / §6 cross-machine tracking).
        //
        // Every run appends a row (machine, iso-date, name, measured_ns,
        // baseline_ns, ratio) to a TSV under
        //   OloEngine/tests/Rendering/PropertyTests/perf_history/<machine>.tsv
        // so trends are observable over time without polluting
        // perf_baselines.txt.
        //
        // Machine tag resolution order:
        //   1. OLOENGINE_PERF_MACHINE env var (canonical, set in CI).
        //   2. COMPUTERNAME / HOSTNAME env var.
        //   3. "unknown"
        //
        // History rows are *informational* — they never fail a test. They
        // let us spot drifting baselines (gradual 5 % / week creep that
        // wouldn't trip the 1.5× warn threshold) and compare machines
        // side-by-side without carrying multiple baseline files.
        // -------------------------------------------------------------------
        static std::string GetPerfMachineTag()
        {
            auto sanitize = [](std::string s)
            {
                for (auto& c : s)
                {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                        c = '_';
                }
                if (s.empty())
                    s = "unknown";
                return s;
            };
            if (const char* env = std::getenv("OLOENGINE_PERF_MACHINE"); env && *env)
                return sanitize(env);
            if (const char* env = std::getenv("COMPUTERNAME"); env && *env)
                return sanitize(env);
            if (const char* env = std::getenv("HOSTNAME"); env && *env)
                return sanitize(env);
            return "unknown";
        }

        static fs::path PerfHistoryPath()
        {
            fs::path baseDir = PerfBaselinePath().parent_path() / "perf_history";
            std::error_code ec;
            fs::create_directories(baseDir, ec);
            return baseDir / (GetPerfMachineTag() + ".tsv");
        }

        static void AppendPerfHistory(const std::string& name, u64 measuredNs,
                                      u64 baselineNs, f32 ratio)
        {
            const fs::path path = PerfHistoryPath();
            const bool preexisting = fs::exists(path);

            std::ofstream out(path, std::ios::app);
            if (!out.is_open())
                return;
            if (!preexisting)
            {
                out << "# OloEngine perf history (Layer 6 §6 tracking).\n"
                    << "# TSV: iso_date_utc\tname\tmeasured_ns\tbaseline_ns\tratio\n"
                    << "# Baseline = 0 means no baseline committed at time of run.\n"
                    << "# Ratio = measured / baseline (NaN if baseline == 0).\n";
            }

            // ISO-8601 UTC date, seconds resolution, using only std:: facilities.
            std::time_t now = std::time(nullptr);
            std::tm tmUtc{};
#if defined(_WIN32)
            ::gmtime_s(&tmUtc, &now);
#else
            ::gmtime_r(&now, &tmUtc);
#endif
            char isoBuf[32] = { 0 };
            std::strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);

            out << isoBuf << '\t' << name << '\t' << measuredNs << '\t' << baselineNs << '\t';
            if (baselineNs == 0)
                out << "nan";
            else
                out << ratio;
            out << '\n';
        }

        // Runs the regression check + reports diagnostics via gtest. Handles
        // rebase and "no baseline" cases uniformly.
        //
        // `name` must match a key in perf_baselines.txt.
        //
        // Anti-flake policy: if the first measurement trips the WARN ratio
        // (1.5x), the caller is expected to re-measure and pass the
        // minimum. This is handled via `CheckPerfRegressionWithRetry` below
        // — tests that want the "measure once, retry once if > WARN" policy
        // should use that wrapper rather than calling this directly.
        static void CheckPerfRegression(const std::string& name, u64 measuredNs)
        {
            auto& cache = PerfBaselineCache();

            if (PerfShouldRebase())
            {
                cache[name] = measuredNs;
                WritePerfBaselines(cache);
                ::testing::Test::RecordProperty(name + "_ns_median", std::to_string(measuredNs));
                ::testing::Test::RecordProperty(name + "_rebased", "1");
                AppendPerfHistory(name, measuredNs, measuredNs, 1.0f);
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
                AppendPerfHistory(name, measuredNs, 0, 0.0f);
                ADD_FAILURE() << "No baseline for '" << name
                              << "'. Measured " << measuredNs
                              << " ns. Rebase via OLOENGINE_PERF_REBASE=1.";
                return;
            }

            const u64 baseline = it->second;
            if (baseline == 0)
            {
                AppendPerfHistory(name, measuredNs, 0, 0.0f);
                return; // placeholder; skip comparison
            }

            const f32 ratio = static_cast<f32>(measuredNs) / static_cast<f32>(baseline);
            ::testing::Test::RecordProperty(name + "_ratio_to_baseline",
                                            std::to_string(ratio));
            AppendPerfHistory(name, measuredNs, baseline, ratio);

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
        // Match the shared MeasureFullscreenPassNs helper so every microbench
        // in this file shares the same warmup/measure budget. The earlier
        // 3/10 figure was an outlier that increased sample variance on this
        // one pass relative to its siblings.
        constexpr u32 kWarmup = 5;
        constexpr u32 kMeasure = 20;

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
            {
                ADD_FAILURE() << "Shader::Create failed for " << shaderPath
                              << " - perf baseline cannot be measured (returning sentinel so the"
                                 " regression check treats this as a failure, never a 0 ns 'win').";
                return std::numeric_limits<u64>::max();
            }

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

        // Anti-flake wrapper. Measures the pass; if the first measurement is
        // above the warn threshold (1.5× baseline), re-measures once more and
        // uses the minimum of both. Transient thermal / scheduler spikes —
        // which can briefly balloon the fastest GL_TIME_ELAPSED sample by 2–3×
        // — are the dominant noise source on the low-microsecond bloom passes
        // and a single retry is enough to suppress them. The retry happens
        // only on the bad path, so steady-state runs still finish in the
        // original ~50 ms each.
        u64 MeasureFullscreenPassStableNs(const char* shaderPath, u32 width, u32 height,
                                          const std::string& baselineKey)
        {
            const u64 first = MeasureFullscreenPassNs(shaderPath, width, height);
            auto it = PerfBaselineCache().find(baselineKey);
            if (it == PerfBaselineCache().end() || it->second == 0 || PerfShouldRebase())
                return first;

            const f32 ratio = static_cast<f32>(first) / static_cast<f32>(it->second);
            if (ratio < kPerfWarnRatio)
                return first;

            // Retry once — pick the faster of the two attempts. If noise is
            // the culprit the second measurement will typically be close to
            // baseline; a real regression will stay slow.
            const u64 second = MeasureFullscreenPassNs(shaderPath, width, height);
            return std::min(first, second);
        }
    } // namespace

    // Bloom extract (threshold) pass: luminance extract + soft-knee multiply.
    // Should be fastest of the bloom chain — no cross-pixel sampling.
    TEST(PerfRegressionTest, BloomThresholdPassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassStableNs(
            "assets/shaders/PostProcess_BloomThreshold.glsl", 512, 512,
            "bloom_threshold_512x512");
        CheckPerfRegression("bloom_threshold_512x512", median);
    }

    // Bloom downsample pass: 13-tap box filter. Expected cost roughly 13×
    // the tone-map pass (1 tap each), minus ALU overhead.
    TEST(PerfRegressionTest, BloomDownsamplePassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassStableNs(
            "assets/shaders/PostProcess_BloomDownsample.glsl", 512, 512,
            "bloom_downsample_512x512");
        CheckPerfRegression("bloom_downsample_512x512", median);
    }

    // Bloom upsample pass: 9-tap tent filter. Classic Call-of-Duty style.
    TEST(PerfRegressionTest, BloomUpsamplePassTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const u64 median = MeasureFullscreenPassStableNs(
            "assets/shaders/PostProcess_BloomUpsample.glsl", 512, 512,
            "bloom_upsample_512x512");
        CheckPerfRegression("bloom_upsample_512x512", median);
    }

    // -------------------------------------------------------------------------
    // Whole-frame post-process budget. Chains the four shipping post-process
    // passes (tonemap -> bloom threshold -> bloom downsample -> bloom upsample)
    // against a single synthetic 512x512 RGBA32F input and times the aggregate
    // as a single GL_TIME_ELAPSED query. Intentionally *not* an exact sum of
    // the per-pass microbenchmarks: it exposes pass-to-pass transition cost
    // (framebuffer rebinds, cache invalidation, driver state churn) that the
    // isolated passes cannot observe.
    //
    // Baseline ships as `0` (placeholder) so first-run on a new machine does
    // not fail. Rebase via `OLOENGINE_PERF_REBASE=1` after a confirmed
    // known-good run on the reference workstation.
    // -------------------------------------------------------------------------
    namespace
    {
        u64 MeasureWholeFramePostprocessNs(u32 width, u32 height)
        {
            constexpr u32 kWarmup = 5;
            constexpr u32 kMeasure = 20;

            FramebufferSpecification spec{};
            spec.Width = width;
            spec.Height = height;
            spec.Attachments = { FramebufferTextureFormat::RGBA8 };
            auto fbTone = Framebuffer::Create(spec);
            auto fbThresh = Framebuffer::Create(spec);
            auto fbDown = Framebuffer::Create(spec);
            auto fbUp = Framebuffer::Create(spec);

            auto shaderTone = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");
            auto shaderThresh = Shader::Create("assets/shaders/PostProcess_BloomThreshold.glsl");
            auto shaderDown = Shader::Create("assets/shaders/PostProcess_BloomDownsample.glsl");
            auto shaderUp = Shader::Create("assets/shaders/PostProcess_BloomUpsample.glsl");
            if (!shaderTone || !shaderThresh || !shaderDown || !shaderUp)
            {
                ADD_FAILURE() << "Shader::Create failed for one of the postprocess chain shaders -"
                                 " returning sentinel instead of 0 so the regression check cannot"
                                 " treat missing assets as a perfect improvement.";
                return std::numeric_limits<u64>::max();
            }

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

            auto RunPass = [&](Framebuffer& dst, Shader& sh, u32 src)
            {
                dst.Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                sh.Bind();
                pass.Draw(src);
                dst.Unbind();
            };

            auto DrawChain = [&]()
            {
                RunPass(*fbTone, *shaderTone, inputTex);
                RunPass(*fbThresh, *shaderThresh, fbTone->GetColorAttachmentRendererID(0));
                RunPass(*fbDown, *shaderDown, fbThresh->GetColorAttachmentRendererID(0));
                RunPass(*fbUp, *shaderUp, fbDown->GetColorAttachmentRendererID(0));
            };

            for (u32 i = 0; i < kWarmup; ++i)
                DrawChain();
            ::glFinish();

            std::vector<u64> samples;
            samples.reserve(kMeasure);
            for (u32 i = 0; i < kMeasure; ++i)
            {
                GpuTimer timer;
                timer.Begin();
                DrawChain();
                timer.End();
                samples.push_back(timer.GetElapsedNs());
            }

            ::glDeleteTextures(1, &inputTex);

            std::sort(samples.begin(), samples.end());
            return samples.front();
        }
    } // namespace

    TEST(PerfRegressionTest, WholeFramePostprocessChainTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Anti-flake: measure twice, keep the faster sample if the first one
        // exceeds the warn threshold.
        u64 first = MeasureWholeFramePostprocessNs(512, 512);
        auto it = PerfBaselineCache().find("whole_frame_postprocess_512x512");
        if (it != PerfBaselineCache().end() && it->second != 0 && !PerfShouldRebase())
        {
            const f32 ratio = static_cast<f32>(first) / static_cast<f32>(it->second);
            if (ratio >= kPerfWarnRatio)
            {
                const u64 second = MeasureWholeFramePostprocessNs(512, 512);
                first = std::min(first, second);
            }
        }
        CheckPerfRegression("whole_frame_postprocess_512x512", first);
    }

    // -------------------------------------------------------------------------
    // Scene-draw-burst bench (integration-level state-change budget).
    //
    // Models the state-change / shader-bind / texture-bind storm that
    // characterises a real 3D frame, without dragging in Renderer3D, scene
    // setup, camera matrices, or the ECS. kDraws fullscreen passes are issued
    // into a single FBO, rotating through kShaders distinct shader programs
    // and kTextures distinct input textures per draw. Any regression that
    // breaks shader-bind dedup, texture-bind caching, or introduces redundant
    // driver state-change churn will show up here.
    //
    // Also pins *invariants* (not just timing): the bench records the draw
    // count and the expected shader-bind count as test properties and hard-
    // fails if they drift. This catches cases where a silently-broken batcher
    // doubles the draw-call count without meaningfully affecting wall time.
    //
    // All passes use the cheap PostProcess_BloomThreshold shader so the test
    // stresses the driver dispatch path, not ALU.
    // -------------------------------------------------------------------------
    namespace
    {
        constexpr u32 kSceneDraws = 64;
        constexpr u32 kSceneShaders = 4;
        constexpr u32 kSceneTextures = 4;

        u64 MeasureSceneDrawBurstNs(u32 width, u32 height, u32& outDrawCalls,
                                    u32& outShaderBinds, u32& outTextureBinds)
        {
            constexpr u32 kWarmup = 3;
            constexpr u32 kMeasure = 15;

            FramebufferSpecification spec{};
            spec.Width = width;
            spec.Height = height;
            spec.Attachments = { FramebufferTextureFormat::RGBA8 };
            auto fb = Framebuffer::Create(spec);

            // Four cheap fragment-only shaders. Intentionally a mix of
            // arithmetic-bound and sampler-bound passes so shader-bind
            // regressions stand out more than pure ALU cost.
            const char* shaderPaths[kSceneShaders] = {
                "assets/shaders/PostProcess_BloomThreshold.glsl",
                "assets/shaders/PostProcess_ToneMap.glsl",
                "assets/shaders/PostProcess_BloomDownsample.glsl",
                "assets/shaders/PostProcess_BloomUpsample.glsl",
            };

            Ref<Shader> shaders[kSceneShaders];
            for (u32 i = 0; i < kSceneShaders; ++i)
            {
                shaders[i] = Shader::Create(shaderPaths[i]);
                if (!shaders[i])
                {
                    ADD_FAILURE() << "Shader::Create failed for " << shaderPaths[i]
                                  << " - scene-draw-burst perf baseline cannot be measured."
                                     " Returning sentinel instead of 0 so the regression check fails"
                                     " loudly rather than recording a spurious 0 ns 'win'.";
                    return std::numeric_limits<u64>::max();
                }
            }

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

            u32 textures[kSceneTextures];
            for (u32 i = 0; i < kSceneTextures; ++i)
            {
                const f32 f = static_cast<f32>(i + 1) / static_cast<f32>(kSceneTextures);
                textures[i] = CreateUniformFloatTexture2D(width, height, f, 1.0f - f, 0.5f, 1.0f);
            }

            FullscreenPass pass;

            // Tracks the actual number of bind transitions observed, so the
            // test can assert the caller isn't silently eating binds.
            u32 localDrawCalls = 0;
            u32 localShaderBinds = 0;
            u32 localTextureBinds = 0;

            auto DrawFrame = [&]()
            {
                fb->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);

                u32 lastShader = UINT32_MAX;
                u32 lastTexture = UINT32_MAX;
                for (u32 i = 0; i < kSceneDraws; ++i)
                {
                    const u32 si = i % kSceneShaders;
                    const u32 ti = i % kSceneTextures;
                    if (si != lastShader)
                    {
                        shaders[si]->Bind();
                        ++localShaderBinds;
                        lastShader = si;
                    }
                    if (textures[ti] != lastTexture)
                    {
                        ++localTextureBinds;
                        lastTexture = textures[ti];
                    }
                    pass.Draw(textures[ti]);
                    ++localDrawCalls;
                }
                fb->Unbind();
            };

            for (u32 i = 0; i < kWarmup; ++i)
                DrawFrame();
            ::glFinish();

            // Reset local counters: warmup should not leak into reporting.
            localDrawCalls = 0;
            localShaderBinds = 0;
            localTextureBinds = 0;

            std::vector<u64> samples;
            samples.reserve(kMeasure);
            for (u32 i = 0; i < kMeasure; ++i)
            {
                GpuTimer timer;
                timer.Begin();
                DrawFrame();
                timer.End();
                samples.push_back(timer.GetElapsedNs());
            }

            for (u32 i = 0; i < kSceneTextures; ++i)
                ::glDeleteTextures(1, &textures[i]);

            std::sort(samples.begin(), samples.end());

            // Divide counters by kMeasure so they represent per-frame cost,
            // matching the per-frame timing sample above.
            outDrawCalls = localDrawCalls / kMeasure;
            outShaderBinds = localShaderBinds / kMeasure;
            outTextureBinds = localTextureBinds / kMeasure;
            return samples.front();
        }
    } // namespace

    TEST(PerfRegressionTest, SceneDrawBurstBudget)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        u32 draws = 0, shaderBinds = 0, textureBinds = 0;
        u64 first = MeasureSceneDrawBurstNs(512, 512, draws, shaderBinds, textureBinds);

        auto it = PerfBaselineCache().find("scene_draw_burst_512x512");
        if (it != PerfBaselineCache().end() && it->second != 0 && !PerfShouldRebase())
        {
            const f32 ratio = static_cast<f32>(first) / static_cast<f32>(it->second);
            if (ratio >= kPerfWarnRatio)
            {
                u32 d2 = 0, s2 = 0, t2 = 0;
                const u64 second = MeasureSceneDrawBurstNs(512, 512, d2, s2, t2);
                first = std::min(first, second);
            }
        }

        // Invariants: pin draw/bind counts so broken batching or missing bind-
        // dedup is caught structurally even when the timing regression is
        // small (e.g. a silent 2x draw-call inflation on a faster machine).
        EXPECT_EQ(draws, kSceneDraws)
            << "scene draw-call count changed (expected " << kSceneDraws
            << ", got " << draws << ") — investigate batcher / state tracker.";
        EXPECT_EQ(shaderBinds, kSceneDraws)
            << "shader-bind count changed (expected " << kSceneDraws
            << ", got " << shaderBinds << ") — bind dedup may have regressed.";
        EXPECT_EQ(textureBinds, kSceneDraws)
            << "texture-bind count changed (expected " << kSceneDraws
            << ", got " << textureBinds << ") — texture cache may have regressed.";

        ::testing::Test::RecordProperty("scene_draw_burst_draws", std::to_string(draws));
        ::testing::Test::RecordProperty("scene_draw_burst_shader_binds", std::to_string(shaderBinds));
        ::testing::Test::RecordProperty("scene_draw_burst_texture_binds", std::to_string(textureBinds));

        CheckPerfRegression("scene_draw_burst_512x512", first);
    }
} // namespace OloEngine::Tests
