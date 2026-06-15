// =============================================================================
// ComponentTupleCoverageTest.cpp
//
// Meta-test: every `struct *Component` declared in
// `OloEngine/Scene/Components.h` (or the subsystem component headers it
// pulls in) must be listed in the `AllComponents` tuple at the bottom of
// Components.h — and, inversely, every entry in that tuple must resolve to
// a real declared struct.
//
// Why this matters: `AllComponents` is iterated by three separate code
// paths —
//   * Scene::CopyComponent / CopyComponentIfExists  (scene copy + prefab / duplicate)
//   * ScriptGlue::RegisterComponent                 (C# `HasComponent<T>()`)
// A component that is declared, serialized, and save-game-wired but left
// OUT of the tuple silently breaks scene copy, prefab instantiation, and
// the C# `HasComponent<T>()` registration — with NO link error and NO
// other test failure. This is the last unguarded *silent* touch-point of
// the six-point ECS cross-binding checklist in CLAUDE.md. The others fail
// loudly or are covered elsewhere: a missing `OnComponentAdded<T>` is a
// link error, and the SceneSerializer / SaveGame paths each have their own
// coverage test (ComponentSerializerCoverageTest.cpp,
// SaveGame/SaveGameComponentSerializerCoverageTest.cpp).
//
// Components that are intentionally NOT in the tuple (entity identity that
// is copied by hand, or per-tick runtime-derived state) live in the
// `kNotInTuple` exclusion set with the reason documented inline, mirroring
// the `kRuntimeOnly` set in ComponentSerializerCoverageTest.cpp.
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

        fs::path ComponentsHeaderPath()
        {
            return RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scene" / "Components.h";
        }

        // The headers Components.h reaches via #include that declare their
        // own `struct *Component`. This MUST stay in sync with the
        // identically-purposed `componentHeaderRoots` list in
        // ComponentSerializerCoverageTest.cpp — both tests need the same
        // union of declared components to avoid false positives/negatives.
        // When you add a subsystem component header, append it to BOTH lists.
        std::vector<fs::path> ComponentHeaderRoots()
        {
            const fs::path base = RepoRoot() / "OloEngine" / "src" / "OloEngine";
            return {
                ComponentsHeaderPath(),
                base / "Animation" / "AnimatedMeshComponents.h",
                base / "Animation" / "AnimationGraphComponent.h",
                base / "Animation" / "MorphTargets" / "MorphTargetComponents.h",
                base / "Animation" / "IKTargetComponent.h",
                base / "Animation" / "SpringBoneComponent.h",
                base / "Animation" / "NoiseAnimationComponent.h",
                base / "Gameplay" / "Inventory" / "InventoryComponents.h",
                base / "Gameplay" / "Quest" / "QuestComponents.h",
                base / "Gameplay" / "Abilities" / "AbilityComponents.h",
                base / "Scene" / "Streaming" / "StreamingVolumeComponent.h",
                base / "AI" / "AIComponents.h",
                base / "Networking" / "NetworkIdentityComponent.h",
                base / "Renderer" / "Instancing" / "InstancedMeshComponent.h",
                base / "Localization" / "LocalizedTextComponent.h",
                base / "Cinematic" / "CinematicComponent.h",
            };
        }

        // Union of every `struct *Component` declaration across the header roots.
        std::set<std::string> CollectDeclaredComponents()
        {
            // No `^` anchor: std::regex ECMAScript mode isn't multiline by
            // default. The explicit `struct` keyword + `\b` boundary suffice.
            const std::regex structPat{ R"(struct\s+(\w+Component)\b)" };
            std::set<std::string> declared;
            for (const auto& path : ComponentHeaderRoots())
            {
                if (std::error_code ec; !fs::exists(path, ec))
                    continue; // header path may have moved; tolerate (matches serializer test)
                const std::string src = ReadFile(path);
                for (auto it = std::sregex_iterator(src.begin(), src.end(), structPat);
                     it != std::sregex_iterator(); ++it)
                {
                    declared.insert((*it)[1].str());
                }
            }
            return declared;
        }

        // Extract the type tokens listed in
        //     using AllComponents = ComponentGroup< ... >;
        // The tuple is a flat comma-separated list of plain type names (no
        // nested templates), so the first `>;` after `ComponentGroup<` is its
        // terminator.
        std::set<std::string> CollectTupleMembers()
        {
            const std::string headerSrc = ReadFile(ComponentsHeaderPath());
            const std::string marker = "AllComponents = ComponentGroup<";
            const auto markerPos = headerSrc.find(marker);
            if (markerPos == std::string::npos)
                return {};
            const auto listStart = markerPos + marker.size();
            const auto listEnd = headerSrc.find(">;", listStart);
            if (listEnd == std::string::npos)
                return {};
            const std::string tupleText = headerSrc.substr(listStart, listEnd - listStart);

            const std::regex tokenPat{ R"((\w+Component)\b)" };
            std::set<std::string> members;
            for (auto it = std::sregex_iterator(tupleText.begin(), tupleText.end(), tokenPat);
                 it != std::sregex_iterator(); ++it)
            {
                members.insert((*it)[1].str());
            }
            return members;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Forward: every declared component is a member of AllComponents.
    // -------------------------------------------------------------------------
    TEST(ComponentTupleCoverage, EveryDeclaredComponentIsInAllComponentsTuple)
    {
        ASSERT_TRUE(fs::exists(ComponentsHeaderPath()));

        const std::set<std::string> declared = CollectDeclaredComponents();
        ASSERT_FALSE(declared.empty())
            << "Regex matched no *Component structs across the header roots — "
               "header format changed and this test needs updating.";

        const std::set<std::string> tuple = CollectTupleMembers();
        ASSERT_FALSE(tuple.empty())
            << "Couldn't parse the `AllComponents = ComponentGroup<...>` tuple in "
               "Components.h — the declaration moved or changed shape and this "
               "test's parser needs updating.";

        // Components that legitimately do NOT belong in AllComponents. Each
        // entry needs its rationale inline so this set can't quietly grow into
        // a dumping ground.
        const std::set<std::string> kNotInTuple = {
            // Entity identity + name. Copied EXPLICITLY during scene copy and
            // entity creation, not via the tuple — Scene.cpp's CopyComponent
            // call is literally commented "(except IDComponent and TagComponent)".
            "IDComponent",
            "TagComponent",
            // Per-tick runtime-derived state: recomputed every frame, never
            // copied, serialized, or script-registered.
            "UIResolvedRectComponent",  // Layout-resolved UI rect; recomputed each frame by the UI layout pass.
            "DialogueStateComponent",   // Active dialogue progression (current node, text-reveal); rebuilt at runtime.
            "SpringBoneStateComponent", // Spring-bone sim state; SpringBoneComponent.h documents it as deliberately out of the tuple (sim restarts fresh on scene copy).
            "NoiseAnimationStateComponent", // Noise phase accumulator; NoiseAnimationComponent.h documents it as deliberately out of the tuple (phase restarts fresh at zero on scene copy).
        };

        std::vector<std::string> missing;
        for (const auto& name : declared)
        {
            if (kNotInTuple.contains(name))
                continue;
            if (!tuple.contains(name))
                missing.push_back(name);
        }

        if (!missing.empty())
        {
            std::ostringstream oss;
            oss << missing.size()
                << " component type(s) declared in Components.h / its component "
                << "headers but missing from the `AllComponents` tuple:\n";
            for (const auto& n : missing)
                oss << "  - " << n << "\n";
            oss << "\nA component left out of AllComponents is silently skipped by "
                << "scene copy, prefab instantiation, and C# HasComponent<T>() "
                << "registration (no link error, no other test failure). Add it to "
                << "the tuple at the bottom of Components.h — or, if it is entity-"
                << "identity / runtime-derived state, add it to the `kNotInTuple` "
                << "exclusion set in ComponentTupleCoverageTest.cpp with a comment "
                << "explaining why.\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Inverse: every tuple entry resolves to a real declared struct.
    //
    // A stale entry (a component renamed or removed from the headers but left
    // in the tuple) is actually a hard compile error in the real build —
    // AllComponents instantiates the type — but this test localises it to a
    // clear message instead of a wall of template-instantiation errors, and
    // guards the header-root search list at the same time (a tuple entry whose
    // declaring header is missing from `ComponentHeaderRoots()` surfaces here).
    // -------------------------------------------------------------------------
    TEST(ComponentTupleCoverage, EveryAllComponentsTupleEntryHasMatchingStruct)
    {
        ASSERT_TRUE(fs::exists(ComponentsHeaderPath()));

        const std::set<std::string> declared = CollectDeclaredComponents();
        ASSERT_FALSE(declared.empty())
            << "Regex matched no *Component structs across the header roots.";

        const std::set<std::string> tuple = CollectTupleMembers();
        ASSERT_FALSE(tuple.empty())
            << "Couldn't parse the `AllComponents = ComponentGroup<...>` tuple.";

        std::vector<std::string> dangling;
        for (const auto& name : tuple)
        {
            if (!declared.contains(name))
                dangling.push_back(name);
        }

        if (!dangling.empty())
        {
            std::ostringstream oss;
            oss << dangling.size()
                << " entry/entries in the `AllComponents` tuple with no matching "
                << "`struct *Component` in the searched headers:\n";
            for (const auto& n : dangling)
                oss << "  - " << n << "\n";
            oss << "\nEither the component was renamed/removed (update the tuple to "
                << "match), or it is declared in a header not in this test's search "
                << "list — append that header to `ComponentHeaderRoots()` (and to "
                << "the matching list in ComponentSerializerCoverageTest.cpp).\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
