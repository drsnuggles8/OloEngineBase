// =============================================================================
// OloHeaderToolOutputsTest.cpp
//
// Meta-test: the `OHT_GENERATED_FILES` list in tools/OloHeaderTool/CMakeLists.txt
// must name exactly the files tools/OloHeaderTool/main.cpp writes.
//
// Why this needs a guard (issue #758)
// -----------------------------------
//   `OHT_GENERATED_FILES` in tools/OloHeaderTool/CMakeLists.txt is the declared
//   inventory of what this codegen produces — the list a reader consults to answer
//   "what does OloHeaderTool write?". It is hand-maintained in CMake while the
//   actual writes live in main.cpp, and the two drift silently: the #451 and #525
//   slices repeatedly added Scene/Generated artefacts, and a slice that forgets
//   this list leaves the inventory quietly wrong with nothing to say so.
//
//   The failure this prevents is the one the whole issue is about: a generated
//   file that quietly stops being tracked surfaces much later as a coverage-test
//   *parse-count assertion* in a completely unrelated suite, not as a build
//   error naming the file.
//
// How it works
// ------------
//   Both sides are parsed as TEXT, deliberately — the same technique
//   ComponentTupleCoverageTest and ComponentSerializerCoverageTest use, and for
//   the same reason: neither side is reachable as a C++ symbol from the test
//   binary. main.cpp is scanned for `<something>OutDir / "name"` expressions
//   (every write goes through one, whether via WriteIfChanged, ReportWrite, or a
//   local `auto path = ...`), and CMakeLists.txt for the `OHT_GENERATED_FILES`
//   block. The output-directory variable names are the join key.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

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

        fs::path ToolDir()
        {
            return RepoRoot() / "tools" / "OloHeaderTool";
        }

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }

        // main.cpp's local path variables, mapped to the CMake cache variables the
        // CMakeLists spells its list with. These are the join key between the two
        // files; a renamed variable on either side fails here loudly rather than
        // silently dropping that directory's artefacts from the comparison.
        const std::map<std::string, std::string>& OutDirVarToCMakeVar()
        {
            static const std::map<std::string, std::string> map = {
                { "cppOutDir", "OHT_CPP_OUT" },
                { "csOutDir", "OHT_CS_OUT" },
                { "sceneOutDir", "OHT_SCENE_OUT" },
                { "saveGameOutDir", "OHT_SAVEGAME_OUT" },
                { "mcpOutDir", "OHT_MCP_OUT" },
                { "visualScriptOutDir", "OHT_VS_OUT" },
            };
            return map;
        }

        // Every artefact main.cpp writes, as "<CMAKE_VAR>/<relative/path>".
        //
        // Matches `<ident>OutDir / "a"` with an optional second `/ "b"` segment
        // (only Components.Generated.cs currently uses one). The depfile and the
        // stamp are deliberately NOT matched: they are build-tree bookkeeping
        // written to explicitly-passed paths, not source-tree artefacts.
        std::set<std::string> ArtefactsWrittenByTool()
        {
            const std::string source = ReadFile(ToolDir() / "main.cpp");
            // Custom delimiter: the pattern itself contains `)"`, which would
            // terminate a plain R"( ... )" literal early.
            static const std::regex writeRe(
                R"RX((\w+OutDir)\s*/\s*"([^"]+)"(?:\s*/\s*"([^"]+)")?)RX");

            std::set<std::string> result;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), writeRe);
                 it != std::sregex_iterator(); ++it)
            {
                const auto& match = *it;
                const auto varIt = OutDirVarToCMakeVar().find(match[1].str());
                if (varIt == OutDirVarToCMakeVar().end())
                    continue;

                std::string relative = match[2].str();
                if (match[3].matched)
                    relative += "/" + match[3].str();
                result.insert(varIt->second + "/" + relative);
            }
            return result;
        }

        // Every artefact the CMake OHT_GENERATED_FILES list declares, in the same
        // "<CMAKE_VAR>/<relative/path>" spelling.
        std::set<std::string> ArtefactsDeclaredByCMake()
        {
            const std::string cmake = ReadFile(ToolDir() / "CMakeLists.txt");

            const auto listStart = cmake.find("set(OHT_GENERATED_FILES");
            if (listStart == std::string::npos)
                return {};
            const auto listEnd = cmake.find("\n)", listStart);
            const std::string block = cmake.substr(listStart, listEnd - listStart);

            static const std::regex entryRe(R"(\$\{(OHT_\w+)\}/([^"]+))");
            std::set<std::string> result;
            for (auto it = std::sregex_iterator(block.begin(), block.end(), entryRe);
                 it != std::sregex_iterator(); ++it)
            {
                result.insert((*it)[1].str() + "/" + (*it)[2].str());
            }
            return result;
        }

        std::string Join(const std::set<std::string>& items)
        {
            std::ostringstream out;
            for (const auto& item : items)
                out << "\n    " << item;
            return out.str();
        }
    } // namespace

    // Both parsers must actually find something. Without this, a refactor that
    // renames a variable or restructures the CMake list turns the two comparisons
    // below into vacuous empty-set-equals-empty-set successes — the exact way a
    // text-parsing meta-test rots into a no-op.
    TEST(OloHeaderToolOutputs, BothSidesParseToANonEmptySet)
    {
        const auto written = ArtefactsWrittenByTool();
        const auto declared = ArtefactsDeclaredByCMake();

        EXPECT_GE(written.size(), 15u)
            << "Parsed only " << written.size() << " artefact writes out of main.cpp. "
                                                   "The `<var>OutDir / \"name\"` write shape or the OutDirVarToCMakeVar() "
                                                   "key set has changed — fix this test's parser before trusting it.";
        EXPECT_GE(declared.size(), 15u)
            << "Parsed only " << declared.size() << " entries from the OHT_GENERATED_FILES "
                                                    "block in tools/OloHeaderTool/CMakeLists.txt. Has the list been renamed or "
                                                    "restructured?";
    }

    // The load-bearing assertion, in both directions.
    TEST(OloHeaderToolOutputs, CMakeListMatchesTheToolsActualWrites)
    {
        const auto written = ArtefactsWrittenByTool();
        const auto declared = ArtefactsDeclaredByCMake();

        std::set<std::string> missingFromCMake;
        for (const auto& artefact : written)
        {
            if (!declared.contains(artefact))
                missingFromCMake.insert(artefact);
        }
        EXPECT_TRUE(missingFromCMake.empty())
            << "OloHeaderTool writes files the build graph does not know about — add them to "
               "OHT_GENERATED_FILES in tools/OloHeaderTool/CMakeLists.txt:"
            << Join(missingFromCMake);

        std::set<std::string> missingFromTool;
        for (const auto& artefact : declared)
        {
            if (!written.contains(artefact))
                missingFromTool.insert(artefact);
        }
        EXPECT_TRUE(missingFromTool.empty())
            << "OHT_GENERATED_FILES names files OloHeaderTool no longer writes — a stale entry "
               "makes the build graph claim an artefact that will never appear:"
            << Join(missingFromTool);
    }

    // The declared artefacts must exist on disk. They are tracked in git (see
    // CLAUDE.md's OloHeaderTool section), so a missing one means either the
    // generator never ran in this tree or a path in the list is wrong — both of
    // which otherwise surface as an unrelated coverage-test parse failure.
    TEST(OloHeaderToolOutputs, EveryDeclaredArtefactExistsOnDisk)
    {
        const std::map<std::string, fs::path> dirs = {
            { "OHT_CPP_OUT", RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scripting" / "C#" / "Generated" },
            { "OHT_CS_OUT", RepoRoot() / "OloEngine-ScriptCore" / "src" / "OloEngine" },
            { "OHT_SCENE_OUT", RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scene" / "Generated" },
            { "OHT_SAVEGAME_OUT", RepoRoot() / "OloEngine" / "src" / "OloEngine" / "SaveGame" / "Generated" },
            { "OHT_MCP_OUT", RepoRoot() / "OloEditor" / "src" / "MCP" / "Generated" },
            { "OHT_VS_OUT", RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scripting" / "VisualScript" / "Generated" },
        };

        for (const auto& artefact : ArtefactsDeclaredByCMake())
        {
            const auto slash = artefact.find('/');
            ASSERT_NE(slash, std::string::npos) << artefact;
            const auto dirIt = dirs.find(artefact.substr(0, slash));
            ASSERT_NE(dirIt, dirs.end())
                << "Unknown output directory variable in OHT_GENERATED_FILES: " << artefact
                << " — add it to this test's `dirs` map (and check it is a real cache variable "
                   "in tools/OloHeaderTool/CMakeLists.txt).";

            const fs::path path = dirIt->second / artefact.substr(slash + 1);
            EXPECT_TRUE(fs::exists(path))
                << "Declared generated artefact is missing from the source tree: " << path
                << "\nRun `cmake --build <dir> --target GenerateBindings` and re-stage.";
        }
    }
} // namespace OloEngine::Tests
