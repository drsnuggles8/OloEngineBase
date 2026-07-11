#pragma once

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Scene/WorldOriginSettings.h"
#include "OloEngine/Scene/SpatialAcceleration.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/CrowdManager.h"

#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <box2d/id.h>

#pragma warning(push)
#pragma warning(disable : 4996)
#include <entt.hpp>
#pragma warning(pop)

namespace OloEngine
{
    class Entity;
    class MeshSource;
    class Skeleton;
    class Prefab;
    class JoltScene;
    class SceneStreamer;
    struct IKTargetComponent;
    struct SpringBoneComponent;
    struct NoiseAnimationComponent;
    struct AudioSoundGraphComponent;
    struct ClothComponent;
    class DialogueSystem;
    class GameplayEventBus;
    class UINavigation;
    class SystemScheduler;

    namespace Animation
    {
        struct SpringBoneState;
        struct NoiseAnimationState;
    } // namespace Animation

    namespace Audio
    {
        class AudioEventsManager;
        class AudioCommandRegistry;
    } // namespace Audio

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
        void RevertPrefabComponent(Entity entity, const std::string& componentName) const;
        void ApplyPrefabComponent(Entity entity, const std::string& componentName) const;
        void MarkPrefabComponentOverridden(Entity entity, const std::string& componentName) const;

        void OnRuntimeStart();
        void OnRuntimeStop();

        void OnSimulationStart();
        void OnSimulationStop();

        void OnUpdateRuntime(Timestep ts);
        // Deterministic real-time entry for windowed hosts (editor Play,
        // OloRuntime): accumulate the raw frame delta `frameTs` and advance the
        // gameplay simulation in fixed `fixedDt` steps (N catch-up steps,
        // clamped against a spiral of death), then render once at the display
        // rate. This decouples the simulation rate from the frame rate, which is
        // what makes a run reproducible and unlocks rollback/replay (issue
        // #452). Headless hosts and tests that need exact single-step control
        // keep calling OnUpdateRuntime(ts) directly. Both paths funnel through
        // the same SimulateRuntimeStep, so they advance identical state.
        void OnUpdateRuntimeFixed(Timestep frameTs, f32 fixedDt);
        void OnUpdateSimulation(Timestep ts, EditorCamera const& camera);
        void OnUpdateEditor(Timestep ts, EditorCamera const& camera);
        void OnViewportResize(u32 width, u32 height);

        // Count of fixed simulation steps executed since the scene started
        // ticking (one per gameplay tick). The addressable tick index that
        // rollback/replay netcode keys off, and the signal frame-rate-
        // independence tests use to assert two differently-paced runs took the
        // same number of steps. Reset to 0 at OnRuntimeStart.
        [[nodiscard("Store this!")]] u64 GetSimulationTick() const
        {
            return m_SimulationTick;
        }

        // ── Render interpolation (issue #502) ───────────────────────────────
        // Decouples the display rate from the fixed simulation tick. When
        // enabled, OnUpdateRuntimeFixed keeps the two most recent fixed-tick
        // states and RenderRuntime draws an interpolated pose using
        // alpha = accumulator / fixedStep as the blend factor, so motion stays
        // smooth even when the refresh rate isn't a multiple of the sim rate
        // (e.g. 60 Hz sim on a 144 Hz display). Purely a presentation concern:
        // it never mutates the persisted simulation state (poses are overwritten
        // for the draw then restored), so it does NOT affect determinism (#484).
        // On by default; the editor exposes a toggle.
        void SetRenderInterpolationEnabled(bool enabled)
        {
            m_RenderInterpolationEnabled = enabled;
            // Drop the cached snapshot pair when disabling so re-enabling doesn't
            // blend from a stale pose before the next fresh capture — until then
            // ShouldInterpolateThisFrame() falls back to the live pose.
            if (!enabled)
            {
                m_HasInterpSnapshots = false;
            }
        }
        [[nodiscard("Store this!")]] bool IsRenderInterpolationEnabled() const
        {
            return m_RenderInterpolationEnabled;
        }
        // The blend factor used for the most recent render, in [0, 1]:
        // accumulator / fixedStep after the last OnUpdateRuntimeFixed call.
        [[nodiscard("Store this!")]] f32 GetRenderInterpolationAlpha() const
        {
            return m_RenderInterpAlpha;
        }
        // The interpolated LOCAL transform matrix rendering would use for
        // `entity` this frame (lerp of the last two fixed-tick poses at the
        // current alpha). Falls back to the entity's live transform when
        // interpolation is disabled, no snapshot pair exists yet, or the entity
        // isn't present in both snapshots. Exposed for tests / diagnostics.
        [[nodiscard]] glm::mat4 GetInterpolatedLocalTransform(entt::entity entity) const;

        [[nodiscard]] u32 GetViewportWidth() const
        {
            return m_ViewportWidth;
        }
        [[nodiscard]] u32 GetViewportHeight() const
        {
            return m_ViewportHeight;
        }

        void SetViewportOffset(glm::vec2 offset)
        {
            m_ViewportOffset = offset;
        }
        [[nodiscard]] glm::vec2 GetViewportOffset() const
        {
            return m_ViewportOffset;
        }

        [[nodiscard]] Entity DuplicateEntity(Entity entity);

        [[nodiscard("Store this!")]] Entity FindEntityByName(std::string_view name);
        [[nodiscard("Store this!")]] Entity GetEntityByUUID(UUID uuid);

        void UpdateEntityName(entt::entity entity, const std::string& oldName, const std::string& newName);

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

        // IK target resolution: copies IKTargetComponent and resolves entity-linked targets.
        // Returns true if entity has IKTargetComponent; resolved result written into `out`.
        bool ResolveIKTargets(Entity entity, IKTargetComponent& out) const;

        // Spring-bone resolution: returns the entity's enabled SpringBoneComponent
        // (or nullptr) and ensures the runtime SpringBoneStateComponent exists,
        // writing a pointer to its state into `outState`.
        const SpringBoneComponent* ResolveSpringBone(Entity entity, Animation::SpringBoneState*& outState);

        // Noise-animator resolution: returns the entity's enabled
        // NoiseAnimationComponent (or nullptr) and ensures the runtime
        // NoiseAnimationStateComponent exists, writing a pointer to its state
        // into `outState`.
        const NoiseAnimationComponent* ResolveNoiseAnimation(Entity entity, Animation::NoiseAnimationState*& outState);

        [[nodiscard("Store this!")]] bool IsRunning() const
        {
            return m_IsRunning;
        }

        // Mark the scene as running without invoking the full
        // `OnRuntimeStart` lifecycle. Used by headless test harnesses
        // (Functional tests) that exercise per-tick behaviour but can't
        // call `OnRuntimeStart` because it depends on `Application::Get()`.
        // Production code should keep using `OnRuntimeStart` so the
        // physics/audio/dialogue init runs properly.
        void SetRunning(bool running) noexcept
        {
            m_IsRunning = running;
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

        // Latest world-space particle positions of a live cloth soft body (issue #460),
        // row-major in the generating grid order. Refreshed every runtime tick from the
        // Jolt soft body (GPU-free), so it is readable headless. Returns nullptr when the
        // entity has no live cloth body (edit mode, no ClothComponent, or cloth disabled).
        // Backs the cloth functional tests and any gameplay query of the draped shape.
        const std::vector<glm::vec3>* GetClothVertexPositions(UUID entityID) const;

        // Physics lifecycle (public for external scene setup)
        void OnPhysics3DStart();
        void OnPhysics3DStop();
        void OnPhysics2DStart();
        void OnPhysics2DStop();

        // Audio runtime init. Production code reaches this via
        // OnRuntimeStart (non-headless path). Exposed for headless test
        // harnesses that need a working AudioEventsManager + position
        // resolver without depending on Application::Get().
        void InitAudioRuntime();

        // Per-entity SoundGraph startup. Shared between InitAudioRuntime (loops
        // over every AudioSoundGraphComponent at OnRuntimeStart) and the
        // OnComponentAdded<AudioSoundGraphComponent> specialisation (runtime-
        // spawned entities — script-spawned, networked actors arriving mid-
        // session). Without this shared entry point, components added after
        // OnRuntimeStart stay silent until the next InitAudioRuntime.
        void InitializeAudioSoundGraph(AudioSoundGraphComponent& sgc) const;

        // DialogueSystem instantiation. Production code reaches this via
        // OnRuntimeStart. Exposed for headless test harnesses that drive
        // dialogue state machines without invoking the full runtime
        // lifecycle (which would also wire up scripting, networking, etc.).
        void InitDialogueSystem();

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
        void SetWorldAxisHelperVisible(bool visible)
        {
            m_ShowWorldAxisHelper = visible;
        }
        [[nodiscard("Store this!")]] bool IsWorldAxisHelperVisible() const
        {
            return m_ShowWorldAxisHelper;
        }
        void SetCameraFrustumsVisible(bool visible)
        {
            m_ShowCameraFrustums = visible;
        }
        [[nodiscard("Store this!")]] bool AreCameraFrustumsVisible() const
        {
            return m_ShowCameraFrustums;
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

        // ── Floating-origin / origin-rebasing (issue #429) ──────────────────
        // Scene-level config (serialized + carried through Scene::Copy). See
        // WorldOriginSettings.h for the mechanism overview.
        void SetWorldOriginSettings(const WorldOriginSettings& settings)
        {
            m_WorldOriginSettings = settings;
            SanitizeWorldOriginSettings(m_WorldOriginSettings);
        }
        [[nodiscard]] const WorldOriginSettings& GetWorldOriginSettings() const
        {
            return m_WorldOriginSettings;
        }
        [[nodiscard]] WorldOriginSettings& GetWorldOriginSettings()
        {
            return m_WorldOriginSettings;
        }

        // The absolute (authored) world coordinate that currently maps to the
        // rebased-space origin (0,0,0). Starts at (0,0,0) and accumulates -shift
        // on every RebaseOrigin, so `absolute = rebased + GetWorldOrigin()`.
        // Runtime-only: reset to (0,0,0) at OnRuntimeStart, never serialized or
        // carried through Scene::Copy (a fresh Play session always starts at the
        // authored coordinates).
        [[nodiscard]] const glm::vec3& GetWorldOrigin() const
        {
            return m_WorldOrigin;
        }
        // Convert between the live rebased space (what every stored transform /
        // physics body holds) and the original authored absolute space. Use these
        // whenever gameplay/tools must reason in a frame the rebase must not move
        // (a persisted waypoint, a networked absolute position, a save file).
        [[nodiscard]] glm::vec3 RebasedToAbsolute(const glm::vec3& rebased) const
        {
            return rebased + m_WorldOrigin;
        }
        [[nodiscard]] glm::vec3 AbsoluteToRebased(const glm::vec3& absolute) const
        {
            return absolute - m_WorldOrigin;
        }

        // Shift every stored world position — root-entity TransformComponents,
        // 3D (Jolt) rigid bodies + terrain + character controllers, and 2D
        // (Box2D) bodies — by `shift`, atomically on the game thread, then
        // re-propagate world matrices and accumulate the origin offset. Only ROOT
        // entities' local translations move (children are parent-relative, so the
        // whole hierarchy translates uniformly). A zero (or non-finite) shift is a
        // no-op. Exposed for tests / tools; the runtime triggers it automatically.
        void RebaseOrigin(const glm::vec3& shift);

        // If rebasing is enabled and `referenceWorldPos` (in rebased space) is
        // beyond RebaseThreshold from the rebased origin, rebase by the grid-
        // snapped delta that brings it back near origin. Returns the applied
        // shift (zero when nothing was done). Called once per frame from
        // UpdateStreaming on the game thread with physics idle, so it never races
        // the parallel/worker-dispatched gameplay systems.
        glm::vec3 MaybeRebaseOrigin(const glm::vec3& referenceWorldPos);

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

        // Spatial acceleration — a uniform grid over every entity's
        // TransformComponent position, rebuilt once per runtime tick (inside
        // OnUpdateRuntime, after scripts/physics/navigation have moved entities
        // and before query consumers like AI perception run). Gameplay systems
        // use it for proximity queries instead of an O(n) scan over all
        // entities. The index is runtime-only — it reflects the most recent
        // tick and is empty before the first OnUpdateRuntime call.
        [[nodiscard]] const SceneSpatialIndex& GetSpatialIndex() const
        {
            return m_SpatialIndex;
        }

        // Convenience forwarders so gameplay code can query without reaching
        // through GetSpatialIndex(). Results are entity UUIDs; resolve them with
        // GetEntityByUUID. See SceneSpatialIndex for ordering / edge-case
        // semantics. These read the index as last rebuilt — they do NOT re-scan
        // the registry, so a query reflects positions as of the previous
        // UpdateSpatialIndex (i.e. this tick's, when called from a system that
        // runs after it).
        [[nodiscard]] std::vector<UUID> QueryEntitiesInRadius(const glm::vec3& center, f32 radius) const
        {
            return m_SpatialIndex.QueryRadius(center, radius);
        }
        [[nodiscard]] std::vector<UUID> QueryEntitiesInAABB(const glm::vec3& min, const glm::vec3& max) const
        {
            return m_SpatialIndex.QueryAABB(min, max);
        }
        [[nodiscard]] std::vector<UUID> QueryNearestEntities(const glm::vec3& center, u32 count,
                                                             f32 maxRadius = std::numeric_limits<f32>::max()) const
        {
            return m_SpatialIndex.NearestN(center, count, maxRadius);
        }

        // Rebuild the spatial index from the live TransformComponent positions.
        // Called automatically once per OnUpdateRuntime tick; exposed so headless
        // harnesses / tools can refresh it after mutating transforms outside the
        // tick (e.g. a unit test that places entities then queries immediately).
        void UpdateSpatialIndex();

        // Compose parent-chain world matrices for every entity with a
        // TransformComponent in one flat, depth-sorted sweep (issue #499):
        // roots first, breadth-first over RelationshipComponent::m_Children,
        // so a parent's WorldTransformComponent is always written before any
        // child that reads it. Called automatically once per tick (runtime,
        // simulation, and editor-preview updates); exposed so headless
        // harnesses / tests can refresh world transforms after reparenting or
        // mutating local transforms outside a tick.
        void PropagateWorldTransforms();

        // Reads the composed world matrix written by PropagateWorldTransforms()
        // for a raw entt handle (render/submission loops iterate EnTT views
        // directly rather than through the Entity wrapper). Falls back to the
        // local transform if the propagation pass hasn't run yet this tick.
        // Defined out-of-line in Scene.cpp: TransformComponent / WorldTransformComponent
        // are only forward-declared here (full definitions live in Components.h).
        [[nodiscard("Store this!")]] glm::mat4 GetWorldTransform(entt::entity entity) const;

        // Audio Events
        [[nodiscard]] Audio::AudioCommandRegistry* GetAudioCommandRegistry()
        {
            return m_AudioCommandRegistry.get();
        }

        // Gameplay event dispatcher — quest/inventory systems publish their
        // POD notification payloads here; UI / audio / scripting subscribe.
        // Always non-null (constructed in the Scene ctor). See GameplayEventBus.h.
        // Defined out-of-line so Scene.h only needs the forward declaration.
        [[nodiscard]] GameplayEventBus& GetGameplayEvents();
        [[nodiscard]] const GameplayEventBus& GetGameplayEvents() const;

        // Runtime UI navigation + widget-event state (focus target, OnClick /
        // OnValueChanged / OnSubmit delegates). Runtime-only — never serialized
        // or copied, Clear()ed on OnRuntimeStop. Driven by UINavigationSystem
        // each OnUpdateRuntime tick. Always non-null (constructed in the ctor).
        // Defined out-of-line so Scene.h only needs the forward declaration.
        [[nodiscard]] UINavigation& GetUINavigation();
        [[nodiscard]] const UINavigation& GetUINavigation() const;

        // Recover the entity that owns a component instance stored in this
        // scene's registry. Returns a null Entity if `component` is not one of
        // this registry's T components. Used by the scripting glue, which binds
        // methods on component references without a handle back to the owning
        // entity (needed to stamp gameplay events with the entity UUID).
        // O(n) in the number of live T components — fine at script-call rates.
        template<typename T>
        [[nodiscard]] Entity GetEntityForComponent(const T& component);

        // Editor-mode streamer management (allows streaming preview without entering Play mode)
        void InitializeEditorStreamer();
        void ShutdownEditorStreamer();

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Scene;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

      private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);

        // Called by Entity::RemoveComponent before the component is erased
        // from the registry, so subsystem teardown can read runtime state
        // (e.g. m_RuntimeBodyToken for Rigidbody3DComponent). The primary
        // template is intentionally declaration-only — every component type
        // that supports RemoveComponent<T>() has an explicit specialisation
        // in Scene.cpp, mirroring the OnComponentAdded pattern. The compiler
        // emits an unresolved-symbol error for any component type that adds
        // RemoveComponent<T>() callsites without a matching specialisation,
        // forcing the engine author to acknowledge the new component's
        // teardown semantics.
        template<typename T>
        void OnComponentRemoved(Entity entity, T& component);

        void RenderScene(EditorCamera const& camera);
        void RenderScene3D(EditorCamera const& camera);
        void RenderScene3D(Camera const& camera, const glm::mat4& cameraTransform);
        void ProcessScene3DSharedLogic(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                       const glm::vec3& cameraPosition,
                                       f32 cameraNearClip, f32 cameraFarClip);
        void LoadAndRenderSkybox();
        // Override the global IBL when a baked reflection probe contains the
        // camera. Falls through (no-op) when no probe applies, leaving the
        // EnvironmentMapComponent IBL set by LoadAndRenderSkybox in place.
        void ApplyReflectionProbeOverride(const glm::vec3& cameraPosition);
        void RenderParticleSystems(const glm::vec3& camPos, f32 nearClip, f32 farClip);
        void RenderUIOverlay();
        void ProcessSnowDeformers(Timestep ts, TMap<u64, glm::vec3>& prevPositions);

        // Sub-stages of the runtime tick, shared by OnUpdateRuntime (single
        // step) and OnUpdateRuntimeFixed (accumulated fixed steps). Splitting
        // them lets the windowed loop run the simulation N times per displayed
        // frame while rendering exactly once (issue #452).
        //   UpdateStreaming     — locale refresh + scene streaming; once per frame.
        //   SimulateRuntimeStep — one gameplay tick (scripts, physics, AI, …);
        //                         advances by exactly `ts` and bumps the tick
        //                         counter. The pause / single-step gate lives in
        //                         the callers, not here.
        //   RenderRuntime       — camera resolve + UI layout + draw; once per frame.
        void UpdateStreaming();
        void SimulateRuntimeStep(Timestep ts);
        void RenderRuntime(Timestep ts);

        // ── Render interpolation snapshots (issue #502) ─────────────────────
        // A per-entity local-transform pose captured at a fixed-tick boundary.
        // Deliberately NOT named *Component so it never gets swept into the
        // generated ECS/serializer tuples — it is transient render state.
        struct InterpTransform
        {
            glm::vec3 Translation{ 0.0f };
            glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec3 Scale{ 1.0f };
        };
        // Snapshot every entity's live local TransformComponent into `out`
        // (clearing prior contents). Keyed by the raw entt entity id.
        void CaptureLocalTransforms(std::unordered_map<u32, InterpTransform>& out);
        // Compose the interpolated pose for `entity` at the current alpha
        // (m_RenderInterpAlpha) into `out`. Returns false (leaving `out`
        // untouched) when interpolation is disabled, no snapshot pair exists, or
        // the entity is absent from one of the snapshots — callers then use the
        // live transform.
        [[nodiscard]] bool ComputeInterpolatedLocal(entt::entity entity, InterpTransform& out) const;
        // True when this frame should render interpolated poses: the toggle is
        // on, a snapshot pair exists, and m_RenderInterpAlpha > 0.0f (alpha == 0
        // renders the current pose verbatim, so the overwrite is skipped).
        [[nodiscard]] bool ShouldInterpolateThisFrame() const;
        // Step 2D (Box2D) + 3D (Jolt, incl. m_SimulationTime-driven buoyancy)
        // physics one tick and sync the results back onto the ECS transforms
        // (Rigidbody2D / Rigidbody3D / CharacterController3D). This is the
        // SYNCHRONOUS whole-step path, used by the editor Simulate-mode tick
        // (OnUpdateSimulation). The runtime gameplay schedule instead drives the
        // kick/fence split below so the ECS-free world step can overlap the
        // physics-shadow systems; both paths share PostPhysicsSync and the
        // JoltScene phase methods, so they cannot drift. The caller advances
        // m_SimulationTime before calling.
        void StepPhysics(Timestep ts);

        // ── Async physics split (issue #453, UE TG_DuringPhysics analog) ────────
        // KickPhysicsStep — GAME THREAD. Queues buoyancy forces, drains queued
        //   contact events, advances the fixed-step accumulator, runs the ECS-
        //   reading character/vehicle phase, then launches the ECS-free world
        //   step (Box2D + Jolt world update) as an engine task. Falls back to
        //   fully-synchronous stepping when the frame authorizes != 1 fixed step
        //   (idle / hitch catch-up), when parallel execution is disabled, or
        //   when there is no 3D physics scene — the fallback is bit-identical to
        //   StepPhysics' stepping order.
        // FencePhysicsStep — GAME THREAD. Joins the in-flight world step, runs
        //   the joint-break phase (ECS reads + event publish, per-step impulse
        //   state must be consumed before the next world update), writes body
        //   poses back to the ECS transforms, and runs PostPhysicsSync. Every
        //   downstream transform consumer is ordered after this node by the
        //   derived graph. Between kick and fence, only the physics-shadow
        //   systems (see GetGameplayScheduler) run — on the game thread, so they
        //   need no worker-thread-safety audit, only physics-independence.
        void KickPhysicsStep(Timestep ts);
        void FencePhysicsStep();
        // Cloth readback + Box2D / Jolt rigid-body / character-controller ECS
        // transform sync — the tail shared verbatim by StepPhysics (sync path)
        // and FencePhysicsStep (async path).
        void PostPhysicsSync();

        // ── Gameplay systems that make up one SimulateRuntimeStep tick ──────────
        // Each is one node in the declarative dependency graph (issue #453); the
        // execution order is DERIVED from the read/write + before/after
        // constraints declared in GetGameplayScheduler(), not from the order these
        // are called here. Bodies are the historical hard-coded blocks, moved out
        // of SimulateRuntimeStep verbatim so the derived sequential run is a
        // bit-for-bit no-op. See SystemScheduler.h.
        void UpdateScripts(Timestep ts);         // C# + Lua entity OnUpdate
        void UpdateCinematics(Timestep ts);      // authored sequence playback
        void UpdateDialogue(Timestep ts);        // dialogue runner
        void UpdateAnimation(Timestep ts);       // skeletal + morph-only sampling
        void UpdateAnimationGraphs(Timestep ts); // animation state machines
        void EvaluateMorphTargets();             // deform meshes from morph weights
        void UpdateNavigation(Timestep ts);      // pathfinding / crowds
        void UpdatePerception(Timestep ts);      // AI sight sensing
        void UpdateAI(Timestep ts);              // behavior trees / FSM / GOAP
        void UpdateInventory(Timestep ts);       // pickups / despawn
        void UpdateQuest(Timestep ts);           // quest timers / conditions
        void UpdateAbilities(Timestep ts);       // gameplay ability system
        void UpdateAudio(Timestep ts);           // listener/source pose sync + events
        // Particle update is split by GPU usage (issue #576): the CPU partition
        // is worker-dispatchable (Parallelizable), the GPU partition stays on the
        // game thread because it issues GL compute. UpdateParticlesPartition does
        // the shared camera-LOD + group walk; each wrapper picks its partition.
        void UpdateParticlesCPU(Timestep ts); // CPU-only systems (worker-safe)
        void UpdateParticlesGPU(Timestep ts); // GPU / GL-compute systems (game thread)
        void UpdateParticlesPartition(Timestep ts, bool gpuPartition);
        void UpdateSnowDeformers(Timestep ts); // snow deformation stamps + ejecta

      public:
        // The process-wide gameplay system schedule. Built once (thread-safe
        // function-local static), shared across every Scene / tick because each
        // system's exec callback takes the Scene by reference and captures nothing
        // instance-specific. Building it derives the execution order and throws
        // SystemSchedulerError if the authored graph is cyclic or references an
        // unknown system. Public for tests / diagnostics (DependsOn seam queries);
        // the systems it runs are still private Scene members.
        static SystemScheduler& GetGameplayScheduler();

        // The derived gameplay-system execution order, for tests / diagnostics.
        // Proves the scheduler reproduces the historical hard-coded sequence.
        static const std::vector<std::string>& GetGameplaySystemOrderForTesting();

      private:
      private:
        entt::registry m_Registry;
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
        glm::vec2 m_ViewportOffset{ 0.0f, 0.0f };
        glm::mat4 m_CameraViewProjection{ 1.0f }; // Cached for UI world-anchor projection
        bool m_IsRunning = false;
        bool m_IsPaused = false;
        bool m_PendingReload = false;
        int m_StepFrames = 0;
        u64 m_TerrainFrameCounter = 0;
        u64 m_StreamingFrameCounter = 0;
        // Fixed-timestep accumulator for OnUpdateRuntimeFixed: leftover real
        // time (< fixedDt) carried into the next frame. Tick counter increments
        // once per SimulateRuntimeStep.
        f32 m_FixedTimeAccumulator = 0.0f;
        u64 m_SimulationTick = 0;
        // Render interpolation (issue #502). The two most recent fixed-tick
        // local-transform poses (m_InterpPrev = one tick behind m_InterpCurr),
        // the blend factor for the last render (accumulator / fixedDt), whether
        // a valid snapshot pair exists yet, and the enable toggle (on by
        // default). Maps are keyed by the raw entt entity id; cleared/refilled
        // each capture so destroyed entities drop out naturally.
        std::unordered_map<u32, InterpTransform> m_InterpPrev;
        std::unordered_map<u32, InterpTransform> m_InterpCurr;
        f32 m_RenderInterpAlpha = 0.0f;
        bool m_HasInterpSnapshots = false;
        bool m_RenderInterpolationEnabled = true;
        // Deterministic simulation clock (seconds), advanced by exactly `ts` per
        // sim tick, used for time-driven physics (buoyancy wave phase) so it is
        // reproducible across frame pacings / rollback instead of wall-clock.
        // Reset to 0 at OnRuntimeStart.
        f32 m_SimulationTime = 0.0f;
        // Spiral-of-death cap: most fixed steps a single frame may run before
        // the accumulator is clamped and excess wall-time dropped. 15 mirrors
        // Application::s_MaxTimestep (0.25 s) at the 60 Hz default.
        static constexpr u32 kMaxFixedStepsPerFrame = 15;
        // Last-observed LocalizationManager generation. LocalizationSystem
        // compares against this to skip the LocalizedTextComponent sweep when
        // nothing's changed. Starts at 0 so the first tick always refreshes.
        u64 m_LocalizationGeneration = 0;
        friend class LocalizationSystem;
        bool m_Is3DModeEnabled = false;                        // Toggle for 3D rendering mode
        bool m_RenderingEnabled = true;                        // Skip rendering when throttled
        bool m_ShowGrid = true;                                // Viewport grid visibility
        bool m_ShowLightGizmos = true;                         // Light gizmo visibility
        bool m_ShowWorldAxisHelper = true;                     // World-origin XYZ axes visibility
        bool m_ShowCameraFrustums = true;                      // Per-CameraComponent frustum gizmo visibility
        f32 m_GridSpacing = 1.0f;                              // Viewport grid spacing
        f32 m_LastAnimationTime = -1.0f;                       // Tracks previous-frame animation time for wind/water/foliage velocity reprojection
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
        WorldOriginSettings m_WorldOriginSettings;             // Floating-origin / rebase config (issue #429)
        // Runtime-only origin accumulator: absolute = rebased + m_WorldOrigin.
        // Reset to (0,0,0) at OnRuntimeStart; never serialized or copied.
        glm::vec3 m_WorldOrigin{ 0.0f };

        // Per-entity previous positions for velocity estimation (snow ejecta)
        TMap<u64, glm::vec3> m_RuntimeSnowPrevPositions;
        TMap<u64, glm::vec3> m_EditorSnowPrevPositions;

        b2WorldId m_PhysicsWorld = b2_nullWorldId;
        std::unique_ptr<JoltScene> m_JoltScene;

        // In-flight async physics world step (issue #453): launched by
        // KickPhysicsStep, joined by FencePhysicsStep within the SAME tick — it
        // never outlives a SimulateRuntimeStep call, so no teardown handling is
        // needed. Invalid whenever the kick took the synchronous fallback.
        Tasks::TTask<void> m_PhysicsStepTask;
        // Fixed steps the current frame authorized (BeginSteps) and whether the
        // kick launched the world step asynchronously — the fence uses these to
        // run the deferred joint-break phase only on the async single-step path.
        u32 m_PhysicsStepsThisFrame = 0;
        bool m_PhysicsStepRanAsync = false;

        // Cloth soft-body runtime state (issue #460), keyed by the owning ClothComponent
        // entity. Built at OnPhysics3DStart, torn down at OnPhysics3DStop; never serialized
        // or copied. m_Positions / m_Normals are CPU buffers refreshed each runtime tick from
        // JoltScene::GetClothVertices (GPU-free — functional tests read them, the dedicated
        // server needs no GL). m_RenderMesh is the deforming render mesh, built and its VBO
        // updated lazily only inside the GL render pass; a headless run leaves it null.
        struct ClothRuntimeState
        {
            Ref<MeshSource> m_RenderMesh;
            std::vector<glm::vec3> m_Positions;
            std::vector<glm::vec3> m_Normals;
            u32 m_Columns = 0;
            u32 m_Rows = 0;

            // Skeleton attachment (issue #460 cape slice). Resolved once at physics start
            // from ClothComponent::m_AttachmentEntity / m_AttachmentBone. When inactive the
            // cloth's pinned vertices stay welded to the world (pre-cape behaviour).
            bool m_AttachmentActive = false;
            UUID m_AttachEntity = 0;             // entity carrying the SkeletonComponent to follow
            i32 m_AttachBoneIndex = -1;          // -1 = use m_AttachEntity's own world transform
            std::vector<u32> m_AttachedVertices; // pinned particle indices, into m_Positions order
            // Each pinned vertex's rest position expressed in the resolved bone's local
            // frame at bind time. target_world = boneWorld_now * m_AttachedLocalOffsets[i].
            std::vector<glm::vec3> m_AttachedLocalOffsets;
        };
        std::unordered_map<UUID, ClothRuntimeState> m_ClothRuntime;

        // ── Cloth skeleton attachment (issue #460 cape slice) ──────────────────
        // Declared here (not up by PostPhysicsSync) because they reference the
        // ClothRuntimeState defined just above. SetupClothAttachment resolves
        // ClothComponent::m_AttachmentEntity / m_AttachmentBone into a ClothRuntimeState
        // (bone index + the pinned-vertex weld offsets), once, at physics start.
        // DriveClothAttachments runs each tick BEFORE the physics step (next to
        // ClothWindSystem::OnUpdate): for every attached cloth it looks up the bone's
        // current world transform and drives the pinned particles' velocities toward
        // their welded targets so the cloth follows the animation. `dt` is the frame
        // delta the physics step will advance. ResolveClothAttachmentTransform composes
        // the bone's world matrix (entity world transform × skeleton global bone
        // transform); returns false if the attachment entity has vanished.
        void SetupClothAttachment(Entity clothEntity, const ClothComponent& cloth, ClothRuntimeState& state);
        void DriveClothAttachments(f32 dt);
        [[nodiscard]] bool ResolveClothAttachmentTransform(const ClothRuntimeState& state, glm::mat4& outBoneWorld) const;

        std::unique_ptr<SceneStreamer> m_SceneStreamer;
        std::unique_ptr<DialogueSystem> m_DialogueSystem;
        DialogueVariables m_DialogueVariables;

        // Navigation
        Ref<NavMesh> m_NavMesh;
        std::unique_ptr<NavMeshQuery> m_NavMeshQuery;
        std::unique_ptr<CrowdManager> m_CrowdManager;

        // Spatial acceleration (runtime-only; rebuilt each OnUpdateRuntime tick,
        // never serialized/copied). See GetSpatialIndex / UpdateSpatialIndex.
        SceneSpatialIndex m_SpatialIndex;

        // Scratch buffers for PropagateWorldTransforms (issue #499) — persistent
        // across ticks and .clear()ed at the top of each call instead of being
        // reconstructed/reserved from scratch, so the flat BFS sweep doesn't
        // pay a fresh heap allocation for every tick.
        std::vector<entt::entity> m_TransformOrder;
        std::unordered_set<entt::entity> m_TransformVisited;
        std::vector<entt::entity> m_TransformQueue;

        // Audio Events
        std::unique_ptr<Audio::AudioCommandRegistry> m_AudioCommandRegistry;
        std::unique_ptr<Audio::AudioEventsManager> m_AudioEventsManager;

        // Gameplay event dispatcher (runtime-only; never serialized/copied)
        std::unique_ptr<GameplayEventBus> m_GameplayEventBus;

        // Runtime UI navigation state (runtime-only; never serialized/copied)
        std::unique_ptr<UINavigation> m_UINavigation;

        // Entity UUID -> entt::entity lookup map
        // Using TMap for O(1) lookup with better cache locality
        TMap<UUID, entt::entity> m_EntityMap;

        // Entity name -> entt::entity fast lookup cache
        // Uses multimap to handle duplicate entity names correctly.
        // Maintained by CreateEntityWithUUID, DestroyEntity, and UpdateEntityName.
        std::unordered_multimap<std::string, entt::entity> m_EntityNameMap;

        std::string m_Name = "Untitled";

        friend class Entity;
        friend class SceneSerializer;
        friend class SceneStreamer;
        friend class SceneHierarchyPanel;
        friend class LightProbeBaker;
        friend class ReflectionProbeBaker;
        friend class SaveGameSerializer;
    };
} // namespace OloEngine
