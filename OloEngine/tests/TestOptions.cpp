#include "OloEnginePCH.h"
#include "TestOptions.h"

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
                "  --olo-bench-assert             make micro-benchmarks assert their budgets\n"
                "  --olo-soundgraph-perf          run the SoundGraph throughput measurement\n"
                "  --olo-gl-backend=<egl|glfw>    force the context-creation path\n"
                "  --olo-keep-temp                leave per-test temp directories on disk\n"
                "  --olo-video=<path>             an FFmpeg-decodable file for the decode tests\n"
                "  --olo-pathtracer-evidence      write the path-tracer reference images\n"
                "  --olo-mcp-attach-seconds=<n>   MCP discovery-file wait for the attach test\n");
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
                s_Options.GoldenVendor = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-perf-machine"))
            {
                s_Options.PerfMachine = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-gl-backend"))
            {
                s_Options.GlBackend = *v;
            }
            else if (const auto v = ValueOf(arg, "--olo-video"))
            {
                s_Options.VideoPath = *v;
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
                     arg == "--olo-mcp-attach-seconds")
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
