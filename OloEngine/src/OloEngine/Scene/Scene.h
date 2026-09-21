#pragma once

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Physics3D/BoatWakeSystem.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Core/FrameTimeTail.h"
#include "OloEngine/Scene/AnimalScheduler.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Scene/WorldOriginSettings.h"
#include "OloEngine/Scene/SceneLightmap.h"
#include "OloEngine/Scene/SpatialAcceleration.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/CrowdManager.h"
// The groom deformation's per-entity runtime state is held below, and
// GroomRootTransform / GroomHistoryResetCause are members of it rather than
// pointers to it — so the complete type is needed here, not a declaration.
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomBodyCollider.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomStrandRequest.h"
// Scene builds the groom/target signatures itself to answer
// GroomBindingAsset::CheckCompatibility before it deforms, so the builder is a
// complete type here rather than the declaration GroomBinding.h leaves.
#include "OloEngine/Groom/GroomBindingBuilder.h"

#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <box2d/id.h>

#pragma warning(push)
#pragma warning(disable : 4996)
#include <entt/entt.hpp>
#pragma warning(pop)

namespace OloEngine
{
    class Entity;
    class MeshSource;
    class Skeleton;
    class Prefab;
    class JoltScene;
    class FluidWorld;
    class SceneStreamer;
    struct IKTargetComponent;
    struct SpringBoneComponent;
    struct NoiseAnimationComponent;
    struct FootIKComponent;
    struct FootIKStateComponent;
    struct AudioSoundGraphComponent;
    struct ClothComponent;
    // The groom binding's two public surface resolvers take these; a
    // declaration is enough because both are passed by pointer or reference
    // and this header must not pull Components.h in behind it.
    struct MeshComponent;
    struct LODGroupComponent;
    struct SkeletonComponent;
    class DialogueSystem;
    class SubtitleSystem;
    class GameplayEventBus;
    class UINavigation;
    class SystemScheduler;
    struct FlockingWorkspace;
    struct StructuralGraph;

    namespace VisualScript
    {
        class VisualScriptSystem;
    } // namespace VisualScript

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

        // Release the process-wide render defaults Scene.cpp caches lazily (currently
        // the fallback PBR material every un-materialed submesh draws with). Not
        // per-scene state, so ~Scene() is the wrong place; called from
        // ~Application after Project::Unload() and before Renderer::Shutdown(), i.e.
        // the narrowest teardown that is unconditional for every session that can
        // create them and still has a live graphics context (#839).
        // See docs/agent-rules/lazy-static-release-ownership.md.
        static void ReleaseSharedRenderDefaults();

        [[nodiscard("Store this!")]] Entity CreateEntity(const std::string& name = std::string());
        [[nodiscard("Store this!")]] Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
        void DestroyEntity(Entity entity);

        // Destroy `entity` together with its whole descendant subtree (children
        // first, root last) and unlink it from its parent's child list. Plain
        // DestroyEntity leaves children behind as orphans — correct for the
        // editor's delete-one-node semantics, wrong for gameplay, where a
        // destroyed prefab instance must take its hierarchy with it. This is
        // what the deferred script destroy applies (issue #643).
        void DestroyEntityAndChildren(Entity entity);

        // Prefab instantiation
        [[nodiscard("Store this!")]] Entity Instantiate(AssetHandle prefabHandle);
        [[nodiscard("Store this!")]] Entity InstantiateWithUUID(AssetHandle prefabHandle, UUID uuid);

        // ── Script-driven runtime spawning (issue #643) ──────────────────────
        // The three structural operations gameplay scripts (C# + Lua) may ask
        // for. All three are DEFERRED: the request is appended to a command
        // queue and applied by FlushPendingEntityCommands at a drain point that
        // is guaranteed to sit outside every EnTT view/group iteration.
        //
        // Why deferral is mandatory, not defensive: Scene::UpdateScripts walks
        // `m_Registry.view<ScriptComponent>()` and `view<LuaScriptComponent>()`
        // while dispatching OnUpdate. A prefab whose root carries a
        // ScriptComponent pushes into the very pool being iterated, and a
        // destroy swap-and-pops out of it — both invalidate the live iterator.
        // The same hazard exists for a script callback invoked from any other
        // system's iteration (dialogue actions, UI button handlers, gameplay
        // event subscribers), which is why the rule is "always defer" rather
        // than "defer only inside UpdateScripts": one uniform contract cannot
        // be broken by adding a new script-invoking call site.
        //
        // Spawn calls pre-allocate and return the new entity's UUID
        // immediately, so a script can store the handle in the same call; the
        // entity itself materialises at the next drain. That is why the spawn
        // transform is part of the request rather than something the caller
        // pokes in afterwards.
        //
        // Thread-safety: the REQUEST side (these three calls) is mutex-guarded,
        // so concurrent callers cannot corrupt the queue. That alone does NOT
        // make the surface worker-safe: the APPLY side performs EnTT structural
        // changes and is game-thread-only, and IsEntityLiveForScripts falls
        // through to an unlocked m_EntityMap read. Marking the Scripts node
        // .Parallelizable() therefore needs more than this mutex — see the
        // drain-placement note in Scene::UpdateScripts.
        [[nodiscard("The spawned entity's UUID is the only handle to it")]] UUID
        ScriptCreateEntity(const std::string& name, const glm::vec3& translation);
        [[nodiscard("The spawned entity's UUID is the only handle to it")]] UUID
        ScriptInstantiatePrefab(AssetHandle prefabHandle, const glm::vec3& translation,
                                const glm::vec3& rotationEuler, const glm::vec3& scale);
        void ScriptDestroyEntity(UUID entityID);

        // Apply every queued command, in request order. Safe to call with an
        // empty queue (the common case — one cheap flag read). Re-entrant calls
        // are ignored: a spawned entity's OnCreate may queue further commands,
        // which the drain picks up in a following round rather than nesting.
        void FlushPendingEntityCommands();

        // Discard every queued command without applying it. Called at runtime
        // stop so commands from the last tick of a session cannot leak into the
        // next one.
        void ClearPendingEntityCommands();

        [[nodiscard("Store this!")]] sizet GetPendingEntityCommandCount() const;

        // A UUID handed out by ScriptCreateEntity / ScriptInstantiatePrefab
        // whose entity has not been materialised yet.
        [[nodiscard("Store this!")]] bool IsEntitySpawnPending(UUID entityID) const;
        // A live entity that a script has already asked to destroy.
        [[nodiscard("Store this!")]] bool IsEntityDestroyPending(UUID entityID) const;
        // The script-facing liveness answer: true while the handle refers to an
        // entity that exists or is about to, false once it has been destroyed
        // or asked to be. This is what Entity.IsValid reports so a script sees
        // its own spawn/destroy requests reflected immediately rather than one
        // drain later.
        [[nodiscard("Store this!")]] bool IsEntityLiveForScripts(UUID entityID) const;

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

        /// This frame's multi-animal scheduling telemetry (issue #1258): what
        /// the population asked for, what it was given, which axis dominates,
        /// and whether the budget could be met at all.
        ///
        /// Read by the editor's statistics panel and by the evidence tests.
        /// Zeroed every frame the scheduler runs, so a stale read is a frame of
        /// zeroes rather than a frame of last frame's numbers.
        [[nodiscard]] const AnimalSchedulerStats& GetAnimalSchedulerStats() const noexcept
        {
            return m_AnimalSchedulerStats;
        }

        /// This frame's per-animal decisions, keyed by entity UUID. Empty when
        /// nothing in the scene carries an AnimalBudgetComponent.
        [[nodiscard]] const std::unordered_map<UUID, AnimalSchedule>& GetAnimalSchedules() const noexcept
        {
            return m_AnimalSchedules;
        }

        /// The rolling frame-time distribution — p50/p95/p99/max plus a count
        /// of frames over `budgetMs` (#1258, criterion 4).
        ///
        /// THE TAIL IS THE POINT, not the mean. Amortising a population's work
        /// across frames does not remove it, and badly phased it makes the 99th
        /// percentile WORSE while every average improves — so this is the
        /// statistic the budget has to be judged on. Accumulated whether or not
        /// the budget is enabled, so the off arm is a usable control.
        [[nodiscard]] FrameTimeTailStats GetFrameTimeTail(f32 budgetMs = 0.0f) const
        {
            return m_FrameTimeTail.Query(budgetMs);
        }

        /// Drop the frame-time window. Called across a discontinuity — a scene
        /// load, a play-mode transition, a resolution change — because a window
        /// spanning two different configurations describes neither.
        void ResetFrameTimeTail()
        {
            m_FrameTimeTail.Reset();
        }

        // Deterministic simulation clock (seconds since OnRuntimeStart), advanced
        // by exactly one timestep per gameplay tick. This — NOT wall-clock
        // Time::GetTime — is the phase every time-driven physics system samples
        // the water surface with, so floating/driven bodies are reproducible
        // across frame pacings and rollback re-sim (issue #452). Exposed for the
        // scheduler-registered force systems (Boat) that need the wave clock.
        [[nodiscard("Store this!")]] f32 GetSimulationTime() const
        {
            return m_SimulationTime;
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

        // Resolve ground-adaptation foot/hand IK for the entity (issue #631
        // part 3): lazily adds the runtime FootIKStateComponent, refreshes the
        // per-foot ground cache from Jolt raycasts (probing from LAST tick's
        // foot pose — the animation systems run pre-PhysicsKick where Jolt
        // queries are legal), and resolves hand target entity overrides.
        // Returns nullptr (outState untouched) when the entity has no enabled
        // FootIKComponent.
        const FootIKComponent* ResolveFootIK(Entity entity, FootIKStateComponent*& outState);

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
            // A reload and a load are the same request with different targets,
            // so the newer one wins rather than both firing (see
            // SetPendingSceneLoad).
            if (pending)
            {
                m_PendingSceneLoad.clear();
                m_PendingSceneLoadSaveSlot.clear();
            }
        }

        // --- Runtime scene transitions (issue #642) --------------------------
        //
        // A script asks for a scene change by path; the HOST picks the request
        // up after the tick has returned and performs the swap (OloRuntime's
        // RuntimeLayer, or the editor's Play mode). Scene deliberately does not
        // load anything itself: tearing the registry down from inside
        // OnUpdateRuntime would destroy the very scene mid-iteration, which is
        // the same hazard SetPendingReload exists to avoid. This generalizes
        // that mechanism from "reload m_ScenePath" to "load an arbitrary path".
        //
        // The request is a raw, script-supplied string ("Level2",
        // "Level2.olo", "Scenes/Level2.olo"); resolving it against the game's
        // scene directory is the host's job — see SceneTransition::ResolveScenePath.
        //
        // Reload and load are mutually exclusive: whichever was requested last
        // during a tick is the one that happens.
        [[nodiscard("Store this!")]] bool HasPendingSceneLoad() const
        {
            return !m_PendingSceneLoad.empty();
        }
        [[nodiscard("Store this!")]] const std::string& GetPendingSceneLoad() const
        {
            return m_PendingSceneLoad;
        }
        [[nodiscard("Store this!")]] const std::string& GetPendingSceneLoadSaveSlot() const
        {
            return m_PendingSceneLoadSaveSlot;
        }
        void SetPendingSceneLoad(std::string_view path)
        {
            m_PendingSceneLoad.assign(path);
            m_PendingSceneLoadSaveSlot.clear();
            if (!m_PendingSceneLoad.empty())
            {
                m_PendingReload = false;
            }
        }
        // Continue-game transition: the host loads the target scene, restores
        // `saveSlot` into that not-yet-running scene, and only then performs the
        // normal stop -> swap -> start sequence. Pairing the slot with the scene
        // request prevents a process-global request from leaking into a later
        // unrelated transition.
        void SetPendingSceneLoadFromSave(std::string_view path, std::string_view saveSlot)
        {
            if (path.empty() || saveSlot.empty())
            {
                return;
            }
            m_PendingSceneLoad.assign(path);
            m_PendingSceneLoadSaveSlot.assign(saveSlot);
            m_PendingReload = false;
        }
        void ClearPendingSceneLoad()
        {
            m_PendingSceneLoad.clear();
            m_PendingSceneLoadSaveSlot.clear();
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

        // Structural connectivity for progressive collapse (issue #786): the
        // support graph over this scene's StructuralNodeComponent pieces, built
        // lazily and kept across ticks by DestructibleSystem. Runtime-only —
        // never serialized, never copied by Scene::Copy; it is fully rederived
        // from the components.
        [[nodiscard]] StructuralGraph& GetStructuralGraph() const
        {
            return *m_StructuralGraph;
        }

        // Fluid access (issue #630): the per-scene registry of PBF solver
        // instances, created lazily by FluidSystem on the first tick that sees
        // a FluidComponent. Defined out-of-line (FluidWorld is only
        // forward-declared here).
        [[nodiscard]] FluidWorld& GetFluidWorld();
        [[nodiscard]] FluidWorld* TryGetFluidWorld() const
        {
            return m_FluidWorld.get();
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

        // Terrain sculpt/erosion collision sync (issue #469). After a sculpt or erosion
        // edit mutates a single-tile terrain's CPU height field over the given sample
        // rect ([regionX, regionX+regionWidth) columns × [regionZ, regionZ+regionHeight)
        // rows), refresh the static height-field collision body so a dropped body rests on
        // the NEW surface. No-op unless physics is running and the terrain has a live
        // collision body (edit mode has no JoltScene). Streamed terrains are not sculpted,
        // so their per-tile bodies are managed by the streaming reconcile instead. Pass the
        // whole edited region (or the accumulated stroke rect); it is snapped to Jolt's
        // block grid internally. Debounce at the call site — one call per stroke settle,
        // not per drag frame. Returns true if a collision body was updated.
        bool UpdateTerrainCollisionAfterEdit(Entity terrainEntity, u32 regionX, u32 regionZ,
                                             u32 regionWidth, u32 regionHeight);

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

        // VisualScriptSystem instantiation (issue #634). Same rationale as
        // InitDialogueSystem: production code reaches it via OnRuntimeStart, and
        // headless harnesses need it without the rest of the runtime lifecycle
        // (which requires Application::Get()). Idempotent.
        void InitVisualScriptRuntime();

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

        void SetLightmapSettings(const SceneLightmapSettings& settings)
        {
            m_LightmapSettings = settings;
            if (m_LightmapRuntime)
            {
                m_LightmapRuntime->Invalidate();
            }
        }
        [[nodiscard]] const SceneLightmapSettings& GetLightmapSettings() const
        {
            return m_LightmapSettings;
        }
        [[nodiscard]] SceneLightmapSettings& GetLightmapSettings()
        {
            return m_LightmapSettings;
        }
        // The runtime-resolved lightmap state; created lazily. Never null after
        // this call, but only IsValid() once Resolve() found a non-stale bake.
        // Non-const Ref on purpose: Ref<T> propagates const through operator->,
        // and callers must Resolve()/Invalidate() the runtime.
        [[nodiscard]] Ref<SceneLightmapRuntime>& GetLightmapRuntime()
        {
            if (!m_LightmapRuntime)
            {
                m_LightmapRuntime = Ref<SceneLightmapRuntime>::Create();
            }
            return m_LightmapRuntime;
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

        // Node-graph gameplay logic (issue #634). Runtime-only: created at
        // OnRuntimeStart, destroyed at OnRuntimeStop, never serialized or copied
        // with the scene. Null outside a runtime session.
        [[nodiscard("Store this!")]] VisualScript::VisualScriptSystem* GetVisualScripts() const
        {
            return m_VisualScriptSystem.get();
        }

        // Caption overlay (issue #458). Created alongside the dialogue system;
        // null outside the runtime lifecycle. Game code pushes non-dialogue
        // captions through SubtitleSystem::ShowCaption.
        SubtitleSystem* GetSubtitleSystem() const
        {
            return m_SubtitleSystem.get();
        }

        // Navigation
        void SetNavMesh(const Ref<NavMesh>& navMesh);

        // Bake a navmesh from the current scene geometry, using every
        // NavMeshBoundsComponent's box (defaults if none) and off-mesh links, then
        // install it via SetNavMesh. Returns false if generation failed. Shared by
        // the OnRuntimeStart auto-bake and the floating-origin rebake path.
        bool BakeNavMesh();

        // Floating-origin rebase (issue #613): keep a live navmesh + crowd
        // consistent across an origin shift. Detour cannot be translated in place,
        // so this shifts the bake inputs (NavMeshBoundsComponent boxes / links) and
        // each agent's world-space target by `shift`, regenerates the mesh at the
        // shifted location, and restores agent targets. No-op without a live
        // navmesh. Called by RebaseOrigin.
        void RebaseNavigation(const glm::vec3& shift);

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

        /// The MeshSource an animated entity is actually drawn from this frame:
        /// the LOD level the deformation pass wrote, or the entity's own source
        /// when it has no animated LOD selection (#1227).
        ///
        /// PUBLIC because it is the one resolver, and #1249 found out what
        /// happens when a second consumer guesses. The editor's "does this
        /// binding still fit?" read-out and its Build Binding action were
        /// resolving `MeshComponent::m_MeshSource` directly while the runtime
        /// resolved this — so on an LOD-grouped body the inspector cooked and
        /// validated against one surface and the renderer refused against
        /// another, showing a green "Attaches" next to a coat stuck at its bind
        /// pose. Every consumer asks this and nothing else.
        [[nodiscard]] static Ref<MeshSource> ResolveAnimatedSurface(const LODGroupComponent* lodComp,
                                                                    const MeshComponent& meshComp);

        /// The Skeleton behind a body, resolved the way the deformation resolves
        /// it: the entity's SkeletonComponent first, then the surface's own.
        /// Both authoring shapes exist in this repo and a consumer that
        /// understood only one would silently stop deforming on the other.
        ///
        /// Returns a CONST pointer: every consumer of it only reads the palettes
        /// and the history flags, and the deformation must never be the thing
        /// that writes a pose.
        [[nodiscard]] static const Skeleton* ResolveSurfaceSkeleton(const SkeletonComponent* skeletonComp,
                                                                    const Ref<MeshSource>& surface);

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
        // Submit every TilemapComponent's visible chunks into the batch that is
        // already open (issue #646). Called from all three 2D draw sites — the
        // editor camera, the 2D-only runtime frame and the 3D UI-composite
        // overlay — so a tilemap is never visible in one and missing from
        // another. `viewProjection` is whatever BeginScene was given, and is the
        // frustum the chunk culler tests against.
        void RenderTilemaps(const glm::mat4& viewProjection);
        // Create the static Box2D bodies backing every TilemapComponent with
        // GenerateColliders set (issue #646). Called at the end of
        // OnPhysics2DStart, once the world exists.
        void BuildTilemapColliders();
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
        void UpdateScripts(Timestep ts);       // C# + Lua entity OnUpdate
        void UpdateVisualScripts(Timestep ts); // node-graph gameplay logic (issue #634)
        void UpdateCinematics(Timestep ts);    // authored sequence playback
        // Reusable player + camera rig (issue #645). Split across two nodes for
        // ORDERING, not for parallelism: the input/movement half must land
        // before the physics kick so the same tick's step integrates it, while
        // the camera half must observe the target's FINAL pose and so runs last.
        // See PlayerRigSystem.h.
        void UpdatePlayerRig(Timestep ts);       // input -> character motion (pre-physics)
        void UpdateCameraRig(Timestep ts);       // spring-arm camera placement (post-everything)
        void UpdateDialogue(Timestep ts);        // dialogue runner
        void UpdateSubtitles(Timestep ts);       // caption overlay (issue #458)
        void UpdateLocomotion(Timestep ts);      // velocity->animation-parameter controller (issue #631)
        void UpdateRetargeting(Timestep ts);     // live-retarget clip bake (issue #631)
        void UpdateAnimation(Timestep ts);       // skeletal + morph-only sampling
        void UpdateAnimationGraphs(Timestep ts); // animation state machines
        void UpdateRootMotion(Timestep ts);      // apply extracted root-motion deltas (issue #631)
        void EvaluateMorphTargets();             // deform meshes from morph weights
        // Resolve, once per frame at the frame boundary, which conventional mesh LOD
        // level every ANIMATED entity's shared surface is drawn at (#1227). Must run
        // before the morph deformation pass: that pass writes into the very mesh the
        // renderer is about to draw, so the two have to agree on which mesh that is.
        void SelectAnimatedSurfaceLOD();
        void UpdateNavigation(Timestep ts); // pathfinding / crowds
        void UpdatePerception(Timestep ts); // AI sight sensing
        void UpdateAI(Timestep ts);         // behavior trees / FSM / GOAP
        // Flocking is split across two nodes (issue #731): the neighbour search
        // + force solve is worker-safe because it writes nothing but each
        // agent's own component, while the integrate/move half writes
        // TransformComponent and so must stay on the game thread. See
        // FlockingSystem.h for the full rationale.
        void UpdateBoidSteering(Timestep ts); // neighbour search + steering forces (worker-safe)
        void UpdateBoidMovement(Timestep ts); // integrate velocity + move entities (game thread)
        // Walk the reproducible animal population along its authored paths
        // (issue #1258). Registered in the gameplay scheduler as "AnimalPaths".
        void UpdateAnimalPaths(Timestep ts);
        void UpdateInventory(Timestep ts);     // pickups / despawn
        void UpdateDiscovery(Timestep ts);     // landing-trigger discovery loop + objective UI (issue #881)
        void UpdateDestructibles(Timestep ts); // shatter breakables + age/cleanup debris (issue #459)
        void UpdateQuest(Timestep ts);         // quest timers / conditions
        void UpdateProgression(Timestep ts);   // XP/level resolution + point grants (issue #635)
        void UpdateAbilities(Timestep ts);     // gameplay ability system
        void UpdateCombat(Timestep ts);        // weapon input, traces, and projectiles (post-physics)
        void UpdateAudio(Timestep ts);         // listener/source pose sync + events
        // Particle update is split by GPU usage (issue #576): the CPU partition
        // is worker-dispatchable (Parallelizable), the GPU partition stays on the
        // game thread because it issues GL compute. UpdateParticlesPartition does
        // the shared camera-LOD + group walk; each wrapper picks its partition.
        void UpdateParticlesCPU(Timestep ts); // CPU-only systems (worker-safe)
        void UpdateParticlesGPU(Timestep ts); // GPU / GL-compute systems (game thread)
        void UpdateParticlesPartition(Timestep ts, bool gpuPartition);
        void UpdateSnowDeformers(Timestep ts); // snow deformation stamps + ejecta
        void UpdateBoatWake(Timestep ts);      // boat hull-pose history + wake splats (#967)

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
        // Script-requested scene to switch to, unresolved. Empty = no request.
        std::string m_PendingSceneLoad;
        // Optional save slot restored into the loaded scene before runtime
        // startup. Empty means an ordinary authored-scene transition.
        std::string m_PendingSceneLoadSaveSlot;
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
        bool m_Is3DModeEnabled = false;    // Toggle for 3D rendering mode
        bool m_RenderingEnabled = true;    // Skip rendering when throttled
        bool m_ShowGrid = true;            // Viewport grid visibility
        bool m_ShowLightGizmos = true;     // Light gizmo visibility
        bool m_ShowWorldAxisHelper = true; // World-origin XYZ axes visibility
        bool m_ShowCameraFrustums = true;  // Per-CameraComponent frustum gizmo visibility
        f32 m_GridSpacing = 1.0f;          // Viewport grid spacing
        f32 m_LastAnimationTime = -1.0f;   // Tracks previous-frame animation time for wind/water/foliage velocity reprojection
        // Animation clock for water/foliage/wind. NOT the wall clock: it stops
        // while the scene is paused. See ProcessScene3DSharedLogic.
        f32 m_AnimationClock = 0.0f;
        f32 m_AnimationClockWall = -1.0f;                      // last wall-clock sample, -1 = unseeded
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
        SceneLightmapSettings m_LightmapSettings;              // Baked-GI lightmap settings (issue #439)
        // Runtime-resolved lightmap state (atlas texture + per-entity regions).
        // Deliberately NOT copied by Scene::Copy() — the runtime copy
        // re-resolves at Play start so stale state cannot survive a transition.
        Ref<SceneLightmapRuntime> m_LightmapRuntime;

        // Issue #864: signature of the last virtual-geometry "silent zero" state warned
        // about, so the per-frame check in the submission loop logs a state CHANGE rather
        // than the same line every frame. 0 means "not currently in the silent-zero state",
        // which is also what re-arms the warning if a fixed scene breaks again.
        u64 m_VirtualGeometrySilentZeroWarned = 0;

        // Runtime-only origin accumulator: absolute = rebased + m_WorldOrigin.
        // Reset to (0,0,0) at OnRuntimeStart; never serialized or copied.
        glm::vec3 m_WorldOrigin{ 0.0f };

        // Durable identity for the GPU-scene extraction owner. A pointer is not
        // sufficient: an allocator may reuse a destroyed Scene's address and
        // accidentally carry temporal history into the replacement scene.
        UUID m_GPUSceneOwnerToken;

        // Per-entity previous positions for velocity estimation (snow ejecta)
        TMap<u64, glm::vec3> m_RuntimeSnowPrevPositions;
        TMap<u64, glm::vec3> m_EditorSnowPrevPositions;
        // Bounded, time-stamped hull-pose history per boat (issue #967).
        // Scene-owned, and split runtime/editor exactly like the snow maps
        // above, so it dies with the scene instead of leaking a trail from one
        // play session into the next.
        TMap<u64, BoatWakeTrail> m_RuntimeBoatWakeTrails;
        TMap<u64, BoatWakeTrail> m_EditorBoatWakeTrails;

        b2WorldId m_PhysicsWorld = b2_nullWorldId;
        std::unique_ptr<JoltScene> m_JoltScene;

        // PBF fluid solver instances (issue #630), lazily created by
        // FluidSystem; destroyed with the scene on the game thread (the GPU
        // solver owns GL buffers). unique_ptr with an incomplete FluidWorld is
        // fine — ~Scene() is defined in Scene.cpp where the type is complete.
        std::unique_ptr<FluidWorld> m_FluidWorld;

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

        // ── Groom surface binding runtime (issue #1249) ───────────────
        //
        // Per-entity working state for a groom bound to a body: the scratch
        // buffers the per-frame deformation fills, and the identity of
        // everything the previous frame's strand positions depend on.
        //
        // KEYED BY UUID and held HERE rather than on GroomBindingComponent, the
        // same split m_ClothRuntime makes for its weld offsets: this is
        // per-frame working data, and keeping it out of the component is what
        // lets the component stay trivially copyable, hole-free and
        // automatically scene-serialized.
        //
        // The identity fields exist to answer ONE question every frame — "are
        // this entity's previous-frame strand positions still about the same
        // coat on the same body in a continuous pose?" — and every field is
        // there because a NO answer to it has a different cause that has to be
        // nameable (GroomHistoryResetCause). A single "dirty" bool could not
        // distinguish an LOD switch from a teleport, and the whole point of
        // attributing a reset is that a coat that ghosts is diagnosable.
        struct GroomBindingRuntimeState
        {
            // Scratch, reused across frames so a bound groom does not allocate
            // per frame. SelectedCurves is the strand budget's selection;
            // Transforms is one entry per curve of the groom.
            std::vector<u32> m_SelectedCurves;
            std::vector<GroomRootTransform> m_Transforms;

            // ── History identity ───────────────────────────────

            AssetHandle m_Binding = 0; ///< binding asset last deformed with
            AssetHandle m_Groom = 0;   ///< groom asset last deformed
            UUID m_Target = 0;         ///< body entity last deformed against

            /// The target's REST VERTEX BUFFER identity, not its MeshSource
            /// pointer: a conventional LOD switch hands the entity a different
            /// MeshSource and a freed one can be replaced at the same address
            /// (the recycling trap DeformedSurfaceKey and
            /// MorphTargetComponent::BaseCacheKey both key around). A
            /// MeshSource generation counter cannot be recycled.
            u64 m_TargetGeneration = 0;
            u32 m_TargetVertexCount = 0;
            u32 m_TargetIndexCount = 0;

            /// The skeleton's deformation revision pair as of the last frame
            /// this groom was deformed. Compared rather than the palette bytes,
            /// because the pair is exactly the shared output's own statement of
            /// continuity (#1228) and re-deriving it here would be a second
            /// spelling that could disagree with it.
            u32 m_DeformationRevision = 0;
            u32 m_PrevDeformationRevision = 0;

            /// Fold of everything OTHER than the palette that moves the target's
            /// vertices — today the morph weights, applied on the CPU straight
            /// into the vertex array. A hash for the reason
            /// GPUSceneAnimatedSurface::m_MorphStateHash is one: every consumer
            /// asks the same question of it.
            u64 m_MorphStateHash = 0;

            /// World-space position of the target last frame, for the teleport
            /// test. The TARGET's, not the groom's: the coat is carried by the
            /// body, so a body that cuts across the level invalidates the coat's
            /// history whether or not the groom entity itself moved.
            glm::vec3 m_TargetWorldPosition{ 0.0f };

            /// The target-to-groom matrix the PREVIOUS frame deformed with, fed
            /// back as GroomDeformationInputs::PrevSurfaceToGroom so a previous
            /// position is skinned and mapped in the same frame throughout.
            /// Only meaningful while m_HasHistory holds; the reset path sets it
            /// to this frame's matrix, which is what makes the first frame after
            /// a teleport emit zero motion rather than the teleport itself.
            glm::mat4 m_PrevSurfaceToGroom{ 1.0f };

            // ── The compatibility verdict, cached ───────────────────────
            //
            // Signing a target means an FNV pass over every index AND every
            // vertex position, byte at a time; signing a groom means one over
            // every curve root; and CheckCompatibility then scans every record.
            // Run per groom per camera per frame on the tick thread, that is
            // millions of serial steps for an answer that changes only when one
            // of the identities below does.
            //
            // So the verdict is cached and re-derived only when the CHEAP keys
            // move — the asset handles, the mesh generation, the counts. Those
            // are exactly the things the signatures are derived FROM, so a
            // change that could alter the verdict cannot slip past them: a
            // re-cooked groom is a new handle or a new curve count, a rebuilt
            // mesh bumps its generation, an LOD switch changes the counts.
            //
            // What this deliberately does not do is re-hash to confirm. A hash
            // that agrees with itself every frame is not a check, it is a cost.
            GroomBindingRejectReason m_CachedVerdict = GroomBindingRejectReason::None;
            bool m_HasCachedVerdict = false;

            bool m_HasHistory = false;
            /// Why the CURRENT frame has no usable history. Meaningful only
            /// while m_HasHistory is false — a coat that has recovered its
            /// history has no live discontinuity to name, and reporting the last
            /// one would make a debug view name a reset that is over.
            GroomHistoryResetCause m_ResetCause = GroomHistoryResetCause::FirstUse;

            /// The refusal already written to the log for this entity, so the
            /// warning fires on a CHANGE of reason rather than once per frame.
            /// A binding refusal is permanent until someone rebuilds something,
            /// so without this one misconfigured groom writes sixty lines a
            /// second and the log stops being readable at all — the same
            /// rate-limit GroomRenderPass keeps for its composition fallback.
            GroomBindingRejectReason m_LastReportedReject = GroomBindingRejectReason::None;
        };
        std::unordered_map<UUID, GroomBindingRuntimeState> m_GroomBindingRuntime;

        // == Groom guide simulation runtime (issue #1250) ==
        //
        // Per-entity working state for a SIMULATED coat: the particles, the
        // fixed-step accumulator, the fitted body proxy and the two frames of
        // guide displacement the rendered coat interpolates from.
        //
        // KEYED BY UUID and held HERE rather than on GroomSimulationComponent,
        // the same split m_GroomBindingRuntime and m_ClothRuntime make and for
        // the same reason: this is per-frame working data, and keeping it out of
        // the component is what lets the component stay trivially copyable,
        // hole-free and automatically scene-serialized.
        //
        // SEPARATE FROM GroomBindingRuntimeState rather than folded into it,
        // because the two have different lifetimes and different reset rules. A
        // coat can be bound and not simulated (every #1249 scene is), and a
        // binding refusal is not automatically a simulation reset -- the coat
        // keeps its particles while a body finishes loading, and re-seeds only
        // when the shape it is simulated AGAINST has actually changed.
        struct GroomSimulationRuntimeState
        {
            GroomGuideSimulationState m_Solver;

            /// The influence table's guide SLOTS this entity's budget selected,
            /// in ascending slot order, and the inverse map the strand build
            /// reads. Kept as a pair rather than re-derived per frame because
            /// the strand build indexes the inverse one per strand per point.
            std::vector<u32> m_SlotOfGuide; ///< guide index -> table slot
            std::vector<u32> m_GuideOfSlot; ///< table slot -> guide index, or GroomNoGuide

            /// The groomed rest shape of every simulated guide point, WORLD
            /// space. Scratch, refilled every frame; held so a bound groom does
            /// not allocate per frame.
            std::vector<glm::vec3> m_Targets;

            /// This frame's and last frame's OBJECT-space offset from that rest
            /// shape. The pair is what makes a simulated coat's motion vectors
            /// real: the previous position of a moving strand is not recoverable
            /// from any matrix, exactly as #1249 found for the binding.
            std::vector<glm::vec3> m_Displacements;
            std::vector<glm::vec3> m_PrevDisplacements;

            /// The fitted body proxy, in the body's REST object space, and this
            /// frame's resolution of it into world space.
            std::vector<GroomColliderBinding> m_ColliderBindings;
            std::vector<GroomCollider> m_Colliders;
            GroomColliderBuildStats m_ColliderStats;

            /// The identity the fitted proxy was built against. The proxy is a
            /// pure function of the surface, so it is re-fitted exactly when one
            /// of these moves -- the same keys, and the same argument, as
            /// GroomBindingRuntimeState::m_CachedVerdict.
            UUID m_ProxyTarget = 0;
            u64 m_ProxyGeneration = 0;
            u32 m_ProxyVertexCount = 0;
            u32 m_ProxyBoneCount = 0;
            bool m_HasProxy = false;

            /// The groom the guide budget was selected for, and the budget it
            /// was selected with. A budget change re-selects; nothing else does,
            /// because re-selecting hands the solver a different particle set
            /// and re-seeds it.
            AssetHandle m_BudgetGroom = 0;
            u32 m_BudgetByRole[GroomCoatRoleCount]{};

            /// The last m_ResetKey this entity was seen with. A CHANGE is the
            /// reset; comparing rather than clearing is what makes the control
            /// work identically from the inspector, a script, a save game and an
            /// MCP write -- see GroomSimulationComponent::m_ResetKey.
            u32 m_ResetKey = 0;
            bool m_HasResetKey = false;

            /// The GROOM entity's world position last frame, for the teleport
            /// test. The groom's and not the body's, because this solver runs in
            /// WORLD space: it is the coat's own motion through the level that
            /// the particles have inertia against.
            glm::vec3 m_WorldPosition{ 0.0f };

            /// False when this frame's previous displacements are not
            /// comparable. Distinct from the binding's history: a coat whose
            /// binding is fine can still have had its particles re-seeded.
            bool m_HasHistory = false;
            GroomHistoryResetCause m_ResetCause = GroomHistoryResetCause::FirstUse;
        };
        std::unordered_map<UUID, GroomSimulationRuntimeState> m_GroomSimulationRuntime;

        // Representation-LOD hysteresis state, per entity (issue #1252).
        //
        // HERE RATHER THAN IN GroomRenderPass, and that is not a filing choice.
        // A pass runs once per CAMERA — a split-screen scene, a second
        // viewport and a reflection probe all re-enter it — while a hysteresis
        // must advance once per FRAME. Advanced in the pass, a two-viewport
        // scene would burn its hold twice as fast and a coat straddling a
        // threshold would hand over in half the frames it was authored to.
        // Exactly the reason the guide simulation's clock is consumed in
        // PublishGroomStrandRequests, and the same fix.
        //
        // Keyed by UUID and swept against the entities seen this frame, for
        // m_GroomRegionMaps' reason: an entity destroyed mid-session would
        // otherwise leave its counters resident for the Scene's lifetime.
        std::unordered_map<UUID, GroomLodState> m_GroomLodRuntime;

        // ── Multi-animal scheduling budgets (issue #1258) ────────────────
        //
        // HERE RATHER THAN IN A PASS, and for GroomLodState's reason: a pass
        // runs once per CAMERA while a hysteresis and a starvation counter must
        // advance once per FRAME. Advanced in a pass, a split-screen scene
        // would burn its hold twice as fast and starve at double rate against a
        // budget that did not.
        //
        // Keyed by UUID and swept against the animals seen this frame, so an
        // entity destroyed mid-session does not leave its counters resident for
        // the Scene's lifetime.
        std::unordered_map<UUID, AnimalScheduleState> m_AnimalScheduleRuntime;

        /// This frame's decisions, gathered once and spent in three places: the
        /// animation tick rate in UpdateAnimation, the groom budgets in
        /// PublishGroomStrandRequests, and the shadow bias the pass applies.
        ///
        /// ONE EVALUATION, THREE CONSUMERS — the rule #1252 states for
        /// AdvanceGroomLod and for the same reason: deciding it in each place
        /// would mean three evaluations of a hysteretic function and three sets
        /// of counters that drift apart by construction.
        std::unordered_map<UUID, AnimalSchedule> m_AnimalSchedules;

        AnimalSchedulerStats m_AnimalSchedulerStats;

        /// Frames since the scene started running, used ONLY to phase the
        /// staggered deformation ticks. Its absolute value is never read — only
        /// `(counter + phase) % period` — so a wrap is harmless.
        ///
        /// THE STAGGER IS THE WHOLE POINT of this counter, and it is what makes
        /// the frame-time TAIL fall rather than only the mean. Forty animals at
        /// a quarter rate all ticking on the same frame is the same peak cost
        /// as forty animals at full rate, once every four frames; spread across
        /// the period it is a quarter of the peak. A scheduler without the
        /// phase improves every average and leaves the stutter exactly where it
        /// was.
        u64 m_AnimalFrameCounter = 0;

        /// The clock the deformation stagger is phased on, advanced once per
        /// UpdateAnimation — which is the function the gate's body runs in.
        ///
        /// A SECOND COUNTER AND NOT m_AnimalFrameCounter, because the two count
        /// different events: UpdateAnimation runs from SimulateRuntimeStep,
        /// which the fixed-timestep accumulator calls zero or more times per
        /// rendered frame. Phasing on the frame counter freezes animals outright
        /// whenever the display rate is a multiple of the fixed step — see
        /// ShouldPoseAnimalThisFrame.
        u64 m_AnimalPoseTick = 0;

        /// Gather every animal, share the frame out between them, and publish
        /// the result into m_AnimalSchedules. Called once per frame from the
        /// frame boundary, beside SelectAnimatedSurfaceLOD and for its reason:
        /// it must run BEFORE the tick that poses the bodies and before the
        /// submission that draws their coats.
        void ScheduleAnimalPopulationForFrame(Timestep ts);

        /// The rolling frame-time window (#1258, criterion 4).
        ///
        /// HERE RATHER THAN ON Application, because it has to be readable from
        /// a headless test and from OloServer, neither of which has one — and
        /// because what this feature needs to compare is two runs of the SAME
        /// scene with the budget on and off, which is a scene-scoped question.
        /// Ten seconds at 60 Hz: the shortest window in which a 99th percentile
        /// means anything (see FrameTimeTail.h).
        FrameTimeTail m_FrameTimeTail{ FrameTimeTail::kDefaultCapacity };

        /// This frame's schedule for `id`, or null when the animal is not
        /// budgeted. Null is not an error: it is the answer for every entity
        /// without an AnimalBudgetComponent, which is every entity in every
        /// scene authored before #1258.
        [[nodiscard]] const AnimalSchedule* FindAnimalSchedule(UUID id) const;

        /// Whether this animal's skeleton should be posed on this frame.
        ///
        /// True for every unbudgeted entity, so the pre-#1258 behaviour is
        /// exactly "no AnimalBudgetComponent". For a budgeted one the period is
        /// 2^step and the phase is derived from the UUID, which is what spreads
        /// the population's expensive frames instead of aligning them.
        ///
        /// `outScaledSeconds` receives the time the pose must advance by when
        /// the answer is true: the WHOLE period's worth, not one frame's, so
        /// the clip time never drifts behind the world and an animal at a
        /// quarter rate is not also running at a quarter speed.
        [[nodiscard]] bool ShouldPoseAnimalThisFrame(UUID id, f32 frameSeconds, f32& outScaledSeconds) const;

        // The guide-to-strand influence table, keyed by GROOM ASSET HANDLE and
        // invalidated by the asset's identity, exactly as m_GroomRegionMaps is
        // keyed and invalidated -- and for the same reason: a hot-reload or a
        // re-cook hands the asset manager a NEW GroomAsset under the same
        // handle, so a handle-only key would serve a table describing the
        // previous groom's guides forever.
        //
        // The table is a pure function of the asset and costs one pass over the
        // curves, so it is built once and shared by every entity wearing that
        // groom. See GroomGuideInfluence.h for why it is derived at runtime
        // rather than cooked.
        struct GroomGuideInfluenceCacheEntry
        {
            Ref<GroomAsset> m_Source;
            Ref<GroomGuideInfluenceTable> m_Table;
        };
        std::unordered_map<AssetHandle, GroomGuideInfluenceCacheEntry> m_GroomGuideInfluence;

        /// The influence table for `groom`, built on first use and cached.
        /// Never null.
        [[nodiscard]] Ref<GroomGuideInfluenceTable> ResolveGroomGuideInfluence(AssetHandle handle,
                                                                               const Ref<GroomAsset>& groom);

        /// Choose which of the asset's guides this entity will simulate, and
        /// APPEND their curve indices to `selectedCurves`.
        ///
        /// The append is the load-bearing part. A guide the strand budget did
        /// not select has no deformed root transform, so simulating it would
        /// solve against the BIND POSE while the body moves -- a coat that
        /// lags its own animal by a whole animation. Widening the deformer's
        /// selection is what makes the guides and the strands agree about which
        /// pose they are in.
        ///
        /// Returns false when this entity is not simulated at all, in which case
        /// `selectedCurves` is untouched and the frame costs exactly what it did
        /// before this issue.
        [[nodiscard]] bool SelectGroomSimulationGuides(Entity groomEntity, const GroomAsset& groom,
                                                       const GroomCoatContext& coat,
                                                       const GroomStrandRequest& request,
                                                       std::vector<u32>& selectedCurves);

        /// Step this entity's guide simulation and fill in `request`'s
        /// simulation half. Called from DeformGroomAgainstSurface, AFTER the
        /// root transforms exist and BEFORE they are swapped into the request,
        /// because the guides' targets are derived from them.
        ///
        /// Leaves the request UN-SIMULATED on every refusal, which draws the
        /// groomed rest coat -- visibly still, and therefore diagnosable, rather
        /// than a coat moving with somebody else's particles.
        void SimulateGroomGuides(Entity groomEntity, const GroomAsset& groom, const GroomBindingAsset& binding,
                                 const GroomCoatContext& coat, const GroomSurfaceView& surface,
                                 const GroomSkinningView& skinning, const glm::mat4& surfaceToGroom,
                                 UUID targetId, u64 targetGeneration,
                                 std::span<const GroomRootTransform> transforms, bool bindingHasHistory,
                                 GroomStrandRequest& request);

        /// Drop every simulated groom's particles, attributing the cause. The
        /// simulation twin of ResetGroomBindingHistory, called from the same
        /// places for the same reason.
        void ResetGroomSimulation(GroomHistoryResetCause cause);

        /// Seconds the guide simulation should advance by this frame.
        ///
        /// ZERO IN EDIT MODE and zero while paused, both deliberately. A scene
        /// must not change just from being open -- the rule
        /// TimeOfDayComponent::m_AdvanceInEditMode states for the clock -- and a
        /// paused frame must hold the pose it paused on, which is the
        /// pause/resume half of the issue's criterion 1.
        f32 m_GroomSimulationDeltaSeconds = 0.0f;

        // ── Coat authoring: the CPU copy of a root-UV map (issue #1251) ──
        //
        // A regional or colour map is a Texture2D asset, and the strand build
        // samples it PER STRAND on the CPU — see GroomRegionMap for why that is
        // the right side of the bus for a value that decides whether a strand
        // exists at all. Getting the pixels back out of a Texture2D is a GPU
        // readback, so it is done ONCE per map and cached here.
        //
        // KEYED BY HANDLE, INVALIDATED BY THE TEXTURE OBJECT'S IDENTITY. A
        // hot-reload hands the asset manager a NEW Texture2D under the same
        // handle, so a handle-only key would serve the old pixels forever and an
        // artist repainting a density map would see nothing change. Holding the
        // source Ref and comparing it is what catches that.
        struct GroomRegionMapCacheEntry
        {
            Ref<Texture2D> m_Source;
            Ref<GroomRegionMap> m_Map;
            /// True once a readback has been ATTEMPTED and failed, so a process
            /// with no usable graphics device (OloServer, a headless test) logs
            /// once rather than every frame.
            bool m_Failed = false;
        };
        std::unordered_map<AssetHandle, GroomRegionMapCacheEntry> m_GroomRegionMaps;

        /// The CPU copy of a texture asset's pixels, or null. Null is not an
        /// error: it is the answer for handle 0, for an asset that will not
        /// resolve, and for a process with no graphics device — and the coat
        /// reads a null map as "no modulation", which is the coat the groom
        /// asset already describes.
        [[nodiscard]] Ref<GroomRegionMap> ResolveGroomRegionMap(AssetHandle handle);

        /// Publish this frame's grooms for the production strand pass, deforming
        /// the bound ones against their body surfaces first (issues #1246,
        /// #1249). Called once per frame from ProcessScene3DSharedLogic.
        void PublishGroomStrandRequests();

        /// Fill in `request`'s binding half: resolve the body, check the binding
        /// against it, decide whether the previous frame is comparable, and
        /// evaluate the root transforms for the strands the budget will draw.
        ///
        /// Leaves the request UNBOUND and records a reason on every refusal.
        /// There is deliberately no path here that rebinds, re-snaps or falls
        /// back to a nearest point: a coat that is quietly attached to the wrong
        /// part of the body is the failure this whole feature exists to prevent
        /// (GroomBinding.h says so at more length).
        void DeformGroomAgainstSurface(Entity groomEntity, const GroomAsset& groom, GroomStrandRequest& request);

        /// Drop every bound groom's previous-frame data, attributing the cause.
        /// For wholesale discontinuities: scene load, play-mode transitions —
        /// the groom twin of SkeletalDeformationSystem::ResetHistory, and called
        /// from the same places for the same reason.
        void ResetGroomBindingHistory(GroomHistoryResetCause cause);

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
        // Declared AFTER m_DialogueSystem so it is destroyed first: both hold
        // UUIDs into m_EntityMap and tear their UI entities down in reverse
        // declaration order (same constraint the m_DialogueSystem.reset() in
        // ~Scene documents).
        std::unique_ptr<SubtitleSystem> m_SubtitleSystem;
        DialogueVariables m_DialogueVariables;
        std::unique_ptr<VisualScript::VisualScriptSystem> m_VisualScriptSystem;

        // Navigation
        Ref<NavMesh> m_NavMesh;
        std::unique_ptr<NavMeshQuery> m_NavMeshQuery;
        std::unique_ptr<CrowdManager> m_CrowdManager;

        // Spatial acceleration (runtime-only; rebuilt each OnUpdateRuntime tick,
        // never serialized/copied). See GetSpatialIndex / UpdateSpatialIndex.
        SceneSpatialIndex m_SpatialIndex;

        // Flocking scratch (issue #731) — runtime-only, rebuilt at the top of
        // every BoidSteering tick and never serialized/copied. Persistent only
        // so the per-tick snapshot + hash rebuild don't allocate. Held behind a
        // pointer so this header doesn't drag the flocking + spatial-hash
        // headers into every TU that includes Scene.h (same reason as
        // m_JoltScene / m_CrowdManager above); ~Scene() is out-of-line in
        // Scene.cpp where the type is complete.
        std::unique_ptr<FlockingWorkspace> m_FlockingWorkspace;

        // Support graph for progressive collapse (issue #786). See GetStructuralGraph.
        std::unique_ptr<StructuralGraph> m_StructuralGraph;

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

        // ── Deferred script spawn/destroy queue (issue #643) ─────────────────
        // See the ScriptCreateEntity / FlushPendingEntityCommands block in the
        // public section for the contract.
        struct PendingEntityCommand
        {
            enum class Kind : u8
            {
                CreateEntity,
                InstantiatePrefab,
                DestroyEntity
            };

            Kind m_Kind = Kind::CreateEntity;
            // Spawns: the UUID handed back to the script at request time.
            // Destroys: the target.
            UUID m_EntityID{ 0 };
            AssetHandle m_PrefabHandle{ 0 };
            std::string m_Name;
            glm::vec3 m_Translation{ 0.0f };
            glm::vec3 m_RotationEuler{ 0.0f };
            glm::vec3 m_Scale{ 1.0f };
        };

        // Screen a caller-supplied entity UUID against the live entity map,
        // returning either it or a freshly generated replacement. Mandatory
        // before CreateEntityWithUUID: that function Add()s unconditionally,
        // which OVERWRITES an existing mapping and strands the previous
        // occupant. Asserts in Debug (a collision means a real id-source bug);
        // regenerates and warns in Release. `context` names the caller in the
        // log.
        [[nodiscard("The screened UUID is the one you must create with")]] UUID
        ResolveUUIDCollision(UUID uuid, const char* context);

        // Apply one queued command. Game-thread only, and only ever called from
        // inside FlushPendingEntityCommands' drain loop.
        void ApplyPendingEntityCommand(const PendingEntityCommand& cmd);
        // Fire C# OnCreate / Lua OnCreate for a freshly spawned entity. The
        // OnRuntimeStart sweep only covers entities that existed when the
        // session started, so without this a script-spawned scripted entity
        // would receive OnUpdate having never received OnCreate.
        void FireSpawnScriptLifecycle(Entity entity);
        // Same, for a spawned prefab's whole subtree (children can carry
        // scripts too).
        void FireSpawnScriptLifecycleRecursive(Entity entity);

        // Guards the queue, both pending sets, and the drain flag below.
        // Uncontended today (scripts run on the game thread) but present so
        // marking the Scripts node Parallelizable can never silently corrupt
        // the queue.
        mutable std::mutex m_EntityCommandMutex;
        std::vector<PendingEntityCommand> m_PendingEntityCommands;
        std::unordered_set<UUID> m_PendingSpawnIDs;
        std::unordered_set<UUID> m_PendingDestroyIDs;
        // Re-entrancy guard for FlushPendingEntityCommands: a spawned entity's
        // OnCreate can queue more commands, and those must be applied by the
        // outer drain's next round, never by a nested drain running inside the
        // outer one's batch. Read and written under m_EntityCommandMutex, so it
        // cannot disagree with the queue it guards.
        bool m_DrainingEntityCommands = false;
        // Bound on drain rounds per FLUSH INVOCATION, so a script that
        // unconditionally spawns from OnCreate cannot hang the tick. Leftovers
        // stay queued for the next flush. Note UpdateScripts flushes twice per
        // tick (before and after the script loops), so a runaway spawner gets
        // up to two round-budgets per tick — still bounded, still progressing,
        // never blocking the frame.
        static constexpr u32 kMaxEntityCommandDrainRounds = 8;

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
