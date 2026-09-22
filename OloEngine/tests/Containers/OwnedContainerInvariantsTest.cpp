// =============================================================================
// OwnedContainerInvariantsTest.cpp — issue #738, ADR 0012
//
// OLO_TEST_LAYER: plumbing
//
// Pins the parts of the container migration whose rule is ABSOLUTE, so the work
// cannot silently decay one new field at a time. These are invariants, not a
// ratchet: every number below is exact and none of them may move without a
// deliberate decision recorded in the ADR.
//
// Why this is narrow, and deliberately not a subsystem-wide ratchet
// -----------------------------------------------------------------
// The obvious guard — count `std::vector` / `std::string` members across the
// four migrated subsystems and let the number only fall — was measured before
// being written and rejected. Master added 269 `std::vector` members and 79
// `std::string` members across Renderer/Terrain/Dialogue/Scene/UI in the 30
// days to 2026-09-22 (392 commits). A guard that fires on most pull requests
// is a guard that gets worked around, and ADR 0012 keeps `std::` on the whole
// binding surface (entt, yaml-cpp, sol2, Mono, ImGui, Jolt, spdlog) — which no
// textual scan can distinguish from engine-owned data.
//
// So this file pins the three places where the rule admits no exception:
//
//   1. No ECS component holds a `std::vector`. All fourteen converted in step 6;
//      the binding-surface carve-out covers component `std::string` FIELDS, not
//      their sequence containers.
//   2. The four element types that unblocked those fourteen still hold `FString`.
//      Reverting any one re-creates the `free(): invalid pointer` abort that
//      `Containers/String.h:26-33` documents — and it aborts only under
//      libstdc++, so a local MSVC run will not show it.
//   3. `TIsTriviallyRelocatable` still defaults to `std::is_trivially_copyable_v`
//      and the container asserts are still hard. Restoring the always-true
//      default would silently un-guard every relocation in the engine.
//
// Anything broader belongs in the audit tool (`tools/container-audit/`) and in
// the judgement call the guide describes, not here.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
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

        std::string ReadEngineFile(const fs::path& relative, std::string& error)
        {
            const fs::path path = RepoRoot() / "OloEngine" / "src" / "OloEngine" / relative;
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                error = "could not open " + path.string();
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        // Strip // and /* */ so a mention in prose cannot fail the test — the
        // guides talk about these types by name constantly. Newlines survive so
        // reported line numbers stay usable.
        std::string StripComments(const std::string& text)
        {
            std::string out;
            out.reserve(text.size());
            enum class Mode
            {
                Code,
                Line,
                Block
            } mode = Mode::Code;

            for (sizet i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
                switch (mode)
                {
                    case Mode::Code:
                        if (c == '/' && next == '/')
                        {
                            mode = Mode::Line;
                            ++i;
                        }
                        else if (c == '/' && next == '*')
                        {
                            mode = Mode::Block;
                            ++i;
                        }
                        else
                        {
                            out += c;
                        }
                        break;
                    case Mode::Line:
                        if (c == '\n')
                        {
                            mode = Mode::Code;
                            out += c;
                        }
                        break;
                    case Mode::Block:
                        if (c == '*' && next == '/')
                        {
                            mode = Mode::Code;
                            ++i;
                        }
                        else if (c == '\n')
                        {
                            out += c;
                        }
                        break;
                }
            }
            return out;
        }

        // A MEMBER declaration, not any mention: indented, a type, a name, a
        // semicolon. A local inside a function body that feeds yaml-cpp or sol2
        // is not what this guards, and matching those would make the test a
        // ratchet by accident.
        std::vector<std::string> FindMemberDeclarations(const std::string& source, const std::string& typePattern)
        {
            const std::regex member(R"(^[ \t]+(?:const\s+|mutable\s+|inline\s+|static\s+)*)" + typePattern +
                                        R"(\s+m?_?\w+\s*(?:=[^;]*)?;)",
                                    std::regex::multiline);
            std::vector<std::string> hits;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), member), end = std::sregex_iterator();
                 it != end; ++it)
            {
                std::string line = it->str();
                // Trim for a readable failure message.
                const auto first = line.find_first_not_of(" \t\r\n");
                const auto last = line.find_last_not_of(" \t\r\n");
                hits.push_back(first == std::string::npos ? line : line.substr(first, last - first + 1));
            }
            return hits;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // 1. No component holds a std::vector.
    // -------------------------------------------------------------------------
    TEST(OwnedContainerInvariants, NoComponentFieldIsAStdVector)
    {
        std::string error;
        const std::string source = StripComments(ReadEngineFile("Scene/Components.h", error));
        ASSERT_TRUE(error.empty()) << error;
        ASSERT_FALSE(source.empty()) << "Components.h read as empty — the scan would pass vacuously.";

        const auto hits = FindMemberDeclarations(source, R"(std::vector\s*<[^;]*>)");

        std::ostringstream detail;
        for (const auto& hit : hits)
        {
            detail << "\n    " << hit;
        }
        EXPECT_TRUE(hits.empty())
            << "Components.h gained " << hits.size() << " std::vector member(s):" << detail.str()
            << "\n\n  Every component sequence container is a TArray (issue #738 step 6, ADR 0012)."
            << "\n  The binding-surface carve-out covers component std::string FIELDS, which five"
            << "\n  generated consumers marshal — it does not cover their vectors."
            << "\n  Use TArray<T>, and give the element type a TIsTriviallyRelocatable"
            << "\n  specialisation derived member by member if it is not trivially copyable.";
    }

    // -------------------------------------------------------------------------
    // 2. The four element types that unblocked the fourteen vectors.
    // -------------------------------------------------------------------------
    TEST(OwnedContainerInvariants, TheFourUnblockingElementTypesStillHoldFString)
    {
        struct Expectation
        {
            const char* File;
            const char* Field;
        };
        // ADR 0012 decision 9 converts outward from these four; each one is what
        // made its enclosing std::vector unconvertible.
        const Expectation expectations[] = {
            { "Renderer/Material.h", "m_Name" },
            { "Terrain/Foliage/FoliageLayer.h", "Name" },
            { "Dialogue/DialogueTypes.h", "Text" },
            { "Scene/Components.h", "m_Label" },
        };

        for (const auto& expectation : expectations)
        {
            SCOPED_TRACE(std::string(expectation.File) + " :: " + expectation.Field);
            std::string error;
            const std::string source = StripComments(ReadEngineFile(expectation.File, error));
            ASSERT_TRUE(error.empty()) << error;
            ASSERT_FALSE(source.empty()) << "file read as empty";

            const std::regex asFString(R"(\bFString\s+)" + std::string(expectation.Field) + R"(\s*(?:=[^;]*)?;)");
            const std::regex asStdString(R"(\bstd::string\s+)" + std::string(expectation.Field) +
                                         R"(\s*(?:=[^;]*)?;)");

            EXPECT_FALSE(std::regex_search(source, asStdString))
                << expectation.Field << " reverted to std::string.\n"
                << "  TArray relocates bitwise; libstdc++'s std::string keeps an SSO self-pointer\n"
                << "  that does not survive that. The abort is documented at Containers/String.h:26-33\n"
                << "  and reproduces ONLY under libstdc++ — a green MSVC run here means nothing.";
            EXPECT_TRUE(std::regex_search(source, asFString))
                << expectation.Field << " is no longer declared as FString.\n"
                << "  If it was renamed or moved, update this expectation in the same commit;\n"
                << "  if it was converted to something else, say why in the PR.";
        }
    }

    // -------------------------------------------------------------------------
    // 3. The trait still fails closed.
    // -------------------------------------------------------------------------
    TEST(OwnedContainerInvariants, RelocationTraitStillFailsClosed)
    {
        // A type with a non-trivial member must NOT be reported relocatable
        // unless someone opted it in deliberately. This is the whole guard:
        // #738 step 2 replaced a default of `true` with this.
        struct HasANonTrivialMember
        {
            std::string Owned;
        };
        static_assert(!TIsTriviallyRelocatable_V<HasANonTrivialMember>,
                      "TIsTriviallyRelocatable no longer fails closed — a type with a non-trivially-copyable "
                      "member is being reported as relocatable. Every TArray in the engine is un-guarded "
                      "until this is restored (issue #738 step 2, ADR 0012 decision 2).");

        static_assert(TIsTriviallyRelocatable_V<i32>, "a scalar must still be relocatable");
        static_assert(TIsTriviallyRelocatable_V<FString>,
                      "FString carries an explicit opt-in — it owns a heap buffer with no self-pointer");
        static_assert(TIsTriviallyRelocatable_V<TArray<i32>>, "TArray carries an explicit opt-in");

        // The warn-only form was what made step 2 necessary: it is a
        // [[deprecated]]-backed warning, so a violation compiled and shipped.
        std::string error;
        const std::string containers[] = { "Containers/Array.h", "Containers/CompactSet.h", "Containers/Deque.h",
                                           "Containers/SparseArray.h" };
        for (const auto& relative : containers)
        {
            SCOPED_TRACE(relative);
            const std::string source = StripComments(ReadEngineFile(relative, error));
            ASSERT_TRUE(error.empty()) << error;
            ASSERT_FALSE(source.empty()) << "file read as empty";

            EXPECT_EQ(source.find("OLO_STATIC_ASSERT_WARN"), std::string::npos)
                << relative << " uses OLO_STATIC_ASSERT_WARN again.\n"
                << "  That macro is a warning, not a gate — it is why a relocation violation could\n"
                << "  reach master before #738. The assert must be a hard static_assert.";
            EXPECT_NE(source.find("static_assert("), std::string::npos)
                << relative << " has no static_assert at all; the relocation gate is gone.";
        }
    }
} // namespace OloEngine::Tests
