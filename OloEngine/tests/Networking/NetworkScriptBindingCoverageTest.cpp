#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// The C# networking bindings are three hand-maintained lists that must agree:
//
//   1. `Network_*` internal-call DEFINITIONS in ScriptGlue.cpp,
//   2. their `OLO_ADD_INTERNAL_CALL(Network_*)` REGISTRATIONS in the same file,
//   3. the `extern` DECLARATIONS in OloEngine-ScriptCore's InternalCalls.cs.
//
// A mismatch between any two compiles and links perfectly. It fails at RUNTIME,
// on the first call, with Mono's "internal call not found" — i.e. only when
// someone actually plays a networked game with a C# script attached. That is the
// same shape of quiet failure the whole of issue #636 was about, so it gets a
// test instead of a comment.
//
// This parses the sources as text, matching the existing coverage-test pattern
// (ComponentSerializerCoverageTest et al). Scoped to the `Network_*` surface
// deliberately: it guards the bindings this change introduced without coupling
// the networking suite to unrelated drift elsewhere in ScriptGlue.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace fs = std::filesystem;

namespace
{
    [[nodiscard]] fs::path RepoRoot()
    {
        return fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
    }

    [[nodiscard]] std::string ReadFile(const fs::path& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    [[nodiscard]] std::set<std::string> MatchAll(const std::string& text, const std::regex& pattern)
    {
        std::set<std::string> found;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it)
        {
            found.insert((*it)[1].str());
        }
        return found;
    }

    [[nodiscard]] std::string Join(const std::set<std::string>& names)
    {
        std::string out;
        for (const auto& n : names)
        {
            if (!out.empty())
            {
                out += ", ";
            }
            out += n;
        }
        return out.empty() ? "(none)" : out;
    }

    // Names present in `a` but not in `b`.
    [[nodiscard]] std::set<std::string> Missing(const std::set<std::string>& a, const std::set<std::string>& b)
    {
        std::set<std::string> diff;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::inserter(diff, diff.begin()));
        return diff;
    }
} // namespace

class NetworkScriptBindingCoverageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const fs::path glue = RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scripting" / "C#" / "ScriptGlue.cpp";
        const fs::path internalCalls =
            RepoRoot() / "OloEngine-ScriptCore" / "src" / "OloEngine" / "InternalCalls.cs";

        m_GlueText = ReadFile(glue);
        m_InternalCallsText = ReadFile(internalCalls);

        ASSERT_FALSE(m_GlueText.empty()) << "could not read " << glue.string();
        ASSERT_FALSE(m_InternalCallsText.empty()) << "could not read " << internalCalls.string();

        // `static <type> Network_Foo(` — the native definition.
        m_Defined = MatchAll(m_GlueText, std::regex{ R"(static\s+[\w:*&<>\s]+?\b(Network_\w+)\s*\()" });
        // `OLO_ADD_INTERNAL_CALL(Network_Foo);` — the Mono registration.
        m_Registered = MatchAll(m_GlueText, std::regex{ R"(OLO_ADD_INTERNAL_CALL\(\s*(Network_\w+)\s*\))" });
        // `internal static extern <type> Network_Foo(` — the managed declaration.
        m_Declared = MatchAll(m_InternalCallsText,
                              std::regex{ R"(extern\s+[\w\[\]<>,\s]+?\b(Network_\w+)\s*\()" });
    }

    std::string m_GlueText;
    std::string m_InternalCallsText;
    std::set<std::string> m_Defined;
    std::set<std::string> m_Registered;
    std::set<std::string> m_Declared;
};

TEST_F(NetworkScriptBindingCoverageTest, ParsingFoundTheBindings)
{
    // Guard against the regexes silently matching nothing — a coverage test that
    // parses to an empty set passes every other assertion vacuously.
    EXPECT_GE(m_Defined.size(), 10u) << "parsed only: " << Join(m_Defined);
    EXPECT_GE(m_Registered.size(), 10u) << "parsed only: " << Join(m_Registered);
    EXPECT_GE(m_Declared.size(), 10u) << "parsed only: " << Join(m_Declared);
}

TEST_F(NetworkScriptBindingCoverageTest, EveryNativeBindingIsRegisteredWithMono)
{
    const auto unregistered = Missing(m_Defined, m_Registered);
    EXPECT_TRUE(unregistered.empty())
        << "Network_* functions defined in ScriptGlue.cpp but never passed to OLO_ADD_INTERNAL_CALL: "
        << Join(unregistered)
        << "\nC# calling one of these throws at runtime, on first use, with 'internal call not found'.";
}

TEST_F(NetworkScriptBindingCoverageTest, EveryManagedDeclarationHasANativeBinding)
{
    const auto unbound = Missing(m_Declared, m_Registered);
    EXPECT_TRUE(unbound.empty())
        << "Network_* externs declared in InternalCalls.cs with no matching OLO_ADD_INTERNAL_CALL: "
        << Join(unbound)
        << "\nThese link fine and fail on first call.";
}

TEST_F(NetworkScriptBindingCoverageTest, EveryRegisteredBindingIsReachableFromCSharp)
{
    // The inverse direction: a native binding nothing declares is dead weight, and
    // usually means a managed declaration was forgotten rather than that the
    // binding is genuinely unused.
    const auto unreachable = Missing(m_Registered, m_Declared);
    EXPECT_TRUE(unreachable.empty())
        << "Network_* internal calls registered natively but not declared in InternalCalls.cs: "
        << Join(unreachable);
}
