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
    } // namespace

    TEST(ComponentSerializerCoverage, EveryDeclaredComponentIsHandledBySceneSerializer)
    {
        const fs::path componentsHeader = RepoRoot() / "OloEngine" / "src" /
                                          "OloEngine" / "Scene" / "Components.h";
        const fs::path serializerCpp = RepoRoot() / "OloEngine" / "src" /
                                       "OloEngine" / "Scene" / "SceneSerializer.cpp";
        ASSERT_TRUE(fs::exists(componentsHeader));
        ASSERT_TRUE(fs::exists(serializerCpp));

        const std::string headerSrc = ReadFile(componentsHeader);
        const std::string serializerSrc = ReadFile(serializerCpp);

        // Find every `struct <Name>Component` declaration in the header.
        // No `^` anchor: `std::regex`'s ECMAScript mode doesn't enable
        // multiline by default. The `\b` boundary and the explicit
        // `struct` keyword are enough to avoid false matches.
        const std::regex structPat{ R"(struct\s+(\w+Component)\b)" };
        std::set<std::string> declared;
        for (auto it = std::sregex_iterator(headerSrc.begin(), headerSrc.end(), structPat);
             it != std::sregex_iterator(); ++it)
        {
            declared.insert((*it)[1].str());
        }
        ASSERT_FALSE(declared.empty())
            << "Regex didn't match any *Component structs in Components.h — header "
               "format changed and this test needs updating.";

        // Runtime-only components (derived state, networking runtime
        // state, etc.) that legitimately have no on-disk representation.
        // Each entry needs the rationale documented inline so the
        // exclusion list doesn't quietly grow into a dumping ground.
        const std::set<std::string> kRuntimeOnly = {
            "IDComponent",             // Entity UUID; serialised as the top-level `Entity: <uuid>` line, not as a sub-map under the entity.
            "DialogueStateComponent",  // Active dialogue progression (current node, text-reveal progress); recomputed at runtime.
            "InstancePortalComponent", // Networking runtime — assigned by the instance / portal manager, not authored in scenes.
            "NetworkLODComponent",     // Networking-derived LOD level; set by the interest manager per tick.
            "PhaseComponent",          // Animation phase runtime state; recomputed each tick.
            "UIResolvedRectComponent", // Layout-resolved UI rect; computed each tick by the UI system.
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
                << " component type(s) declared in Components.h but not plumbed "
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
        const fs::path componentsHeader = RepoRoot() / "OloEngine" / "src" /
                                          "OloEngine" / "Scene" / "Components.h";
        const fs::path serializerCpp = RepoRoot() / "OloEngine" / "src" /
                                       "OloEngine" / "Scene" / "SceneSerializer.cpp";
        ASSERT_TRUE(fs::exists(componentsHeader));
        ASSERT_TRUE(fs::exists(serializerCpp));

        const std::string headerSrc = ReadFile(componentsHeader);
        const std::string serializerSrc = ReadFile(serializerCpp);

        // Components.h includes other component headers transitively
        // (Animation, UI, Streaming, …). We also need to consult those
        // for `struct *Component` declarations or this test produces
        // false positives for legitimate references to externally-
        // declared components.
        //
        // Cheaper than walking every included file: pull the union of
        // every `*Component` token used as a struct declaration across
        // a curated set of header roots that Components.h pulls in.
        // MAINTENANCE: when adding new subsystem/component headers (especially
        // those that declare `struct *Component` outside of Components.h),
        // append the header path here to avoid false positives where this
        // coverage test flags externally-declared components as missing
        // serializer support.
        const std::vector<fs::path> componentHeaderRoots = {
            componentsHeader,
            // The headers Components.h itself reaches via #include — the
            // engine groups subsystem components in their own files.
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Animation" / "AnimatedMeshComponents.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Animation" / "AnimationGraphComponent.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Animation" / "MorphTargets" / "MorphTargetComponents.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Animation" / "IKTargetComponent.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Gameplay" / "Inventory" / "InventoryComponents.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Gameplay" / "Quest" / "QuestComponents.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Gameplay" / "Abilities" / "AbilityComponents.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Scene" / "Streaming" / "StreamingVolumeComponent.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "AI" / "AIComponents.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Networking" / "NetworkIdentityComponent.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Instancing" / "InstancedMeshComponent.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Localization" / "LocalizedTextComponent.h",
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Cinematic" / "CinematicComponent.h",
        };

        const std::regex structPat{ R"(struct\s+(\w+Component)\b)" };
        std::set<std::string> allDeclared;
        for (const auto& path : componentHeaderRoots)
        {
            if (std::error_code ec; !fs::exists(path, ec))
                continue; // header path may have moved; tolerate
            const std::string src = ReadFile(path);
            for (auto it = std::sregex_iterator(src.begin(), src.end(), structPat);
                 it != std::sregex_iterator(); ++it)
            {
                allDeclared.insert((*it)[1].str());
            }
        }

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
                << "list — append that header path to `componentHeaderRoots` above.\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
