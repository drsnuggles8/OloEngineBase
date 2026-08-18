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

#include <algorithm>
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

        constexpr const char* kLifecycleSource = "OloEngine/src/OloEngine/Renderer/Renderer3DLifecycle.cpp";

        struct TeardownSite
        {
            const char* Source;
            const char* Signature;
        };

        // The teardowns that run for EVERY session, whether or not Renderer3D came up:
        // Renderer::Shutdown() plus the two it calls UNCONDITIONALLY. Releasing a
        // warmup-created static where it is created — in Renderer2D's or ShaderWarmup's own
        // teardown — is equally correct (it was fix option (a) on #814), so the test must
        // accept it rather than push people towards one specific function.
        //
        // Renderer3D::Shutdown() is deliberately absent: Renderer::Shutdown() calls it behind
        // `if (Renderer3D::HasInitialized())`, and that conditional IS the bug. (It therefore
        // also contributes the name "Renderer3D" to nothing — Renderer3D is not warmup-reachable,
        // so the conditional call inside the scanned body cannot skew the verdict.)
        constexpr TeardownSite kAlwaysRunTeardowns[] = {
            { "OloEngine/src/OloEngine/Renderer/Renderer.cpp", "void Renderer::Shutdown()" },
            { "OloEngine/src/OloEngine/Renderer/Renderer2D.cpp", "void Renderer2D::Shutdown()" },
            { "OloEngine/src/OloEngine/Renderer/ShaderWarmup.cpp", "void ShaderWarmup::Shutdown()" }
        };

        // The two sources a session that never initialises Renderer3D still runs end to end:
        // Renderer::Init() always calls ShaderWarmup::Init() then Renderer2D::Init(), and
        // Renderer2D::Init() drives ShaderWarmup::RenderProgressFrame(). See the second test.
        constexpr const char* kWarmupPathSources[] = {
            "OloEngine/src/OloEngine/Renderer/Renderer2D.cpp",
            "OloEngine/src/OloEngine/Renderer/ShaderWarmup.cpp"
        };

        // Facilities known to be engine-wide rather than 3D-only, SEEDED into the scan below
        // rather than discovered from a teardown body. Without this, deleting a release call
        // outright (rather than moving it back into Renderer3D::Shutdown) would drop the
        // facility from the discovered set entirely and the test would stop examining it —
        // failing, but on the wrong assertion and with misleading advice. Verified against the
        // compiled test: with the seed, BOTH "moved back into Renderer3D::Shutdown" and "deleted
        // outright" produce the real `unreleased.empty()` failure naming MeshPrimitives.
        constexpr const char* kEngineWideFacilities[] = { "MeshPrimitives" };

        [[nodiscard]] std::filesystem::path RepoFile(const std::string& relative)
        {
            // Walk up from the start cwd until the file appears, so the test does not care
            // whether it is run from the repo root or a build directory.
            std::error_code ec;
            for (std::filesystem::path dir = s_StartCwd; !dir.empty(); dir = dir.parent_path())
            {
                if (std::filesystem::path candidate = dir / relative;
                    std::filesystem::exists(candidate, ec))
                {
                    return candidate;
                }
                if (!dir.has_relative_path())
                {
                    break; // reached the root
                }
            }
            return s_StartCwd / relative; // report the path we looked for
        }

        // Every `Foo::Shutdown*()` / `Foo::Get().Shutdown*()` / `Foo::GetInstance().Shutdown*()`
        // in a teardown body, reduced to the bare facility name `Foo`. `Release*` and `Clear*`
        // are matched too — `TerrainGPUQuadtree::ReleaseSharedPatchMesh()` and
        // `GPUResourceQueue::Clear()` are release sites under a different verb. Members
        // (`s_Data.ForwardPlus.Shutdown()`) have no `::` and are deliberately not matched: they
        // belong to Renderer3D by construction, so they cannot be misfiled.
        [[nodiscard]] std::vector<std::string> CollectReleasedFacilities(const std::string& body)
        {
            const std::regex re(
                R"RX(([A-Z][A-Za-z0-9_]*)::(?:Get\(\)\.|GetInstance\(\)\.)?(?:Shutdown|Release|Clear)[A-Za-z0-9_]*\()RX");
            std::vector<std::string> names;
            for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
                 it != std::sregex_iterator(); ++it)
            {
                if (std::string name = (*it)[1].str();
                    std::find(names.begin(), names.end(), name) == names.end())
                {
                    names.push_back(std::move(name));
                }
            }
            return names;
        }
    } // namespace

    // Every GPU-owning static that DebugLiveGpuOwningStatics() knows about must be released
    // by Shutdown(). This is the invariant whose violation segfaulted the test binary at exit.
    TEST(RendererShutdown, ShutdownReleasesEveryGpuOwningStaticItKnowsAbout)
    {
        const std::string source = ReadFile(RepoFile(kLifecycleSource));
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

    // A facility whose release lives ONLY in Renderer3D::Shutdown() is never released in a session
    // that never initialises Renderer3D — and such sessions are real: Renderer::Init() always
    // brings up ShaderWarmup + Renderer2D, and OloRuntime exits before 3D init when it finds no
    // start scene. Renderer2D::Init() draws the warmup progress frame, which asks
    // MeshPrimitives::GetFullscreenTriangle() for the shared fullscreen-triangle VAO — a lazy
    // static whose only release site was inside Renderer3D::Shutdown(). It survived teardown in
    // every such session (#814): on Vulkan as a surviving VertexArray plus two live VMA
    // allocations at vmaDestroyAllocator; on GL as the same objects leaked silently, because GL
    // has no allocator-teardown assertion. The suite never saw it because the suite (like the
    // editor) always initialises Renderer3D.
    //
    // So: anything the warmup path can touch must be released by a teardown that ALWAYS runs —
    // Renderer::Shutdown(), which owns both sub-renderers, or one of the two it calls
    // unconditionally (Renderer2D's or ShaderWarmup's own). Releasing it where it is created is
    // equally correct; only Renderer3D::Shutdown() is disqualified, because Renderer::Shutdown()
    // guards it with `if (Renderer3D::HasInitialized())`.
    //
    // Source-text, for the reason the first test explains: Renderer3D does not support
    // Init-after-Shutdown, so "bring it up warmup-only, tear it down, assert nothing is left"
    // cannot be written in-process without destroying the rest of the suite.
    //
    // LIMIT, stated so nobody over-trusts it: reachability here is a ONE-HOP TEXTUAL scan of the
    // two warmup-path sources. A facility the warmup path reaches indirectly (through a third
    // file) is invisible to it. The empirical backstop is the Vulkan teardown forensics (#794),
    // which names every surviving object in a real `OloRuntime --rhi=vulkan` run.
    TEST(RendererShutdown, WarmupReachableFacilitiesAreReleasedBySubsystemNeutralShutdown)
    {
        const std::string lifecycleSource = ReadFile(RepoFile(kLifecycleSource));
        ASSERT_FALSE(lifecycleSource.empty()) << "could not read Renderer3DLifecycle.cpp — is the cwd the repo root?";

        const std::string shutdown3D =
            StripLineComments(ExtractFunctionBody(lifecycleSource, "void Renderer3D::Shutdown()"));
        ASSERT_FALSE(shutdown3D.empty())
            << "Renderer3D::Shutdown() not found — did it move? This test must be repointed.";

        std::string alwaysRunTeardown;
        for (const TeardownSite& site : kAlwaysRunTeardowns)
        {
            const std::string source = ReadFile(RepoFile(site.Source));
            ASSERT_FALSE(source.empty()) << "could not read " << site.Source << " — is the cwd the repo root?";
            const std::string body = StripLineComments(ExtractFunctionBody(source, site.Signature));
            ASSERT_FALSE(body.empty())
                << site.Signature << " not found in " << site.Source
                << " — did it move? This test must be repointed.";
            alwaysRunTeardown += body;
            alwaysRunTeardown += '\n';
        }

        // The union of both teardowns, plus the seeded engine-wide roster. The union is what
        // keeps moving a facility from one body to the other from making this test vacuous —
        // the very edit that fixes #814 removes MeshPrimitives from the 3D body — and the seed
        // is what keeps *deleting* a release call from doing the same.
        std::vector<std::string> facilities = CollectReleasedFacilities(shutdown3D);
        for (std::string& name : CollectReleasedFacilities(alwaysRunTeardown))
        {
            if (std::find(facilities.begin(), facilities.end(), name) == facilities.end())
            {
                facilities.push_back(std::move(name));
            }
        }
        for (const char* seeded : kEngineWideFacilities)
        {
            if (std::find(facilities.begin(), facilities.end(), seeded) == facilities.end())
            {
                facilities.emplace_back(seeded);
            }
        }
        ASSERT_FALSE(facilities.empty())
            << "parsed no released facilities out of either Shutdown() — the regex has drifted from the "
               "code, which would make this test silently vacuous";

        std::string warmupPath;
        for (const char* relative : kWarmupPathSources)
        {
            const std::string text = ReadFile(RepoFile(relative));
            ASSERT_FALSE(text.empty()) << "could not read " << relative;
            warmupPath += StripLineComments(text);
            warmupPath += '\n';
        }

        // A RELEASE CALL, not a mention: `MeshPrimitives::CreateCube()` appearing in a teardown
        // body must not read as a release. (Control flow is still beyond a text scan — a release
        // wrapped in `if (GetAPI() == Vulkan)` would satisfy this while GL leaked again. The
        // runtime forensics named above are the backstop for that.)
        const std::vector<std::string> released = CollectReleasedFacilities(alwaysRunTeardown);

        std::vector<std::string> warmupReachable;
        std::vector<std::string> unreleased;
        for (const std::string& facility : facilities)
        {
            if (warmupPath.find(facility + "::") == std::string::npos)
            {
                continue; // 3D-only by this scan — Renderer3D::Shutdown() is its rightful owner
            }
            warmupReachable.push_back(facility);
            if (std::find(released.begin(), released.end(), facility) == released.end())
            {
                unreleased.push_back(facility);
            }
        }

        // Non-vacuity: #814's own case must still be one of the cases this test examines. If
        // ShaderWarmup legitimately stops using the fullscreen triangle, update this expectation
        // deliberately rather than letting the test quietly stop covering anything.
        const bool meshPrimitivesExamined =
            std::find(warmupReachable.begin(), warmupReachable.end(), "MeshPrimitives") != warmupReachable.end();
        EXPECT_TRUE(meshPrimitivesExamined)
            << "MeshPrimitives is no longer detected as warmup-reachable. Either ShaderWarmup stopped "
               "using MeshPrimitives::GetFullscreenTriangle() (fine — retarget this expectation), or the "
               "scan above has drifted and is no longer testing anything.";

        std::string names;
        for (const std::string& n : unreleased)
        {
            names += (names.empty() ? "" : ", ") + n;
        }

        EXPECT_TRUE(unreleased.empty())
            << "these facilities are reachable from the warmup path (Renderer2D::Init / ShaderWarmup) but are\n"
               "NOT released by any teardown that always runs — only from Renderer3D::Shutdown(), or\n"
               "not at all: "
            << names
            << "\n\nRenderer3D::Shutdown() does not run in a session that never initialises Renderer3D — the\n"
               "OloRuntime start-scene-missing path is exactly that, and it is how #814 was found. Whatever\n"
               "such a facility allocated survives teardown: Vulkan names it at vmaDestroyAllocator, GL leaks\n"
               "it silently.\n\n"
               "Call `<Facility>::Shutdown();` from a teardown that ALWAYS runs — Renderer::Shutdown() in\n"
               "Renderer.cpp (it owns both sub-renderers), or the Renderer2D / ShaderWarmup teardown that\n"
               "created the resource in the first place.";
    }
} // namespace OloEngine::Tests
