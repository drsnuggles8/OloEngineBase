// OLO_TEST_LAYER: plumbing
//
// Renderer3D::Shutdown() must release EVERY GPU-resource-holding member of its static
// s_Data — not most of them.
//
// A Ref<> that Shutdown() forgets does not leak quietly. It survives into STATIC
// destruction at process exit, where its destructor frees GPU buffers through
// FrameResourceManager / RendererMemoryTracker / GPUResourceInspector — Meyer's
// singletons that are already gone by then. The process segfaults on the way out AFTER
// GoogleTest has printed "[  PASSED  ]" and its summary, so the run reads as a clean pass
// and only the exit code (139) disagrees. OloEngineTest.cpp calls Renderer::Shutdown()
// explicitly for exactly this reason.
//
// That is how Ref<GPUFrustumCuller> hid: Init() created it, Shutdown() reset the other
// fifteen members but not that one, and only the occlusion tests ever populate its buffer
// pool — so the crash needed a suite-wide bisect to locate, and never fired in CI at all
// (no GL context there means those tests SKIP, the pool stays empty, nothing dangles).
//
// WHY THIS IS A SOURCE-TEXT TEST AND NOT A RUNTIME ONE.
// The obvious test — bring the renderer up, draw, call Shutdown(), assert nothing is
// left — cannot be written in-process: Renderer3D does not support Init-after-Shutdown
// (the shader library is not cleared, so the next test that re-inits aborts with
// "Shader 'Renderer2D_Quad' already exists"). Shutting the renderer down mid-suite
// destroys the suite. So instead this checks the invariant where it actually lives: the
// SOURCE. Every member Renderer3D::DebugLiveGpuOwningStatics() reports on must be
// released inside Renderer3D::Shutdown(). Add a GPU-owning member to one and forget the
// other, and this fails — headless, on every PR, with a message naming the member.
//
// The repo already uses this idiom for the same reason (ComponentSerializerCoverageTest,
// ComponentTupleCoverageTest): the touch-points are hand-maintained, so a text scan is the
// only guard that cannot itself drift.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path);
            EXPECT_TRUE(in.is_open()) << "cannot open " << path.string();
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // The body of a function, from its signature to the matching closing brace at
        // brace-depth 0. Good enough for two well-formed functions in one file.
        [[nodiscard]] std::string ExtractFunctionBody(const std::string& source, const std::string& signature)
        {
            const sizet start = source.find(signature);
            if (start == std::string::npos)
            {
                return {};
            }
            const sizet open = source.find('{', start);
            if (open == std::string::npos)
            {
                return {};
            }

            i32 depth = 0;
            for (sizet i = open; i < source.size(); ++i)
            {
                if (source[i] == '{')
                {
                    ++depth;
                }
                else if (source[i] == '}')
                {
                    if (--depth == 0)
                    {
                        return source.substr(open, i - open + 1);
                    }
                }
            }
            return {};
        }

        // Drop // line comments. Without this, a call that has merely been COMMENTED OUT
        // still matches the "is it released?" search below — which would make this test
        // pass against the very bug it exists to catch. (It did, first try.)
        [[nodiscard]] std::string StripLineComments(const std::string& source)
        {
            std::string out;
            out.reserve(source.size());
            std::istringstream in(source);
            std::string line;
            while (std::getline(in, line))
            {
                if (const sizet slashes = line.find("//"); slashes != std::string::npos)
                {
                    line.erase(slashes);
                }
                out += line;
                out += '\n';
            }
            return out;
        }

        // The working directory at PROCESS START. Captured during static init, before any
        // test body can run — because some other test in this binary chdir()s, and a plain
        // relative path resolved inside the test body then misses the file. (It did: this
        // test passed under --gtest_filter and failed in the full run, purely on cwd.)
        const std::filesystem::path s_StartCwd = std::filesystem::current_path();

        constexpr const char* kRelativeSource = "OloEngine/src/OloEngine/Renderer/Renderer3DLifecycle.cpp";

        [[nodiscard]] std::filesystem::path LifecycleSource()
        {
            // Walk up from the start cwd until the file appears, so the test does not care
            // whether it is run from the repo root or a build directory.
            std::error_code ec;
            for (std::filesystem::path dir = s_StartCwd; !dir.empty(); dir = dir.parent_path())
            {
                if (std::filesystem::path candidate = dir / kRelativeSource;
                    std::filesystem::exists(candidate, ec))
                {
                    return candidate;
                }
                if (!dir.has_relative_path())
                {
                    break; // reached the root
                }
            }
            return s_StartCwd / kRelativeSource; // report the path we looked for
        }
    } // namespace

    // Every GPU-owning static that DebugLiveGpuOwningStatics() knows about must be released
    // by Shutdown(). This is the invariant whose violation segfaulted the test binary at exit.
    TEST(RendererShutdown, ShutdownReleasesEveryGpuOwningStaticItKnowsAbout)
    {
        const std::string source = ReadFile(LifecycleSource());
        ASSERT_FALSE(source.empty()) << "could not read Renderer3DLifecycle.cpp — is the cwd the repo root?";

        const std::string diagnosticsBody =
            ExtractFunctionBody(source, "std::vector<std::string> Renderer3D::DebugLiveGpuOwningStatics()");
        ASSERT_FALSE(diagnosticsBody.empty())
            << "Renderer3D::DebugLiveGpuOwningStatics() not found — did it move? This test must be repointed.";

        const std::string shutdownBody =
            StripLineComments(ExtractFunctionBody(source, "void Renderer3D::Shutdown()"));
        ASSERT_FALSE(shutdownBody.empty())
            << "Renderer3D::Shutdown() not found — did it move? This test must be repointed.";

        // Every member named in a note("Name", s_Data.Member != nullptr) line.
        const std::regex noteRe(R"RX(note\(\s*"([A-Za-z0-9_]+)"\s*,\s*s_Data\.([A-Za-z0-9_]+))RX");
        std::vector<std::string> members;
        for (auto it = std::sregex_iterator(diagnosticsBody.begin(), diagnosticsBody.end(), noteRe);
             it != std::sregex_iterator(); ++it)
        {
            members.push_back((*it)[2].str());
        }

        ASSERT_FALSE(members.empty())
            << "parsed no members out of DebugLiveGpuOwningStatics() — the regex has drifted from the code, "
               "which would make this test silently vacuous";

        std::vector<std::string> notReleased;
        for (const std::string& member : members)
        {
            // Accept either `s_Data.Member.Reset()` or `s_Data.Member->Shutdown()`/`.Shutdown()`.
            const std::string pattern = "s_Data\\." + member + "(\\.Reset\\(\\)|->Shutdown\\(\\)|\\.Shutdown\\(\\))";
            const std::regex releaseRe(pattern);
            if (!std::regex_search(shutdownBody, releaseRe))
            {
                notReleased.push_back(member);
            }
        }

        std::string names;
        for (const std::string& n : notReleased)
        {
            names += (names.empty() ? "" : ", ") + n;
        }

        EXPECT_TRUE(notReleased.empty())
            << "Renderer3D::Shutdown() does not release these GPU-owning statics: " << names
            << "\n\nA Ref<> that Shutdown() forgets is destroyed during STATIC destruction at process exit,\n"
               "after FrameResourceManager / RendererMemoryTracker / GPUResourceInspector are gone — which\n"
               "segfaults the binary on the way out, AFTER every test result has printed, so the run still\n"
               "reads as a clean pass and only the exit code (139) disagrees. That is exactly how\n"
               "Ref<GPUFrustumCuller> hid.\n\n"
               "Add `s_Data.<Member>.Reset();` to Renderer3D::Shutdown() in Renderer3DLifecycle.cpp.";
    }
} // namespace OloEngine::Tests
