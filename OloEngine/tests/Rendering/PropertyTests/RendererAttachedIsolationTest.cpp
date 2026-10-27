// OLO_TEST_LAYER: meta
//
// RendererAttachedTest's isolation is a LIST — the settings structs it restores
// and the simulation systems it resets between tests — and a list goes stale
// the day a new one is added. That is how the leaks kept coming back: PCSS
// softened every golden shadow after one test, fog hazed them (#1533), rain
// left falling put particles into a paused foliage frame (#1502), each found by
// a failure somewhere downstream and fixed by adding one more line.
//
// These read the engine's own declarations and the fixture's source, so adding
// a Renderer3D::Get*Settings() accessor or a system with a static Reset()
// fails here, at the declaration, instead of in whichever test happens to run
// after the one that uses it.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        fs::path RepoRoot()
        {
            return fs::path(OLO_TEST_EDITOR_ROOT).parent_path();
        }

        std::string ReadText(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        }

        sizet CountOf(const std::string& haystack, const std::string& needle)
        {
            sizet count = 0;
            for (sizet at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size()))
                ++count;
            return count;
        }

        const fs::path kFixture = "OloEngine/tests/Rendering/PropertyTests/RendererAttachedTest.cpp";
    } // namespace

    TEST(RendererAttachedIsolation, EveryProcessWideSettingsStructIsRestored)
    {
        const std::string renderer = ReadText(RepoRoot() / "OloEngine/src/OloEngine/Renderer/Renderer3D.h");
        const std::string fixture = ReadText(RepoRoot() / kFixture);
        ASSERT_FALSE(renderer.empty());
        ASSERT_FALSE(fixture.empty());

        const std::regex accessor(R"(static\s+[\w:]+&\s+(Get\w+Settings)\(\))");
        std::set<std::string> accessors;
        for (std::sregex_iterator it(renderer.begin(), renderer.end(), accessor), end; it != end; ++it)
            accessors.insert((*it)[1].str());
        // Twelve on the day this was written. Fewer means the pattern stopped
        // matching, and the loop below would pass having checked nothing.
        ASSERT_GE(accessors.size(), 12u) << "the accessor scan of Renderer3D.h found too few";

        for (const std::string& name : accessors)
        {
            // Once to snapshot in SetUp, once to restore in TearDown.
            EXPECT_GE(CountOf(fixture, "Renderer3D::" + name + "()"), 2u)
                << "Renderer3D::" << name << "() is process-wide state that RendererAttachedTest does not "
                << "snapshot and restore. Add a field to ProcessWideSettings and both lines in "
                << kFixture.generic_string() << ", or a test that changes it leaks into every test after it.";
        }
    }

    TEST(RendererAttachedIsolation, EveryProcessWideSimulationSystemIsReset)
    {
        const std::string fixture = ReadText(RepoRoot() / kFixture);
        ASSERT_FALSE(fixture.empty());

        // A class with a static Reset() keeps world state in process-global
        // storage: particles, a field, an intensity. Its name is the nearest
        // class or struct declared before the Reset.
        const std::regex reset(R"(\bstatic\s+void\s+Reset\(\)\s*;)");
        const std::regex type(R"(\b(?:class|struct)\s+(?:OLO_API\s+)?(\w+)\s*(?:final\s*)?(?::[^{;]*)?\{)");
        std::set<std::string> systems;
        for (const auto& entry : fs::recursive_directory_iterator(RepoRoot() / "OloEngine/src/OloEngine"))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".h")
                continue;
            const std::string text = ReadText(entry.path());
            for (std::sregex_iterator it(text.begin(), text.end(), reset), end; it != end; ++it)
            {
                const std::string before = text.substr(0, static_cast<sizet>(it->position()));
                std::string owner;
                for (std::sregex_iterator t(before.begin(), before.end(), type), tend; t != tend; ++t)
                    owner = (*t)[1].str();
                if (!owner.empty())
                    systems.insert(owner);
            }
        }
        // Nine on the day this was written; see above.
        ASSERT_GE(systems.size(), 9u) << "the static Reset() scan found too few systems";

        for (const std::string& system : systems)
        {
            EXPECT_EQ(CountOf(fixture, system + "::Reset();"), 1u)
                << system << " has a static Reset() — process-global state that outlives a scene — and "
                << "RendererAttachedTest::SetUp does not call it, so a test that leaves it running leaks into "
                << "the next. Call it there (" << kFixture.generic_string() << ").";
        }
    }
} // namespace OloEngine::Tests
