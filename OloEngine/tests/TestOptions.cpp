#include "OloEnginePCH.h"
#include "TestOptions.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        TestOptions s_Options;

        [[noreturn]] void Fail(std::string_view message, std::string_view arg)
        {
            std::fprintf(stderr,
                         "OloEngine-Tests: %.*s: '%.*s'\n"
                         "Run with --olo-help to list the available --olo-* flags.\n",
                         static_cast<int>(message.size()), message.data(),
                         static_cast<int>(arg.size()), arg.data());
            std::exit(2);
        }

        void PrintHelp()
        {
            std::puts(
                "OloEngine test options (in addition to gtest's own --gtest_* flags):\n"
                "  --olo-golden-rebase            overwrite golden images with this run's output\n"
                "  --olo-golden-vendor=<name>     GPU vendor whose golden set to compare against\n"
                "  --olo-perf-rebase              overwrite this machine's perf baseline\n"
                "  --olo-perf-strict              fail on a perf regression instead of warning\n"
                "  --olo-perf-machine=<name>      perf baseline key, overriding the hostname\n"
                "  --olo-strict-renderer-state    fail a test that leaks process-global renderer\n"
                "                                 configuration (it is restored either way)\n"
                "  --olo-bench-assert             make micro-benchmarks assert their budgets\n"
                "  --olo-soundgraph-perf          run the SoundGraph throughput measurement\n"
                "  --olo-gl-backend=<egl|glfw|none|auto> force the context-creation path; `none` creates\n"
                "                                 no context, so every GPU-gated test skips (hosted parity);\n"
                "                                 `auto` is the default and may also be given explicitly\n"
                "  --olo-require-gpu              fail, rather than skip, a GPU-gated test when no GL 4.6\n"
                "                                 context could be created (a GPU job's own guard)\n"
                "  --olo-keep-temp                leave per-test temp directories on disk\n"
                "  --olo-video=<path>             an FFmpeg-decodable file for the decode tests\n"
                "  --olo-pathtracer-evidence      write the path-tracer reference images\n"
                "  --olo-mcp-attach-seconds=<n>   MCP discovery-file wait for the attach test\n"
                "  --olo-bake-shader-pack=<path>  bake a ShaderPack.osp to this path (headless)\n"
                "  --olo-capture-manifest=<path>  run the benchmark capture this manifest describes\n"
                "  --olo-capture-out=<dir>        override the capture's result directory (a relative\n"
                "                                 dir resolves against OloEditor/, the capture cwd)\n");
        }

        // Returns the value of `--name=value`, or nullopt when `arg` is not that flag.
        std::optional<std::string_view> ValueOf(std::string_view arg, std::string_view name)
        {
            if (!arg.starts_with(name))
            {
                return std::nullopt;
            }
            std::string_view rest = arg.substr(name.size());
            if (rest.empty() || rest.front() != '=')
            {
                return std::nullopt;
            }
            return rest.substr(1);
        }

        // `--olo-golden-vendor` and `--olo-perf-machine` both become a PATH
        // COMPONENT at their use sites — `assets/tests/{golden,visual}/<vendor>/`
        // (GoldenImageTests.cpp, AtmosphereVisualEvidenceTest.cpp) and
        // `perf_history/<machine>.tsv` (PerfRegressionTests.cpp) — and two of
        // those three sites concatenate the value with no checking at all. That
        // matters because `--olo-golden-rebase` WRITES to the composed path, so
        // a value containing a separator would create directories and drop PNGs
        // outside the asset tree.
        //
        // Only AtmosphereVisualEvidenceTest validated, and its comment claims to
        // "mirror GoldenImageTests::GoldenBaselineDir" — which is precisely the
        // hardening that never made it back to the mirror it names. Validating
        // the VALUE once, here, is what stops that drift recurring: a third
        // consumer added later is covered without knowing this rule exists.
        //
        // A whitelist rather than a separator blacklist, because the blacklist
        // has to enumerate every way a platform can spell "rooted" — on Windows
        // a drive-relative `C:vendor` contains no separator at all yet
        // fs::path treats it as rooted, so `base /= value` silently discards
        // `base`. Vendor and machine tags are short identifiers (`amd`,
        // `llvmpipe`, `ci-llvmpipe-windows`), so the whitelist costs nothing.
        void RequirePlainPathComponent(std::string_view value, std::string_view arg)
        {
            const bool plain = !value.empty() && value != "." && value != ".." &&
                               std::ranges::all_of(value,
                                                   [](char c)
                                                   {
                                                       return (c >= 'a' && c <= 'z') ||
                                                              (c >= 'A' && c <= 'Z') ||
                                                              (c >= '0' && c <= '9') ||
                                                              c == '-' || c == '_' || c == '.';
                                                   });
            if (!plain)
            {
                Fail("value must be a single plain path component "
                     "(letters, digits, '-', '_', '.'; not '.' or '..')",
                     arg);
            }
        }
    } // namespace

    void ParseTestOptions(int& argc, char** argv)
    {
        std::vector<char*> kept;
        kept.reserve(static_cast<sizet>(argc));

        for (int i = 0; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            // Anything that is not ours passes through untouched — gtest's own
            // flags, the binary name, and any positional argument.
            if (i == 0 || !arg.starts_with("--olo-"))
            {
                kept.push_back(argv[i]);
                continue;
            }

            if (arg == "--olo-help")
            {
                PrintHelp();
                std::exit(0);
            }
            else if (arg == "--olo-golden-rebase")
            {
                s_Options.GoldenRebase = true;
            }
            else if (arg == "--olo-perf-rebase")
            {
                s_Options.PerfRebase = true;
            }
            else if (arg == "--olo-perf-strict")
            {
                s_Options.PerfStrict = true;
            }
            else if (arg == "--olo-strict-renderer-state")
            {
                s_Options.StrictRendererState = true;
            }
            else if (arg == "--olo-bench-assert")
            {
                s_Options.BenchAssert = true;
            }
            else if (arg == "--olo-soundgraph-perf")
            {
                s_Options.SoundGraphPerf = true;
            }
            else if (arg == "--olo-keep-temp")
            {
                s_Options.KeepTemp = true;
            }
            else if (arg == "--olo-pathtracer-evidence")
            {
                s_Options.PathTracerEvidence = true;
            }
            else if (const auto v = ValueOf(arg, "--olo-golden-vendor"))
            {
                RequirePlainPathComponent(*v, arg);
                s_Options.GoldenVendor = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-perf-machine"))
            {
                RequirePlainPathComponent(*v, arg);
                s_Options.PerfMachine = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-gl-backend"))
            {
                // Validated here, not at the use site. RenderPropertyTest.cpp
                // used to read any spelling and fall back to auto-detection,
                // which on a machine WITH a GPU means a typo'd `--olo-gl-backend=nnoe`
                // quietly runs every GL-gated test the caller meant to skip
                // -- the exact silent-switch these flags exist to prevent.
                if (*v == "egl")
                    s_Options.GlBackend = GlBackend::Egl;
                else if (*v == "glfw")
                    s_Options.GlBackend = GlBackend::Glfw;
                else if (*v == "none")
                    s_Options.GlBackend = GlBackend::None;
                else if (*v == "auto")
                    s_Options.GlBackend = GlBackend::Auto;
                else
                    Fail("--olo-gl-backend must be one of egl, glfw, none, auto", arg);
            }
            else if (arg == "--olo-require-gpu")
            {
                s_Options.RequireGpu = true;
            }
            else if (const auto v = ValueOf(arg, "--olo-video"))
            {
                s_Options.VideoPath = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-bake-shader-pack"))
            {
                s_Options.BakeShaderPackPath = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-capture-manifest"))
            {
                s_Options.CaptureManifestPath = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-capture-out"))
            {
                s_Options.CaptureOutDir = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-mcp-attach-seconds"))
            {
                i32 parsed = 0;
                const char* begin = v->data();
                const char* end = begin + v->size();
                if (const auto [ptr, ec] = std::from_chars(begin, end, parsed);
                    ec != std::errc{} || ptr != end || parsed < 0)
                {
                    Fail("--olo-mcp-attach-seconds needs a non-negative integer", arg);
                }
                s_Options.McpAttachSeconds = parsed;
            }
            else if (arg == "--olo-golden-vendor" || arg == "--olo-perf-machine" ||
                     arg == "--olo-gl-backend" || arg == "--olo-video" ||
                     arg == "--olo-mcp-attach-seconds" || arg == "--olo-bake-shader-pack" ||
                     arg == "--olo-capture-manifest" || arg == "--olo-capture-out")
            {
                // The name is right but the `=value` is missing — say that,
                // rather than sending someone hunting for a typo.
                Fail("option needs a =value", arg);
            }
            else
            {
                // Deliberately fatal. A silently-ignored typo would reproduce
                // exactly the "why did nothing happen?" problem these flags
                // replace.
                Fail("unknown option", arg);
            }
        }

        // Both flags together have no sane meaning: one promises no context,
        // the other demands one, and whichever wins silently is wrong.
        if (s_Options.RequireGpu && s_Options.GlBackend == GlBackend::None)
        {
            Fail("--olo-require-gpu contradicts --olo-gl-backend=none", "--olo-require-gpu");
        }

        for (sizet i = 0; i < kept.size(); ++i)
        {
            argv[i] = kept[i];
        }
        argc = static_cast<int>(kept.size());
        argv[argc] = nullptr;
    }

    const TestOptions& Options()
    {
        return s_Options;
    }
} // namespace OloEngine::Tests
