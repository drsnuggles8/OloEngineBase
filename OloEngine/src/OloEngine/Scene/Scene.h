#pragma once

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/CrowdManager.h"

#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <box2d/id.h>

#pragma warning(push)
#pragma warning(disable : 4996)
#include "entt.hpp"
#pragma warning(pop)

namespace OloEngine
{
    class Entity;
    class MeshSource;
    class Skeleton;
    class Prefab;
    class JoltScene;
    class SceneStreamer;
    class DialogueSystem;

    class Scene : public Asset
    {
      public:
        Scene();
        ~Scene();

        static Ref<Scene> Create();
        static Ref<Scene> Copy(Ref<Scene>& other);

        [[nodiscard("Store this!")]] Entity CreateEntity(const std::string& name = std::string());
        [[nodiscard("Store this!")]] Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
        void DestroyEntity(Entity entity);

        // Prefab instantiation
        [[nodiscard("Store this!")]] Entity Instantiate(AssetHandle prefabHandle);
        [[nodiscard("Store this!")]] Entity InstantiateWithUUID(AssetHandle prefabHandle, UUID uuid);

        // Prefab override management
        void UpdateAllPrefabInstances();
        void RevertPrefabComponent(Entity entity, const std::string& componentName);
        void ApplyPrefabComponent(Entity entity, const std::string& componentName);
        void MarkPrefabComponentOverridden(Entity entity, const std::string& componentName);

        void OnRuntimeStart();
        void OnRuntimeStop();

        void OnSimulationStart();
        void OnSimulationStop();

        void OnUpdateRuntime(Timestep ts);
        void OnUpdateSimulation(Timestep ts, EditorCamera const& camera);
        void OnUpdateEditor(Timestep ts, EditorCamera const& camera);
        void OnViewportResize(u32 width, u32 height);

        [[nodiscard]] u32 GetViewportWidth() const
        {
            return m_ViewportWidth;
        }
        [[nodiscard]] u32 GetViewportHeight() const
        {
            return m_ViewportHeight;
        }

        [[nodiscard]] Entity DuplicateEntity(Entity entity);

        [[nodiscard("Store this!")]] Entity FindEntityByName(std::string_view name);
        [[nodiscard("Store this!")]] Entity GetEntityByUUID(UUID uuid);

        [[nodiscard("Store this!")]] Entity GetPrimaryCameraEntity();

        [[nodiscard("Store this!")]] Entity FindEntityByName(std::string_view name) const;
        [[nodiscard("Store this!")]] Entity GetEntityByUUID(UUID uuid) const;

        [[nodiscard("Store this!")]] Entity GetPrimaryCameraEntity() const;

        // Bone entity management (Hazel-style)
        std::vector<glm::mat4> GetModelSpaceBoneTransforms(const std::vector<UUID>& boneEntityIds, const MeshSource& meshSource) const;
        std::vector<UUID> FindBoneEntityIds(Entity rootEntity, const Skeleton& skeleton) const;
        glm::mat4 FindRootBoneTransform(Entity entity, const std::vector<UUID>& boneEntityIds) const;
        void BuildBoneEntityIds(Entity entity);
        void BuildMeshBoneEntityIds(Entity entity, Entity rootEntity);
        void BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity);

        // Entity lookup utilities
        [[nodiscard("Store this!")]] std::optional<Entity> TryGetEntityWithUUID(UUID id) const;

        [[nodiscard("Store this!")]] bool IsRunning() const
        {
            return m_IsRunning;
        }
        [[nodiscard("Store this!")]] bool IsPaused() const
        {
            return m_IsPaused;
        }

        void SetPaused(bool paused)
        {
            m_IsPaused = paused;
        }

        [[nodiscard("Store this!")]] bool GetPendingReload() const
        {
            return m_PendingReload;
        }
        void SetPendingReload(bool pending)
        {
            m_PendingReload = pending;
        }

        void Step(int frames = 1);

        void SetName(std::string_view name);
        [[nodiscard("Store this!")]] const std::string& GetName() const
        {
            return m_Name;
        }

        template<typename... Components>
        auto GetAllEntitiesWith()
        {
            return m_Registry.view<Components...>();
        }

        template<typename... Components>
        auto GetAllEntitiesWith() const
        {
            return m_Registry.view<Components...>();
        }

        // Physics access
        JoltScene* GetPhysicsScene() const
        {
            return m_JoltScene.get();
        }

        // Physics lifecycle (public for external scene setup)
        void OnPhysics3DStart();
        void OnPhysics3DStop();

        // 3D rendering mode
        void SetIs3DModeEnabled(bool enabled)
        {
            m_Is3DModeEnabled = enabled;
        }
        [[nodiscard("Store this!")]] bool IsIs3DModeEnabled() const
        {
            return m_Is3DModeEnabled;
        }

        // Render throttling — skip scene rendering while keeping simulation running
        void SetRenderingEnabled(bool enabled)
        {
            m_RenderingEnabled = enabled;
        }

        // Viewport grid settings (editor only)
        void SetGridVisible(bool visible)
        {
            m_ShowGrid = visible;
        }
        [[nodiscard("Store this!")]] bool IsGridVisible() const
        {
            return m_ShowGrid;
        }
        void SetLightGizmosVisible(bool visible)
        {
            m_ShowLightGizmos = visible;
        }
        [[nodiscard("Store this!")]] bool AreLightGizmosVisible() const
        {
            return m_ShowLightGizmos;
        }
        void SetGridSpacing(f32 spacing)
        {
            if (spacing > 0.0f)
            {
                m_GridSpacing = spacing;
            }
        }
        [[nodiscard("Store this!")]] f32 GetGridSpacing() const
        {
            return m_GridSpacing;
        }

        // Skeleton visualization settings (editor only)
        struct SkeletonVisualizationSettings
        {
            bool ShowSkeleton = false;
            bool ShowBones = true;
            bool ShowJoints = true;
            f32 JointSize = 1.0f;
            f32 BoneThickness = 1.0f;
        };

        void SetSkeletonVisualization(const SkeletonVisualizationSettings& settings)
        {
            m_SkeletonVisualization = settings;
        }
        [[nodiscard]] const SkeletonVisualizationSettings& GetSkeletonVisualization() const
        {
            return m_SkeletonVisualization;
        }
        [[nodiscard]] SkeletonVisualizationSettings& GetSkeletonVisualization()
        {
            return m_SkeletonVisualization;
        }

        void SetPostProcessSettings(const PostProcessSettings& settings)
        {
            m_PostProcessSettings = settings;
        }
        [[nodiscard]] const PostProcessSettings& GetPostProcessSettings() const
        {
            return m_PostProcessSettings;
        }
        [[nodiscard]] PostProcessSettings& GetPostProcessSettings()
        {
            return m_PostProcessSettings;
        }

        void SetSnowSettings(const SnowSettings& settings)
        {
            m_SnowSettings = settings;
        }
        [[nodiscard]] const SnowSettings& GetSnowSettings() const
        {
            return m_SnowSettings;
        }
        [[nodiscard]] SnowSettings& GetSnowSettings()
        {
            return m_SnowSettings;
        }

        void SetFogSettings(const FogSettings& settings)
        {
            m_FogSettings = settings;
        }
        [[nodiscard]] const FogSettings& GetFogSettings() const
        {
            return m_FogSettings;
        }
        [[nodiscard]] FogSettings& GetFogSettings()
        {
            return m_FogSettings;
        }

        void SetWindSettings(const WindSettings& settings)
        {
            m_WindSettings = settings;
        }
        [[nodiscard]] const WindSettings& GetWindSettings() const
        {
            return m_WindSettings;
        }
        [[nodiscard]] WindSettings& GetWindSettings()
        {
            return m_WindSettings;
        }

        void SetSnowAccumulationSettings(const SnowAccumulationSettings& settings)
        {
            m_SnowAccumulationSettings = settings;
        }
        [[nodiscard]] const SnowAccumulationSettings& GetSnowAccumulationSettings() const
        {
            return m_SnowAccumulationSettings;
        }
        [[nodiscard]] SnowAccumulationSettings& GetSnowAccumulationSettings()
        {
            return m_SnowAccumulationSettings;
        }

        void SetSnowEjectaSettings(const SnowEjectaSettings& settings)
        {
            m_SnowEjectaSettings = settings;
        }
        [[nodiscard]] const SnowEjectaSettings& GetSnowEjectaSettings() const
        {
            return m_SnowEjectaSettings;
        }
        [[nodiscard]] SnowEjectaSettings& GetSnowEjectaSettings()
        {
            return m_SnowEjectaSettings;
        }

        void SetPrecipitationSettings(const PrecipitationSettings& settings)
        {
            m_PrecipitationSettings = settings;
        }
        [[nodiscard]] const PrecipitationSettings& GetPrecipitationSettings() const
        {
            return m_PrecipitationSettings;
        }
        [[nodiscard]] PrecipitationSettings& GetPrecipitationSettings()
        {
            return m_PrecipitationSettings;
        }

        void SetStreamingSettings(const StreamingSettings& settings)
        {
            m_StreamingSettings = settings;
        }
        [[nodiscard]] const StreamingSettings& GetStreamingSettings() const
        {
            return m_StreamingSettings;
        }
        [[nodiscard]] StreamingSettings& GetStreamingSettings()
        {
            return m_StreamingSettings;
        }

        SceneStreamer* GetSceneStreamer() const
        {
            return m_SceneStreamer.get();
        }

        DialogueVariables& GetDialogueVariables()
        {
            return m_DialogueVariables;
        }
        const DialogueVariables& GetDialogueVariables() const
        {
            return m_DialogueVariables;
        }
        DialogueSystem* GetDialogueSystem() const
        {
            return m_DialogueSystem.get();
        }

        // Navigation
        void SetNavMesh(const Ref<NavMesh>& navMesh);
        [[nodiscard]] Ref<NavMesh> GetNavMesh() const
        {
            return m_NavMesh;
        }
        [[nodiscard]] NavMeshQuery* GetNavMeshQuery()
        {
            return m_NavMeshQuery.get();
        }
        [[nodiscard]] CrowdManager* GetCrowdManager()
        {
            return m_CrowdManager.get();
        }

        // Editor-mode streamer management (allows streaming preview without entering Play mode)
        void InitializeEditorStreamer();
        void ShutdownEditorStreamer();

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Scene;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

      private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);

        void OnPhysics2DStart();
        void OnPhysics2DStop();

        void RenderScene(EditorCamera const& camera);
        void RenderScene3D(EditorCamera const& camera);
        void RenderScene3D(Camera const& camera, const glm::mat4& cameraTransform);
        void ProcessScene3DSharedLogic(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                       const glm::vec3& cameraPosition,
                                       f32 cameraNearClip, f32 cameraFarClip);
        void LoadAndRenderSkybox();
        void RenderParticleSystems(const glm::vec3& camPos, f32 nearClip, f32 farClip);
        void RenderUIOverlay();
        void ProcessSnowDeformers(Timestep ts, TMap<u64, glm::vec3>& prevPositions);

      private:
        entt::registry m_Registry;
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
        glm::mat4 m_CameraViewProjection{ 1.0f }; // Cached for UI world-anchor projection
        bool m_IsRunning = false;
        bool m_IsPaused = false;
        bool m_PendingReload = false;
        int m_StepFrames = 0;
        u64 m_TerrainFrameCounter = 0;
        u64 m_StreamingFrameCounter = 0;
        bool m_Is3DModeEnabled = false;                        // Toggle for 3D rendering mode
        bool m_RenderingEnabled = true;                        // Skip rendering when throttled
        bool m_ShowGrid = true;                                // Viewport grid visibility
        bool m_ShowLightGizmos = true;                         // Light gizmo visibility
        f32 m_GridSpacing = 1.0f;                              // Viewport grid spacing
        bool m_PreviousMouseButtonDown = false;                // Track mouse state for UI input
        bool m_UILayoutResolvedThisFrame = false;              // Guard against double ResolveLayout per frame
        glm::vec2 m_RuntimeCameraLastMouse{ 0.0f, 0.0f };      // FPS fly-camera mouse tracking
        SkeletonVisualizationSettings m_SkeletonVisualization; // Editor skeleton visualization
        PostProcessSettings m_PostProcessSettings;             // Post-processing settings
        SnowSettings m_SnowSettings;                           // Snow rendering settings
        FogSettings m_FogSettings;                             // Fog & atmospheric scattering settings
        WindSettings m_WindSettings;                           // Wind simulation settings
        SnowAccumulationSettings m_SnowAccumulationSettings;   // Snow accumulation & deformation
        SnowEjectaSettings m_SnowEjectaSettings;               // Snow ejecta particle settings
        PrecipitationSettings m_PrecipitationSettings;         // Precipitation system settings
        StreamingSettings m_StreamingSettings;                 // Scene streaming settings

        // Per-entity previous positions for velocity estimation (snow ejecta)
        TMap<u64, glm::vec3> m_RuntimeSnowPrevPositions;
        TMap<u64, glm::vec3> m_EditorSnowPrevPositions;

        b2WorldId m_PhysicsWorld = b2_nullWorldId;
        std::unique_ptr<JoltScene> m_JoltScene;
        std::unique_ptr<SceneStreamer> m_SceneStreamer;
        std::unique_ptr<DialogueSystem> m_DialogueSystem;
        DialogueVariables m_DialogueVariables;

        // Navigation
        Ref<NavMesh> m_NavMesh;
        std::unique_ptr<NavMeshQuery> m_NavMeshQuery;
        std::unique_ptr<CrowdManager> m_CrowdManager;

        // Entity UUID -> entt::entity lookup map
        // Using TMap for O(1) lookup with better cache locality
        TMap<UUID, entt::entity> m_EntityMap;

        std::string m_Name = "Untitled";

        friend class Entity;
        friend class SceneSerializer;
        friend class SceneStreamer;
        friend class SceneHierarchyPanel;
        friend class LightProbeBaker;
        friend class SaveGameSerializer;
    };
} // namespace OloEngine
