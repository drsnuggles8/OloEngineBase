// =============================================================================
// FunctionalTestFixtureCoverageTest.cpp
//
// Meta-test: every `Enable*` helper declared on `OloEngine::Functional::
// FunctionalTest` must be called by at least one Functional test under
// `OloEngine/tests/Functional/`. If a helper is added to the fixture
// but no test exercises it, that helper rots: a future engine change
// breaks the helper's setup path and nobody notices because the
// callsite set is empty.
//
// Catches the silent "we shipped a fixture method but never wrote the
// test it was meant for" class of regression — the fixture's API
// surface should be load-bearing.
//
// Detection
// ---------
//   Parse `FunctionalTest.h` for the public `Enable*` method
//   declarations, including const helpers. Then search test translation units
//   under `OloEngine/tests/Functional/` for calls. Fixture definitions,
//   declarations, comments and literals do not count as coverage.
//
// The check is static text search; runtime introspection isn't needed
// (and isn't available in C++ without RTTI tricks).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
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

        // Repo root: OLO_TEST_EDITOR_ROOT is the OloEditor/ dir; its
        // parent is the repo root.
        fs::path RepoRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        }

        // Preserve only code tokens. The raw-string alternative precedes the
        // ordinary string alternative so embedded C++/Lua fixtures cannot
        // manufacture helper calls (or TEST registrations).
        std::string CodeOnly(const std::string& source)
        {
            // Keep preprocessing-number tokens before matching character
            // literals: an apostrophe in 1'000 is a digit separator. C++ also
            // permits multi-character literals, which can contain decoy calls.
            static const std::regex tokens{
                R"olo((\b[0-9][A-Za-z0-9_'.]*)|//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\2"|"(?:\\[\s\S]|[^"\\])*"|'(?:\\[\s\S]|[^'\\])*')olo"
            };
            std::string code;
            sizet previousEnd = 0;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), tokens);
                 it != std::sregex_iterator(); ++it)
            {
                const auto start = static_cast<sizet>(it->position());
                code.append(source, previousEnd, start - previousEnd);
                code += (*it)[1].matched ? it->str() : " ";
                previousEnd = start + static_cast<sizet>(it->length());
            }
            code.append(source, previousEnd, source.size() - previousEnd);
            return code;
        }

        std::set<std::string> EnableDeclarations(const std::string& header)
        {
            const std::string code = CodeOnly(header);
            static const std::regex declaration{
                R"(\bvoid\s+(Enable\w+)\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?;)"
            };
            std::set<std::string> methods;
            for (auto it = std::sregex_iterator(code.begin(), code.end(), declaration);
                 it != std::sregex_iterator(); ++it)
            {
                methods.insert((*it)[1].str());
            }
            return methods;
        }

        std::set<std::string> EnableCallsInTests(const std::string& source,
                                                 const std::set<std::string>& methods)
        {
            std::string code = CodeOnly(source);
            static const std::regex testRegistration{ R"(\b(?:TEST|TEST_F|TEST_P|TYPED_TEST)\s*\()" };
            if (!std::regex_search(code, testRegistration))
                return {};

            // Even a test TU can define fixture methods. A signature is not a
            // call, so erase it before searching the remaining code.
            static const std::regex helperSignature{
                R"(\bvoid\s+(?:\w+::)*Enable\w+\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?)"
            };
            code = std::regex_replace(code, helperSignature, " ");

            std::set<std::string> calls;
            for (const auto& name : methods)
            {
                if (std::regex_search(code, std::regex{ R"(\b)" + name + R"(\s*\()" }))
                    calls.insert(name);
            }
            return calls;
        }
    } // namespace

    TEST(FunctionalTestFixtureCoverage, ConstHelpersAreIncludedInTheContract)
    {
        EXPECT_EQ(EnableDeclarations(R"(
            void EnablePhysics3D();
            void EnableAnimation() const;
            // void EnableCommentOnly();
        )"),
                  (std::set<std::string>{ "EnableAnimation", "EnablePhysics3D" }));
    }

    TEST(FunctionalTestFixtureCoverage, FixtureDefinitionsAndDeclarationsAreNotCoverage)
    {
        const std::set<std::string> methods{ "EnableAnimation", "EnablePhysics3D" };
        EXPECT_TRUE(EnableCallsInTests(R"(
            void FunctionalTest::EnablePhysics3D() {}
            void EnableAnimation() const;
            TEST /* synthetic registration */ (OtherContract, Runs) {}
        )",
                                       methods)
                        .empty());
        EXPECT_TRUE(EnableCallsInTests("void Helper() { EnablePhysics3D(); }", methods).empty());
    }

    TEST(FunctionalTestFixtureCoverage, OnlyCodeCallsInTestsProvideCoverage)
    {
        const std::set<std::string> methods{ "EnableAnimation", "EnablePhysics3D" };
        EXPECT_EQ(EnableCallsInTests(R"source(
            // EnablePhysics3D();
            /* EnablePhysics3D(); */
            const char* ordinary = "EnablePhysics3D();";
            const char* raw = R"decoy("EnablePhysics3D();")decoy";
            TEST_F /* synthetic registration */ (SceneContract, Runs) { this->EnableAnimation(); }
        )source",
                                     methods),
                  (std::set<std::string>{ "EnableAnimation" }));
        EXPECT_TRUE(EnableCallsInTests(R"source(
            const char* decoy = R"text(TEST_F(Fake, Test) { EnablePhysics3D(); })text";
        )source",
                                       methods)
                        .empty());
    }

    TEST(FunctionalTestFixtureCoverage, DigitSeparatorsPreserveCallsAndCharacterLiteralsDoNotCount)
    {
        const std::set<std::string> methods{ "EnableAnimation", "EnableLua", "EnablePhysics3D" };
        EXPECT_EQ(EnableCallsInTests(R"source(
            TEST_F /* synthetic registration */ (SceneContract, Runs)
            {
                auto a = 1'000;
                EnableLua();
                auto b = 2'000;
                auto character = 'x';
                auto multiCharacter = 'EnablePhysics3D()';
                this->EnableAnimation();
            }
        )source",
                                     methods),
                  (std::set<std::string>{ "EnableAnimation", "EnableLua" }));
    }

    TEST(FunctionalTestFixtureCoverage, EveryEnableHelperIsCalledBySomeFunctionalTest)
    {
        const fs::path fixtureHeader = RepoRoot() /
                                       "OloEngine" / "tests" / "Functional" / "FunctionalTest.h";
        ASSERT_TRUE(fs::exists(fixtureHeader))
            << "FunctionalTest.h not found at " << fixtureHeader.string();

        // Slurp the header.
        std::string headerSrc;
        {
            std::ifstream f(fixtureHeader, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            headerSrc = buf.str();
        }

        const auto enableMethods = EnableDeclarations(headerSrc);
        ASSERT_FALSE(enableMethods.empty())
            << "Regex did not detect any `Enable*` method in FunctionalTest.h — "
               "the fixture header's signature style changed and this test needs updating.";

        // Walk every .cpp under OloEngine/tests/Functional/, collecting
        // which Enable* methods are called somewhere.
        const fs::path functionalRoot = RepoRoot() / "OloEngine" / "tests" / "Functional";
        std::set<std::string> callsiteMethods;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(functionalRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".cpp")
                continue;

            std::ifstream f(entry.path(), std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            const std::string src = buf.str();

            const auto calls = EnableCallsInTests(src, enableMethods);
            callsiteMethods.insert(calls.begin(), calls.end());
        }

        // Difference: declared - called.
        std::vector<std::string> uncovered;
        for (const auto& name : enableMethods)
        {
            if (!callsiteMethods.contains(name))
                uncovered.push_back(name);
        }

        if (!uncovered.empty())
        {
            std::ostringstream oss;
            oss << uncovered.size() << " FunctionalTest::Enable* method(s) have no test callsite:\n";
            for (const auto& name : uncovered)
                oss << "  - " << name << "()\n";
            oss << "\nAdd a Functional test that calls each of the above, or remove the "
                << "unused fixture helper.\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
