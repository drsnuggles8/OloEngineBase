// =============================================================================
// AssetContentValidityTest.cpp
//
// Catches the class of OloEditor breakage where a sample asset file on
// disk has malformed YAML, missing required keys, or values of the wrong
// type. The editor surfaces this as "scene fails to load" / "inventory
// silently empty when X.oloitem is referenced" / "dialogue tree blank"
// the moment a user opens that file — undiagnosable without engine logs.
//
// Scope
// -----
//   Structural YAML validation only. Each file under
//   `OloEditor/SandboxProject/Assets/` of a known asset type must:
//     - parse as well-formed YAML (no syntax errors),
//     - declare its expected top-level key (Scene / Item / Quest / Dialogue),
//     - meet the minimum-structure contract for that asset type (entities
//       have UUIDs, items have an ID, etc.).
//
// What this test does NOT do
// --------------------------
//   * Full SceneSerializer.Deserialize() — that path constructs ECS state,
//     touches the asset manager, instantiates materials/meshes, and is
//     covered by integration tests that mount a temp project. Trying to
//     run it without a mounted project crashes on null asset lookups (by
//     design — production never runs the deserializer without a project).
//   * Cross-asset reference validation (does this scene's mesh GUID exist
//     in the registry?). Belongs in a registry-consistency test.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Gameplay/Progression/CharacterClassDatabase.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        std::vector<fs::path> EnumerateFilesByExtension(const fs::path& dir, std::string_view extension)
        {
            std::vector<fs::path> out;
            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
                return out;
            for (auto& entry : fs::recursive_directory_iterator(dir, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().extension() == extension)
                    out.push_back(entry.path());
            }
            std::ranges::sort(out);
            return out;
        }

        struct Failure
        {
            std::string Path;
            std::string Reason;
        };

        // Parse the file as YAML, returning std::nullopt on error and
        // recording the reason. Centralized so all asset-type tests give
        // consistent failure messages.
        std::optional<YAML::Node> ParseYAMLFile(const fs::path& path, std::string& outReason)
        {
            try
            {
                return YAML::LoadFile(path.generic_string());
            }
            catch (const YAML::Exception& e)
            {
                outReason = std::string("YAML parse error: ") + e.what();
            }
            catch (const std::exception& e)
            {
                outReason = std::string("std::exception during YAML parse: ") + e.what();
            }
            catch (...)
            {
                outReason = "unknown exception during YAML parse";
            }
            return std::nullopt;
        }

        void ReportFailures(const char* assetKind, const std::vector<Failure>& failures, sizet checked)
        {
            ASSERT_GE(checked, 1u) << "No " << assetKind << " files were enumerated — test root broken?";
            if (failures.empty())
                return;

            std::ostringstream oss;
            oss << failures.size() << " " << assetKind << " file(s) failed validation:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }

        fs::path SandboxAssetsRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets";
        }

        // Slurp a whole asset file for the serializers' string-based
        // TestDeserializeFromYAML entry points (no asset manager required).
        std::string ReadFileToString(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Scenes (.olo)
    // -------------------------------------------------------------------------

    TEST(AssetContentValidity, AllSandboxScenesAreStructurallyValid)
    {
        const auto scenes = EnumerateFilesByExtension(SandboxAssetsRoot() / "Scenes", ".olo");
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
            {
                failures.push_back({ path.generic_string(), reason });
                continue;
            }

            if (!(*node)["Scene"])
            {
                failures.push_back({ path.generic_string(),
                                     "Missing top-level 'Scene' key — not a scene file?" });
                continue;
            }
            const YAML::Node entities = (*node)["Entities"];
            if (!entities)
            {
                // Empty scenes are allowed (no Entities key at all is fine);
                // but if the key is present it must be a sequence.
                continue;
            }
            if (!entities.IsSequence())
            {
                failures.push_back({ path.generic_string(),
                                     "'Entities' is present but not a YAML sequence" });
                continue;
            }
            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                {
                    failures.push_back({ path.generic_string(),
                                         "Entities[" + std::to_string(i) + "] is not a map" });
                    continue;
                }
                if (!ent["Entity"])
                {
                    failures.push_back({ path.generic_string(),
                                         "Entities[" + std::to_string(i) + "] missing 'Entity' UUID" });
                    continue;
                }
                // UUID must be parseable as an integer (engine UUID is u64).
                try
                {
                    (void)ent["Entity"].as<u64>();
                }
                catch (...)
                {
                    failures.push_back({ path.generic_string(),
                                         "Entities[" + std::to_string(i) + "] 'Entity' is not a u64" });
                }
            }
        }

        ReportFailures("scene", failures, scenes.size());
    }

    // -------------------------------------------------------------------------
    // Items (.oloitem)
    // -------------------------------------------------------------------------

    TEST(AssetContentValidity, AllSandboxItemsAreStructurallyValid)
    {
        const auto items = EnumerateFilesByExtension(SandboxAssetsRoot() / "Items", ".oloitem");
        std::vector<Failure> failures;

        for (const auto& path : items)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
            {
                failures.push_back({ path.generic_string(), reason });
                continue;
            }

            const YAML::Node item = (*node)["ItemDefinition"];
            if (!item)
            {
                failures.push_back({ path.generic_string(),
                                     "Missing top-level 'ItemDefinition' key — not an item file?" });
                continue;
            }
            if (!item["ItemID"])
            {
                failures.push_back({ path.generic_string(),
                                     "Missing 'ItemDefinition.ItemID' — items must have a stable identifier" });
            }
        }

        ReportFailures("item", failures, items.size());
    }

    // -------------------------------------------------------------------------
    // Quests (.oloquest)
    // -------------------------------------------------------------------------

    TEST(AssetContentValidity, AllSandboxQuestsAreStructurallyValid)
    {
        const auto quests = EnumerateFilesByExtension(SandboxAssetsRoot() / "Quests", ".oloquest");
        std::vector<Failure> failures;

        for (const auto& path : quests)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
            {
                failures.push_back({ path.generic_string(), reason });
                continue;
            }

            // Quest files are flat YAML — keys at the root, no nesting
            // under a `Quest:` wrapper. Required: a stable QuestID and
            // a Stages sequence.
            if (!(*node)["QuestID"])
            {
                failures.push_back({ path.generic_string(),
                                     "Missing top-level 'QuestID' — quests must have a stable identifier" });
                continue;
            }
            const YAML::Node stages = (*node)["Stages"];
            if (!stages || !stages.IsSequence())
            {
                failures.push_back({ path.generic_string(),
                                     "Missing or non-sequence 'Stages' — every quest must declare stages" });
            }
        }

        ReportFailures("quest", failures, quests.size());
    }

    // -------------------------------------------------------------------------
    // Dialogues (.olodialogue)
    // -------------------------------------------------------------------------

    TEST(AssetContentValidity, AllSandboxDialoguesAreStructurallyValid)
    {
        const auto dialogues = EnumerateFilesByExtension(SandboxAssetsRoot() / "Dialogues", ".olodialogue");

        // Dialogues are an optional asset type — the sandbox may legitimately
        // ship zero of them — so we don't assert ≥1 file here.
        std::vector<Failure> failures;

        for (const auto& path : dialogues)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
            {
                failures.push_back({ path.generic_string(), reason });
                continue;
            }

            // Dialogue files declare a top-level key — accept either
            // 'Dialogue' or 'DialogueTree' depending on which authoring
            // version they were saved by.
            if (!(*node)["Dialogue"] && !(*node)["DialogueTree"])
            {
                failures.push_back({ path.generic_string(),
                                     "Missing top-level 'Dialogue' or 'DialogueTree' key" });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " dialogue file(s) failed validation:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Within-scene UUID uniqueness
    //
    // Engine code uses an entity's UUID as a stable identifier across
    // serialization sessions and across the script-binding boundary
    // (every `Entity.GetUUID()` lookup goes through it). Two entities
    // sharing the same UUID inside a single scene means:
    //   - The engine's UUID→Entity map sees only one of them (the last
    //     deserialized wins), so the other entity is effectively
    //     orphaned for any code that looks up by UUID — including
    //     RelationshipComponent's parent/child links, the asset
    //     registry's per-entity asset bindings, save-game restore, and
    //     network identity resolution.
    //   - Per-entity script state collides: a Lua script bound to two
    //     entities that share a UUID will see only one of them.
    //
    // The bug class is silent: the scene loads, the entities exist in
    // EnTT (which uses its own per-registry IDs), and the editor
    // displays them — but any UUID-based interaction silently picks one.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, AllSandboxScenesHaveUniqueEntityUUIDs)
    {
        const auto scenes = EnumerateFilesByExtension(SandboxAssetsRoot() / "Scenes", ".olo");
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
                continue; // structural-parse failures handled elsewhere

            const YAML::Node entities = (*node)["Entities"];
            if (!entities || !entities.IsSequence())
                continue;

            std::set<u64> seen;
            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                    continue;
                const YAML::Node uuidNode = ent["Entity"];
                if (!uuidNode || !uuidNode.IsScalar())
                    continue;
                u64 uuid;
                try
                {
                    uuid = uuidNode.as<u64>();
                }
                catch (...)
                {
                    continue;
                }

                if (!seen.insert(uuid).second)
                {
                    failures.push_back({
                        path.generic_string(),
                        "Entities[" + std::to_string(i) + "] reuses UUID " +
                            std::to_string(uuid) +
                            " — UUID-based engine lookups will silently pick one entity.",
                    });
                }
            }
        }

        ReportFailures("scene (with duplicate entity UUID)", failures, scenes.size());
    }

    // -------------------------------------------------------------------------
    // Structural integrity: PrefabComponent has both required UUIDs
    //
    // Every scene's `PrefabComponent` is two UUID fields:
    //   - `PrefabID` — points at the source prefab asset.
    //   - `PrefabEntityID` — the specific entity within that prefab this
    //     instance corresponds to.
    // Both are required for the prefab override system to resolve which
    // entity in the source to merge against. A missing or zero UUID on
    // either field surfaces in OloEditor as "this prefab instance ignores
    // override edits" — the override-resolution code silently falls back
    // to a no-op and the user sees their changes to the prefab not flow
    // through to the instance.
    //
    // We don't validate that the PrefabID resolves to an on-disk
    // `.oloprefab` file — the sandbox ships unit-test-fixture scenes
    // (`PrefabOverrideTest.olo`) whose PrefabIDs are deliberate
    // placeholders used by the prefab metadata tests in C++. Whether
    // the prefab is real is orthogonal to whether the YAML structure
    // is correct.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, AllSandboxScenePrefabComponentsHaveRequiredUUIDs)
    {
        const auto scenes = EnumerateFilesByExtension(SandboxAssetsRoot() / "Scenes", ".olo");
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
                continue;

            const YAML::Node entities = (*node)["Entities"];
            if (!entities || !entities.IsSequence())
                continue;

            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                    continue;
                const YAML::Node prefab = ent["PrefabComponent"];
                if (!prefab || !prefab.IsMap())
                    continue;

                auto check = [&](const char* key)
                {
                    const YAML::Node n = prefab[key];
                    if (!n || !n.IsScalar())
                    {
                        failures.push_back({
                            path.generic_string(),
                            "Entities[" + std::to_string(i) + "].PrefabComponent missing '" + std::string(key) + "' UUID field",
                        });
                        return;
                    }
                    u64 v = 0;
                    try
                    {
                        v = n.as<u64>();
                    }
                    catch (...)
                    {
                        failures.push_back({
                            path.generic_string(),
                            "Entities[" + std::to_string(i) + "].PrefabComponent." + key +
                                " is not a u64",
                        });
                        return;
                    }
                    if (v == 0)
                    {
                        failures.push_back({
                            path.generic_string(),
                            "Entities[" + std::to_string(i) + "].PrefabComponent." + key +
                                " is the zero UUID — prefab override resolution will silently no-op",
                        });
                    }
                };
                check("PrefabID");
                check("PrefabEntityID");
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " PrefabComponent structural defect(s):\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Within-scene PrefabEntityID uniqueness
    //
    // Companion to `AllSandboxScenePrefabComponentsHaveRequiredUUIDs`.
    // Two prefab instances in the same scene that share a
    // `PrefabEntityID` claim to be the same "slot" within the source
    // prefab — the override-resolution system can only address one of
    // them. Symptom in OloEditor: "I edited prefab override values on
    // instance A but they appeared on instance B" because the lookup
    // table can only hold one entry per (PrefabID, PrefabEntityID).
    //
    // Note that the same PrefabEntityID showing up in DIFFERENT scenes
    // is fine — each scene maintains its own instance map.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, AllSandboxScenesHaveUniquePrefabEntityIDsPerPrefab)
    {
        const auto scenes = EnumerateFilesByExtension(SandboxAssetsRoot() / "Scenes", ".olo");
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
                continue;

            const YAML::Node entities = (*node)["Entities"];
            if (!entities || !entities.IsSequence())
                continue;

            // Key = (PrefabID, PrefabEntityID). Two entities with the
            // same pair = duplicate slot, the failure case.
            std::set<std::pair<u64, u64>> seen;

            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                    continue;
                const YAML::Node prefab = ent["PrefabComponent"];
                if (!prefab || !prefab.IsMap())
                    continue;
                const YAML::Node pid = prefab["PrefabID"];
                const YAML::Node eid = prefab["PrefabEntityID"];
                if (!pid || !pid.IsScalar() || !eid || !eid.IsScalar())
                    continue;
                u64 pidVal = 0, eidVal = 0;
                try
                {
                    pidVal = pid.as<u64>();
                    eidVal = eid.as<u64>();
                }
                catch (...)
                {
                    continue;
                }
                if (pidVal == 0 || eidVal == 0)
                    continue; // covered by structural test

                if (!seen.insert({ pidVal, eidVal }).second)
                {
                    failures.push_back({
                        path.generic_string(),
                        "Entities[" + std::to_string(i) +
                            "].PrefabComponent (PrefabID=" + std::to_string(pidVal) +
                            ", PrefabEntityID=" + std::to_string(eidVal) +
                            ") duplicates an earlier entity in this scene — "
                            "prefab override resolution will pick one instance.",
                    });
                }
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " duplicate (PrefabID, PrefabEntityID) pair(s):\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // ShaderGraphs (.olosg)
    // -------------------------------------------------------------------------

    TEST(AssetContentValidity, AllSandboxShaderGraphsAreStructurallyValid)
    {
        const auto graphs = EnumerateFilesByExtension(SandboxAssetsRoot() / "ShaderGraphs", ".olosg");
        std::vector<Failure> failures;

        for (const auto& path : graphs)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
            {
                failures.push_back({ path.generic_string(), reason });
                continue;
            }

            const YAML::Node graph = (*node)["ShaderGraph"];
            if (!graph)
            {
                failures.push_back({ path.generic_string(),
                                     "Missing top-level 'ShaderGraph' key" });
                continue;
            }
            const YAML::Node nodes = graph["Nodes"];
            if (!nodes || !nodes.IsSequence())
            {
                failures.push_back({ path.generic_string(),
                                     "Missing or non-sequence 'ShaderGraph.Nodes' — a graph "
                                     "must declare at least its node list" });
                continue;
            }
            // Every node must have an ID and a TypeName — without those
            // the graph compiler can't dispatch nor link.
            for (sizet i = 0; i < nodes.size(); ++i)
            {
                const YAML::Node n = nodes[i];
                if (!n.IsMap())
                {
                    failures.push_back({ path.generic_string(),
                                         "Nodes[" + std::to_string(i) + "] is not a map" });
                    continue;
                }
                if (!n["ID"])
                    failures.push_back({ path.generic_string(),
                                         "Nodes[" + std::to_string(i) + "] missing 'ID'" });
                if (!n["TypeName"])
                    failures.push_back({ path.generic_string(),
                                         "Nodes[" + std::to_string(i) + "] missing 'TypeName'" });
            }
        }

        ReportFailures("shader-graph", failures, graphs.size());
    }

    // -------------------------------------------------------------------------
    // Cross-asset reference: scene-bound scripts exist on disk
    //
    // Catches the silent class of breakage where a scene references a C#
    // class (`ScriptComponent.ClassName = Sandbox.PlayerController`) or
    // a Lua script (`LuaScriptComponent.ScriptFile = Scripts/.../X.lua`)
    // that's been renamed or deleted. The editor surfaces this as "the
    // entity exists but its scripted behaviour doesn't run" — no error,
    // just silent dropout.
    //
    // For C# scripts the convention is `<Namespace>.<ClassName>` mapping
    // to `Assets/Scripts/Source/<ClassName>.cs`. We strip the namespace
    // and check the file exists; missing .cs files mean the DLL won't
    // contain the referenced class.
    //
    // For Lua scripts the `ScriptFile` is an `Assets/`-relative path.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SandboxSceneReferencedScriptsExistOnDisk)
    {
        const fs::path assetsRoot = SandboxAssetsRoot();
        const fs::path sourceDir = assetsRoot / "Scripts" / "Source";
        const auto scenes = EnumerateFilesByExtension(assetsRoot / "Scenes", ".olo");
        ASSERT_FALSE(scenes.empty());

        std::vector<Failure> failures;

        // Walk every scene file and inspect each entity for script
        // components.
        for (const auto& scenePath : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(scenePath, reason);
            if (!node)
                continue; // parse-level failures are AllSandboxScenes...'s job

            const YAML::Node entities = (*node)["Entities"];
            if (!entities || !entities.IsSequence())
                continue;

            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                    continue;

                // C# ScriptComponent.ClassName
                if (const YAML::Node sc = ent["ScriptComponent"]; sc && sc.IsMap())
                {
                    if (const YAML::Node cn = sc["ClassName"]; cn && cn.IsScalar())
                    {
                        const std::string fullName = cn.as<std::string>();
                        // Strip the namespace prefix ("Sandbox.Foo" → "Foo").
                        // A bare name (no '.') is also valid.
                        const auto dot = fullName.rfind('.');
                        const std::string className = (dot == std::string::npos)
                                                          ? fullName
                                                          : fullName.substr(dot + 1);
                        const fs::path expected = sourceDir / (className + ".cs");
                        std::error_code ec;
                        if (!fs::exists(expected, ec))
                        {
                            failures.push_back({
                                scenePath.generic_string(),
                                "Entities[" + std::to_string(i) +
                                    "].ScriptComponent.ClassName = '" + fullName +
                                    "' but no source file at " + expected.generic_string(),
                            });
                        }
                        else
                        {
                            // The file exists — verify it actually contains an
                            // Entity-derived class named `className`. The engine's
                            // script binding only resolves classes that derive
                            // from `OloEngine.Entity`; binding to a helper class
                            // silently no-ops.
                            std::ifstream sf(expected, std::ios::binary);
                            std::ostringstream buf;
                            buf << sf.rdbuf();
                            const std::string src = buf.str();
                            const std::regex pat{
                                R"(class\s+)" + className + R"(\s*:\s*Entity\b)",
                            };
                            if (!std::regex_search(src, pat))
                            {
                                failures.push_back({
                                    scenePath.generic_string(),
                                    "Entities[" + std::to_string(i) +
                                        "].ScriptComponent.ClassName = '" + fullName +
                                        "' resolves to " + expected.generic_string() +
                                        ", but that file does not declare `class " +
                                        className + " : Entity` — the runtime "
                                                    "script-binding system will silently no-op.",
                                });
                            }
                        }
                    }
                }

                // Lua LuaScriptComponent.ScriptFile
                if (const YAML::Node lsc = ent["LuaScriptComponent"]; lsc && lsc.IsMap())
                {
                    if (const YAML::Node sf = lsc["ScriptFile"]; sf && sf.IsScalar())
                    {
                        const std::string scriptFile = sf.as<std::string>();
                        if (scriptFile.empty())
                            continue; // Empty ScriptFile is "no script bound" — fine.

                        const fs::path expected = assetsRoot / scriptFile;
                        std::error_code ec;
                        if (!fs::exists(expected, ec))
                        {
                            failures.push_back({
                                scenePath.generic_string(),
                                "Entities[" + std::to_string(i) +
                                    "].LuaScriptComponent.ScriptFile = '" + scriptFile +
                                    "' but no file at " + expected.generic_string(),
                            });
                        }
                    }
                }
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " scene-bound script reference(s) missing on disk:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // AssetRegistry.oar — parseability + on-disk path consistency
    //
    // The .oar file is the editor's binary index of every asset in the
    // project: UUID → file path mappings, asset types, dependency
    // counts. The editor reads it on startup; a corrupted or out-of-sync
    // registry leaves the editor with "asset GUID resolves to a path
    // that doesn't exist" silent failures on every scene load (mesh
    // shows up as the default fallback, texture as the missing-texture
    // checker pattern, etc.) — exactly the breakage the user described
    // when they kicked off this testing push.
    //
    // We check two invariants:
    //   1. The binary .oar deserializes cleanly via the production
    //      `AssetRegistry::Deserialize` code path.
    //   2. Every path mentioned in the registry exists on disk under
    //      the sandbox project root.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SandboxAssetRegistryDeserialisesAndPathsResolve)
    {
        const fs::path projectRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        const fs::path registryPath = projectRoot / "AssetRegistry.oar";
        ASSERT_TRUE(fs::exists(registryPath))
            << "AssetRegistry.oar not found at " << registryPath.string();

        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(registryPath))
            << "AssetRegistry::Deserialize returned false on the production .oar — "
               "the binary file is corrupt or the on-disk format has drifted from "
               "the deserialiser. Opening the editor would silently fail to load assets.";

        const sizet count = registry.GetAssetCount();
        EXPECT_GT(count, 0u)
            << "Deserialised registry is empty — the editor would resolve every "
               "asset GUID to 'not found'.";

        // Fetch-on-demand assets (scripts/assets/asset-manifest.json) are deliberately NOT
        // committed — too large for git history, or licence-restricted — and are absent until the
        // user runs Fetch-Assets.ps1. The manifest's own contract is explicit: "Every consumer
        // (scene, test) must degrade gracefully when an asset is absent ... never fail." CI never
        // fetches them, so a registered-but-unfetched asset (e.g. the 137 MB Stanford dragon the
        // Nanite stress scene uses, issue #629) must be TOLERATED here, not reported missing.
        // Collect their resolved paths from the manifest so those are skipped below; every other
        // registered path is still required to exist.
        std::set<std::string> fetchOnDemand;
        {
            const fs::path repoRoot = fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
            const fs::path manifestPath = repoRoot / "scripts" / "assets" / "asset-manifest.json";
            if (std::ifstream in(manifestPath); in)
            {
                try
                {
                    nlohmann::json manifest;
                    in >> manifest;
                    for (const auto& asset : manifest.value("assets", nlohmann::json::array()))
                    {
                        if (const std::string dest = asset.value("dest", std::string{}); !dest.empty())
                            fetchOnDemand.insert((repoRoot / dest).lexically_normal().generic_string());
                    }
                }
                catch (const std::exception&)
                {
                    // A malformed manifest must not silently disable the check — leave the set
                    // empty so every registered path is validated as before.
                }
            }
        }

        // Each registry entry's FilePath should point at a real file
        // under the project root. (Engine convention: AssetMetadata
        // stores the project-relative path resolved against the
        // project's Assets directory.)
        const auto allAssets = registry.GetAllAssets();
        std::vector<Failure> missing;
        u32 fetchOnDemandAbsent = 0;
        for (const auto& metadata : allAssets)
        {
            const fs::path resolved = projectRoot / metadata.FilePath;
            std::error_code ec;
            if (!fs::exists(resolved, ec))
            {
                if (fetchOnDemand.contains(resolved.lexically_normal().generic_string()))
                {
                    ++fetchOnDemandAbsent; // optional asset the user has not fetched — not a failure
                    continue;
                }
                missing.push_back({
                    metadata.FilePath.generic_string(),
                    "registered in AssetRegistry.oar but no file at " + resolved.generic_string(),
                });
            }
        }

        if (fetchOnDemandAbsent > 0)
        {
            std::cout << "[ INFO     ] " << fetchOnDemandAbsent
                      << " fetch-on-demand asset(s) registered but not present (run scripts/Fetch-Assets.ps1"
                         " to materialise them); tolerated per asset-manifest.json.\n";
        }

        if (!missing.empty())
        {
            std::ostringstream oss;
            oss << missing.size() << " registered asset path(s) missing on disk:\n";
            for (const auto& f : missing)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            oss << "\nTo clean the registry, run:\n"
                << "  OloEngine-Tests.exe --gtest_also_run_disabled_tests "
                << "--gtest_filter=AssetContentValidity.DISABLED_RebaseAssetRegistry\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Reverse asset-registry consistency
    //
    // Catches the inverse of the previous test: a file with a supported
    // asset extension exists on disk under `Assets/` but no entry in
    // AssetRegistry.oar references it. The editor surfaces this as "I
    // imported a texture by dropping it into the Assets/ folder but the
    // editor doesn't see it" — the file is on disk, but the registry's
    // out of sync (developer didn't trigger an asset rescan, the .oar
    // was committed before the file was, or the registry import logic
    // skipped the file silently).
    //
    // We only check files whose extension is a *supported* asset type
    // (per `AssetExtensions::GetAllSupportedExtensions()`). Source code,
    // build artefacts, and any non-asset content are out of scope.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, EverySupportedAssetOnDiskIsInTheRegistry)
    {
        const fs::path projectRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        const fs::path assetsRoot = projectRoot / "Assets";
        const fs::path registryPath = projectRoot / "AssetRegistry.oar";
        ASSERT_TRUE(fs::exists(registryPath));

        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(registryPath));

        // Collect every project-relative path the registry knows about,
        // normalised to forward-slash form.
        std::set<std::string> registeredPaths;
        for (const auto& metadata : registry.GetAllAssets())
        {
            std::string p = metadata.FilePath.generic_string();
            registeredPaths.insert(std::move(p));
        }

        // Walk every file under `Assets/` whose extension marks it as a
        // tracked asset type, and check the registry knows about it.
        std::vector<Failure> unregistered;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(assetsRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;

            // AssetExtensions::IsExtensionSupported takes a normalised
            // string (with or without leading dot). The std::filesystem
            // extension() returns it with the leading dot.
            if (const std::string ext = entry.path().extension().string(); !AssetExtensions::IsExtensionSupported(ext))
                continue;

            // Path the registry stores: project-relative, forward
            // slashes, including the `Assets/` prefix.
            std::string relative = fs::relative(entry.path(), projectRoot, ec).generic_string();
            if (ec)
                continue;

            if (!registeredPaths.contains(relative))
            {
                unregistered.push_back({
                    relative,
                    "supported asset file exists on disk but is not in AssetRegistry.oar — "
                    "the editor will not see it.",
                });
            }
        }

        if (!unregistered.empty())
        {
            std::ostringstream oss;
            oss << unregistered.size() << " on-disk asset file(s) missing from registry:\n";
            for (const auto& f : unregistered)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            oss << "\nTo regenerate the registry, launch OloEditor (which auto-rescans),\n"
                << "or run the DISABLED_RebaseAssetRegistry helper (currently only removes\n"
                << "stale entries — extend it if a clean-rebuild path becomes useful).\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Asset registry path case-sensitivity
    //
    // Windows file systems are case-insensitive: `Assets/textures/foo.png`
    // resolves whether the on-disk path is `Assets/Textures/Foo.png` or
    // any other casing. Linux/WSL is case-sensitive — a path stored in
    // the registry with the wrong casing simply won't resolve.
    //
    // Detection: for each registry path, walk it component-by-component
    // and at each level enumerate the parent directory to find the
    // case-exact filename. If the registry's stored case doesn't match
    // what the OS reports, the path would fail on Linux even though it
    // works on Windows.
    //
    // This is a Linux/WSL-specific catch. On Windows the test reports
    // the mismatches that would break on Linux without having to run
    // the editor over there.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SandboxAssetRegistryPathsMatchOnDiskCasing)
    {
        const fs::path projectRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        const fs::path registryPath = projectRoot / "AssetRegistry.oar";
        ASSERT_TRUE(fs::exists(registryPath));

        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(registryPath));

        // For each path the registry knows about, walk every component
        // and confirm the case-exact filename is present in its parent
        // directory.
        std::vector<Failure> mismatches;
        for (const auto& metadata : registry.GetAllAssets())
        {
            const fs::path relative = metadata.FilePath;
            fs::path accumulated = projectRoot;
            bool mismatched = false;

            for (auto component = relative.begin();
                 component != relative.end() && !mismatched;
                 ++component)
            {
                const std::string expectedName = component->generic_string();
                if (expectedName.empty() || expectedName == ".")
                    continue;

                // Enumerate the directory and look for a case-exact
                // match. Case-insensitive matches (different casing) get
                // recorded as the mismatch — we found the file but the
                // registry stores it with wrong case.
                bool foundExact = false;
                bool foundCaseInsensitive = false;
                std::string actualName;
                if (std::error_code ec; fs::is_directory(accumulated, ec))
                {
                    for (auto& entry : fs::directory_iterator(accumulated, ec))
                    {
                        if (ec)
                            break;
                        const std::string name = entry.path().filename().generic_string();
                        if (name == expectedName)
                        {
                            foundExact = true;
                            break;
                        }
                        // ASCII-only case-fold for the case-insensitive
                        // match. Asset filenames in this project are ASCII;
                        // non-ASCII would need full Unicode case folding
                        // which isn't worth pulling ICU in for. std::tolower
                        // requires the unsigned-char cast to avoid UB on
                        // negative char values.
                        if (name.size() == expectedName.size())
                        {
                            bool ci = true;
                            for (sizet k = 0; k < name.size(); ++k)
                            {
                                const int al = std::tolower(static_cast<unsigned char>(name[k]));
                                const int bl = std::tolower(static_cast<unsigned char>(expectedName[k]));
                                if (al != bl)
                                {
                                    ci = false;
                                    break;
                                }
                            }
                            if (ci)
                            {
                                foundCaseInsensitive = true;
                                actualName = name;
                            }
                        }
                    }
                }
                if (!foundExact)
                {
                    if (foundCaseInsensitive)
                    {
                        mismatches.push_back({
                            metadata.FilePath.generic_string(),
                            "registry component '" + expectedName +
                                "' has wrong case — on-disk is '" + actualName +
                                "'. Path would not resolve on Linux/WSL.",
                        });
                    }
                    mismatched = true;
                }
                accumulated /= expectedName;
            }
        }

        if (!mismatches.empty())
        {
            std::ostringstream oss;
            oss << mismatches.size() << " registry path(s) with case mismatch vs disk:\n";
            for (const auto& f : mismatches)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Scene asset-handle reachability
    //
    // Scenes reference assets (meshes, materials, shader graphs,
    // animation graphs, light probe baked data, …) via u64 GUIDs.
    // The runtime looks up the GUID in the AssetRegistry; an
    // unregistered GUID means the editor can't load the asset and the
    // scene's component silently falls back to defaults.
    //
    // We walk every scene's YAML for known asset-handle field names,
    // skip zero (uninitialised) values, and verify each non-zero GUID
    // resolves to a registry entry.
    //
    // Field name list is conservative: only fields the engine
    // explicitly uses as AssetHandle ABI today. Adding a new asset
    // type means appending its serialisation key here so future scenes
    // get the same coverage.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, AllSandboxSceneAssetHandlesAreInTheRegistry)
    {
        const fs::path projectRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        const fs::path registryPath = projectRoot / "AssetRegistry.oar";
        ASSERT_TRUE(fs::exists(registryPath));
        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(registryPath));

        // Known asset-handle field names in scene YAML. These are
        // serialised as u64 scalars; any other u64 scalar is treated
        // as a non-handle (entity UUID, etc.) and skipped.
        const std::set<std::string> handleFieldNames = {
            "ShaderGraphHandle",
            "AnimationGraphAssetHandle",
            "BakedDataAsset",
            "EnvironmentMapAsset",
            "MeshHandle",
            "MaterialHandle",
            "TextureHandle",
            "FontHandle",
            "AudioFileHandle",
            "ColliderAsset",     // MeshCollider3D / ConvexMeshCollider3D / TriangleMeshCollider3D
            "RegionAssetHandle", // StreamingVolumeComponent
            "BehaviorTreeAsset", // BehaviorTreeComponent (note: no "Handle" suffix)
            "StateMachineAsset", // StateMachineComponent (note: no "Handle" suffix)
        };

        const auto scenes = EnumerateFilesByExtension(projectRoot / "Assets" / "Scenes", ".olo");
        std::vector<Failure> failures;
        u32 handlesChecked = 0;

        // Recursive walker — for each map node, inspect its keys and
        // descend.
        std::function<void(const YAML::Node&, const std::string&, const std::string&)> walk =
            [&](const YAML::Node& n, const std::string& path, const std::string& currentKey)
        {
            if (n.IsScalar())
            {
                if (!handleFieldNames.contains(currentKey))
                    return;
                u64 handle = 0;
                try
                {
                    handle = n.as<u64>();
                }
                catch (...)
                {
                    return;
                }
                if (handle == 0)
                    return; // unset
                ++handlesChecked;
                if (!registry.Exists(static_cast<AssetHandle>(handle)))
                {
                    failures.push_back({
                        path,
                        currentKey + " = " + std::to_string(handle) +
                            " is not in AssetRegistry.oar — the editor "
                            "would resolve to a missing asset.",
                    });
                }
            }
            else if (n.IsSequence())
            {
                for (sizet i = 0; i < n.size(); ++i)
                    walk(n[i], path, currentKey);
            }
            else if (n.IsMap())
            {
                for (auto it = n.begin(); it != n.end(); ++it)
                {
                    const std::string key = it->first.as<std::string>("?");
                    walk(it->second, path, key);
                }
            }
            else
            {
                // No additional handling required.
            }
        };

        for (const auto& path : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
                continue;
            walk(*node, path.generic_string(), "");
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " unregistered scene asset handle(s):\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
        // No EXPECT_GT here: scenes may legitimately use zero non-zero
        // asset handles today; the test is a regression guard.
        (void)handlesChecked;
    }

    // -------------------------------------------------------------------------
    // Shader cache integrity
    //
    // The engine caches compiled shader binaries under
    // `OloEditor/assets/cache/shader/opengl/` and `cache/shader/vulkan/`,
    // one set of files per source `.glsl`. The cache is checked into
    // git so first-build / fresh-clone doesn't need to recompile the
    // 42-shader pipeline.
    //
    // When a developer deletes or renames a `.glsl`, the corresponding
    // cache entries can be left behind: the runtime ignores orphan cache
    // files because nothing requests them, but they bloat the working
    // tree and confuse new developers ("what shader is this for?").
    //
    // Filename pattern (from the on-disk format): `<Name>.glsl.cached_{opengl|vulkan}.<stage>`.
    // To validate, strip the cache suffix back to `<Name>.glsl` and
    // verify the source still exists under `OloEditor/assets/shaders/`.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, ShaderCacheEntriesAllHaveLiveGlslSources)
    {
        const fs::path editorRoot = fs::path{ OLO_TEST_EDITOR_ROOT };
        const fs::path shaderRoot = editorRoot / "assets" / "shaders";
        const fs::path cacheRoot = editorRoot / "assets" / "cache" / "shader";
        if (!fs::exists(cacheRoot))
            return; // Cache hasn't been generated yet (fresh clone before first build).

        std::vector<Failure> orphans;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(cacheRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;

            // Strip everything from `.cached_` onwards to recover the
            // source filename. Matches all backend / stage variants
            // (`cached_opengl.frag`, `cached_vulkan.pgr`, `cached_opengl.comp`).
            const std::string filename = entry.path().filename().generic_string();
            const auto cachedPos = filename.find(".cached_");
            if (cachedPos == std::string::npos)
                continue; // not a cache entry
            const std::string sourceName = filename.substr(0, cachedPos);
            if (sourceName.empty())
                continue;

            // Engine-internal shaders (convention: leading double
            // underscore) are constructed procedurally by
            // `ShaderWarmup::Init` / `ShaderLibrary::InitFallbackShader`
            // — they have no on-disk source by design. The cache still
            // stores their compiled binaries so first-run is fast.
            if (sourceName.size() >= 2 && sourceName[0] == '_' && sourceName[1] == '_')
                continue;

            // Recursively search the shader root for a file with this
            // name. Most cached shaders live at the root, but compute
            // shaders are in `compute/`.
            bool found = false;
            for (auto& candidate : fs::recursive_directory_iterator(shaderRoot, ec))
            {
                if (ec)
                    break;
                if (!candidate.is_regular_file())
                    continue;
                if (candidate.path().filename().generic_string() == sourceName)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                orphans.push_back({
                    entry.path().generic_string(),
                    "cache file references '" + sourceName +
                        "' but no matching source under " + shaderRoot.generic_string(),
                });
            }
        }

        if (!orphans.empty())
        {
            std::ostringstream oss;
            oss << orphans.size() << " orphan shader cache entries:\n";
            for (const auto& f : orphans)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            oss << "\nTo clean: delete each orphan cache file. The engine will "
                << "regenerate any it actually needs on next launch.\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Cache-directory whitelist — every cached file must match a known pattern.
    //
    // The engine writes binary caches under `OloEditor/assets/cache/<kind>/`
    // (`shader/`, `ibl/`, `physics/`, `shapes/`, &hellip;). When a new cache
    // type is added without classification here, this test fails — forcing the
    // change to extend the whitelist *and* its validation rules (e.g. extend
    // the orphan-detection above to cover the new kind).
    //
    // Without this gate, a future cache kind could silently accumulate junk
    // files for years before anyone notices the working tree growing.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, AllCacheFilesMatchKnownPattern)
    {
        const fs::path cacheRoot = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                   "assets" / "cache";
        if (!fs::exists(cacheRoot))
            return; // Fresh clone before first build — nothing to validate.

        // Known cache kinds and the regex each file under them must match.
        // Adding a new cache kind without an entry here intentionally fails
        // this test.
        struct KnownCacheKind
        {
            std::string Subdir;
            std::regex Pattern;
            std::string Description;
        };
        const KnownCacheKind known[] = {
            // Both authored shaders (`<Name>.glsl.cached_*`) and engine-internal
            // procedurally-constructed shaders (`__Foo.cached_*`, e.g.
            // `__Fallback`, `__WarmupBoot` from ShaderWarmup / ShaderLibrary)
            // share this cache directory. `.glsl.` is therefore optional.
            { "shader",
              std::regex(R"(.+?(\.glsl)?\.cached_(opengl|vulkan)\.(vert|frag|comp|tesc|tese|geom|pgr))"),
              "<name>[.glsl].cached_{opengl|vulkan}.{stage|pgr}" },
            { "ibl",
              std::regex(R"([0-9a-fA-F]+_(irradiance|prefilter|brdf)\.iblcache)"),
              "<hash>_{irradiance|prefilter|brdf}.iblcache" },
            // Mesh cache (MeshCache::GetMeshCachePath):
            // `<sanitized_prefix>_<prefixFnv16hex><sourcePathFnv16hex>.omesh`.
            // Sanitized prefix can be empty (for the default static cache), so
            // the leading group permits zero characters; the two hashes are
            // each 16 uppercase hex digits.
            { "mesh",
              std::regex(R"([A-Za-z0-9_-]*_[0-9A-F]{32}\.omesh)"),
              "[<prefix>]_<prefix-hash><path-hash>.omesh" },
            // Animation cache (MeshCache::GetAnimationCachePath): just the
            // source-path hash with the .oanim extension.
            { "animation",
              std::regex(R"([0-9A-F]{16}\.oanim)"),
              "<path-hash>.oanim" },
            // physics/ and shapes/ are reserved for future caches. Until they
            // grow content with a stable filename pattern, any file appearing
            // there is unclassified and fails the test below.
        };

        std::vector<Failure> unclassified;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(cacheRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;

            const fs::path relToCache = fs::relative(entry.path(), cacheRoot, ec);
            if (ec)
            {
                ec.clear();
                continue;
            }

            // First component identifies the kind (e.g. "shader" in "shader/opengl/Foo.glsl.cached_opengl.frag").
            const fs::path firstComponent = *relToCache.begin();
            const std::string kindName = firstComponent.generic_string();
            const std::string filename = entry.path().filename().generic_string();

            auto it = std::ranges::find_if(known,
                                           [&](const KnownCacheKind& k)
                                           { return k.Subdir == kindName; });
            if (it == std::end(known))
            {
                unclassified.push_back({
                    entry.path().generic_string(),
                    "cache subdirectory '" + kindName +
                        "' has no entry in the known-cache whitelist (extend AllCacheFilesMatchKnownPattern).",
                });
                continue;
            }
            if (!std::regex_match(filename, it->Pattern))
            {
                unclassified.push_back({
                    entry.path().generic_string(),
                    "file does not match expected pattern for '" + kindName +
                        "' cache (" + it->Description + ").",
                });
            }
        }

        if (!unclassified.empty())
        {
            std::ostringstream oss;
            oss << unclassified.size() << " unclassified cache file(s):\n";
            for (const auto& f : unclassified)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            oss << "\nTo fix: either delete the offending file (if stray), or "
                << "extend the whitelist in AssetContentValidityTest.cpp"
                << " with a regex covering the new cache kind.\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Dead-shader detector: every production .glsl shader must be referenced
    // from source code by stem (e.g. "Basic3D") or filename ("Basic3D.glsl").
    //
    // Catches the "shader gets replaced but the old file isn't deleted" class
    // of drift — e.g. when a feature is reimplemented (selection outline:
    // PostProcess_SelectionOutline.glsl → JumpFlood_*.glsl) and the old shader
    // stays around as zombie content. The cache-evidence approach (every
    // shader has at least one `.cached_*` file) was tried first and rejected:
    // it false-positives on conditional shaders (debug viz, fallback paths)
    // that are real but only compile when their code path fires.
    //
    // Caveat: a stem appearing only inside a code comment will register as
    // "referenced" — false negative. The point is to catch truly-orphan files,
    // not to verify the load path lives.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, EveryProductionShaderIsReferencedFromSource)
    {
        const fs::path repoRoot = fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        const fs::path shaderRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(shaderRoot)) << "Shader root missing: " << shaderRoot;

        // Enumerate production .glsl files (matches the runtime convention:
        // top-level shaders + compute/, excluding include/ and tests/).
        std::vector<fs::path> sources;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(shaderRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".glsl")
                continue;
            const std::string rel = fs::relative(entry.path(), shaderRoot, ec).generic_string();
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (rel.starts_with("include/") || rel.starts_with("tests/"))
                continue;
            sources.push_back(entry.path());
        }
        ASSERT_GE(sources.size(), 1u) << "Enumerated zero production shaders.";

        // Slurp source files we'll scan for references. Limit to engine C++
        // sources — looking elsewhere (scenes, scripts) would inflate false-
        // negative noise without catching more dead-shader bugs.
        const fs::path scanRoots[] = {
            repoRoot / "OloEngine" / "src",
            repoRoot / "OloEditor" / "src",
        };
        std::string aggregateSource;
        aggregateSource.reserve(8 * 1024 * 1024);
        for (const auto& root : scanRoots)
        {
            if (!fs::exists(root, ec))
                continue;
            for (auto& entry : fs::recursive_directory_iterator(root, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                if (const std::string ext = entry.path().extension().generic_string(); ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl")
                    continue;
                std::ifstream in(entry.path(), std::ios::binary);
                if (!in)
                    continue;
                std::ostringstream ss;
                ss << in.rdbuf();
                aggregateSource += ss.str();
                aggregateSource += '\n';
            }
        }
        ASSERT_GE(aggregateSource.size(), 1024u) << "Source scan produced no content — paths broken.";

        std::vector<Failure> unreferenced;
        for (const auto& src : sources)
        {
            const std::string filename = src.filename().generic_string(); // "Foo.glsl"
            const std::string stem = src.stem().generic_string();         // "Foo"

            // A shader is "referenced" if either the filename-with-extension
            // (used by `Shader::Create("assets/shaders/Foo.glsl")`) or the
            // bare stem (used by `ShaderLibrary.Get("Foo")`) appears in the
            // scanned source. Engine-internal procedural shaders (leading
            // double-underscore) have no on-disk source — they wouldn't reach
            // this loop, but document the convention.
            if (aggregateSource.find(filename) != std::string::npos)
                continue;
            if (aggregateSource.find(stem) != std::string::npos)
                continue;

            unreferenced.push_back({
                src.generic_string(),
                "neither '" + filename + "' nor '" + stem + "' appears anywhere "
                                                            "under OloEngine/src or OloEditor/src — shader is orphan content.",
            });
        }

        if (!unreferenced.empty())
        {
            std::ostringstream oss;
            oss << unreferenced.size() << " orphan shader(s):\n";
            for (const auto& f : unreferenced)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            oss << "\nTypical cause: a feature was reimplemented and the old "
                << "shader file wasn't deleted alongside its load-call. Fix by "
                << "deleting the orphan .glsl source (and its `.cached_*` "
                << "siblings, which ShaderCacheEntriesAllHaveLiveGlslSources "
                << "will then flag for cleanup).\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // .oloproj path discipline — no absolute paths
    //
    // A `.oloproj` whose paths leak a developer's machine-local layout
    // (`C:/Users/.../Scenes/Foo.olo`) is unportable: everyone else's
    // editor would resolve to "file not found" on open. Catches
    // accidental absolute paths from drag-and-drop in the editor's
    // project-settings panel — relative paths are the discipline.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SandboxProjectFilePathsAreAllRelative)
    {
        const fs::path projectFile = fs::path{ OLO_TEST_EDITOR_ROOT } /
                                     "SandboxProject" / "Sandbox.oloproj";
        ASSERT_TRUE(fs::exists(projectFile));

        std::string reason;
        auto node = ParseYAMLFile(projectFile, reason);
        ASSERT_TRUE(node) << "Sandbox.oloproj failed to parse: " << reason;
        const YAML::Node project = (*node)["Project"];
        ASSERT_TRUE(project);

        // Fields known to be path-bearing. We only check the on-disk
        // YAML directly (not the post-load resolved absolute paths,
        // which the deserializer intentionally builds).
        const std::array<const char*, 3> pathFields = {
            "StartScene",
            "AssetDirectory",
            "ScriptModulePath",
        };

        std::vector<Failure> failures;
        for (const char* key : pathFields)
        {
            const YAML::Node n = project[key];
            if (!n || !n.IsScalar())
                continue;
            const std::string value = n.as<std::string>();
            if (value.empty())
                continue;

            // Heuristics for absolute paths:
            //   - POSIX absolute: starts with '/'.
            //   - Windows drive: char[0] is a letter and char[1] is ':'.
            //   - UNC: starts with '\\\\'.
            const bool isPosixAbs = value.front() == '/';
            const bool isWinDrive = value.size() >= 2 &&
                                    std::isalpha(static_cast<u8>(value[0])) &&
                                    value[1] == ':';
            const bool isUnc = value.size() >= 2 && value[0] == '\\' && value[1] == '\\';

            if (isPosixAbs || isWinDrive || isUnc)
            {
                failures.push_back({
                    projectFile.generic_string(),
                    std::string("Project.") + key + " = '" + value +
                        "' looks absolute — should be project-relative for portability.",
                });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " absolute path(s) in Sandbox.oloproj:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // DISABLED_RebaseAssetRegistry — manual cleanup helper
    //
    // Mirrors the existing `OLOENGINE_GOLDEN_REBASE=1` pattern for golden
    // image baselines. The asset registry is regenerated automatically by
    // OloEditor on next launch from on-disk asset discovery; when a
    // developer can't launch the editor (CI, headless dev box), this
    // helper rebuilds the registry to match disk:
    //   1. drops stale entries whose paths no longer resolve, and
    //   2. adds supported on-disk assets the registry is missing — the
    //      clean-rebuild path the older "only removes" version invited.
    // The two passes together make `EverySupportedAssetOnDiskIsInTheRegistry`
    // and the stale-path checks pass again after assets are added/moved.
    //
    // Disabled by default — only runs when explicitly filtered in:
    //   --gtest_also_run_disabled_tests
    //   --gtest_filter=AssetContentValidity.DISABLED_RebaseAssetRegistry
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, DISABLED_RebaseAssetRegistry)
    {
        const fs::path projectRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject";
        const fs::path registryPath = projectRoot / "AssetRegistry.oar";
        ASSERT_TRUE(fs::exists(registryPath));

        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(registryPath));

        // Pass 1 — drop stale entries whose file no longer resolves.
        const auto allAssets = registry.GetAllAssets();
        u32 removedCount = 0;
        for (const auto& metadata : allAssets)
        {
            const fs::path resolved = projectRoot / metadata.FilePath;
            std::error_code ec;
            if (!fs::exists(resolved, ec))
            {
                registry.RemoveAsset(metadata.Handle);
                ++removedCount;
            }
        }

        // Pass 2 — reconcile supported on-disk assets with the registry.
        // Mirror EverySupportedAssetOnDiskIsInTheRegistry's project-relative,
        // forward-slash path comparison so the two stay in lockstep. A file
        // whose path differs from its registry entry only by case (Windows /
        // NTFS is case-insensitive) is RE-CASED in place rather than minting a
        // duplicate — otherwise the helper would leave the stale wrong-case
        // entry that SandboxAssetRegistryPathsMatchOnDiskCasing flags.
        auto toLower = [](std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };

        std::set<std::string> registeredPaths;                      // exact-case generic
        std::unordered_map<std::string, AssetMetadata> byLowerPath; // case-folded -> metadata
        for (const auto& metadata : registry.GetAllAssets())
        {
            const std::string p = metadata.FilePath.generic_string();
            registeredPaths.insert(p);
            byLowerPath.emplace(toLower(p), metadata);
        }

        const fs::path assetsRoot = projectRoot / "Assets";
        u32 addedCount = 0;
        u32 recasedCount = 0;
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(assetsRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (!AssetExtensions::IsExtensionSupported(ext))
                continue;
            const std::string relative = fs::relative(entry.path(), projectRoot, ec).generic_string();
            if (ec || relative.empty() || registeredPaths.contains(relative))
                continue; // already registered with exact casing

            if (auto it = byLowerPath.find(toLower(relative)); it != byLowerPath.end())
            {
                // Registered under different casing — re-case in place, don't duplicate.
                AssetMetadata fixed = it->second;
                fixed.FilePath = relative;
                registry.UpdateMetadata(fixed.Handle, fixed);
                registeredPaths.insert(relative);
                it->second.FilePath = relative;
                ++recasedCount;
                continue;
            }

            AssetMetadata md;
            md.Handle = UUID(); // fresh random handle, editor-style
            md.Type = AssetExtensions::GetAssetTypeFromExtension(ext);
            md.FilePath = relative; // stored generic (forward-slash)
            md.Status = AssetStatus::None;
            registry.AddAsset(md);
            registeredPaths.insert(relative);
            byLowerPath.emplace(toLower(relative), md);
            ++addedCount;
        }

        ASSERT_TRUE(registry.Serialize(registryPath));
        std::cout << "Rebased AssetRegistry.oar: removed " << removedCount
                  << " stale entries, added " << addedCount << " new on-disk asset(s), recased "
                  << recasedCount << " path(s); " << registry.GetAssetCount() << " total.\n";
    }

    // -------------------------------------------------------------------------
    // Scene ScriptField names match C# field declarations
    //
    // Every entry in a scene's `ScriptComponent.ScriptFields` is supposed
    // to override a `public <Type> <Name>` field declared in the C#
    // script class. If the field is renamed on the C# side but the scene
    // YAML keeps the old name, the engine's script-binding system
    // silently drops the override — the developer's tweak in the editor
    // panel doesn't apply, and the script runs with the C# default.
    //
    // We verify: every `Name` in `ScriptFields` matches a `public ...
    // Name [;=]` declaration somewhere in the corresponding .cs file.
    // We deliberately don't do strict type-matching (the C# `float` /
    // YAML `Float` mapping has many cases); name-presence is the most
    // common rename-drift case and the simplest sharp invariant.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SceneScriptFieldNamesMatchCSharpDeclarations)
    {
        const fs::path assetsRoot = SandboxAssetsRoot();
        const fs::path sourceDir = assetsRoot / "Scripts" / "Source";
        const auto scenes = EnumerateFilesByExtension(assetsRoot / "Scenes", ".olo");
        std::vector<Failure> failures;

        for (const auto& scenePath : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(scenePath, reason);
            if (!node)
                continue;
            const YAML::Node entities = (*node)["Entities"];
            if (!entities || !entities.IsSequence())
                continue;

            for (sizet i = 0; i < entities.size(); ++i)
            {
                const YAML::Node ent = entities[i];
                if (!ent.IsMap())
                    continue;
                const YAML::Node sc = ent["ScriptComponent"];
                if (!sc || !sc.IsMap())
                    continue;
                const YAML::Node fields = sc["ScriptFields"];
                if (!fields || !fields.IsSequence() || fields.size() == 0)
                    continue;

                // Resolve the .cs file by class name.
                std::string fullName = sc["ClassName"] ? sc["ClassName"].as<std::string>() : "";
                if (fullName.empty())
                    continue;
                const auto dot = fullName.rfind('.');
                const std::string className = (dot == std::string::npos)
                                                  ? fullName
                                                  : fullName.substr(dot + 1);
                const fs::path csFile = sourceDir / (className + ".cs");
                if (std::error_code ec; !fs::exists(csFile, ec))
                    continue;

                std::ifstream sf(csFile, std::ios::binary);
                std::ostringstream buf;
                buf << sf.rdbuf();
                const std::string csSource = buf.str();

                for (sizet j = 0; j < fields.size(); ++j)
                {
                    const YAML::Node field = fields[j];
                    if (!field.IsMap())
                        continue;
                    const YAML::Node nameNode = field["Name"];
                    if (!nameNode || !nameNode.IsScalar())
                        continue;
                    const std::string fieldName = nameNode.as<std::string>();
                    if (fieldName.empty())
                        continue;

                    // Match `public <Type> <FieldName> [=;]` — Type is
                    // any single identifier (we don't enforce it).
                    const std::regex pat{
                        R"(public\s+\w+\s+)" + fieldName + R"(\s*[;=])",
                    };
                    if (!std::regex_search(csSource, pat))
                    {
                        failures.push_back({
                            scenePath.generic_string(),
                            "Entities[" + std::to_string(i) +
                                "].ScriptComponent.ScriptFields[" + std::to_string(j) +
                                "].Name = '" + fieldName + "' but " + csFile.filename().generic_string() +
                                " has no `public <Type> " + fieldName + "` declaration — "
                                                                        "scene override will be silently dropped.",
                        });
                        continue;
                    }

                    // If the YAML Type is one of the engine's known
                    // ScriptFieldType enum values, verify the C# side
                    // declares the matching C# type. Catches "renamed
                    // the type on C# side, scene YAML still says old."
                    const YAML::Node typeNode = field["Type"];
                    if (!typeNode || !typeNode.IsScalar())
                        continue;
                    const std::string yamlType = typeNode.as<std::string>();

                    // Map: YAML ScriptFieldType ↔ C# type token. Mirrors
                    // ScriptEngine.h's ScriptFieldType enum.
                    static const std::unordered_map<std::string, std::vector<std::string>> kTypeMap = {
                        { "Float", { "float" } },
                        { "Double", { "double" } },
                        { "Bool", { "bool" } },
                        { "Char", { "char" } },
                        { "Byte", { "byte" } },
                        { "Short", { "short" } },
                        { "Int", { "int" } },
                        { "Long", { "long" } },
                        { "UByte", { "byte" } }, // unsigned byte aliased as `byte` in C#
                        { "UShort", { "ushort" } },
                        { "UInt", { "uint" } },
                        { "ULong", { "ulong" } },
                        { "Vector2", { "Vector2" } },
                        { "Vector3", { "Vector3" } },
                        { "Vector4", { "Vector4" } },
                        { "Entity", { "Entity" } },
                    };

                    auto it = kTypeMap.find(yamlType);
                    if (it == kTypeMap.end())
                        continue; // unknown ScriptFieldType — skip (e.g. None/0 sentinel)

                    bool typeMatches = false;
                    for (const auto& csType : it->second)
                    {
                        const std::regex typedPat{
                            R"(public\s+)" + csType + R"(\s+)" + fieldName + R"(\s*[;=])",
                        };
                        if (std::regex_search(csSource, typedPat))
                        {
                            typeMatches = true;
                            break;
                        }
                    }
                    if (!typeMatches)
                    {
                        failures.push_back({
                            scenePath.generic_string(),
                            "Entities[" + std::to_string(i) +
                                "].ScriptComponent.ScriptFields[" + std::to_string(j) +
                                "] (Name='" + fieldName + "', Type='" + yamlType +
                                "') does not match the C# declaration in " +
                                csFile.filename().generic_string() +
                                " — field exists but with a different type. Editor "
                                "override values will be dropped.",
                        });
                    }
                }
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " ScriptField name mismatch(es):\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // EditorPreferences.yaml
    //
    // The editor reads this on startup; corrupt YAML or a missing
    // top-level `EditorPreferences` key would make the editor either
    // silently revert to defaults (frustrating) or, depending on the
    // deserializer's robustness, hard-crash. Validate the on-disk file
    // structurally.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SandboxEditorPreferencesAreStructurallyValid)
    {
        const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } /
                              "SandboxProject" / "EditorPreferences.yaml";
        ASSERT_TRUE(fs::exists(path))
            << "EditorPreferences.yaml not found at " << path.string();

        std::string reason;
        auto node = ParseYAMLFile(path, reason);
        ASSERT_TRUE(node) << "EditorPreferences.yaml failed to parse: " << reason;

        const YAML::Node prefs = (*node)["EditorPreferences"];
        ASSERT_TRUE(prefs)
            << "EditorPreferences.yaml missing top-level 'EditorPreferences' key.";
        EXPECT_TRUE(prefs.IsMap())
            << "'EditorPreferences' must be a YAML map, not a sequence or scalar.";
    }

    // -------------------------------------------------------------------------
    // InputActions.yaml — optional input action map
    //
    // `Project::GetInputActionMapPath()` resolves to
    // `<project>/Config/InputActions.yaml`. The engine loads it on
    // project open if present; missing is fine (input actions are an
    // optional engine feature). If present, malformed YAML or a missing
    // top-level `InputActions` key crashes the editor's input subsystem
    // on startup.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // Path-based asset references in scenes resolve on disk
    //
    // Some asset references in scene YAML are stored as raw paths
    // (legacy Hazel-derived content) rather than UUIDs through the
    // registry: `AudioSourceComponent.Filepath`, `MeshComponent.FilePath`,
    // `TextComponent.FontPath`, `SpriteRendererComponent.TexturePath`,
    // `EnvironmentMapComponent.FilePath`, etc.
    //
    // The conventions vary by content era — newer engine code resolves
    // paths against `Project::GetAssetDirectory()`, but older content
    // still uses paths relative to OloEditor's working directory
    // (`assets/models/...`) or the repo root (`../../assets/...`). We
    // try multiple candidate roots and accept if any resolves; flagging
    // only paths that don't resolve under ANY of them.
    //
    // Empty values are skipped (the field is set to empty when no asset
    // is bound, which is valid).
    // -------------------------------------------------------------------------
    namespace
    {
        bool PathResolvesUnderAnyRoot(const std::string& value,
                                      const std::vector<fs::path>& candidateRoots)
        {
            if (value.empty())
                return true; // unset — fine
            std::error_code ec;
            // Absolute path: check directly.
            const fs::path raw{ value };
            if (raw.is_absolute())
                return fs::exists(raw, ec);
            // Relative path: try each candidate root.
            for (const auto& root : candidateRoots)
            {
                if (fs::exists(root / raw, ec))
                    return true;
            }
            return false;
        }
    } // namespace

    TEST(AssetContentValidity, AllSandboxScenePathReferencesResolve)
    {
        const fs::path editorRoot = fs::path{ OLO_TEST_EDITOR_ROOT };
        const fs::path projectRoot = editorRoot / "SandboxProject";
        const fs::path assetsRoot = projectRoot / "Assets";
        const fs::path repoRoot = editorRoot.parent_path();

        const std::vector<fs::path> candidateRoots = {
            assetsRoot,  // Project::GetAssetDirectory() convention
            editorRoot,  // OloEditor working-dir convention (`assets/...`)
            projectRoot, // Project::GetProjectDirectory() convention
            repoRoot,    // `../../assets/...` repo-root convention
        };

        // Known path-bearing field name set (substring match — any key
        // ending in Path / Filepath / File / ScriptFile counts).
        auto isPathField = [](const std::string& key)
        {
            return key.ends_with("Path") || key == "Filepath" ||
                   key == "FilePath" || key == "ScriptFile";
        };

        std::vector<Failure> failures;
        std::function<void(const YAML::Node&, const std::string&, const std::string&)> walk;
        walk = [&](const YAML::Node& n, const std::string& path, const std::string& currentKey)
        {
            if (n.IsScalar())
            {
                if (!isPathField(currentKey))
                    return;
                const std::string value = n.as<std::string>();
                if (value.empty())
                    return;
                // Cubemap folder references (Skybox folder) are
                // directories; the check works equally for dirs.
                if (!PathResolvesUnderAnyRoot(value, candidateRoots))
                {
                    failures.push_back({
                        path,
                        currentKey + " = '" + value + "' does not resolve under any "
                                                      "candidate root (Project Assets/, OloEditor/, repo root). "
                                                      "The runtime asset loader would silently fail to find it.",
                    });
                }
            }
            else if (n.IsSequence())
            {
                for (sizet i = 0; i < n.size(); ++i)
                    walk(n[i], path, currentKey);
            }
            else if (n.IsMap())
            {
                for (auto it = n.begin(); it != n.end(); ++it)
                {
                    const std::string key = it->first.as<std::string>("?");
                    walk(it->second, path, key);
                }
            }
            else
            {
                // No additional handling required.
            }
        };

        const auto scenes = EnumerateFilesByExtension(assetsRoot / "Scenes", ".olo");
        for (const auto& scenePath : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(scenePath, reason);
            if (!node)
                continue;
            walk(*node, scenePath.generic_string(), "");
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " scene path reference(s) don't resolve:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Cross-platform path normalization in scene YAML
    //
    // OloEngine builds on Windows, Linux, and WSL (per CLAUDE.md). A
    // scene file saved with a Windows-style backslash path
    // (`ScriptFile: Scripts\LuaScripts\X.lua`) parses fine on Windows
    // but resolves to a single literal filename on Linux —
    // `Scripts\LuaScripts\X.lua` becomes a non-existent file. Symptom
    // on Linux: "the script binding is silently empty even though
    // Windows shows it working."
    //
    // We walk every parsed YAML scalar in each scene and assert none
    // contain a backslash. yaml-cpp normalizes escape sequences during
    // parsing, so any backslash that survives is an unescaped path
    // separator that's going to break on POSIX file systems.
    // -------------------------------------------------------------------------
    namespace
    {
        void WalkYAMLForBackslashes(const YAML::Node& node,
                                    const std::string& sceneRelPath,
                                    const std::string& keyPath,
                                    std::vector<Failure>& out)
        {
            if (node.IsScalar())
            {
                const std::string value = node.as<std::string>();
                if (value.find('\\') != std::string::npos)
                {
                    out.push_back({
                        sceneRelPath,
                        "scalar at '" + keyPath + "' = '" + value +
                            "' contains '\\' — Windows-style path separator "
                            "that won't resolve on Linux/WSL.",
                    });
                }
            }
            else if (node.IsSequence())
            {
                for (sizet i = 0; i < node.size(); ++i)
                    WalkYAMLForBackslashes(
                        node[i], sceneRelPath,
                        keyPath + "[" + std::to_string(i) + "]", out);
            }
            else if (node.IsMap())
            {
                for (auto it = node.begin(); it != node.end(); ++it)
                {
                    const std::string key = it->first.as<std::string>("?");
                    WalkYAMLForBackslashes(
                        it->second, sceneRelPath,
                        keyPath.empty() ? key : (keyPath + "." + key), out);
                }
            }
            else
            {
                // No additional handling required.
            }
        }
    } // namespace

    TEST(AssetContentValidity, AllSandboxScenePathsUseForwardSlashes)
    {
        const auto scenes = EnumerateFilesByExtension(SandboxAssetsRoot() / "Scenes", ".olo");
        std::vector<Failure> failures;

        for (const auto& path : scenes)
        {
            std::string reason;
            auto node = ParseYAMLFile(path, reason);
            if (!node)
                continue;
            WalkYAMLForBackslashes(*node, path.generic_string(), "", failures);
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " scene scalar(s) contain Windows backslashes:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Cross-platform path normalization across all YAML asset types
    //
    // Companion to the scenes-only check above: prefab + dialogue +
    // item + quest + shader-graph assets are also YAML and can also
    // ship Windows-style backslash paths that would silently break on
    // Linux/WSL. Same walker, broader asset surface.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, AllSandboxAssetYAMLPathsUseForwardSlashes)
    {
        const fs::path assetsRoot = SandboxAssetsRoot();
        struct AssetTypeRoot
        {
            const char* Subdir;
            const char* Extension;
        };
        const std::array<AssetTypeRoot, 7> roots = { {
            { "Items", ".oloitem" },
            { "Quests", ".oloquest" },
            { "Dialogues", ".olodialogue" },
            { "ShaderGraphs", ".olosg" },
            { "Progression", ".oloxpcurve" },
            { "Progression", ".oloskilltree" },
            { "Progression", ".olocharclass" },
        } };

        std::vector<Failure> failures;
        u32 filesScanned = 0;
        for (const auto& [subdir, ext] : roots)
        {
            const auto files = EnumerateFilesByExtension(assetsRoot / subdir, ext);
            filesScanned += static_cast<u32>(files.size());
            for (const auto& path : files)
            {
                std::string reason;
                auto node = ParseYAMLFile(path, reason);
                if (!node)
                    continue;
                WalkYAMLForBackslashes(*node, path.generic_string(), "", failures);
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " asset scalar(s) contain Windows backslashes:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }

        EXPECT_GT(filesScanned, 0u)
            << "No asset YAML files were scanned — test root broken?";
    }

    TEST(AssetContentValidity, SandboxInputActionsAreStructurallyValid)
    {
        const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } /
                              "SandboxProject" / "Config" / "InputActions.yaml";
        if (!fs::exists(path))
            return; // optional asset — current sandbox doesn't ship one.

        std::string reason;
        auto node = ParseYAMLFile(path, reason);
        ASSERT_TRUE(node) << "InputActions.yaml failed to parse: " << reason;

        // Accept either a top-level `InputActions` (canonical) or
        // `ActionMap` (engine has historically used both — match either).
        EXPECT_TRUE((*node)["InputActions"] || (*node)["ActionMap"])
            << "InputActions.yaml missing top-level 'InputActions' (or 'ActionMap') key.";
    }

    // -------------------------------------------------------------------------
    // Progression data assets (issue #635): every shipped
    // .oloxpcurve / .oloskilltree / .olocharclass under Assets/Progression/
    // must load through the REAL serializer string path (the same parse the
    // AssetManager runs, minus file I/O) and pass structural validation. A
    // broken sample here surfaces as "class/skill tree silently missing" the
    // moment a user wires the asset in the editor.
    // -------------------------------------------------------------------------
    TEST(AssetContentValidity, SandboxExperienceCurvesAreStructurallyValid)
    {
        const auto files = EnumerateFilesByExtension(SandboxAssetsRoot() / "Progression", ".oloxpcurve");
        ASSERT_GE(files.size(), 1u)
            << "no .oloxpcurve files found under Assets/Progression — sandbox content moved?";

        const ExperienceCurveSerializer serializer;
        for (const auto& path : files)
        {
            auto curve = Ref<ExperienceCurve>::Create();
            EXPECT_TRUE(serializer.TestDeserializeFromYAML(ReadFileToString(path), curve))
                << path.generic_string() << ": failed to deserialize as an ExperienceCurve";
            EXPECT_GE(curve->GetXPForLevelUp(1), 1)
                << path.generic_string() << ": the loaded curve must satisfy the >= 1 XP floor";
            EXPECT_GE(curve->GetMaxLevel(), 1) << path.generic_string();
            EXPECT_LE(curve->GetMaxLevel(), 1000)
                << path.generic_string() << ": MaxLevel outside the sanitized [1, 1000] range";
        }
    }

    TEST(AssetContentValidity, SandboxSkillTreesAreStructurallyValid)
    {
        const auto files = EnumerateFilesByExtension(SandboxAssetsRoot() / "Progression", ".oloskilltree");
        ASSERT_GE(files.size(), 1u)
            << "no .oloskilltree files found under Assets/Progression — sandbox content moved?";

        const SkillTreeDatabaseSerializer serializer;
        for (const auto& path : files)
        {
            auto tree = Ref<SkillTreeDatabase>::Create();
            ASSERT_TRUE(serializer.TestDeserializeFromYAML(ReadFileToString(path), tree))
                << path.generic_string()
                << ": failed to deserialize (duplicate/empty node id, dangling prerequisite, or cycle)";

            std::string error;
            EXPECT_TRUE(tree->Validate(&error))
                << path.generic_string() << ": loaded tree fails validation: " << error;
            EXPECT_FALSE(tree->m_Nodes.empty())
                << path.generic_string() << ": a shipped skill tree with zero nodes is dead content";
            for (const auto& node : tree->m_Nodes)
            {
                EXPECT_NE(tree->FindNode(node.NodeID), nullptr)
                    << path.generic_string() << ": node '" << node.NodeID
                    << "' not findable — the serializer did not RebuildIndex() on load";
            }
        }
    }

    TEST(AssetContentValidity, SandboxCharacterClassesAreStructurallyValid)
    {
        const auto files = EnumerateFilesByExtension(SandboxAssetsRoot() / "Progression", ".olocharclass");
        ASSERT_GE(files.size(), 1u)
            << "no .olocharclass files found under Assets/Progression — sandbox content moved?";

        const CharacterClassDatabaseSerializer serializer;
        for (const auto& path : files)
        {
            auto db = Ref<CharacterClassDatabase>::Create();
            ASSERT_TRUE(serializer.TestDeserializeFromYAML(ReadFileToString(path), db))
                << path.generic_string() << ": failed to deserialize (duplicate/empty class id?)";

            std::string error;
            EXPECT_TRUE(db->Validate(&error))
                << path.generic_string() << ": loaded database fails validation: " << error;
            EXPECT_FALSE(db->m_Classes.empty())
                << path.generic_string() << ": a shipped class database with zero classes is dead content";
            for (const auto& classDef : db->m_Classes)
            {
                EXPECT_NE(db->FindClass(classDef.ClassID), nullptr)
                    << path.generic_string() << ": class '" << classDef.ClassID
                    << "' not findable — the serializer did not RebuildIndex() on load";
            }
        }
    }
} // namespace OloEngine::Tests
