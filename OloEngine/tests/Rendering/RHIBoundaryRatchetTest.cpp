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
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
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

        // Phase 3's bind-site rule, in ONE place so the corpus scan and the
        // unit test cannot drift apart.
        //
        // The identifier-boundary guard is not cosmetic: `BindTexture(` is a
        // SUFFIX of `glBindTexture(`, so without it a single raw GL call in the
        // sweep bucket would inflate both `sweep_gl_calls` and
        // `sweep_bind_texture_sites`. That bucket is at zero today, so the
        // numbers happen to be unaffected — exactly the latent-until-someone-
        // regresses shape the digit-separator trap had.
        u32 CountBindSites(const std::string& blanked)
        {
            u32 count = 0;
            for (const std::string_view needle : { std::string_view("BindTexture("),
                                                   std::string_view("BindImageTexture(") })
            {
                for (sizet pos = blanked.find(needle); pos != std::string::npos;
                     pos = blanked.find(needle, pos + needle.size()))
                {
                    // Must start an identifier. `BindTexture(` inside
                    // `glBindTexture(` is preceded by 'l' and is not a site.
                    if (pos > 0 && IsWordChar(blanked[pos - 1]))
                    {
                        continue;
                    }
                    ++count;
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

            // Phase 2 step 3 (the identity currency). `sweep_renderer_id` counts
            // the identifier `RendererID` in any spelling — the `GetRendererID()`
            // accessor, the `using RendererID = u32` alias, and the backend's
            // `m_RendererID` member — anywhere outside Platform/ and
            // Renderer/Debug/. A native object name may not be NAMED in the
            // sweep bucket, let alone held.
            u32 SweepRendererId = 0;
            // Uses of the backend's handle->native escape hatch outside
            // Platform/. Sibling of DebugEscapeHatch: two hatches, two baselines,
            // both zero where they do not belong.
            u32 BackendResolveHatch = 0;

            // Phase 3 (the bindless rehearsal). Slot-based texture-binding call
            // sites outside Platform/ and Renderer/Debug/ — every `BindTexture(`
            // and `BindImageTexture(` the engine still performs. Under
            // heap-bindless there is no slot to bind to: the pass writes
            // `RHI::OffsetOf(view)` into a UBO and the shader indexes the heap
            // (ADR 0011 §1.3 item 1), so this is the counter that measures the
            // phase's actual conversion rather than its infrastructure.
            //
            // IT IS NOT EXPECTED TO REACH ZERO IN PHASE 3, and that is a
            // deliberate difference from the Phase 2 counters. `ARB_bindless_texture`
            // is not universally available, so the slot-based path must survive
            // as the fallback on any device without it — a zero here would mean
            // the engine had stopped working on those machines, not that the
            // phase was finished. What it must do is FALL, and never rise.
            u32 SweepBindTextureSites = 0;
            std::map<std::string, u32> SweepBindTextureSitesByFile;

            std::map<std::string, u32> SweepRendererIdByFile;

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

                // --- Phase 2 step 3: the identity currency -------------------
                //
                // `Platform/` owns native names, and `Renderer/Debug/` still
                // holds them pending its Phase 8 relocation. Everywhere else,
                // naming one at all is the regression.
                //
                // Renderer/RHI/ is NOT exempted here, unlike for the debug
                // escape hatch: the registry names no native object, it stores
                // an opaque u64, so a `RendererID` appearing there would be a
                // real leak rather than a declaration.
                const bool nativeNamesAllowed =
                    rel.starts_with("Platform/") || rel.starts_with("OloEngine/Renderer/Debug/");
                if (!nativeNamesAllowed)
                {
                    const u32 rendererIds = CountOccurrences(blanked, "RendererID");
                    tally.SweepRendererId += rendererIds;
                    if (rendererIds > 0)
                    {
                        tally.SweepRendererIdByFile[rel] = rendererIds;
                    }
                }

                // Renderer/RHI/ is excluded for the same reason the debug hatch
                // excludes it: the registry DECLARES and DEFINES this function,
                // so counting those would put the baseline above zero and hide
                // the first real caller — which is the only thing worth seeing.
                if (!rel.starts_with("Platform/") && !rel.starts_with("OloEngine/Renderer/RHI/"))
                {
                    tally.BackendResolveHatch += CountOccurrences(blanked, "ResolveNativeForBackend");
                }

                // --- Phase 3: the act of binding ----------------------------
                //
                // Same bucket rule as the identity currency: `Platform/` is the
                // backend and legitimately binds, and `Renderer/Debug/` binds to
                // inspect. Everywhere else, a texture bind is a site heap-bindless
                // deletes.
                //
                // `Renderer/RHI/` is NOT exempted. It declares the heap that
                // replaces binding and must never perform one — and because the
                // count runs on the blanked text, the header can go on
                // explaining what it replaces without inflating the number.
                //
                // The trailing '(' matters: it counts CALLS and DECLARATIONS but
                // not the many prose mentions of the family name in the facade's
                // own comments, which is the same precision the `gl[A-Z](` rule
                // was written for. Note "BindTexture(" is deliberately not a
                // substring of "BindImageTexture(", so the two needles do not
                // double-count each other.
                if (!nativeNamesAllowed)
                {
                    const u32 binds = CountBindSites(blanked);
                    tally.SweepBindTextureSites += binds;
                    if (binds > 0)
                    {
                        tally.SweepBindTextureSitesByFile[rel] = binds;
                    }
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
        ExpectRatchet("sweep_renderer_id", tally.SweepRendererId, baseline);
        ExpectRatchet("backend_resolve_hatch", tally.BackendResolveHatch, baseline);
        ExpectRatchet("sweep_bind_texture_sites", tally.SweepBindTextureSites, baseline);

        if (::testing::Test::HasFailure())
        {
            std::ostringstream work;
            work << "\n  Sweep-bucket files still calling GL (" << tally.SweepCallsByFile.size()
                 << " files, " << tally.SweepGLCalls << " calls):\n";
            for (const auto& [file, count] : tally.SweepCallsByFile)
            {
                work << "      " << count << "\t" << file << "\n";
            }
            work << "\n  Files still naming a native renderer ID ("
                 << tally.SweepRendererIdByFile.size() << " files, " << tally.SweepRendererId
                 << " mentions):\n";
            for (const auto& [file, count] : tally.SweepRendererIdByFile)
            {
                work << "      " << count << "\t" << file << "\n";
            }
            work << "\n  Files still binding textures to slots ("
                 << tally.SweepBindTextureSitesByFile.size() << " files, " << tally.SweepBindTextureSites
                 << " sites) — each becomes a heap offset written into a UBO:\n";
            for (const auto& [file, count] : tally.SweepBindTextureSitesByFile)
            {
                work << "      " << count << "\t" << file << "\n";
            }
            GTEST_LOG_(INFO) << work.str();
        }
    }

    // -------------------------------------------------------------------------
    // The structural half of Phase 2 step 3, and the reason the counters above
    // are a regression guard rather than the proof.
    //
    // A count of `RendererID` mentions is gameable: rename the accessor and the
    // number goes to zero while a native GL name still crosses the boundary.
    // What is NOT gameable is the type system. `RHI::ResourceHandle` is not
    // constructible or convertible from an integer, so a translation unit
    // holding a native name literally cannot call the facade with it — the same
    // "provable rather than measured" property ADR 0011 §1.7 credits
    // `sweep_glad_includes` with, one level up.
    //
    // This test pins the two things that property rests on, both of which a
    // well-meaning refactor could remove without any counter noticing.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, ResourceHandleCannotBeSpelledAsANativeId)
    {
        // (a) NO IMPLICIT CONVERSION EITHER WAY. This is the property the sweep
        //     rests on: once the facade takes handles, `BindTexture(slot, myU32)`
        //     does not compile, so a translation unit holding a native name
        //     cannot reach the driver through it.
        static_assert(!std::is_convertible_v<u32, RHI::ResourceHandle>);
        static_assert(!std::is_convertible_v<RHI::ResourceHandle, u32>);

        // (b) `is_constructible_v<RHI::ResourceHandle, u32>` is deliberately NOT
        //     asserted false, because it is TRUE and cannot be made false without
        //     giving up the aggregate.
        //
        //     Handle is an aggregate `{ u32 Index; u32 Generation; }`, and C++20
        //     parenthesized aggregate initialization (P0960) makes
        //     `RHI::ResourceHandle(someNativeId)` well-formed — it initialises
        //     Index and value-initialises Generation. An earlier version of this
        //     test asserted the opposite and failed to compile, which is how the
        //     hole was found.
        //
        //     It is a narrow and self-defusing hole, and the assertions below are
        //     what make that true rather than merely hoped:
        //       * it needs EXPLICIT direct-init syntax, so it cannot happen by
        //         accident at a call site — copy-init `ResourceHandle h = id;` is
        //         still rejected, which is what (a) pins;
        //       * the result carries Generation == 0, which `IsValid()` rejects
        //         by construction, so it names no object and resolves to 0.
        //     A native id smuggled through this route is therefore inert, not
        //     mistaken for a live resource.
        static_assert(std::is_constructible_v<RHI::ResourceHandle, u32>,
                      "If this ever becomes false the comment above is stale — good news, "
                      "but re-check whether Handle is still an aggregate.");
        {
            constexpr RHI::ResourceHandle smuggled(42u);
            static_assert(smuggled.Index == 42u);
            static_assert(smuggled.Generation == 0u);
            static_assert(!smuggled.IsValid(),
                          "A handle built from a bare integer must name nothing. If a future "
                          "Handle gains a non-zero default Generation this stops holding and "
                          "the aggregate-init hole stops being inert.");
            EXPECT_EQ(RHI::ResourceRegistry::Get().ResolveNativeForBackend(smuggled), 0u);
        }

        // (c) The two identity levels stay mutually unrelated, so Phase 3 can
        //     introduce ViewHandle over the top of this without a sweep. See
        //     ADR 0011 amendment (11) for why ViewHandle is deferred rather than
        //     built now.
        static_assert(!std::is_convertible_v<RHI::ResourceHandle, RHI::ViewHandle>);
        static_assert(!std::is_convertible_v<RHI::ViewHandle, RHI::ResourceHandle>);
        static_assert(!std::is_constructible_v<RHI::ResourceHandle, RHI::ViewHandle>);
        static_assert(!std::is_constructible_v<RHI::ViewHandle, RHI::ResourceHandle>);
    }

    // -------------------------------------------------------------------------
    // The facade is where the boundary is actually drawn, so assert on it
    // directly rather than only on the aggregate corpus count. A new virtual
    // taking `u32 someTextureID` would keep every counter above at zero while
    // handing the sweep bucket a native name to produce.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, FacadeNativeIdParametersOnlyEverShrink)
    {
        const fs::path header = RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Renderer" / "RendererAPI.h";
        const std::string blanked = BlankLiterals(ReadFile(header));
        ASSERT_FALSE(blanked.empty()) << "Could not read " << header.string();

        // `u32 <something>ID` / `<something>Id` in a declaration. The engine's
        // own non-resource u32s (slot, unit, mipLevel, attachmentIndex, width,
        // bindingPoint, ...) do not end in ID, and the one that does — the
        // debug-marker `u32 id` in PushDebugGroup — is not a resource and is
        // spelled without a prefix, so it is excluded by requiring at least one
        // leading character.
        const std::regex nativeIdParam(R"(\bu32\s+[A-Za-z_]\w*(ID|Id)\b)");

        std::vector<std::string> offenders;
        for (std::sregex_iterator it(blanked.begin(), blanked.end(), nativeIdParam), end; it != end; ++it)
        {
            offenders.push_back(it->str());
        }

        const nlohmann::json baseline = LoadBaseline();
        ASSERT_FALSE(baseline.is_discarded()) << "rhi_boundary_baseline.json failed to parse";
        ExpectRatchet("facade_native_id_params", static_cast<u32>(offenders.size()), baseline);

        if (::testing::Test::HasFailure())
        {
            std::ostringstream work;
            work << "\n  RendererAPI parameters still naming a native resource id ("
                 << offenders.size() << "):\n";
            for (const auto& o : offenders)
            {
                work << "      " << o << "\n";
            }
            work << "  ADR 0011 §1.1: the facade's currency is identity, not a driver name.\n"
                 << "  Take RHI::ResourceHandle and resolve it inside Platform/<Backend>/.\n";
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

        // Phase 3's counter needs its own floor, and for a reason the other two
        // do not have: it is the ONLY counter here that a legitimate change
        // could drive to zero, so "zero" is not self-evidently a broken
        // scanner. It is nonetheless not reachable — the slot-based path must
        // survive for devices without ARB_bindless_texture (that is the whole
        // fallback design), so the engine will always contain some.
        //
        // 5, NOT 20. The count is 22 today, so a floor of 20 left two sites of
        // headroom and would have FAILED a legitimate further conversion — the
        // opposite of what a floor guard is for, and flatly contrary to the "an
        // order of magnitude below today's count" this comment used to claim.
        // The irreducible residue is the facade declarations plus the seam's own
        // fallbacks, on the order of ten; 5 sits below that and still catches a
        // needle that has stopped matching.
        EXPECT_GT(tally.SweepBindTextureSites, 5u)
            << "Found only " << tally.SweepBindTextureSites << " slot-based texture-bind sites outside "
            << "Platform/. Heap-bindless deletes these, but it cannot delete ALL of them: the "
            << "slot-based path is the fallback for devices without ARB_bindless_texture. A number "
            << "this low means the needle stopped matching, not that the conversion finished.";
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

        // Phase 3's bind-site needle rides on the same blanking, so pin it the
        // same way. The trailing '(' is what separates a call from the facade's
        // own prose, and the two needles must not double-count each other —
        // "BindTexture(" is deliberately NOT a substring of "BindImageTexture(",
        // and a needle pair that overlapped would inflate the baseline in a way
        // no assertion could distinguish from real bind sites.
        // Calls the SAME function the corpus scan uses, deliberately. An earlier
        // version re-implemented the expression here, so a fix in one place would
        // not have reached the other — the exact drift this file's own counting-
        // rule tests exist to prevent.
        auto countBinds = [](const std::string& source)
        { return CountBindSites(BlankLiterals(source)); };

        EXPECT_EQ(countBinds("context.BindTexture(TEX_DIFFUSE, tex);"), 1u);
        EXPECT_EQ(countBinds("RenderCommand::BindImageTexture(0, tex, 0, false, 0, a, f);"), 1u);
        EXPECT_EQ(countBinds("BindImageTexture(0, t, 0, false, 0, a, f);"), 1u)
            << "BindImageTexture must count once, not once for each needle";
        EXPECT_EQ(countBinds("// BindTexture(slot, tex) disappears under heap-bindless"), 0u)
            << "prose about the family name must not inflate the baseline";
        EXPECT_EQ(countBinds("virtual void BindTexture(u32 slot, RHI::ResourceHandle t) = 0;"), 1u)
            << "a declaration is a site too — it is what the conversion has to delete";
        EXPECT_EQ(countBinds("m_Stats.BindTextureCalls = 0;"), 0u) << "no '(' — not a call";
        EXPECT_EQ(countBinds("glBindTexture(GL_TEXTURE_2D, 0);"), 0u)
            << "a raw GL call is NOT a heap-convertible bind site — `BindTexture(` is a suffix of it";
        EXPECT_EQ(countBinds("RenderCommand::BindTexture(0, t); glBindTexture(GL_TEXTURE_2D, 0);"), 1u)
            << "one facade site, one GL call, on the same line";

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
    // MINTING A TEXTURE IDENTITY OBLIGES YOU TO RETIRE ITS VIEWS, and the rule
    // needs a guard because breaking it is silent.
    //
    // A backend type that calls `m_RHIHandle.Sync(ResourceKind::Texture, ...)`
    // can have heap descriptors minted against it — a bindless descriptor names
    // the underlying GL OBJECT, so it dangles the moment that object is deleted.
    // `~OpenGLTexture2D` and `~OpenGLTextureCubemap` paid that debt from the
    // start; `~OpenGLTexture2DArray` and `~OpenGLTexture3D` never did, and both
    // are bound as storage-image descriptors through the heap (the wind field,
    // the froxel fog volumes, the cloud noise volumes, the ocean FFT ping-pong).
    //
    // NOTHING CAUGHT IT FOR TWO PRs. It fails no test, renders nothing wrong,
    // and costs exactly one `GL_INVALID_OPERATION: Not a valid texture` in the
    // log at process shutdown — under `OLO_RHI_BINDLESS=1` only, which is not
    // the configuration CI runs. The GPU-side proof lives in
    // `BindlessHeapGpuTest` (which really destroys a Texture3D and a
    // Texture2DArray and checks residency drops), but that test can only cover
    // the types someone thought to write it for. This one covers the types
    // nobody has written yet, which is where the next instance will be.
    //
    // Scanned on the blanked text so the prose above — and the identical prose
    // in OpenGLUtilities.h — cannot satisfy the rule it describes.
    // -------------------------------------------------------------------------
    TEST(RHIBoundaryRatchet, EveryTextureTypeThatMintsAnIdentityRetiresItsViews)
    {
        const fs::path backend = RepoRoot() / "OloEngine" / "src" / "Platform" / "OpenGL";
        ASSERT_TRUE(fs::exists(backend)) << "Missing " << backend.string();

        // WHITESPACE-TOLERANT, because the plain substring was a needle that could
        // stop matching in silence. clang-format wraps a long call, and
        // `m_RHIHandle.Sync(` + newline + `RHI::ResourceKind::Texture2D, ...)` does
        // not contain the literal above — so the file would be skipped as a
        // non-minter and its missing retire would never be checked. A scanner that
        // under-reports passes for the wrong reason, which is the failure mode this
        // whole test file exists to prevent.
        static const std::regex kMint(R"(Sync\s*\(\s*RHI::ResourceKind::Texture)");

        // EITHER spelling satisfies the rule, and the second one is not a
        // loophole. `Utils::RetireTextureViews` is the form a texture TYPE
        // should use — one owned identity, one call, noexcept. `OpenGLFramebuffer`
        // is the other shape: it owns N attachment identities rather than one
        // `m_RHIHandle`, and retires them in a loop at two lifecycle points. It
        // is already correct, and a guard that demanded the helper would be
        // demanding a refactor rather than enforcing a property.
        //
        // The property IS "you retire what you minted". Which call you retire
        // with is style; not retiring at all is the bug.
        static constexpr std::string_view kRetireHelper = "RetireTextureViews(";
        static constexpr std::string_view kRetireDirect = "RetireResource(";

        u32 mintingFiles = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(backend, ec), end; it != end; it.increment(ec))
        {
            std::error_code entryEc;
            if (ec || !it->is_regular_file(entryEc) || entryEc || it->path().extension() != ".cpp")
            {
                continue;
            }

            const std::string blanked = BlankLiterals(ReadFile(it->path()));
            if (!std::regex_search(blanked, kMint))
            {
                continue;
            }
            ++mintingFiles;

            const bool retires = blanked.find(kRetireHelper) != std::string::npos ||
                                 blanked.find(kRetireDirect) != std::string::npos;
            EXPECT_TRUE(retires)
                << it->path().filename().string()
                << " mints a texture identity with m_RHIHandle.Sync(ResourceKind::Texture, ...) but "
                   "never retires it. Call Utils::RetireTextureViews() from the destructor — it pairs "
                   "the heap's RetireResource with the slot path's InvalidateTextureBinding and "
                   "cannot throw out of a destructor (Platform/OpenGL/OpenGLUtilities.h). Skipping it "
                   "leaves a resident bindless descriptor on a deleted GL object: undefined behaviour "
                   "when sampled, and a lone 'GL_INVALID_OPERATION: Not a valid texture' at shutdown "
                   "if you are lucky (issue #691 Phase 3).";
        }

        // The anchor that stops a broken scan from passing silently: the four
        // OpenGLTexture* units plus OpenGLFramebuffer (its attachments) mint one
        // today.
        EXPECT_GE(mintingFiles, 5u)
            << "Expected at least OpenGLTexture{,2DArray,3D,Cubemap}.cpp and OpenGLFramebuffer.cpp "
               "to mint a texture identity — a lower number means this scan stopped finding them, "
               "not that the engine grew simpler.";
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
