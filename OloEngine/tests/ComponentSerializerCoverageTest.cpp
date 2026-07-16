// =============================================================================
// ComponentSerializerCoverageTest.cpp
//
// Meta-test: every `struct *Component` declared in
// `OloEngine/Scene/Components.h` (or via its included headers) must
// be plumbed through `SceneSerializer.cpp` — i.e., the literal
// `"<Name>Component"` key appears in the emit / deserialize code path.
//
// Catches the silent "I added a new component type but forgot to wire
// it into the serializer" bug class: instances of the component get
// added to entities via the editor's component-add menu, the user
// sets values, hits save, and the values silently disappear on
// reload because the emit path never wrote them.
//
// Known runtime-only components (no on-disk representation by design)
// are listed in the exclusion set with the reason documented inline.
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

        // The scene-serializer codegen (issue #380) moves the trivial-component
        // serialize/deserialize blocks out of SceneSerializer.cpp into two generated
        // .inl files (OloHeaderTool emits one block per all-trivial component). The
        // coverage corpus must therefore include those generated files, or every
        // migrated component would read as "not plumbed through the serializer".
        fs::path GeneratedSerializeInl()
        {
            return RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scene" /
                   "Generated" / "SceneSerializeComponents.Generated.inl";
        }
        fs::path GeneratedDeserializeInl()
        {
            return RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scene" /
                   "Generated" / "SceneDeserializeComponents.Generated.inl";
        }

        // The full set of headers that declare `struct *Component` types reachable
        // from the ECS, not just Components.h itself — the engine groups subsystem
        // components in their own files (Animation, Gameplay, Networking, …).
        // Components.h only #includes a handful of these transitively; the rest are
        // pulled in by CMake globbing / other TUs, so a text-only scan of
        // Components.h misses them entirely. Both the forward ("declared but not
        // serialized") and inverse ("serialized but not declared") checks must
        // share this exact list, or a component can slip through one direction.
        //
        // MAINTENANCE: when adding new subsystem/component headers (especially
        // those that declare `struct *Component` outside of Components.h),
        // append the header path here — this is the single shared source of
        // truth for both directions of this coverage test.
        std::vector<fs::path> ComponentHeaderRoots()
        {
            const fs::path root = RepoRoot() / "OloEngine" / "src" / "OloEngine";
            return {
                root / "Scene" / "Components.h",
                root / "Animation" / "AnimatedMeshComponents.h",
                root / "Animation" / "AnimationGraphComponent.h",
                root / "Animation" / "MorphTargets" / "MorphTargetComponents.h",
                root / "Animation" / "IKTargetComponent.h",
                root / "Animation" / "SpringBoneComponent.h",
                root / "Animation" / "NoiseAnimationComponent.h",
                root / "Animation" / "Retargeting" / "RetargetingComponent.h",
                root / "Animation" / "FootIKComponent.h",
                root / "Animation" / "LocomotionComponent.h",
                root / "Gameplay" / "Inventory" / "InventoryComponents.h",
                root / "Gameplay" / "Quest" / "QuestComponents.h",
                root / "Gameplay" / "Abilities" / "AbilityComponents.h",
                root / "Gameplay" / "Progression" / "ProgressionComponents.h",
                root / "Scene" / "Streaming" / "StreamingVolumeComponent.h",
                root / "AI" / "AIComponents.h",
                root / "Networking" / "NetworkIdentityComponent.h",
                root / "Renderer" / "Instancing" / "InstancedMeshComponent.h",
                root / "Localization" / "LocalizedTextComponent.h",
                root / "Cinematic" / "CinematicComponent.h",
            };
        }

        // Find every `struct <Name>Component` declaration across the given headers.
        // No `^` anchor: `std::regex`'s ECMAScript mode doesn't enable multiline by
        // default. The `\b` boundary and the explicit `struct` keyword are enough
        // to avoid false matches.
        std::set<std::string> CollectDeclaredComponents(const std::vector<fs::path>& headerRoots)
        {
            const std::regex structPat{ R"(struct\s+(\w+Component)\b)" };
            std::set<std::string> declared;
            for (const auto& path : headerRoots)
            {
                if (std::error_code ec; !fs::exists(path, ec))
                    continue; // header path may have moved; tolerate
                const std::string src = ReadFile(path);
                for (auto it = std::sregex_iterator(src.begin(), src.end(), structPat);
                     it != std::sregex_iterator(); ++it)
                {
                    declared.insert((*it)[1].str());
                }
            }
            return declared;
        }
    } // namespace

    TEST(ComponentSerializerCoverage, EveryDeclaredComponentIsHandledBySceneSerializer)
    {
        const fs::path serializerCpp = RepoRoot() / "OloEngine" / "src" /
                                       "OloEngine" / "Scene" / "SceneSerializer.cpp";
        ASSERT_TRUE(fs::exists(serializerCpp));
        ASSERT_TRUE(fs::exists(GeneratedSerializeInl()))
            << "Generated serialize .inl missing — build GenerateBindings first.";
        ASSERT_TRUE(fs::exists(GeneratedDeserializeInl()))
            << "Generated deserialize .inl missing — build GenerateBindings first.";

        // Corpus = the hand-written serializer PLUS the two OloHeaderTool-generated
        // .inl files. A component handled by either path counts as plumbed-through.
        const std::string serializerSrc = ReadFile(serializerCpp) +
                                          ReadFile(GeneratedSerializeInl()) +
                                          ReadFile(GeneratedDeserializeInl());

        // Scan the FULL header-root list (Components.h + every subsystem header),
        // not just Components.h — otherwise a non-trivial component declared in a
        // subsystem header (e.g. InventoryComponent) escapes detection entirely
        // (issue #545).
        const std::set<std::string> declared = CollectDeclaredComponents(ComponentHeaderRoots());
        ASSERT_FALSE(declared.empty())
            << "Regex didn't match any *Component structs in the header roots — "
               "header format changed and this test needs updating.";

        // Runtime-only components (derived state, networking runtime
        // state, etc.) that legitimately have no on-disk representation.
        // Each entry needs the rationale documented inline so the
        // exclusion list doesn't quietly grow into a dumping ground.
        const std::set<std::string> kRuntimeOnly = {
            "IDComponent",                  // Entity UUID; serialised as the top-level `Entity: <uuid>` line, not as a sub-map under the entity.
            "DialogueStateComponent",       // Active dialogue progression (current node, text-reveal progress); recomputed at runtime.
            "InstancePortalComponent",      // Networking runtime — assigned by the instance / portal manager, not authored in scenes.
            "NetworkLODComponent",          // Networking-derived LOD level; set by the interest manager per tick.
            "NoiseAnimationStateComponent", // Per-tick noise-animation phase/offset state; recomputed each tick, not authored.
            "PhaseComponent",               // Animation phase runtime state; recomputed each tick.
            "FootIKStateComponent",         // Foot-IK adaptation state (plant locks, pelvis smoothing, per-tick ground cache); recomputed at runtime, never authored (issue #631).
            "LocomotionStateComponent",     // Locomotion controller state (gait, smoothing, stride-warp base speeds); recomputed at runtime, never authored (issue #631).
            "RetargetingStateComponent",    // Live-retargeting bake cache (baked clips + last-baked settings); rebuilt lazily at runtime, never authored (issue #631).
            "SpringBoneStateComponent",     // Per-tick spring-bone simulation state (velocity, current position); recomputed each tick.
            "UIResolvedRectComponent",      // Layout-resolved UI rect; computed each tick by the UI system.
            "WorldTransformComponent",      // Composed parent-chain world matrix; rebuilt every tick by Scene::PropagateWorldTransforms() (issue #499).
        };

        std::vector<std::string> missing;
        for (const auto& name : declared)
        {
            if (kRuntimeOnly.contains(name))
                continue;
            // The serializer references each component by its quoted
            // type-name string on the emit side AND the deserialize side.
            const std::string token = '"' + name + '"';
            if (serializerSrc.find(token) == std::string::npos)
                missing.push_back(name);
        }

        if (!missing.empty())
        {
            std::ostringstream oss;
            oss << missing.size()
                << " component type(s) declared in the header roots but not plumbed "
                << "through SceneSerializer.cpp:\n";
            for (const auto& n : missing)
                oss << "  - " << n << "\n";
            oss << "\nEither add Serialize/Deserialize branches for the component, "
                << "or — if it's runtime-only — add it to the `kRuntimeOnly` "
                << "exclusion set in ComponentSerializerCoverageTest.cpp with a "
                << "comment explaining why.\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Inverse: every component the serializer mentions actually exists.
    //
    // Catches dead code in `SceneSerializer.cpp` after a component
    // struct is renamed or removed from `Components.h`. Compiles fine
    // (it's just a YAML string and a missing-key branch falls through
    // silently), but the dead serializer branch confuses future readers
    // and could subtly mis-route data if the dead name happens to match
    // a new-but-unrelated YAML key.
    // -------------------------------------------------------------------------
    TEST(ComponentSerializerCoverage, EverySerializerComponentReferenceHasMatchingStruct)
    {
        const fs::path serializerCpp = RepoRoot() / "OloEngine" / "src" /
                                       "OloEngine" / "Scene" / "SceneSerializer.cpp";
        ASSERT_TRUE(fs::exists(serializerCpp));

        // Same combined corpus as the forward test: scan the hand-written serializer
        // AND the generated .inl so a renamed/removed component is caught wherever
        // its serialization lives.
        const std::string serializerSrc = ReadFile(serializerCpp) +
                                          ReadFile(GeneratedSerializeInl()) +
                                          ReadFile(GeneratedDeserializeInl());

        // Same header-root list as the forward test — see ComponentHeaderRoots()
        // above. Consulting the full list (not just Components.h) avoids false
        // positives for legitimate references to externally-declared components.
        const std::set<std::string> allDeclared = CollectDeclaredComponents(ComponentHeaderRoots());

        // Find every `"<X>Component"` literal in the serializer.
        const std::regex tokenPat{ R"(\"(\w+Component)\")" };
        std::set<std::string> referenced;
        for (auto it = std::sregex_iterator(serializerSrc.begin(), serializerSrc.end(), tokenPat);
             it != std::sregex_iterator(); ++it)
        {
            referenced.insert((*it)[1].str());
        }
        ASSERT_FALSE(referenced.empty()) << "Serializer string-literal scan produced no matches.";

        std::vector<std::string> dead;
        for (const auto& name : referenced)
        {
            if (!allDeclared.contains(name))
                dead.push_back(name);
        }

        if (!dead.empty())
        {
            std::ostringstream oss;
            oss << dead.size()
                << " component name(s) referenced by SceneSerializer.cpp but with no "
                << "matching `struct *Component` in the searched headers:\n";
            for (const auto& n : dead)
                oss << "  - " << n << "\n";
            oss << "\nEither remove the dead serializer branch, or — if the "
                << "component is declared in a header not in this test's search "
                << "list — append that header path to `ComponentHeaderRoots()` above.\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Disjointness: a component must be serialized by EXACTLY ONE of the
    // hand-written SceneSerializer.cpp or the OloHeaderTool-generated .inl —
    // never both. The scene serializer codegen (issue #380) generates a block
    // for every all-trivial component NOT in the generator's
    // kComponentsCustomSerialize set; if a component is BOTH generated AND left
    // hand-written, the two `if (entity.HasComponent<T>())` blocks both run —
    // serialize emits a duplicate YAML key and deserialize calls AddComponent<T>
    // twice (an EnTT assert/throw). This catches that double-emit at test time
    // instead of letting it ship as a runtime corruption.
    // -------------------------------------------------------------------------
    TEST(ComponentSerializerCoverage, NoComponentIsBothHandWrittenAndGenerated)
    {
        const fs::path serializerCpp = RepoRoot() / "OloEngine" / "src" /
                                       "OloEngine" / "Scene" / "SceneSerializer.cpp";
        ASSERT_TRUE(fs::exists(serializerCpp));
        ASSERT_TRUE(fs::exists(GeneratedSerializeInl()))
            << "Generated serialize .inl missing — build GenerateBindings first.";

        const std::string handWrittenSrc = ReadFile(serializerCpp);
        const std::string generatedSrc = ReadFile(GeneratedSerializeInl());

        // Hand-written serialize blocks open with `out << YAML::Key << "X";`;
        // generated ones with `entity.HasComponent<X>()`. Collect the component
        // name from each and intersect.
        const std::regex handPat{ R"(out << YAML::Key << \"(\w+Component)\";)" };
        const std::regex genPat{ R"(entity\.HasComponent<(\w+Component)>\(\))" };

        std::set<std::string> handWritten;
        for (auto it = std::sregex_iterator(handWrittenSrc.begin(), handWrittenSrc.end(), handPat);
             it != std::sregex_iterator(); ++it)
        {
            handWritten.insert((*it)[1].str());
        }
        std::set<std::string> generated;
        for (auto it = std::sregex_iterator(generatedSrc.begin(), generatedSrc.end(), genPat);
             it != std::sregex_iterator(); ++it)
        {
            generated.insert((*it)[1].str());
        }
        ASSERT_FALSE(generated.empty())
            << "Generated serialize .inl produced no HasComponent<> blocks — the "
               "codegen or this test's pattern is stale.";

        std::vector<std::string> both;
        for (const auto& name : generated)
        {
            if (handWritten.contains(name))
                both.push_back(name);
        }

        if (!both.empty())
        {
            std::ostringstream oss;
            oss << both.size()
                << " component(s) serialized BOTH by hand in SceneSerializer.cpp AND "
                << "by the generated .inl (double-emit → duplicate YAML key / double "
                << "AddComponent):\n";
            for (const auto& n : both)
                oss << "  - " << n << "\n";
            oss << "\nRemove the hand-written block, or add the component to "
                << "kComponentsCustomSerialize in tools/OloHeaderTool/main.cpp so the "
                << "generator skips it.\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
