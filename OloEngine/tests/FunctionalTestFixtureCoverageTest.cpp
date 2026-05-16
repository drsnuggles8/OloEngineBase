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
//   declarations. Then for each, grep every .cpp under
//   `OloEngine/tests/Functional/` for a callsite (`.Enable*(`). At
//   least one must exist.
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
    } // namespace

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

        // Extract every `void Enable<X>(...);` method declared on the
        // class. The fixture's idiom is one declaration per line of the
        // form `void EnableX(...);` (the leading `void` and trailing
        // `;` keep the regex sharp).
        const std::regex enableDeclPat{ R"(void\s+(Enable\w+)\s*\([^)]*\)\s*;)" };
        std::set<std::string> enableMethods;
        for (auto it = std::sregex_iterator(headerSrc.begin(), headerSrc.end(), enableDeclPat);
             it != std::sregex_iterator(); ++it)
        {
            enableMethods.insert((*it)[1].str());
        }
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

            for (const auto& name : enableMethods)
            {
                // Match `EnableX(` (with any leading qualifier) as a
                // function call. The trailing `(` excludes the method's
                // own declaration / address-of references.
                const std::regex callPat{ R"(\b)" + name + R"(\s*\()" };
                if (std::regex_search(src, callPat))
                    callsiteMethods.insert(name);
            }
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
