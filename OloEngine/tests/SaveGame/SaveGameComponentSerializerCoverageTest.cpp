// =============================================================================
// SaveGameComponentSerializerCoverageTest.cpp
//
// Meta-test: every component registered via `REGISTER_SAVE_COMPONENT(...)` in
// `SaveGame/SaveGameComponentSerializer.cpp` must also be enumerated in BOTH
// per-entity loops in `SaveGame/SaveGameSerializer.cpp` — the
// `SAVE_COMPONENT(...)` list (capture) and the `TRY_LOAD_COMPONENT(...)` /
// direct `LOAD_COMPONENT(...)` list (restore) — and vice versa.
//
// Catches the silent "registered but never captured" bug class (issue #325):
// `RegisterAll()` registers a `Serialize()` overload, but the capture/restore
// loops are explicit enumerations, so a component missing from either list
// round-trips fine through scene YAML yet silently vanishes through
// save-games — inventory/quest/ability progress lost, no error, no warning.
//
// Sibling of ComponentSerializerCoverageTest.cpp, which pins the equivalent
// contract for the scene-YAML path (Components.h ↔ SceneSerializer.cpp).
// =============================================================================

#include "OloEnginePCH.h"

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

        fs::path SaveGameDir()
        {
            return RepoRoot() / "OloEngine" / "src" / "OloEngine" / "SaveGame";
        }

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }

        std::set<std::string> Matches(const std::string& src, const std::regex& pat)
        {
            std::set<std::string> out;
            for (auto it = std::sregex_iterator(src.begin(), src.end(), pat);
                 it != std::sregex_iterator(); ++it)
            {
                out.insert((*it)[1].str());
            }
            return out;
        }

        // Every pattern requires the trailing `;` of an invocation so the
        // `#define SAVE_COMPONENT(ComponentType, ...)` / `#define
        // TRY_LOAD_COMPONENT(ComponentType)` lines (which continue with `\`
        // or a macro body instead of `;`) never match.
        std::set<std::string> RegisteredComponents()
        {
            const std::string src = ReadFile(SaveGameDir() / "SaveGameComponentSerializer.cpp");
            return Matches(src, std::regex{ R"(REGISTER_SAVE_COMPONENT\((\w+)\);)" });
        }

        std::set<std::string> SavedComponents(const std::string& serializerSrc)
        {
            return Matches(serializerSrc, std::regex{ R"(SAVE_COMPONENT\((\w+), entity, writer\);)" });
        }

        std::set<std::string> LoadedComponents(const std::string& serializerSrc)
        {
            // Union of the TRY_LOAD_COMPONENT list and direct LOAD_COMPONENT
            // calls: IDComponent is restored via a bare LOAD_COMPONENT (no
            // `if (!matched)` guard) because it is always the first block of
            // an entity, so both spellings count as load coverage.
            std::set<std::string> loaded =
                Matches(serializerSrc, std::regex{ R"(TRY_LOAD_COMPONENT\((\w+)\);)" });
            std::set<std::string> direct =
                Matches(serializerSrc, std::regex{ R"(LOAD_COMPONENT\((\w+), entity, typeHash, compData\);)" });
            loaded.insert(direct.begin(), direct.end());
            return loaded;
        }

        void ExpectSubset(const std::set<std::string>& names,
                          const std::set<std::string>& allowed,
                          const char* missingFrom, const char* fixHint)
        {
            std::vector<std::string> missing;
            for (const auto& name : names)
            {
                if (!allowed.contains(name))
                    missing.push_back(name);
            }
            if (!missing.empty())
            {
                std::ostringstream oss;
                oss << missing.size() << " component type(s) missing from " << missingFrom << ":\n";
                for (const auto& n : missing)
                    oss << "  - " << n << "\n";
                oss << "\n"
                    << fixHint << "\n";
                FAIL() << oss.str();
            }
        }
    } // namespace

    // Forward: a registered serializer that the capture/restore loops never
    // invoke is dead code AND a data-loss bug — the component silently
    // vanishes through save-games.
    TEST(SaveGameComponentSerializerCoverage, EveryRegisteredComponentIsCapturedAndRestored)
    {
        const fs::path serializerCpp = SaveGameDir() / "SaveGameSerializer.cpp";
        ASSERT_TRUE(fs::exists(serializerCpp));

        const std::set<std::string> registered = RegisteredComponents();
        ASSERT_FALSE(registered.empty())
            << "REGISTER_SAVE_COMPONENT scan produced no matches — "
               "SaveGameComponentSerializer.cpp format changed and this test needs updating.";

        const std::string serializerSrc = ReadFile(serializerCpp);
        const std::set<std::string> saved = SavedComponents(serializerSrc);
        const std::set<std::string> loaded = LoadedComponents(serializerSrc);
        ASSERT_FALSE(saved.empty()) << "SAVE_COMPONENT scan produced no matches.";
        ASSERT_FALSE(loaded.empty()) << "TRY_LOAD_COMPONENT / LOAD_COMPONENT scan produced no matches.";

        ExpectSubset(registered, saved,
                     "the SAVE_COMPONENT(...) capture list in SaveGameSerializer.cpp",
                     "Add a SAVE_COMPONENT(<Name>, entity, writer); entry to CaptureSceneState — "
                     "a registered component absent from the capture loop is silently dropped "
                     "from every save-game. If the component is intentionally session-only, "
                     "remove its REGISTER_SAVE_COMPONENT entry instead.");
        ExpectSubset(registered, loaded,
                     "the TRY_LOAD_COMPONENT(...) restore list in SaveGameSerializer.cpp",
                     "Add a TRY_LOAD_COMPONENT(<Name>); entry to DeserializeEntitiesInto — "
                     "a component that is saved but never restored is silently dropped "
                     "on every load. If the component is intentionally session-only, "
                     "remove its REGISTER_SAVE_COMPONENT entry instead.");
    }

    // Inverse: an enumerated component with no registered serializer would
    // either fail to compile (SAVE_COMPONENT calls the Serialize overload
    // directly) or — once overloads move behind the registry — silently
    // skip. Either way the three lists must stay in lock-step.
    TEST(SaveGameComponentSerializerCoverage, EveryCapturedAndRestoredComponentIsRegistered)
    {
        const std::set<std::string> registered = RegisteredComponents();
        ASSERT_FALSE(registered.empty());

        const std::string serializerSrc = ReadFile(SaveGameDir() / "SaveGameSerializer.cpp");
        const std::set<std::string> saved = SavedComponents(serializerSrc);
        const std::set<std::string> loaded = LoadedComponents(serializerSrc);
        ASSERT_FALSE(saved.empty());
        ASSERT_FALSE(loaded.empty());

        ExpectSubset(saved, registered,
                     "RegisterAll() in SaveGameComponentSerializer.cpp (referenced by SAVE_COMPONENT)",
                     "Add a REGISTER_SAVE_COMPONENT(<Name>); entry to RegisterAll(), or remove "
                     "the stale SAVE_COMPONENT entry.");
        ExpectSubset(loaded, registered,
                     "RegisterAll() in SaveGameComponentSerializer.cpp (referenced by TRY_LOAD_COMPONENT)",
                     "Add a REGISTER_SAVE_COMPONENT(<Name>); entry to RegisterAll(), or remove "
                     "the stale TRY_LOAD_COMPONENT entry.");

        // Symmetry between the two enumeration lists follows from both being
        // subsets of `registered` plus the forward test's superset checks,
        // but assert it directly for a clearer failure message.
        EXPECT_EQ(saved, loaded)
            << "SAVE_COMPONENT and TRY_LOAD_COMPONENT lists have drifted apart — "
               "a component saved but not loaded (or vice versa) is silently dropped.";
    }
} // namespace OloEngine::Tests
