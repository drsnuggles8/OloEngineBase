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
// Regression policy (ratio of measured to baseline):
//   - <= baseline      : within budget (improvements are welcome)
//   - up to 1.5x       : noise band; these are sub-10 µs passes
//   - 1.5x .. 2.5x     : WARN-level regression
//   - > 2.5x           : FAIL-level regression
//
// Enforcement is OPT-IN. By default a WARN/FAIL-level regression (and the
// 100 ms sanity-ceiling breach) is RECORDED — gtest property + perf_history
// TSV — and logged via GTEST_LOG_(WARNING), but does NOT fail the test. These
// low-µs GPU microbenchmarks flake under machine/GPU contention: a loaded
// full-suite run inflates every sample 1.5–3.4x even though the same test
// passes in isolation (issue #324), so failing on the number eroded trust in
// the suite without the perf data ever gating CI (they SKIP on GPU-less
// runners). Set OLOENGINE_PERF_STRICT=1 for a deliberate regression gate on a
// quiet machine; regressions otherwise stay observable via the recorded
// history + perf_trend.py. (The SceneDrawBurst draw/bind-count invariants are
// structural, not timing, and still hard-fail unconditionally.)
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
//   5. Whole-frame post-process chain at 512×512
//   6. Scene draw burst at 512×512 (classic mesh path)
//   7. Virtualized geometry (Nanite, issue #629) — see the block at the bottom
//      of this file. The virtual path had NO perf coverage at all until then:
//      ~43 tests asserted it was CORRECT and not one asserted it was FAST, which
//      is uncomfortable given that being fast is the entire reason it exists. A
//      3x regression in the cluster cull would have been invisible.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
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

        // Whether a regression should FAIL the test (strict mode) or merely be
        // recorded + logged (the default). These are low-µs GPU microbenchmarks
        // pinned to one dev workstation's baselines; under machine/GPU
        // contention a full-suite run can inflate every sample 1.5–3.4× and trip
        // the thresholds, even though the same test passes cleanly in isolation
        // (issue #324). Defaulting to non-fatal stops that flakiness from eroding
        // trust in the suite while still capturing every measurement (gtest
        // properties + perf_history TSV) so regressions stay observable via the
        // trend tool. Set OLOENGINE_PERF_STRICT=1 for a deliberate regression
        // gate on a quiet machine. Same env-var convention as PerfShouldRebase().
        static bool PerfShouldEnforce()
        {
            const char* env = std::getenv("OLOENGINE_PERF_STRICT");
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
                if (fs::path candidate = cwd / relFromRoot; fs::exists(candidate))
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
            if (static bool loaded = false; !loaded)
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
                        if (auto hash = line.find('#'); hash != std::string::npos)
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
                << "# Regression policy (see PerfRegressionTests.cpp). Enforcement is\n"
                << "# OPT-IN: by default both WARN- and FAIL-level regressions are RECORDED\n"
                << "# (gtest property + perf_history TSV) and logged via GTEST_LOG_(WARNING),\n"
                << "# but do NOT fail the test \xe2\x80\x94 these low-microsecond GPU microbenchmarks\n"
                << "# flake under machine/GPU contention (issue #324). Set\n"
                << "# OLOENGINE_PERF_STRICT=1 to turn a regression into an ADD_FAILURE\n"
                << "# failure on a quiet machine.\n"
                << "#   - Measured >= 2.5x baseline  -> FAIL-level (fails only when STRICT)\n"
                << "#   - Measured >= 1.5x baseline  -> WARN-level (fails only when STRICT)\n"
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
            std::ranges::sort(keys);
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

            // Strict mode fails on a regression; the default records + logs it
            // but never fails (see PerfShouldEnforce — these microbenchmarks
            // flake under contention, issue #324). The non-fatal path still
            // prints to the test log so a regression stays visible without
            // breaking an ordinary full-suite run.
            const bool enforce = PerfShouldEnforce();
            const auto note = [enforce](const std::string& detail)
            {
                if (enforce)
                    ADD_FAILURE() << detail;
                else
                    GTEST_LOG_(WARNING) << "[perf, non-strict] " << detail
                                        << " — set OLOENGINE_PERF_STRICT=1 to fail on this.";
            };

            // Sanity ceiling: a µs-scale pass taking >100 ms is a catastrophe,
            // not contention noise. Still routed through `note`, so a wedged
            // machine can't break an ordinary (non-strict) run either.
            if (measuredNs >= kSanityCeilingNs)
            {
                note(name + " took " + std::to_string(measuredNs) + " ns (> " + std::to_string(kSanityCeilingNs) + " ns sanity ceiling — something is catastrophically wrong)");
            }

            auto it = cache.find(name);
            if (it == cache.end())
            {
                // Missing baseline = gentle reminder; rebase via OLOENGINE_PERF_REBASE=1.
                AppendPerfHistory(name, measuredNs, 0, 0.0f);
                note("No baseline for '" + name + "'. Measured " + std::to_string(measuredNs) + " ns. Rebase via OLOENGINE_PERF_REBASE=1.");
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

            std::ostringstream ratioMsg;
            ratioMsg << name << ": " << measuredNs << " ns vs baseline " << baseline
                     << " ns (" << ratio << "x)";
            if (ratio >= kPerfFailRatio)
            {
                note("PERF REGRESSION " + ratioMsg.str() + "; fail threshold " + std::to_string(kPerfFailRatio) + "x");
            }
            else if (ratio >= kPerfWarnRatio)
            {
                note("perf warning " + ratioMsg.str() + "; warn threshold " + std::to_string(kPerfWarnRatio) + "x");
            }
            else
            {
                // Within the noise band — no report.
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

        std::ranges::sort(samples);
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

            std::ranges::sort(samples);
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

            if (const f32 ratio = static_cast<f32>(first) / static_cast<f32>(it->second); ratio < kPerfWarnRatio)
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

            std::ranges::sort(samples);
            return samples.front();
        }
    } // namespace

    TEST(PerfRegressionTest, WholeFramePostprocessChainTimingIsMeasurable)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Anti-flake: measure twice, keep the faster sample if the first one
        // exceeds the warn threshold.
        u64 first = MeasureWholeFramePostprocessNs(512, 512);
        if (auto it = PerfBaselineCache().find("whole_frame_postprocess_512x512"); it != PerfBaselineCache().end() && it->second != 0 && !PerfShouldRebase())
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

            std::ranges::sort(samples);

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

        if (auto it = PerfBaselineCache().find("scene_draw_burst_512x512"); it != PerfBaselineCache().end() && it->second != 0 && !PerfShouldRebase())
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

    // =========================================================================
    // Virtualized geometry (Nanite-style cluster LOD DAG, issue #629)
    //
    // Until this block, the virtual renderer had ZERO performance coverage. It
    // had ~43 tests proving it renders CORRECTLY — monotone DAG error, watertight
    // cuts, SW-vs-HW pixel parity, Hi-Z culling, streaming, shadows — and not one
    // proving it renders FAST, which is the entire reason the subsystem exists.
    // A change that made the cluster cull 3x slower would have left every test
    // green.
    //
    // Four measurements, in two deliberate PAIRS. A lone number ("the virtual
    // path takes N ms") is nearly meaningless — it drifts with the driver, the
    // GPU clock, and the scene. A RATIO between two paths rendering the SAME mesh
    // in the SAME frame is the thing that actually carries a claim:
    //
    //   virtual_geometry_frame vs classic_geometry_frame
    //       -> what virtualized geometry actually buys on this mesh. Enabled by
    //          the new RendererSettings::VirtualGeometryEnabled master switch,
    //          which falls a VirtualMeshComponent back to the classic mesh path
    //          instead of dropping it — same geometry, same materials, only the
    //          renderer differs. The engine could not measure this before,
    //          because there was no way to render the same scene both ways.
    //
    //   virtual_swraster_frame vs virtual_hwraster_frame
    //       -> the compute software rasterizer vs the hardware MDI path. Nanite's
    //          core claim is that sub-pixel triangles rasterize FASTER in compute
    //          than through the hardware rasterizer; this pair is where that claim
    //          becomes falsifiable rather than folklore.
    //
    // BOTH PATHS MUST BE DOING THE SAME WORK, and that is harder to guarantee than it looks.
    // This scene has a shadow-casting sun and no classic mesh, and for a while the virtual path
    // was silently casting NO SHADOWS at all (ShadowRenderPass only counted classic casters, so
    // with no classic mesh present it skipped the whole pass — fixed, and pinned by
    // VirtualGeometryVisualEvidence.VirtualMeshCastsWhenItIsTheOnlyCasterInTheScene). The
    // benchmark happily reported the virtual path as 6.4x FASTER, and a good part of that lead
    // was simply work it was not doing. Post-fix the honest figure is ~1.7x.
    //
    // The lesson generalises past this one bug: a perf A/B is only as trustworthy as the
    // guarantee that both sides render the same frame. The structural invariants below (cluster
    // counts, triangle counts) exist to enforce exactly that, and they should be EXTENDED, not
    // trusted as sufficient, whenever a new subsystem joins the frame.
    //
    // Timed with glFinish + wall clock rather than a GL_TIME_ELAPSED query,
    // unlike the microbenchmarks above: these drive a full engine frame through
    // RunEditorFrames, and the renderer's own profiler already has GL_TIME_ELAPSED
    // queries open during it. GL_TIME_ELAPSED queries cannot nest, so wrapping the
    // frame in one is a conflict, not a measurement. At whole-frame scale (hundreds
    // of µs to ms) the glFinish bracket is the honest instrument anyway.
    //
    // No entries are added to perf_baselines.txt here on purpose: a missing
    // baseline RECORDS rather than fails, so the first run on a real GPU is the
    // rebase (OLOENGINE_PERF_REBASE=1). These SKIP with no GL context, so they
    // never run on a free CI runner — that is expected, not a gap.
    // =========================================================================

    namespace
    {
        // 1280x720, not 640x360: the fixed per-pixel cost (deferred lighting, post) scales with
        // pixels while the geometry cost does not, so a small viewport makes the frame LESS
        // geometry-bound, which is the opposite of what this benchmark needs.
        constexpr u32 kVgWidth = 1280;
        constexpr u32 kVgHeight = 720;

        // The geometry load. 48 x 327,680 = ~15.7 MILLION triangles per frame through the
        // classic path — enough that the vertex stage and rasterizer, not the lighting, own the
        // frame. The virtual path replaces that with a screen-space-error-bounded DAG cut.
        constexpr u32 kSphereSubdivisions = 7;
        constexpr u32 kSphereTriangles = 327680; // 20 * 4^7
        constexpr u32 kGridCols = 8;
        constexpr u32 kInstanceCount = 48;
        constexpr u64 kClassicTrianglesPerFrame =
            static_cast<u64>(kInstanceCount) * static_cast<u64>(kSphereTriangles);

        constexpr glm::vec3 kCameraPos{ 0.0f, 0.0f, 8.0f };

        // Below this, the frame is not geometry-bound and NOTHING this benchmark reports about
        // the two geometry paths means anything. See the guard in the tests.
        constexpr u64 kMinGeometryBoundNs = 2'000'000; // 2 ms of GPU time on the classic path

        // Icosphere with shared vertices (closed manifold) — the same generator as
        // VirtualMeshBuilderTest / VirtualGeometryVisualEvidenceTest, kept local per the
        // repo's self-contained-test rule.
        Ref<MeshSource> MakeVgIcosphere(u32 subdivisions)
        {
            const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;
            std::vector<glm::vec3> positions = {
                { -1.0f, t, 0.0f },
                { 1.0f, t, 0.0f },
                { -1.0f, -t, 0.0f },
                { 1.0f, -t, 0.0f },
                { 0.0f, -1.0f, t },
                { 0.0f, 1.0f, t },
                { 0.0f, -1.0f, -t },
                { 0.0f, 1.0f, -t },
                { t, 0.0f, -1.0f },
                { t, 0.0f, 1.0f },
                { -t, 0.0f, -1.0f },
                { -t, 0.0f, 1.0f },
            };
            for (auto& p : positions)
            {
                p = glm::normalize(p);
            }
            std::vector<u32> indices = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11,
                                         4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3,
                                         6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };

            for (u32 s = 0; s < subdivisions; ++s)
            {
                std::map<std::pair<u32, u32>, u32> midpointCache;
                auto midpoint = [&](u32 a, u32 b) -> u32
                {
                    std::pair<u32, u32> const key = std::minmax(a, b);
                    if (auto it = midpointCache.find(key); it != midpointCache.end())
                    {
                        return it->second;
                    }
                    auto index = static_cast<u32>(positions.size());
                    positions.push_back(glm::normalize((positions[a] + positions[b]) * 0.5f));
                    midpointCache.emplace(key, index);
                    return index;
                };
                std::vector<u32> next;
                next.reserve(indices.size() * 4);
                for (sizet i = 0; i + 2 < indices.size(); i += 3)
                {
                    u32 const a = indices[i];
                    u32 const b = indices[i + 1];
                    u32 const c = indices[i + 2];
                    u32 const ab = midpoint(a, b);
                    u32 const bc = midpoint(b, c);
                    u32 const ca = midpoint(c, a);
                    for (u32 idx : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
                    {
                        next.push_back(idx);
                    }
                }
                indices = std::move(next);
            }

            TArray<Vertex> vertices;
            vertices.Reserve(static_cast<i32>(positions.size()));
            constexpr f32 kPi = 3.14159265358979323846f;
            for (const glm::vec3& p : positions)
            {
                glm::vec2 const uv{ std::atan2(p.z, p.x) / (2.0f * kPi) + 0.5f,
                                    std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f };
                vertices.Add(Vertex(p, p, uv));
            }
            TArray<u32> meshIndices;
            meshIndices.Reserve(static_cast<i32>(indices.size()));
            for (u32 const index : indices)
            {
                meshIndices.Add(index);
            }

            const u32 vertexCount = static_cast<u32>(vertices.Num());
            const u32 indexCount = static_cast<u32>(meshIndices.Num());
            auto meshSource = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));

            // A SUBMESH AND A Build() — without these the mesh renders on the VIRTUAL path and
            // draws NOTHING on the classic one, which would silently gut the A/B.
            //
            // The MeshSource(vertices, indices) constructor creates neither. The virtual path does
            // not care: VirtualMeshRegistry builds its clusters straight from the raw vertex/index
            // arrays. The classic path walks GetSubmeshes(), finds it empty, and draws nothing at
            // all — so the "classic" half of this benchmark rendered 364 triangles out of
            // 15,728,640 and reported a confident (and completely meaningless) ratio. Every
            // MeshPrimitives factory does exactly this; the correctness tests never noticed
            // because they only ever drove the virtual path with a procedural mesh.
            Submesh submesh;
            submesh.m_BaseVertex = 0;
            submesh.m_BaseIndex = 0;
            submesh.m_IndexCount = indexCount;
            submesh.m_VertexCount = vertexCount;
            submesh.m_MaterialIndex = 0;
            submesh.m_IsRigged = false;
            submesh.m_NodeName = "Icosphere";
            meshSource->AddSubmesh(submesh);

            meshSource->Build(); // GPU vertex array — the classic path draws through this
            return meshSource;
        }
    } // namespace

    class VirtualGeometryPerf : public RendererAttachedTest
    {
      public:
        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset needs an active project + asset manager.
            if (!Project::GetActive() || !Project::GetAssetManager())
            {
                std::error_code ec;
                fs::path const projectDir = fs::temp_directory_path() / "OloEngineVirtualGeometryPerf";
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Perf.oloproj");
                    proj << "Project:\n"
                            "  Name: VirtualGeometryPerf\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Perf.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kVgWidth, kVgHeight);
            Scene& scene = GetScene();

            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.2f, -0.95f, 0.25f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.0f;
            }

            // THE SCENE MUST BE GEOMETRY-BOUND OR THE BENCHMARK IS WORTHLESS.
            //
            // A geometry renderer benchmarked on a frame whose cost is lighting and post-process
            // measures lighting and post-process. The first version of this fixture used ONE
            // 20k-triangle sphere, and the result was exactly that fraud: 16x-ing the triangle
            // count moved the frame by 0.13 ms, because the geometry was a rounding error next
            // to the fixed per-pixel cost. It "passed" while answering a question nobody asked.
            //
            // So: kInstanceCount dense spheres, each kSphereTriangles triangles, tiled across
            // the view. The classic path must actually push all of them through the vertex
            // stage and the rasterizer; the virtual path gets to replace them with a
            // screen-space-error-bounded DAG cut. THAT difference is the entire thesis of
            // virtualized geometry, and this is the scene where it either shows up or doesn't.
            //
            // The spheres are placed at increasing distance, so the far ones project to a few
            // pixels — which is precisely the regime (sub-pixel triangles) that the hardware
            // rasterizer handles worst and the compute rasterizer exists for. A grid all at the
            // same depth would test only one point on the curve, and the least interesting one.
            {
                auto meshSource = MakeVgIcosphere(kSphereSubdivisions);
                AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(meshSource);

                for (u32 i = 0; i < kInstanceCount; ++i)
                {
                    const u32 col = i % kGridCols;
                    const u32 row = i / kGridCols;

                    Entity sphere = scene.CreateEntity("VirtualSphere");
                    auto& tc = sphere.GetComponent<TransformComponent>();

                    // A wall of spheres filling the view, NOT a corridor receding into the
                    // distance. The receding layout looked more Nanite-ish and was wrong: the far
                    // instances fell outside the frustum, got culled, and the classic path ended
                    // up "drawing" 15.7M triangles in 0.577 ms — i.e. drawing almost none of them.
                    // Every instance must be ON SCREEN or the comparison is against phantom work.
                    // (The triangle-count guard below is what caught this, and stays to catch the
                    // next person who retunes this layout.)
                    //
                    // Scaled down and packed tight so all kInstanceCount fit the frustum at
                    // kCameraPos, while still covering enough pixels to keep the rasterizer busy.
                    tc.Translation = { (static_cast<f32>(col) - 0.5f * static_cast<f32>(kGridCols - 1)) * 1.5f,
                                       (static_cast<f32>(row) - 0.5f * static_cast<f32>(kInstanceCount / kGridCols - 1)) * 1.5f,
                                       -1.0f - static_cast<f32>(row) * 0.35f };
                    tc.Scale = glm::vec3(0.62f);

                    auto& vm = sphere.AddComponent<VirtualMeshComponent>();
                    vm.m_MeshSource = handle;
                    vm.m_ErrorThresholdPixels = 1.0f;
                    auto& mat = sphere.AddComponent<MaterialComponent>();
                    mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                    mat.m_Material.SetRoughnessFactor(0.6f);
                }
            }
        }

      protected:
        // GPU time for one frame, via a PAIR OF GL_TIMESTAMP QUERIES.
        //
        // NOT wall-clock, and NOT GL_TIME_ELAPSED. Both alternatives are wrong here, in ways
        // that are easy to get away with:
        //
        //   * Wall clock (glFinish + steady_clock) measures CPU + GPU. This test binary is a
        //     DEBUG, unoptimized build, where the CPU-side render graph costs milliseconds —
        //     so a wall-clock frame time is dominated by unoptimized C++ and barely moves when
        //     the GPU's workload changes. The first version of this benchmark did exactly that
        //     and produced a ~3.1 ms "floor" that swamped a 16x increase in triangle count.
        //     Every other benchmark in this file measures GPU time for precisely this reason.
        //
        //   * GL_TIME_ELAPSED (what the rest of this file uses) cannot NEST, and the engine's
        //     own GPUPassTimerPool holds elapsed-queries open across the frame we are timing.
        //     Wrapping the frame in one is a GL error, not a measurement.
        //
        // Timestamp queries have neither problem: they are latched in the GPU command stream
        // rather than bracketing a region, so they nest freely with the profiler's queries and
        // report GPU-side nanoseconds. Delta of the two = GPU time for the frame's commands.
        //
        // 5 warmup frames, then 20 measured — MINIMUM wins. Jitter (scheduling, clock ramp,
        // thermal) only ever LENGTHENS a sample, so the fastest is the cleanest signal. Same
        // aggregation policy as every other benchmark in this file.
        u64 MeasureFrameGpuNs()
        {
            EditorCamera camera(45.0f, static_cast<f32>(kVgWidth) / static_cast<f32>(kVgHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kVgWidth), static_cast<f32>(kVgHeight));
            camera.SetPose(kCameraPos, 0.0f, 0.0f);

            constexpr u32 kWarmup = 5;
            constexpr u32 kMeasure = 20;

            RunEditorFrames(camera, kWarmup);

            u32 queries[2] = { 0, 0 };
            ::glGenQueries(2, queries);

            u64 best = std::numeric_limits<u64>::max();
            for (u32 i = 0; i < kMeasure; ++i)
            {
                ::glQueryCounter(queries[0], GL_TIMESTAMP);
                RunEditorFrames(camera, 1);
                ::glQueryCounter(queries[1], GL_TIMESTAMP);

                GLint available = 0;
                while (!available)
                    ::glGetQueryObjectiv(queries[1], GL_QUERY_RESULT_AVAILABLE, &available);

                GLuint64 startNs = 0;
                GLuint64 endNs = 0;
                ::glGetQueryObjectui64v(queries[0], GL_QUERY_RESULT, &startNs);
                ::glGetQueryObjectui64v(queries[1], GL_QUERY_RESULT, &endNs);

                if (endNs > startNs)
                    best = std::min(best, static_cast<u64>(endNs - startNs));
            }

            ::glDeleteQueries(2, queries);
            return best;
        }
    };

    // The headline pair: ~15.7M triangles of classic geometry vs the same scene through the
    // cluster LOD DAG.
    TEST_F(VirtualGeometryPerf, VirtualVsClassicFrameBudget)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // --- virtual path ---
        settings.VirtualGeometryEnabled = true;
        Renderer3D::ApplyRendererSettings();
        const u64 virtualNs = MeasureFrameGpuNs();

        // Structural invariant, independent of timing: this run must ACTUALLY have gone
        // through the cluster pipeline. A benchmark that silently measures the wrong path
        // looks perfectly healthy — it just answers a different question than the one asked.
        const VirtualCullStats virtualStats = VirtualMeshRegistry::Get().ReadFrameCullStats();
        EXPECT_GT(virtualStats.DrawnClusters(), 0u)
            << "the 'virtual' run drew no clusters — it was not on the virtual path at all, so this "
               "benchmark is comparing the classic path against itself";

        // --- classic path, same scene, same meshes ---
        settings.VirtualGeometryEnabled = false;
        Renderer3D::ApplyRendererSettings();
        const u64 classicNs = MeasureFrameGpuNs();

        const VirtualCullStats classicStats = VirtualMeshRegistry::Get().ReadFrameCullStats();
        EXPECT_EQ(classicStats.DrawnClusters(), 0u)
            << "the 'classic' run still drew clusters — the VirtualGeometryEnabled master switch did "
               "not actually fall the mesh back to the classic path, so the A/B is measuring nothing";

        // Restore. Renderer settings are process-global; leaking a disabled virtual path into
        // whatever test runs next would silently change what IT renders.
        settings.VirtualGeometryEnabled = true;
        Renderer3D::ApplyRendererSettings();

        // THE GUARD THAT MAKES THIS BENCHMARK MEAN ANYTHING.
        //
        // Assert on the triangles the renderer REPORTS DRAWING, not on the triangles the scene
        // nominally contains. Those are different numbers, and the gap between them is where a
        // benchmark goes to die: frustum culling, a fallback that silently submits nothing, an
        // LOD swap — any of them leave the scene "containing" 15.7M triangles while the GPU draws
        // a small fraction, and the benchmark then reports a confident ratio about work that
        // never happened.
        //
        // This is not hypothetical. The first version of this guard trusted the arithmetic and
        // reported that the classic path drew 15,728,640 triangles in 0.577 ms — twenty-seven
        // BILLION triangles per second, which no GPU can do. They were being culled. Without this
        // check the headline would have been "virtualized geometry is 6.4x slower", a number
        // derived entirely from comparing against work that was never submitted.
        //
        // So: the classic path must actually have RENDERED most of what the scene contains, AND
        // that must have cost real GPU time. Either condition alone can be satisfied by a lie.
        const u32 classicTrianglesDrawn = RendererProfiler::GetInstance().GetCurrentFrameData().m_TrianglesRendered;
        ::testing::Test::RecordProperty("classic_triangles_drawn", std::to_string(classicTrianglesDrawn));

        EXPECT_GT(static_cast<u64>(classicTrianglesDrawn), kClassicTrianglesPerFrame / 2)
            << "the classic path reported drawing only " << classicTrianglesDrawn << " of the "
            << kClassicTrianglesPerFrame
            << " triangles this scene contains. The geometry is being culled, or the fallback is "
               "submitting nothing — either way the 'classic' side of this A/B is not doing the work "
               "it claims to represent, and the ratio is meaningless. Fix the SCENE (are the instances "
               "actually inside the frustum?) before trusting any number here.";

        EXPECT_GT(classicNs, kMinGeometryBoundNs)
            << "the classic path drew " << classicTrianglesDrawn << " triangles in "
            << (static_cast<f64>(classicNs) / 1e6)
            << " ms of GPU time — this frame is NOT geometry-bound, so comparing the two geometry "
               "paths on it measures lighting and post-process wearing a geometry costume. Raise "
               "kInstanceCount / kSphereSubdivisions until geometry actually owns the frame.";

        const f64 ratio = static_cast<f64>(virtualNs) / static_cast<f64>(classicNs);
        ::testing::Test::RecordProperty("virtual_vs_classic_ratio", std::to_string(ratio));
        ::testing::Test::RecordProperty("classic_triangles_per_frame", std::to_string(kClassicTrianglesPerFrame));
        GTEST_LOG_(INFO) << "[nanite] " << kClassicTrianglesPerFrame << " tris: classic "
                         << (static_cast<f64>(classicNs) / 1e6) << " ms GPU, virtual "
                         << (static_cast<f64>(virtualNs) / 1e6) << " ms GPU, ratio " << ratio;

        CheckPerfRegression("virtual_geometry_frame_1280x720", virtualNs);
        CheckPerfRegression("classic_geometry_frame_1280x720", classicNs);
    }

    // Nanite's core claim — sub-pixel triangles rasterize faster in compute than through the
    // hardware rasterizer — made falsifiable.
    TEST_F(VirtualGeometryPerf, SoftwareVsHardwareRasterBudget)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        settings.VirtualGeometryEnabled = true;
        Renderer3D::ApplyRendererSettings();

        auto& registry = VirtualMeshRegistry::Get();

        registry.SetSwRasterMode(VirtualSwRasterMode::ForceSoftware);
        const u64 swNs = MeasureFrameGpuNs();
        const VirtualCullStats swStats = registry.ReadFrameCullStats();

        registry.SetSwRasterMode(VirtualSwRasterMode::Disabled); // hardware MDI only
        const u64 hwNs = MeasureFrameGpuNs();
        const VirtualCullStats hwStats = registry.ReadFrameCullStats();

        registry.SetSwRasterMode(VirtualSwRasterMode::Auto); // don't leak the mode

        // The invariant that keeps this pair honest. Without it, both halves could quietly be
        // rasterizing the same way and the "comparison" would be two samples of one path.
        EXPECT_GT(swStats.SoftwareRasterized, 0u)
            << "ForceSoftware rasterized nothing in software — the SW half of this A/B is measuring "
               "the hardware path";
        EXPECT_EQ(hwStats.SoftwareRasterized, 0u)
            << "swRasterMode=Disabled still software-rasterized clusters — the HW half of this A/B is "
               "not actually hardware-only";
        EXPECT_GT(hwStats.HardwareDraws, 0u) << "the hardware-only run issued no hardware draws";

        const f64 ratio = static_cast<f64>(swNs) / static_cast<f64>(hwNs);
        ::testing::Test::RecordProperty("swraster_vs_hwraster_ratio", std::to_string(ratio));
        GTEST_LOG_(INFO) << "[nanite] software raster " << (static_cast<f64>(swNs) / 1e6)
                         << " ms GPU, hardware raster " << (static_cast<f64>(hwNs) / 1e6)
                         << " ms GPU, ratio " << ratio;

        CheckPerfRegression("virtual_swraster_frame_1280x720", swNs);
        CheckPerfRegression("virtual_hwraster_frame_1280x720", hwNs);
    }
} // namespace OloEngine::Tests
