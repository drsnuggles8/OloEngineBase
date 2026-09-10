// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationComponentRegistry.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpServer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "UndoRedo/EditorCommand.h"

#include <concepts>
#include <functional>
#include <string>
#include <unordered_set>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;

        class ComponentTestHost final : public IAutomationHost
        {
          public:
            MCP::EditorMcpContext Hooks;
            bool InMainJob = false;
            int MarshalCount = 0;
            std::function<void()> BeforeMainJob;

            const MCP::EditorMcpContext& Context() const override
            {
                return Hooks;
            }

            bool IsCurrentCallCancelled() const override
            {
                return false;
            }

            bool PublishArtifact(AutomationArtifact) override
            {
                return false;
            }

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& job, std::chrono::milliseconds) override
            {
                ++MarshalCount;
                if (BeforeMainJob)
                {
                    BeforeMainJob();
                }
                InMainJob = true;
                const auto result = job();
                InMainJob = false;
                return result;
            }

            void EmitProgressUpdate(f64, f64, const std::string&) const override
            {
            }
        };

        class McpAutomationComponents : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                m_Scene = Ref<Scene>::Create();
                m_Entity = m_Scene->CreateEntityWithUUID(UUID(98765432101234567ULL), "Authoring");
                m_Host.Hooks.GetActiveScene = [this]
                {
                    EXPECT_TRUE(m_Host.InMainJob);
                    return m_Scene;
                };
                m_Host.Hooks.GetCommandHistory = [this]() -> CommandHistory*
                {
                    EXPECT_TRUE(m_Host.InMainJob);
                    return m_EditMode ? &m_History : nullptr;
                };
                RegisterComponentAuthoringCommands(m_Registry);
            }

            AutomationInvocation Invoke(const char* name, const char* component,
                                        AutomationWriteConsent consent = AutomationWriteConsent::Granted)
            {
                return m_Registry.Invoke(m_Host, name,
                                         Json{ { "entity", std::to_string(static_cast<u64>(m_Entity.GetUUID())) },
                                               { "component", component } },
                                         consent);
            }

            Ref<Scene> m_Scene;
            Entity m_Entity;
            ComponentTestHost m_Host;
            CommandHistory m_History;
            AutomationRegistry m_Registry;
            bool m_EditMode = true;
        };

        template<typename... T>
        constexpr sizet TupleSize(ComponentGroup<T...>)
        {
            return sizeof...(T);
        }

        class ScopedComponentAssets
        {
          public:
            ScopedComponentAssets()
                : m_Previous(Project::HasAssetManager() ? Project::GetAssetManager() : nullptr),
                  m_Manager(Ref<RuntimeAssetManager>::Create(false))
            {
                Project::SetAssetManager(m_Manager);
            }

            ~ScopedComponentAssets()
            {
                if (m_Previous)
                {
                    Project::SetAssetManager(m_Previous);
                }
                else
                {
                    Project::Unload();
                }
            }

            Ref<AssetManagerBase> Manager() const
            {
                return m_Manager;
            }

          private:
            Ref<AssetManagerBase> m_Previous;
            Ref<RuntimeAssetManager> m_Manager;
        };
    } // namespace

    TEST_F(McpAutomationComponents, GeneratedUniverseIncludesEveryAuthoredTypeAndProtectedRuntimeTypes)
    {
        const auto result = m_Registry.Invoke(m_Host, "olo_component_list_types", Json::object());
        ASSERT_TRUE(result.Ran());
        ASSERT_FALSE(result.Result.IsError);
        const auto& types = result.Result.StructuredContent.at("components");
        sizet authored = 0;
        std::unordered_set<std::string> names;
        for (const auto& type : types)
        {
            EXPECT_TRUE(names.insert(type.at("component").get<std::string>()).second);
            if (type.at("authored").get<bool>())
            {
                ++authored;
            }
        }
        EXPECT_EQ(authored, TupleSize(AllComponents{}));
        EXPECT_EQ(types.size(), ComponentTypes().size());
        EXPECT_TRUE(names.contains("SkeletonComponent"));
        EXPECT_TRUE(names.contains("TagComponent"));
        EXPECT_TRUE(names.contains("WorldTransformComponent"));
        EXPECT_TRUE(names.contains("DialogueStateComponent"));
        EXPECT_FALSE(FindComponentType("TransformComponent")->Removable);
        EXPECT_FALSE(FindComponentType("RelationshipComponent")->Addable);
        EXPECT_FALSE(FindComponentType("IDComponent")->RejectionReason.empty());
        EXPECT_FALSE(m_History.CanUndo());
    }

    TEST_F(McpAutomationComponents, EveryAddableGeneratedTypeSupportsDefaultPresenceUndo)
    {
        for (const auto& type : ComponentTypes())
        {
            if (!type.Addable)
                continue;
            SCOPED_TRACE(type.Name);
            ASSERT_FALSE(type.Has(m_Entity));
            ASSERT_FALSE(Invoke("olo_component_add", type.Name.c_str()).Result.IsError);
            ASSERT_TRUE(type.Has(m_Entity));
            ASSERT_FALSE(Invoke("olo_component_remove", type.Name.c_str()).Result.IsError);
            ASSERT_FALSE(type.Has(m_Entity));
            m_History.Undo();
            ASSERT_TRUE(type.Has(m_Entity));
            m_History.Undo();
            ASSERT_FALSE(type.Has(m_Entity));
            EXPECT_FALSE(m_History.CanUndo());
            EXPECT_FALSE(m_History.IsDirty());
            m_History.Clear();
        }
    }

    TEST_F(McpAutomationComponents, NonEqualityComponentRemoveRestoresNestedDataInOneUndoStep)
    {
        static_assert(!std::equality_comparable<ParticleSystemComponent>);
        auto& particles = m_Entity.AddComponent<ParticleSystemComponent>();
        particles.System.Emitter.RateOverTime = 71.5f;
        particles.ChildSystems.resize(2);
        particles.ChildSystems[1].Emitter.RateOverTime = 123.0f;

        const auto removed = Invoke("olo_component_remove", "ParticleSystemComponent");
        ASSERT_TRUE(removed.Ran());
        ASSERT_FALSE(removed.Result.IsError);
        EXPECT_FALSE(m_Entity.HasComponent<ParticleSystemComponent>());
        EXPECT_TRUE(m_History.IsDirty());
        m_History.Undo();
        ASSERT_TRUE(m_Entity.HasComponent<ParticleSystemComponent>());
        const auto& restored = m_Entity.GetComponent<ParticleSystemComponent>();
        EXPECT_FLOAT_EQ(restored.System.Emitter.RateOverTime, 71.5f);
        ASSERT_EQ(restored.ChildSystems.size(), 2u);
        EXPECT_FLOAT_EQ(restored.ChildSystems[1].Emitter.RateOverTime, 123.0f);
        EXPECT_FALSE(m_History.CanUndo());
        EXPECT_FALSE(m_History.IsDirty());
        m_History.Redo();
        EXPECT_FALSE(m_Entity.HasComponent<ParticleSystemComponent>());
    }

    TEST_F(McpAutomationComponents, AddRedoPreservesGeneratedDefaultUUIDs)
    {
        const auto added = Invoke("olo_component_add", "PrefabComponent");
        ASSERT_TRUE(added.Ran());
        ASSERT_FALSE(added.Result.IsError);
        const auto prefab = m_Entity.GetComponent<PrefabComponent>();
        m_History.Undo();
        EXPECT_FALSE(m_Entity.HasComponent<PrefabComponent>());
        m_History.Redo();
        const auto& restored = m_Entity.GetComponent<PrefabComponent>();
        EXPECT_EQ(restored.m_PrefabID, prefab.m_PrefabID);
        EXPECT_EQ(restored.m_PrefabEntityID, prefab.m_PrefabEntityID);
    }

    TEST_F(McpAutomationComponents, CameraRemovalUndoRestoresSnapshotAfterViewportHook)
    {
        m_Scene->OnViewportResize(1920, 1080);
        auto& camera = m_Entity.AddComponent<CameraComponent>();
        camera.Camera.SetViewportSize(1000, 1000);
        camera.FixedAspectRatio = true;
        ASSERT_FALSE(Invoke("olo_component_remove", "CameraComponent").Result.IsError);
        m_History.Undo();
        EXPECT_FLOAT_EQ(m_Entity.GetComponent<CameraComponent>().Camera.GetAspectRatio(), 1.0f);
        EXPECT_TRUE(m_Entity.GetComponent<CameraComponent>().FixedAspectRatio);
    }

    TEST_F(McpAutomationComponents, IdempotentOperationsDoNotCreateUndoOrDirtyState)
    {
        m_Entity.AddComponent<PointLightComponent>();
        EXPECT_FALSE(Invoke("olo_component_add", "PointLightComponent").Result.StructuredContent.at("changed").get<bool>());
        EXPECT_FALSE(Invoke("olo_component_remove", "CameraComponent").Result.StructuredContent.at("changed").get<bool>());
        EXPECT_FALSE(m_History.CanUndo());
        EXPECT_FALSE(m_History.IsDirty());
    }

    TEST_F(McpAutomationComponents, ConsentAndEditModeAreCheckedBeforeMutation)
    {
        const auto denied = Invoke("olo_component_add", "PointLightComponent", AutomationWriteConsent::Withheld);
        EXPECT_EQ(denied.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
        EXPECT_EQ(m_Host.MarshalCount, 0);
        m_Host.BeforeMainJob = [this]
        { m_EditMode = false; };
        const auto playing = Invoke("olo_component_add", "PointLightComponent");
        ASSERT_TRUE(playing.Ran());
        EXPECT_TRUE(playing.Result.IsError);
        EXPECT_FALSE(m_Entity.HasComponent<PointLightComponent>());
        EXPECT_FALSE(m_History.CanUndo());
    }

    TEST_F(McpAutomationComponents, ProtectedAndUnknownTypesAreRejectedWithoutChanges)
    {
        EXPECT_TRUE(Invoke("olo_component_remove", "TransformComponent").Result.IsError);
        EXPECT_TRUE(Invoke("olo_component_add", "WorldTransformComponent").Result.IsError);
        EXPECT_TRUE(Invoke("olo_component_add", "MadeUpComponent").Result.IsError);
        EXPECT_TRUE(m_Entity.HasComponent<TransformComponent>());
        EXPECT_FALSE(m_History.CanUndo());
    }

    TEST_F(McpAutomationComponents, QueryDistinguishesAbsentFromATypeWithoutWritableFields)
    {
        const auto absent = Invoke("olo_component_get", "SkeletonComponent");
        ASSERT_FALSE(absent.Result.IsError);
        EXPECT_FALSE(absent.Result.StructuredContent.at("present").get<bool>());
        ASSERT_FALSE(Invoke("olo_component_add", "SkeletonComponent").Result.IsError);
        const auto present = Invoke("olo_component_get", "SkeletonComponent");
        ASSERT_FALSE(present.Result.IsError);
        EXPECT_TRUE(present.Result.StructuredContent.at("present").get<bool>());
        EXPECT_TRUE(present.Result.StructuredContent.at("fields").is_array());
        EXPECT_EQ(present.Result.StructuredContent.at("entity"), "98765432101234567");
    }

    TEST_F(McpAutomationComponents, RuntimeSiblingRemovalIsRefusedAtomically)
    {
        m_Entity.AddComponent<SpringBoneComponent>();
        m_Entity.AddComponent<SpringBoneStateComponent>();
        const auto result = Invoke("olo_component_remove", "SpringBoneComponent");
        EXPECT_TRUE(result.Result.IsError);
        EXPECT_TRUE(m_Entity.HasComponent<SpringBoneComponent>());
        EXPECT_TRUE(m_Entity.HasComponent<SpringBoneStateComponent>());
        EXPECT_FALSE(m_History.CanUndo());
        EXPECT_FALSE(ValidateEntitySnapshot(m_Entity).empty());
    }

    TEST_F(McpAutomationComponents, TerrainRemovalUndoRetainsMutableAuthoringResources)
    {
        auto& terrain = m_Entity.AddComponent<TerrainComponent>();
        auto heights = Ref<TerrainData>::Create();
        auto material = Ref<TerrainMaterial>::Create();
        auto voxels = Ref<VoxelOverride>::Create();
        terrain.m_TerrainData = heights;
        terrain.m_Material = material;
        terrain.m_VoxelOverride = voxels;
        terrain.m_NeedsRebuild = false;
        terrain.m_MaterialNeedsRebuild = false;
        terrain.m_AutoSplatNeedsRebuild = false;

        EXPECT_TRUE(ValidateEntitySnapshot(m_Entity).empty());
        EXPECT_FALSE(ValidateEntityDuplication(m_Entity).empty());
        ASSERT_FALSE(Invoke("olo_component_remove", "TerrainComponent").Result.IsError);
        m_History.Undo();
        const auto& restored = m_Entity.GetComponent<TerrainComponent>();
        EXPECT_EQ(restored.m_TerrainData, heights);
        EXPECT_EQ(restored.m_Material, material);
        EXPECT_EQ(restored.m_VoxelOverride, voxels);
        EXPECT_FALSE(restored.m_NeedsRebuild);
        EXPECT_FALSE(restored.m_MaterialNeedsRebuild);
        EXPECT_FALSE(restored.m_AutoSplatNeedsRebuild);
        m_History.Redo();
        m_History.Undo();
        EXPECT_EQ(m_Entity.GetComponent<TerrainComponent>().m_VoxelOverride, voxels);
    }

    TEST_F(McpAutomationComponents, GeneratedLODRemovalUndoRestoresTheSameAssetsAndHandles)
    {
        ASSERT_TRUE(Project::HasAssetManager() || !Project::GetActive());
        ScopedComponentAssets assets;
        const auto asset = Ref<Asset>::Create();
        const auto handle = AssetManager::AddMemoryOnlyAsset(asset);
        ASSERT_NE(static_cast<u64>(handle), 0u);
        auto& lod = m_Entity.AddComponent<LODGroupComponent>();
        lod.m_AutoGenerated = true;
        lod.m_GeneratedLODHandles = { handle };
        lod.m_LODGroup.Levels.emplace_back(handle, 40.0f, 16);
        EXPECT_TRUE(ValidateEntitySnapshot(m_Entity).empty());
        EXPECT_FALSE(ValidateEntityDuplication(m_Entity).empty());

        ASSERT_FALSE(Invoke("olo_component_remove", "LODGroupComponent").Result.IsError);
        EXPECT_FALSE(assets.Manager()->GetMemoryAsset(handle));
        m_History.Undo();
        const auto& restored = m_Entity.GetComponent<LODGroupComponent>();
        EXPECT_EQ(restored.m_GeneratedLODHandles, std::vector<AssetHandle>{ handle });
        ASSERT_EQ(restored.m_LODGroup.Levels.size(), 1u);
        EXPECT_EQ(restored.m_LODGroup.Levels.front().MeshHandle, handle);
        EXPECT_EQ(assets.Manager()->GetMemoryAsset(handle), asset);
        m_History.Redo();
        EXPECT_FALSE(assets.Manager()->GetMemoryAsset(handle));
        m_History.Undo();
        EXPECT_EQ(assets.Manager()->GetMemoryAsset(handle), asset);
        // Clean up through the real hook before returning the process-wide manager.
        m_Entity.RemoveComponent<LODGroupComponent>();
        m_History.Clear();
    }

    TEST_F(McpAutomationComponents, EntitySnapshotRetainsGeneratedLODAssetsAcrossDestruction)
    {
        ASSERT_TRUE(Project::HasAssetManager() || !Project::GetActive());
        ScopedComponentAssets assets;
        const auto asset = Ref<Asset>::Create();
        const auto handle = AssetManager::AddMemoryOnlyAsset(asset);
        ASSERT_NE(static_cast<u64>(handle), 0u);
        auto& lod = m_Entity.AddComponent<LODGroupComponent>();
        lod.m_GeneratedLODHandles = { handle };
        lod.m_LODGroup.Levels.emplace_back(handle, 15.0f);
        const auto snapshots = CaptureEntityComponents(m_Entity);
        const auto uuid = m_Entity.GetUUID();
        m_Scene->DestroyEntity(m_Entity);
        EXPECT_FALSE(assets.Manager()->GetMemoryAsset(handle));
        m_Entity = m_Scene->CreateEntityWithUUID(uuid, "Restored");
        for (const auto& snapshot : snapshots)
        {
            snapshot->Restore(m_Entity);
        }
        EXPECT_EQ(assets.Manager()->GetMemoryAsset(handle), asset);
        EXPECT_EQ(m_Entity.GetComponent<LODGroupComponent>().m_GeneratedLODHandles,
                  std::vector<AssetHandle>{ handle });
        m_Entity.RemoveComponent<LODGroupComponent>();
    }

    TEST_F(McpAutomationComponents, UnsavedAnimationGraphSurvivesRemovalAndEntityDestruction)
    {
        auto graph = Ref<AnimationGraph>::Create();
        graph->Layers.emplace_back().Name = "Unsaved authored layer";
        m_Entity.AddComponent<AnimationGraphComponent>().RuntimeGraph = graph;
        ASSERT_FALSE(Invoke("olo_component_remove", "AnimationGraphComponent").Result.IsError);
        ASSERT_FALSE(m_Entity.HasComponent<AnimationGraphComponent>());
        m_History.Undo();
        ASSERT_EQ(m_Entity.GetComponent<AnimationGraphComponent>().RuntimeGraph, graph);
        EXPECT_EQ(graph->Layers.front().Name, "Unsaved authored layer");
        m_History.Redo();
        m_History.Undo();
        ASSERT_EQ(m_Entity.GetComponent<AnimationGraphComponent>().RuntimeGraph, graph);

        EXPECT_FALSE(ValidateEntityDuplication(m_Entity).empty());
        const auto snapshots = CaptureEntityComponents(m_Entity);
        const auto uuid = m_Entity.GetUUID();
        m_Scene->DestroyEntity(m_Entity);
        m_Entity = m_Scene->CreateEntityWithUUID(uuid, "Restored");
        for (const auto& snapshot : snapshots)
            snapshot->Restore(m_Entity);
        ASSERT_EQ(m_Entity.GetComponent<AnimationGraphComponent>().RuntimeGraph, graph);
        EXPECT_EQ(graph->Layers.front().Name, "Unsaved authored layer");
    }

    TEST_F(McpAutomationComponents, MissingGeneratedLODOwnershipIsRejectedBeforeRemoval)
    {
        auto& lod = m_Entity.AddComponent<LODGroupComponent>();
        lod.m_GeneratedLODHandles = { AssetHandle(0) };
        EXPECT_TRUE(Invoke("olo_component_remove", "LODGroupComponent").Result.IsError);
        EXPECT_TRUE(m_Entity.HasComponent<LODGroupComponent>());
        EXPECT_FALSE(m_History.CanUndo());
        lod.m_GeneratedLODHandles.clear();
    }

    TEST_F(McpAutomationComponents, SnapshotRestoresTransformRelationshipAndComponents)
    {
        m_Entity.GetComponent<TransformComponent>().Translation = { 3.0f, 5.0f, 7.0f };
        auto child = m_Scene->CreateEntity("Child");
        child.SetParent(m_Entity);
        m_Entity.AddComponent<ParticleSystemComponent>().System.Emitter.RateOverTime = 33.0f;
        const auto snapshots = CaptureEntityComponents(m_Entity);
        auto target = m_Scene->CreateEntity("Restore target");
        for (const auto& snapshot : snapshots)
        {
            snapshot->Restore(target);
        }
        EXPECT_FLOAT_EQ(target.GetComponent<TransformComponent>().Translation.y, 5.0f);
        ASSERT_TRUE(target.HasComponent<RelationshipComponent>());
        EXPECT_EQ(target.Children(), m_Entity.Children());
        ASSERT_TRUE(target.HasComponent<ParticleSystemComponent>());
        EXPECT_FLOAT_EQ(target.GetComponent<ParticleSystemComponent>().System.Emitter.RateOverTime, 33.0f);
        // The entity command owns remapping the copied hierarchy before exposing it.
        target.GetComponent<RelationshipComponent>().m_Children.clear();
    }
} // namespace OloEngine::Automation
