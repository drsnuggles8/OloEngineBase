// OLO_TEST_LAYER: plumbing
// =============================================================================
// ShaderRegistryTeardownOrderTest.cpp
//
// Issue #1088 — AddressSanitizer heap-use-after-free at process exit:
//
//   READ of size 8 ... _Find_last
//     #3  ShaderResourceRegistry::Unregister(unsigned int)
//     #4  OloEngine::OpenGLShader::~OpenGLShader
//    #16  OloEngine::ShaderLibrary::~ShaderLibrary
//   freed by: `dynamic atexit destructor for 's_Registries'`
//
// ShaderResourceRegistry keeps a process-wide `u32 -> ShaderResourceRegistry*`
// map, and ~OpenGLShader unregisters from it. As a plain Meyers singleton the
// map is first touched at RUNTIME (when a shader is first linked), so its
// atexit entry is registered LATER than the ones for the namespace-scope
// ShaderLibrary statics (Renderer2D::m_ShaderLibrary and
// Renderer3D::m_ShaderLibrary, both dynamically initialised before main).
// Static destruction is LIFO, so the map is destroyed FIRST and every
// Ref<Shader> the shader libraries then release walks freed buckets.
//
// The fix makes the map immortal — a deliberately leaked heap allocation, the
// same shape RHI::ResourceRegistry::Get, VulkanImageInfoRegistry::Get and
// GetProgramLabelRegistry already use for exactly this reason. See
// docs/agent-rules/lazy-static-release-ownership.md.
//
// THE MAP WAS NOT ALONE. Tracing the rest of ~OpenGLShader found five more
// registries on the same path with the same defect: FrameResourceManager,
// RendererMemoryTracker, ShaderDebugger and Shader.cpp's two
// program-capability sets. kRegistrySites below is that whole roster.
//
// Only SOME of them can ever produce a report, and that is worth knowing before
// trusting a green sanitizer run. A Meyers singleton lives in static storage,
// which is never freed — only heap memory it OWNS is — so the fault needs the
// teardown call to reach heap-owning state. The map and the two sets reach
// find()/erase() on freed buckets; RendererMemoryTracker early-returns on
// m_IsShutdown (true after Renderer::Shutdown()), and ShaderDebugger
// early-returns on !m_IsInitialized, which Application.cpp alone ever sets.
// All six are unsafe by construction; an early-out just makes a real lifetime
// bug invisible to ASan rather than absent. Hence a SOURCE check over the whole
// roster, not a dynamic one over whichever fault aborts first.
//
// TWO GUARDS, because neither alone is enough:
//
//   1. A SOURCE-TEXT check on the accessor. The fault only manifests under
//      ASan, so every configuration that is NOT an ASan build — the msvc and
//      dev-cached trees, the plain CI build job — could reintroduce it and stay
//      green. This one fails in every configuration, on every platform, with no
//      GPU and no sanitizer.
//
//   2. A TEARDOWN PROBE, below, whose destructor runs during static
//      destruction after the map's would-be destructor. It makes the fault
//      deterministic under ASan for ANY invocation of this binary, instead of
//      only for the process shapes that happened to populate both statics —
//      which is what made #1088 look like a flake for as long as it did.
//
//      This is the half that gates CI, and the reason it needs no GPU is the
//      whole point. asan.yml DOES run a Windows clang-cl ASan job over the full
//      ctest suite — but on a GPU-less runner, so every test that creates a GL
//      shader skips, the registries are never populated, and the teardown fault
//      cannot arise. That is why #1088 reached master. The probe populates the
//      map itself and touches it from a static destructor, so it reproduces
//      there with no GL context at all.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderResourceRegistry.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        // A shader id no real GL program will hold. Register() only STORES the
        // pointer — it never dereferences it — so a pointer to this file's own
        // registry object is all the probe needs.
        constexpr u32 kProbeShaderID = 0xF1F10088u;

        ShaderResourceRegistry s_ProbeRegistry;

        // Set by the probe's constructor, which runs during dynamic
        // initialisation. The test below asserts it, because a probe that was
        // optimised away or never constructed guards nothing.
        bool s_ProbeArmed = false;

        // -------------------------------------------------------------------
        // Guard 2 — the teardown probe.
        // -------------------------------------------------------------------
        // Constructed during dynamic initialisation, i.e. before main and
        // therefore before anything first touches the registry map at runtime.
        // Static destruction is LIFO, so this destructor runs AFTER the map's
        // own destructor would have — the exact order ~ShaderLibrary and
        // ~OpenGLShader hit in #1088, reproduced unconditionally.
        //
        // The constructor deliberately does NOT touch the map: doing so would
        // register the map's atexit entry FIRST and invert the very ordering
        // this probe exists to exercise.
        struct ShaderRegistryTeardownProbe
        {
            ShaderRegistryTeardownProbe()
            {
                s_ProbeArmed = true;
            }

            ShaderRegistryTeardownProbe(const ShaderRegistryTeardownProbe&) = delete;
            ShaderRegistryTeardownProbe& operator=(const ShaderRegistryTeardownProbe&) = delete;

            ~ShaderRegistryTeardownProbe()
            {
                ShaderResourceRegistry::Register(kProbeShaderID, &s_ProbeRegistry);
                const bool found = ShaderResourceRegistry::Find(kProbeShaderID) == &s_ProbeRegistry;
                ShaderResourceRegistry::Unregister(kProbeShaderID);
                const bool cleared = ShaderResourceRegistry::Find(kProbeShaderID) == nullptr;

                if (found && cleared)
                {
                    return;
                }

                // Loud and countable rather than a silent early-out: under ASan
                // the read above already aborts with the #1088 report, but a
                // non-instrumented build reaches here with garbage instead, and
                // "the registry map did not survive teardown" must not be a
                // thing this binary can exit 0 on.
                std::fputs("\nFATAL: ShaderResourceRegistry's process map did not survive static "
                           "destruction (issue #1088). A shader released during teardown would "
                           "unregister into a destroyed map. See "
                           "docs/agent-rules/lazy-static-release-ownership.md.\n",
                           stderr);
                std::fflush(stderr);
                std::_Exit(88);
            }
        };

        const ShaderRegistryTeardownProbe s_TeardownProbe;

        // -------------------------------------------------------------------
        // Guard 1 — source-text helpers.
        // -------------------------------------------------------------------
        const std::filesystem::path s_StartCwd = std::filesystem::current_path();

        // Walk up from the start cwd until the file appears, so the test does
        // not care whether it runs from the repo root or a build directory.
        [[nodiscard]] std::filesystem::path RepoFile(const std::string& relative)
        {
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

        [[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path);
            EXPECT_TRUE(in.is_open()) << "cannot open " << path.string();
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Blank out comments and string/char literals, preserving length and
        // newlines so nothing else shifts.
        //
        // Load-bearing, not tidiness: the accessor this guard parses is mostly
        // a long explanatory comment, including a quoted ASan stack. Brace
        // matching over the raw text would let a single `}` written into that
        // comment — another stack frame, a `s_Data{}` — close the body early
        // and fail the check with a "#1088 has regressed" message while the fix
        // is perfectly intact. A stray `{` mirrors it, running the match past
        // the real closing brace.
        [[nodiscard]] std::string StripCommentsAndLiterals(const std::string& source)
        {
            std::string out = source;
            enum class State
            {
                Code,
                LineComment,
                BlockComment,
                String,
                Char
            };
            State state = State::Code;

            for (sizet i = 0; i < out.size(); ++i)
            {
                const char c = out[i];
                const char next = (i + 1 < out.size()) ? out[i + 1] : '\0';

                switch (state)
                {
                    case State::Code:
                        if (c == '/' && next == '/')
                        {
                            state = State::LineComment;
                            out[i] = out[i + 1] = ' ';
                            ++i;
                        }
                        else if (c == '/' && next == '*')
                        {
                            state = State::BlockComment;
                            out[i] = out[i + 1] = ' ';
                            ++i;
                        }
                        else if (c == '"')
                        {
                            state = State::String;
                        }
                        else if (c == '\'')
                        {
                            state = State::Char;
                        }
                        break;

                    case State::LineComment:
                        if (c == '\n')
                        {
                            state = State::Code;
                        }
                        else
                        {
                            out[i] = ' ';
                        }
                        break;

                    case State::BlockComment:
                        if (c == '*' && next == '/')
                        {
                            state = State::Code;
                            out[i] = out[i + 1] = ' ';
                            ++i;
                        }
                        else if (c != '\n')
                        {
                            out[i] = ' ';
                        }
                        break;

                    case State::String:
                    case State::Char:
                    {
                        const char closing = (state == State::String) ? '"' : '\'';
                        if (c == '\\')
                        {
                            out[i] = ' ';
                            if (i + 1 < out.size() && next != '\n')
                            {
                                out[i + 1] = ' ';
                                ++i;
                            }
                        }
                        else if (c == closing)
                        {
                            state = State::Code;
                        }
                        else if (c != '\n')
                        {
                            out[i] = ' ';
                        }
                        break;
                    }
                }
            }

            return out;
        }

        // The body of a function, from its signature to the matching closing
        // brace at brace-depth 0. Call with comment-stripped source.
        [[nodiscard]] std::string ExtractFunctionBody(const std::string& source,
                                                      const std::string& signature)
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
                    --depth;
                    if (depth == 0)
                    {
                        return source.substr(open, i - open + 1);
                    }
                }
            }
            return {};
        }

        // Every process-wide registry a shader destructor reaches on its way
        // out. All of them are touched during static destruction, because the
        // ShaderLibrary statics that own shaders (Renderer2D::m_ShaderLibrary,
        // Renderer3D::m_ShaderLibrary) are namespace-scope and outlive any
        // lazily-created singleton.
        //
        // SEEDED deliberately rather than discovered. A scan that discovers
        // what to check can stop checking — the lesson
        // docs/agent-rules/lazy-static-release-ownership.md learned twice. A
        // new registry on this path means a new row here.
        struct RegistrySite
        {
            const char* m_Source;
            const char* m_Signature;
            const char* m_ReachedBy;
        };

        constexpr RegistrySite kRegistrySites[] = {
            { "OloEngine/src/OloEngine/Renderer/ShaderResourceRegistry.cpp",
              "GetRegisteredShaderRegistryMap()",
              "~OpenGLShader -> ShaderResourceRegistry::Unregister (the #1088 report)" },
            { "OloEngine/src/OloEngine/Renderer/Commands/FrameResourceManager.cpp",
              "FrameResourceManager::Get()",
              "~OpenGLShader / ~OpenGLComputeShader -> Get().SubmitForDeletion" },
            { "OloEngine/src/OloEngine/Renderer/Debug/RendererMemoryTracker.cpp",
              "RendererMemoryTracker::GetInstance()",
              "~OpenGLShader -> OLO_TRACK_DEALLOC" },
            { "OloEngine/src/OloEngine/Renderer/Debug/ShaderDebugger.cpp",
              "ShaderDebugger::GetInstance()",
              "~OpenGLShader -> OLO_SHADER_UNREGISTER (OLO_DEBUG only)" },
            { "OloEngine/src/OloEngine/Renderer/Shader.cpp",
              "BindlessPrograms()",
              "~OpenGLShader -> deletion lambda -> Shader::UnregisterProgram" },
            { "OloEngine/src/OloEngine/Renderer/Shader.cpp",
              "MaterialOffsetPrograms()",
              "~OpenGLShader -> deletion lambda -> Shader::UnregisterProgram" },
        };
    } // namespace

    // The invariant, checked in every configuration: the accessor must hand
    // back a never-destroyed object. A `static std::unordered_map<...> m;`
    // there compiles, passes every non-ASan test, and reinstates #1088.
    TEST(ShaderRegistryTeardownOrderTest, EveryRegistryAShaderDestructorReachesIsNeverDestroyed)
    {
        // The leaked form: a static POINTER initialised from `new`. Nothing
        // else gives the object a lifetime that outlives static destruction.
        static const std::regex kLeakedSingleton(R"(static\s+[^;{}]*\*\s*\w+\s*=\s*new\b)");

        for (const RegistrySite& site : kRegistrySites)
        {
            const std::filesystem::path path = RepoFile(site.m_Source);
            const std::string source = ReadFile(path);
            ASSERT_FALSE(source.empty()) << "could not read " << path.string();

            const std::string body =
                ExtractFunctionBody(StripCommentsAndLiterals(source), site.m_Signature);
            ASSERT_FALSE(body.empty())
                << "could not find the body of " << site.m_Signature << " in " << path.string()
                << " — this guard has stopped checking that site. If the accessor was renamed or "
                   "moved, update kRegistrySites here rather than deleting the row.";

            EXPECT_TRUE(std::regex_search(body, kLeakedSingleton))
                << site.m_Signature << " in " << site.m_Source
                << " is no longer a deliberately leaked singleton.\n\nIts body is now:\n"
                << body << "\n\nThis registry is reached by " << site.m_ReachedBy
                << ", which runs during STATIC DESTRUCTION for any shader still owned by the "
                   "namespace-scope ShaderLibrary statics. A plain `static T x;` here is torn "
                   "down before they are, so the destructor reads freed memory (issue #1088, "
                   "ASan heap-use-after-free). Keep the `static auto* x = new ...` form. See "
                   "docs/agent-rules/lazy-static-release-ownership.md.";
        }
    }

    // Non-vacuity: a typo in a signature would make the loop above pass with
    // fewer sites than intended, and the per-site ASSERT only fires for rows
    // that ARE reached.
    TEST(ShaderRegistryTeardownOrderTest, TheRegistrySiteRosterIsNotEmptyOrTruncated)
    {
        ASSERT_GE(std::size(kRegistrySites), 6u)
            << "rows have been removed from kRegistrySites. Every registry a shader destructor "
               "reaches must stay listed; deleting a row silently stops checking it.";
    }

    // The extraction above must survive an edit to the accessor's comment.
    // Without comment stripping a single `}` written into that (long, and
    // brace-free only by luck) comment truncates the body and reports the fix
    // as regressed — a false red on a correct tree, which is the worst kind of
    // guard failure because the obvious response is to "fix" working code.
    TEST(ShaderRegistryTeardownOrderTest, BodyExtractionIgnoresBracesInCommentsAndLiterals)
    {
        const std::string decoy =
            "auto Accessor()\n"
            "{\n"
            "    // a closing brace in prose: }\n"
            "    /* and a block one: } plus an opener { */\n"
            "    const char* s = \"} { \\\" }\";\n"
            "    static auto* s_Thing = new std::unordered_map<u32, int>();\n"
            "    return *s_Thing;\n"
            "}\n";

        const std::string body = ExtractFunctionBody(StripCommentsAndLiterals(decoy), "Accessor()");
        ASSERT_FALSE(body.empty()) << "extraction found no body at all";
        EXPECT_NE(body.find("s_Thing"), std::string::npos)
            << "extraction stopped at a brace inside a comment or string literal, so the guard "
               "would read a truncated body and report a regression that is not there. Extracted:\n"
            << body;
    }

    // Non-vacuity for guard 2: an unconstructed or elided probe silently
    // guards nothing, and its destructor's failure mode is invisible from
    // inside the test binary's normal reporting.
    TEST(ShaderRegistryTeardownOrderTest, TeardownProbeIsArmed)
    {
        EXPECT_TRUE(s_ProbeArmed)
            << "the static-destruction probe was never constructed, so nothing will exercise the "
               "registry map after the shader libraries are torn down.";
    }

    // The same round-trip the probe performs at teardown, during the run,
    // where a failure can be reported normally. If this fails, the probe's
    // teardown result means nothing.
    TEST(ShaderRegistryTeardownOrderTest, RegisterFindUnregisterRoundTrips)
    {
        ShaderResourceRegistry::Register(kProbeShaderID, &s_ProbeRegistry);
        EXPECT_EQ(ShaderResourceRegistry::Find(kProbeShaderID), &s_ProbeRegistry);
        ShaderResourceRegistry::Unregister(kProbeShaderID);
        EXPECT_EQ(ShaderResourceRegistry::Find(kProbeShaderID), nullptr);
    }
} // namespace OloEngine::Tests
