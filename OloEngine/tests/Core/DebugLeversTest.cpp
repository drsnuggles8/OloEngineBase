// =============================================================================
// DebugLeversTest.cpp
//
// Guards Core/DebugLevers.{h,inl,cpp} — the registry the engine's ~21
// debug/diagnostic levers live in.
//
// Two jobs, and the second is the one that matters over time:
//
//   1. The registry BEHAVES: setters round-trip per kind, a setter that runs
//      before anything has read a lever survives the lazy environment seed, and
//      Snapshot() describes every lever with a usable name and help string.
//   2. The registry stays THE list. A grep test fails if a new lever is added
//      the old way — a bare `Env::IsTruthy("OLO_...")` somewhere in the engine
//      — because that is exactly how the previous 21 accumulated. Without this
//      the registry is a snapshot of one afternoon rather than an invariant.
//
// What is deliberately NOT tested here: environment seeding. Values seed once
// per process and this binary's main() has already run, so a test cannot set a
// variable and observe it. That path is covered by driving the levers from a
// separate process; see the commit that introduced this file.
// =============================================================================

// OLO_TEST_LAYER: unit

#include "OloEnginePCH.h"

#include "OloEngine/Core/DebugLevers.h"

#include <gtest/gtest.h>

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

        fs::path RepoRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        }

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }

        // Files allowed to read an OLO_* variable through Env:: directly.
        //
        // Everything here belongs to a different category than a debug lever: it
        // describes HOW THE PROCESS WAS LAUNCHED and who, if anyone, is watching
        // it — not a switch you flip to investigate a bug. That family already
        // has its own front door in Core/Interactivity.h (`SetNonInteractive`),
        // and modals consult `IsNonInteractive()` rather than a lever. Listing
        // them in the registry would give one concept two answers.
        //
        // Keep this list SHORT. If you are adding to it to silence this test,
        // the lever almost certainly belongs in DebugLevers.inl instead.
        const std::set<std::string> kNonLeverFiles = {
            "DebugLevers.cpp",   // the registry itself
            "Interactivity.cpp", // OLO_NON_INTERACTIVE — the concept above
            // OLO_EDITOR_UNSAVED_PROMPT pre-answers the unsaved-changes modal
            // for an automated launch. Same family as
            // OLO_EDITOR_AUTOSAVE_RECOVERY in EditorLayer.cpp (OloEditor, so
            // outside this scan) — a launch-time answer, not a debug lever.
            "WindowsPlatformUtils.cpp",
        };
    } // namespace

    // -------------------------------------------------------------------
    // 1. Behaviour
    // -------------------------------------------------------------------

    TEST(DebugLevers, SnapshotDescribesEveryLever)
    {
        const std::vector<Levers::LeverInfo> levers = Levers::Snapshot();
        ASSERT_FALSE(levers.empty()) << "the registry is empty — DebugLevers.inl did not expand";

        std::set<std::string_view> names;
        for (const Levers::LeverInfo& lever : levers)
        {
            EXPECT_FALSE(lever.Name.empty());
            EXPECT_TRUE(lever.Name.starts_with("OLO_"))
                << "'" << lever.Name << "' does not look like one of this engine's variables";
            // The help text is what the startup log and the MCP tool show, so
            // an empty one makes the lever undiscoverable — the exact problem
            // the registry exists to fix.
            EXPECT_GT(lever.Help.size(), 20u) << "'" << lever.Name << "' has no usable help text";
            EXPECT_FALSE(lever.Value.empty());
            EXPECT_TRUE(names.insert(lever.Name).second) << "duplicate lever '" << lever.Name << "'";
        }
    }

    TEST(DebugLevers, TogglesRoundTrip)
    {
        const bool original = Levers::PoisonTransients();

        Levers::SetPoisonTransients(true);
        EXPECT_TRUE(Levers::PoisonTransients());
        Levers::SetPoisonTransients(false);
        EXPECT_FALSE(Levers::PoisonTransients());

        Levers::SetPoisonTransients(original);
        EXPECT_EQ(Levers::PoisonTransients(), original);
    }

    TEST(DebugLevers, ExactMatchLeversRoundTripThroughTheSameSetter)
    {
        // Exact vs lenient is about how the ENVIRONMENT is parsed; from code
        // both are plain booleans.
        const bool original = Levers::TerrainCpuLod();
        Levers::SetTerrainCpuLod(!original);
        EXPECT_EQ(Levers::TerrainCpuLod(), !original);
        Levers::SetTerrainCpuLod(original);
        EXPECT_EQ(Levers::TerrainCpuLod(), original);
    }

    TEST(DebugLevers, TristateDistinguishesUnsetFromOff)
    {
        const Levers::Tristate original = Levers::TaskGraphDynamicPrioritization();

        Levers::SetTaskGraphDynamicPrioritization(Levers::Tristate::Off);
        EXPECT_EQ(Levers::TaskGraphDynamicPrioritization(), Levers::Tristate::Off);
        Levers::SetTaskGraphDynamicPrioritization(Levers::Tristate::On);
        EXPECT_EQ(Levers::TaskGraphDynamicPrioritization(), Levers::Tristate::On);
        Levers::SetTaskGraphDynamicPrioritization(Levers::Tristate::Unset);
        EXPECT_EQ(Levers::TaskGraphDynamicPrioritization(), Levers::Tristate::Unset)
            << "Unset must survive as its own state — it means 'leave the hardware-derived default alone', "
               "which is not the same as Off";

        Levers::SetTaskGraphDynamicPrioritization(original);
    }

    TEST(DebugLevers, NumericLeversDistinguishUnsetFromZero)
    {
        const std::optional<i64> originalInt = Levers::ParallelForYieldMs();

        // The whole reason these return optional: a lever set to 0 and a lever
        // nobody set must not look the same, or "unparseable" silently becomes
        // "zero" — which is what std::atoi used to do at these call sites.
        Levers::SetParallelForYieldMs(0);
        ASSERT_TRUE(Levers::ParallelForYieldMs().has_value());
        EXPECT_EQ(*Levers::ParallelForYieldMs(), 0);

        Levers::SetParallelForYieldMs(std::nullopt);
        EXPECT_FALSE(Levers::ParallelForYieldMs().has_value());

        Levers::SetParallelForYieldMs(originalInt);

        const std::optional<f32> originalRatio = Levers::TaskGraphOversubscriptionRatio();
        Levers::SetTaskGraphOversubscriptionRatio(2.5f);
        ASSERT_TRUE(Levers::TaskGraphOversubscriptionRatio().has_value());
        EXPECT_FLOAT_EQ(*Levers::TaskGraphOversubscriptionRatio(), 2.5f);
        Levers::SetTaskGraphOversubscriptionRatio(std::nullopt);
        EXPECT_FALSE(Levers::TaskGraphOversubscriptionRatio().has_value());
        Levers::SetTaskGraphOversubscriptionRatio(originalRatio);
    }

    TEST(DebugLevers, ActiveSummaryNamesOnlyNonDefaultLevers)
    {
        const bool original = Levers::BlackSquareHunt();
        Levers::SetBlackSquareHunt(false);
        EXPECT_EQ(Levers::ActiveSummary().find("OLO_RG_BLACKSQUARE_HUNT"), std::string::npos)
            << "a lever at its default must not appear in the startup log";

        Levers::SetBlackSquareHunt(true);
        const std::string summary = Levers::ActiveSummary();
        EXPECT_NE(summary.find("OLO_RG_BLACKSQUARE_HUNT"), std::string::npos);
        // The provenance matters when reading a log: a lever set by code is a
        // different situation from one the operator exported.
        EXPECT_NE(summary.find("set in code"), std::string::npos);

        Levers::SetBlackSquareHunt(original);
    }

    // -------------------------------------------------------------------
    // 2. The registry stays THE list
    // -------------------------------------------------------------------

    TEST(DebugLevers, NoEngineCodeReadsAnOloVariableOutsideTheRegistry)
    {
        // How the previous 21 levers accumulated: each one was a perfectly
        // reasonable local `Env::IsTruthy("OLO_...")`, and nothing objected. If
        // this test fails, add the lever to Core/DebugLevers.inl instead —
        // that is the whole cost, and it buys the startup log, the MCP tool and
        // a setter for free.
        // Custom delimiter: the pattern itself contains `)"`, which would close
        // a plain R"( ... )" literal early.
        const std::regex pattern(R"RX(Env::(?:Get|GetInt|IsTruthy|IsExactly)\s*\(\s*"(OLO[A-Z0-9_]*)")RX");

        std::vector<std::string> offenders;
        const fs::path root = RepoRoot() / "OloEngine" / "src";
        ASSERT_TRUE(fs::exists(root)) << "engine source tree not found at " << root;

        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const fs::path& path = entry.path();
            if (path.extension() != ".cpp" && path.extension() != ".h")
            {
                continue;
            }
            if (kNonLeverFiles.contains(path.filename().string()))
            {
                continue;
            }

            const std::string text = ReadFile(path);
            for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
            {
                offenders.push_back(path.filename().string() + " reads " + (*it)[1].str() + " directly");
            }
        }

        EXPECT_TRUE(offenders.empty())
            << "these read an OLO_* variable through Env:: instead of the lever registry:\n  "
            << [&offenders]
        {
            std::string joined;
            for (const std::string& o : offenders)
            {
                joined += o + "\n  ";
            }
            return joined;
        }();
    }

    TEST(DebugLevers, EveryLeverInTheTableIsReachableThroughSnapshot)
    {
        // Catches a .inl row that expands in one macro block but not another —
        // e.g. a new kind added to the accessors and forgotten in Snapshot(),
        // which would leave the lever settable but invisible to the startup log
        // and the MCP tool.
        const std::string table = ReadFile(RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Core" / "DebugLevers.inl");
        ASSERT_FALSE(table.empty()) << "DebugLevers.inl not found or empty";

        const std::regex row(R"RX(OLO_LEVER_[A-Z]+\s*\(\s*\w+\s*,\s*"(OLO[A-Z0-9_]*)")RX");
        std::set<std::string> declared;
        for (std::sregex_iterator it(table.begin(), table.end(), row), end; it != end; ++it)
        {
            declared.insert((*it)[1].str());
        }
        ASSERT_FALSE(declared.empty()) << "parsed no levers out of DebugLevers.inl — the row regex is stale";

        std::set<std::string> reported;
        for (const Levers::LeverInfo& lever : Levers::Snapshot())
        {
            reported.insert(std::string(lever.Name));
        }

        EXPECT_EQ(declared, reported)
            << "DebugLevers.inl and Snapshot() disagree about which levers exist (" << declared.size()
            << " in the table, " << reported.size() << " reported)";
    }
} // namespace OloEngine::Tests
