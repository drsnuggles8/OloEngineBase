// OLO_TEST_LAYER: plumbing
// =============================================================================
// RHIBoundaryRatchetTest.cpp
//
// Phase 2 ratchet for issue #691 (RHI: add a Vulkan backend alongside OpenGL
// 4.6). Pulled forward from Phase 2 into Phase 1 — see ADR 0011
// (docs/adr/0011-rhi-neutral-resource-and-binding-model.md).
//
// Phase 2 is a ~313-call-site sweep whose only stated safety net is
// golden-image parity. That makes it a single heroic diff nobody can review.
// This test turns it into a measurable, incremental one: it counts raw OpenGL
// usage outside Platform/OpenGL/ and fails when any counter RISES above a
// checked-in baseline (rhi_boundary_baseline.json, next to this file).
//
// THE NUMBERS MAY ONLY GO DOWN. On a rise, do not edit the baseline — route the
// new call through the RendererAPI facade. On a fall, lower the baseline in the
// same commit; the failure message prints the exact value to paste.
//
// Counting rule (must stay identical to the one documented in the baseline
// JSON): an identifier matching gl[A-Z][A-Za-z0-9_]* immediately followed by
// '(', counted AFTER C/C++ comments and string/char/raw-string literal bodies
// are blanked out.
//
// Pattern precision matters more than the stripping: with this same pattern,
// blanking comments and literals removes only 16 of 565 hits, whereas a pattern
// loose enough to also match glfw* is what produced the task handover's "724"
// (and with it the phantom claim that Platform/*Window.cpp holds 65 GL calls —
// it holds none). Both are done here because this number is asserted on.
// =============================================================================

#include "OloEnginePCH.h"

// Compile the declaration-only RHI vocabulary. Nothing else includes these yet
// (that is the point — Phase 1 is "no code motion"), so without this they would
// never be parsed by a compiler and could rot into non-compiling code before
// Phase 2 picks them up.
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        fs::path RepoRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        }

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        }

        // Blank out comments and string/char/raw-string literal bodies, keeping
        // the result the same length as the input so byte offsets still line up
        // with the original (BlankLiterals' caller relies on that to read
        // #include paths, which are themselves quoted).
        std::string BlankLiterals(const std::string& text)
        {
            std::string out(text.size(), ' ');
            const sizet n = text.size();
            sizet i = 0;

            auto keep = [&](sizet from, sizet to)
            {
                for (sizet k = from; k < to && k < n; ++k)
                {
                    out[k] = text[k];
                }
            };
            // Newlines are preserved inside blanked spans so line-oriented
            // reasoning (and debugging) still works.
            auto blankKeepingNewlines = [&](sizet from, sizet to)
            {
                for (sizet k = from; k < to && k < n; ++k)
                {
                    out[k] = (text[k] == '\n') ? '\n' : ' ';
                }
            };

            while (i < n)
            {
                const char c = text[i];

                if (c == '/' && i + 1 < n && text[i + 1] == '/')
                {
                    sizet j = text.find('\n', i);
                    j = (j == std::string::npos) ? n : j;
                    blankKeepingNewlines(i, j);
                    i = j;
                    continue;
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '*')
                {
                    sizet j = text.find("*/", i + 2);
                    j = (j == std::string::npos) ? n : j + 2;
                    blankKeepingNewlines(i, j);
                    i = j;
                    continue;
                }

                // Raw string literal: R"delim( ... )delim". Must be handled
                // explicitly — this tree has 34 of them (GLSL snippets in
                // ShaderBindingLayout.h among others), and treating the opening
                // quote as an ordinary one would mis-scan the rest of the file.
                if (c == 'R' && i + 1 < n && text[i + 1] == '"')
                {
                    const bool isIdentifierChar = (i > 0) &&
                                                  (std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0 ||
                                                   text[i - 1] == '_');
                    if (!isIdentifierChar)
                    {
                        const sizet delimStart = i + 2;
                        const sizet openParen = text.find('(', delimStart);
                        if (openParen != std::string::npos && openParen - delimStart <= 16)
                        {
                            const std::string terminator =
                                ")" + text.substr(delimStart, openParen - delimStart) + "\"";
                            sizet j = text.find(terminator, openParen + 1);
                            j = (j == std::string::npos) ? n : j + terminator.size();
                            blankKeepingNewlines(i, j);
                            i = j;
                            continue;
                        }
                    }
                }

                // A `'` only opens a char literal when the previous character is
                // not alphanumeric. Without this, the DIGIT SEPARATOR in
                // `1'000'000.0` reads as a literal opener — and an odd number of
                // them on a line (`if (x == 1'000) glFinish();`) makes the
                // scanner blank forward to the newline and silently swallow a
                // real GL call. There are 77 digit separators under
                // OloEngine/src, four of them in Renderer/Debug/ files that do
                // call GL, so this is a live undercount risk, not a theoretical
                // one. The rule also skips the `'` in prefixed literals like
                // u8'x' / L'\n', which is harmless: a char literal cannot
                // contain a GL call either way.
                const bool opensCharLiteral =
                    c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0);

                if (c == '"' || opensCharLiteral)
                {
                    const char quote = c;
                    sizet j = i + 1;
                    while (j < n)
                    {
                        if (text[j] == '\\')
                        {
                            j += 2;
                            continue;
                        }
                        if (text[j] == quote)
                        {
                            ++j;
                            break;
                        }
                        if (text[j] == '\n') // unterminated — bail rather than eat the file
                        {
                            break;
                        }
                        ++j;
                    }
                    blankKeepingNewlines(i, j);
                    i = j;
                    continue;
                }

                keep(i, i + 1);
                ++i;
            }

            return out;
        }

        [[nodiscard]] bool IsWordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        // Count identifiers matching gl[A-Z][A-Za-z0-9_]* immediately followed
        // by '(' (optionally separated by whitespace).
        u32 CountGLCalls(const std::string& blanked)
        {
            u32 count = 0;
            const sizet n = blanked.size();

            for (sizet i = 0; i + 2 < n; ++i)
            {
                if (blanked[i] != 'g' || blanked[i + 1] != 'l')
                {
                    continue;
                }
                if (i > 0 && IsWordChar(blanked[i - 1]))
                {
                    continue; // part of a longer identifier
                }
                if (std::isupper(static_cast<unsigned char>(blanked[i + 2])) == 0)
                {
                    continue; // rules out glfw*, glm::, "gl" alone
                }

                sizet j = i + 2;
                while (j < n && IsWordChar(blanked[j]))
                {
                    ++j;
                }
                while (j < n && (blanked[j] == ' ' || blanked[j] == '\t' || blanked[j] == '\n' || blanked[j] == '\r'))
                {
                    ++j;
                }
                if (j < n && blanked[j] == '(')
                {
                    ++count;
                    i = j;
                }
            }

            return count;
        }

        u32 CountOccurrences(const std::string& blanked, std::string_view needle)
        {
            u32 count = 0;
            for (sizet pos = blanked.find(needle); pos != std::string::npos;
                 pos = blanked.find(needle, pos + needle.size()))
            {
                ++count;
            }
            return count;
        }

        // True when the file has a live (non-commented-out) #include naming
        // `header`. Searched on the blanked text so a commented-out include does
        // not count, but the path is read back out of the ORIGINAL text at the
        // same offset — a quoted #include "glad/gl.h" has its path blanked as a
        // string literal, and BlankLiterals preserves offsets exactly so this
        // stays valid.
        bool IncludesHeader(const std::string& raw, const std::string& blanked, std::string_view header)
        {
            static constexpr std::string_view kInclude = "#include";
            const std::string_view rawView{ raw };
            for (sizet pos = blanked.find(kInclude); pos != std::string::npos;
                 pos = blanked.find(kInclude, pos + kInclude.size()))
            {
                sizet eol = blanked.find('\n', pos);
                eol = (eol == std::string::npos) ? raw.size() : eol;
                if (rawView.substr(pos, eol - pos).find(header) != std::string_view::npos)
                {
                    return true;
                }
            }
            return false;
        }

        bool IncludesGlad(const std::string& raw, const std::string& blanked)
        {
            return IncludesHeader(raw, blanked, "glad/gl.h");
        }

        enum class Bucket
        {
            Sweep,   ///< OloEngine/** minus Renderer/Debug/ — Phase 2, must reach zero
            Tools,   ///< OloEngine/Renderer/Debug/ — Phase 8, relocated not exempted
            Backend, ///< Platform/OpenGL/ — the backend itself, never a violation
        };

        [[nodiscard]] std::string Normalize(const fs::path& relative)
        {
            std::string s = relative.generic_string();
            return s;
        }

        [[nodiscard]] Bucket BucketFor(const std::string& rel)
        {
            if (rel.starts_with("Platform/OpenGL/"))
            {
                return Bucket::Backend;
            }
            if (rel.starts_with("OloEngine/Renderer/Debug/"))
            {
                return Bucket::Tools;
            }
            return Bucket::Sweep;
        }

        struct Tally
        {
            u32 SweepGLCalls = 0;
            u32 SweepGladIncludes = 0;
            u32 ToolsGLCalls = 0;
            u32 DebugEscapeHatch = 0;

            // Sanity anchors — see the FloorGuards test. Without these, a broken
            // scanner or a wrong repo root reports every counter as zero and the
            // ratchet passes forever while measuring nothing.
            u32 BackendGLCalls = 0;
            u32 FilesScanned = 0;

            // Reported on failure so the sweep has a work list, not just a number.
            std::map<std::string, u32> SweepCallsByFile;
        };

        Tally Scan()
        {
            Tally tally;
            const fs::path src = RepoRoot() / "OloEngine" / "src";
            const std::set<std::string> extensions = { ".cpp", ".h", ".hpp", ".inl", ".cc", ".cxx" };

            std::error_code ec;
            for (fs::recursive_directory_iterator it(src, ec), end; it != end; it.increment(ec))
            {
                // The error_code overload throughout: a permission hiccup or a
                // dangling symlink must not throw out of a test whose whole job
                // is to report a number.
                std::error_code entryEc;
                if (ec || !it->is_regular_file(entryEc) || entryEc)
                {
                    continue;
                }
                if (!extensions.contains(it->path().extension().string()))
                {
                    continue;
                }

                const std::string rel = Normalize(fs::relative(it->path(), src, ec));
                if (ec)
                {
                    continue;
                }

                ++tally.FilesScanned;

                const std::string raw = ReadFile(it->path());
                const std::string blanked = BlankLiterals(raw);
                const u32 calls = CountGLCalls(blanked);
                const bool glad = IncludesGlad(raw, blanked);

                switch (BucketFor(rel))
                {
                    case Bucket::Backend:
                        tally.BackendGLCalls += calls;
                        break;
                    case Bucket::Tools:
                        tally.ToolsGLCalls += calls;
                        break;
                    case Bucket::Sweep:
                        tally.SweepGLCalls += calls;
                        if (calls > 0)
                        {
                            tally.SweepCallsByFile[rel] = calls;
                        }
                        if (glad)
                        {
                            ++tally.SweepGladIncludes;
                        }
                        break;
                }

                // The escape hatch is legitimate in Renderer/Debug/ (the
                // introspection tools genuinely need the native object) and in
                // Platform/ (that is where native handles live). Renderer/RHI/
                // is excluded because it DECLARES the function — counting the
                // declaration would put the baseline at 1 and hide the first
                // real caller.
                const bool escapeHatchAllowed = rel.starts_with("Platform/") ||
                                                rel.starts_with("OloEngine/Renderer/Debug/") ||
                                                rel.starts_with("OloEngine/Renderer/RHI/");
                if (!escapeHatchAllowed)
                {
                    tally.DebugEscapeHatch += CountOccurrences(blanked, "GetNativeHandleForDebug");
                }
            }

            return tally;
        }

        nlohmann::json LoadBaseline()
        {
            const fs::path path =
                RepoRoot() / "OloEngine" / "tests" / "Rendering" / "rhi_boundary_baseline.json";
            std::ifstream in(path);
            EXPECT_TRUE(in.is_open()) << "Missing ratchet baseline: " << path.string();
            if (!in.is_open())
            {
                return nlohmann::json::object();
            }
            return nlohmann::json::parse(in, nullptr, false);
        }

        void ExpectRatchet(const char* counter, u32 measured, const nlohmann::json& baseline)
        {
            ASSERT_TRUE(baseline.contains(counter)) << "Baseline JSON has no key '" << counter << "'";
            const u32 allowed = baseline.at(counter).get<u32>();

            EXPECT_LE(measured, allowed)
                << "\n  RHI boundary ratchet REGRESSED: '" << counter << "' rose from " << allowed
                << " to " << measured << ".\n"
                << "  New raw OpenGL was added outside Platform/OpenGL/ (issue #691, ADR 0011).\n"
                << "  Do NOT raise the baseline to make this green — route the call through the\n"
                << "  RendererAPI facade instead. If the facade genuinely cannot express it,\n"
                << "  that is a Phase 2 design gap worth a comment on #691.\n";

            if (measured < allowed)
            {
                ADD_FAILURE()
                    << "\n  RHI boundary ratchet IMPROVED: '" << counter << "' fell from " << allowed
                    << " to " << measured << ". Nice — now lock it in.\n"
                    << "  Edit OloEngine/tests/Rendering/rhi_boundary_baseline.json:\n"
                    << "      \"" << counter << "\": " << measured << "\n"
                    << "  (The ratchet only holds if the baseline follows the progress down.)\n";
            }
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The ratchet proper.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, RawGLOutsideTheBackendOnlyEverShrinks)
    {
        const Tally tally = Scan();
        const nlohmann::json baseline = LoadBaseline();
        ASSERT_FALSE(baseline.is_discarded()) << "rhi_boundary_baseline.json failed to parse";

        ExpectRatchet("sweep_gl_calls", tally.SweepGLCalls, baseline);
        ExpectRatchet("sweep_glad_includes", tally.SweepGladIncludes, baseline);
        ExpectRatchet("tools_gl_calls", tally.ToolsGLCalls, baseline);
        ExpectRatchet("debug_escape_hatch", tally.DebugEscapeHatch, baseline);

        if (::testing::Test::HasFailure())
        {
            std::ostringstream work;
            work << "\n  Sweep-bucket files still calling GL (" << tally.SweepCallsByFile.size()
                 << " files, " << tally.SweepGLCalls << " calls):\n";
            for (const auto& [file, count] : tally.SweepCallsByFile)
            {
                work << "      " << count << "\t" << file << "\n";
            }
            GTEST_LOG_(INFO) << work.str();
        }
    }

    // -------------------------------------------------------------------------
    // Guards against the ratchet silently measuring nothing.
    //
    // Every counter above is an upper-bound assertion, so a wrong repo root, a
    // broken literal-stripper, or a changed directory layout would report zero
    // for everything and pass forever. These floors make that failure loud.
    // Their thresholds are deliberately far below the real values (497 backend
    // calls, 1417 files) so ordinary progress never trips them — they detect a
    // broken harness, not a changed codebase.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, FloorGuardsProveTheScannerActuallyRan)
    {
        const Tally tally = Scan();

        EXPECT_GT(tally.FilesScanned, 500u)
            << "Scanned only " << tally.FilesScanned << " files under OloEngine/src — the walk is "
            << "broken or OLO_TEST_EDITOR_ROOT points somewhere unexpected. Every ratchet "
            << "assertion is an upper bound, so this would otherwise pass while measuring nothing.";

        EXPECT_GT(tally.BackendGLCalls, 100u)
            << "Found only " << tally.BackendGLCalls << " GL calls inside Platform/OpenGL/. The "
            << "OpenGL backend is the one place raw GL is expected, so a low count here means the "
            << "literal-stripper or the call matcher is broken, not that the backend shrank.";
    }

    // -------------------------------------------------------------------------
    // The counting rule is load-bearing (the baseline numbers only mean anything
    // relative to it), so pin it directly rather than only through the corpus.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, CountingRuleIgnoresCommentsStringsAndNonGLPrefixes)
    {
        auto count = [](const std::string& source)
        { return CountGLCalls(BlankLiterals(source)); };

        EXPECT_EQ(count("glBindTexture(0, 1);"), 1u);
        EXPECT_EQ(count("glBindTexture (0, 1);"), 1u) << "whitespace before '(' is still a call";
        EXPECT_EQ(count("a = glGetError();\nb = glGetError();"), 2u);

        EXPECT_EQ(count("// calls glBindTexture(0, 1) eventually"), 0u) << "line comment";
        EXPECT_EQ(count("/* glBindTexture(0, 1); */"), 0u) << "block comment";
        EXPECT_EQ(count("Log(\"failed in glBindTexture()\");"), 0u) << "string literal";
        EXPECT_EQ(count("const char* s = R\"(glBindTexture(0,1))\";"), 0u) << "raw string literal";
        EXPECT_EQ(count("auto s = R\"glsl(glBindTexture(0,1))glsl\";"), 0u) << "delimited raw string";

        EXPECT_EQ(count("glfwInit();"), 0u) << "GLFW is not GL — this is the 724-vs-549 discrepancy";
        EXPECT_EQ(count("glm::vec3 v;"), 0u) << "glm is not GL";
        EXPECT_EQ(count("m_glBindTexture(0);"), 0u) << "must be at an identifier boundary";
        EXPECT_EQ(count("GLenum e = GL_TRUE;"), 0u) << "types and constants are counted separately";
        EXPECT_EQ(count("myglBindTexture(0);"), 0u) << "suffix of a longer identifier";

        // A block comment closed on a later line must not swallow real calls.
        EXPECT_EQ(count("/* note\n   about glClear(0) */\nglClear(0);"), 1u);

        // Digit separators are not char literals. `1'000` has an ODD number of
        // quotes, so a naive scanner blanks forward to the newline and loses the
        // call — a silent undercount, the worst failure mode for a ratchet.
        EXPECT_EQ(count("if (x == 1'000) glFinish();"), 1u) << "odd digit separator";
        EXPECT_EQ(count("f = t / 1'000'000.0; glFinish();"), 1u) << "even digit separators";
        EXPECT_EQ(count("char c = ';'; glFinish();"), 1u) << "a real char literal still blanks";
    }

    // -------------------------------------------------------------------------
    // The RHI vocabulary headers must stay backend-free. This is the whole
    // premise of ADR 0011: `Renderer/RHI/` is the one place the sweep converts
    // *toward*, so a backend header appearing there would quietly re-open the
    // boundary at its narrowest point.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, RHIHeadersNameNoBackendTypes)
    {
        const fs::path rhiDir = RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Renderer" / "RHI";
        ASSERT_TRUE(fs::exists(rhiDir)) << "Missing " << rhiDir.string();

        static constexpr std::string_view kForbiddenHeaders[] = {
            "glad/gl.h",
            "vulkan/vulkan.h",
            "volk.h",
        };
        static constexpr std::string_view kForbiddenTypes[] = {
            "GLenum",
            "GLuint",
            "GLint",
            "GLsizei",
            "VkImage",
            "VkBuffer",
            "VkDevice",
        };

        u32 headersChecked = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(rhiDir, ec), end; it != end; it.increment(ec))
        {
            std::error_code entryEc;
            if (ec || !it->is_regular_file(entryEc) || entryEc || it->path().extension() != ".h")
            {
                continue;
            }
            ++headersChecked;

            const std::string raw = ReadFile(it->path());

            // Checked on the BLANKED text, not the raw text. The first version of
            // this test scanned raw and immediately failed on the RHI headers
            // themselves — whose comments name GLenum and VkImage precisely to
            // explain that they are forbidden. A guard that bans a token cannot
            // also ban the documentation of the ban; the property worth enforcing
            // is that no backend type is *used*, and blanking comments and string
            // literals tests exactly that.
            const std::string blanked = BlankLiterals(raw);
            for (const std::string_view forbidden : kForbiddenTypes)
            {
                EXPECT_EQ(blanked.find(forbidden), std::string::npos)
                    << it->path().filename().string() << " uses the backend type '" << forbidden
                    << "'. Renderer/RHI/ is the API-neutral vocabulary (ADR 0011) — if you need a "
                    << "backend type you are writing backend code, and it belongs in Platform/<Backend>/.";
            }

            // Includes go through the #include scan rather than a plain find, so
            // that naming a header in prose is fine while actually including it
            // is not — and so a quoted include is still caught despite its path
            // being blanked as a string literal.
            for (const std::string_view forbidden : kForbiddenHeaders)
            {
                EXPECT_FALSE(IncludesHeader(raw, blanked, forbidden))
                    << it->path().filename().string() << " includes the backend header '" << forbidden
                    << "'. Renderer/RHI/ must stay compilable without any backend SDK present.";
            }
        }

        EXPECT_GE(headersChecked, 2u) << "Expected at least RHITypes.h and RHIResources.h";
    }

    // -------------------------------------------------------------------------
    // Compile-time properties the sweep will depend on.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, HeapOffsetIsShaderVisibleAndHandlesAreComparable)
    {
        // HeapOffset is written into UBO/SSBO fields and read by GLSL as an
        // array index, so it must stay layout-compatible with a plain u32.
        static_assert(sizeof(RHI::HeapOffset) == sizeof(u32));
        static_assert(std::is_trivially_copyable_v<RHI::HeapOffset>);
        static_assert(std::is_trivially_copyable_v<RHI::ResourceHandle>);

        // Default-constructed handles are invalid, so a forgotten assignment is
        // caught rather than silently addressing slot 0 / object 0 — which is
        // exactly what a default-constructed `u32 RendererID = 0` does today.
        EXPECT_FALSE(RHI::ResourceHandle{}.IsValid());
        EXPECT_FALSE(RHI::HeapOffset{}.IsValid());

        // The generation is what a raw GL name cannot provide: GL recycles
        // object names, so two different objects can compare equal through
        // Texture::operator== (which compares GetRendererID()) when one was
        // deleted and another created. TransientPool's alias reporting depends
        // on telling those apart.
        constexpr RHI::ResourceHandle first{ .Index = 7, .Generation = 1 };
        constexpr RHI::ResourceHandle recycled{ .Index = 7, .Generation = 2 };
        EXPECT_FALSE(first == recycled);
        EXPECT_TRUE(first.IsValid());

        // View identity is a SEPARATE handle type, not a ResourceHandle reused:
        // one resource maps to many views (CreateDepthArrayCompareOffView is the
        // existing proof) and the view is what owns a heap slot. The two must not
        // be silently interchangeable, so this must not compile as an assignment.
        static_assert(!std::is_convertible_v<RHI::ResourceHandle, RHI::ViewHandle>);
        static_assert(!std::is_convertible_v<RHI::ViewHandle, RHI::ResourceHandle>);
        static_assert(std::is_trivially_copyable_v<RHI::ViewHandle>);
        EXPECT_FALSE(RHI::ViewHandle{}.IsValid());

        // Write accesses must classify as writes — the unified Access lattice
        // exists because ResourceTransition's RGWriteUsage -> RGReadUsage pair
        // structurally cannot express a write -> write transition (ADR 0011 §1.5).
        static_assert(RHI::IsWriteAccess(RHI::Access::StorageWrite));
        static_assert(RHI::IsWriteAccess(RHI::Access::ColorAttachmentWrite));
        static_assert(!RHI::IsWriteAccess(RHI::Access::ShaderSampleRead));
        static_assert(!RHI::IsWriteAccess(RHI::Access::Undefined));
    }
} // namespace OloEngine::Tests
