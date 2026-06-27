// OLO_TEST_LAYER: unit
// =============================================================================
// ComponentHandlerCoverageTest.cpp
//
// Meta-test guarding the OloHeaderTool-generated Scene::OnComponentAdded /
// OnComponentRemoved no-op lists:
//   Scene/Generated/OnComponentAdded.Generated.inl
//   Scene/Generated/OnComponentRemoved.Generated.inl
//
// Those files hold one OLO_ON_COMPONENT_{ADDED,REMOVED}_NOOP(T) per
// `struct *Component` whose add/remove callback is a pure no-op — i.e. every
// component MINUS the hand-written custom handlers. Scene.cpp #includes them
// inside a macro that expands each to an empty `template<>` specialization, so
// they ARE the engine/editor link safety net: the OnComponentAdded/Removed
// primary templates are declaration-only, and a component added/removed without
// a specialization is an unresolved symbol at link time.
//
// This test is NOT redundant with that link error. The build only fails for a
// component that is actually Add/RemoveComponent'd somewhere; a brand-new
// component nobody instantiates yet, or a stale generated file checked in after
// a rename, slips past the linker. This test reconciles the generated lists
// against the universe of declared components and the curated custom-handler
// exclusion sets, so drift surfaces as a clear failure instead of a future
// surprise link error.
//
// The universe of components is the same one the generator scans: every
// `struct *Component`. That equals the generated AllComponents tuple PLUS the
// generator's kComponentsNotInTuple set (tuple = all-structs − kNotInTuple), so
// we reconstruct it from those two without re-scanning the source tree.
//
// The three exclusion sets below MUST stay in sync with their namesakes in
// tools/OloHeaderTool/main.cpp — exactly the sync contract ComponentTupleCoverage
// Test enforces for kComponentsNotInTuple. A mismatch here means the generator
// and this guard disagree about which handlers are hand-written.
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

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }

        fs::path GeneratedDir()
        {
            return RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scene" / "Generated";
        }

        // Extract the type tokens listed in `using AllComponents = ComponentGroup<...>;`.
        // (Same parse as ComponentTupleCoverageTest — the tuple is a flat comma list.)
        std::set<std::string> CollectTupleMembers()
        {
            const std::string src = ReadFile(GeneratedDir() / "AllComponents.Generated.inl");
            const std::string marker = "AllComponents = ComponentGroup<";
            const auto markerPos = src.find(marker);
            if (markerPos == std::string::npos)
                return {};
            const auto listStart = markerPos + marker.size();
            const auto listEnd = src.find(">;", listStart);
            if (listEnd == std::string::npos)
                return {};
            const std::string tupleText = src.substr(listStart, listEnd - listStart);

            const std::regex tokenPat{ R"((\w+Component)\b)" };
            std::set<std::string> members;
            for (auto it = std::sregex_iterator(tupleText.begin(), tupleText.end(), tokenPat);
                 it != std::sregex_iterator(); ++it)
            {
                members.insert((*it)[1].str());
            }
            return members;
        }

        // Extract the macro invocations from a generated no-op list. Anchored at
        // line start (multiline) so the macro mention inside the header comment
        // ("One OLO_..._NOOP(T) per ...") is not counted — only real invocations.
        std::set<std::string> CollectNoopList(const fs::path& path, const std::string& macro)
        {
            const std::string src = ReadFile(path);
            const std::regex pat{ "(?:^|\\n)" + macro + R"(\((\w+)\))" };
            std::set<std::string> names;
            for (auto it = std::sregex_iterator(src.begin(), src.end(), pat);
                 it != std::sregex_iterator(); ++it)
            {
                names.insert((*it)[1].str());
            }
            return names;
        }

        // Mirror of tools/OloHeaderTool/main.cpp::kComponentsNotInTuple. Reconstructs
        // the full scanned universe: tuple ∪ kNotInTuple == every `struct *Component`.
        const std::set<std::string> kNotInTuple = {
            "IDComponent",
            "TagComponent",
            "UIResolvedRectComponent",
            "DialogueStateComponent",
            "SpringBoneStateComponent",
            "NoiseAnimationStateComponent",
        };

        // Mirror of tools/OloHeaderTool/main.cpp::kComponentsCustomOnAdd — the
        // components whose OnComponentAdded<T> is hand-written in Scene.cpp and is
        // therefore NOT in the generated no-op list. Keep in sync with the generator.
        const std::set<std::string> kCustomOnAdd = {
            "CameraComponent",
            "LocalizedTextComponent",
            "CinematicComponent",
            "AudioSoundGraphComponent",
            "VideoOverlayComponent",
            "VideoSurfaceComponent",
            "Rigidbody3DComponent",
            "PhysicsJoint3DComponent",
            "VehicleComponent",
            "RagdollComponent",
            "CharacterController3DComponent",
        };

        // Mirror of tools/OloHeaderTool/main.cpp::kComponentsCustomOnRemove — the
        // components whose OnComponentRemoved<T> is hand-written in Scene.cpp.
        const std::set<std::string> kCustomOnRemove = {
            "Rigidbody2DComponent",
            "Rigidbody3DComponent",
            "PhysicsJoint3DComponent",
            "VehicleComponent",
            "RagdollComponent",
            "CharacterController3DComponent",
            "AudioSoundGraphComponent",
            "VideoOverlayComponent",
            "VideoSurfaceComponent",
            "SpringBoneComponent",
            "NoiseAnimationComponent",
        };

        // The full universe of declared `struct *Component`, reconstructed as
        // tuple ∪ kNotInTuple (see header).
        std::set<std::string> ComponentUniverse()
        {
            std::set<std::string> universe = CollectTupleMembers();
            universe.insert(kNotInTuple.begin(), kNotInTuple.end());
            return universe;
        }

        // Shared check: the generated no-op `list` must equal universe − custom.
        // Reports both directions (missing no-op → would be a link error; stray
        // entry → a custom handler that should be hand-written, or a dangling name).
        void ExpectNoopListMatchesUniverse(const std::set<std::string>& list,
                                           const std::set<std::string>& custom,
                                           const char* callback,
                                           const char* customSetName)
        {
            const std::set<std::string> universe = ComponentUniverse();
            ASSERT_FALSE(universe.empty())
                << "Couldn't reconstruct the component universe — the AllComponents tuple "
                   "failed to parse. Regenerate with `cmake --build build --target "
                   "GenerateBindings`.";
            ASSERT_FALSE(list.empty())
                << "Parsed zero entries from the generated OnComponent" << callback
                << " no-op list — regenerate it, or the macro/emit shape changed and this "
                   "test's parser needs updating.";

            std::vector<std::string> missing; // in universe, not custom, but not generated
            for (const auto& c : universe)
            {
                if (custom.contains(c))
                    continue;
                if (!list.contains(c))
                    missing.push_back(c);
            }

            std::vector<std::string> stray; // generated, but custom or not a real component
            for (const auto& c : list)
            {
                if (custom.contains(c))
                    stray.push_back(c + " (in " + customSetName + " — should be hand-written)");
                else if (!universe.contains(c))
                    stray.push_back(c + " (no matching struct *Component)");
            }

            std::ostringstream oss;
            if (!missing.empty())
            {
                oss << missing.size() << " component(s) declared but missing a generated "
                    << "OnComponent" << callback << " no-op:\n";
                for (const auto& n : missing)
                    oss << "  - " << n << "\n";
                oss << "Each would be an unresolved-symbol link error the moment it is "
                    << callback << "'d. Regenerate (`cmake --build build --target "
                                   "GenerateBindings`) and re-stage the .inl, or — if it needs a real "
                                   "body — hand-write the specialization in Scene.cpp AND add it to "
                    << customSetName << " (generator) and this test.\n";
            }
            if (!stray.empty())
            {
                oss << stray.size() << " unexpected entry/entries in the generated "
                    << "OnComponent" << callback << " no-op list:\n";
                for (const auto& n : stray)
                    oss << "  - " << n << "\n";
                oss << "A component with a hand-written body must be excluded by the "
                       "generator's "
                    << customSetName << " set (and listed in this test's mirror); a name "
                                        "with no matching struct is a stale rename — regenerate.\n";
            }
            if (!missing.empty() || !stray.empty())
                FAIL() << oss.str();
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The generated OnComponentAdded no-op list == universe − kComponentsCustomOnAdd.
    // -------------------------------------------------------------------------
    TEST(ComponentHandlerCoverage, OnComponentAddedNoopListMatchesUniverse)
    {
        const std::set<std::string> added =
            CollectNoopList(GeneratedDir() / "OnComponentAdded.Generated.inl", "OLO_ON_COMPONENT_ADDED_NOOP");
        ExpectNoopListMatchesUniverse(added, kCustomOnAdd, "Added", "kComponentsCustomOnAdd");
    }

    // -------------------------------------------------------------------------
    // The generated OnComponentRemoved no-op list == universe − kComponentsCustomOnRemove.
    // -------------------------------------------------------------------------
    TEST(ComponentHandlerCoverage, OnComponentRemovedNoopListMatchesUniverse)
    {
        const std::set<std::string> removed =
            CollectNoopList(GeneratedDir() / "OnComponentRemoved.Generated.inl", "OLO_ON_COMPONENT_REMOVED_NOOP");
        ExpectNoopListMatchesUniverse(removed, kCustomOnRemove, "Removed", "kComponentsCustomOnRemove");
    }

    // -------------------------------------------------------------------------
    // Sanity: the two custom-handler sets are genuinely different. This is the
    // whole reason the generator keeps two sets instead of one — e.g. a camera
    // does real init on add but nothing on remove. If they ever became identical,
    // someone likely copy-pasted one set over the other.
    // -------------------------------------------------------------------------
    TEST(ComponentHandlerCoverage, CustomAddAndRemoveSetsDiffer)
    {
        EXPECT_NE(kCustomOnAdd, kCustomOnRemove)
            << "kComponentsCustomOnAdd and kComponentsCustomOnRemove are identical — they "
               "are supposed to differ (add-only vs remove-only custom handlers). Check "
               "for a copy-paste error against tools/OloHeaderTool/main.cpp.";
    }
} // namespace OloEngine::Tests
