#pragma once

// =============================================================================
// PerfBaselineGate — the Layer-6 baseline gate, shared (issue #1526).
//
// perf_baselines.txt, the per-machine perf_history TSV and the WARN / FAIL
// policy used to live file-local in PerfRegressionTests.cpp. The Vulkan
// staging baselines (VulkanPassSuiteTest.cpp) need the same gate against the
// same file, so the helpers live here as inline functions: one baseline cache
// per process, which matters because a --olo-perf-rebase run rewrites the
// WHOLE file from that cache and two copies would each drop the other's keys.
//
// Policy and file format are unchanged; see CheckPerfRegression.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Environment.h"
#include "../../TestOptions.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::Tests::PerfBaseline
{

    // Regression thresholds relative to baseline. Improvements never fail.
    // Thresholds are loose because these microbenchmarks run in the low
    // microsecond range where driver scheduling + GPU idle-ramp add
    // nontrivial variance even after 20 samples-min aggregation.
    inline constexpr f32 kPerfWarnRatio = 1.50f;
    inline constexpr f32 kPerfFailRatio = 2.50f;

    // Absolute ceiling to catch pathological regressions even when no
    // baseline is present (still enforced alongside the ratio check).
    inline constexpr u64 kSanityCeilingNs = 100 * 1000 * 1000ull; // 100 ms

    inline bool PerfShouldRebase()
    {
        return OloEngine::Tests::Options().PerfRebase;
    }

    // Whether a regression should FAIL the test (strict mode) or merely be
    // recorded + logged (the default). These are low-µs GPU microbenchmarks
    // pinned to one dev workstation's baselines; under machine/GPU
    // contention a full-suite run can inflate every sample 1.5–3.4× and trip
    // the thresholds, even though the same test passes cleanly in isolation
    // (issue #324). Defaulting to non-fatal stops that flakiness from eroding
    // trust in the suite while still capturing every measurement (gtest
    // properties + perf_history TSV) so regressions stay observable via the
    // trend tool. Pass --olo-perf-strict for a deliberate regression
    // gate on a quiet machine.
    inline bool PerfShouldEnforce()
    {
        return OloEngine::Tests::Options().PerfStrict;
    }

    // Locate the perf_baselines.txt file. We try a small list of
    // candidate paths to be robust against varying working directories
    // (tests are typically run from the repo root via the VS Code task,
    // but IDE test runners often run from the build output dir).
    inline std::filesystem::path PerfBaselinePath()
    {
        const std::filesystem::path relFromRoot = std::filesystem::path("OloEngine") / "tests" / "Rendering" / "PropertyTests" / "perf_baselines.txt";

        // Walk up from CWD looking for a match. Covers:
        //   - run from repo root (cwd/OloEngine/tests/...)
        //   - run from build/OloEngine/tests/Debug (walk up 4 levels)
        std::filesystem::path cwd = std::filesystem::current_path();
        for (int i = 0; i < 8; ++i)
        {
            if (std::filesystem::path candidate = cwd / relFromRoot; std::filesystem::exists(candidate))
                return candidate;
            if (!cwd.has_parent_path() || cwd == cwd.parent_path())
                break;
            cwd = cwd.parent_path();
        }
        // Fallback: next to this translation unit at build time.
        return std::filesystem::path(__FILE__).parent_path() / "perf_baselines.txt";
    }

    inline std::unordered_map<std::string, u64>& PerfBaselineCache()
    {
        static std::unordered_map<std::string, u64> cache;
        if (static bool loaded = false; !loaded)
        {
            loaded = true;
            std::filesystem::path path = PerfBaselinePath();
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
    inline void WritePerfBaselines(const std::unordered_map<std::string, u64>& baselines)
    {
        std::filesystem::path path = PerfBaselinePath();
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
            << "# To rebase (after confirming the change is expected), run the test\n"
            << "# binary with --olo-perf-rebase. The harness will rewrite this file with\n"
            << "# fresh numbers from that run and pass unconditionally.\n"
            << "#\n"
            << "# Regression policy (see PerfRegressionTests.cpp). Enforcement is\n"
            << "# OPT-IN: by default both WARN- and FAIL-level regressions are RECORDED\n"
            << "# (gtest property + perf_history TSV) and logged via GTEST_LOG_(WARNING),\n"
            << "# but do NOT fail the test \xe2\x80\x94 these low-microsecond GPU microbenchmarks\n"
            << "# flake under machine/GPU contention (issue #324). Pass\n"
            << "# --olo-perf-strict to turn a regression into an ADD_FAILURE on a\n"
            << "# quiet machine.\n"
            << "#   - Measured >= 2.5x baseline  -> FAIL-level (fails only when STRICT)\n"
            << "#   - Measured >= 1.5x baseline  -> WARN-level (fails only when STRICT)\n"
            << "#   - Faster than baseline       -> PASS, never fail on improvements\n"
            << "#\n"
            << "# Metric: minimum of 20 samples after 5 warmups. Fullscreen passes use\n"
            << "# GL_TIME_ELAPSED; CPU/submission benches use steady_clock and state\n"
            << "# their unit in the test log. The fastest run is the cleanest signal.\n\n";
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
    //   1. --olo-perf-machine=<name> (canonical, passed in CI).
    //   2. COMPUTERNAME / HOSTNAME env var.
    //   3. "unknown"
    //
    // History rows are *informational* — they never fail a test. They
    // let us spot drifting baselines (gradual 5 % / week creep that
    // wouldn't trip the 1.5× warn threshold) and compare machines
    // side-by-side without carrying multiple baseline files.
    // -------------------------------------------------------------------
    inline std::string GetPerfMachineTag()
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
        if (const std::string& tag = OloEngine::Tests::Options().PerfMachine; !tag.empty())
            return sanitize(tag);
        // Not ours to own: the hostname is the OS's, and only it can answer.
        if (const auto name = Env::Get("COMPUTERNAME"))
            return sanitize(*name);
        if (const auto name = Env::Get("HOSTNAME"))
            return sanitize(*name);
        return "unknown";
    }

    inline std::filesystem::path PerfHistoryPath()
    {
        std::filesystem::path baseDir = PerfBaselinePath().parent_path() / "perf_history";
        std::error_code ec;
        std::filesystem::create_directories(baseDir, ec);
        return baseDir / (GetPerfMachineTag() + ".tsv");
    }

    inline void AppendPerfHistory(const std::string& name, u64 measuredNs,
                                  u64 baselineNs, f32 ratio)
    {
        const std::filesystem::path path = PerfHistoryPath();
        const bool preexisting = std::filesystem::exists(path);

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
    inline void CheckPerfRegression(const std::string& name, u64 measuredNs)
    {
        ::testing::Test::RecordProperty("regression_gate", "workstation lower-bound baseline; not a frame-tail gate");
        auto& cache = PerfBaselineCache();

        if (PerfShouldRebase())
        {
            cache[name] = measuredNs;
            WritePerfBaselines(cache);
            ::testing::Test::RecordProperty(name + "_ns_lower_bound", std::to_string(measuredNs));
            ::testing::Test::RecordProperty(name + "_rebased", "1");
            AppendPerfHistory(name, measuredNs, measuredNs, 1.0f);
            return;
        }

        ::testing::Test::RecordProperty(name + "_ns_lower_bound", std::to_string(measuredNs));

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
                                    << " — pass --olo-perf-strict to fail on this.";
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
            // Missing baseline = gentle reminder; rebase via --olo-perf-rebase.
            AppendPerfHistory(name, measuredNs, 0, 0.0f);
            note("No baseline for '" + name + "'. Measured " + std::to_string(measuredNs) + " ns. Rebase via --olo-perf-rebase.");
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

    // General counterpart to MeasureFullscreenPassStableNs. A single
    // attempt takes the minimum of 20 batched samples; when that still
    // crosses the warning ratio, retry once and keep the faster attempt.
    // This is the same anti-flake policy as the GPU microbenchmarks, applied
    // to the render-graph executor's allocation/callback-heavy hot path.
    template<typename MeasureFn>
    inline u64 MeasureBenchmarkStableNs(const std::string& baselineKey, MeasureFn&& measure)
    {
        const u64 first = measure();
        const auto it = PerfBaselineCache().find(baselineKey);
        if (it == PerfBaselineCache().end() || it->second == 0u || PerfShouldRebase())
            return first;

        if (const f32 ratio = static_cast<f32>(first) / static_cast<f32>(it->second);
            ratio < kPerfWarnRatio)
        {
            return first;
        }

        return std::min(first, measure());
    }
} // namespace OloEngine::Tests::PerfBaseline
