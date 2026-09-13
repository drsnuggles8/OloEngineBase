// OLO_TEST_LAYER: unit

// The prefab automation commands (issue #1129) against a real Project, a real
// EditorAssetManager in a throwaway directory, and a real CommandHistory.
//
// The two cases the issue's acceptance criteria name are marked ACCEPTANCE
// below. They are why this file exists:
//
//   (a) instantiate -> override a field -> apply -> every OTHER instance
//       reflects it, and ONE undo puts all of them back. That is a cross-object
//       undo: the prefab asset's own Scene, the .oloprefab on disk, and every
//       re-synced instance in the active scene, in one entry.
//   (b) the override query answers "what has this instance diverged on" without
//       opening the editor -- field by field, not just "some component changed".
//
// Everything else guards a way one of those could pass while still losing
// somebody's data, or a way a nested prefab could be silently mis-applied.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationComponentRegistry.h"
#include "Automation/AutomationPrefabCommands.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpServer.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/Scene.h"
#include "TestTempDir.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace OloEngine::Automation::Tests
{
    namespace
    {
        using Json = nlohmann::json;

        // No editor and no transport, but a REAL CommandHistory and a real scene:
        // MarshalRead runs the job inline, which is what the commands' own
        // threading contract allows.
        class PrefabHost final : public IAutomationHost
        {
          public:
            PrefabHost()
            {
                m_Context.GetActiveScene = [this]()
                { return SceneAvailable ? ActiveScene : Ref<OloEngine::Scene>{}; };
                m_Context.GetCommandHistory = [this]() -> CommandHistory*
                { return HistoryAvailable ? &History : nullptr; };
                m_Context.InvalidateEntityReferences = [this]()
                { ++SelectionClears; };
            }

            [[nodiscard]] const MCP::EditorMcpContext& Context() const override
            {
                return m_Context;
            }

            [[nodiscard]] bool IsCurrentCallCancelled() const override
            {
                return false;
            }

            [[nodiscard]] bool PublishArtifact(AutomationArtifact) override
            {
                return false;
            }

            Ref<OloEngine::Scene> ActiveScene;
            CommandHistory History;
            bool HistoryAvailable = true;
            bool SceneAvailable = true;
            int SelectionClears = 0;

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& job, std::chrono::milliseconds) override
            {
                return job();
            }
            void EmitProgressUpdate(f64, f64, const std::string&) const override {}

          private:
            MCP::EditorMcpContext m_Context;
        };

        std::string ReadAll(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        }

        std::string Id(Entity entity)
        {
            return std::to_string(static_cast<u64>(entity.GetUUID()));
        }

        bool Contains(const Json& array, const std::string& value)
        {
            return std::find(array.begin(), array.end(), Json(value)) != array.end();
        }
    } // namespace

    class AutomationPrefabCommandsTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Project = OloEngine::Tests::TempDir("prefabcmd");
            std::error_code ec;
            std::filesystem::create_directories(m_Project / "Assets" / "Prefabs", ec);
            {
                std::ofstream config(m_Project / "Test.oloproj", std::ios::binary | std::ios::trunc);
                config << "Project:\n"
                          "  Name: PrefabCommandsTest\n"
                          "  StartScene: \"\"\n"
                          "  AssetDirectory: \"Assets\"\n"
                          "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(m_Project / "Test.oloproj"));

            auto manager = Ref<EditorAssetManager>::Create();
            // startFileWatcher=false: the watcher thread outlives the case and
            // races the next one's manager.
            manager->Initialize(false);
            Project::SetAssetManager(manager);

            m_Host.ActiveScene = Ref<OloEngine::Scene>::Create();
            RegisterPrefabCommands(m_Registry);
        }

        // Project::Load and SetAssetManager install PROCESS-GLOBAL state and this
        // binary runs many cases in one process; leaving them set would hand the
        // next fixture a project rooted in this case's temp directory.
        void TearDown() override
        {
            m_Host.ActiveScene = nullptr;
            Project::Unload();
            std::error_code ec;
            std::filesystem::remove_all(m_Project, ec);
        }

        AutomationInvocation Invoke(const std::string& command, const Json& args = Json::object())
        {
            return m_Registry.Invoke(m_Host, command, args, AutomationWriteConsent::Granted);
        }

        Json Ok(const std::string& command, const Json& args)
        {
            const auto invocation = Invoke(command, args);
            EXPECT_TRUE(invocation.Ran()) << invocation.Message;
            EXPECT_FALSE(invocation.Result.IsError) << invocation.Result.Content.dump(2);
            return invocation.Result.StructuredContent.is_object() ? invocation.Result.StructuredContent
                                                                   : Json::object();
        }

        std::string Failure(const std::string& command, const Json& args)
        {
            const auto invocation = Invoke(command, args);
            EXPECT_TRUE(invocation.Ran()) << invocation.Message;
            EXPECT_TRUE(invocation.Result.IsError) << invocation.Result.StructuredContent.dump(2);
            return invocation.Result.Content.empty() ? std::string{}
                                                     : invocation.Result.Content.at(0).value("text", std::string{});
        }

        // A parent with one child, both carrying a SpriteRendererComponent, written
        // out as a prefab. `m_Source` is the now-linked source root.
        std::string MakePrefab(const std::string& path)
        {
            Entity root = m_Host.ActiveScene->CreateEntity("Turret");
            Entity child = m_Host.ActiveScene->CreateEntity("Gun");
            child.SetParent(root);
            root.AddComponent<SpriteRendererComponent>();
            child.AddComponent<SpriteRendererComponent>();
            root.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
            m_Source = root;
            return Ok("olo_prefab_create", { { "entity", Id(root) }, { "path", path } })
                .value("prefab", std::string{});
        }

        // The same prefab, with the source subtree removed and the history reset,
        // so a case can count undo entries from zero and every instance in the
        // scene is one it made on purpose.
        std::string MakeDetachedPrefab(const std::string& path = "Assets/Prefabs/Turret.oloprefab")
        {
            const std::string handle = MakePrefab(path);
            m_Host.ActiveScene->DestroyEntityAndChildren(m_Source);
            m_Source = {};
            m_Host.History.Clear();
            return handle;
        }

        Entity Find(const std::string& id) const
        {
            const auto entity = m_Host.ActiveScene->TryGetEntityWithUUID(UUID(std::stoull(id)));
            return entity ? *entity : Entity{};
        }

        Entity Instantiate(const std::string& handle, const Json& extra = Json::object())
        {
            Json args = extra;
            args["prefab"] = handle;
            return Find(Ok("olo_prefab_instantiate", args).at("entity"));
        }

        Ref<Prefab> Load(const std::string& handle) const
        {
            return AssetManager::GetAsset<Prefab>(AssetHandle(std::stoull(handle)));
        }

        // Undoing to the bottom of the stack destroys and recreates the entities an
        // instantiate made, which invalidates every held Entity handle -- so depth
        // is asserted through the stack's own top description rather than by
        // walking it.
        std::string TopUndo() const
        {
            return m_Host.History.CanUndo() ? m_Host.History.GetUndoDescription() : std::string{};
        }

        std::filesystem::path PrefabPath(const std::string& name) const
        {
            return m_Project / "Assets" / "Prefabs" / name;
        }

        std::filesystem::path m_Project;
        PrefabHost m_Host;
        AutomationRegistry m_Registry;
        Entity m_Source;
    };

    // Prefab.cpp's copyable list and the generated component table are two
    // independently maintained spellings of the same set. A name in one and not
    // the other would drop that component out of every prefab operation with no
    // error anywhere -- which is how LuaScriptComponent went missing until #643.
    TEST_F(AutomationPrefabCommandsTest, EveryCopyableComponentNameResolvesToAKnownType)
    {
        const auto& names = Prefab::CopyableComponentNames();
        EXPECT_GT(names.size(), 50u);
        for (const auto& name : names)
        {
            const auto* type = FindComponentType(name);
            ASSERT_NE(type, nullptr) << name << " is in Prefab::CopyableComponentNames but is not a component type.";
            EXPECT_TRUE(type->Authored) << name << " is copied into prefabs but is not an authored type.";
            EXPECT_TRUE(static_cast<bool>(type->Capture)) << name << " cannot be captured, so prefab undo cannot hold it.";
            EXPECT_TRUE(static_cast<bool>(type->Remove)) << name << " cannot be removed, so prefab undo cannot clear it.";
        }
    }

    TEST_F(AutomationPrefabCommandsTest, ConsentAndEditModeAreRequiredBeforeMutating)
    {
        const std::string handle = MakeDetachedPrefab();

        const auto denied = m_Registry.Invoke(m_Host, "olo_prefab_instantiate", Json{ { "prefab", handle } });
        EXPECT_EQ(denied.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
        EXPECT_FALSE(m_Host.History.CanUndo());

        m_Host.HistoryAvailable = false;
        EXPECT_NE(Failure("olo_prefab_instantiate", { { "prefab", handle } }).find("Edit mode"), std::string::npos);
        m_Host.HistoryAvailable = true;

        m_Host.SceneAvailable = false;
        EXPECT_FALSE(Failure("olo_prefab_overrides", { { "entity", "1" } }).empty());
    }

    TEST_F(AutomationPrefabCommandsTest, CreateLinksTheSourceSubtreeAndUndoUnlinksAndDeletesTheFile)
    {
        const std::string handle = MakePrefab("Assets/Prefabs/Turret.oloprefab");
        ASSERT_FALSE(handle.empty());
        const std::filesystem::path file = PrefabPath("Turret.oloprefab");
        ASSERT_TRUE(std::filesystem::exists(file));
        const std::string written = ReadAll(file);
        EXPECT_FALSE(written.empty());

        // Both the root and the child are linked, each to its OWN copy -- not both
        // to the prefab root, which is what a fallback resolution would produce and
        // what would later apply a child's override onto the root.
        Entity child = *m_Host.ActiveScene->TryGetEntityWithUUID(m_Source.Children().front());
        ASSERT_TRUE(m_Source.HasComponent<PrefabComponent>());
        ASSERT_TRUE(child.HasComponent<PrefabComponent>());
        const UUID rootSource = m_Source.GetComponent<PrefabComponent>().m_PrefabEntityID;
        const UUID childSource = child.GetComponent<PrefabComponent>().m_PrefabEntityID;
        EXPECT_NE(static_cast<u64>(rootSource), static_cast<u64>(childSource));
        Ref<Prefab> prefab = Load(handle);
        ASSERT_TRUE(prefab);
        EXPECT_EQ(static_cast<u64>(prefab->FindSourceEntity(m_Source).GetUUID()), static_cast<u64>(rootSource));
        EXPECT_EQ(static_cast<u64>(prefab->FindSourceEntity(child).GetUUID()), static_cast<u64>(childSource));
        EXPECT_EQ(prefab->FindSourceEntity(child).GetName(), "Gun");

        ASSERT_EQ(TopUndo(), "Create Prefab");
        m_Host.History.Undo();
        EXPECT_FALSE(m_Host.History.CanUndo()) << "create-from-subtree is ONE undo entry";
        EXPECT_FALSE(m_Source.HasComponent<PrefabComponent>());
        EXPECT_FALSE(child.HasComponent<PrefabComponent>());
        EXPECT_FALSE(std::filesystem::exists(file));
        EXPECT_FALSE(AssetManager::IsAssetValid(AssetHandle(std::stoull(handle))));

        m_Host.History.Redo();
        EXPECT_TRUE(m_Source.HasComponent<PrefabComponent>());
        EXPECT_TRUE(std::filesystem::exists(file));
        EXPECT_EQ(ReadAll(file), written);
        EXPECT_TRUE(AssetManager::IsAssetValid(AssetHandle(std::stoull(handle))));
    }

    TEST_F(AutomationPrefabCommandsTest, InstantiateCreatesTheHierarchyAndUndoRedoRetainsEveryUUID)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity anchor = m_Host.ActiveScene->CreateEntity("Anchor");

        const Json result =
            Ok("olo_prefab_instantiate", { { "prefab", handle }, { "parent", Id(anchor) }, { "name", "TurretA" } });
        ASSERT_EQ(result.at("children").size(), 1u);
        EXPECT_EQ(result.at("entityCount").get<u64>(), 2u);
        EXPECT_TRUE(result.at("nestedPrefabs").empty());
        const std::string rootId = result.at("entity");
        const std::string childId = result.at("children").at(0);
        EXPECT_EQ(result.at("parent"), Id(anchor));

        Entity instance = Find(rootId);
        ASSERT_TRUE(instance);
        EXPECT_EQ(instance.GetName(), "TurretA");
        EXPECT_TRUE(instance.HasComponent<SpriteRendererComponent>());
        // The prefab carried a non-default translation; so must the instance.
        EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Translation.y, 2.0f);
        ASSERT_EQ(anchor.Children().size(), 1u);

        m_Host.History.Undo();
        EXPECT_FALSE(m_Host.ActiveScene->TryGetEntityWithUUID(UUID(std::stoull(rootId))).has_value());
        EXPECT_FALSE(m_Host.ActiveScene->TryGetEntityWithUUID(UUID(std::stoull(childId))).has_value());
        EXPECT_TRUE(anchor.Children().empty());
        EXPECT_GT(m_Host.SelectionClears, 0);

        m_Host.History.Redo();
        Entity again = Find(rootId);
        ASSERT_TRUE(again);
        EXPECT_EQ(again.GetName(), "TurretA");
        ASSERT_EQ(again.Children().size(), 1u);
        EXPECT_EQ(static_cast<u64>(again.Children().front()), std::stoull(childId));
        EXPECT_EQ(static_cast<u64>(again.GetParentUUID()), static_cast<u64>(anchor.GetUUID()));
        ASSERT_EQ(anchor.Children().size(), 1u);
    }

    // ACCEPTANCE (b): the override query answers "what has this instance diverged
    // on" without the editor, FIELD BY FIELD -- including divergence the editor
    // never marked, which is all Prefab::DetectOverrides could report.
    TEST_F(AutomationPrefabCommandsTest, OverrideQueryReportsFieldLevelDivergenceTheEditorNeverMarked)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity instance = Instantiate(handle);

        const Json clean = Ok("olo_prefab_overrides", { { "entity", Id(instance) } });
        EXPECT_EQ(clean.at("divergedComponents").get<u32>(), 0u);
        EXPECT_EQ(clean.at("brokenLinks").get<u32>(), 0u);
        ASSERT_EQ(clean.at("entities").size(), 2u);
        EXPECT_FALSE(clean.at("entities").at(0).at("diverged").get<bool>());
        EXPECT_FALSE(clean.at("undoable").get<bool>());

        // Diverge one field, exactly as an agent would with olo_entity_set_field,
        // and mark NOTHING. This is the case DetectOverrides cannot see.
        instance.GetComponent<TransformComponent>().Translation.x = 42.0f;
        instance.AddComponent<CircleRendererComponent>();

        const Json diverged = Ok("olo_prefab_overrides", { { "entity", Id(instance) } });
        EXPECT_EQ(diverged.at("divergedEntities").get<u32>(), 1u);
        EXPECT_EQ(diverged.at("divergedComponents").get<u32>(), 2u);
        EXPECT_TRUE(diverged.at("entities").at(0).at("markedOverridden").empty());

        bool sawTranslation = false;
        bool sawAddedCircle = false;
        for (const auto& component : diverged.at("entities").at(0).at("components"))
        {
            if (component.at("component") == "TransformComponent")
            {
                EXPECT_EQ(component.at("state"), "modified");
                EXPECT_GT(component.at("comparedFields").get<u32>(), 0u);
                ASSERT_EQ(component.at("fields").size(), 1u);
                EXPECT_EQ(component.at("fields").at(0).at("field"), "Translation");
                EXPECT_FLOAT_EQ(component.at("fields").at(0).at("instance").at(0).get<f32>(), 42.0f);
                EXPECT_FLOAT_EQ(component.at("fields").at(0).at("prefab").at(0).get<f32>(), 1.0f);
                sawTranslation = true;
            }
            if (component.at("component") == "CircleRendererComponent")
            {
                EXPECT_EQ(component.at("state"), "added");
                sawAddedCircle = true;
            }
        }
        EXPECT_TRUE(sawTranslation);
        EXPECT_TRUE(sawAddedCircle);

        // includeChildren:false narrows the answer to the root alone.
        EXPECT_EQ(Ok("olo_prefab_overrides", { { "entity", Id(instance) }, { "includeChildren", false } })
                      .at("entities")
                      .size(),
                  1u);
    }

    // A component a prefab does not carry at all must be NAMED, not omitted: an
    // instance carrying one really does differ from anything the prefab can
    // express, and "no overrides" about it would be a lie.
    TEST_F(AutomationPrefabCommandsTest, OverrideQueryNamesComponentsAPrefabCannotCarry)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity instance = Instantiate(handle);
        ASSERT_EQ(std::ranges::find(Prefab::CopyableComponentNames(), "BoidComponent"),
                  Prefab::CopyableComponentNames().end())
            << "this case needs a component prefabs do NOT carry";
        instance.AddComponent<BoidComponent>();

        const Json overrides = Ok("olo_prefab_overrides", { { "entity", Id(instance) } });
        const auto& entity = overrides.at("entities").at(0);
        EXPECT_TRUE(Contains(entity.at("untrackedComponents"), "BoidComponent")) << entity.dump(2);
        // ... and it is not reported as an override, because the prefab side has no
        // opinion about it at all.
        for (const auto& component : entity.at("components"))
        {
            EXPECT_NE(component.at("component"), "BoidComponent");
        }
    }

    // ACCEPTANCE (a): apply -> every other instance reflects it, and ONE undo puts
    // the prefab, the file and every re-synced instance back.
    TEST_F(AutomationPrefabCommandsTest, ApplyReachesEveryInstanceAndOneUndoRestoresAllOfThem)
    {
        const std::string handle = MakeDetachedPrefab();
        const std::filesystem::path file = PrefabPath("Turret.oloprefab");
        const std::string fileBefore = ReadAll(file);

        Entity a = Instantiate(handle);
        Entity b = Instantiate(handle);
        Entity c = Instantiate(handle);
        ASSERT_TRUE(a && b && c);
        ASSERT_EQ(TopUndo(), "Instantiate Prefab");

        a.GetComponent<SpriteRendererComponent>().TilingFactor = 7.5f;

        const Json applied = Ok("olo_prefab_apply", { { "entity", Id(a) },
                                                      { "component", "SpriteRendererComponent" },
                                                      { "field", "TilingFactor" } });
        EXPECT_TRUE(applied.at("prefabFileWritten").get<bool>());
        EXPECT_FALSE(applied.contains("prefabFileNote"));
        ASSERT_EQ(applied.at("applied").size(), 1u);
        EXPECT_EQ(applied.at("applied").at(0).at("field"), "TilingFactor");
        EXPECT_EQ(applied.at("resyncedInstances").size(), 2u);
        EXPECT_TRUE(applied.at("skippedInstances").empty());

        EXPECT_FLOAT_EQ(b.GetComponent<SpriteRendererComponent>().TilingFactor, 7.5f);
        EXPECT_FLOAT_EQ(c.GetComponent<SpriteRendererComponent>().TilingFactor, 7.5f);
        Ref<Prefab> prefab = Load(handle);
        ASSERT_TRUE(prefab);
        EXPECT_FLOAT_EQ(prefab->FindSourceEntity(a).GetComponent<SpriteRendererComponent>().TilingFactor, 7.5f);
        EXPECT_NE(ReadAll(file), fileBefore);

        // ONE entry, not four: a single Ctrl-Z is the acceptance criterion, and the
        // entry under it is still the third instantiate.
        ASSERT_EQ(TopUndo(), "Apply Prefab Override");
        m_Host.History.Undo();
        EXPECT_EQ(TopUndo(), "Instantiate Prefab");

        EXPECT_FLOAT_EQ(a.GetComponent<SpriteRendererComponent>().TilingFactor, 7.5f)
            << "undo restores the instance's own override, it does not revert it";
        EXPECT_FLOAT_EQ(b.GetComponent<SpriteRendererComponent>().TilingFactor, 1.0f);
        EXPECT_FLOAT_EQ(c.GetComponent<SpriteRendererComponent>().TilingFactor, 1.0f);
        EXPECT_FLOAT_EQ(prefab->FindSourceEntity(a).GetComponent<SpriteRendererComponent>().TilingFactor, 1.0f);
        EXPECT_EQ(ReadAll(file), fileBefore);

        m_Host.History.Redo();
        EXPECT_FLOAT_EQ(b.GetComponent<SpriteRendererComponent>().TilingFactor, 7.5f);
        EXPECT_FLOAT_EQ(prefab->FindSourceEntity(a).GetComponent<SpriteRendererComponent>().TilingFactor, 7.5f);
        EXPECT_NE(ReadAll(file), fileBefore);
    }

    TEST_F(AutomationPrefabCommandsTest, ApplyClearsTheInstanceMarksAndResyncCanBeDeclined)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        Entity b = Instantiate(handle);
        a.GetComponent<SpriteRendererComponent>().TilingFactor = 2.0f;
        a.GetComponent<PrefabComponent>().MarkComponentOverridden("SpriteRendererComponent");

        const Json applied = Ok("olo_prefab_apply", { { "entity", Id(a) },
                                                      { "component", "SpriteRendererComponent" },
                                                      { "resyncInstances", false } });
        EXPECT_TRUE(applied.at("resyncedInstances").empty());
        EXPECT_FALSE(a.GetComponent<PrefabComponent>().IsComponentOverridden("SpriteRendererComponent"));
        EXPECT_FLOAT_EQ(b.GetComponent<SpriteRendererComponent>().TilingFactor, 1.0f);

        m_Host.History.Undo();
        EXPECT_TRUE(a.GetComponent<PrefabComponent>().IsComponentOverridden("SpriteRendererComponent"));
    }

    // A peer that deliberately overrides the component keeps its own value:
    // stomping it would make apply a way to lose somebody else's edit.
    TEST_F(AutomationPrefabCommandsTest, ApplySkipsAPeerThatOverridesTheSameComponent)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        Entity b = Instantiate(handle);

        b.GetComponent<SpriteRendererComponent>().TilingFactor = 3.0f;
        b.GetComponent<PrefabComponent>().MarkComponentOverridden("SpriteRendererComponent");
        a.GetComponent<SpriteRendererComponent>().TilingFactor = 7.5f;

        const Json applied = Ok("olo_prefab_apply", { { "entity", Id(a) }, { "component", "SpriteRendererComponent" } });
        EXPECT_TRUE(applied.at("resyncedInstances").empty());
        ASSERT_EQ(applied.at("skippedInstances").size(), 1u);
        EXPECT_EQ(applied.at("skippedInstances").at(0).at("entity"), Id(b));
        EXPECT_TRUE(Contains(applied.at("skippedInstances").at(0).at("components"), "SpriteRendererComponent"));
        EXPECT_FLOAT_EQ(b.GetComponent<SpriteRendererComponent>().TilingFactor, 3.0f);
    }

    // A whole-instance apply moves a component the instance ADDED and one it
    // REMOVED in the same entry, and undo takes both directions back.
    TEST_F(AutomationPrefabCommandsTest, WholeInstanceApplyMovesAddedAndRemovedComponents)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        Entity b = Instantiate(handle);

        a.AddComponent<CircleRendererComponent>();
        a.RemoveComponent<SpriteRendererComponent>();

        const Json applied = Ok("olo_prefab_apply", { { "entity", Id(a) } });
        EXPECT_EQ(applied.at("applied").size(), 2u);
        EXPECT_TRUE(b.HasComponent<CircleRendererComponent>());
        EXPECT_FALSE(b.HasComponent<SpriteRendererComponent>());
        Ref<Prefab> prefab = Load(handle);
        Entity source = prefab->FindSourceEntity(a);
        ASSERT_TRUE(source);
        EXPECT_TRUE(source.HasComponent<CircleRendererComponent>());
        EXPECT_FALSE(source.HasComponent<SpriteRendererComponent>());

        m_Host.History.Undo();
        EXPECT_FALSE(b.HasComponent<CircleRendererComponent>());
        EXPECT_TRUE(b.HasComponent<SpriteRendererComponent>());
        EXPECT_FALSE(source.HasComponent<CircleRendererComponent>());
        EXPECT_TRUE(source.HasComponent<SpriteRendererComponent>());
    }

    TEST_F(AutomationPrefabCommandsTest, RevertTakesOneFieldBackAndLeavesThePrefabAlone)
    {
        const std::string handle = MakeDetachedPrefab();
        const std::filesystem::path file = PrefabPath("Turret.oloprefab");
        const std::string fileBefore = ReadAll(file);
        Entity a = Instantiate(handle);

        a.GetComponent<TransformComponent>().Translation = { 9.0f, 9.0f, 9.0f };
        a.GetComponent<SpriteRendererComponent>().TilingFactor = 4.0f;
        a.GetComponent<PrefabComponent>().MarkComponentOverridden("TransformComponent");

        const Json reverted = Ok("olo_prefab_revert", { { "entity", Id(a) },
                                                        { "component", "TransformComponent" },
                                                        { "field", "Translation" } });
        ASSERT_EQ(reverted.at("reverted").size(), 1u);
        EXPECT_TRUE(reverted.at("changed").get<bool>());
        EXPECT_FLOAT_EQ(a.GetComponent<TransformComponent>().Translation.x, 1.0f);
        // Only the named field moved; the other divergence is untouched.
        EXPECT_FLOAT_EQ(a.GetComponent<SpriteRendererComponent>().TilingFactor, 4.0f);
        EXPECT_FALSE(a.GetComponent<PrefabComponent>().IsComponentOverridden("TransformComponent"));
        EXPECT_EQ(ReadAll(file), fileBefore);

        m_Host.History.Undo();
        EXPECT_FLOAT_EQ(a.GetComponent<TransformComponent>().Translation.x, 9.0f);
        EXPECT_TRUE(a.GetComponent<PrefabComponent>().IsComponentOverridden("TransformComponent"));
    }

    TEST_F(AutomationPrefabCommandsTest, WholeInstanceRevertClearsEveryDivergenceInOneEntry)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        a.GetComponent<TransformComponent>().Translation = { 9.0f, 9.0f, 9.0f };
        a.AddComponent<CircleRendererComponent>();

        const Json reverted = Ok("olo_prefab_revert", { { "entity", Id(a) } });
        EXPECT_EQ(reverted.at("reverted").size(), 2u);
        EXPECT_FLOAT_EQ(a.GetComponent<TransformComponent>().Translation.x, 1.0f);
        EXPECT_FALSE(a.HasComponent<CircleRendererComponent>());
        EXPECT_EQ(Ok("olo_prefab_overrides", { { "entity", Id(a) } }).at("divergedComponents").get<u32>(), 0u);

        m_Host.History.Undo();
        EXPECT_FLOAT_EQ(a.GetComponent<TransformComponent>().Translation.x, 9.0f);
        EXPECT_TRUE(a.HasComponent<CircleRendererComponent>());
    }

    // Reverting nothing must not leave a no-op entry the user has to press Ctrl-Z
    // past, the same rule olo_entity_reparent follows.
    TEST_F(AutomationPrefabCommandsTest, RevertingACleanInstanceAddsNoUndoEntry)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        ASSERT_EQ(TopUndo(), "Instantiate Prefab");

        const Json reverted = Ok("olo_prefab_revert", { { "entity", Id(a) } });
        EXPECT_TRUE(reverted.at("reverted").empty());
        EXPECT_FALSE(reverted.at("changed").get<bool>());
        EXPECT_FALSE(reverted.at("undoable").get<bool>());
        EXPECT_EQ(TopUndo(), "Instantiate Prefab") << "a no-op revert must not leave an entry to press Ctrl-Z past";
    }

    // The same no-op rule as revert, on the side that also writes a FILE: an apply
    // with nothing to move must not rewrite the .oloprefab (moving its timestamp
    // and waking the asset watcher) or leave an entry to press Ctrl-Z past.
    TEST_F(AutomationPrefabCommandsTest, ApplyingNothingWritesNothing)
    {
        const std::string handle = MakeDetachedPrefab();
        const std::filesystem::path file = PrefabPath("Turret.oloprefab");
        const std::string fileBefore = ReadAll(file);
        const auto stampBefore = std::filesystem::last_write_time(file);
        Entity a = Instantiate(handle);
        ASSERT_EQ(TopUndo(), "Instantiate Prefab");

        for (const Json& args : { Json{ { "entity", Id(a) } },
                                  Json{ { "entity", Id(a) }, { "component", "SpriteRendererComponent" } },
                                  Json{ { "entity", Id(a) },
                                        { "component", "SpriteRendererComponent" },
                                        { "field", "TilingFactor" } } })
        {
            const Json applied = Ok("olo_prefab_apply", args);
            EXPECT_FALSE(applied.at("changed").get<bool>()) << args.dump();
            EXPECT_FALSE(applied.at("undoable").get<bool>()) << args.dump();
            EXPECT_TRUE(applied.at("applied").empty()) << args.dump();
            EXPECT_FALSE(applied.at("prefabFileWritten").get<bool>()) << args.dump();
        }
        EXPECT_EQ(ReadAll(file), fileBefore);
        EXPECT_EQ(stampBefore, std::filesystem::last_write_time(file));
        EXPECT_EQ(TopUndo(), "Instantiate Prefab");
    }

    TEST_F(AutomationPrefabCommandsTest, UnpackBreaksOneLevelAndKeepsNestedInstances)
    {
        const std::string outer = MakeDetachedPrefab("Assets/Prefabs/Turret.oloprefab");
        Entity plainRoot = m_Host.ActiveScene->CreateEntity("Rack");
        plainRoot.AddComponent<SpriteRendererComponent>();
        const std::string inner =
            Ok("olo_prefab_create", { { "entity", Id(plainRoot) }, { "path", "Assets/Prefabs/Rack.oloprefab" } })
                .value("prefab", std::string{});
        ASSERT_FALSE(inner.empty());

        Entity host = Instantiate(outer);
        Entity nested = Instantiate(inner, { { "parent", Id(host) } });
        // A SECOND instance of the same prefab, parented under the first. Same
        // handle, so only the "its source is the prefab ROOT" rule keeps it out.
        Entity sibling = Instantiate(outer, { { "parent", Id(host) } });
        ASSERT_TRUE(nested && sibling);

        const Json unpacked = Ok("olo_prefab_unpack", { { "entity", Id(host) } });
        EXPECT_EQ(unpacked.at("unpacked").get<u32>(), 2u) << "the instance root and its own child, nothing nested";
        EXPECT_FALSE(host.HasComponent<PrefabComponent>());
        EXPECT_TRUE(nested.HasComponent<PrefabComponent>());
        EXPECT_TRUE(sibling.HasComponent<PrefabComponent>());
        ASSERT_EQ(unpacked.at("nestedInstancesKept").size(), 2u);

        m_Host.History.Undo();
        EXPECT_TRUE(host.HasComponent<PrefabComponent>());
        EXPECT_TRUE(host.GetComponent<PrefabComponent>().IsValid());

        const Json recursive = Ok("olo_prefab_unpack", { { "entity", Id(host) }, { "recursive", true } });
        EXPECT_EQ(recursive.at("unpacked").get<u32>(), 5u) << "2 owned + the nested single + the nested pair";
        EXPECT_FALSE(nested.HasComponent<PrefabComponent>());
        EXPECT_FALSE(sibling.HasComponent<PrefabComponent>());
        EXPECT_TRUE(recursive.at("nestedInstancesKept").empty());

        m_Host.History.Undo();
        EXPECT_TRUE(nested.HasComponent<PrefabComponent>());
        EXPECT_TRUE(sibling.HasComponent<PrefabComponent>());

        Entity neverAPrefab = m_Host.ActiveScene->CreateEntity("Bare");
        EXPECT_NE(Failure("olo_prefab_unpack", { { "entity", Id(neverAPrefab) } }).find("not a prefab instance"),
                  std::string::npos);
    }

    // "A silently wrong nested apply is the worst outcome here." An apply whose
    // SOURCE entity belongs to another prefab is refused by name.
    TEST_F(AutomationPrefabCommandsTest, ApplyIsRefusedWhenTheSourceEntityBelongsToANestedPrefab)
    {
        const std::string inner = MakeDetachedPrefab("Assets/Prefabs/Inner.oloprefab");
        Entity outerRoot = m_Host.ActiveScene->CreateEntity("OuterHost");
        outerRoot.AddComponent<SpriteRendererComponent>();
        const std::string outer =
            Ok("olo_prefab_create", { { "entity", Id(outerRoot) }, { "path", "Assets/Prefabs/Outer.oloprefab" } })
                .value("prefab", std::string{});
        ASSERT_FALSE(outer.empty());

        // Make the OUTER prefab's own source entity an instance of the inner
        // prefab -- exactly the shape a nested prefab has. olo_prefab_create
        // refuses to author this, which is why the case builds it by hand.
        Ref<Prefab> outerPrefab = Load(outer);
        ASSERT_TRUE(outerPrefab);
        Entity outerSource = outerPrefab->FindSourceEntity(outerRoot);
        ASSERT_TRUE(outerSource);
        outerSource.AddOrReplaceComponent<PrefabComponent>(AssetHandle(std::stoull(inner)), outerSource.GetUUID());

        outerRoot.GetComponent<SpriteRendererComponent>().TilingFactor = 5.0f;
        const std::string refusal = Failure("olo_prefab_apply", { { "entity", Id(outerRoot) } });
        EXPECT_NE(refusal.find("nested prefab"), std::string::npos) << refusal;
        EXPECT_NE(refusal.find(inner), std::string::npos) << refusal;
        // Nothing moved, and no undo entry was left behind.
        EXPECT_FLOAT_EQ(outerSource.GetComponent<SpriteRendererComponent>().TilingFactor, 1.0f);

        // Reverting only READS the source, so it stays allowed.
        const Json reverted = Ok("olo_prefab_revert", { { "entity", Id(outerRoot) } });
        EXPECT_TRUE(reverted.at("changed").get<bool>());
        EXPECT_FLOAT_EQ(outerRoot.GetComponent<SpriteRendererComponent>().TilingFactor, 1.0f);
    }

    TEST_F(AutomationPrefabCommandsTest, CreateRefusesASubtreeThatAlreadyContainsAnInstance)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity holder = m_Host.ActiveScene->CreateEntity("Holder");
        ASSERT_TRUE(Instantiate(handle, { { "parent", Id(holder) } }));

        const std::string message =
            Failure("olo_prefab_create", { { "entity", Id(holder) }, { "path", "Assets/Prefabs/Nope.oloprefab" } });
        EXPECT_NE(message.find("nest"), std::string::npos) << message;
        EXPECT_FALSE(std::filesystem::exists(PrefabPath("Nope.oloprefab")));
    }

    TEST_F(AutomationPrefabCommandsTest, CreateRefusesPathsOutsideTheAssetDirectoryAndNonPrefabExtensions)
    {
        Entity root = m_Host.ActiveScene->CreateEntity("Loose");
        EXPECT_NE(Failure("olo_prefab_create", { { "entity", Id(root) }, { "path", "Assets/Prefabs/x.txt" } })
                      .find(".oloprefab"),
                  std::string::npos);
        EXPECT_NE(Failure("olo_prefab_create", { { "entity", Id(root) }, { "path", "Outside/x.oloprefab" } })
                      .find("asset directory"),
                  std::string::npos);
        EXPECT_NE(Failure("olo_prefab_create", { { "entity", Id(root) }, { "path", "Assets/../Assets2/x.oloprefab" } })
                      .find("asset directory"),
                  std::string::npos);
        // Spelled so it is absolute on Windows and outside the asset directory on
        // Linux: either way the refusal must be non-empty, and the two platforms
        // legitimately refuse it for different reasons.
        EXPECT_FALSE(
            Failure("olo_prefab_create", { { "entity", Id(root) }, { "path", "C:/somewhere/x.oloprefab" } }).empty());

        // A second create at the same path never overwrites.
        ASSERT_FALSE(Ok("olo_prefab_create", { { "entity", Id(root) }, { "path", "Assets/Prefabs/Loose.oloprefab" } })
                         .at("prefab")
                         .get<std::string>()
                         .empty());
        Entity other = m_Host.ActiveScene->CreateEntity("Other");
        EXPECT_NE(Failure("olo_prefab_create", { { "entity", Id(other) }, { "path", "Assets/Prefabs/Loose.oloprefab" } })
                      .find("already exists"),
                  std::string::npos);
    }

    TEST_F(AutomationPrefabCommandsTest, ArgumentsAreValidatedBeforeAnythingIsTouched)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        Entity plain = m_Host.ActiveScene->CreateEntity("Plain");
        const std::string top = TopUndo();

        EXPECT_NE(Failure("olo_prefab_overrides", { { "entity", Id(plain) } }).find("not a prefab instance"),
                  std::string::npos);
        EXPECT_NE(Failure("olo_prefab_apply", { { "entity", Id(a) }, { "field", "Translation" } })
                      .find("requires component"),
                  std::string::npos);
        EXPECT_NE(
            Failure("olo_prefab_apply", { { "entity", Id(a) }, { "component", "BoidComponent" } }).find("does not carry"),
            std::string::npos);
        EXPECT_NE(Failure("olo_prefab_apply",
                          { { "entity", Id(a) }, { "component", "TransformComponent" }, { "field", "NoSuchField" } })
                      .find("NoSuchField"),
                  std::string::npos);
        EXPECT_NE(Failure("olo_prefab_instantiate", { { "prefab", "0" } }).find("nonzero"), std::string::npos);
        EXPECT_NE(Failure("olo_prefab_instantiate", { { "prefab", "123456789" } }).find("No prefab asset"),
                  std::string::npos);
        EXPECT_NE(Failure("olo_prefab_instantiate", { { "prefab", handle }, { "parent", "777" } }).find("does not exist"),
                  std::string::npos);
        EXPECT_EQ(TopUndo(), top) << "a refused call must not leave an undo entry";
    }

    // A broken link is reported, never resolved to the prefab ROOT. Falling back
    // would apply a child's override onto the root -- the wrong entity, silently.
    TEST_F(AutomationPrefabCommandsTest, ABrokenPrefabLinkIsReportedRatherThanResolvedToTheRoot)
    {
        const std::string handle = MakeDetachedPrefab();
        Entity a = Instantiate(handle);
        Entity child = *m_Host.ActiveScene->TryGetEntityWithUUID(a.Children().front());
        child.GetComponent<PrefabComponent>().m_PrefabEntityID = UUID(987654321);

        const Json overrides = Ok("olo_prefab_overrides", { { "entity", Id(a) } });
        EXPECT_EQ(overrides.at("brokenLinks").get<u32>(), 1u);
        bool sawBroken = false;
        for (const auto& entity : overrides.at("entities"))
        {
            if (entity.at("linkBroken").get<bool>())
            {
                EXPECT_EQ(entity.at("entity"), Id(child));
                EXPECT_FALSE(entity.at("diverged").get<bool>());
                sawBroken = true;
            }
        }
        EXPECT_TRUE(sawBroken);
        EXPECT_NE(Failure("olo_prefab_apply", { { "entity", Id(child) } }).find("link is broken"), std::string::npos);
    }
} // namespace OloEngine::Automation::Tests
