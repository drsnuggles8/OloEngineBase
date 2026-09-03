#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine::Tests
{
    // @brief The test binary's own knobs, as command-line flags.
    //
    // These were twelve environment variables read from twenty-six scattered
    // `std::getenv` sites, each with its own parse. That was the wrong
    // mechanism twice over:
    //
    //   * **They were invisible.** A flag shows up in the command line CI
    //     logs print; an environment variable set three YAML levels up does
    //     not, so "why did this run rebase the goldens?" was archaeology.
    //   * **They disagreed about their own values.** The AMD conformance
    //     workflow carried a comment explaining that it must pass `'0'` rather
    //     than `'false'`, because of the three readers of
    //     OLOENGINE_GOLDEN_REBASE one tested only the first character — so
    //     `false` *enabled* a rebase there. A flag is present or absent; that
    //     entire class of question stops existing.
    //
    // Environment variables also leak into child processes. A test that spawns
    // the editor (McpHeadlessAttachTest) handed it the whole set by accident.
    //
    // Parsed once in `main` BEFORE `InitGoogleTest`, and read through
    // `Options()` thereafter. Unknown `--olo-*` flags are a hard error rather
    // than a shrug: a typo'd `--olo-golden-rebse` that silently did nothing
    // would reproduce the invisibility this replaces.
    // Which windowing/context path RenderPropertyFixture takes for the shared
    // GL context. Parsed once from --olo-gl-backend, so the accepted spellings
    // live in exactly one place (TestOptions.cpp) and the fixture switches on
    // an enum with no fallback branch to drift into.
    //
    //   Auto        prefer GLFW (what a developer gets locally); fall back to
    //               EGL when GLFW cannot reach a display server.
    //   Glfw / Egl  pin one path. Headless CI pins Egl so a missing display
    //               server is a deterministic choice, not a silent switch: two
    //               backends can produce subtly different pixels, and a golden
    //               baseline must know which one produced it.
    //   None        attempt no context. Every GPU-gated test then skips exactly
    //               as on a GitHub-hosted runner (#1015 stage B).
    enum class GlBackend
    {
        Auto,
        Glfw,
        Egl,
        None
    };

    struct TestOptions
    {
        // --olo-golden-rebase : overwrite golden images with this run's output.
        bool GoldenRebase = false;
        // --olo-golden-vendor=<name> : GPU vendor whose golden set to compare
        // against, when a machine's own vendor should not be inferred.
        std::string GoldenVendor;
        // --olo-perf-rebase : overwrite the perf baseline for this machine.
        bool PerfRebase = false;
        // --olo-perf-strict : fail on a perf regression instead of warning.
        bool PerfStrict = false;
        // --olo-perf-machine=<name> : baseline key, overriding the hostname.
        std::string PerfMachine;
        // --olo-bench-assert : make the micro-benchmarks assert their budgets
        // rather than only reporting.
        bool BenchAssert = false;
        // --olo-soundgraph-perf : run the SoundGraph throughput measurement.
        bool SoundGraphPerf = false;
        // --olo-gl-backend=<egl|glfw|none|auto> : force the context-creation
        // path. `auto` is the default and is also accepted explicitly.
        // `egl` is the headless surfaceless route the GPU runners need. `none`
        // creates no context at all, so every GPU-gated test skips exactly as
        // it does on a GitHub-hosted runner -- the switch that lets a run on
        // the self-hosted GPU box mean the same thing as the hosted run of the
        // same commit (issue #1015, stage B). Validated at parse time: a typo
        // here used to fall back to auto-detection silently, and on a box with
        // a GPU that is the difference between "tested nothing" and "tested
        // 200 GL tests under a sanitizer".
        GlBackend GlBackend = GlBackend::Auto;
        // --olo-require-gpu : turn the GPU gate's skip into a FAILURE. For a
        // job whose whole purpose is the GPU-gated tests (the nightly
        // GPU-under-sanitizer baseline, #1015 stage C): without it, a broken
        // EGL path skips every such test and the run goes green having
        // verified nothing. Rejected together with `--olo-gl-backend=none`.
        bool RequireGpu = false;
        // --olo-keep-temp : leave per-test temp directories on disk.
        bool KeepTemp = false;
        // --olo-video=<path> : an FFmpeg-decodable file for the real-decode
        // tests. Empty falls back to the probed default paths, and those tests
        // GTEST_SKIP when nothing is found.
        std::string VideoPath;
        // --olo-pathtracer-evidence : write the path-tracer reference images.
        bool PathTracerEvidence = false;
        // --olo-mcp-attach-seconds=<n> : how long the headless attach test
        // waits for the editor's MCP discovery file. 0 keeps the test default.
        i32 McpAttachSeconds = 0;
        // --olo-bake-shader-pack=<path> : bake a ShaderPack.osp to this path
        // instead of running the test suite normally — the headless CI
        // producer for issue #908 (no GL context; see ShaderPackBakeTest).
        // Empty means "not baking".
        std::string BakeShaderPackPath;
        // --olo-capture-manifest=<path> : run the benchmark capture described
        // by this manifest instead of the normal suite — the deterministic
        // scene/AOV capture entry point for issue #974 (needs a GL 4.6
        // context; see BenchmarkCaptureTest). Empty means "not capturing".
        std::string CaptureManifestPath;
        // --olo-capture-out=<dir> : override the manifest capture's result
        // directory (default: assets/benchmark/captures/<manifest Id>/).
        std::string CaptureOutDir;
    };

    // Parse and consume the `--olo-*` flags. Call before InitGoogleTest so the
    // gtest parser never sees them. Exits with a diagnostic on an unknown
    // `--olo-*` flag or a malformed value.
    void ParseTestOptions(int& argc, char** argv);

    [[nodiscard]] const TestOptions& Options();
} // namespace OloEngine::Tests
