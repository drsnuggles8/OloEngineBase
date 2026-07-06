#include "OloEnginePCH.h"
#include "Scene.h"
#include "Entity.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>

#include "Components.h"
#include "Prefab.h"
#include "SystemScheduler.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/InstancePlacementAsset.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/UnderwaterCaustics.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/ProceduralSky.h"
#include "OloEngine/Renderer/StarNestSky.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/WaterSurface.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Animation/BoneEntityUtils.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Asset/SoundConfigAsset.h"
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSystem.h"
#include "OloEngine/Cinematic/CinematicSystem.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationGraphAsset.h"
#include "OloEngine/Animation/AnimationGraphSystem.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltShapes.h"
#include "OloEngine/Physics3D/BuoyancySystem.h"
#include "OloEngine/UI/UILayoutSystem.h"
#include "OloEngine/UI/UIRenderer.h"
#include "OloEngine/UI/UIInputSystem.h"
#include "OloEngine/UI/UINavigationSystem.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Localization/LocalizationSystem.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/Particle/ParticleRenderer.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Particle/TrailRenderer.h"
#include "OloEngine/Video/VideoSystem.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainChunk.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainTile.h"
#include "OloEngine/Terrain/TerrainStreamer.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/Voxel/MarchingCubes.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Snow/SnowAccumulationSystem.h"
#include "OloEngine/Snow/SnowEjectaSystem.h"
#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Navigation/NavigationSystem.h"
#include "OloEngine/AI/AISystem.h"
#include "OloEngine/AI/Perception/PerceptionSystem.h"
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDialogueBridge.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Audio/AudioEvents/AudioEventsManager.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Audio/AudioTransform.h"
#include "OloEngine/Audio/DSP/Spatializer/Spatializer.h"
#include "OloEngine/Project/Project.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <ranges>

// Box2D
#include <box2d/box2d.h>

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h> // cloth soft body (issue #460)

namespace OloEngine
{
    // ── EnTT owning-group ownership map (issue #443) ──────────────────────────
    // EnTT v4 rule (registry.hpp:1064 assertion "Conflicting groups"): a
    // component may be *owned* by at most ONE owning group; a component owned by
    // one group may still be *observed* (entt::get<>) by any number of others.
    // The hottest per-frame multi-component loops below use owning / partial-
    // owning groups so their owned pools iterate as a tight packed range. Keep
    // this table current — creating a group that owns an already-owned component
    // is a runtime assert, not a compile error:
    //
    //   OWNED component            GROUP (owner)                         OBSERVED (get)
    //   -----------------------    ----------------------------------    ---------------
    //   TransformComponent         2D sprite draw loop                   SpriteRendererComponent
    //   AudioListenerComponent     audio listener sync                   TransformComponent
    //   AudioSourceComponent       audio source sync                     TransformComponent
    //   Rigidbody3DComponent       physics 3D transform sync (#443)      TransformComponent
    //   ParticleSystemComponent    particle system update/render (#443)  TransformComponent
    //   AnimationStateComponent }  animation update (full-owning, #443)  — (none)
    //   SkeletonComponent       }
    //
    // TransformComponent is already owned by the sprite loop, so physics and
    // particles CANNOT own it — they partial-own their unique component and
    // borrow Transform via entt::get<>, matching the existing audio groups.
    // Animation owns both AnimationStateComponent + SkeletonComponent (neither
    // is shared with another hot loop), giving a full-owning group. Views over
    // these components elsewhere (morph-only anim, skeleton mesh, single-
    // component particle draws) are unaffected — views never conflict with
    // ownership; the group only reorders the owned pools.
    // ──────────────────────────────────────────────────────────────────────────

    namespace
    {
        // Forward declarations for the cloth soft-body helpers (issue #460). They are
        // defined in the anonymous namespace next to OnPhysics3DStart (far below), but
        // OnUpdateRuntime — which sits above that — recomputes cloth normals each tick,
        // so it needs them declared first.
        void ComputeClothNormals(const std::vector<glm::vec3>& positions, u32 columns, u32 rows,
                                 std::vector<glm::vec3>& outNormals);
        std::vector<u32> BuildClothGridIndices(u32 columns, u32 rows);

        // Box2D velocity iterations, shared by the synchronous step (StepPhysics)
        // and the async kick's world-step task so the two can never drift.
        // TODO: position iterations when Box2D position-iteration support lands.
        constexpr i32 kPhysics2DVelocityIterations = 6;
    } // namespace

    // Underwater test (WATER_FUTURE_IMPROVEMENTS.md §7.2). True when `cameraPos`
    // lies inside the water plane's XZ footprint; writes the signed vertical gap
    // (still-surface Y − camera Y; positive = camera below the surface) to
    // `outGap`. Works in the water plane's local space so translated / rotated /
    // scaled grids are handled uniformly (grid is centred on local origin,
    // spanning ±size/2 in X/Z at local Y = 0).
    static bool GetWaterCameraFootprintGap(const glm::mat4& modelMat, const WaterComponent& water,
                                           const glm::vec3& cameraPos, f32& outGap)
    {
        const glm::mat4 invModel = glm::inverse(modelMat);
        const glm::vec4 localCam = invModel * glm::vec4(cameraPos, 1.0f);
        if (!std::isfinite(localCam.x) || !std::isfinite(localCam.y) || !std::isfinite(localCam.z))
            return false;
        // Sanitize the world sizes before clamping: std::clamp passes NaN through,
        // which would make halfX/halfZ NaN and the bounds test below always "pass"
        // (every comparison with NaN is false), wrongly treating the water as
        // infinite. Fall back to the 0.1f minimum used elsewhere (fail-closed:
        // a tiny tile the camera won't be inside) so non-finite sizes don't activate fog.
        const f32 safeSizeX = std::isfinite(water.m_WorldSizeX) ? water.m_WorldSizeX : 0.1f;
        const f32 safeSizeZ = std::isfinite(water.m_WorldSizeZ) ? water.m_WorldSizeZ : 0.1f;
        const f32 halfX = std::clamp(safeSizeX, 0.1f, 10000.0f) * 0.5f;
        const f32 halfZ = std::clamp(safeSizeZ, 0.1f, 10000.0f) * 0.5f;
        if (localCam.x < -halfX || localCam.x > halfX || localCam.z < -halfZ || localCam.z > halfZ)
            return false;
        const glm::vec4 surfaceWorld = modelMat * glm::vec4(localCam.x, 0.0f, localCam.z, 1.0f);
        outGap = surfaceWorld.y - cameraPos.y;
        return true;
    }

    // Audio listener/source orientation comes from columns of an entity's inverse local
    // transform. A singular (zero-scale) or non-finite transform makes glm::inverse emit
    // NaN/Inf, and normalizing a zero/NaN vector yields NaN — which would poison the 3D
    // spatializer's lookAt/length math (issue #424). Fall back to the canonical axis (what
    // an identity transform yields) when the basis vector is degenerate.
    static glm::vec3 SafeAudioBasis(const glm::vec3& basis, const glm::vec3& fallback)
    {
        if (std::isfinite(basis.x) && std::isfinite(basis.y) && std::isfinite(basis.z) && glm::length(basis) > 1e-4f)
            return glm::normalize(basis);
        return fallback;
    }

    static void DrawTextWithShadow(const TextComponent& text, const glm::mat4& worldTransform, int entityID)
    {
        if (text.DropShadow)
        {
            glm::mat4 shadowTransform = glm::translate(worldTransform, glm::vec3(text.ShadowDistance, -text.ShadowDistance, 0.0f));
            Renderer2D::DrawString(text.TextString, text.FontAsset, shadowTransform, { text.ShadowColor, text.Kerning, text.LineSpacing, text.MaxWidth }, entityID);
        }
        Renderer2D::DrawString(text.TextString, worldTransform, text, entityID);
    }

    [[nodiscard("Store this!")]] static b2BodyType Rigidbody2DTypeToBox2DBody(const Rigidbody2DComponent::BodyType bodyType)
    {
        switch (bodyType)
        {
            using enum OloEngine::Rigidbody2DComponent::BodyType;
            case Static:
                return b2_staticBody;
            case Dynamic:
                return b2_dynamicBody;
            case Kinematic:
                return b2_kinematicBody;
        }

        OLO_CORE_ASSERT(false, "Unknown body type");
        return b2_staticBody;
    }

    Scene::Scene()
        : m_JoltScene(std::make_unique<JoltScene>(this)), m_GameplayEventBus(std::make_unique<GameplayEventBus>()),
          m_UINavigation(std::make_unique<UINavigation>())
    {
        // Pre-create every EnTT storage/group the Parallelizable gameplay systems
        // (issue #453: Abilities, Audio) touch, on the constructing thread. EnTT's
        // view()/group() lazily CREATE missing storage — a structural mutation of
        // the registry's pool map — so a first-touch from two concurrent worker
        // tasks races that map (and, without ENTT_USE_ATOMIC, entt's global
        // type-index counter), corrupting the type→pool mapping process-wide (the
        // entt "Unexpected type" assert). With the containers pre-created, the
        // worker-side view()/group() calls are pure reads of registry structure.
        // The audio groups were previously first created in InitAudioRuntime,
        // which headless hosts (dedicated server, Functional tests) never call —
        // the constructor covers every host. Extend this list when marking
        // another system Parallelizable (see GetGameplayScheduler's audit table).
        (void)m_Registry.storage<TransformComponent>();
        (void)m_Registry.storage<AbilityComponent>();
        (void)m_Registry.storage<AudioListenerComponent>();
        (void)m_Registry.storage<AudioSourceComponent>();
        (void)m_Registry.storage<AudioSoundGraphComponent>();
        (void)m_Registry.group<AudioListenerComponent>(entt::get<TransformComponent>);
        (void)m_Registry.group<AudioSourceComponent>(entt::get<TransformComponent>);
    }

    template<typename T>
    Entity Scene::GetEntityForComponent(const T& component)
    {
        for (auto const e : m_Registry.view<T>())
        {
            if (&m_Registry.get<T>(e) == &component)
            {
                return { e, this };
            }
        }
        return {};
    }

    // Explicit instantiations for the component types the scripting glue needs
    // to map back to their owning entity when publishing gameplay events.
    template Entity Scene::GetEntityForComponent<InventoryComponent>(const InventoryComponent&);
    template Entity Scene::GetEntityForComponent<QuestJournalComponent>(const QuestJournalComponent&);

    GameplayEventBus& Scene::GetGameplayEvents()
    {
        return *m_GameplayEventBus;
    }

    const GameplayEventBus& Scene::GetGameplayEvents() const
    {
        return *m_GameplayEventBus;
    }

    UINavigation& Scene::GetUINavigation()
    {
        return *m_UINavigation;
    }

    const UINavigation& Scene::GetUINavigation() const
    {
        return *m_UINavigation;
    }

    Ref<Scene> Scene::Create()
    {
        return Ref<Scene>(new Scene());
    }

    Scene::~Scene()
    {
        // Tear down subsystems that hold references back into the scene
        // *before* the implicit member-destruction pass starts unwinding
        // m_EntityMap / m_Registry. DialogueSystem owns a UIController whose
        // ~Shutdown path calls Scene::GetEntityByUUID — that asserts on an
        // empty m_EntityMap, which is what the implicit destruction order
        // (members destroyed in reverse declaration order) would otherwise
        // produce because m_EntityMap is declared after m_DialogueSystem.
        // Reset the system here while every member it might touch is still
        // alive.
        m_DialogueSystem.reset();

        if (b2World_IsValid(m_PhysicsWorld))
        {
            b2DestroyWorld(m_PhysicsWorld);
            m_PhysicsWorld = b2_nullWorldId;
        }
    }

    template<typename... Component>
    static void CopyComponent(entt::registry& dstRegistry, entt::registry& srcRegistry, const TMap<UUID, entt::entity>& enttMap)
    {
        ([&]()
         {
			auto view = srcRegistry.view<Component>();
			for (auto entity : view)
			{
				entt::entity dstEntity = enttMap.FindChecked(srcRegistry.get<IDComponent>(entity).ID);

				const auto& srcComponent = srcRegistry.get<Component>(entity);
				dstRegistry.emplace_or_replace<Component>(dstEntity, srcComponent);
			} }(), ...);
    }

    template<typename... Component>
    static void CopyComponent(ComponentGroup<Component...>, entt::registry& dst, entt::registry& src, const TMap<UUID, entt::entity>& enttMap)
    {
        CopyComponent<Component...>(dst, src, enttMap);
    }

    template<typename... Component>
    static void CopyComponentIfExists(Entity dst, Entity src)
    {
        ([&]()
         {
			if (src.HasComponent<Component>())
			{
				dst.AddOrReplaceComponent<Component>(src.GetComponent<Component>());
			} }(), ...);
    }

    template<typename... Component>
    static void CopyComponentIfExists(ComponentGroup<Component...>, Entity dst, Entity src)
    {
        CopyComponentIfExists<Component...>(dst, src);
    }

    Ref<Scene> Scene::Copy(Ref<Scene>& other)
    {
        // Mute the per-entity EntitySpawn flood: a whole-scene copy (Play / Simulate /
        // duplicate) recreates every entity, which is internal churn, not the kind of
        // spawn the "what just happened" timeline cares about. Play itself is recorded
        // once by OnRuntimeStart. (#306 item B)
        DiagnosticsEventLog::SuppressScope suppressSpawnFlood;

        Ref<Scene> newScene = Ref<Scene>::Create();

        newScene->m_ViewportWidth = other->m_ViewportWidth;
        newScene->m_ViewportHeight = other->m_ViewportHeight;
        newScene->m_StreamingSettings = other->m_StreamingSettings;

        auto& srcSceneRegistry = other->m_Registry;
        auto& dstSceneRegistry = newScene->m_Registry;
        TMap<UUID, entt::entity> enttMap;

        // Create entities in new scene
        for (const auto idView = srcSceneRegistry.view<IDComponent>(); auto e : std::ranges::reverse_view(idView))
        {
            const UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
            const auto& name = srcSceneRegistry.get<TagComponent>(e).Tag;
            const Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
            enttMap.Add(uuid, static_cast<entt::entity>(newEntity));
        }

        // Copy components (except IDComponent and TagComponent)
        CopyComponent(AllComponents{}, dstSceneRegistry, srcSceneRegistry, enttMap);

        // Propagate navmesh so NavigationSystem works in the copied scene
        if (other->m_NavMesh)
            newScene->SetNavMesh(other->m_NavMesh);

        return newScene;
    }

    [[nodiscard]] Entity Scene::CreateEntity(const std::string& name)
    {
        return CreateEntityWithUUID(UUID(), name);
    }

    [[nodiscard]] Entity Scene::CreateEntityWithUUID(const UUID uuid, const std::string& name)
    {
        auto entity = Entity{ m_Registry.create(), this };
        auto& idComponent = entity.AddComponent<IDComponent>();
        idComponent.ID = uuid;

        entity.AddComponent<TransformComponent>();
        // RelationshipComponent will be added on-demand when needed
        // entity.AddComponent<RelationshipComponent>();

        auto& tag = entity.AddComponent<TagComponent>();
        tag.Tag = name.empty() ? "Entity" : name;

        m_EntityMap.Add(uuid, entity);
        m_EntityNameMap.emplace(tag.Tag, static_cast<entt::entity>(entity));

        // Unified diagnostics timeline (#306 item B). Suppressed during whole-scene
        // bulk creation (Scene::Copy on Play, deserialize on load) so the ring buffer
        // records interactive/runtime spawns rather than internal churn.
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::EntitySpawn,
                                          "Spawned entity '" + tag.Tag + "'", static_cast<u64>(uuid));

        return entity;
    }

    [[nodiscard]] Entity Scene::Instantiate(AssetHandle prefabHandle)
    {
        return InstantiateWithUUID(prefabHandle, UUID());
    }

    [[nodiscard]] Entity Scene::InstantiateWithUUID(AssetHandle prefabHandle, UUID uuid)
    {
        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(prefabHandle);
        if (!prefab)
        {
            OLO_CORE_ERROR("Scene::InstantiateWithUUID - Failed to load prefab with handle {}", prefabHandle);
            return {};
        }

        // Generate UUID if not provided
        if (!uuid)
            uuid = UUID();

        // Check for UUID collision and resolve if necessary
        if (m_EntityMap.Contains(uuid))
        {
            UUID originalUuid = uuid;
#ifdef OLO_DEBUG
            OLO_CORE_ASSERT(false, "Scene::InstantiateWithUUID - UUID collision detected! UUID {} already exists in scene", static_cast<u64>(uuid));
#else
            OLO_CORE_WARN("Scene::InstantiateWithUUID - UUID collision detected! UUID {} already exists, generating new UUID", static_cast<u64>(uuid));

            // Generate new unique UUID
            do
            {
                uuid = UUID();
            } while (m_EntityMap.Contains(uuid));

            OLO_CORE_WARN("Scene::InstantiateWithUUID - Resolved collision: original UUID {} replaced with new UUID {}", static_cast<u64>(originalUuid), static_cast<u64>(uuid));
#endif
        }

        // Create a new entity from the prefab
        Entity entity = prefab->Instantiate(*this, uuid);

        return entity;
    }

    void Scene::UpdateAllPrefabInstances()
    {
        OLO_PROFILE_FUNCTION();

        auto view = GetAllEntitiesWith<PrefabComponent>();
        for (auto e : view)
        {
            Entity entity{ e, *this };
            const auto& pc = entity.GetComponent<PrefabComponent>();
            if (!pc.IsValid())
                continue;

            Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(pc.m_PrefabID);
            if (!prefab)
                continue;

            prefab->UpdateInstanceFromPrefab(entity);
        }
    }

    void Scene::RevertPrefabComponent(Entity entity, const std::string& componentName) const
    {
        OLO_PROFILE_FUNCTION();

        if (!entity || !entity.HasComponent<PrefabComponent>())
            return;

        auto& pc = entity.GetComponent<PrefabComponent>();
        if (!pc.IsValid())
            return;

        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(pc.m_PrefabID);
        if (!prefab)
            return;

        if (prefab->RevertComponent(entity, componentName))
        {
            pc.ClearComponentOverride(componentName);
            pc.m_AddedComponents.erase(componentName);
            pc.m_RemovedComponents.erase(componentName);
        }
    }

    void Scene::ApplyPrefabComponent(Entity entity, const std::string& componentName) const
    {
        OLO_PROFILE_FUNCTION();

        if (!entity || !entity.HasComponent<PrefabComponent>())
            return;

        auto& pc = entity.GetComponent<PrefabComponent>();
        if (!pc.IsValid())
            return;

        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(pc.m_PrefabID);
        if (!prefab)
            return;

        if (prefab->ApplyComponentToPrefab(entity, componentName))
        {
            pc.ClearComponentOverride(componentName);
            pc.m_AddedComponents.erase(componentName);
            pc.m_RemovedComponents.erase(componentName);
        }
    }

    void Scene::MarkPrefabComponentOverridden(Entity entity, const std::string& componentName) const
    {
        OLO_PROFILE_FUNCTION();

        if (!entity || !entity.HasComponent<PrefabComponent>())
            return;

        auto& pc = entity.GetComponent<PrefabComponent>();
        if (pc.IsValid())
        {
            pc.MarkComponentOverridden(componentName);
        }
    }

    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity || !entity.HasComponent<IDComponent>())
            return;

        // Capture UUID before the Lua callback — the script may re-entrantly
        // destroy this entity, invalidating the handle. Capture the name too, for
        // the diagnostics timeline recorded once the destroy actually completes.
        UUID entityUUID = entity.GetUUID();
        std::string entityName = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};

        // Dispatch Lua OnDestroy before the entity is removed from the registry
        if (m_IsRunning && entity.HasComponent<LuaScriptComponent>())
        {
            if (auto const& lsc = entity.GetComponent<LuaScriptComponent>(); !lsc.ScriptFile.empty())
            {
                LuaScriptEngine::OnDestroyEntity(entity);
            }
        }

        // The Lua callback may have re-entrantly destroyed this entity already.
        if (!m_Registry.valid(entity))
            return;

        // Remove from name cache before destroying
        if (entity.HasComponent<TagComponent>())
        {
            auto const& tag = entity.GetComponent<TagComponent>().Tag;
            auto [rangeBegin, rangeEnd] = m_EntityNameMap.equal_range(tag);
            for (auto it = rangeBegin; it != rangeEnd; ++it)
            {
                if (it->second == static_cast<entt::entity>(entity))
                {
                    m_EntityNameMap.erase(it);
                    break;
                }
            }
        }

        // A VehicleConstraint references (and step-listens on) the chassis body,
        // so it must be torn down before that body is destroyed below — otherwise
        // the still-registered step listener dereferences a freed body on the next
        // physics tick (and UpdateVehicleControllers would look up the now-gone
        // entity). m_Registry.destroy() does NOT fire OnComponentRemoved, so do it
        // explicitly here, vehicle-before-body, matching OnPhysics3DStop's order.
        if (m_JoltScene && entity.HasComponent<VehicleComponent>())
        {
            auto& vehicle = entity.GetComponent<VehicleComponent>();
            if (vehicle.m_RuntimeVehicleToken != 0)
            {
                m_JoltScene->DestroyVehicle(entity);
                vehicle.m_RuntimeVehicleToken = 0;
            }
        }

        // Tear down physics bodies before the registry forgets about the entity —
        // otherwise JoltScene's body table holds a dangling entity ID and trips
        // an entt assertion at JoltScene::Shutdown when it tries to release the
        // bodies. This mirrors what OnPhysics3DStop does for an entire scene
        // teardown; here we do it for one entity.
        if (m_JoltScene && entity.HasComponent<Rigidbody3DComponent>())
        {
            auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();
            if (rb3d.m_RuntimeBodyToken != 0)
            {
                m_JoltScene->DestroyBody(entity);
                rb3d.m_RuntimeBodyToken = 0;
            }
        }

        // Same deal for character controllers — without this, JoltScene keeps
        // a live JoltCharacterController referring to a destroyed entity.
        if (m_JoltScene && entity.HasComponent<CharacterController3DComponent>())
        {
            m_JoltScene->DestroyCharacterController(entity);
        }

        // Terrain collision is a raw static body keyed by entity UUID (no
        // Rigidbody3DComponent), so it isn't covered by the cleanup above. Drop it
        // here too, else JoltScene keeps a live body + BodyID→entity mapping for a
        // destroyed entity until shutdown (m_Registry.destroy() doesn't fire
        // OnComponentRemoved<TerrainComponent>).
        if (m_JoltScene && entity.HasComponent<TerrainComponent>())
        {
            auto& terrain = entity.GetComponent<TerrainComponent>();
            if (terrain.m_RuntimeCollisionBodyToken != 0)
            {
                m_JoltScene->DestroyTerrainBody(entityUUID);
                terrain.m_RuntimeCollisionBodyToken = 0;
            }
        }

        m_Registry.destroy(entity);
        m_EntityMap.Remove(entityUUID);

        m_RuntimeSnowPrevPositions.Remove(entityUUID);
        m_EditorSnowPrevPositions.Remove(entityUUID);

        // Unified diagnostics timeline (#306 item B), recorded only once the destroy
        // has actually gone through (the early returns above bail before this).
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::EntityDestroy,
                                          "Destroyed entity '" + entityName + "'", static_cast<u64>(entityUUID));
    }

    void Scene::InitDialogueSystem()
    {
        m_DialogueSystem = std::make_unique<DialogueSystem>(this);
        // Composition root: bridge dialogue action/condition nodes to the quest
        // system so NPC conversations can accept/advance/complete quests and
        // branch on quest state. Lives here so both the runtime (OnRuntimeStart)
        // and headless test harnesses (EnableDialogue) wire the same handlers.
        RegisterQuestDialogueHandlers(*m_DialogueSystem, *this);
    }

    void Scene::InitAudioRuntime()
    {
        // Initialize audio events system. Extracted from OnRuntimeStart so
        // headless test harnesses (Functional tests) can spin audio up
        // without going through Application::Get().IsHeadless().
        m_AudioCommandRegistry = std::make_unique<Audio::AudioCommandRegistry>();
        m_AudioEventsManager = std::make_unique<Audio::AudioEventsManager>();

        if (const auto project = Project::GetActive())
        {
            auto eventsPath = Project::GetAssetDirectory() / "audio" / "AudioEvents.yaml";
            if (std::filesystem::exists(eventsPath))
            {
                m_AudioCommandRegistry->Deserialize(eventsPath);
            }
        }

        m_AudioEventsManager->Init(m_AudioCommandRegistry.get());
        m_AudioEventsManager->SetPositionResolver([this](u64 objectID, glm::vec3& outPos) -> bool
                                                  {
            if (auto entity = TryGetEntityWithUUID(UUID(objectID)); entity && entity->HasComponent<TransformComponent>())
            {
                outPos = entity->GetComponent<TransformComponent>().Translation;
                return true;
            }
            return false; });
        Audio::AudioPlayback::SetManager(m_AudioEventsManager.get());

        for (auto listenerView = m_Registry.group<AudioListenerComponent>(entt::get<TransformComponent>); auto&& [e, ac, tc] : listenerView.each())
        {
            ac.Listener = Ref<AudioListener>::Create();
            if (ac.Active)
            {
                const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
                const glm::vec3 forward = SafeAudioBasis(glm::vec3(inverted[2]), glm::vec3(0.0f, 0.0f, 1.0f));
                ac.Listener->SetConfig(ac.Config);
                ac.Listener->SetPosition(tc.Translation);
                ac.Listener->SetDirection(-forward);
                // Seed the 3D spatializer's listener pose so SoundGraph voices registered just
                // below (InitializeAudioSoundGraph) compute their initial relative position
                // against the real listener, not the default origin (issue #424).
                if (auto* spatializer = AudioEngine::GetSpatializer())
                {
                    Audio::Transform listenerTransform;
                    listenerTransform.Position = tc.Translation;
                    listenerTransform.Orientation = -forward;
                    listenerTransform.Up = SafeAudioBasis(glm::vec3(inverted[1]), glm::vec3(0.0f, 1.0f, 0.0f));
                    spatializer->UpdateListener(listenerTransform);
                }
                break;
            }
        }

        for (auto sourceView = m_Registry.group<AudioSourceComponent>(entt::get<TransformComponent>); auto&& [e, ac, tc] : sourceView.each())
        {
            // A SoundConfig (.olosoundc) preset, when assigned, is the source of truth for this
            // source's playback parameters: load it and stamp Config before either the event path
            // or the direct-play path reads it.
            if (ac.SoundConfigHandle != 0)
            {
                if (auto preset = AssetManager::GetAsset<SoundConfigAsset>(ac.SoundConfigHandle))
                    ac.Config = preset->m_Config;
            }

            // Event-driven audio: post the start event instead of direct play
            if (ac.UseEventSystem && ac.StartCommandID.IsValid() && ac.Config.PlayOnAwake)
            {
                Entity entity(e, this);
                ac.ActiveEventID = Audio::AudioPlayback::PostTrigger(ac.StartCommandID, static_cast<u64>(entity.GetUUID()));
                continue;
            }

            if (ac.Source)
            {
                const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
                const glm::vec3 forward = SafeAudioBasis(glm::vec3(inverted[2]), glm::vec3(0.0f, 0.0f, 1.0f));
                ac.Source->SetConfig(ac.Config);
                ac.Source->SetPosition(tc.Translation);
                ac.Source->SetDirection(forward);
                if (ac.Config.PlayOnAwake)
                {
                    ac.Source->Play();
                }
            }
        }

        // Sound graph components: load the referenced graph asset, compile a prototype via the
        // SoundGraphCache, instantiate a runnable graph, and hand it to a SoundGraphSound wrapper
        // for this entity. The miniaudio callback bridge that actually outputs audio from a
        // SoundGraphSource is not wired yet — this builds the lifecycle scaffolding (asset load,
        // per-entity ownership, ReplaceGraph) so hot-reload and editor wiring have something
        // concrete to swap against once the callback work lands.
        OLO_PROFILE_SCOPE("Scene::InitAudioRuntime - SoundGraph startup");
        for (auto soundGraphView = m_Registry.view<AudioSoundGraphComponent>(); auto entityID : soundGraphView)
        {
            InitializeAudioSoundGraph(soundGraphView.get<AudioSoundGraphComponent>(entityID));
        }
    }

    void Scene::InitializeAudioSoundGraph(AudioSoundGraphComponent& sgc) const
    {
        if (sgc.SoundGraphHandle == 0)
        {
            return;
        }

        // Idempotent: skip if a Sound wrapper already exists. InitAudioRuntime is
        // called once at OnRuntimeStart and OnComponentAdded fires per-entity for
        // runtime-spawned components — without this guard, a hot-reload pathway
        // that re-runs InitAudioRuntime would leak the previous SoundGraphSound.
        if (sgc.Sound)
        {
            return;
        }

        auto graphAsset = AssetManager::GetAsset<SoundGraphAsset>(sgc.SoundGraphHandle);
        if (!graphAsset)
        {
            OLO_CORE_WARN("Scene::InitializeAudioSoundGraph - AudioSoundGraphComponent references missing graph asset {}", sgc.SoundGraphHandle);
            return;
        }

        Ref<Audio::SoundGraph::Prototype> prototype = graphAsset->GetCompiledPrototype();
        if (!prototype)
        {
            // Lazy compile — first runtime use of an asset that wasn't saved through the
            // editor (or was saved by an old build that didn't compile). Cache the result
            // back on the asset so subsequent entities sharing this graph skip the work.
            prototype = Audio::SoundGraph::CompileAssetToPrototype(*graphAsset);
            if (!prototype)
            {
                OLO_CORE_WARN("Scene::InitializeAudioSoundGraph - SoundGraphAsset {} compile failed; not playable", sgc.SoundGraphHandle);
                return;
            }
            graphAsset->SetCompiledPrototype(prototype);
        }

        Ref<Audio::SoundGraph::SoundGraph> graphInstance = Audio::SoundGraph::CreateInstance(prototype);
        if (!graphInstance)
        {
            OLO_CORE_WARN("Scene::InitializeAudioSoundGraph - CreateInstance returned null for SoundGraphAsset {}", sgc.SoundGraphHandle);
            return;
        }

        sgc.Sound = Ref<Audio::SoundGraph::SoundGraphSound>::Create();
        sgc.Sound->InitializeAudioCallback();
        if (!sgc.Sound->InitializeFromGraph(graphInstance))
        {
            OLO_CORE_WARN("Scene::InitializeAudioSoundGraph - SoundGraphSound::InitializeFromGraph failed for asset {}", sgc.SoundGraphHandle);
            // Paired teardown: InitializeAudioCallback attached the source to the
            // live ma_engine. Mirror the runtime-stop path (ReleaseResources +
            // null) so the half-initialised source detaches before the Ref drops.
            sgc.Sound->ReleaseResources();
            sgc.Sound = nullptr;
            return;
        }

        // Stash the originating asset handle so the asset-reload dispatcher can find
        // this source when its graph asset is edited on disk and call ReplaceGraph().
        if (auto* source = sgc.Sound->GetSource())
        {
            source->SetSourceAssetHandle(sgc.SoundGraphHandle);
        }

        // Sanitise floats before forwarding to the audio runtime. SceneSerializer
        // round-trips these through YAML (SceneSerializer.cpp ~L1587), so NaN/Inf
        // from a hand-edited scene or a future network-spawn path could land here.
        // Per cpp-coding-quality §2b: substitute the default rather than passing
        // garbage to the audio engine.
        const f32 safeVolume = std::isfinite(sgc.VolumeMultiplier) ? sgc.VolumeMultiplier : 1.0f;
        const f32 safePitch = std::isfinite(sgc.PitchMultiplier) ? sgc.PitchMultiplier : 1.0f;
        if (!std::isfinite(sgc.VolumeMultiplier))
        {
            OLO_CORE_WARN("Scene::InitializeAudioSoundGraph - non-finite VolumeMultiplier on asset {}; using 1.0", sgc.SoundGraphHandle);
        }
        if (!std::isfinite(sgc.PitchMultiplier))
        {
            OLO_CORE_WARN("Scene::InitializeAudioSoundGraph - non-finite PitchMultiplier on asset {}; using 1.0", sgc.SoundGraphHandle);
        }
        sgc.Sound->SetVolume(safeVolume);
        sgc.Sound->SetPitch(safePitch);
        sgc.Sound->SetLooping(sgc.Looping);

        if (sgc.PlayOnAwake)
        {
            sgc.Sound->Play();
        }
    }

    void Scene::OnRuntimeStart()
    {
        m_IsRunning = true;

        // Deterministic run setup (issue #452): reset the fixed-timestep tick
        // counter / accumulator / animation clock and re-seed the gameplay RNG
        // from the application's configured seed. Doing it here — the single
        // authoritative "entered Play mode" point for both the editor and
        // OloRuntime — means every play-through starts from an identical RNG
        // state, tick 0, and time 0, so a run replays identically given the same
        // seed and inputs. This seeds the GAME THREAD's RandomUtils stream — the
        // one game-thread gameplay (loot rolls, particle jitter, …) consumes. The
        // generator is thread_local, so RNG drawn on worker/job threads rides a
        // separate, time-seeded stream and is NOT reproducible (a #452 follow-up).
        m_SimulationTick = 0;
        m_FixedTimeAccumulator = 0.0f;
        m_SimulationTime = 0.0f;
        // Discard interpolation snapshots so the first rendered frame of a fresh
        // run draws the exact tick-0 pose (no blend from a stale prior run).
        m_InterpPrev.clear();
        m_InterpCurr.clear();
        m_HasInterpSnapshots = false;
        m_RenderInterpAlpha = 0.0f;
        RandomUtils::SetGlobalSeed(Application::Get().GetRandomSeed());

        // Unified diagnostics timeline (#306 item B): the single authoritative fire for
        // "entered Play mode" — the editor copies the scene then calls this; OloRuntime
        // calls it on game start. OnSimulationStart deliberately does not record (it is
        // not a gameplay play).
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Play, "Entered Play mode", 0, GetName());

        // Reset animation-time history so the first runtime frame seeds itself
        // (see OnUpdateRender's m_LastAnimationTime < 0.0f branch). Without
        // this, a stale value carried over from a previous edit-mode session
        // produces a huge dt on the first runtime frame, which shows up as
        // bogus motion vectors in TAA / motion blur for the wind / water /
        // foliage shaders that consume PrevAnimationTime.
        m_LastAnimationTime = -1.0f;

        // Seed the spatial index empty for this session. It is only rebuilt
        // inside OnUpdateRuntime, so without this a Scene that is run, stopped,
        // and run again would expose the previous session's entity positions to
        // any query made between OnRuntimeStart and the first tick. Empty is a
        // valid answer; stale is not. UpdateSpatialIndex remains the sole
        // per-tick rebuild path.
        m_SpatialIndex.Clear();

        // In headless mode, disable rendering
        if (Application::Get().IsHeadless())
        {
            m_RenderingEnabled = false;
        }

        OnPhysics2DStart();
        OnPhysics3DStart();

        if (!Application::Get().IsHeadless())
        {
            InitAudioRuntime();
        }

        // Dialogue system initialization
        InitDialogueSystem();

        // Auto-start cinematics flagged PlayOnStart. CinematicSystem only
        // advances components whose Playing flag is set; this is what flips it
        // on as runtime begins (the edit-mode tick never plays cinematics).
        for (auto e : m_Registry.view<CinematicComponent>())
        {
            if (auto& cine = m_Registry.get<CinematicComponent>(e); cine.PlayOnStart)
            {
                cine.PlayFromStart();
            }
        }

        // Scripting
        {
            ScriptEngine::OnRuntimeStart(this);
            // Instantiate all script entities
            for (const auto scriptView = m_Registry.view<ScriptComponent>(); const auto e : scriptView)
            {
                Entity entity = { e, this };
                ScriptEngine::OnCreateEntity(entity);
            }
        }

        // Lua scripting
        {
            LuaScriptEngine::OnRuntimeStart(this);
            for (const auto luaView = m_Registry.view<LuaScriptComponent>(); const auto e : luaView)
            {
                Entity entity = { e, this };
                auto const& luaComp = entity.GetComponent<LuaScriptComponent>();
                if (!luaComp.ScriptFile.empty())
                {
                    auto scriptPath = Project::GetAssetFileSystemPath(luaComp.ScriptFile);
                    LuaScriptEngine::OnCreateEntity(entity, scriptPath.string());
                }
            }
        }

        // Start animations
        {
            auto animView = m_Registry.view<AnimationStateComponent>();
            for (auto e : animView)
            {
                auto& animState = animView.get<AnimationStateComponent>(e);
                if (animState.m_CurrentClip)
                {
                    animState.m_IsPlaying = true;
                    animState.m_CurrentTime = 0.0f;
                }
            }
        }

        // Scene streaming initialization
        if (m_StreamingSettings.Enabled)
        {
            m_SceneStreamer = std::make_unique<SceneStreamer>();
            SceneStreamerConfig config;
            config.LoadRadius = m_StreamingSettings.DefaultLoadRadius;
            config.UnloadRadius = m_StreamingSettings.DefaultUnloadRadius;
            config.MaxLoadedRegions = m_StreamingSettings.MaxLoadedRegions;
            config.RegionDirectory = m_StreamingSettings.RegionDirectory;
            m_SceneStreamer->Initialize(this, config);
        }

        // Auto-bake NavMesh if agents exist but no valid NavMesh is loaded
        if (!m_NavMesh || !m_NavMesh->IsValid())
        {
            auto agentView = GetAllEntitiesWith<NavAgentComponent>();
            if (agentView.begin() != agentView.end())
            {
                // Determine bounds from NavMeshBoundsComponent or use defaults
                glm::vec3 boundsMin(-100.0f, -10.0f, -100.0f);
                glm::vec3 boundsMax(100.0f, 50.0f, 100.0f);

                auto boundsView = GetAllEntitiesWith<NavMeshBoundsComponent>();
                bool firstBounds = true;
                std::vector<OffMeshLink> links;
                for (auto e : boundsView)
                {
                    const auto& bounds = m_Registry.get<NavMeshBoundsComponent>(e);
                    if (firstBounds)
                    {
                        boundsMin = bounds.m_Min;
                        boundsMax = bounds.m_Max;
                        firstBounds = false;
                    }
                    else
                    {
                        boundsMin = glm::min(boundsMin, bounds.m_Min);
                        boundsMax = glm::max(boundsMax, bounds.m_Max);
                    }
                    links.insert(links.end(), bounds.m_Links.begin(), bounds.m_Links.end());
                }

                OLO_CORE_INFO("[Scene] Auto-baking NavMesh for {} agent(s)...",
                              std::distance(agentView.begin(), agentView.end()));

                NavMeshSettings settings;
                auto navMesh = NavMeshGenerator::Generate(this, settings, boundsMin, boundsMax, links);
                if (navMesh)
                {
                    SetNavMesh(navMesh);
                    OLO_CORE_INFO("[Scene] NavMesh auto-baked: {} polys", navMesh->GetPolyCount());
                }
                else
                {
                    OLO_CORE_WARN("[Scene] NavMesh auto-bake failed — NavAgent pathfinding will not work");
                }
            }
        }
    }

    void Scene::OnRuntimeStop()
    {
        // Unified diagnostics timeline (#306 item B): the authoritative "left Play mode"
        // fire. Recorded up front, while the scene is still intact. Teardown below tears
        // down systems but does not route through Scene::DestroyEntity, so no spurious
        // EntityDestroy flood follows.
        DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::Stop, "Left Play mode", 0, GetName());

        // Stop any global fullscreen video while the GL context is still alive, so its
        // texture is freed here rather than at static-destruction time (no context).
        // Per-entity video players are torn down with their components on scene destruction.
        VideoSystem::StopFullscreen();

        // Shut down streaming before other systems
        if (m_SceneStreamer)
        {
            m_SceneStreamer->Shutdown();
            m_SceneStreamer.reset();
        }

        ScriptEngine::OnRuntimeStop();

        // Snapshot entity IDs before dispatching Lua OnDestroy — callbacks may
        // destroy other entities and mutate the underlying view.
        std::vector<entt::entity> luaEntities;
        for (const auto luaView = m_Registry.view<LuaScriptComponent>(); const auto e : luaView)
            luaEntities.push_back(e);

        for (const auto e : luaEntities)
        {
            if (!m_Registry.valid(e))
                continue;
            if (auto const* lsc = m_Registry.try_get<LuaScriptComponent>(e); lsc && !lsc->ScriptFile.empty())
            {
                LuaScriptEngine::OnDestroyEntity({ e, this });
            }
        }
        // LuaScriptEngine::OnRuntimeStop releases any GOAP agents built from Lua
        // (their actions hold sol callbacks) before the Lua state can be torn down.
        LuaScriptEngine::OnRuntimeStop();

        // Defer clearing the running flag until after Lua teardown so that
        // per-entity Lua cleanup in DestroyEntity still fires during callbacks.
        m_IsRunning = false;

        // Shut down dialogue system
        m_DialogueSystem.reset();
        m_DialogueVariables.Clear();

        // Reset per-entity dialogue runtime state
        for (auto view = m_Registry.view<DialogueComponent>(); auto&& [e, dc] : view.each())
        {
            dc.m_HasTriggered = false;
        }
        m_Registry.clear<DialogueStateComponent>();

        for (auto view = m_Registry.view<AudioSourceComponent>(); auto&& [e, ac] : view.each())
        {
            if (ac.Source)
                ac.Source->Stop();
            ac.ActiveEventID = 0;
        }

        for (auto view = m_Registry.view<AudioSoundGraphComponent>(); auto&& [e, sgc] : view.each())
        {
            if (sgc.Sound)
            {
                sgc.Sound->Stop();
                sgc.Sound->ReleaseResources();
                sgc.Sound = nullptr;
            }
        }

        // Shut down audio events system
        if (m_AudioEventsManager)
        {
            m_AudioEventsManager->Shutdown();
            Audio::AudioPlayback::SetManager(nullptr);
            m_AudioEventsManager.reset();
        }
        m_AudioCommandRegistry.reset();

        OnPhysics2DStop();
        OnPhysics3DStop();

        m_RuntimeSnowPrevPositions.Empty();
        m_EditorSnowPrevPositions.Empty();

        // Reset animation clock on stop so re-entering runtime (or switching
        // to edit mode) seeds a fresh baseline instead of leaking a stale
        // PrevAnimationTime into TAA / motion-blur / wind / water shaders.
        m_LastAnimationTime = -1.0f;

        // Drop gameplay-event subscriptions so a stop/play cycle doesn't
        // accumulate stale handlers (scripts/UI re-subscribe on next start).
        if (m_GameplayEventBus)
        {
            m_GameplayEventBus->Clear();
        }

        // Drop UI focus + widget-event delegates so a stop/play cycle starts with
        // a clean focus state and no stale callbacks (scripts/UI re-register on start).
        if (m_UINavigation)
        {
            m_UINavigation->Clear();
        }

        // Drop any ephemeral MCP sun-direction override (#316 Part 4) so leaving
        // Play mode restores the authored procedural-sky sun. The override is a
        // diagnostics-server lighting-iteration aid; it must never outlive a
        // play/stop cycle. A no-op when none is active.
        Renderer3D::ClearSunDirectionOverride();
    }

    void Scene::OnSimulationStart()
    {
        // Same reset as OnRuntimeStart — simulation mode also re-baselines the
        // animation clock so first-frame velocity reprojection isn't bogus, and
        // zeroes the deterministic fixed-timestep clock so each Simulate session
        // starts from tick 0 / time 0. Without the m_SimulationTime reset the
        // buoyancy wave phase would keep accumulating across Simulate start/stop
        // cycles (issue #452).
        m_LastAnimationTime = -1.0f;
        m_SimulationTick = 0;
        m_FixedTimeAccumulator = 0.0f;
        m_SimulationTime = 0.0f;
        m_InterpPrev.clear();
        m_InterpCurr.clear();
        m_HasInterpSnapshots = false;
        m_RenderInterpAlpha = 0.0f;

        OnPhysics2DStart();
        OnPhysics3DStart();
    }

    void Scene::OnSimulationStop()
    {
        OnPhysics2DStop();
        OnPhysics3DStop();

        // Mirror OnRuntimeStop so returning to edit mode doesn't leak stale
        // animation-clock history into shaders that consume PrevAnimationTime.
        m_LastAnimationTime = -1.0f;
    }

    void Scene::SetNavMesh(const Ref<NavMesh>& navMesh)
    {
        m_NavMesh = navMesh;
        if (navMesh && navMesh->IsValid())
        {
            m_NavMeshQuery = std::make_unique<NavMeshQuery>(navMesh);
            m_CrowdManager = std::make_unique<CrowdManager>();
            m_CrowdManager->Initialize(navMesh);
        }
        else
        {
            m_NavMeshQuery.reset();
            m_CrowdManager.reset();
        }

        // Reset per-agent runtime state so no entity keeps stale IDs or paths
        auto agentView = GetAllEntitiesWith<NavAgentComponent>();
        for (auto e : agentView)
        {
            auto& agent = m_Registry.get<NavAgentComponent>(e);
            agent.m_CrowdAgentId = -1;
            agent.m_HasTarget = false;
            agent.m_HasPath = false;
            agent.m_PathCorners.clear();
            agent.m_CurrentCornerIndex = 0;
        }
    }

    // Process sub-emitter triggers that reference child systems.
    // For triggers with a valid ChildSystemIndex, emit into the corresponding child system's pool.
    static void ProcessChildSubEmitters(ParticleSystemComponent& psc, f32 dt, const glm::vec3& emitterPos)
    {
        OLO_PROFILE_FUNCTION();

        const auto& triggers = psc.System.GetPendingTriggers();
        if (triggers.empty() || psc.ChildSystems.empty())
        {
            return;
        }

        auto& rng = RandomUtils::GetGlobalRandom();

        for (const auto& trigger : triggers)
        {
            if (trigger.ChildSystemIndex < 0 || static_cast<u32>(trigger.ChildSystemIndex) >= psc.ChildSystems.size())
            {
                continue; // Legacy parent-pool trigger or invalid index
            }

            auto& childSystem = psc.ChildSystems[trigger.ChildSystemIndex];
            auto& childPool = childSystem.GetPool();
            const auto& childEmitter = childSystem.Emitter;

            u32 firstSlot = childPool.GetAliveCount();
            u32 emitted = childPool.Emit(trigger.EmitCount);

            for (u32 i = 0; i < emitted; ++i)
            {
                u32 idx = firstSlot + i;
                childPool.m_Positions[idx] = trigger.Position;

                glm::vec3 randomVec(
                    rng.GetFloat32InRange(-1.0f, 1.0f),
                    rng.GetFloat32InRange(-1.0f, 1.0f),
                    rng.GetFloat32InRange(-1.0f, 1.0f));
                f32 randomLen = glm::length(randomVec);
                glm::vec3 dir = (randomLen > 0.0001f) ? randomVec / randomLen : glm::vec3(0.0f, 1.0f, 0.0f);
                f32 speed = childEmitter.InitialSpeed + rng.GetFloat32InRange(-childEmitter.SpeedVariance, childEmitter.SpeedVariance);
                glm::vec3 velocity = dir * std::max(speed, 0.0f) + trigger.Velocity;
                childPool.m_Velocities[idx] = velocity;
                childPool.m_InitialVelocities[idx] = velocity;

                childPool.m_Colors[idx] = childEmitter.InitialColor;
                childPool.m_InitialColors[idx] = childEmitter.InitialColor;

                f32 size = std::max(childEmitter.InitialSize + rng.GetFloat32InRange(-childEmitter.SizeVariance, childEmitter.SizeVariance), 0.0f);
                childPool.m_Sizes[idx] = size;
                childPool.m_InitialSizes[idx] = size;
                childPool.m_Rotations[idx] = childEmitter.InitialRotation + rng.GetFloat32InRange(-childEmitter.RotationVariance, childEmitter.RotationVariance);

                f32 lifetime = rng.GetFloat32InRange(std::min(childEmitter.LifetimeMin, childEmitter.LifetimeMax), std::max(childEmitter.LifetimeMin, childEmitter.LifetimeMax));
                childPool.m_Lifetimes[idx] = lifetime;
                childPool.m_MaxLifetimes[idx] = lifetime;
            }
        }

        // Tick child systems (they run their own modules independent of parent)
        for (auto& childSystem : psc.ChildSystems)
        {
            if (childSystem.GetAliveCount() > 0 || childSystem.Playing)
            {
                // Child systems emit only from triggers, suppress normal rate-based emission
                // by zeroing the LOD multiplier (which scales rate without mutating RateOverTime)
                childSystem.SetLODSpawnRateMultiplier(0.0f);
                childSystem.Update(dt, emitterPos);
            }
        }
    }

    // Helper to set GL blend state for particle rendering
    static void SetParticleBlendMode(ParticleBlendMode mode)
    {
        // Flush any pending Renderer2D batch before changing GL blend state,
        // otherwise queued quads may render with the wrong blend mode (§1.5)
        Renderer2D::Flush();

        switch (mode)
        {
            case ParticleBlendMode::Alpha:
                RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case ParticleBlendMode::Additive:
                RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE);
                break;
            case ParticleBlendMode::PremultipliedAlpha:
                RenderCommand::SetBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                break;
        }
    }

    // Helper to restore default blend mode after particle rendering
    static void RestoreDefaultBlendMode()
    {
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // Per-entity CPU morph-target deformation: deform the mesh by the component's
    // active weights (or restore the base mesh when they go inactive) and re-upload
    // the vertex buffer. Shared by the runtime (OnUpdateRuntime) and editor-preview
    // (OnUpdateEditor) morph passes so the two can't drift. Weights are produced
    // upstream by the animation/graph samplers or by script/preset writes.
    static void EvaluateEntityMorphTargets(MorphTargetComponent& morphComp, MeshComponent& meshComp)
    {
        if (!meshComp.m_MeshSource)
            return;

        // Auto-populate MorphTargets from MeshSource if not already set
        if (!morphComp.MorphTargets && meshComp.m_MeshSource->HasMorphTargets())
            morphComp.MorphTargets = meshComp.m_MeshSource->GetMorphTargets();

        if (!morphComp.HasActiveWeights() || !morphComp.MorphTargets)
        {
            // Restore base mesh only on transition from active → inactive
            if (morphComp.WasMorphActive && !morphComp.BasePositions.empty() && meshComp.m_MeshSource)
            {
                auto& meshSource = meshComp.m_MeshSource;
                auto& mutableVerts = meshSource->GetVertices();
                for (u32 i = 0; i < static_cast<u32>(morphComp.BasePositions.size()) && i < static_cast<u32>(mutableVerts.Num()); ++i)
                {
                    mutableVerts[i].Position = morphComp.BasePositions[i];
                    mutableVerts[i].Normal = morphComp.BaseNormals[i];
                }
                auto& vb = const_cast<Ref<VertexBuffer>&>(meshSource->GetVertexBuffer());
                vb->SetData({ mutableVerts.GetData(), static_cast<u32>(mutableVerts.Num() * sizeof(Vertex)) });
            }
            morphComp.WasMorphActive = false;
            return;
        }

        auto& meshSource = meshComp.m_MeshSource;
        auto& vertices = meshSource->GetVertices();

        // Cache base vertex data on first evaluation
        if (morphComp.BasePositions.empty() && vertices.Num() > 0)
        {
            morphComp.BasePositions.resize(vertices.Num());
            morphComp.BaseNormals.resize(vertices.Num());
            for (u32 i = 0; i < static_cast<u32>(vertices.Num()); ++i)
            {
                morphComp.BasePositions[i] = vertices[i].Position;
                morphComp.BaseNormals[i] = vertices[i].Normal;
            }
        }

        if (morphComp.BasePositions.empty())
            return;

        // Evaluate morph deformation
        std::vector<glm::vec3> outPositions;
        std::vector<glm::vec3> outNormals;
        if (MorphTargetSystem::EvaluateMorphTargets(morphComp,
                                                    morphComp.BasePositions, morphComp.BaseNormals,
                                                    outPositions, outNormals))
        {
            // Write deformed data back into MeshSource vertices and re-upload to the GPU
            auto& mutableVerts = meshSource->GetVertices();
            for (u32 i = 0; i < static_cast<u32>(outPositions.size()) && i < static_cast<u32>(mutableVerts.Num()); ++i)
            {
                mutableVerts[i].Position = outPositions[i];
                mutableVerts[i].Normal = outNormals[i];
            }

            auto& vb = const_cast<Ref<VertexBuffer>&>(meshSource->GetVertexBuffer());
            vb->SetData({ mutableVerts.GetData(), static_cast<u32>(mutableVerts.Num() * sizeof(Vertex)) });
        }
        morphComp.WasMorphActive = true;
    }

    void Scene::UpdateSpatialIndex()
    {
        OLO_PROFILE_FUNCTION();

        // Full rebuild from scratch each call: entities move every frame, so an
        // incremental update would re-bin nearly all of them anyway, and a clean
        // rebuild drops destroyed entities for free (no stale-handle bookkeeping).
        // Read IDComponent + TransformComponent together so we get the UUID
        // without an Entity-wrapper round-trip per entity.
        m_SpatialIndex.Clear();
        for (auto&& [e, id, transform] : m_Registry.view<IDComponent, TransformComponent>().each())
        {
            m_SpatialIndex.Insert(id.ID, transform.Translation);
        }
    }

    glm::mat4 Scene::GetWorldTransform(entt::entity entity) const
    {
        if (auto const* world = m_Registry.try_get<WorldTransformComponent>(entity))
            return world->WorldMatrix;
        return m_Registry.get<TransformComponent>(entity).GetTransform();
    }

    void Scene::PropagateWorldTransforms()
    {
        OLO_PROFILE_FUNCTION();

        // Resolves an entity's parent, treating a missing/invalid/dangling
        // RelationshipComponent::m_ParentHandle (e.g. DestroyEntity doesn't
        // clean up former children — see HierarchyChildFollowsPhysicsParentTest)
        // as "no parent" rather than propagating a stale reference.
        auto resolveParent = [this](entt::entity entity) -> entt::entity
        {
            auto const* rel = m_Registry.try_get<RelationshipComponent>(entity);
            if (!rel || static_cast<u64>(rel->m_ParentHandle) == 0)
                return entt::null;

            auto const* parentEntity = m_EntityMap.Find(rel->m_ParentHandle);
            if (!parentEntity || !m_Registry.valid(*parentEntity) || !m_Registry.all_of<TransformComponent>(*parentEntity))
                return entt::null;

            return *parentEntity;
        };

        auto view = m_Registry.view<TransformComponent>();

        // Flat, depth-sorted traversal order: breadth-first from every root
        // (no resolvable parent) so a parent always precedes its children.
        // Buffers are persistent Scene members (issue #499 perf follow-up) —
        // clear() keeps prior capacity, avoiding a fresh allocation every tick.
        std::vector<entt::entity>& order = m_TransformOrder;
        order.clear();
        order.reserve(view.size());

        std::unordered_set<entt::entity>& visited = m_TransformVisited;
        visited.clear();
        visited.reserve(view.size());

        std::vector<entt::entity>& queue = m_TransformQueue;
        queue.clear();
        queue.reserve(view.size());
        for (auto entity : view)
        {
            if (resolveParent(entity) == entt::null)
                queue.push_back(entity);
        }

        for (sizet head = 0; head < queue.size(); ++head)
        {
            entt::entity const entity = queue[head];
            if (!visited.insert(entity).second)
                continue;

            order.push_back(entity);

            auto const* rel = m_Registry.try_get<RelationshipComponent>(entity);
            if (!rel)
                continue;

            for (UUID const childId : rel->m_Children)
            {
                auto const* childEntity = m_EntityMap.Find(childId);
                if (!childEntity || !m_Registry.valid(*childEntity) || visited.contains(*childEntity))
                    continue;
                if (!m_Registry.all_of<TransformComponent>(*childEntity))
                    continue;
                // Only descend if the child's own resolved parent agrees this
                // entity is it — guards against m_Children/m_ParentHandle
                // drifting out of sync (data corruption, not a supported path).
                if (resolveParent(*childEntity) != entity)
                    continue;

                queue.push_back(*childEntity);
            }
        }

        // Defensive fallback: any TransformComponent entity unreachable from a
        // root (corrupted relationship data, or a cycle that slipped past
        // Entity::SetParent's guard via direct deserialization) still gets a
        // defined world matrix (== its local transform) instead of staying
        // stale from a prior tick.
        for (auto entity : view)
        {
            if (visited.insert(entity).second)
                order.push_back(entity);
        }

        // One linear sweep: parents are guaranteed to precede their children,
        // so each child composes against an already-written parent world matrix.
        for (auto entity : order)
        {
            glm::mat4 const& local = m_Registry.get<TransformComponent>(entity).GetTransform();
            entt::entity const parent = resolveParent(entity);

            glm::mat4 world = local;
            if (parent != entt::null)
            {
                if (auto const* parentWorld = m_Registry.try_get<WorldTransformComponent>(parent))
                    world = parentWorld->WorldMatrix * local;
            }

            m_Registry.emplace_or_replace<WorldTransformComponent>(entity, world);
        }
    }

    void Scene::OnUpdateRuntime(Timestep const ts)
    {
        PerformanceProfiler* perfProfiler = nullptr;
        if (auto* app = Application::TryGet())
        {
            perfProfiler = app->GetPerformanceProfiler();
        }
        OLO_PERF_SCOPE("Scene::OnUpdateRuntime", perfProfiler);

        UpdateStreaming();

        // Advance the gameplay simulation by exactly `ts`. This single-step
        // contract is relied on by the Functional-test harness, the headless
        // server, and any caller that hand-feeds a timestep. Real-time windowed
        // hosts call OnUpdateRuntimeFixed instead, which drives this same step
        // at a fixed dt via an accumulator. The pause / single-step gate is the
        // original `m_StepFrames-- > 0` post-decrement — only evaluated while
        // paused — so frame-by-frame editor scrubbing is unchanged.
        if (!m_IsPaused || m_StepFrames-- > 0)
        {
            SimulateRuntimeStep(ts);
        }

        RenderRuntime(ts);
    }

    void Scene::OnUpdateRuntimeFixed(Timestep const frameTs, f32 const fixedDt)
    {
        PerformanceProfiler* perfProfiler = nullptr;
        if (auto* app = Application::TryGet())
        {
            perfProfiler = app->GetPerformanceProfiler();
        }
        OLO_PERF_SCOPE("Scene::OnUpdateRuntimeFixed", perfProfiler);

        UpdateStreaming();

        // Sanitize the incoming frame delta before it ever reaches the
        // accumulator. A non-finite (NaN/Inf) or negative value must not get in:
        // NaN compares false against every threshold, so it would neither clamp
        // nor step — it would stick in m_FixedTimeAccumulator forever and
        // permanently freeze the simulation (while RenderRuntime kept drawing).
        // The original OnUpdateRuntime carried no state and self-corrected the
        // next frame; the accumulator does not, so guard it here.
        f32 safeFrameTs = static_cast<f32>(frameTs);
        if (!std::isfinite(safeFrameTs) || safeFrameTs < 0.0f)
        {
            safeFrameTs = 0.0f;
        }

        // A non-positive fixedDt would loop forever / divide the wall clock by
        // zero — treat it as "no simulation this frame" and just render.
        if (fixedDt > 0.0f)
        {
            if (m_IsPaused)
            {
                // Frame-by-frame stepping while paused: one queued step advances
                // exactly one fixed tick (mirrors OnUpdateRuntime's gate). Real
                // wall-time is intentionally not accumulated while paused, so the
                // accumulator can't dump a burst of catch-up steps on un-pause.
                if (m_StepFrames > 0)
                {
                    --m_StepFrames;
                    SimulateRuntimeStep(fixedDt);
                }
            }
            else
            {
                m_FixedTimeAccumulator += safeFrameTs;

                // Spiral-of-death guard: if a hitch left more than the cap's
                // worth of time queued, clamp and drop the excess so the sim
                // slows rather than freezing the host while it tries to catch
                // up. Mirrors Application::s_MaxTimestep and JoltScene's own
                // accumulator clamp. Note the determinism guarantee this enables
                // is "same fixed-tick sequence + same inputs => same state"
                // (what rollback/replay drive off), NOT "two wall-clock runs that
                // hitch differently end identically" — a hitch past the cap drops
                // real time on the real-time presentation path by design.
                if (const f32 maxAccumulator = static_cast<f32>(kMaxFixedStepsPerFrame) * fixedDt;
                    m_FixedTimeAccumulator > maxAccumulator)
                {
                    m_FixedTimeAccumulator = maxAccumulator;
                }

                u32 steps = 0;
                while (m_FixedTimeAccumulator >= fixedDt && steps < kMaxFixedStepsPerFrame)
                {
                    // Render interpolation (issue #502): snapshot the live local
                    // transforms into m_InterpPrev BEFORE each step so, after the
                    // loop, m_InterpPrev holds the state one tick behind the final
                    // step and m_InterpCurr (captured below) holds the post-step
                    // state. RenderRuntime blends between them by alpha. Skipped
                    // entirely when interpolation is off. The overwrite each
                    // iteration is intentional — only the pre-last-step pose
                    // survives, which is exactly the "previous" state to blend from.
                    if (m_RenderInterpolationEnabled)
                    {
                        CaptureLocalTransforms(m_InterpPrev);
                    }
                    SimulateRuntimeStep(fixedDt);
                    m_FixedTimeAccumulator -= fixedDt;
                    ++steps;
                }

                // Publish the fresh "current" pose and the blend factor for the
                // render below. Only when a step actually ran this frame — an
                // idle frame (accumulator < fixedDt) keeps the existing pair and
                // just advances alpha, which is what fills the gap between ticks.
                if (steps > 0 && m_RenderInterpolationEnabled)
                {
                    CaptureLocalTransforms(m_InterpCurr);
                    m_HasInterpSnapshots = true;
                }

                // Blend factor for this frame's render: how far the leftover
                // accumulator has advanced toward the next fixed tick, in [0, 1].
                // Paused / fixedDt<=0 frames leave it untouched (last value), so a
                // paused view shows the frozen pose it last rendered.
                m_RenderInterpAlpha = std::clamp(m_FixedTimeAccumulator / fixedDt, 0.0f, 1.0f);
            }
        }

        RenderRuntime(safeFrameTs);
    }

    void Scene::CaptureLocalTransforms(std::unordered_map<u32, InterpTransform>& out)
    {
        out.clear();
        auto view = m_Registry.view<TransformComponent>();
        out.reserve(view.size());
        for (auto entity : view)
        {
            const auto& tc = view.get<TransformComponent>(entity);
            out.emplace(static_cast<u32>(std::to_underlying(entity)),
                        InterpTransform{ tc.Translation, tc.GetRotation(), tc.Scale });
        }
    }

    bool Scene::ShouldInterpolateThisFrame() const
    {
        // No point blending when the toggle is off, we have no prior state to
        // blend from, or we're sitting exactly on a tick boundary (alpha == 0
        // renders the current pose verbatim, so skip the overwrite cost).
        return m_RenderInterpolationEnabled && m_HasInterpSnapshots && m_RenderInterpAlpha > 0.0f;
    }

    bool Scene::ComputeInterpolatedLocal(entt::entity entity, InterpTransform& out) const
    {
        if (!m_RenderInterpolationEnabled || !m_HasInterpSnapshots)
        {
            return false;
        }

        const auto id = static_cast<u32>(std::to_underlying(entity));
        const auto prevIt = m_InterpPrev.find(id);
        const auto currIt = m_InterpCurr.find(id);
        // An entity present in only one snapshot (spawned or destroyed between
        // the two ticks) has no meaningful blend — render it at its live pose.
        if (prevIt == m_InterpPrev.end() || currIt == m_InterpCurr.end())
        {
            return false;
        }

        const InterpTransform& prev = prevIt->second;
        const InterpTransform& curr = currIt->second;
        const f32 a = m_RenderInterpAlpha;

        out.Translation = glm::mix(prev.Translation, curr.Translation, a);
        out.Scale = glm::mix(prev.Scale, curr.Scale, a);
        // Shortest-arc quaternion blend so a near-180° tick doesn't spin the
        // long way. glm::slerp normalizes and picks the shorter hemisphere.
        out.Rotation = glm::slerp(prev.Rotation, curr.Rotation, a);
        return true;
    }

    glm::mat4 Scene::GetInterpolatedLocalTransform(entt::entity entity) const
    {
        if (InterpTransform interp; ComputeInterpolatedLocal(entity, interp))
        {
            return glm::translate(glm::mat4(1.0f), interp.Translation) *
                   glm::toMat4(interp.Rotation) *
                   glm::scale(glm::mat4(1.0f), interp.Scale);
        }
        // Fall back to the live local transform.
        if (auto const* tc = m_Registry.try_get<TransformComponent>(entity))
        {
            return tc->GetTransform();
        }
        return glm::mat4(1.0f);
    }

    void Scene::UpdateStreaming()
    {
        // Refresh LocalizedTextComponent → TextComponent.TextString if the
        // active locale changed since last frame. Cheap when no change.
        LocalizationSystem::UpdateLocalizedText(*this);
        // Scene streaming update (runs even when paused to finish pending loads)
        if (m_SceneStreamer)
        {
            glm::vec3 camPos{ 0.0f };
            if (auto cam = GetPrimaryCameraEntity(); cam)
            {
                camPos = cam.GetComponent<TransformComponent>().Translation;
            }
            ++m_StreamingFrameCounter;
            m_SceneStreamer->Update(camPos, m_StreamingFrameCounter);
        }
    }

    void Scene::StepPhysics(Timestep const ts)
    {
        // Guard against ticking before OnPhysics2DStart created a world —
        // the Jolt path below already does this; matching it keeps the
        // tick safe for headless tests / minimal scenes that never opt
        // in to 2D physics.
        if (b2World_IsValid(m_PhysicsWorld))
        {
            b2World_Step(m_PhysicsWorld, ts.GetSeconds(), kPhysics2DVelocityIterations);
        }

        // Update 3D physics
        if (m_JoltScene)
        {
            // Buoyancy forces must be queued BEFORE the step so Jolt
            // integrates them this frame. Drive the wave phase from the
            // deterministic m_SimulationTime (NOT wall-clock Time::GetTime),
            // so buoyant bodies are reproducible across frame pacings and
            // rollback re-sim (issue #452). At a steady frame rate this
            // equals wall-time, so floating bodies still track the rendered
            // wave crests (Physics3D/BuoyancySystem, WATER §5.1).
            BuoyancySystem::OnUpdate(this, m_SimulationTime, ts.GetSeconds());
            // JoltScene::Simulate runs its OWN fixed-step accumulator at a
            // hardcoded 1/60. This is 1:1 with one gameplay tick only when
            // the canonical fixed dt (Application::GetFixedTimeStep) equals
            // 1/60 — the production case. If they ever diverge, 3D physics
            // and scripts/animation/2D-physics tick at different rates; keep
            // them equal (issue #452 follow-up: single shared fixed step).
            m_JoltScene->Simulate(ts.GetSeconds());
        }

        PostPhysicsSync();
    }

    void Scene::KickPhysicsStep(Timestep const ts)
    {
        const f32 frameSeconds = ts.GetSeconds();

        if (m_JoltScene)
        {
            // Buoyancy forces must be queued BEFORE the world step so Jolt
            // integrates them this frame (see StepPhysics for the deterministic
            // m_SimulationTime reasoning — issue #452).
            BuoyancySystem::OnUpdate(this, m_SimulationTime, frameSeconds);
            // Contact handlers may grow script/registry access — game thread only.
            m_JoltScene->PreSimulate();
            m_PhysicsStepsThisFrame = m_JoltScene->BeginSteps(frameSeconds);
        }
        else
        {
            m_PhysicsStepsThisFrame = 0;
        }

        // Async single-step fast path (the production 1:1 fixed-tick case): run
        // the ECS-reading character/vehicle phase here on the game thread, then
        // launch the ECS-free world step (Box2D + Jolt world update) as a task.
        // The physics-shadow systems registered between PhysicsKick and
        // PhysicsFence execute on the game thread while it runs. High priority:
        // the fence blocks the rest of the tick on it. With zero workers,
        // Tasks::Launch executes the body inline right here, which degrades to
        // the synchronous order.
        m_PhysicsStepRanAsync = m_JoltScene && m_PhysicsStepsThisFrame == 1 &&
                                SystemScheduler::IsParallelExecutionEnabled();
        if (m_PhysicsStepRanAsync)
        {
            const f32 fixedDt = m_JoltScene->GetFixedTimeStep();
            m_JoltScene->StepCharacterAndVehiclePhase(fixedDt);
            m_PhysicsStepTask = Tasks::Launch(
                "Scene::PhysicsWorldStep",
                [this, frameSeconds, fixedDt]
                {
                    if (b2World_IsValid(m_PhysicsWorld))
                    {
                        b2World_Step(m_PhysicsWorld, frameSeconds, kPhysics2DVelocityIterations);
                    }
                    m_JoltScene->StepWorldPhase(fixedDt);
                },
                Tasks::ETaskPriority::High);
        }
        else
        {
            // Synchronous fallback — idle frames (0 steps), hitch catch-up
            // (N ≥ 2, where the per-sub-step character/world interleave must be
            // preserved), the sequential kill-switch, or no 3D physics. Stepping
            // order matches StepPhysics exactly.
            if (b2World_IsValid(m_PhysicsWorld))
            {
                b2World_Step(m_PhysicsWorld, frameSeconds, kPhysics2DVelocityIterations);
            }
            if (m_JoltScene)
            {
                const f32 fixedDt = m_JoltScene->GetFixedTimeStep();
                for (u32 stepIndex = 0; stepIndex < m_PhysicsStepsThisFrame; ++stepIndex)
                {
                    m_JoltScene->Step(fixedDt);
                }
            }
        }
    }

    void Scene::FencePhysicsStep()
    {
        // Join the in-flight world step. A default (invalid) handle — the
        // synchronous fallback — waits on nothing.
        if (m_PhysicsStepTask.IsValid())
        {
            m_PhysicsStepTask.Wait();
            m_PhysicsStepTask = {};
        }

        if (m_JoltScene)
        {
            if (m_PhysicsStepRanAsync)
            {
                // The async path defers the joint-break phase here: it reads
                // components / publishes events (game thread), and the per-step
                // constraint impulses it consumes must be read before the NEXT
                // world update overwrites them — i.e. before the next kick.
                m_JoltScene->StepJointBreakPhase(m_JoltScene->GetFixedTimeStep());
            }
            // Write body poses back to the ECS transforms (both paths — the
            // synchronous fallback stepped the world without syncing).
            m_JoltScene->SynchronizeTransforms();
        }

        PostPhysicsSync();
        m_PhysicsStepRanAsync = false;
        m_PhysicsStepsThisFrame = 0;
    }

    void Scene::PostPhysicsSync()
    {
        // Cloth readback (issue #460): pull each soft body's deformed world-space
        // particle positions into its CPU buffers and recompute smooth normals. This
        // is GPU-free (no render mesh touched here), so it runs headless / on the
        // dedicated server; the render pass uploads these to the deforming VBO.
        if (m_JoltScene)
        {
            for (auto& [entityID, state] : m_ClothRuntime)
            {
                if (m_JoltScene->GetClothVertices(entityID, state.m_Positions))
                    ComputeClothNormals(state.m_Positions, state.m_Columns, state.m_Rows, state.m_Normals);
            }
        }

        // Retrieve transform from Box2D
        if (b2World_IsValid(m_PhysicsWorld))
        {
            for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
            {
                Entity entity = { e, this };
                auto& transform = entity.GetComponent<TransformComponent>();
                const auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

                // Runtime-added Rigidbody2DComponent has RuntimeBody ==
                // b2_nullBodyId until OnPhysics2DStart creates one.
                // Skip rather than calling Box2D with an invalid handle.
                if (!b2Body_IsValid(rb2d.RuntimeBody))
                    continue;

                b2Vec2 position = b2Body_GetPosition(rb2d.RuntimeBody);
                b2Rot rotation = b2Body_GetRotation(rb2d.RuntimeBody);

                transform.Translation.x = position.x;
                transform.Translation.y = position.y;
                {
                    glm::vec3 euler = transform.GetRotationEuler();
                    euler.z = b2Rot_GetAngle(rotation);
                    transform.SetRotationEuler(euler);
                }
            }
        }

        // Retrieve transforms from Jolt 3D physics. Partial-owning group: owns
        // Rigidbody3DComponent, borrows TransformComponent (owned by the sprite
        // loop) — see the ownership map at the top of this file (issue #443).
        for (auto rbGroup = m_Registry.group<Rigidbody3DComponent>(entt::get<TransformComponent>); const auto e : rbGroup)
        {
            Entity entity = { e, this };
            auto& transform = rbGroup.get<TransformComponent>(e);
            const auto& rb3d = rbGroup.get<Rigidbody3DComponent>(e);

            if (rb3d.m_RuntimeBodyToken != 0 && rb3d.m_Type != BodyType3D::Static && m_JoltScene)
            {
                // Get the body from JoltScene and sync transforms
                auto body = m_JoltScene->GetBody(entity);
                if (body)
                {
                    auto pos = body->GetPosition();
                    auto rot = body->GetRotation();

                    transform.Translation = pos;
                    transform.SetRotation(glm::normalize(rot));
                }
            }
        }

        // Retrieve transforms from Jolt character controllers — without
        // this loop the controller's CharacterVirtual moves internally
        // but the entity's TransformComponent never updates, so the
        // character "walks" only inside Jolt while ECS thinks it never
        // moved. Mirrors the rigid-body sync above.
        if (m_JoltScene)
        {
            for (const auto view = m_Registry.view<CharacterController3DComponent, TransformComponent>(); const auto e : view)
            {
                Entity entity = { e, this };
                if (auto controller = m_JoltScene->GetCharacterController(entity))
                {
                    auto& transform = entity.GetComponent<TransformComponent>();
                    transform.Translation = controller->GetTranslation();
                    transform.SetRotation(controller->GetRotation());
                }
            }
        }
    }

    // Named resource channels for the gameplay schedule below — constants, not
    // ad-hoc string literals at each declaration site, so a channel can never
    // silently fork into two spellings (the derived edges only connect EXACT
    // matches; a typo'd channel would just drop its edges without any error).
    namespace GameplayChannel
    {
        // Component-space TransformComponent data (Translation/Rotation/Scale).
        constexpr std::string_view kLocalTransforms = "LocalTransforms";
        // Parent-composed world matrices published by PropagateWorldTransforms.
        // No in-schedule reader today — rendering and external queries consume
        // it after the tick — but writers/readers declared against it keep the
        // compose pass ordered after every local-transform mover (WAR edges).
        constexpr std::string_view kWorldTransforms = "WorldTransforms";
        // Morph-target weights sampled by the animation systems.
        constexpr std::string_view kMorphWeights = "MorphWeights";
        // The rebuilt SceneSpatialIndex.
        constexpr std::string_view kSpatialIndex = "SpatialIndex";
        // AI sight-sensing results.
        constexpr std::string_view kPerception = "Perception";
        // The in-flight physics world step: written by PhysicsKick (launching the
        // task), consumed by PhysicsFence (joining it). Any system declaring
        // NEITHER this channel NOR a physics-touched channel may sit between them
        // — the physics shadow.
        constexpr std::string_view kPhysicsInFlight = "PhysicsInFlight";
    } // namespace GameplayChannel

    SystemScheduler& Scene::GetGameplayScheduler()
    {
        // Built once, thread-safe (function-local static) and shared across every
        // Scene / tick — each exec callback takes the Scene by reference and
        // captures nothing instance-specific. The derived topological order equals
        // the registration order below (the tie-break), and every REAL data
        // dependency is declared (documented inline) so a future reordering that
        // violates one is caught by the sort instead of silently corrupting a tick.
        //
        // ── Deliberate deltas from the pre-#453 hard-coded sequence ────────────
        // The physics step is split into PhysicsKick / PhysicsFence (UE
        // TG_DuringPhysics analog): the ECS-free world update runs as an engine
        // task while the GAME THREAD runs the "physics shadow" — the systems
        // registered between kick and fence. Shadow systems need NO worker-
        // thread-safety audit (they stay on the game thread); they only must be
        // independent of physics state, transforms, and in-flight Jolt queries.
        //   * Dialogue moved from its historical pre-animation slot into the
        //     shadow: it touches no transforms, no physics, no animation state
        //     (typewriter progress + UI entity churn + input — all game-thread).
        //   * Quest moved from its historical post-AI slot into the shadow: it
        //     ticks journal timers and publishes bus events; subscribers still
        //     run on the game thread.
        // Their shadow placement is tie-break positioning (they have no edges);
        // the schedule order test pins it, and DependsOn seam tests prove the
        // shadow legality (no path linking them to either physics node).
        static SystemScheduler s_Scheduler = []
        {
            using namespace GameplayChannel;
            SystemScheduler sched;

            // Scripts move entities arbitrarily (transforms, velocities): model as
            // a writer of the local transforms every downstream mover/reader sees.
            sched.AddSystem("Scripts", [](Scene& s, Timestep ts)
                            { s.UpdateScripts(ts); })
                .Writes(kLocalTransforms);

            // Cinematics overwrite authored transforms AFTER scripts so a playing
            // cutscene wins for the frame (write-after-write on LocalTransforms).
            sched.AddSystem("Cinematics", [](Scene& s, Timestep ts)
                            { s.UpdateCinematics(ts); })
                .Writes(kLocalTransforms);

            // Animation samples skeleton pose + morph weights; it READS the posed
            // local transforms (read-after-write orders it after cinematics) and
            // writes the morph weights MorphEval consumes. It does NOT write
            // entity transforms (bone pose lives in skeleton/component state).
            sched.AddSystem("Animation", [](Scene& s, Timestep ts)
                            { s.UpdateAnimation(ts); })
                .Reads(kLocalTransforms)
                .Writes(kMorphWeights);

            sched.AddSystem("AnimationGraph", [](Scene& s, Timestep ts)
                            { s.UpdateAnimationGraphs(ts); })
                .Reads(kLocalTransforms)
                .Writes(kMorphWeights);

            // Morph deformation runs after BOTH morph-weight writers (read-after-
            // write on MorphWeights).
            sched.AddSystem("MorphEval", [](Scene& s, Timestep)
                            { s.EvaluateMorphTargets(); })
                .Reads(kMorphWeights);

            // Kick the physics step: buoyancy force queueing + contact-event
            // drain + the ECS-reading character/vehicle phase run here on the
            // game thread (hence Reads(LocalTransforms)), then the ECS-free
            // world update (Box2D + Jolt) launches as an engine task —
            // published as the PhysicsInFlight channel the fence consumes.
            sched.AddSystem("PhysicsKick", [](Scene& s, Timestep ts)
                            { s.KickPhysicsStep(ts); })
                .Reads(kLocalTransforms)
                .Writes(kPhysicsInFlight);

            // ── The physics shadow ─────────────────────────────────────────────
            // These run on the GAME THREAD while the world step runs on workers.
            // A shadow system must not touch transforms, physics state, or issue
            // Jolt queries (the broadphase is mid-update). Structural registry
            // changes and bus publishes are fine — the world step never touches
            // the ECS registry (verified: the ECS-reading phases were hoisted
            // into kick/fence).
            sched.AddSystem("Dialogue", [](Scene& s, Timestep ts)
                            { s.UpdateDialogue(ts); });
            sched.AddSystem("Quest", [](Scene& s, Timestep ts)
                            { s.UpdateQuest(ts); });

            // Fence the physics step: join the world-step task, run the joint-
            // break phase (ECS reads + event publish), and write body poses back
            // to the ECS transforms — the write declaration is what orders every
            // downstream transform consumer after the fence.
            sched.AddSystem("PhysicsFence", [](Scene& s, Timestep)
                            { s.FencePhysicsStep(); })
                .Reads(kPhysicsInFlight)
                .Writes(kLocalTransforms);

            // Compose world matrices once every local-transform mover has run;
            // publishes WorldTransforms for post-tick consumers (rendering, #499).
            sched.AddSystem("PropagateTransforms", [](Scene& s, Timestep)
                            { s.PropagateWorldTransforms(); })
                .Reads(kLocalTransforms)
                .Writes(kWorldTransforms);

            // Navigation reads agent positions AND writes them (crowd sync +
            // manual path following move TransformComponent.Translation). The
            // write is why it must stay out of the physics shadow, and it gives
            // SpatialIndex/Audio/Particles/Snow/Inventory their read-after-write
            // edges. Known consequence (historical behavior, now explicit): nav's
            // moves compose into world matrices on the NEXT tick's propagate.
            sched.AddSystem("Navigation", [](Scene& s, Timestep ts)
                            { s.UpdateNavigation(ts); })
                .ReadsWrites(kLocalTransforms);

            // Spatial index rebuilds from TransformComponent.Translation — the
            // read-after-write edge from Navigation (the last transform writer)
            // replaces the old explicit After("Navigation"), and publishes the
            // SpatialIndex channel Perception consumes.
            sched.AddSystem("SpatialIndex", [](Scene& s, Timestep)
                            { s.UpdateSpatialIndex(); })
                .Reads(kLocalTransforms)
                .Writes(kSpatialIndex);

            // Perception queries the spatial index and publishes sight results.
            // (Its line-of-sight raycasts also require the world step complete —
            // guaranteed transitively: SpatialIndex reads transforms written by
            // the fence.)
            sched.AddSystem("Perception", [](Scene& s, Timestep ts)
                            { s.UpdatePerception(ts); })
                .Reads(kSpatialIndex)
                .Writes(kPerception);

            // AI consumes THIS frame's perception results (the critical seam).
            sched.AddSystem("AI", [](Scene& s, Timestep ts)
                            { s.UpdateAI(ts); })
                .Reads(kPerception);

            // ── Parallelizable() markings — per-system thread-safety audit (issue
            // #453 parallel slice). The executor guarantees a marked system only
            // overlaps OTHER marked systems, so the audit question is exactly
            // "is this body thread-safe against the rest of the marked set?".
            // Do not flip one on without re-running that audit:
            //   Abilities  SAFE   — mutates only per-entity AbilityComponent data;
            //                       no bus, no RNG, no GL, no structural changes.
            //   Audio      SAFE   — miniaudio setters are internally atomic
            //                       (single-writer preserved), spatializer uses a
            //                       lock-free dirty flag; no RNG, no GL, no
            //                       structural changes. Its EnTT groups/storages
            //                       (and Abilities') are pre-created in the Scene
            //                       CONSTRUCTOR — worker-side view()/group()
            //                       first-touch would be a structural registry
            //                       mutation; extend that pre-warm list when
            //                       marking a new system.
            //   Inventory  UNSAFE — DestroyEntity (structural) + publishes to the
            //                       non-thread-safe GameplayEventBus. (It IS a
            //                       candidate for the physics shadow instead, but
            //                       it reads post-physics transforms for pickup
            //                       proximity — moving it would change what it
            //                       observes; the Reads(LocalTransforms) edge
            //                       below enforces its post-fence slot.)
            //   Particles  UNSAFE — per-component UseGPU path issues GL compute
            //                       from inside Update, and sub-emitters draw from
            //                       the thread_local RNG (worker stream is time-
            //                       seeded → breaks #452 determinism). CPU-only +
            //                       reseeded RNG is the follow-up candidate.
            //   Snow       UNSAFE — GL compute dispatch / SSBO upload
            //                       (SnowAccumulationSystem::SubmitDeformers,
            //                       SnowEjectaSystem::EmitAt) must stay on the GL
            //                       thread.
            // (Dialogue / Quest run in the physics shadow above — game thread, no
            // worker audit needed. Navigation / MorphEval are pinned main-thread:
            // TransformComponent writes / GL vertex-buffer upload.)
            sched.AddSystem("Inventory", [](Scene& s, Timestep ts)
                            { s.UpdateInventory(ts); })
                .Reads(kLocalTransforms);
            sched.AddSystem("Abilities", [](Scene& s, Timestep ts)
                            { s.UpdateAbilities(ts); })
                .Parallelizable();

            // Audio / particles / snow read entity translations/rotations (local
            // space — the historical behavior) and never move entities, so they
            // are mutually independent.
            sched.AddSystem("Audio", [](Scene& s, Timestep ts)
                            { s.UpdateAudio(ts); })
                .Reads(kLocalTransforms)
                .Parallelizable();
            sched.AddSystem("Particles", [](Scene& s, Timestep ts)
                            { s.UpdateParticles(ts); })
                .Reads(kLocalTransforms);
            sched.AddSystem("SnowDeformers", [](Scene& s, Timestep ts)
                            { s.UpdateSnowDeformers(ts); })
                .Reads(kLocalTransforms);

            sched.Build(); // derive order now; throws SystemSchedulerError if the graph is bad
            return sched;
        }();
        return s_Scheduler;
    }

    const std::vector<std::string>& Scene::GetGameplaySystemOrderForTesting()
    {
        return GetGameplayScheduler().GetOrderedNames();
    }

    void Scene::SimulateRuntimeStep(Timestep const ts)
    {
        ++m_SimulationTick;
        // Deterministic simulation clock: advances by exactly `ts` each tick, so
        // time-driven physics (buoyancy wave phase) is a function of the tick
        // sequence rather than the wall clock — reproducible across frame pacings
        // and rollback re-sim (issue #452). At a steady frame rate this tracks
        // wall-time, so wall-clock water visuals and floating bodies stay in sync.
        m_SimulationTime += static_cast<f32>(ts);

        // Run every gameplay system once, in an order DERIVED from the read/write
        // + before/after constraints each declares (issue #453) rather than from
        // the source order of these calls. The schedule is built and topologically
        // sorted once. The physics world step runs as an engine task between the
        // PhysicsKick and PhysicsFence nodes while the physics-shadow systems
        // execute on the game thread, and audited systems (Abilities, Audio) run
        // as worker tasks — see GetGameplayScheduler for the layout, the shadow
        // rules, and the audit table. Video playback is deliberately NOT a sim
        // system — it is a presentation concern advanced once per displayed frame
        // by RenderRuntime, frozen while paused (issue #452).
        GetGameplayScheduler().Execute(*this, ts);
    }

    void Scene::UpdateScripts(Timestep ts)
    {
        // Update scripts
        {
            // C# Entity OnUpdate
            for (auto scriptView = m_Registry.view<ScriptComponent>(); auto e : scriptView)
            {
                Entity entity = { e, this };
                ScriptEngine::OnUpdateEntity(entity, ts);
            }

            // Lua Entity OnUpdate
            for (auto luaView = m_Registry.view<LuaScriptComponent>(); auto e : luaView)
            {
                if (auto const& lsc = luaView.get<LuaScriptComponent>(e); !lsc.ScriptFile.empty())
                {
                    Entity entity = { e, this };
                    LuaScriptEngine::OnUpdateEntity(entity, ts);
                }
            }
        }
    }

    void Scene::UpdateCinematics(Timestep ts)
    {
        // Advance cinematic sequences. Runs after scripts so a playing
        // cutscene's authored transforms / camera win over script motion
        // for the frame, and before animation/physics so downstream
        // systems see the posed entities.
        CinematicSystem::Update(*this, ts);
    }

    void Scene::UpdateDialogue(Timestep ts)
    {
        // Update dialogue system
        if (m_DialogueSystem)
        {
            OLO_PROFILE_SCOPE("DialogueSystem::Update");
            m_DialogueSystem->Update(ts);
        }
    }

    void Scene::UpdateAnimation(Timestep ts)
    {
        // Update animations. Full-owning group over AnimationStateComponent +
        // SkeletonComponent (neither is shared with the physics/particle hot
        // loops, so both pools are owned — issue #443 ownership map).
        {
            auto animView = m_Registry.group<AnimationStateComponent, SkeletonComponent>();
            for (auto e : animView)
            {
                auto& animState = animView.get<AnimationStateComponent>(e);
                auto& skelComp = animView.get<SkeletonComponent>(e);

                if (animState.m_IsPlaying && animState.m_CurrentClip && skelComp.m_Skeleton)
                {
                    IKTargetComponent tempIk;
                    Entity entity = { e, this };
                    const IKTargetComponent* ikTarget = ResolveIKTargets(entity, tempIk) ? &tempIk : nullptr;
                    Animation::SpringBoneState* springState = nullptr;
                    const SpringBoneComponent* springBone = ResolveSpringBone(entity, springState);
                    Animation::NoiseAnimationState* noiseState = nullptr;
                    const NoiseAnimationComponent* noise = ResolveNoiseAnimation(entity, noiseState);
                    auto const& entityTransform = entity.GetComponent<TransformComponent>().GetTransform();
                    Animation::AnimationSystem::Update(animState, *skelComp.m_Skeleton, ts.GetSeconds(), ikTarget, entityTransform, springBone, springState, noise, noiseState);

                    // Sample morph target keyframes from the current animation clip
                    if (!animState.m_CurrentClip->MorphKeyframes.empty())
                    {
                        if (entity.HasComponent<MorphTargetComponent>())
                        {
                            auto& morphComp = entity.GetComponent<MorphTargetComponent>();
                            MorphTargetSystem::SampleMorphKeyframes(animState.m_CurrentClip, animState.m_CurrentTime, morphComp);
                        }
                    }
                }
            }

            // Handle morph-only animation (entities with AnimationState but no Skeleton)
            auto morphAnimView = m_Registry.view<AnimationStateComponent, MorphTargetComponent>(entt::exclude<SkeletonComponent>);
            for (auto e : morphAnimView)
            {
                auto& animState = morphAnimView.get<AnimationStateComponent>(e);
                if (!animState.m_IsPlaying || !animState.m_CurrentClip)
                    continue;

                // Advance time for morph-only entities
                animState.m_CurrentTime += ts.GetSeconds();
                if (const float duration = animState.m_CurrentClip->Duration; duration > 0.0f && animState.m_CurrentTime > duration)
                {
                    animState.m_CurrentTime -= static_cast<int>(animState.m_CurrentTime / duration) * duration;
                }

                if (!animState.m_CurrentClip->MorphKeyframes.empty())
                {
                    auto& morphComp = morphAnimView.get<MorphTargetComponent>(e);
                    MorphTargetSystem::SampleMorphKeyframes(animState.m_CurrentClip, animState.m_CurrentTime, morphComp);
                }
            }
        }
    }

    void Scene::UpdateAnimationGraphs(Timestep ts)
    {
        // Update animation graphs
        {
            OLO_PROFILE_SCOPE("Animation Graph Update");
            auto graphView = m_Registry.view<AnimationGraphComponent, SkeletonComponent>();
            for (auto e : graphView)
            {
                auto& graphComp = graphView.get<AnimationGraphComponent>(e);
                auto& skelComp = graphView.get<SkeletonComponent>(e);

                if (!skelComp.m_Skeleton)
                {
                    continue;
                }

                // Lazy-load RuntimeGraph from asset if not yet initialized
                if (!graphComp.RuntimeGraph && graphComp.AnimationGraphAssetHandle != 0)
                {
                    if (auto graphAsset = AssetManager::GetAsset<AnimationGraphAsset>(graphComp.AnimationGraphAssetHandle))
                    {
                        auto templateGraph = graphAsset->GetGraph();
                        if (templateGraph)
                        {
                            graphComp.RuntimeGraph = templateGraph->Clone();

                            // Backfill missing parameters from graph defaults without clobbering existing values
                            if (graphComp.Parameters.GetAll().empty())
                            {
                                graphComp.Parameters = graphComp.RuntimeGraph->Parameters;
                            }
                            else
                            {
                                for (auto const& [name, param] : graphComp.RuntimeGraph->Parameters.GetAll())
                                {
                                    if (!graphComp.Parameters.HasParameter(name))
                                    {
                                        switch (param.ParamType)
                                        {
                                            case AnimationParameterType::Float:
                                                graphComp.Parameters.DefineFloat(name, param.FloatValue);
                                                break;
                                            case AnimationParameterType::Int:
                                                graphComp.Parameters.DefineInt(name, param.IntValue);
                                                break;
                                            case AnimationParameterType::Bool:
                                                graphComp.Parameters.DefineBool(name, param.BoolValue);
                                                break;
                                            case AnimationParameterType::Trigger:
                                                graphComp.Parameters.DefineTrigger(name);
                                                break;
                                        }
                                    }
                                }
                            }
                            graphComp.RuntimeGraph->Start();

                            // Resolve clip name references to actual clip pointers from the entity's model
                            Entity entity = { e, this };
                            if (entity.HasComponent<AnimationStateComponent>())
                            {
                                auto const& animState = entity.GetComponent<AnimationStateComponent>();
                                if (!animState.m_AvailableClips.empty())
                                {
                                    graphComp.RuntimeGraph->ResolveClips(animState.m_AvailableClips);
                                }
                            }
                        }
                    }
                }

                if (graphComp.RuntimeGraph)
                {
                    // Retry clip resolution if clips became available after graph init
                    if (graphComp.RuntimeGraph->HasUnresolvedClips())
                    {
                        Entity entity = { e, this };
                        if (entity.HasComponent<AnimationStateComponent>())
                        {
                            auto const& animState = entity.GetComponent<AnimationStateComponent>();
                            if (!animState.m_AvailableClips.empty())
                            {
                                graphComp.RuntimeGraph->ResolveClips(animState.m_AvailableClips);
                            }
                        }
                    }

                    // Ensure the graph has been started
                    for (auto& layer : graphComp.RuntimeGraph->Layers)
                    {
                        if (layer.StateMachine && !layer.StateMachine->HasStarted())
                        {
                            graphComp.RuntimeGraph->Start();
                            break;
                        }
                    }
                    IKTargetComponent graphTempIk;
                    Entity graphEntity = { e, this };
                    const IKTargetComponent* graphIkTarget = ResolveIKTargets(graphEntity, graphTempIk) ? &graphTempIk : nullptr;
                    Animation::SpringBoneState* graphSpringState = nullptr;
                    const SpringBoneComponent* graphSpringBone = ResolveSpringBone(graphEntity, graphSpringState);
                    Animation::NoiseAnimationState* graphNoiseState = nullptr;
                    const NoiseAnimationComponent* graphNoise = ResolveNoiseAnimation(graphEntity, graphNoiseState);
                    auto const& graphEntityTransform = graphEntity.GetComponent<TransformComponent>().GetTransform();
                    MorphTargetComponent* graphMorph = graphEntity.HasComponent<MorphTargetComponent>()
                                                           ? &graphEntity.GetComponent<MorphTargetComponent>()
                                                           : nullptr;
                    Animation::AnimationGraphSystem::Update(graphComp, *skelComp.m_Skeleton, ts.GetSeconds(), graphIkTarget, graphEntityTransform, graphSpringBone, graphSpringState, graphNoise, graphNoiseState, graphMorph);
                }
            }
        }
    }

    void Scene::EvaluateMorphTargets()
    {
        // Evaluate morph targets for all entities with active weights.
        // Runs after BOTH the AnimationStateComponent and animation-graph
        // updates so morph weights sampled from either path this frame are
        // deformed this same frame (no one-frame lag). Morph deformation
        // happens before skeletal skinning (morph first, then skin).
        {
            OLO_PROFILE_SCOPE("Morph Target Evaluation");
            auto morphView = m_Registry.view<MorphTargetComponent, MeshComponent>();
            for (auto e : morphView)
            {
                EvaluateEntityMorphTargets(morphView.get<MorphTargetComponent>(e), morphView.get<MeshComponent>(e));
            }
        }
    }

    void Scene::UpdateNavigation(Timestep ts)
    {
        // Update navigation / pathfinding
        NavigationSystem::OnUpdate(this, ts.GetSeconds());
    }

    void Scene::UpdatePerception(Timestep ts)
    {
        // Refresh AI sight perception before AI decisions so behavior trees /
        // FSMs / GOAP see fresh sensor data the same frame. Uses the spatial
        // index above for an O(local) proximity query instead of an O(n)
        // scan over every perceptible entity.
        PerceptionSystem::OnUpdate(this, ts.GetSeconds());
    }

    void Scene::UpdateAI(Timestep ts)
    {
        // Update AI (behavior trees and state machines)
        AISystem::OnUpdate(this, ts.GetSeconds());
    }

    void Scene::UpdateInventory(Timestep ts)
    {
        // Update inventory system (pickups, despawn)
        InventorySystem::OnUpdate(this, ts.GetSeconds());
    }

    void Scene::UpdateQuest(Timestep ts)
    {
        // Update quest system (timers, conditions)
        QuestSystem::OnUpdate(this, ts.GetSeconds());
    }

    void Scene::UpdateAbilities(Timestep ts)
    {
        // Update gameplay ability system (abilities, effects, cooldowns)
        GameplayAbilitySystem::OnUpdate(this, ts.GetSeconds());
    }

    void Scene::UpdateAudio(Timestep ts)
    {
        auto listenerView = m_Registry.group<AudioListenerComponent>(entt::get<TransformComponent>);
        for (auto&& [e, ac, tc] : listenerView.each())
        {
            if (ac.Active)
            {
                const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
                const glm::vec3 forward = SafeAudioBasis(glm::vec3(inverted[2]), glm::vec3(0.0f, 0.0f, 1.0f));
                ac.Listener->SetPosition(tc.Translation);
                ac.Listener->SetDirection(-forward);
                // Keep the 3D spatializer's listener in sync each frame so SoundGraph voices
                // pan/attenuate relative to the live listener pose (issue #424).
                if (auto* spatializer = AudioEngine::GetSpatializer())
                {
                    Audio::Transform listenerTransform;
                    listenerTransform.Position = tc.Translation;
                    listenerTransform.Orientation = -forward;
                    listenerTransform.Up = SafeAudioBasis(glm::vec3(inverted[1]), glm::vec3(0.0f, 1.0f, 0.0f));
                    spatializer->UpdateListener(listenerTransform);
                }
                break;
            }
        }

        auto sourceView = m_Registry.group<AudioSourceComponent>(entt::get<TransformComponent>);
        for (auto&& [e, ac, tc] : sourceView.each())
        {
            if (ac.Source)
            {
                Entity entity = { e, this };
                const glm::mat4 inverted = glm::inverse(entity.GetLocalTransform());
                const glm::vec3 forward = SafeAudioBasis(glm::vec3(inverted[2]), glm::vec3(0.0f, 0.0f, 1.0f));
                ac.Source->SetPosition(tc.Translation);
                ac.Source->SetDirection(forward);
            }
        }

        // Drive 3D position/orientation for SoundGraph voices (issue #424). Mirrors the
        // AudioSource loop above; SetLocation/SetOrientation route into the per-voice
        // spatializer node, which no-ops until the voice actually hosts one.
        for (auto sgView = m_Registry.view<AudioSoundGraphComponent, TransformComponent>(); auto&& [e, sgc, tc] : sgView.each())
        {
            if (sgc.Sound)
            {
                const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
                const glm::vec3 forward = SafeAudioBasis(glm::vec3(inverted[2]), glm::vec3(0.0f, 0.0f, 1.0f));
                const glm::vec3 up = SafeAudioBasis(glm::vec3(inverted[1]), glm::vec3(0.0f, 1.0f, 0.0f));
                sgc.Sound->SetLocation(tc.Translation);
                sgc.Sound->SetOrientation(forward, up);
            }
        }

        // Process audio events queue
        if (m_AudioEventsManager)
        {
            m_AudioEventsManager->Update(ts);
        }
    }

    void Scene::UpdateParticles(Timestep ts)
    {
        // Update particle systems
        {
            // Quick camera position lookup for LOD
            glm::vec3 camPos(0.0f);
            for (const auto camView = m_Registry.view<TransformComponent, CameraComponent>(); const auto camEntity : camView)
            {
                const auto& [camTransform, cam] = camView.get<TransformComponent, CameraComponent>(camEntity);
                if (cam.Primary)
                {
                    camPos = camTransform.Translation;
                    break;
                }
            }

            // Partial-owning group: owns ParticleSystemComponent, borrows
            // TransformComponent (issue #443 ownership map).
            for (auto psGroup = m_Registry.group<ParticleSystemComponent>(entt::get<TransformComponent>); auto entity : psGroup)
            {
                const auto& transform = psGroup.get<TransformComponent>(entity);
                auto& psc = psGroup.get<ParticleSystemComponent>(entity);
                // Provide Jolt scene for raycast collision
                psc.System.SetJoltScene(m_JoltScene.get());
                psc.System.UpdateLOD(camPos, transform.Translation);

                // Compute parent velocity for velocity inheritance
                glm::vec3 parentVelocity(0.0f);
                if (std::abs(psc.System.VelocityInheritance) > 1e-6f && ts > 0.0f)
                {
                    parentVelocity = (transform.Translation - psc.System.GetEmitterPosition()) / static_cast<f32>(ts);
                }

                psc.System.Update(ts, transform.Translation, parentVelocity, transform.GetRotation());

                // Process sub-emitter triggers for child systems
                ProcessChildSubEmitters(psc, ts, transform.Translation);
            }
        }
    }

    void Scene::UpdateSnowDeformers(Timestep ts)
    {
        // Process snow deformer entities — submit deformation stamps and emit ejecta
        ProcessSnowDeformers(ts, m_RuntimeSnowPrevPositions);
    }

    void Scene::RenderRuntime(Timestep const ts)
    {
        // Advance video playback once per displayed frame at the display rate
        // (frozen while paused). This is a presentation concern moved out of the
        // fixed-step sim body so it runs exactly once per frame instead of 0..N
        // times — no high-fps stutter, no wasted decode/upload during catch-up
        // (issue #452). It does not affect simulation state, so display-rate
        // timing here does not break determinism.
        if (!m_IsPaused)
        {
            VideoSystem::OnUpdate(this, static_cast<f32>(ts));
        }

        // Find the primary camera
        Camera const* mainCamera = nullptr;
        glm::mat4 cameraTransform;
        // Remember the primary camera entity + whether it's the live fly-cam so
        // the render-interpolation pass (below) can blend a gameplay camera's
        // view but leave the display-rate fly-cam untouched (issue #502).
        entt::entity primaryCameraEntity = entt::null;
        bool primaryCameraIsFlyCam = false;
        {
            for (const auto view = m_Registry.view<TransformComponent, CameraComponent>(); const auto entity : view)
            {
                auto& transform = view.get<TransformComponent>(entity);
                const auto& camera = view.get<CameraComponent>(entity);

                if (camera.Primary)
                {
                    // FPS fly-camera controls: WASD/QE movement + mouse look.
                    // This is an opt-in debug/free-fly camera driven by live input
                    // and the variable frame delta (`ts`), so it runs here in the
                    // display-rate render stage, NOT the fixed-step sim — it is a
                    // viewing aid, deliberately outside the deterministic state the
                    // gameplay simulation reproduces (issue #452). Gameplay cameras
                    // moved by scripts/physics live in SimulateRuntimeStep and stay
                    // frame-rate-independent.
                    if (camera.RuntimeControl)
                    {
                        const glm::vec2 mouse{ Input::GetMouseX(), Input::GetMouseY() };
                        const glm::vec2 delta = (mouse - m_RuntimeCameraLastMouse) * 0.003f;
                        m_RuntimeCameraLastMouse = mouse;

                        // Mouse look — pitch (X rotation) and yaw (Y rotation)
                        if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
                        {
                            glm::vec3 euler = transform.GetRotationEuler();
                            euler.y -= delta.x * 0.8f;
                            euler.x -= delta.y * 0.8f;
                            euler.x = glm::clamp(euler.x, glm::radians(-89.0f), glm::radians(89.0f));
                            transform.SetRotationEuler(euler);
                        }

                        // WASD + QE movement (always active)
                        const glm::quat orientation = transform.GetRotation();
                        const glm::vec3 forward = glm::rotate(orientation, glm::vec3(0.0f, 0.0f, -1.0f));
                        const glm::vec3 right = glm::rotate(orientation, glm::vec3(1.0f, 0.0f, 0.0f));

                        f32 speed = camera.FlySpeed * ts;
                        if (Input::IsKeyPressed(Key::LeftShift))
                        {
                            speed *= 3.0f;
                        }

                        if (Input::IsKeyPressed(Key::W))
                        {
                            transform.Translation += forward * speed;
                        }
                        if (Input::IsKeyPressed(Key::S))
                        {
                            transform.Translation -= forward * speed;
                        }
                        if (Input::IsKeyPressed(Key::A))
                        {
                            transform.Translation -= right * speed;
                        }
                        if (Input::IsKeyPressed(Key::D))
                        {
                            transform.Translation += right * speed;
                        }
                        if (Input::IsKeyPressed(Key::E))
                        {
                            transform.Translation.y += speed;
                        }
                        if (Input::IsKeyPressed(Key::Q))
                        {
                            transform.Translation.y -= speed;
                        }
                    }

                    mainCamera = &camera.Camera;
                    cameraTransform = transform.GetTransform();
                    primaryCameraEntity = entity;
                    primaryCameraIsFlyCam = camera.RuntimeControl;
                    break;
                }
            }
        }

        // ── Render interpolation (issue #502) ───────────────────────────────
        // Overwrite every entity's live local transform with the pose blended
        // between the last two fixed-tick snapshots (alpha = accumulator /
        // fixedStep), recompose world matrices, render, then restore the
        // authoritative poses at the end of the frame. This makes EVERY render
        // read — GetWorldTransform (world matrices) AND the direct
        // TransformComponent::GetTransform() reads in RenderScene3D — see the
        // interpolated pose without threading alpha through every draw call. The
        // simulation state itself is untouched (restored before this function
        // returns), so determinism (#484) is preserved and any post-render
        // consumer this frame (SaveGameManager::Tick, editor gizmos) still reads
        // the exact fixed-tick pose.
        //
        // The primary camera is excluded from the overwrite: its live local may
        // have diverged from the snapshot this frame (the fly-cam mutates it at
        // display rate above), so restoring from the snapshot would erase that
        // movement. A gameplay (non-fly) camera is instead blended directly into
        // cameraTransform so the VIEW interpolates without an ECS round-trip.
        const bool interpolateThisFrame = ShouldInterpolateThisFrame();
        // Full-component copies (not just TRS): TransformComponent's Rotation /
        // RotationEuler are private and SetRotation() re-derives the Euler
        // representation, so restoring via the setter could drift RotationEuler
        // even when the quat is bit-identical. Copying the whole component and
        // assigning it back restores every field — incl. the private Euler and
        // the matrix cache — exactly, guaranteeing zero simulation-state drift.
        std::vector<std::pair<entt::entity, TransformComponent>> restorePoses;
        if (interpolateThisFrame)
        {
            if (mainCamera && !primaryCameraIsFlyCam)
            {
                cameraTransform = GetInterpolatedLocalTransform(primaryCameraEntity);
            }

            for (const auto view = m_Registry.view<TransformComponent>(); const auto entity : view)
            {
                if (entity == primaryCameraEntity)
                {
                    continue;
                }
                InterpTransform interp;
                if (!ComputeInterpolatedLocal(entity, interp))
                {
                    continue;
                }
                auto& tc = view.get<TransformComponent>(entity);
                // Save the authoritative pose before overwriting so it can be
                // restored verbatim after the draw.
                restorePoses.emplace_back(entity, tc);
                tc.Translation = interp.Translation;
                tc.Scale = interp.Scale;
                tc.SetRotation(interp.Rotation);
            }

            // Recompose parent-chain world matrices from the interpolated locals
            // so GetWorldTransform() reads see the blended pose.
            if (!restorePoses.empty())
            {
                PropagateWorldTransforms();
            }
        }

        // Update camera VP matrix before resolving UI layout so world-anchor
        // projections use the current frame's camera, not the previous one.
        if (mainCamera)
        {
            m_CameraViewProjection = mainCamera->GetProjection() * glm::inverse(cameraTransform);
        }
        else
        {
            // No camera — reset to identity so world-anchored UI doesn't use stale matrices
            m_CameraViewProjection = glm::mat4(1.0f);
        }

        // Process UI input during runtime (resolve layout first so hit-rects are current)
        m_UILayoutResolvedThisFrame = false;
        if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
        {
            UILayoutSystem::ResolveLayout(*this, m_ViewportWidth, m_ViewportHeight, m_CameraViewProjection);
            m_UILayoutResolvedThisFrame = true;

            const glm::vec2 mousePos = Input::GetMousePosition();
            const bool mouseDown = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
            const bool mousePressed = mouseDown && !m_PreviousMouseButtonDown;
            m_PreviousMouseButtonDown = mouseDown;

            // Gather keyboard input for focused UI text fields: typed characters
            // come from the Input char buffer; edit keys are edge-triggered.
            UIKeyboardInput keyboard;
            keyboard.m_TypedCharacters = Input::GetTypedCharacters();
            keyboard.m_Backspace = Input::IsKeyJustPressed(Key::Backspace);
            keyboard.m_Delete = Input::IsKeyJustPressed(Key::Delete);
            keyboard.m_CursorLeft = Input::IsKeyJustPressed(Key::Left);
            keyboard.m_CursorRight = Input::IsKeyJustPressed(Key::Right);
            keyboard.m_Home = Input::IsKeyJustPressed(Key::Home);
            keyboard.m_End = Input::IsKeyJustPressed(Key::End);
            UIInputSystem::ProcessInput(*this, mousePos, mouseDown, mousePressed, 0.0f, 0.0f, keyboard);

            // Gamepad / keyboard focus navigation + widget event delegates. Runs
            // after ProcessInput so mouse-driven state changes are visible to the
            // change-detection pass, and reads the active input context's menu
            // actions (live only while InputContextType::Menu is active).
            UINavigationSystem::Update(*this, UINavigationSystem::PollActions());
        }

        if (mainCamera && m_RenderingEnabled)
        {
            if (m_Is3DModeEnabled)
            {
                // Set up the UICompositePass callback before the render graph executes.
                // The callback runs during UICompositePass::Execute(), after the
                // post-processed scene has been blitted as background.
                Renderer3D::SetUICompositeRenderCallback([this, mainCamera, cameraTransform]()
                                                         {
                                                             // World-space 2D overlays (sprites, circles, text)
                                                             Renderer2D::BeginScene(*mainCamera, cameraTransform);

                                                             for (const auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>); const auto entity : group)
                                                             {
                                                                 const auto& sprite = group.get<SpriteRendererComponent>(entity);
                                                                 Renderer2D::DrawSprite(GetWorldTransform(entity), sprite, static_cast<int>(std::to_underlying(entity)));
                                                             }

                                                             for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
                                                             {
                                                                 const auto& circle = view.get<CircleRendererComponent>(entity);
                                                                 Renderer2D::DrawCircle(GetWorldTransform(entity), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(std::to_underlying(entity)));
                                                             }

                                                             for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
                                                             {
                                                                 const auto& text = view.get<TextComponent>(entity);
                                                                 DrawTextWithShadow(text, GetWorldTransform(entity), static_cast<int>(std::to_underlying(entity)));
                                                             }

                                                             Renderer2D::EndScene();

                                                             // Screen-space UI overlay
                                                             RenderUIOverlay(); });

                RenderScene3D(*mainCamera, cameraTransform);
            }
            else
            {
                // 2D-only frame: the 3D render path is skipped, so the
                // animation-time cache wasn't advanced. If the next frame
                // toggles back into 3D, `prevAnimationTime` would be the
                // stale timestamp from the last 3D frame — producing a
                // spurious per-fragment velocity spike on water/foliage/
                // wind shaders (dt == however long we spent in 2D). Reset
                // the sentinel so 3D resume re-seeds `prevAnimationTime ==
                // animationTime` (zero rigid motion for that first frame).
                m_LastAnimationTime = -1.0f;

                // 2D mode - render directly (no render graph)
                Renderer2D::BeginScene(*mainCamera, cameraTransform);

                for (const auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>); const auto entity : group)
                {
                    const auto& sprite = group.get<SpriteRendererComponent>(entity);
                    Renderer2D::DrawSprite(GetWorldTransform(entity), sprite, static_cast<int>(std::to_underlying(entity)));
                }

                for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
                {
                    const auto& circle = view.get<CircleRendererComponent>(entity);
                    Renderer2D::DrawCircle(GetWorldTransform(entity), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(std::to_underlying(entity)));
                }

                for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
                {
                    const auto& text = view.get<TextComponent>(entity);
                    DrawTextWithShadow(text, GetWorldTransform(entity), static_cast<int>(std::to_underlying(entity)));
                }

                // 2D particles (3D particles are rendered by ParticleRenderPass)
                for (auto view = m_Registry.view<ParticleSystemComponent>(); auto entity : view)
                {
                    auto& psc = view.get<ParticleSystemComponent>(entity);
                    auto& sys = psc.System;
                    glm::vec3 offset = (sys.SimulationSpace == ParticleSpace::Local) ? sys.GetEmitterPosition() : glm::vec3(0.0f);

                    const ModuleTextureSheetAnimation* sheet = sys.TextureSheetModule.Enabled ? &sys.TextureSheetModule : nullptr;

                    SetParticleBlendMode(sys.BlendMode);
                    ParticleRenderer::RenderParticles2D(sys.GetPool(), psc.Texture, offset, static_cast<int>(std::to_underlying(entity)), nullptr, sheet);

                    for (sizet c = 0; c < psc.ChildSystems.size(); ++c)
                    {
                        auto& childSys = psc.ChildSystems[c];
                        SetParticleBlendMode(childSys.BlendMode);
                        Ref<Texture2D> childTex = (c < psc.ChildTextures.size()) ? psc.ChildTextures[c] : nullptr;
                        ParticleRenderer::RenderParticles2D(childSys.GetPool(), childTex, offset, static_cast<int>(std::to_underlying(entity)), nullptr, nullptr);
                    }

                    RestoreDefaultBlendMode();
                }

                Renderer2D::EndScene();

                RenderUIOverlay();
            }
        }
        else
        {
            // No primary camera or rendering disabled: the 3D render path
            // is skipped entirely, so the animation-time cache wasn't
            // advanced. Mirror the 2D-only branch's reset so whenever 3D
            // resumes (camera re-acquired or rendering re-enabled) the
            // next frame re-seeds `prevAnimationTime == animationTime`
            // instead of using whatever timestamp was last recorded.
            m_LastAnimationTime = -1.0f;
        }

        // Restore the authoritative fixed-tick poses overwritten for the
        // interpolated draw above, then recompose world matrices so any
        // post-render reader this frame sees the exact simulation state (#502).
        if (!restorePoses.empty())
        {
            for (const auto& [entity, pose] : restorePoses)
            {
                if (auto* tc = m_Registry.try_get<TransformComponent>(entity))
                {
                    *tc = pose;
                }
            }
            PropagateWorldTransforms();
        }
    }

    void Scene::OnUpdateSimulation(const Timestep ts, EditorCamera const& camera)
    {
        PerformanceProfiler* perfProfiler = nullptr;
        if (auto* app = Application::TryGet())
        {
            perfProfiler = app->GetPerformanceProfiler();
        }
        OLO_PERF_SCOPE("Scene::OnUpdateSimulation", perfProfiler);
        if (!m_IsPaused || m_StepFrames-- > 0)
        {
            // Advance the deterministic simulation clock so the buoyancy wave
            // phase below is tick-driven (see SimulateRuntimeStep).
            m_SimulationTime += static_cast<f32>(ts);

            // Physics: 2D + 3D step and transform sync (shared with the other
            // runtime/simulate tick — see Scene::StepPhysics).
            StepPhysics(ts);
        }

        // Compose parent-chain world matrices before rendering, unconditionally
        // (even while paused) so gizmo edits made in play-in-editor "simulate"
        // mode still show composed parent transforms this frame (#499).
        PropagateWorldTransforms();

        // Render based on mode
        if (m_RenderingEnabled)
        {
            if (m_Is3DModeEnabled)
            {
                RenderScene3D(camera);
            }
            else
            {
                // 2D-only simulation frame: reset so resuming 3D doesn't emit
                // a spurious velocity spike from the stale PrevAnimationTime.
                m_LastAnimationTime = -1.0f;

                RenderScene(camera);
                RenderUIOverlay();
            }
        }
        else
        {
            // Rendering disabled: skip both 3D and 2D paths. Reset the
            // animation-time sentinel so resuming rendering re-seeds
            // `prevAnimationTime == animationTime` on the first 3D frame.
            m_LastAnimationTime = -1.0f;
        }
    }

    void Scene::OnUpdateEditor([[maybe_unused]] Timestep const ts, EditorCamera const& camera)
    {
        PerformanceProfiler* perfProfiler = nullptr;
        if (auto* app = Application::TryGet())
        {
            perfProfiler = app->GetPerformanceProfiler();
        }
        OLO_PERF_SCOPE("Scene::OnUpdateEditor", perfProfiler);
        // Refresh LocalizedTextComponent → TextComponent.TextString so the
        // editor reflects locale changes in real time.
        LocalizationSystem::UpdateLocalizedText(*this);

        // Compose parent-chain world matrices early — editor-preview mode has
        // no physics step, and gizmo/inspector edits to local transforms need
        // to show composed parent transforms in this same frame's render (#499).
        PropagateWorldTransforms();

        // Scene streaming update (editor preview)
        if (m_SceneStreamer)
        {
            ++m_StreamingFrameCounter;
            m_SceneStreamer->Update(camera.GetPosition(), m_StreamingFrameCounter);
        }

        // Update particle systems so they preview in the editor
        {
            const glm::vec3 camPos = camera.GetPosition();
            // Reuses the ParticleSystemComponent owning group (issue #443).
            for (auto psGroup = m_Registry.group<ParticleSystemComponent>(entt::get<TransformComponent>); auto entity : psGroup)
            {
                const auto& transform = psGroup.get<TransformComponent>(entity);
                auto& psc = psGroup.get<ParticleSystemComponent>(entity);
                psc.System.UpdateLOD(camPos, transform.Translation);
                psc.System.Update(ts, transform.Translation, glm::vec3(0.0f), transform.GetRotation());

                // Process sub-emitter triggers for child systems
                ProcessChildSubEmitters(psc, ts, transform.Translation);
            }
        }

        // Process snow deformer entities in editor preview
        ProcessSnowDeformers(ts, m_EditorSnowPrevPositions);

        // Update animations so they preview in the editor (IK responds to target movement).
        // Reuses the AnimationStateComponent + SkeletonComponent owning group (issue #443).
        {
            auto animView = m_Registry.group<AnimationStateComponent, SkeletonComponent>();
            for (auto e : animView)
            {
                auto& animState = animView.get<AnimationStateComponent>(e);
                auto& skelComp = animView.get<SkeletonComponent>(e);

                if (animState.m_IsPlaying && animState.m_CurrentClip && skelComp.m_Skeleton)
                {
                    IKTargetComponent tempIk;
                    Entity entity = { e, this };
                    const IKTargetComponent* ikTarget = ResolveIKTargets(entity, tempIk) ? &tempIk : nullptr;
                    Animation::SpringBoneState* springState = nullptr;
                    const SpringBoneComponent* springBone = ResolveSpringBone(entity, springState);
                    Animation::NoiseAnimationState* noiseState = nullptr;
                    const NoiseAnimationComponent* noise = ResolveNoiseAnimation(entity, noiseState);
                    auto const& entityTransform = entity.GetComponent<TransformComponent>().GetTransform();
                    Animation::AnimationSystem::Update(animState, *skelComp.m_Skeleton, ts.GetSeconds(), ikTarget, entityTransform, springBone, springState, noise, noiseState);

                    // Sample morph target keyframes from the current animation clip
                    if (!animState.m_CurrentClip->MorphKeyframes.empty())
                    {
                        if (entity.HasComponent<MorphTargetComponent>())
                        {
                            auto& morphComp = entity.GetComponent<MorphTargetComponent>();
                            MorphTargetSystem::SampleMorphKeyframes(animState.m_CurrentClip, animState.m_CurrentTime, morphComp);
                        }
                    }
                }
            }
        }

        // Evaluate morph targets for editor preview (mirrors the runtime morph evaluation pass)
        {
            OLO_PROFILE_SCOPE("Editor Morph Target Evaluation");
            auto morphView = m_Registry.view<MorphTargetComponent, MeshComponent>();
            for (auto e : morphView)
            {
                EvaluateEntityMorphTargets(morphView.get<MorphTargetComponent>(e), morphView.get<MeshComponent>(e));
            }
        }

        // Render based on mode
        if (m_RenderingEnabled)
        {
            // Cache editor camera VP for UI world-anchor projection (nameplates etc.)
            m_CameraViewProjection = camera.GetViewProjection();

            if (m_Is3DModeEnabled)
            {
                // Set up the UICompositePass callback for editor overlays.
                // Renders text and UI into the UICompositePass FBO so the editor
                // viewport (which reads UICompositePass output) shows them.
                Renderer3D::SetUICompositeRenderCallback([this, &camera]()
                                                         {
                                                             Renderer2D::BeginScene(camera);

                                                             for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
                                                             {
                                                                 const auto& text = view.get<TextComponent>(entity);
                                                                 DrawTextWithShadow(text, GetWorldTransform(entity), static_cast<int>(std::to_underlying(entity)));
                                                             }

                                                             Renderer2D::EndScene();

                                                             RenderUIOverlay(); });

                RenderScene3D(camera);
            }
            else
            {
                // 2D-only editor frame: same reset as the runtime branch so
                // toggling between 2D and 3D modes doesn't leak a stale
                // `prevAnimationTime` into water/foliage/wind on resume.
                m_LastAnimationTime = -1.0f;

                RenderScene(camera);
                RenderUIOverlay();
            }
        }
        else
        {
            // Rendering disabled in editor: skip 3D and 2D. Reset the
            // animation-time sentinel so toggling rendering back on
            // re-seeds `prevAnimationTime == animationTime`.
            m_LastAnimationTime = -1.0f;
        }
    }

    void Scene::InitializeEditorStreamer()
    {
        OLO_PROFILE_FUNCTION();

        if (m_SceneStreamer)
        {
            return; // Already running
        }

        m_SceneStreamer = std::make_unique<SceneStreamer>();
        SceneStreamerConfig config;
        config.LoadRadius = m_StreamingSettings.DefaultLoadRadius;
        config.UnloadRadius = m_StreamingSettings.DefaultUnloadRadius;
        config.MaxLoadedRegions = m_StreamingSettings.MaxLoadedRegions;
        config.RegionDirectory = m_StreamingSettings.RegionDirectory;
        m_SceneStreamer->Initialize(this, config);
    }

    void Scene::ShutdownEditorStreamer()
    {
        OLO_PROFILE_FUNCTION();

        if (m_SceneStreamer)
        {
            m_SceneStreamer->Shutdown();
            m_SceneStreamer.reset();
        }
    }

    void Scene::OnViewportResize(const u32 width, const u32 height)
    {
        if ((m_ViewportWidth == width) && (m_ViewportHeight == height))
        {
            return;
        }

        m_ViewportWidth = width;
        m_ViewportHeight = height;

        // Resize our non-FixedAspectRatio cameras
        for (const auto view = m_Registry.view<CameraComponent>(); auto const entity : view)
        {
            auto& cameraComponent = view.get<CameraComponent>(entity);
            if (!cameraComponent.FixedAspectRatio)
            {
                cameraComponent.Camera.SetViewportSize(width, height);
            }
        }
    }

    void Scene::Step(int frames)
    {
        m_StepFrames = frames;
    }

    [[nodiscard]] Entity Scene::DuplicateEntity(Entity entity)
    {
        Entity newEntity = CreateEntity(entity.GetName());

        CopyComponentIfExists(AllComponents{}, newEntity, entity);

        // Fix up special-case components after bulk copy
        // RelationshipComponent: clear parent/children to avoid sharing hierarchy links
        if (newEntity.HasComponent<RelationshipComponent>())
        {
            auto& rel = newEntity.GetComponent<RelationshipComponent>();
            rel.m_ParentHandle = UUID(0);
            rel.m_Children.clear();
        }

        // CameraComponent: force Primary = false to avoid multiple primaries
        if (newEntity.HasComponent<CameraComponent>())
        {
            newEntity.GetComponent<CameraComponent>().Primary = false;
        }

        return newEntity;
    }

    void Scene::SetName(std::string_view name)
    {
        m_Name = name;
    }

    [[nodiscard]] Entity Scene::GetPrimaryCameraEntity()
    {
        for (const auto view = m_Registry.view<CameraComponent>(); auto const entity : view)
        {
            if (const auto& camera = view.get<CameraComponent>(entity); camera.Primary)
            {
                return Entity{ entity, this };
            }
        }
        return {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Scene::OnComponentAdded / OnComponentRemoved explicit specializations.
    //
    // The primary templates (Scene.h) are declaration-only, so every component
    // that supports Add/RemoveComponent<T>() needs an explicit specialization in
    // this TU — a missing one is a link error (engine for add, OloEditor for
    // remove). The overwhelming majority are pure no-ops; OloHeaderTool generates
    // those from the `struct *Component` scan (minus the custom-handler exclusion
    // sets kComponentsCustomOnAdd / kComponentsCustomOnRemove) into
    // Scene/Generated/OnComponent{Added,Removed}.Generated.inl, #include'd here and
    // (for the remove list) further below. Only the specializations with real
    // init/teardown bodies are hand-written. A component given a real body here
    // MUST be in the matching exclusion set, or its generated no-op collides with
    // it (duplicate-definition build error). ComponentHandlerCoverageTest guards
    // the generated lists against drift.
    // ─────────────────────────────────────────────────────────────────────────

    // Generated no-op OnComponentAdded specializations (see header above).
#define OLO_ON_COMPONENT_ADDED_NOOP(T) \
    template<>                         \
    void Scene::OnComponentAdded<T>(Entity, T&) {}
#include "OloEngine/Scene/Generated/OnComponentAdded.Generated.inl"
#undef OLO_ON_COMPONENT_ADDED_NOOP

    // Skeleton is used as a bare component type, not a `struct *Component`, so the
    // generator's scan never emits it — its no-op specialization stays hand-written
    // (both the add here and the remove in the no-op block below).
    template<>
    void Scene::OnComponentAdded<Skeleton>(Entity, Skeleton&) {}

    template<>
    void Scene::OnComponentAdded<CinematicComponent>(Entity, CinematicComponent& component)
    {
        // If a cinematic is added while the scene is already running, honour
        // PlayOnStart now — OnRuntimeStart (which normally does this) has passed.
        if (m_IsRunning && component.PlayOnStart)
        {
            component.PlayFromStart();
        }
    }

    // Runtime-spawned AudioSoundGraphComponent entities (script-spawned, networked
    // actors arriving mid-session, dropped loot, etc.) need their Sound wrapper
    // initialised on the spot. Without this hook, InitAudioRuntime only fires once
    // at OnRuntimeStart and components added afterwards stay silent.
    //
    // Gate: m_AudioEventsManager is non-null iff InitAudioRuntime has run and
    // OnRuntimeStop hasn't yet torn it down. That covers the editor's play mode,
    // OloRuntime, and Functional tests that explicitly call InitAudioRuntime in
    // headless mode — while staying inert in edit mode (where adding a component
    // should not allocate audio resources) and in non-audio headless builds.
    template<>
    void Scene::OnComponentAdded<AudioSoundGraphComponent>(Entity, AudioSoundGraphComponent& component)
    {
        if (m_AudioEventsManager)
        {
            InitializeAudioSoundGraph(component);
        }
    }

    // Video components allocate no resources on add: VideoSystem::OnUpdate lazily creates
    // the VideoPlayer (honouring PlayOnStart / AutoPlay) on the first runtime tick, which
    // also covers entities added mid-play. Editing the component in edit mode must not spin
    // up a decode thread, so these stay empty.
    template<>
    void Scene::OnComponentAdded<VideoOverlayComponent>(Entity, VideoOverlayComponent&)
    {
    }

    template<>
    void Scene::OnComponentAdded<VideoSurfaceComponent>(Entity, VideoSurfaceComponent&)
    {
    }

    template<>
    void Scene::OnComponentAdded<Rigidbody3DComponent>(Entity entity, Rigidbody3DComponent& component)
    {
        // If physics is already running when a Rigidbody3DComponent is added at
        // runtime (projectile spawn, networked actor arrival, dropped loot),
        // create the Jolt body immediately. Without this hook the runtime-added
        // body silently never simulates because OnPhysics3DStart only iterates
        // entities that existed at start time.
        // Convention: callers should add the collider component (Box / Sphere /
        // Capsule / Mesh) BEFORE the Rigidbody3DComponent so the body's shape
        // resolves correctly on construction.
        if (m_JoltScene && m_JoltScene->IsInitialized())
        {
            if (auto body = m_JoltScene->CreateBody(entity))
            {
                component.m_RuntimeBodyToken = static_cast<u64>(body->GetBodyID().GetIndexAndSequenceNumber());
            }
        }
    }

    // Specialisation: when a Rigidbody3DComponent is removed at runtime, the
    // owning JoltScene must release the body. Without this hook, the body
    // stays in JoltScene::m_Bodies and SynchronizeTransforms keeps writing
    // its post-physics position into the (now-orphaned) TransformComponent.
    template<>
    void Scene::OnComponentRemoved<Rigidbody3DComponent>(Entity entity, Rigidbody3DComponent& component)
    {
        // The chassis body backs any VehicleConstraint on this entity, and that
        // constraint step-listens on the body, so destroy the vehicle before the
        // body it drives — otherwise the constraint dereferences a freed body on
        // the next physics tick (a vehicle without its chassis can't simulate).
        if (m_JoltScene && entity.HasComponent<VehicleComponent>())
        {
            auto& vehicle = entity.GetComponent<VehicleComponent>();
            if (vehicle.m_RuntimeVehicleToken != 0)
            {
                m_JoltScene->DestroyVehicle(entity);
                vehicle.m_RuntimeVehicleToken = 0;
            }
        }

        if (m_JoltScene && component.m_RuntimeBodyToken != 0)
        {
            m_JoltScene->DestroyBody(entity);
            component.m_RuntimeBodyToken = 0;
        }
    }

    // Specialisation: when a TerrainComponent is detached at runtime, release the
    // static height-field collision body it owns (issue #428). That body is a raw
    // JPH body keyed by entity UUID, outside the Rigidbody3D/vehicle/character
    // teardown, so without this hook it (and its BodyID→entity mapping) leaks until
    // JoltScene shutdown. Entity destruction routes through Scene::DestroyEntity,
    // which does the same explicitly because m_Registry.destroy() never fires this.
    template<>
    void Scene::OnComponentRemoved<TerrainComponent>(Entity entity, TerrainComponent& component)
    {
        if (m_JoltScene && component.m_RuntimeCollisionBodyToken != 0)
        {
            m_JoltScene->DestroyTerrainBody(entity.GetUUID());
            component.m_RuntimeCollisionBodyToken = 0;
        }
    }

    template<>
    void Scene::OnComponentAdded<PhysicsJoint3DComponent>(Entity entity, PhysicsJoint3DComponent& /*component*/)
    {
        // If physics is already running when a joint is added at runtime, create
        // the Jolt constraint immediately (mirrors the Rigidbody3D runtime-add
        // hook). Both endpoint bodies must already exist; CreateConstraint warns
        // and skips otherwise, and sets m_RuntimeConstraintToken on success.
        if (m_JoltScene && m_JoltScene->IsInitialized())
        {
            m_JoltScene->CreateConstraint(entity);
            // Rebuild collision filtering so a runtime-added no-collide joint
            // takes effect (and a collide joint leaves filtering unchanged).
            m_JoltScene->ApplyJointCollisionFilters();
        }
    }

    // Specialisation: when a PhysicsJoint3DComponent is removed at runtime, the
    // owning JoltScene must release the Jolt constraint. Without this hook the
    // constraint stays registered and keeps pulling on bodies that may have been
    // re-purposed.
    template<>
    void Scene::OnComponentRemoved<PhysicsJoint3DComponent>(Entity entity, PhysicsJoint3DComponent& component)
    {
        if (m_JoltScene && component.m_RuntimeConstraintToken != 0)
        {
            m_JoltScene->DestroyConstraint(entity);
            component.m_RuntimeConstraintToken = 0;
            // Rebuild collision filtering so removing a no-collide joint at runtime
            // re-enables collision between the two bodies it had connected.
            if (m_JoltScene->IsInitialized())
                m_JoltScene->ApplyJointCollisionFilters();
        }
    }

    template<>
    void Scene::OnComponentAdded<VehicleComponent>(Entity entity, VehicleComponent& /*component*/)
    {
        // If physics is already running when a vehicle is added at runtime, build
        // the Jolt VehicleConstraint immediately (mirrors the joint runtime-add
        // hook). The chassis rigidbody must already exist; CreateVehicle warns and
        // skips otherwise, and sets m_RuntimeVehicleToken on success.
        if (m_JoltScene && m_JoltScene->IsInitialized())
        {
            (void)m_JoltScene->CreateVehicle(entity);
        }
    }

    // Specialisation: when a VehicleComponent is removed at runtime, the owning
    // JoltScene must release the Jolt VehicleConstraint (and unregister its step
    // listener). Without this hook the constraint stays registered and keeps
    // driving a chassis body that may have been re-purposed.
    template<>
    void Scene::OnComponentRemoved<VehicleComponent>(Entity entity, VehicleComponent& component)
    {
        if (m_JoltScene && component.m_RuntimeVehicleToken != 0)
        {
            m_JoltScene->DestroyVehicle(entity);
            component.m_RuntimeVehicleToken = 0;
        }
    }

    template<>
    void Scene::OnComponentAdded<RagdollComponent>(Entity /*entity*/, RagdollComponent& /*component*/)
    {
        // Intentionally empty. Unlike rigidbodies / joints / vehicles, a ragdoll
        // is NOT built on a mid-play add: JoltScene::CreateRagdolls authors a
        // chain of Rigidbody3D + collider + SwingTwist-joint components onto the
        // bone entities, and that must run BEFORE the body/constraint creation
        // passes (i.e. before JoltScene::Initialize) so the bodies resolve their
        // shapes and the joints resolve both endpoints — see Scene::OnPhysics3DStart.
        // Mid-game ragdoll enable/disable is an explicit follow-up (issue #308).
    }

    // Specialisation: when a RagdollComponent is removed at runtime, drop any
    // generated bodies/joints the ragdoll authored onto its bone entities so they
    // don't linger after the ragdoll is gone. DestroyRagdoll is idempotent (a
    // no-op when nothing was generated), so this is safe in edit mode too.
    template<>
    void Scene::OnComponentRemoved<RagdollComponent>(Entity entity, RagdollComponent& component)
    {
        if (m_JoltScene && component.m_RuntimeRagdollToken != 0)
        {
            m_JoltScene->DestroyRagdoll(entity);
            component.m_RuntimeRagdollToken = 0;
        }
    }

    // Specialisation: same idea for the character controller path.
    template<>
    void Scene::OnComponentRemoved<CharacterController3DComponent>(Entity entity, CharacterController3DComponent& /*component*/)
    {
        if (m_JoltScene)
        {
            m_JoltScene->DestroyCharacterController(entity);
        }
    }

    template<>
    void Scene::OnComponentAdded<CharacterController3DComponent>(Entity entity, CharacterController3DComponent& /*component*/)
    {
        // Mirrors the Rigidbody3DComponent hook above. JoltScene::CreateCharacterController
        // builds the JoltCharacterController and registers it for per-frame Update().
        // Convention: add the collider component (Capsule recommended) BEFORE the
        // CharacterController3DComponent so the controller's shape resolves at
        // construction time.
        if (m_JoltScene && m_JoltScene->IsInitialized())
        {
            (void)m_JoltScene->CreateCharacterController(entity);
        }
    }

    [[nodiscard]] Entity Scene::FindEntityByName(std::string_view name)
    {
        auto const nameStr = std::string(name);
        auto [rangeBegin, rangeEnd] = m_EntityNameMap.equal_range(nameStr);
        auto it = rangeBegin;
        while (it != rangeEnd)
        {
            if (m_Registry.valid(it->second))
                return Entity{ it->second, this };
            // Stale entry — erase returns the next iterator within the bucket
            it = m_EntityNameMap.erase(it);
        }
        // Fallback O(n) scan for cache misses (e.g. after rename without UpdateEntityName)
        for (auto view = m_Registry.view<TagComponent>(); auto entity : view)
        {
            const TagComponent& tc = view.get<TagComponent>(entity);
            if (tc.Tag == name)
            {
                m_EntityNameMap.emplace(nameStr, entity);
                return Entity{ entity, this };
            }
        }
        return {};
    }

    [[nodiscard]] Entity Scene::GetEntityByUUID(UUID uuid)
    {
        OLO_CORE_ASSERT(m_EntityMap.Contains(uuid));
        return { m_EntityMap.FindChecked(uuid), this };
    }

    [[nodiscard]] Entity Scene::FindEntityByName(std::string_view name) const
    {
        auto const nameStr = std::string(name);
        auto [rangeBegin, rangeEnd] = m_EntityNameMap.equal_range(nameStr);
        for (auto it = rangeBegin; it != rangeEnd; ++it)
        {
            if (m_Registry.valid(it->second))
                return Entity{ it->second, const_cast<Scene*>(this) };
        }
        for (auto view = m_Registry.view<TagComponent>(); auto entity : view)
        {
            const TagComponent& tc = view.get<TagComponent>(entity);
            if (tc.Tag == name)
            {
                return Entity{ entity, const_cast<Scene*>(this) };
            }
        }
        return {};
    }

    void Scene::UpdateEntityName(entt::entity entity, const std::string& oldName, const std::string& newName)
    {
        // Remove old entry for this specific entity
        auto [rangeBegin, rangeEnd] = m_EntityNameMap.equal_range(oldName);
        for (auto it = rangeBegin; it != rangeEnd; ++it)
        {
            if (it->second == entity)
            {
                m_EntityNameMap.erase(it);
                break;
            }
        }
        m_EntityNameMap.emplace(newName, entity);
    }

    [[nodiscard]] Entity Scene::GetEntityByUUID(UUID uuid) const
    {
        OLO_CORE_ASSERT(m_EntityMap.Contains(uuid));
        // SAFETY: this is const Scene*, but Entity requires non-const Scene*
        // This is safe because Entity lookup only reads entity data
        return { m_EntityMap.FindChecked(uuid), const_cast<Scene*>(this) };
    }

    [[nodiscard]] Entity Scene::GetPrimaryCameraEntity() const
    {
        for (const auto view = m_Registry.view<CameraComponent>(); auto const entity : view)
        {
            if (const auto& camera = view.get<CameraComponent>(entity); camera.Primary)
            {
                // SAFETY: this is const Scene*, but Entity requires non-const Scene*
                // This is safe because camera lookup only reads entity data
                return Entity{ entity, const_cast<Scene*>(this) };
            }
        }
        return {};
    }

    // Bone entity management methods (Hazel-style)
    std::vector<glm::mat4> Scene::GetModelSpaceBoneTransforms(const std::vector<UUID>& boneEntityIds, const MeshSource& meshSource) const
    {
        return BoneEntityUtils::GetModelSpaceBoneTransforms(boneEntityIds, &meshSource, this);
    }

    std::vector<UUID> Scene::FindBoneEntityIds(Entity rootEntity, const Skeleton& skeleton) const
    {
        return BoneEntityUtils::FindBoneEntityIds(rootEntity, &skeleton, this);
    }

    glm::mat4 Scene::FindRootBoneTransform(Entity entity, const std::vector<UUID>& boneEntityIds) const
    {
        return BoneEntityUtils::FindRootBoneTransform(entity, boneEntityIds, this);
    }

    void Scene::BuildBoneEntityIds(Entity entity)
    {
        // Build bone entity IDs for the given entity, using itself as the root entity
        // This is useful when the entity is both the mesh and the root of the bone hierarchy
        BuildMeshBoneEntityIds(entity, entity);
    }

    void Scene::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity)
    {
        BoneEntityUtils::BuildMeshBoneEntityIds(entity, rootEntity, this);
    }

    void Scene::BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity)
    {
        BoneEntityUtils::BuildAnimationBoneEntityIds(entity, rootEntity, this);
    }

    [[nodiscard]] std::optional<Entity> Scene::TryGetEntityWithUUID(UUID id) const
    {
        if (auto* entityPtr = m_EntityMap.Find(id))
        {
            return Entity{ *entityPtr, this };
        }
        return std::nullopt;
    }

    bool Scene::ResolveIKTargets(Entity entity, IKTargetComponent& out) const
    {
        if (!entity.HasComponent<IKTargetComponent>())
        {
            return false;
        }

        out = entity.GetComponent<IKTargetComponent>();

        if (static_cast<u64>(out.AimTargetEntity) != 0)
        {
            if (auto targetEnt = TryGetEntityWithUUID(out.AimTargetEntity))
            {
                out.AimTarget = targetEnt->GetComponent<TransformComponent>().Translation;
            }
        }
        if (static_cast<u64>(out.LimbTargetEntity) != 0)
        {
            if (auto targetEnt = TryGetEntityWithUUID(out.LimbTargetEntity))
            {
                out.LimbTarget = targetEnt->GetComponent<TransformComponent>().Translation;
            }
        }
        if (static_cast<u64>(out.ChainTargetEntity) != 0)
        {
            if (auto targetEnt = TryGetEntityWithUUID(out.ChainTargetEntity))
            {
                out.ChainTarget = targetEnt->GetComponent<TransformComponent>().Translation;
            }
        }

        return true;
    }

    const SpringBoneComponent* Scene::ResolveSpringBone(Entity entity, Animation::SpringBoneState*& outState)
    {
        outState = nullptr;
        if (!entity.HasComponent<SpringBoneComponent>())
        {
            return nullptr;
        }

        auto& springBone = entity.GetComponent<SpringBoneComponent>();
        if (!springBone.Enabled)
        {
            return nullptr;
        }

        if (!entity.HasComponent<SpringBoneStateComponent>())
        {
            entity.AddComponent<SpringBoneStateComponent>();
        }
        outState = &entity.GetComponent<SpringBoneStateComponent>().State;
        return &springBone;
    }

    const NoiseAnimationComponent* Scene::ResolveNoiseAnimation(Entity entity, Animation::NoiseAnimationState*& outState)
    {
        outState = nullptr;
        if (!entity.HasComponent<NoiseAnimationComponent>())
        {
            return nullptr;
        }

        auto& noise = entity.GetComponent<NoiseAnimationComponent>();
        if (!noise.Enabled)
        {
            return nullptr;
        }

        if (!entity.HasComponent<NoiseAnimationStateComponent>())
        {
            entity.AddComponent<NoiseAnimationStateComponent>();
        }
        outState = &entity.GetComponent<NoiseAnimationStateComponent>().State;
        return &noise;
    }

    void Scene::OnPhysics2DStart()
    {
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = { 0.0f, -9.81f };
        m_PhysicsWorld = b2CreateWorld(&worldDef);

        for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
        {
            Entity entity = { e, this };
            auto const& transform = entity.GetComponent<TransformComponent>();
            auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

            b2BodyDef bodyDef = b2DefaultBodyDef();
            bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
            bodyDef.position = { transform.Translation.x, transform.Translation.y };
            bodyDef.rotation = b2MakeRot(transform.GetRotationEuler().z);

            b2BodyId body = b2CreateBody(m_PhysicsWorld, &bodyDef);
            b2Body_SetFixedRotation(body, rb2d.FixedRotation);
            rb2d.RuntimeBody = body;

            // Apply persisted velocities (non-zero after a save/load cycle)
            if (rb2d.Type != Rigidbody2DComponent::BodyType::Static)
            {
                b2Body_SetLinearVelocity(body, { rb2d.LinearVelocity.x, rb2d.LinearVelocity.y });
                b2Body_SetAngularVelocity(body, rb2d.AngularVelocity);
            }

            if (entity.HasComponent<BoxCollider2DComponent>())
            {
                auto const& bc2d = entity.GetComponent<BoxCollider2DComponent>();

                b2ShapeDef shapeDef = b2DefaultShapeDef();
                shapeDef.density = bc2d.Density;
                shapeDef.material.friction = bc2d.Friction;
                shapeDef.material.restitution = bc2d.Restitution;

                b2Polygon polygon = b2MakeOffsetBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y,
                                                    { bc2d.Offset.x, bc2d.Offset.y }, b2MakeRot(0.0f));
                b2CreatePolygonShape(body, &shapeDef, &polygon);
            }

            if (entity.HasComponent<CircleCollider2DComponent>())
            {
                auto const& cc2d = entity.GetComponent<CircleCollider2DComponent>();

                b2ShapeDef shapeDef = b2DefaultShapeDef();
                shapeDef.density = cc2d.Density;
                shapeDef.material.friction = cc2d.Friction;
                shapeDef.material.restitution = cc2d.Restitution;

                b2Circle circle = { b2Vec2(cc2d.Offset.x, cc2d.Offset.y), transform.Scale.x * cc2d.Radius };
                b2CreateCircleShape(body, &shapeDef, &circle);
            }
        }
    }

    void Scene::OnPhysics2DStop()
    {
        if (b2World_IsValid(m_PhysicsWorld))
        {
            b2DestroyWorld(m_PhysicsWorld);
            m_PhysicsWorld = b2_nullWorldId;
        }
    }

    namespace
    {
        // Build (or rebuild) the static height-field collision body for a single-tile
        // terrain entity from its CPU height field (issue #428). Skips streamed terrains
        // (per-tile collision is a follow-up). Caller guarantees physics is initialized.
        //
        // The heights are taken from the already-built TerrainData when present (editor /
        // runtime render path); otherwise they are generated GPU-free so headless runtimes
        // (dedicated server, functional tests) still get collision — GenerateHeightField
        // produces the identical field the render path's GenerateHeightmap would.
        void BuildTerrainCollisionBody(JoltScene& joltScene, Entity entity, TerrainComponent& terrain)
        {
            // Drop any prior terrain body first, so every early-out below (collision
            // disabled, streaming, no CPU heights, shape build failure) leaves a
            // consistent "no collision body" state instead of a stale body on the old
            // surface. The success path re-creates fresh (CreateTerrainBody is itself
            // idempotent, so the duplicate destroy is a cheap no-op).
            joltScene.DestroyTerrainBody(entity.GetUUID());
            terrain.m_RuntimeCollisionBodyToken = 0;

            if (!terrain.m_CollisionEnabled || terrain.m_StreamingEnabled)
                return;

            u32 resolution = 0;
            const std::vector<f32>* heightsPtr = nullptr;
            std::vector<f32> generated;

            if (terrain.m_TerrainData && terrain.m_TerrainData->GetResolution() > 0)
            {
                resolution = terrain.m_TerrainData->GetResolution();
                heightsPtr = &terrain.m_TerrainData->GetHeightData();
            }
            else if (terrain.m_ProceduralEnabled)
            {
                TerrainGenerator::HeightParams params;
                params.Resolution = terrain.m_ProceduralResolution;
                params.Seed = terrain.m_ProceduralSeed;
                params.Octaves = terrain.m_ProceduralOctaves;
                params.Frequency = terrain.m_ProceduralFrequency;
                params.Lacunarity = terrain.m_ProceduralLacunarity;
                params.Persistence = terrain.m_ProceduralPersistence;
                params.Shaping = terrain.m_HeightShaping;
                params.ErosionIterations = terrain.m_ProceduralErosionIterations;
                TerrainGenerator::GenerateHeightField(generated, params);
                resolution = terrain.m_ProceduralResolution;
                heightsPtr = &generated;
            }
            else if (terrain.m_HeightmapPath.empty())
            {
                // Flat terrain — mirrors the render path's CreateFlat(256, 0).
                resolution = 256;
                generated.assign(static_cast<sizet>(resolution) * resolution, 0.0f);
                heightsPtr = &generated;
            }
            else
            {
                // Heightmap-file terrain whose CPU heights aren't built yet (headless).
                // File loading is GPU-coupled today; the render-built TerrainData path
                // covers editor/runtime. Headless heightmap-file collision is a follow-up.
                OLO_CORE_WARN("Terrain collision: heightmap-file terrain has no CPU heights yet; skipping collision for entity {0}", (u64)entity.GetUUID());
                return;
            }

            const auto& transform = entity.GetComponent<TransformComponent>();
            JPH::Ref<JPH::Shape> shape = JoltShapes::CreateTerrainHeightFieldShape(
                *heightsPtr, resolution, terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale, transform.Scale);
            if (!shape)
            {
                terrain.m_RuntimeCollisionBodyToken = 0;
                return;
            }

            JPH::BodyID bodyID = joltScene.CreateTerrainBody(entity, shape, transform.Translation, transform.GetRotation());
            terrain.m_RuntimeCollisionBodyToken = bodyID.IsInvalid() ? 0 : static_cast<u64>(bodyID.GetIndexAndSequenceNumber());
        }

        // ── Cloth soft bodies (issue #460) ─────────────────────────────────────
        // Row-major triangle indices for a columns×rows cloth grid (two triangles per
        // cell). Winding matches the soft-body faces built in CreateClothSharedSettings.
        std::vector<u32> BuildClothGridIndices(u32 columns, u32 rows)
        {
            std::vector<u32> indices;
            if (columns < 2 || rows < 2)
                return indices;
            indices.reserve(static_cast<sizet>(columns - 1) * (rows - 1) * 6);
            for (u32 row = 0; row + 1 < rows; ++row)
            {
                for (u32 col = 0; col + 1 < columns; ++col)
                {
                    const u32 i0 = row * columns + col;
                    const u32 i1 = i0 + 1;
                    const u32 i2 = i0 + columns;
                    const u32 i3 = i2 + 1;
                    indices.push_back(i0);
                    indices.push_back(i2);
                    indices.push_back(i1);
                    indices.push_back(i1);
                    indices.push_back(i2);
                    indices.push_back(i3);
                }
            }
            return indices;
        }

        // Smooth per-vertex normals for the deformed cloth: accumulate each grid triangle's
        // face normal onto its three vertices, then normalize. Lets the render mesh shade
        // correctly as the sheet folds. outNormals is resized to match positions.
        void ComputeClothNormals(const std::vector<glm::vec3>& positions, u32 columns, u32 rows,
                                 std::vector<glm::vec3>& outNormals)
        {
            outNormals.assign(positions.size(), glm::vec3(0.0f));
            if (positions.size() != static_cast<sizet>(columns) * rows || columns < 2 || rows < 2)
                return;

            auto addTri = [&](u32 a, u32 b, u32 c)
            {
                const glm::vec3 n = glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
                outNormals[a] += n;
                outNormals[b] += n;
                outNormals[c] += n;
            };
            for (u32 row = 0; row + 1 < rows; ++row)
            {
                for (u32 col = 0; col + 1 < columns; ++col)
                {
                    const u32 i0 = row * columns + col;
                    const u32 i1 = i0 + 1;
                    const u32 i2 = i0 + columns;
                    const u32 i3 = i2 + 1;
                    addTri(i0, i2, i1);
                    addTri(i1, i2, i3);
                }
            }
            for (glm::vec3& n : outNormals)
            {
                const f32 len = glm::length(n);
                n = (len > 1.0e-6f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }
    } // namespace

    void Scene::OnPhysics3DStart()
    {
        // Ensure JoltScene was properly initialized in constructor
        OLO_CORE_ASSERT(m_JoltScene, "JoltScene should be initialized in constructor");

        // Ragdoll pass FIRST: expand every enabled RagdollComponent into a chain
        // of Rigidbody3D + collider + SwingTwist-joint components on its bone
        // entities. This is a pure ECS-authoring pass (no Jolt yet) and must run
        // before Initialize() so (a) the body/joint loops below pick the generated
        // components up, and (b) the OnComponentAdded hooks stay no-ops here
        // (m_JoltScene is not initialized yet), avoiding redundant body creation.
        m_JoltScene->CreateRagdolls();

        m_JoltScene->Initialize();

        if (!m_JoltScene->IsInitialized())
        {
            OLO_CORE_ERROR("Failed to initialize 3D physics system");
            return;
        }

        // Create physics bodies for all entities with Rigidbody3DComponent.
        // Establishing the Rigidbody3D owning group here front-loads its one-time
        // packing pass at physics-start, before the per-step transform-sync loop
        // in OnPhysics3DUpdate reuses it (issue #443 ownership map).
        auto rbGroup = m_Registry.group<Rigidbody3DComponent>(entt::get<TransformComponent>);
        for (auto entity : rbGroup)
        {
            Entity ent = { entity, this };

            // Create the physics body - JoltScene will handle shape creation based on components
            auto body = m_JoltScene->CreateBody(ent);
            if (body)
            {
                auto& rb3d = rbGroup.get<Rigidbody3DComponent>(entity);
                // Store only the body token for safe runtime access
                rb3d.m_RuntimeBodyToken = static_cast<std::uint64_t>(body->GetBodyID().GetIndexAndSequenceNumber());
            }
        }

        // Build character controllers for any entity that already has a
        // CharacterController3DComponent at start-of-physics. The runtime-add
        // hook (OnComponentAdded specialization) covers entities created
        // afterwards.
        auto ccView = m_Registry.view<CharacterController3DComponent, TransformComponent>();
        for (auto entity : ccView)
        {
            Entity ent = { entity, this };
            (void)m_JoltScene->CreateCharacterController(ent);
        }

        // Joint second pass: every rigidbody now exists, so two-body constraints
        // can resolve both endpoints. CreateConstraint sets m_RuntimeConstraintToken
        // on success; runtime-added joints are covered by the
        // OnComponentAdded<PhysicsJoint3DComponent> hook.
        auto jointView = m_Registry.view<PhysicsJoint3DComponent>();
        for (auto entity : jointView)
        {
            Entity ent = { entity, this };
            (void)m_JoltScene->CreateConstraint(ent);
        }

        // Every body and joint exists now, so joints that opted out of connected-
        // body collision (m_CollideConnected == false) can be filtered.
        m_JoltScene->ApplyJointCollisionFilters();

        // Vehicle pass: every chassis rigidbody now exists, so the Jolt
        // VehicleConstraint can be built around it. CreateVehicle sets
        // m_RuntimeVehicleToken on success; runtime-added vehicles are covered by
        // the OnComponentAdded<VehicleComponent> hook.
        auto vehicleView = m_Registry.view<VehicleComponent>();
        for (auto entity : vehicleView)
        {
            Entity ent = { entity, this };
            (void)m_JoltScene->CreateVehicle(ent);
        }

        // Terrain collision pass: give every opted-in single-tile terrain a static
        // height-field body so characters/vehicles rest on it and raycasts hit it
        // (issue #428). Runs last — independent of the rigidbody/joint/vehicle graph.
        auto terrainView = m_Registry.view<TransformComponent, TerrainComponent>();
        for (auto entity : terrainView)
        {
            Entity ent = { entity, this };
            BuildTerrainCollisionBody(*m_JoltScene, ent, ent.GetComponent<TerrainComponent>());
        }

        // Cloth pass: build a Jolt soft body for every enabled ClothComponent from its
        // grid + world transform (issue #460). GPU-free — the deforming render mesh is
        // built lazily in the render pass, so this also runs headless / on the server.
        m_ClothRuntime.clear();
        auto clothView = m_Registry.view<TransformComponent, ClothComponent>();
        for (auto entity : clothView)
        {
            Entity ent = { entity, this };
            const auto& cloth = ent.GetComponent<ClothComponent>();
            if (!cloth.m_Enabled)
                continue;

            const auto& transform = ent.GetComponent<TransformComponent>();
            JPH::Ref<JPH::SoftBodySharedSettings> settings = JoltShapes::CreateClothSharedSettings(
                cloth.m_Columns, cloth.m_Rows, cloth.m_Width, cloth.m_Height, cloth.m_Mass,
                cloth.m_Compliance, cloth.m_BendCompliance, cloth.m_Attachment, transform.GetTransform());
            if (!settings)
                continue;

            JPH::BodyID bodyID = m_JoltScene->CreateClothBody(ent, settings, cloth.m_Iterations, cloth.m_LinearDamping, cloth.m_Pressure);
            if (bodyID.IsInvalid())
                continue;

            // Register runtime render state and seed the CPU position buffer with the rest
            // shape so the first render (before the first physics tick) already has geometry.
            ClothRuntimeState state;
            state.m_Columns = std::clamp(cloth.m_Columns, 2u, 128u);
            state.m_Rows = std::clamp(cloth.m_Rows, 2u, 128u);
            (void)m_JoltScene->GetClothVertices(ent.GetUUID(), state.m_Positions);
            ComputeClothNormals(state.m_Positions, state.m_Columns, state.m_Rows, state.m_Normals);
            m_ClothRuntime[ent.GetUUID()] = std::move(state);
        }
    }

    void Scene::OnPhysics3DStop()
    {
        // Idempotent: m_JoltScene is a lifetime member (created in the ctor and
        // never nulled), so guard on whether physics is actually *running*, not on
        // the pointer. A second OnPhysics3DStop (e.g. a manual stop followed by a
        // harness TearDown stop) then early-returns cleanly instead of re-running
        // the whole body/vehicle/joint/terrain teardown against an already-shut
        // Jolt system — which would dereference dead state once the scene has
        // physics entities. Shutdown() flips IsInitialized() to false.
        if (!m_JoltScene || !m_JoltScene->IsInitialized())
        {
            return;
        }

        // Tear ragdolls down FIRST: this removes the generated Rigidbody3D /
        // collider / SwingTwist-joint components from the bone entities (via their
        // OnComponentRemoved hooks, which release the matching Jolt body/constraint
        // while they still exist), restoring the authored scene before the bulk
        // body/constraint teardown below handles any pre-authored physics.
        m_JoltScene->DestroyAllRagdolls();

        // Remove vehicles and joints first — both reference (and vehicles also
        // step-listen on) bodies, so they must go before the bodies are destroyed.
        auto vehicleView = m_Registry.view<VehicleComponent>();
        for (auto entity : vehicleView)
        {
            Entity ent = { entity, this };
            auto& vehicle = ent.GetComponent<VehicleComponent>();
            if (vehicle.m_RuntimeVehicleToken != 0)
            {
                m_JoltScene->DestroyVehicle(ent);
                vehicle.m_RuntimeVehicleToken = 0;
            }
        }

        auto jointView = m_Registry.view<PhysicsJoint3DComponent>();
        for (auto entity : jointView)
        {
            Entity ent = { entity, this };
            auto& joint = ent.GetComponent<PhysicsJoint3DComponent>();
            if (joint.m_RuntimeConstraintToken != 0)
            {
                m_JoltScene->DestroyConstraint(ent);
                joint.m_RuntimeConstraintToken = 0;
            }
        }

        // Clean up all physics bodies
        auto view = m_Registry.view<Rigidbody3DComponent>();
        for (auto entity : view)
        {
            Entity ent = { entity, this };
            auto& rb3d = ent.GetComponent<Rigidbody3DComponent>();

            if (rb3d.m_RuntimeBodyToken != 0)
            {
                // Destroy the body using the entity
                m_JoltScene->DestroyBody(ent);
                rb3d.m_RuntimeBodyToken = 0;
            }
        }

        // Clean up character controllers symmetrically — JoltScene::Shutdown
        // otherwise inherits dangling JoltCharacterController instances tied
        // to entities that are about to vanish.
        auto controllerView = m_Registry.view<CharacterController3DComponent>();
        for (auto entity : controllerView)
        {
            Entity ent = { entity, this };
            m_JoltScene->DestroyCharacterController(ent);
        }

        // Tear down terrain collision bodies and clear their runtime tokens (Shutdown
        // also sweeps any stragglers, but clearing tokens keeps the components clean).
        auto terrainView = m_Registry.view<TerrainComponent>();
        for (auto entity : terrainView)
        {
            Entity ent = { entity, this };
            auto& terrain = ent.GetComponent<TerrainComponent>();
            if (terrain.m_RuntimeCollisionBodyToken != 0)
            {
                m_JoltScene->DestroyTerrainBody(ent.GetUUID());
                terrain.m_RuntimeCollisionBodyToken = 0;
            }
        }

        // Tear down cloth soft bodies and drop their runtime render state (issue #460).
        // Shutdown below also sweeps any stragglers; this releases the render meshes early.
        for (const auto& [entityID, state] : m_ClothRuntime)
            m_JoltScene->DestroyClothBody(entityID);
        m_ClothRuntime.clear();

        m_JoltScene->Shutdown();
    }

    const std::vector<glm::vec3>* Scene::GetClothVertexPositions(UUID entityID) const
    {
        auto it = m_ClothRuntime.find(entityID);
        if (it == m_ClothRuntime.end() || it->second.m_Positions.empty())
            return nullptr;
        return &it->second.m_Positions;
    }

    void Scene::ProcessSnowDeformers(Timestep ts, TMap<u64, glm::vec3>& prevPositions)
    {
        OLO_PROFILE_FUNCTION();

        const auto& accumSettings = Renderer3D::GetSnowAccumulationSettings();
        const auto& ejectaSettings = Renderer3D::GetSnowEjectaSettings();

        bool const accumActive = accumSettings.Enabled && SnowAccumulationSystem::IsInitialized();
        bool const ejectaActive = ejectaSettings.Enabled && SnowEjectaSystem::IsInitialized();

        if (!accumActive && !ejectaActive)
        {
            return;
        }

        auto deformerView = m_Registry.view<TransformComponent, SnowDeformerComponent>();
        std::vector<glm::vec4> stamps;
        if (accumActive)
        {
            stamps.reserve(deformerView.size_hint() * 2);
        }

        for (auto entity : deformerView)
        {
            auto& transform = deformerView.get<TransformComponent>(entity);
            const auto& deformer = deformerView.get<SnowDeformerComponent>(entity);

            glm::vec3 pos = transform.Translation;

            if (accumActive)
            {
                stamps.emplace_back(pos.x, pos.y, pos.z, deformer.m_DeformRadius);
                stamps.emplace_back(deformer.m_DeformDepth, deformer.m_FalloffExponent, deformer.m_CompactionFactor, 0.0f);
            }

            // Always track position so toggling m_EmitEjecta doesn't cause velocity spikes
            auto entityObj = Entity{ entity, this };
            u64 uuid = entityObj.GetUUID();

            glm::vec3 velocity(0.0f);
            if (glm::vec3* prevPos = prevPositions.Find(uuid))
            {
                if (ts > 0.0f)
                {
                    velocity = (pos - *prevPos) / static_cast<f32>(ts);
                }
                *prevPos = pos;
            }
            else
            {
                prevPositions.FindOrAdd(uuid, pos);
            }

            if (deformer.m_EmitEjecta && ejectaActive)
            {
                SnowEjectaSystem::EmitAt(
                    pos, velocity,
                    deformer.m_DeformRadius,
                    deformer.m_DeformDepth,
                    ejectaSettings);
            }
        }

        if (accumActive && !stamps.empty())
        {
            SnowAccumulationSystem::SubmitDeformers(stamps.data(),
                                                    static_cast<u32>(stamps.size() / 2));
        }
    }

    void Scene::RenderUIOverlay()
    {
        OLO_PROFILE_FUNCTION();
        if (m_ViewportWidth == 0 || m_ViewportHeight == 0)
        {
            OLO_CORE_WARN("RenderUIOverlay: viewport is {}x{} — skipping", m_ViewportWidth, m_ViewportHeight);
            return;
        }

        // UI always renders on top of the scene — disable depth testing
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(true);
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        if (!m_UILayoutResolvedThisFrame)
        {
            UILayoutSystem::ResolveLayout(*this, m_ViewportWidth, m_ViewportHeight, m_CameraViewProjection);
        }

        glm::mat4 uiProjection = glm::ortho(0.0f, static_cast<f32>(m_ViewportWidth),
                                            static_cast<f32>(m_ViewportHeight), 0.0f,
                                            -1.0f, 1.0f);
        UIRenderer::BeginScene(uiProjection);

        // Build sorted draw order based on UICanvasComponent::m_SortOrder
        auto resolvedView = GetAllEntitiesWith<UIResolvedRectComponent>();
        std::vector<entt::entity> uiEntities;
        for (const auto entity : resolvedView)
        {
            uiEntities.push_back(entity);
        }
        std::ranges::sort(uiEntities,
                          [this](entt::entity a, entt::entity b)
                          {
                              i32 sortA = 0;
                              i32 sortB = 0;
                              if (m_Registry.all_of<UICanvasComponent>(a))
                              {
                                  sortA = m_Registry.get<UICanvasComponent>(a).m_SortOrder;
                              }
                              if (m_Registry.all_of<UICanvasComponent>(b))
                              {
                                  sortB = m_Registry.get<UICanvasComponent>(b).m_SortOrder;
                              }
                              if (sortA != sortB)
                              {
                                  return sortA < sortB;
                              }
                              return std::to_underlying(a) < std::to_underlying(b);
                          });

        for (const auto entity : uiEntities)
        {
            const auto& resolved = m_Registry.get<UIResolvedRectComponent>(entity);
            const int eid = static_cast<int>(std::to_underlying(entity));

            if (m_Registry.all_of<UIPanelComponent>(entity))
            {
                UIRenderer::DrawPanel(resolved.m_Position, resolved.m_Size, m_Registry.get<UIPanelComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIButtonComponent>(entity))
            {
                UIRenderer::DrawButton(resolved.m_Position, resolved.m_Size, m_Registry.get<UIButtonComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIImageComponent>(entity))
            {
                UIRenderer::DrawImage(resolved.m_Position, resolved.m_Size, m_Registry.get<UIImageComponent>(entity), eid);
            }

            if (m_Registry.all_of<UITextComponent>(entity))
            {
                UIRenderer::DrawUIText(resolved.m_Position, resolved.m_Size, m_Registry.get<UITextComponent>(entity), eid);
            }

            if (m_Registry.all_of<UISliderComponent>(entity))
            {
                UIRenderer::DrawSlider(resolved.m_Position, resolved.m_Size, m_Registry.get<UISliderComponent>(entity), eid);
            }

            if (m_Registry.all_of<UICheckboxComponent>(entity))
            {
                UIRenderer::DrawCheckbox(resolved.m_Position, resolved.m_Size, m_Registry.get<UICheckboxComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIProgressBarComponent>(entity))
            {
                UIRenderer::DrawProgressBar(resolved.m_Position, resolved.m_Size, m_Registry.get<UIProgressBarComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIInputFieldComponent>(entity))
            {
                UIRenderer::DrawInputField(resolved.m_Position, resolved.m_Size, m_Registry.get<UIInputFieldComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIScrollViewComponent>(entity))
            {
                UIRenderer::DrawScrollView(resolved.m_Position, resolved.m_Size, m_Registry.get<UIScrollViewComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIDropdownComponent>(entity))
            {
                UIRenderer::DrawDropdown(resolved.m_Position, resolved.m_Size, m_Registry.get<UIDropdownComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIToggleComponent>(entity))
            {
                UIRenderer::DrawToggle(resolved.m_Position, resolved.m_Size, m_Registry.get<UIToggleComponent>(entity), eid);
            }
        }

        // Nameplate rendering pass: draw floating HP/Mana bars above entities
        // with NameplateComponent.  Values are read from AbilityComponent.
        {
            auto nameplateView = GetAllEntitiesWith<NameplateComponent, TransformComponent>();
            for (const auto entity : nameplateView)
            {
                const auto& nameplate = nameplateView.get<NameplateComponent>(entity);
                if (!nameplate.m_Enabled)
                {
                    continue;
                }

                const auto& transform = nameplateView.get<TransformComponent>(entity);
                const glm::vec3 worldPos = transform.Translation + nameplate.m_WorldOffset;

                // Project world position to clip space
                const glm::vec4 clipPos = m_CameraViewProjection * glm::vec4(worldPos, 1.0f);
                if (clipPos.w <= 0.0f)
                {
                    continue; // Behind camera
                }

                const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                const f32 screenX = (ndc.x * 0.5f + 0.5f) * static_cast<f32>(m_ViewportWidth);
                const f32 screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(m_ViewportHeight);
                const int eid = static_cast<int>(std::to_underlying(entity));

                // Read health/mana from AbilityComponent
                f32 hp = 0.0f;
                f32 maxHp = 1.0f;
                f32 mana = 0.0f;
                f32 maxMana = 1.0f;
                if (m_Registry.all_of<AbilityComponent>(entity))
                {
                    const auto& abilities = m_Registry.get<AbilityComponent>(entity);
                    hp = abilities.Attributes.GetCurrentValue("Health");
                    maxHp = std::max(abilities.Attributes.GetCurrentValue("MaxHealth"), 1.0f);
                    mana = abilities.Attributes.GetCurrentValue("Mana");
                    maxMana = std::max(abilities.Attributes.GetCurrentValue("MaxMana"), 1.0f);
                }

                f32 currentY = screenY;

                // HP bar (bottom edge at projected point, bar extends upward)
                if (nameplate.m_ShowHealthBar)
                {
                    currentY -= nameplate.m_BarSize.y;
                    const glm::vec2 barPos = { screenX - nameplate.m_BarSize.x * 0.5f, currentY };
                    UIRenderer::DrawRect(barPos, nameplate.m_BarSize, nameplate.m_BarBackgroundColor, eid);
                    const f32 fill = glm::clamp(hp / maxHp, 0.0f, 1.0f);
                    const glm::vec2 fillSize = { nameplate.m_BarSize.x * fill, nameplate.m_BarSize.y };
                    if (fillSize.x > 0.0f)
                    {
                        UIRenderer::DrawRect(barPos, fillSize, nameplate.m_HealthBarColor, eid);
                    }
                }

                // Mana bar below HP bar (gap only applies when health bar is visible)
                if (nameplate.m_ShowManaBar)
                {
                    if (nameplate.m_ShowHealthBar)
                    {
                        currentY += nameplate.m_BarSize.y + nameplate.m_ManaBarGap;
                    }
                    const glm::vec2 manaSize = { nameplate.m_BarSize.x, nameplate.m_BarSize.y * 0.8f };
                    const glm::vec2 barPos = { screenX - manaSize.x * 0.5f, currentY };
                    UIRenderer::DrawRect(barPos, manaSize, nameplate.m_BarBackgroundColor, eid);
                    const f32 fill = glm::clamp(mana / maxMana, 0.0f, 1.0f);
                    const glm::vec2 fillSize = { manaSize.x * fill, manaSize.y };
                    if (fillSize.x > 0.0f)
                    {
                        UIRenderer::DrawRect(barPos, fillSize, nameplate.m_ManaBarColor, eid);
                    }
                }
            }
        }

        // Fullscreen video overlay (cutscenes / studio logos / splash screens) — drawn on
        // top of the scene and UI, letterboxed to the video's aspect with black bars.
        if (auto fullscreenPlayer = VideoSystem::GetFullscreenPlayer(); fullscreenPlayer && fullscreenPlayer->GetTexture().IsInitialized())
        {
            const f32 vw = static_cast<f32>(m_ViewportWidth);
            const f32 vh = static_cast<f32>(m_ViewportHeight);
            const f32 texW = static_cast<f32>(fullscreenPlayer->GetWidth());
            const f32 texH = static_cast<f32>(fullscreenPlayer->GetHeight());

            // Opaque black backdrop so non-covered regions read as letterbox bars.
            UIRenderer::DrawRect({ 0.0f, 0.0f }, { vw, vh }, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

            if (texW > 0.0f && texH > 0.0f)
            {
                const f32 scale = std::min(vw / texW, vh / texH);
                const glm::vec2 size = { texW * scale, texH * scale };
                const glm::vec2 pos = { (vw - size.x) * 0.5f, (vh - size.y) * 0.5f };
                UIRenderer::DrawRect(pos, size, fullscreenPlayer->GetTexture().GetTexture(), glm::vec4(1.0f));
            }
        }

        UIRenderer::EndScene();

        // Restore depth state
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
    }

    void Scene::RenderScene(EditorCamera const& camera)
    {
        Renderer2D::BeginScene(camera);

        // Draw sprites
        {
            for (const auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>(); const auto entity : view)
            {
                const auto& sprite = view.get<SpriteRendererComponent>(entity);

                Renderer2D::DrawSprite(GetWorldTransform(entity), sprite, static_cast<int>(std::to_underlying(entity)));
            }
        }

        // Draw circles
        {
            for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
            {
                const auto& circle = view.get<CircleRendererComponent>(entity);

                Renderer2D::DrawCircle(GetWorldTransform(entity), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(std::to_underlying(entity)));
            }
        }

        // Draw text
        {
            for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
            {
                const auto& text = view.get<TextComponent>(entity);
                DrawTextWithShadow(text, GetWorldTransform(entity), static_cast<int>(std::to_underlying(entity)));
            }
        }

        // Draw particles
        {
            for (auto view = m_Registry.view<ParticleSystemComponent>(); auto entity : view)
            {
                auto& psc = view.get<ParticleSystemComponent>(entity);
                auto& sys = psc.System;
                glm::vec3 offset = (sys.SimulationSpace == ParticleSpace::Local) ? sys.GetEmitterPosition() : glm::vec3(0.0f);

                const ModuleTextureSheetAnimation* sheet = sys.TextureSheetModule.Enabled ? &sys.TextureSheetModule : nullptr;

                SetParticleBlendMode(sys.BlendMode);
                ParticleRenderer::RenderParticles2D(sys.GetPool(), psc.Texture, offset, static_cast<int>(std::to_underlying(entity)), nullptr, sheet);

                for (sizet c = 0; c < psc.ChildSystems.size(); ++c)
                {
                    auto& childSys = psc.ChildSystems[c];
                    SetParticleBlendMode(childSys.BlendMode);
                    Ref<Texture2D> childTex = (c < psc.ChildTextures.size()) ? psc.ChildTextures[c] : nullptr;
                    ParticleRenderer::RenderParticles2D(childSys.GetPool(), childTex, offset, static_cast<int>(std::to_underlying(entity)), nullptr, nullptr);
                }

                RestoreDefaultBlendMode();
            }
        }

        Renderer2D::EndScene();
    }

    // Static cached default material for 3D rendering
    static Ref<Material> s_DefaultMaterial = nullptr;

    static Material& GetDefaultMaterial()
    {
        if (!s_DefaultMaterial)
        {
            s_DefaultMaterial = Material::CreatePBR("Default", glm::vec3(0.8f, 0.8f, 0.8f), 0.0f, 0.5f);
        }
        return *s_DefaultMaterial;
    }

    void Scene::LoadAndRenderSkybox()
    {
        OLO_PROFILE_FUNCTION();

        // Clear stale global IBL from previous scenes — only the current
        // scene's EnvironmentMap (if any) should provide IBL textures.
        Renderer3D::ClearGlobalIBL();

        // StarNestSkyComponent has the highest precedence: the explicit
        // raymarched nebula (issue #292) bakes its own cubemap and routes
        // through the same Renderer3D::DrawSkybox + IBL path as every other
        // sky, so reflective PBR surfaces / reflection probes / SSR all
        // reflect the nebula. Early-return after wiring it so the procedural
        // sky and file-based env-map loops below can't also fire.
        if (auto starView = m_Registry.view<StarNestSkyComponent>(); starView.begin() != starView.end())
        {
            for (auto entity : starView)
            {
                auto& sky = starView.get<StarNestSkyComponent>(entity);

                // Detect dirtiness via parameter hash; rebake only on change.
                // The bake is expensive (six raymarched face renders + IBL
                // convolve) so we gate on the hash rather than rebake per frame.
                StarNestParameters params;
                params.Offset = sky.m_Offset;
                params.Rotation1 = sky.m_Rotation1;
                params.Rotation2 = sky.m_Rotation2;
                params.Formuparam = sky.m_Formuparam;
                params.StepSize = sky.m_StepSize;
                params.Tile = sky.m_Tile;
                params.Brightness = sky.m_Brightness;
                params.DarkMatter = sky.m_DarkMatter;
                params.DistFading = sky.m_DistFading;
                params.Saturation = sky.m_Saturation;
                params.Intensity = sky.m_Intensity;
                params.Iterations = sky.m_Iterations;
                params.VolSteps = sky.m_VolSteps;

                const u64 hash = StarNestSky::HashParameters(params, sky.m_CubemapResolution);
                if (hash != sky.m_LastBakeHash)
                {
                    auto baked = StarNestSky::Generate(params, sky.m_CubemapResolution);
                    if (baked)
                        sky.m_EnvironmentMap = baked;
                    else
                        sky.m_EnvironmentMap.Reset(); // drop the stale bake so the cache matches the (failed) current params

                    // Record the attempt regardless of outcome: a persistent
                    // bake failure (e.g. shader missing) must not re-run the
                    // expensive Generate — and re-log its error — every frame.
                    // A real parameter edit moves the hash and re-triggers a
                    // bake; the editor "Force Rebake" resets m_LastBakeHash to 0
                    // (and clears m_EnvironmentMap) to force a retry on demand.
                    sky.m_LastBakeHash = hash;
                }

                if (!sky.m_EnvironmentMap)
                    continue;

                if (sky.m_EnableSkybox && sky.m_EnvironmentMap->GetEnvironmentMap())
                {
                    auto* packet = Renderer3D::DrawSkybox(sky.m_EnvironmentMap->GetEnvironmentMap());
                    if (packet)
                        Renderer3D::SubmitPacket(packet);
                }

                if (sky.m_EnableIBL && sky.m_EnvironmentMap->HasIBL())
                {
                    auto& envMap = sky.m_EnvironmentMap;
                    Renderer3D::SetGlobalIBL(
                        envMap->GetIrradianceMap() ? envMap->GetIrradianceMap()->GetRendererID() : 0,
                        envMap->GetPrefilterMap() ? envMap->GetPrefilterMap()->GetRendererID() : 0,
                        envMap->GetBRDFLutMap() ? envMap->GetBRDFLutMap()->GetRendererID() : 0,
                        envMap->GetEnvironmentMap() ? envMap->GetEnvironmentMap()->GetRendererID() : 0,
                        sky.m_IBLIntensity);
                }
                return; // Only one Star Nest sky drives the scene
            }
        }

        // ProceduralSkyComponent wins over EnvironmentMapComponent when both
        // live in the same scene: the procedural sky bakes its own cubemap
        // and routes through the same Renderer3D::DrawSkybox + IBL path that
        // the file-based environment map uses.  We early-return after wiring
        // the procedural sky so the loops can't both fire.
        if (auto procView = m_Registry.view<ProceduralSkyComponent>(); procView.begin() != procView.end())
        {
            for (auto entity : procView)
            {
                auto& sky = procView.get<ProceduralSkyComponent>(entity);

                // Time-of-day path: pull sun direction from the first
                // directional light in the scene each tick. Negation
                // converts "outgoing light direction" -> "toward sun".
                if (sky.m_LinkSunToDirectionalLight)
                {
                    auto dirView = m_Registry.view<DirectionalLightComponent>();
                    if (auto it = dirView.begin(); it != dirView.end())
                    {
                        const auto& dirLight = dirView.get<DirectionalLightComponent>(*it);
                        const glm::vec3 newDir = -dirLight.m_Direction;
                        if (std::isfinite(newDir.x) && std::isfinite(newDir.y) && std::isfinite(newDir.z) &&
                            glm::length(newDir) > 1e-4f)
                        {
                            sky.m_SunDirection = newDir;
                        }
                    }
                }

                // Ephemeral MCP sun-direction override (#316 Part 4): when an
                // agent has set a time-of-day / sun-angle via the diagnostics
                // server (olo_scene_set_time_of_day / olo_scene_set_sun_angle),
                // bake with that toward-sun direction instead of the component's
                // serialized value — WITHOUT writing m_SunDirection, so the
                // override stays ephemeral and a scene reload / play-stop /
                // server-stop / explicit clear restores the authored sun. Applied
                // after the directional-light link above so it also wins over it.
                glm::vec3 effectiveSunDirection = sky.m_SunDirection;
                if (Renderer3D::HasSunDirectionOverride())
                    effectiveSunDirection = Renderer3D::GetSunDirectionOverride();

                // Detect dirtiness via parameter hash; rebake on change. The
                // bake is expensive (six cubemap face renders + IBL convolve)
                // so we deliberately gate on the hash rather than rebake every
                // frame. Because the override feeds the same hashed params, a
                // changed (or cleared) override moves the hash and triggers
                // exactly one rebake — no per-frame rebake while it is steady.
                PreethamParameters params;
                params.SunDirection = effectiveSunDirection;
                params.Turbidity = sky.m_Turbidity;
                params.Exposure = sky.m_Exposure;
                params.SunIntensity = sky.m_SunIntensity;
                params.SunDiskSize = sky.m_SunDiskSize;
                params.ShowSunDisk = sky.m_ShowSunDisk;

                const u64 hash = ProceduralSky::HashParameters(params, sky.m_CubemapResolution);
                if (!sky.m_EnvironmentMap || hash != sky.m_LastBakeHash)
                {
                    auto baked = ProceduralSky::Generate(params, sky.m_CubemapResolution);
                    if (baked)
                    {
                        sky.m_EnvironmentMap = baked;
                        sky.m_LastBakeHash = hash;
                    }
                }

                if (!sky.m_EnvironmentMap)
                    continue;

                if (sky.m_EnableSkybox && sky.m_EnvironmentMap->GetEnvironmentMap())
                {
                    auto* packet = Renderer3D::DrawSkybox(sky.m_EnvironmentMap->GetEnvironmentMap());
                    if (packet)
                        Renderer3D::SubmitPacket(packet);
                }

                if (sky.m_EnableIBL && sky.m_EnvironmentMap->HasIBL())
                {
                    auto& envMap = sky.m_EnvironmentMap;
                    Renderer3D::SetGlobalIBL(
                        envMap->GetIrradianceMap() ? envMap->GetIrradianceMap()->GetRendererID() : 0,
                        envMap->GetPrefilterMap() ? envMap->GetPrefilterMap()->GetRendererID() : 0,
                        envMap->GetBRDFLutMap() ? envMap->GetBRDFLutMap()->GetRendererID() : 0,
                        envMap->GetEnvironmentMap() ? envMap->GetEnvironmentMap()->GetRendererID() : 0,
                        sky.m_IBLIntensity);
                }
                return; // Only one procedural sky drives the scene
            }
        }

        auto view = m_Registry.view<EnvironmentMapComponent>();
        for (auto entity : view)
        {
            auto& envMapComp = view.get<EnvironmentMapComponent>(entity);

            // Pull the editor-toggled flags onto an IBLConfiguration that
            // gets forwarded into the loader helpers. Defaults stay otherwise
            // identical to the previous behaviour; only fields the component
            // surfaces are overridden.
            IBLConfiguration iblConfig;
            iblConfig.UseSphericalHarmonics = envMapComp.m_UseSphericalHarmonics;

            // Lazy load environment map from file path if not already loaded
            if (!envMapComp.m_EnvironmentMap && !envMapComp.m_FilePath.empty())
            {
                if (envMapComp.m_IsCubemapFolder)
                {
                    // Load 6 cubemap face textures from folder
                    std::string basePath = envMapComp.m_FilePath;
                    if (!basePath.empty() && basePath.back() != '/' && basePath.back() != '\\')
                        basePath += '/';

                    std::vector<std::string> skyboxFaces = {
                        basePath + "right.jpg",
                        basePath + "left.jpg",
                        basePath + "top.jpg",
                        basePath + "bottom.jpg",
                        basePath + "front.jpg",
                        basePath + "back.jpg"
                    };

                    auto skyboxCubemap = TextureCubemap::Create(skyboxFaces);
                    if (skyboxCubemap)
                    {
                        envMapComp.m_EnvironmentMap = EnvironmentMap::CreateFromCubemap(skyboxCubemap, iblConfig);
                    }
                }
                else
                {
                    envMapComp.m_EnvironmentMap = EnvironmentMap::CreateFromEquirectangular(envMapComp.m_FilePath, iblConfig);
                }
            }

            if (!envMapComp.m_EnvironmentMap)
            {
                continue;
            }

            // Submit skybox draw if enabled
            if (envMapComp.m_EnableSkybox && envMapComp.m_EnvironmentMap->GetEnvironmentMap())
            {
                auto* skyboxPacket = Renderer3D::DrawSkybox(envMapComp.m_EnvironmentMap->GetEnvironmentMap());
                if (skyboxPacket)
                {
                    Renderer3D::SubmitPacket(skyboxPacket);
                }
            }

            // Wire IBL textures independently of skybox visibility
            if (envMapComp.m_EnableIBL && envMapComp.m_EnvironmentMap->HasIBL())
            {
                auto& envMap = envMapComp.m_EnvironmentMap;
                Renderer3D::SetGlobalIBL(
                    envMap->GetIrradianceMap() ? envMap->GetIrradianceMap()->GetRendererID() : 0,
                    envMap->GetPrefilterMap() ? envMap->GetPrefilterMap()->GetRendererID() : 0,
                    envMap->GetBRDFLutMap() ? envMap->GetBRDFLutMap()->GetRendererID() : 0,
                    envMap->GetEnvironmentMap() ? envMap->GetEnvironmentMap()->GetRendererID() : 0,
                    envMapComp.m_IBLIntensity);
            }
            else
            {
                Renderer3D::ClearGlobalIBL();
            }

            return; // Only use first environment map
        }
    }

    void Scene::ApplyReflectionProbeOverride(const glm::vec3& cameraPosition)
    {
        OLO_PROFILE_FUNCTION();

        // Collect probe geometry + parallel pointer array. The pure selection
        // function lives in ReflectionProbeBaker.h so it can be unit-tested
        // without a scene; this loop is just the runtime view-iteration side.
        auto view = m_Registry.view<TransformComponent, ReflectionProbeComponent>();

        std::vector<ReflectionProbeRef> probeRefs;
        std::vector<const ReflectionProbeComponent*> probePtrs;
        probeRefs.reserve(8);
        probePtrs.reserve(8);

        for (auto entity : view)
        {
            auto const& [transform, probe] = view.get<TransformComponent, ReflectionProbeComponent>(entity);
            if (!probe.m_Active || !probe.m_BakedEnvironment || !probe.m_BakedEnvironment->HasIBL())
            {
                continue;
            }
            probeRefs.push_back({ transform.Translation, probe.m_InfluenceRadius });
            probePtrs.push_back(&probe);
        }

        i32 const winner = SelectDominantReflectionProbe(cameraPosition, probeRefs);
        if (winner < 0)
        {
            return; // no probe applies — keep env-map IBL
        }

        auto const* bestProbe = probePtrs[static_cast<sizet>(winner)];
        auto const& envMap = bestProbe->m_BakedEnvironment;
        Renderer3D::SetGlobalIBL(
            envMap->GetIrradianceMap() ? envMap->GetIrradianceMap()->GetRendererID() : 0,
            envMap->GetPrefilterMap() ? envMap->GetPrefilterMap()->GetRendererID() : 0,
            envMap->GetBRDFLutMap() ? envMap->GetBRDFLutMap()->GetRendererID() : 0,
            envMap->GetEnvironmentMap() ? envMap->GetEnvironmentMap()->GetRendererID() : 0,
            bestProbe->m_Intensity);
    }

    // Helper: obtain the shadow VAO RendererID from a Mesh (returns 0 if unavailable).
    [[nodiscard]] static RendererID GetShadowVaoID(const Ref<Mesh>& mesh)
    {
        if (!mesh)
        {
            return 0;
        }
        if (auto const& ms = mesh->GetMeshSource(); ms && ms->HasShadowVertexArray())
        {
            return ms->GetShadowVertexArray()->GetRendererID();
        }
        return 0;
    }

    void Scene::ProcessScene3DSharedLogic(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                          const glm::vec3& cameraPosition,
                                          f32 cameraNearClip, f32 cameraFarClip)
    {
        OLO_PROFILE_FUNCTION();

        glm::mat4 viewProjection = projectionMatrix * viewMatrix;

        // Collect all scene lights into MultiLight UBO and set up shadows
        glm::vec3 directionalLightDir(0.0f, -1.0f, 0.0f);
        bool hasDirectionalShadow = false;
        {
            UBOStructures::MultiLightUBO multiLightData{};
            i32 lightIndex = 0;

            // Sanitize an authored spot-light direction once, reused by every
            // downstream consumer (Forward+ SSBO, MultiLight UBO, and the
            // spot-shadow projection). A zero-length or non-finite direction
            // would make glm::normalize emit NaNs; fall back to a safe -Z unit.
            // Valid directions pass through unchanged.
            const auto sanitizeSpotDir = [](const glm::vec3& dir) -> glm::vec3
            {
                const f32 len2 = glm::dot(dir, dir);
                if (!std::isfinite(len2) || len2 < 1e-8f)
                {
                    return glm::vec3(0.0f, 0.0f, -1.0f);
                }
                return dir;
            };

            // Collect directional lights
            auto dirLightView = m_Registry.view<TransformComponent, DirectionalLightComponent>();
            for (auto entity : dirLightView)
            {
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    break;
                }

                const auto& [transform, dirLight] = dirLightView.get<TransformComponent, DirectionalLightComponent>(entity);

                auto& data = multiLightData.Lights[lightIndex];
                data.Position = glm::vec4(dirLight.m_Direction, 0.0f);   // w=0 for directional
                data.Direction = glm::vec4(dirLight.m_Direction, -1.0f); // w=-1 = no shadow index
                data.Color = glm::vec4(dirLight.m_Color, dirLight.m_Intensity);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // type = DIRECTIONAL_LIGHT = 0

                // The first directional light drives the camera view position
                // (used by shading/specular) and the directional CSM shadow setup.
                if (lightIndex == 0)
                {
                    Renderer3D::SetViewPosition(cameraPosition);

                    directionalLightDir = dirLight.m_Direction;
                    hasDirectionalShadow = dirLight.m_CastShadows;

                    if (dirLight.m_CastShadows)
                    {
                        auto& shadowMap = Renderer3D::GetShadowMap();
                        auto settings = shadowMap.GetSettings();
                        settings.Bias = dirLight.m_ShadowBias;
                        settings.NormalBias = dirLight.m_ShadowNormalBias;
                        settings.MaxShadowDistance = dirLight.m_MaxShadowDistance;
                        settings.CascadeSplitLambda = dirLight.m_CascadeSplitLambda;
                        shadowMap.SetSettings(settings);
                        shadowMap.SetCascadeDebugEnabled(dirLight.m_CascadeDebugVisualization);
                    }
                }

                ++lightIndex;
            }

            // Record how many directional lights were collected (at the start of the array)
            const i32 directionalLightCount = lightIndex;

            // Forward+ tile culling consumes the same point/spot/sphere lights,
            // packed into typed SSBOs. Gather them in THIS single pass (rather
            // than a second scene iteration) and hand them to
            // TiledForwardPlus::SetLights below. Directional lights are not
            // culled, so they are not gathered here. These vectors are not
            // bounded by the MultiLightUBO MAX_LIGHTS cap — the cull buffer
            // clamps to its own (larger) capacity in LightCullingBuffer::Update.
            std::vector<GPUPointLight> fpPointLights;
            std::vector<GPUSpotLight> fpSpotLights;
            std::vector<GPUSphereAreaLight> fpSphereAreaLights;

            // Collect point lights
            u32 pointShadowIndex = 0;
            auto pointLightView = m_Registry.view<TransformComponent, PointLightComponent>();
            fpPointLights.reserve(pointLightView.size_hint());
            for (auto entity : pointLightView)
            {
                const auto& [transform, pointLight] = pointLightView.get<TransformComponent, PointLightComponent>(entity);

                // Forward+ SSBO entry (capacity clamped in LightCullingBuffer::Update)
                fpPointLights.push_back({ glm::vec4(transform.Translation, pointLight.m_Range),
                                          glm::vec4(pointLight.m_Color, pointLight.m_Intensity) });

                // MultiLightUBO entry (shared mixed-type array, capped at MAX_LIGHTS)
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    continue;
                }

                auto& data = multiLightData.Lights[lightIndex];
                data.Position = glm::vec4(transform.Translation, 1.0f); // w=1 for point
                data.Color = glm::vec4(pointLight.m_Color, pointLight.m_Intensity);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, pointLight.m_Attenuation, pointLight.m_Range);
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // type = POINT_LIGHT = 1

                // Encode point shadow index in direction.w (-1 = no shadow)
                if (pointLight.m_CastShadows && pointShadowIndex < ShadowMap::MAX_POINT_SHADOWS)
                {
                    data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, static_cast<f32>(pointShadowIndex));
                    ++pointShadowIndex;
                }
                else
                {
                    data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, -1.0f);
                }

                ++lightIndex;
            }

            // Collect spot lights
            u32 spotShadowIndex = 0;
            auto spotLightView = m_Registry.view<TransformComponent, SpotLightComponent>();
            fpSpotLights.reserve(spotLightView.size_hint());
            for (auto entity : spotLightView)
            {
                const auto& [transform, spotLight] = spotLightView.get<TransformComponent, SpotLightComponent>(entity);

                // Sanitized direction shared by the Forward+ SSBO, the
                // MultiLight UBO, and the spot-shadow projection below.
                const glm::vec3 spotDir = sanitizeSpotDir(spotLight.m_Direction);

                // Forward+ SSBO entry (capacity clamped in LightCullingBuffer::Update)
                fpSpotLights.push_back({ glm::vec4(transform.Translation, spotLight.m_Range),
                                         glm::vec4(glm::normalize(spotDir), glm::cos(glm::radians(spotLight.m_OuterCutoff))),
                                         glm::vec4(spotLight.m_Color, spotLight.m_Intensity),
                                         glm::vec4(glm::cos(glm::radians(spotLight.m_InnerCutoff)), spotLight.m_Attenuation, 0.0f, 0.0f) });

                // MultiLightUBO entry (shared mixed-type array, capped at MAX_LIGHTS)
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    continue;
                }

                auto& data = multiLightData.Lights[lightIndex];
                data.Position = glm::vec4(transform.Translation, 2.0f); // w=2 for spot
                data.Color = glm::vec4(spotLight.m_Color, spotLight.m_Intensity);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, spotLight.m_Attenuation, spotLight.m_Range);
                data.SpotParams = glm::vec4(
                    glm::cos(glm::radians(spotLight.m_InnerCutoff)),
                    glm::cos(glm::radians(spotLight.m_OuterCutoff)),
                    1.0f,
                    2.0f // type = SPOT_LIGHT = 2
                );

                // Encode spot shadow index in direction.w (-1 = no shadow)
                if (spotLight.m_CastShadows && spotShadowIndex < ShadowMap::MAX_SPOT_SHADOWS)
                {
                    data.Direction = glm::vec4(spotDir, static_cast<f32>(spotShadowIndex));
                    ++spotShadowIndex;
                }
                else
                {
                    data.Direction = glm::vec4(spotDir, -1.0f);
                }

                ++lightIndex;
            }

            // Collect sphere area lights. Packed into MultiLightData with the
            // SPHERE_AREA_LIGHT type tag (w=3) and the emitter sphere radius
            // stored in SpotParams.z — see PBRCommon.glsl for the decoder side.
            auto sphereAreaLightView = m_Registry.view<TransformComponent, SphereAreaLightComponent>();
            fpSphereAreaLights.reserve(sphereAreaLightView.size_hint());
            for (auto entity : sphereAreaLightView)
            {
                const auto& [transform, areaLight] = sphereAreaLightView.get<TransformComponent, SphereAreaLightComponent>(entity);

                // Forward+ SSBO entry (capacity clamped in LightCullingBuffer::Update)
                fpSphereAreaLights.push_back({ glm::vec4(transform.Translation, areaLight.m_Radius),
                                               glm::vec4(areaLight.m_Color, areaLight.m_Intensity),
                                               glm::vec4(areaLight.m_Range, 0.0f, 0.0f, 0.0f) });

                // MultiLightUBO entry (shared mixed-type array, capped at MAX_LIGHTS)
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    continue;
                }

                auto& data = multiLightData.Lights[lightIndex];
                data.Position = glm::vec4(transform.Translation, 3.0f); // w=3 for sphere area
                data.Color = glm::vec4(areaLight.m_Color, areaLight.m_Intensity);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, areaLight.m_Range);
                data.SpotParams = glm::vec4(0.0f, 0.0f, areaLight.m_Radius, 3.0f); // type = SPHERE_AREA_LIGHT = 3

                // Sphere area lights cast hard shadows by treating the emitter as a
                // point light at its centre (the representative point), so they
                // borrow a slot from the SAME point-light cubemap pool — hence the
                // shared pointShadowIndex counter, continued after the point lights
                // above. The shadow index rides in direction.w exactly like point
                // lights; the shader's POINT_LIGHT||SPHERE_AREA_LIGHT branch samples
                // u_ShadowMapPointN with it. The setup loop below registers the
                // matching cubemap via SetPointLightShadow at the same index.
                if (areaLight.m_CastShadows && pointShadowIndex < ShadowMap::MAX_POINT_SHADOWS)
                {
                    data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, static_cast<f32>(pointShadowIndex));
                    ++pointShadowIndex;
                }
                else
                {
                    data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, -1.0f); // no shadow
                }

                ++lightIndex;
            }

            multiLightData.LightCount = lightIndex;
            multiLightData.MaxLights = static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS);
            multiLightData.DirectionalLightCount = directionalLightCount;
            Renderer3D::UploadMultiLightUBO(multiLightData, lightIndex);

            // Publish the primary directional light direction for the
            // fog / atmospheric sun-direction derivation (previously carried by
            // the now-retired single-light SceneLight).
            Renderer3D::SetPrimaryDirectionalLightDirection(directionalLightDir);

            // Hand the point/spot/sphere lights gathered above to Forward+ for
            // tile-based culling (no second scene iteration).
            Renderer3D::GetForwardPlus().SetLights(fpPointLights, fpSpotLights, fpSphereAreaLights);

            // Upload light probe volume data if present and dirty
            {
                auto probeVolumeView = m_Registry.view<LightProbeVolumeComponent>();
                for (auto entity : probeVolumeView)
                {
                    auto& lpv = probeVolumeView.get<LightProbeVolumeComponent>(entity);
                    if (!lpv.m_Active || !lpv.m_Dirty)
                    {
                        continue;
                    }

                    ShaderBindingLayout::LightProbeVolumeUBO probeUBO{};
                    probeUBO.BoundsMin = glm::vec4(lpv.m_BoundsMin, 0.0f);
                    probeUBO.BoundsMax = glm::vec4(lpv.m_BoundsMax, 0.0f);
                    probeUBO.GridDimensions = glm::ivec4(lpv.m_Resolution, lpv.GetTotalProbeCount());
                    probeUBO.ProbeSpacing = glm::vec4(lpv.m_Spacing, lpv.m_Spacing, lpv.m_Spacing, 0.0f);
                    probeUBO.Intensity = lpv.m_Intensity;

                    if (lpv.m_BakedDataAsset != 0)
                    {
                        auto probeAsset = AssetManager::GetAsset<LightProbeVolumeAsset>(lpv.m_BakedDataAsset);
                        if (probeAsset && probeAsset->HasBakedData())
                        {
                            probeUBO.Enabled = 1;
                            auto dataSize = static_cast<u32>(probeAsset->CoefficientData.size() * sizeof(glm::vec4));
                            Renderer3D::UploadLightProbeData(probeUBO, probeAsset->CoefficientData.data(), dataSize);
                        }
                        else
                        {
                            probeUBO.Enabled = 0;
                            Renderer3D::UploadLightProbeData(probeUBO, nullptr, 0);
                        }
                    }
                    else
                    {
                        probeUBO.Enabled = 0;
                        Renderer3D::UploadLightProbeData(probeUBO, nullptr, 0);
                    }

                    lpv.m_Dirty = false;
                    break; // Only one active volume at a time
                }
            }

            // If no active+dirty volume was found, ensure probes are disabled
            // to prevent stale data on the GPU
            {
                bool anyActiveVolume = false;
                auto allVolumes = m_Registry.view<LightProbeVolumeComponent>();
                for (auto entity : allVolumes)
                {
                    auto const& vol = allVolumes.get<LightProbeVolumeComponent>(entity);
                    if (vol.m_Active)
                    {
                        anyActiveVolume = true;
                        break;
                    }
                }
                if (!anyActiveVolume)
                {
                    ShaderBindingLayout::LightProbeVolumeUBO disabledUBO{};
                    Renderer3D::UploadLightProbeData(disabledUBO, nullptr, 0);
                }
            }

            // Set up shadow mapping
            auto& shadowMap = Renderer3D::GetShadowMap();
            shadowMap.BeginFrame();

            // CSM for directional light
            if (hasDirectionalShadow && shadowMap.IsEnabled())
            {
                shadowMap.ComputeCSMCascades(
                    directionalLightDir,
                    viewMatrix,
                    projectionMatrix,
                    cameraNearClip,
                    cameraFarClip);
            }

            // Spot light shadows
            {
                u32 spotIdx = 0;
                for (auto entity : spotLightView)
                {
                    if (spotIdx >= ShadowMap::MAX_SPOT_SHADOWS)
                    {
                        break;
                    }

                    const auto& [transform, spotLight] = spotLightView.get<TransformComponent, SpotLightComponent>(entity);
                    if (!spotLight.m_CastShadows)
                    {
                        continue;
                    }

                    shadowMap.SetSpotLightShadow(
                        spotIdx,
                        transform.Translation,
                        sanitizeSpotDir(spotLight.m_Direction),
                        spotLight.m_OuterCutoff,
                        spotLight.m_Range);
                    ++spotIdx;
                }
                shadowMap.SetSpotShadowCount(static_cast<i32>(spotIdx));
            }

            // Point light shadows
            {
                u32 pointIdx = 0;
                for (auto entity : pointLightView)
                {
                    if (pointIdx >= ShadowMap::MAX_POINT_SHADOWS)
                    {
                        break;
                    }

                    const auto& [transform, pointLight] = pointLightView.get<TransformComponent, PointLightComponent>(entity);
                    if (!pointLight.m_CastShadows)
                    {
                        continue;
                    }

                    shadowMap.SetPointLightShadow(
                        pointIdx,
                        transform.Translation,
                        pointLight.m_Range);
                    ++pointIdx;
                }

                // Sphere area lights share the point-light cubemap shadow pool:
                // continue the SAME pointIdx so each casting area light gets a
                // cubemap at the index its direction.w was tagged with in the
                // light-upload loop above. Treated as a point at the sphere
                // centre with the area light's range as the cubemap far plane —
                // a hard representative-point shadow (soft penumbra from the
                // emitter radius is a Phase-2 follow-up).
                for (auto entity : sphereAreaLightView)
                {
                    if (pointIdx >= ShadowMap::MAX_POINT_SHADOWS)
                    {
                        break;
                    }

                    const auto& [transform, areaLight] =
                        sphereAreaLightView.get<TransformComponent, SphereAreaLightComponent>(entity);
                    if (!areaLight.m_CastShadows)
                    {
                        continue;
                    }

                    shadowMap.SetPointLightShadow(
                        pointIdx,
                        transform.Translation,
                        areaLight.m_Range);
                    ++pointIdx;
                }

                shadowMap.SetPointShadowCount(static_cast<i32>(pointIdx));
            }

            shadowMap.UploadUBO();

            // Shadow casters will be submitted during entity traversal below.
            // ShadowRenderPass::Execute() iterates them per cascade/face.
        }

        // Collect local fog volumes and upload to GPU
        {
            FogVolumesUBOData fogVolumes{};

            struct VolumeEntry
            {
                i32 priority;
                const TransformComponent* tc;
                const FogVolumeComponent* fv;
            };
            std::vector<VolumeEntry> entries;

            auto fogVolumeView = m_Registry.view<TransformComponent, FogVolumeComponent>();
            entries.reserve(fogVolumeView.size_hint());
            for (auto entity : fogVolumeView)
            {
                const auto& fogVol = fogVolumeView.get<FogVolumeComponent>(entity);
                if (!fogVol.m_Enabled)
                {
                    continue;
                }
                entries.push_back({ fogVol.m_Priority,
                                    &fogVolumeView.get<TransformComponent>(entity),
                                    &fogVol });
            }

            // Sort by priority (higher priority processed last for consistent blending)
            std::ranges::sort(entries, [](const VolumeEntry& a, const VolumeEntry& b)
                              { return a.priority < b.priority; });

            u32 volumeIdx = 0;
            for (const auto& entry : entries)
            {
                if (volumeIdx >= FogVolumesUBOData::MAX_FOG_VOLUMES)
                {
                    break;
                }

                auto& vol = fogVolumes.Volumes[volumeIdx];
                glm::mat4 worldTransform = entry.tc->GetTransform();
                vol.WorldToLocal = glm::inverse(worldTransform);
                vol.ColorAndDensity = glm::vec4(entry.fv->m_Color, entry.fv->m_Density);
                vol.ShapeAndFalloff = glm::vec4(
                    static_cast<f32>(std::to_underlying(entry.fv->m_Shape)),
                    entry.fv->m_FalloffDistance,
                    entry.fv->m_BlendWeight,
                    entry.fv->m_AffectTransparent ? 1.0f : 0.0f);
                vol.Extents = glm::vec4(entry.fv->m_Extents, 0.0f);
                ++volumeIdx;
            }

            fogVolumes.VolumeCount = glm::ivec4(static_cast<i32>(volumeIdx), 0, 0, 0);
            Renderer3D::UploadFogVolumes(fogVolumes);
        }

        // Initialize terrain chunks if needed and set up terrain rendering
        {
            ++m_TerrainFrameCounter;

            auto terrainView = m_Registry.view<TransformComponent, TerrainComponent>();
            for (auto entity : terrainView)
            {
                auto& terrain = terrainView.get<TerrainComponent>(entity);

                if (terrain.m_StreamingEnabled)
                {
                    // ── Streaming mode ──
                    if (terrain.m_NeedsRebuild)
                    {
                        if (!terrain.m_Streamer)
                        {
                            terrain.m_Streamer = Ref<TerrainStreamer>::Create();
                        }

                        TerrainStreamerConfig config;
                        config.TileWorldSize = terrain.m_TileWorldSize;
                        config.HeightScale = terrain.m_HeightScale;
                        config.TileResolution = terrain.m_TileResolution;
                        config.LoadRadius = terrain.m_StreamingLoadRadius;
                        config.MaxLoadedTiles = terrain.m_StreamingMaxTiles;
                        config.TessellationEnabled = terrain.m_TessellationEnabled;
                        config.TargetTriangleSize = terrain.m_TargetTriangleSize;
                        config.MorphRegion = terrain.m_MorphRegion;
                        config.TileDirectory = terrain.m_TileDirectory;
                        config.TileFilePattern = terrain.m_TileFilePattern;
                        terrain.m_Streamer->Initialize(config);

                        if (terrain.m_Material)
                        {
                            terrain.m_Streamer->SetMaterial(terrain.m_Material);
                        }

                        terrain.m_NeedsRebuild = false;
                    }

                    // Build / rebuild terrain material texture arrays
                    if (terrain.m_MaterialNeedsRebuild && terrain.m_Material && terrain.m_Material->GetLayerCount() > 0)
                    {
                        terrain.m_Material->BuildTextureArrays();
                        terrain.m_Material->LoadSplatmaps();
                        terrain.m_Streamer->SetMaterial(terrain.m_Material);
                        terrain.m_MaterialNeedsRebuild = false;
                    }

                    // Update streamer each frame
                    terrain.m_Streamer->Update(cameraPosition, m_TerrainFrameCounter);
                }
                else
                {
                    // ── Single-tile mode (original) ──
                    if (terrain.m_NeedsRebuild)
                    {
                        // Create TerrainData if not yet loaded
                        if (!terrain.m_TerrainData)
                        {
                            terrain.m_TerrainData = Ref<TerrainData>::Create();
                            if (!terrain.m_HeightmapPath.empty())
                            {
                                terrain.m_TerrainData->LoadFromFile(terrain.m_HeightmapPath);
                            }
                            else if (terrain.m_ProceduralEnabled)
                            {
                                TerrainGenerator::HeightParams params;
                                params.Resolution = terrain.m_ProceduralResolution;
                                params.Seed = terrain.m_ProceduralSeed;
                                params.Octaves = terrain.m_ProceduralOctaves;
                                params.Frequency = terrain.m_ProceduralFrequency;
                                params.Lacunarity = terrain.m_ProceduralLacunarity;
                                params.Persistence = terrain.m_ProceduralPersistence;
                                params.Shaping = terrain.m_HeightShaping;
                                params.ErosionIterations = terrain.m_ProceduralErosionIterations;
                                TerrainGenerator::GenerateHeightmap(*terrain.m_TerrainData, params);
                            }
                            else
                            {
                                terrain.m_TerrainData->CreateFlat(256, 0.0f);
                            }
                        }

                        // Build chunks + quadtree
                        if (!terrain.m_ChunkManager)
                        {
                            terrain.m_ChunkManager = Ref<TerrainChunkManager>::Create();
                        }
                        // Apply LOD config from component
                        terrain.m_ChunkManager->TessellationEnabled = terrain.m_TessellationEnabled;
                        auto& lodCfg = terrain.m_ChunkManager->GetQuadtree().GetConfig();
                        lodCfg.TargetTriangleSize = terrain.m_TargetTriangleSize;
                        lodCfg.MorphRegion = terrain.m_MorphRegion;

                        terrain.m_ChunkManager->GenerateAllChunks(
                            *terrain.m_TerrainData,
                            terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);

                        terrain.m_NeedsRebuild = false;
                        // The height field changed, so any auto-material splatmap
                        // (derived from height/slope) must be regenerated too.
                        terrain.m_AutoSplatNeedsRebuild = true;

                        // Keep collision in sync with the freshly (re)built height field
                        // when running (e.g. a script Regenerate() during play). In edit
                        // mode m_JoltScene is null, so this is a no-op there; the initial
                        // build at OnPhysics3DStart covers terrain that built before play.
                        if (m_JoltScene && m_JoltScene->IsInitialized())
                        {
                            Entity terrainEnt = { entity, this };
                            BuildTerrainCollisionBody(*m_JoltScene, terrainEnt, terrain);
                        }
                    }

                    // Build / rebuild terrain material texture arrays
                    if (terrain.m_MaterialNeedsRebuild && terrain.m_Material && terrain.m_Material->GetLayerCount() > 0)
                    {
                        terrain.m_Material->BuildTextureArrays();
                        terrain.m_Material->LoadSplatmaps();
                        terrain.m_MaterialNeedsRebuild = false;
                    }

                    // Auto-material: derive the splatmap from height/slope rules so
                    // procedurally generated terrain comes out textured (sand →
                    // grass → rock → snow) instead of a single flat layer. Runs
                    // after the material's texture arrays are built and whenever the
                    // height field or the rules change.
                    if (terrain.m_AutoMaterial && terrain.m_AutoSplatNeedsRebuild && terrain.m_TerrainData &&
                        terrain.m_TerrainData->GetResolution() > 0 && terrain.m_Material &&
                        terrain.m_Material->GetLayerCount() > 0 && !terrain.m_LayerRules.empty())
                    {
                        TerrainGenerator::GenerateSplatmap(
                            *terrain.m_Material, *terrain.m_TerrainData, terrain.m_LayerRules,
                            terrain.m_SplatmapGenResolution,
                            terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
                        terrain.m_AutoSplatNeedsRebuild = false;
                    }

                    // Run quadtree LOD selection each frame if tessellation is enabled
                    if (terrain.m_TessellationEnabled && terrain.m_ChunkManager && terrain.m_ChunkManager->IsBuilt())
                    {
                        terrain.m_ChunkManager->SelectVisibleChunks(
                            Renderer3D::GetViewFrustum(),
                            cameraPosition,
                            viewProjection,
                            static_cast<f32>(m_ViewportHeight));
                    }
                }

                // ── Voxel override layer ──
                if (terrain.m_VoxelEnabled)
                {
                    if (!terrain.m_VoxelOverride)
                    {
                        terrain.m_VoxelOverride = Ref<VoxelOverride>::Create();
                        terrain.m_VoxelOverride->Initialize(
                            terrain.m_WorldSizeX, terrain.m_WorldSizeZ,
                            terrain.m_HeightScale, terrain.m_VoxelSize);
                    }

                    // Rebuild dirty voxel meshes on main thread
                    MarchingCubes::RebuildDirtyMeshes(*terrain.m_VoxelOverride, terrain.m_VoxelMeshes);
                }
            }

            // ── Foliage instancing ──
            {
                auto foliageView = m_Registry.view<TransformComponent, TerrainComponent, FoliageComponent>();
                for (auto entity : foliageView)
                {
                    auto& terrain = foliageView.get<TerrainComponent>(entity);
                    auto& foliage = foliageView.get<FoliageComponent>(entity);

                    if (!foliage.m_Enabled || foliage.m_Layers.empty())
                        continue;

                    if (!foliage.m_Renderer)
                    {
                        foliage.m_Renderer = Ref<FoliageRenderer>::Create();
                        foliage.m_NeedsRebuild = true;
                    }

                    if (foliage.m_NeedsRebuild && terrain.m_TerrainData)
                    {
                        foliage.m_Renderer->GenerateInstances(
                            foliage.m_Layers,
                            *terrain.m_TerrainData,
                            terrain.m_Material.get(),
                            terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
                        foliage.m_NeedsRebuild = false;
                    }
                }
            }

            // Submit terrain + voxel command packets (sorted with other opaque geometry)
            // and shadow casters for terrain, voxel, and foliage.
            // Track previous-frame animation time so water/foliage/wind shaders can
            // reproject their on-surface displacement (Gerstner waves, wind sway) for
            // accurate per-fragment velocity output.
            const f32 animationTime = Time::GetTime();
            const f32 prevAnimationTime = (m_LastAnimationTime < 0.0f) ? animationTime : m_LastAnimationTime;
            m_LastAnimationTime = animationTime;
            {
                auto terrainShader = Renderer3D::GetTerrainPBRShader();
                auto voxelShader = Renderer3D::GetVoxelPBRShader();
                // Gate on AnyShadowsRequested() so caster lists stay empty when no
                // light casts shadows this frame — the ShadowRenderPass then early-outs
                // instead of re-submitting every caster ×N cascades/faces against stale
                // (identity) CSM matrices. See issue #522.
                const bool hasActiveShadows = Renderer3D::IsShadowPassAvailable() && Renderer3D::GetShadowMap().IsEnabled() &&
                                              Renderer3D::GetShadowMap().AnyShadowsRequested();

                auto terrainRenderView = m_Registry.view<TransformComponent, TerrainComponent>();
                for (auto entity : terrainRenderView)
                {
                    const auto& [transform, terrain] = terrainRenderView.get<TransformComponent, TerrainComponent>(entity);

                    bool hasMaterial = terrain.m_Material && terrain.m_Material->IsBuilt();

                    // Extract texture IDs for command packets
                    RendererID splatmapID = 0, splatmap1ID = 0;
                    RendererID albedoArrayID = 0, normalArrayID = 0, armArrayID = 0;
                    if (hasMaterial)
                    {
                        auto& mat = terrain.m_Material;
                        if (auto s0 = mat->GetSplatmap(0))
                            splatmapID = s0->GetRendererID();
                        if (auto s1 = mat->GetSplatmap(1))
                            splatmap1ID = s1->GetRendererID();
                        if (mat->GetAlbedoArray())
                            albedoArrayID = mat->GetAlbedoArray()->GetRendererID();
                        if (mat->GetNormalArray())
                            normalArrayID = mat->GetNormalArray()->GetRendererID();
                        if (mat->GetARMArray())
                            armArrayID = mat->GetARMArray()->GetRendererID();
                    }

                    i32 entityID = static_cast<i32>(std::to_underlying(entity));

                    if (terrainShader)
                    {
                        bool useTess = terrain.m_TessellationEnabled;

                        // Lambda to submit command packets for one chunk manager
                        auto submitChunkPackets = [&terrain, &transform, &splatmapID, &splatmap1ID,
                                                   &albedoArrayID, &normalArrayID, &armArrayID,
                                                   &hasMaterial, &useTess, &terrainShader, &entityID,
                                                   &hasActiveShadows](const TerrainChunkManager& chunkMgr,
                                                                      const TerrainData* terrainData,
                                                                      const TerrainMaterial* tileMaterial,
                                                                      f32 worldSizeX, f32 worldSizeZ,
                                                                      f32 heightScale)
                        {
                            if (!chunkMgr.IsBuilt())
                            {
                                return;
                            }

                            RendererID heightmapID = 0;
                            if (terrainData && terrainData->GetGPUHeightmap())
                            {
                                heightmapID = terrainData->GetGPUHeightmap()->GetRendererID();
                            }

                            // Per-tile material overrides entity material texture IDs
                            RendererID tileSplatmapID = splatmapID, tileSplatmap1ID = splatmap1ID;
                            RendererID tileAlbedoArrayID = albedoArrayID, tileNormalArrayID = normalArrayID, tileArmArrayID = armArrayID;
                            bool tileHasMaterial = tileMaterial && tileMaterial->IsBuilt();
                            if (tileHasMaterial && tileMaterial != terrain.m_Material.get())
                            {
                                if (auto s0 = tileMaterial->GetSplatmap(0))
                                    tileSplatmapID = s0->GetRendererID();
                                if (auto s1 = tileMaterial->GetSplatmap(1))
                                    tileSplatmap1ID = s1->GetRendererID();
                                if (tileMaterial->GetAlbedoArray())
                                    tileAlbedoArrayID = tileMaterial->GetAlbedoArray()->GetRendererID();
                                if (tileMaterial->GetNormalArray())
                                    tileNormalArrayID = tileMaterial->GetNormalArray()->GetRendererID();
                                if (tileMaterial->GetARMArray())
                                    tileArmArrayID = tileMaterial->GetARMArray()->GetRendererID();
                            }

                            // Build base terrain UBO (tess factors filled per-chunk)
                            ShaderBindingLayout::TerrainUBO terrainUBOData{};
                            terrainUBOData.WorldSizeAndHeightScale = glm::vec4(
                                worldSizeX, worldSizeZ,
                                heightScale,
                                static_cast<f32>(TerrainChunk::CHUNK_RESOLUTION));
                            u32 res = terrainData ? std::max<u32>(1, terrainData->GetResolution()) : 256;

                            const TerrainMaterial* effectiveMat = tileHasMaterial ? tileMaterial : (hasMaterial ? terrain.m_Material.get() : nullptr);
                            u32 layerCount = effectiveMat ? effectiveMat->GetLayerCount() : 0;
                            f32 triplanarSharpness = 8.0f;
                            terrainUBOData.TerrainParams = glm::vec4(
                                1.0f / static_cast<f32>(res),
                                1.0f / static_cast<f32>(res),
                                static_cast<f32>(layerCount),
                                triplanarSharpness);
                            terrainUBOData.HeightmapResolution = static_cast<i32>(res);

                            if (effectiveMat)
                            {
                                for (u32 i = 0; i < std::min(layerCount, 4u); ++i)
                                {
                                    terrainUBOData.LayerTilingScales0[i] = effectiveMat->GetLayer(i).TilingScale;
                                    terrainUBOData.LayerBlendSharpness0[i] = effectiveMat->GetLayer(i).HeightBlendSharpness;
                                }
                                for (u32 i = 4; i < std::min(layerCount, 8u); ++i)
                                {
                                    terrainUBOData.LayerTilingScales1[i - 4] = effectiveMat->GetLayer(i).TilingScale;
                                    terrainUBOData.LayerBlendSharpness1[i - 4] = effectiveMat->GetLayer(i).HeightBlendSharpness;
                                }
                            }

                            if (useTess)
                            {
                                const auto& selectedChunks = chunkMgr.GetSelectedChunks();
                                for (const auto& rc : selectedChunks)
                                {
                                    auto va = rc.Chunk->GetVertexArray();
                                    if (!va)
                                    {
                                        continue;
                                    }
                                    terrainUBOData.TessFactors = rc.LODData.TessFactors;
                                    terrainUBOData.TessFactors2 = rc.LODData.TessFactors2;
                                    terrainUBOData.TessFactors2.w = 1.0f;

                                    auto* packet = Renderer3D::DrawTerrainPatch(
                                        va->GetRendererID(), rc.Chunk->GetIndexCount(), 3,
                                        terrainShader,
                                        heightmapID, tileSplatmapID, tileSplatmap1ID,
                                        tileAlbedoArrayID, tileNormalArrayID, tileArmArrayID,
                                        transform.GetTransform(), terrainUBOData, entityID);
                                    if (packet)
                                        Renderer3D::SubmitPacket(packet);

                                    // Shadow caster for this chunk
                                    if (hasActiveShadows)
                                    {
                                        Renderer3D::AddTerrainShadowCaster(
                                            va->GetRendererID(), rc.Chunk->GetIndexCount(), 3,
                                            transform.GetTransform(), heightmapID, terrainUBOData);
                                    }
                                }
                            }
                            else
                            {
                                terrainUBOData.TessFactors = glm::vec4(1.0f);
                                terrainUBOData.TessFactors2.w = 1.0f;
                                std::vector<const TerrainChunk*> visibleChunks;
                                chunkMgr.GetVisibleChunks(Renderer3D::GetViewFrustum(), visibleChunks);
                                for (const auto* chunk : visibleChunks)
                                {
                                    auto va = chunk->GetVertexArray();
                                    if (!va)
                                    {
                                        continue;
                                    }

                                    auto* packet = Renderer3D::DrawTerrainPatch(
                                        va->GetRendererID(), chunk->GetIndexCount(), 3,
                                        terrainShader,
                                        heightmapID, tileSplatmapID, tileSplatmap1ID,
                                        tileAlbedoArrayID, tileNormalArrayID, tileArmArrayID,
                                        transform.GetTransform(), terrainUBOData, entityID);
                                    if (packet)
                                        Renderer3D::SubmitPacket(packet);

                                    if (hasActiveShadows)
                                    {
                                        Renderer3D::AddTerrainShadowCaster(
                                            va->GetRendererID(), chunk->GetIndexCount(), 3,
                                            transform.GetTransform(), heightmapID, terrainUBOData);
                                    }
                                }
                            }
                        };

                        if (terrain.m_StreamingEnabled && terrain.m_Streamer)
                        {
                            std::vector<Ref<TerrainTile>> readyTiles;
                            terrain.m_Streamer->GetReadyTiles(readyTiles);
                            for (const auto& tile : readyTiles)
                            {
                                auto tileMat = tile->GetMaterial();
                                auto chunkMgr = tile->GetChunkManager();
                                if (!chunkMgr)
                                {
                                    continue;
                                }
                                submitChunkPackets(*chunkMgr, tile->GetTerrainData().get(),
                                                   tileMat ? tileMat.get() : nullptr,
                                                   tile->WorldSizeX, tile->WorldSizeZ, tile->HeightScale);
                            }
                        }
                        else if (terrain.m_ChunkManager && terrain.m_ChunkManager->IsBuilt())
                        {
                            submitChunkPackets(*terrain.m_ChunkManager, terrain.m_TerrainData.get(),
                                               nullptr,
                                               terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
                        }
                        else
                        {
                            // No additional handling required.
                        }
                    } // if (terrainShader)

                    // Submit voxel mesh command packets
                    if (terrain.m_VoxelEnabled && !terrain.m_VoxelMeshes.empty() && voxelShader)
                    {
                        for (const auto& [coord, mesh] : terrain.m_VoxelMeshes)
                        {
                            if (mesh.VAO && mesh.IndexCount > 0)
                            {
                                auto* packet = Renderer3D::DrawVoxelMesh(
                                    mesh.VAO->GetRendererID(), mesh.IndexCount,
                                    voxelShader,
                                    albedoArrayID, normalArrayID, armArrayID,
                                    transform.GetTransform(), entityID);
                                if (packet)
                                    Renderer3D::SubmitPacket(packet);

                                if (hasActiveShadows)
                                {
                                    Renderer3D::AddVoxelShadowCaster(
                                        mesh.VAO->GetRendererID(), mesh.IndexCount,
                                        transform.GetTransform());
                                }
                            }
                        }
                    }

                    // Submit foliage shadow caster
                    if (hasActiveShadows && m_Registry.all_of<FoliageComponent>(entity))
                    {
                        auto& foliage = m_Registry.get<FoliageComponent>(entity);
                        if (foliage.m_Enabled && foliage.m_Renderer)
                        {
                            auto foliageDepthShader = Renderer3D::GetFoliageDepthShader();
                            if (foliageDepthShader)
                            {
                                Renderer3D::AddFoliageShadowCaster(
                                    foliage.m_Renderer.get(), foliageDepthShader, animationTime);
                            }
                        }
                    }
                }
            }

            // Submit foliage layer draw commands to the FoliageRenderPass command bucket
            {
                auto foliageRenderView = m_Registry.view<TransformComponent, TerrainComponent, FoliageComponent>();
                for (auto foliageEntity : foliageRenderView)
                {
                    auto const& [foliageTransform, fTerrain, foliage] = foliageRenderView.get<TransformComponent, TerrainComponent, FoliageComponent>(foliageEntity);
                    if (!foliage.m_Enabled || !foliage.m_Renderer)
                    {
                        continue;
                    }

                    foliage.m_Renderer->SetTime(animationTime, prevAnimationTime);
                    i32 entityID = static_cast<i32>(std::to_underlying(foliageEntity));
                    glm::mat4 modelMat = foliageTransform.GetTransform();

                    auto layerInfos = foliage.m_Renderer->GetActiveLayerDrawInfo();
                    for (const auto& layer : layerInfos)
                    {
                        auto* packet = Renderer3D::DrawFoliageLayer(
                            layer.VertexArrayID, layer.IndexCount, layer.InstanceCount,
                            layer.AlbedoTextureID,
                            modelMat,
                            animationTime,
                            prevAnimationTime,
                            layer.WindStrength, layer.WindSpeed,
                            layer.ViewDistance, layer.FadeStartDistance, layer.AlphaCutoff,
                            glm::vec4(layer.BaseColor, 0.0f),
                            layer.Bounds,
                            entityID);
                        if (packet)
                        {
                            Renderer3D::SubmitFoliagePacket(packet);
                        }
                    }
                }
            }

            // Submit water surface draw commands to the WaterRenderPass command bucket
            {
                // Planar reflections use a single global reflection plane per
                // frame (first slice). Track the largest reflective water surface
                // as the dominant plane and forward it to
                // PlanarReflectionRenderPass after the loop.
                glm::vec4 planarReflectionPlane{ 0.0f, 1.0f, 0.0f, 0.0f };
                bool planarReflectionActive = false;
                f32 planarReflectionIntensity = 1.0f;
                f32 planarReflectionDistortion = 0.02f;
                f32 bestPlanarArea = -1.0f;

                auto waterView = m_Registry.view<TransformComponent, WaterComponent>();
                for (auto entity : waterView)
                {
                    auto const& [transform, water] = waterView.get<TransformComponent, WaterComponent>(entity);
                    if (!water.m_Enabled)
                    {
                        continue;
                    }

                    // Sanitize the configured surface extents once — clamp to the
                    // mesh-build range so both the generated grid and the planar-
                    // reflection area metric below agree on the surface size. A raw
                    // out-of-range value would otherwise build a clamped mesh but
                    // compare an un-clamped area when picking the dominant reflector.
                    auto const sizeX = std::isfinite(water.m_WorldSizeX)
                                           ? std::clamp(water.m_WorldSizeX, 0.1f, 10000.0f)
                                           : 100.0f;
                    auto const sizeZ = std::isfinite(water.m_WorldSizeZ)
                                           ? std::clamp(water.m_WorldSizeZ, 0.1f, 10000.0f)
                                           : 100.0f;

                    // Lazy mesh initialization / rebuild
                    if (water.m_NeedsRebuild || !water.m_WaterMesh)
                    {
                        const u32 resX = std::clamp(water.m_GridResolutionX, 1u, 1024u);
                        const u32 resZ = std::clamp(water.m_GridResolutionZ, 1u, 1024u);
                        water.m_WaterMesh = MeshPrimitives::CreateWaterGrid(
                            sizeX, sizeZ,
                            resX, resZ);
                        water.m_NeedsRebuild = false;
                    }

                    if (!water.m_WaterMesh || !water.m_WaterMesh->IsValid())
                    {
                        continue;
                    }

                    const auto& submesh = water.m_WaterMesh->GetSubmesh();
                    auto va = water.m_WaterMesh->GetVertexArray();
                    if (!va)
                    {
                        continue;
                    }

                    i32 entityID = static_cast<i32>(std::to_underlying(entity));
                    glm::mat4 modelMat = transform.GetTransform();

                    // Planar reflection: the largest reflective surface wins the
                    // single global reflection plane. Derive the plane from the
                    // FULL water transform so a rotated / scaled water entity
                    // mirrors across its true world surface — the grid is built
                    // in XZ with +Y up, so the world surface normal is the
                    // transformed up axis (modelMat column 1) and the surface
                    // centre is the translation (column 3). The "largest surface"
                    // comparison uses the transformed in-plane extents, not the
                    // raw component sizes. Float-validate so a NaN / degenerate
                    // transform can't poison the mirror matrices.
                    if (water.m_PlanarReflectionsEnabled)
                    {
                        const glm::vec3 surfaceNormal(modelMat[1]);
                        const glm::vec3 surfaceCenter(modelMat[3]);
                        const f32 normalLenSq = glm::dot(surfaceNormal, surfaceNormal);
                        const f32 area = std::abs(sizeX * glm::length(glm::vec3(modelMat[0]))) *
                                         std::abs(sizeZ * glm::length(glm::vec3(modelMat[2])));
                        const glm::vec3 n = surfaceNormal * glm::inversesqrt(normalLenSq);
                        const glm::vec4 candidatePlane(n, -glm::dot(n, surfaceCenter));
                        const bool planeFinite = std::isfinite(normalLenSq) && normalLenSq > 1e-12f &&
                                                 std::isfinite(candidatePlane.w) && std::isfinite(area);
                        if (planeFinite && area > bestPlanarArea)
                        {
                            bestPlanarArea = area;
                            planarReflectionPlane = candidatePlane;
                            planarReflectionActive = true;
                            planarReflectionIntensity = std::isfinite(water.m_PlanarReflectionIntensity)
                                                            ? std::clamp(water.m_PlanarReflectionIntensity, 0.0f, 1.0f)
                                                            : 1.0f;
                            planarReflectionDistortion = std::isfinite(water.m_PlanarReflectionDistortion)
                                                             ? std::clamp(water.m_PlanarReflectionDistortion, 0.0f, 0.25f)
                                                             : 0.02f;
                        }
                    }

                    // Pack component fields into WaterDrawParams
                    Renderer3D::WaterDrawParams waterParams;
                    waterParams.waveParams = glm::vec4(
                        0.0f, // Time — filled by DrawWaterSurface
                        water.m_WaveSpeed,
                        water.m_WaveAmplitude,
                        water.m_WaveFrequency);
                    waterParams.waveDir0 = water.PackWaveDir0();
                    waterParams.waveDir1 = water.PackWaveDir1();
                    waterParams.waterColor = glm::vec4(
                        water.m_WaterColor, water.m_Transparency);
                    waterParams.waterDeepColor = glm::vec4(
                        water.m_DeepColor, water.m_Reflectivity);
                    waterParams.visualParams = glm::vec4(
                        water.m_FresnelPower, water.m_SpecularIntensity,
                        water.m_NormalMapTiling, water.m_NoiseIntensity);

                    // Pack normal map scroll offsets (dir * time * speed)
                    // Normalize scroll directions so magnitude doesn't affect speed
                    auto safeNorm2 = [](glm::vec2 const& v, glm::vec2 const& fallback) -> glm::vec2
                    {
                        if (!std::isfinite(v.x) || !std::isfinite(v.y))
                            return fallback;
                        if (auto const len2 = glm::dot(v, v); len2 > 1e-6f)
                            return v / std::sqrt(len2);
                        return fallback;
                    };
                    glm::vec2 dir0 = safeNorm2(water.m_NormalMapScrollDir0, { 1.0f, 0.0f });
                    glm::vec2 dir1 = safeNorm2(water.m_NormalMapScrollDir1, { 0.0f, 1.0f });
                    // Clamp scroll speeds before computing offsets — raw deserialized values could be huge
                    f32 const speed0 = std::isfinite(water.m_NormalMapScrollSpeed0)
                                           ? std::clamp(water.m_NormalMapScrollSpeed0, 0.0f, 1.0f)
                                           : 0.02f;
                    f32 const speed1 = std::isfinite(water.m_NormalMapScrollSpeed1)
                                           ? std::clamp(water.m_NormalMapScrollSpeed1, 0.0f, 1.0f)
                                           : 0.015f;
                    glm::vec2 scroll0 = dir0 * animationTime * speed0;
                    glm::vec2 scroll1 = dir1 * animationTime * speed1;
                    waterParams.normalMapScroll = glm::vec4(scroll0.x, scroll0.y, scroll1.x, scroll1.y);
                    waterParams.normalMapSpeed = glm::vec4(speed0, speed1, 0.0f, 0.0f);
                    // Validate light direction: fallback to down if non-finite or zero-length
                    glm::vec3 safeLightDir = directionalLightDir;
                    if (!std::isfinite(safeLightDir.x) || !std::isfinite(safeLightDir.y) || !std::isfinite(safeLightDir.z) || glm::dot(safeLightDir, safeLightDir) < 1e-6f)
                    {
                        safeLightDir = glm::vec3(0.0f, -1.0f, 0.0f);
                    }
                    waterParams.lightDirection = glm::vec4(glm::normalize(safeLightDir), 0.0f);

                    // Depth, refraction, foam, SSS params
                    waterParams.depthRefractionParams = glm::vec4(
                        water.m_DepthSofteningDistance,
                        water.m_RefractionDistortion,
                        water.m_RefractionHeightFactor, 0.0f);
                    waterParams.refractionColor = glm::vec4(water.m_RefractionColor, 0.0f);
                    waterParams.foamParams = glm::vec4(
                        water.m_FoamHeightStart, water.m_FoamFadeDistance,
                        water.m_FoamTiling, water.m_FoamBrightness);
                    waterParams.foamParams2 = glm::vec4(
                        water.m_FoamAngleExponent, water.m_ShorelineFoamPower,
                        water.m_SSSIntensity, 0.0f);
                    waterParams.sssColor = glm::vec4(water.m_SSSColor, 0.0f);

                    // SSR params: x=maxSteps (0=disabled), y=stepSize, z=maxDistance, w=thickness
                    waterParams.ssrParams = glm::vec4(
                        water.m_SSREnabled ? water.m_SSRMaxSteps : 0.0f,
                        water.m_SSRStepSize,
                        water.m_SSRMaxDistance,
                        water.m_SSRThickness);

                    // Tessellation params: x=factor (0=disabled), y=minDist,
                    // z=maxDist, w=frustumCullEnable (1.0=on, 0.0=off legacy).
                    // The TCS frustum-cull path skips Gerstner-displaced
                    // patches that lie wholly outside the view frustum; it's
                    // always on at the moment, but the channel is exposed so
                    // the C++ side can disable it without recompiling the
                    // shader if a future debug toggle wants it.
                    waterParams.tessParams = glm::vec4(
                        water.m_TessellationEnabled ? water.m_TessellationFactor : 0.0f,
                        water.m_TessMinDistance,
                        water.m_TessMaxDistance, 1.0f);

                    // Feature toggles
                    waterParams.refractionEnabled = water.m_RefractionEnabled;
                    waterParams.ssrEnabled = water.m_SSREnabled;
                    // Double-sided when enabled; the shader's per-fragment
                    // waterline discard keeps the correct side (§7.2).
                    waterParams.renderFromBelow = water.m_RenderFromBelow;

                    // Sanitize all scalar UBO fields — defence-in-depth against NaN/Inf reaching the GPU
                    auto const safeF = [](f32 v, f32 fallback)
                    { return std::isfinite(v) ? v : fallback; };
                    auto const clampF = [](f32 v, f32 lo, f32 hi, f32 fallback)
                    { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
                    auto const safeV3 = [&clampF](glm::vec4& v, glm::vec3 const& fb)
                    { v.x = clampF(v.x, 0.0f, 1.0f, fb.x); v.y = clampF(v.y, 0.0f, 1.0f, fb.y); v.z = clampF(v.z, 0.0f, 1.0f, fb.z); };

                    // Wave params (y=speed, z=amplitude, w=frequency)
                    waterParams.waveParams.y = clampF(waterParams.waveParams.y, 0.0f, 100.0f, 1.0f);
                    waterParams.waveParams.z = clampF(waterParams.waveParams.z, 0.0f, 100.0f, 0.5f);
                    waterParams.waveParams.w = clampF(waterParams.waveParams.w, 0.0f, 100.0f, 1.0f);
                    // Visual params (x=fresnel, y=specular, z=tiling, w=noise)
                    waterParams.visualParams.x = clampF(waterParams.visualParams.x, 0.1f, 20.0f, 5.0f);
                    waterParams.visualParams.y = clampF(waterParams.visualParams.y, 0.0f, 10.0f, 1.0f);
                    waterParams.visualParams.z = clampF(waterParams.visualParams.z, 0.0f, 50.0f, 1.0f);
                    waterParams.visualParams.w = clampF(waterParams.visualParams.w, 0.0f, 1.0f, 0.3f);
                    // Normal map scroll/speed
                    waterParams.normalMapScroll.x = safeF(waterParams.normalMapScroll.x, 0.0f);
                    waterParams.normalMapScroll.y = safeF(waterParams.normalMapScroll.y, 0.0f);
                    waterParams.normalMapScroll.z = safeF(waterParams.normalMapScroll.z, 0.0f);
                    waterParams.normalMapScroll.w = safeF(waterParams.normalMapScroll.w, 0.0f);
                    waterParams.normalMapSpeed.x = clampF(waterParams.normalMapSpeed.x, 0.0f, 1.0f, 0.02f);
                    waterParams.normalMapSpeed.y = clampF(waterParams.normalMapSpeed.y, 0.0f, 1.0f, 0.015f);
                    // Packed wave directions (x,y=dir validated as pair, z=steepness, w=wavelength)
                    auto safeDir2 = [](glm::vec4& v, f32 fbX, f32 fbY)
                    {
                        if (!std::isfinite(v.x) || !std::isfinite(v.y) || (v.x * v.x + v.y * v.y) < 1e-6f)
                        {
                            v.x = fbX;
                            v.y = fbY;
                        }
                    };
                    safeDir2(waterParams.waveDir0, 1.0f, 0.0f);
                    waterParams.waveDir0.z = clampF(waterParams.waveDir0.z, 0.0f, 1.0f, 0.5f);
                    waterParams.waveDir0.w = clampF(waterParams.waveDir0.w, 0.1f, 500.0f, 10.0f);
                    safeDir2(waterParams.waveDir1, 0.7f, 0.7f);
                    waterParams.waveDir1.z = clampF(waterParams.waveDir1.z, 0.0f, 1.0f, 0.3f);
                    waterParams.waveDir1.w = clampF(waterParams.waveDir1.w, 0.1f, 500.0f, 15.0f);
                    // Colors (RGB + alpha channels: transparency and reflectivity)
                    safeV3(waterParams.waterColor, { 0.1f, 0.4f, 0.5f });
                    waterParams.waterColor.w = clampF(waterParams.waterColor.w, 0.0f, 1.0f, 0.6f);
                    safeV3(waterParams.waterDeepColor, { 0.0f, 0.1f, 0.2f });
                    waterParams.waterDeepColor.w = clampF(waterParams.waterDeepColor.w, 0.0f, 1.0f, 0.5f);
                    safeV3(waterParams.refractionColor, { 0.0f, 0.05f, 0.1f });
                    safeV3(waterParams.sssColor, { 0.0f, 0.5f, 0.4f });
                    // Depth/refraction
                    waterParams.depthRefractionParams.x = clampF(waterParams.depthRefractionParams.x, 0.0f, 50.0f, 2.0f);
                    waterParams.depthRefractionParams.y = clampF(waterParams.depthRefractionParams.y, 0.0f, 0.5f, 0.05f);
                    waterParams.depthRefractionParams.z = clampF(waterParams.depthRefractionParams.z, 0.0f, 2.0f, 0.5f);
                    // Foam
                    waterParams.foamParams.x = clampF(waterParams.foamParams.x, 0.0f, 2.0f, 0.3f);
                    waterParams.foamParams.y = clampF(waterParams.foamParams.y, 0.01f, 5.0f, 0.5f);
                    waterParams.foamParams.z = clampF(waterParams.foamParams.z, 0.0f, 50.0f, 2.0f);
                    waterParams.foamParams.w = clampF(waterParams.foamParams.w, 0.0f, 5.0f, 1.5f);
                    waterParams.foamParams2.x = clampF(waterParams.foamParams2.x, 0.1f, 10.0f, 2.0f);
                    waterParams.foamParams2.y = clampF(waterParams.foamParams2.y, 0.1f, 10.0f, 3.0f);
                    waterParams.foamParams2.z = clampF(waterParams.foamParams2.z, 0.0f, 5.0f, 0.5f);
                    // SSR (x=maxSteps, y=stepSize, z=maxDistance, w=thickness)
                    waterParams.ssrParams.x = clampF(waterParams.ssrParams.x, 0.0f, 256.0f, 64.0f);
                    waterParams.ssrParams.y = clampF(waterParams.ssrParams.y, 0.01f, 1.0f, 0.1f);
                    waterParams.ssrParams.z = clampF(waterParams.ssrParams.z, 1.0f, 200.0f, 50.0f);
                    waterParams.ssrParams.w = clampF(waterParams.ssrParams.w, 0.01f, 5.0f, 0.5f);
                    // Tessellation (x=factor, y=minDist, z=maxDist)
                    waterParams.tessParams.x = clampF(waterParams.tessParams.x, 0.0f, 64.0f, 0.0f);
                    waterParams.tessParams.y = clampF(waterParams.tessParams.y, 1.0f, 500.0f, 10.0f);
                    waterParams.tessParams.z = clampF(waterParams.tessParams.z, 10.0f, 1000.0f, 200.0f);
                    waterParams.tessParams.z = std::max(waterParams.tessParams.z, waterParams.tessParams.y + 1.0f);

                    // Resolve texture IDs — only assign non-zero renderer IDs (skip placeholders)
                    if (water.m_NormalMap0 != 0)
                    {
                        if (auto tex = AssetManager::GetAsset<Texture2D>(water.m_NormalMap0))
                        {
                            if (auto id = tex->GetRendererID(); id != 0)
                                waterParams.normalMap0ID = id;
                        }
                    }
                    if (water.m_NormalMap1 != 0)
                    {
                        if (auto tex = AssetManager::GetAsset<Texture2D>(water.m_NormalMap1))
                        {
                            if (auto id = tex->GetRendererID(); id != 0)
                                waterParams.normalMap1ID = id;
                        }
                    }
                    if (water.m_NoiseTexture != 0)
                    {
                        if (auto tex = AssetManager::GetAsset<Texture2D>(water.m_NoiseTexture))
                        {
                            if (auto id = tex->GetRendererID(); id != 0)
                                waterParams.noiseTextureID = id;
                        }
                    }
                    if (water.m_FoamTexture != 0)
                    {
                        if (auto tex = AssetManager::GetAsset<Texture2D>(water.m_FoamTexture))
                        {
                            if (auto id = tex->GetRendererID(); id != 0)
                                waterParams.foamTextureID = id;
                        }
                    }

                    // FFT ocean (WATER_FUTURE_IMPROVEMENTS.md §1). When enabled,
                    // (re)evaluate the Tessendorf spectral field and hand its
                    // displacement/normal textures to the water shader; otherwise
                    // the analytic Gerstner path runs (fftParams.x stays 0).
                    if (water.m_UseFFT)
                    {
                        if (!water.m_OceanField)
                            water.m_OceanField = Ref<Ocean::OceanFFTField>::Create();

                        // Snap the grid to a power of two (the FFT requires it).
                        u32 fftRes = std::clamp(water.m_FFTResolution, 16u, 512u);
                        u32 pow2 = 16u;
                        while (pow2 * 2u <= fftRes)
                            pow2 *= 2u;

                        Ocean::SpectrumParams sp;
                        sp.m_Resolution = pow2;
                        sp.m_PatchSize = clampF(water.m_FFTPatchSize, 1.0f, 5000.0f, 80.0f);
                        sp.m_WindSpeed = clampF(water.m_FFTWindSpeed, 0.1f, 100.0f, 18.0f);
                        glm::vec2 windDir = water.m_FFTWindDirection;
                        if (!std::isfinite(windDir.x) || !std::isfinite(windDir.y) ||
                            (windDir.x * windDir.x + windDir.y * windDir.y) < 1e-6f)
                            windDir = glm::vec2(1.0f, 0.0f);
                        sp.m_WindDirection = windDir;
                        sp.m_Amplitude = clampF(water.m_FFTAmplitude, 0.0f, 100.0f, 2.0f);
                        sp.m_Choppiness = clampF(water.m_FFTChoppiness, 0.0f, 5.0f, 1.2f);
                        sp.m_Seed = water.m_FFTSeed;
                        // Spectrum selection (§1.4): Phillips or fetch-limited JONSWAP.
                        sp.m_SpectrumType = water.m_FFTSpectrumType;
                        sp.m_JonswapGamma = clampF(water.m_FFTJonswapGamma, 1.0f, 10.0f, 3.3f);
                        sp.m_JonswapFetch = clampF(water.m_FFTJonswapFetch, 1.0f, 1.0e6f, 100000.0f);

                        water.m_OceanField->Update(sp, animationTime, /*uploadToGpu=*/true,
                                                   /*useGpuCompute=*/water.m_FFTUseGpuCompute);

                        const u32 dispID = water.m_OceanField->GetDisplacementTextureID();
                        const u32 derivID = water.m_OceanField->GetDerivativesTextureID();
                        if (dispID != 0 && derivID != 0)
                        {
                            waterParams.fftDisplacementID = dispID;
                            waterParams.fftDerivativesID = derivID;
                            const f32 invPatch = 1.0f / sp.m_PatchSize;
                            // x = enabled, y = 1/patchSize (UV scale), z = heightScale,
                            // w = horizontalScale (choppiness is already baked into the
                            // texture's dx/dz, so keep this at 1).
                            waterParams.fftParams = glm::vec4(
                                1.0f, invPatch, WaterSurface::ClampFFTHeightScale(water.m_FFTHeightScale), 1.0f);
                            // FFT crests can exceed the Gerstner-derived TCS cull
                            // margin; disable the per-patch frustum cull so off-screen-
                            // edge crests aren't clipped early.
                            waterParams.tessParams.w = 0.0f;
                        }
                    }

                    // Compute bounding box for frustum culling — use sanitized values
                    f32 const safeWorldX = std::isfinite(water.m_WorldSizeX)
                                               ? std::clamp(water.m_WorldSizeX, 0.1f, 10000.0f)
                                               : 100.0f;
                    f32 const safeWorldZ = std::isfinite(water.m_WorldSizeZ)
                                               ? std::clamp(water.m_WorldSizeZ, 0.1f, 10000.0f)
                                               : 100.0f;
                    f32 const safeAmplitude = std::isfinite(water.m_WaveAmplitude)
                                                  ? std::clamp(water.m_WaveAmplitude, 0.0f, 100.0f)
                                                  : 0.5f;
                    f32 halfX = safeWorldX * 0.5f;
                    f32 halfZ = safeWorldZ * 0.5f;
                    // Conservative vertical extent for frustum culling. The FFT
                    // field is RMS-normalised to ~0.3·amplitude m and the shader
                    // scales it by m_FFTHeightScale; crests reach a few × RMS, so
                    // 2·(amplitude·heightScale) is a safe upper bound that tracks
                    // the actual displacement (with a small floor so a tiny
                    // amplitude still leaves a usable band).
                    f32 waveH;
                    if (water.m_UseFFT)
                    {
                        const f32 fftAmp = clampF(water.m_FFTAmplitude, 0.0f, 100.0f, 2.0f);
                        const f32 fftHeightScale = WaterSurface::ClampFFTHeightScale(water.m_FFTHeightScale);
                        waveH = std::max(fftAmp * fftHeightScale * 2.0f, 3.0f);
                    }
                    else
                    {
                        waveH = safeAmplitude * 2.0f;
                    }
                    BoundingBox bounds;
                    bounds.Min = glm::vec3(-halfX, -waveH, -halfZ);
                    bounds.Max = glm::vec3(halfX, waveH, halfZ);

                    auto* packet = Renderer3D::DrawWaterSurface(
                        va->GetRendererID(), submesh.m_IndexCount,
                        modelMat,
                        animationTime,
                        prevAnimationTime,
                        waterParams,
                        bounds,
                        entityID);
                    if (packet)
                    {
                        Renderer3D::SubmitWaterPacket(packet);
                    }
                }

                // Hand the dominant reflective surface (or "disabled") to the
                // planar reflection pass. The pass / EndScene handoff gates this
                // to the forward path and refreshes the binding-43 enable flag
                // every frame, so a disabled state can never leave a stale mirror.
                Renderer3D::SetPlanarReflectionState(planarReflectionPlane,
                                                     planarReflectionActive,
                                                     planarReflectionIntensity,
                                                     planarReflectionDistortion);
            }

            // Underwater fog (WATER_FUTURE_IMPROVEMENTS.md §7.2). The tone-map
            // pass fogs each pixel by the length of its view ray that passes
            // below the water plane, so the waterline is handled per pixel. Here
            // we just pick the relevant water volume and hand over its plane /
            // fog params + the reconstruction matrix.
            //
            // Activation is gated to at/below the waterline (within a wave-height
            // margin above the surface). Above that, the water surface's own
            // reflection / refraction owns the look — fogging there would wrongly
            // darken the sky reflection seen from above.
            {
                UnderwaterFogState underwater{};
                f32 bestSurfaceDist = std::numeric_limits<f32>::max();
                // Refraction (§7.2) + caustics (§7.1) params from the winning water
                // volume, sanitized here so the tone-map UBO never carries NaN/Inf.
                // Defaults match WaterComponent so an old scene without these fields
                // still looks right. vec4 packing matches UnderwaterFogUBOData.
                glm::vec4 refractionParams(0.0f);          // strength, scale, speed, chromatic
                glm::vec4 causticParams(0.0f);             // intensity, scale, speed, maxDepth
                glm::vec3 causticColor(0.7f, 0.85f, 1.0f); // caustic light tint
                // God rays (§3.3): intensity, decay, density, weight + sample count
                // + tint. Captured from the winning volume; intensity 0 disables.
                glm::vec4 godRayParams(0.0f);             // intensity, decay, density, weight
                glm::vec3 godRayColor(1.0f, 0.95f, 0.8f); // warm shaft tint
                f32 godRaySamples = 48.0f;                // radial-blur step count (as float for the UBO)
                glm::vec2 godRayShape(0.35f, 16.0f);      // dappleFloor, sunFalloff
                auto sanitizeParam = [](f32 v, f32 lo, f32 hi, f32 fallback) -> f32
                {
                    if (!std::isfinite(v))
                        return fallback;
                    return std::clamp(v, lo, hi);
                };
                auto waterView = m_Registry.view<TransformComponent, WaterComponent>();
                for (auto entity : waterView)
                {
                    auto const& [transform, water] = waterView.get<TransformComponent, WaterComponent>(entity);
                    if (!water.m_Enabled)
                        continue;

                    f32 gap = 0.0f; // surfaceY - cameraY (positive = camera below)
                    if (!GetWaterCameraFootprintGap(transform.GetTransform(), water, cameraPosition, gap))
                        continue;

                    // Activate when the camera is submerged OR within a wave's
                    // reach of the surface (straddling / "covered by a wave").
                    // gap = surfaceY - cameraY, so gap > 0 is submerged and a
                    // small negative gap means the eye is just above the flat
                    // surface but a crest can still wash over it. The ToneMap fog
                    // now reads the per-pixel water-surface depth, so it fogs only
                    // the genuinely-underwater pixels (it no longer mistakes the
                    // seafloor behind the surface for the surface) — which is why
                    // we can safely activate near/above the waterline. Well above
                    // the water (gap < -kWaveReach) stays the water shader's own
                    // refraction/depth tint. Nearest surface (smallest |gap|) wins.
                    constexpr f32 kWaveReach = 2.0f; // generous max crest height above the flat plane
                    if (const f32 absGap = std::abs(gap); gap > -kWaveReach && absGap < bestSurfaceDist)
                    {
                        bestSurfaceDist = absGap;
                        underwater.Active = true;
                        // Sanitize each colour component before clamping — glm::clamp
                        // passes NaN through, so a bad component would propagate into
                        // the fog UBO and break the tone-map pass. Replace non-finite
                        // with 0 first, then clamp to [0,1].
                        const glm::vec3 rawFog = water.m_UnderwaterFogColor;
                        const glm::vec3 finiteFog(std::isfinite(rawFog.x) ? rawFog.x : 0.0f,
                                                  std::isfinite(rawFog.y) ? rawFog.y : 0.0f,
                                                  std::isfinite(rawFog.z) ? rawFog.z : 0.0f);
                        underwater.FogColor = glm::clamp(finiteFog, glm::vec3(0.0f), glm::vec3(1.0f));
                        underwater.Density = std::isfinite(water.m_UnderwaterFogDensity)
                                                 ? std::clamp(water.m_UnderwaterFogDensity, 0.0f, 10.0f)
                                                 : 0.08f;
                        underwater.WaterSurfaceY = cameraPosition.y + gap;

                        // Refraction wobble + chromatic split (§7.2 bullet 2).
                        refractionParams = glm::vec4(
                            sanitizeParam(water.m_UnderwaterRefractionStrength, 0.0f, 0.1f, 0.006f),
                            sanitizeParam(water.m_UnderwaterRefractionScale, 0.0f, 200.0f, 18.0f),
                            sanitizeParam(water.m_UnderwaterRefractionSpeed, 0.0f, 50.0f, 1.2f),
                            sanitizeParam(water.m_UnderwaterChromaticStrength, 0.0f, 1.0f, 0.4f));
                        // Caustics (§7.1). Scale is clamped to a small positive min
                        // so the world-XZ projection always varies spatially.
                        causticParams = glm::vec4(
                            sanitizeParam(water.m_CausticsIntensity, 0.0f, 10.0f, 0.5f),
                            sanitizeParam(water.m_CausticsScale, 0.001f, 10.0f, 0.35f),
                            sanitizeParam(water.m_CausticsSpeed, 0.0f, 50.0f, 0.6f),
                            sanitizeParam(water.m_CausticsMaxDepth, 0.1f, 1000.0f, 25.0f));
                        const glm::vec3 rawCaustic = water.m_CausticsColor;
                        const glm::vec3 finiteCaustic(std::isfinite(rawCaustic.x) ? rawCaustic.x : 0.7f,
                                                      std::isfinite(rawCaustic.y) ? rawCaustic.y : 0.85f,
                                                      std::isfinite(rawCaustic.z) ? rawCaustic.z : 1.0f);
                        causticColor = glm::clamp(finiteCaustic, glm::vec3(0.0f), glm::vec3(1.0f));

                        // God rays (§3.3). Decay is kept strictly < 1 so the march
                        // can't diverge; density bounds the screen-space reach.
                        godRayParams = glm::vec4(
                            sanitizeParam(water.m_GodRayIntensity, 0.0f, 10.0f, 0.5f),
                            sanitizeParam(water.m_GodRayDecay, 0.0f, 0.999f, 0.97f),
                            sanitizeParam(water.m_GodRayDensity, 0.0f, 2.0f, 0.85f),
                            sanitizeParam(water.m_GodRayWeight, 0.0f, 2.0f, 1.0f));
                        godRaySamples = std::clamp(static_cast<f32>(water.m_GodRaySamples), 1.0f, 256.0f);
                        const glm::vec3 rawGodRay = water.m_GodRayColor;
                        const glm::vec3 finiteGodRay(std::isfinite(rawGodRay.x) ? rawGodRay.x : 1.0f,
                                                     std::isfinite(rawGodRay.y) ? rawGodRay.y : 0.95f,
                                                     std::isfinite(rawGodRay.z) ? rawGodRay.z : 0.8f);
                        godRayColor = glm::clamp(finiteGodRay, glm::vec3(0.0f), glm::vec3(1.0f));
                        godRayShape = glm::vec2(
                            sanitizeParam(water.m_GodRayDappleFloor, 0.0f, 1.0f, 0.35f),
                            sanitizeParam(water.m_GodRaySunFalloff, 1.0f, 64.0f, 16.0f));
                    }
                }

                Renderer3D::SetUnderwaterFogState(underwater);

                // Sun "overhead" factor for the caustic fade: the sun's light
                // travels along PrimaryDirectionalLightDir, so a downward (overhead)
                // sun has a negative y. max(-y, 0) on the normalized direction is 1
                // at noon and 0 at/below the horizon. Defaults to straight-down.
                f32 sunOverhead = 0.0f;
                glm::vec3 sunDir = Renderer3D::GetPrimaryDirectionalLightDirection();
                if (const f32 len2 = glm::dot(sunDir, sunDir); std::isfinite(len2) && len2 > 1e-8f)
                    sunOverhead = std::clamp(-sunDir.y / std::sqrt(len2), 0.0f, 1.0f);

                // God rays (§3.3) need the sun's screen-space vanishing point so the
                // tone-map pass can radial-blur toward it. The sun is opposite the
                // light-travel direction, infinitely far, so it projects via the
                // shared CPU mirror (kept in sync with the shader). When it's behind
                // the camera the flag stays 0 and the shader skips the march.
                glm::vec2 sunScreenUV(0.5f);
                f32 sunInFront = 0.0f;
                if (UnderwaterCaustics::GodRaySunScreenUV(viewProjection, sunDir, sunScreenUV))
                    sunInFront = 1.0f;

                UnderwaterFogUBOData uwData{};
                uwData.ColorAndDensity = glm::vec4(underwater.FogColor, underwater.Density);
                // Flags.z carries the animation clock for the refraction wobble +
                // caustic scroll (mirrors the water surface's own wave time).
                uwData.Flags = glm::vec4(underwater.Active ? 1.0f : 0.0f, underwater.WaterSurfaceY, animationTime, 0.0f);
                uwData.CameraPos = glm::vec4(cameraPosition, 0.0f);
                uwData.RefractionParams = refractionParams;
                uwData.CausticParams = causticParams;
                uwData.CausticColorAndSun = glm::vec4(causticColor, sunOverhead);
                uwData.GodRayParams = godRayParams;
                uwData.GodRaySun = glm::vec4(godRaySamples, sunScreenUV.x, sunScreenUV.y, sunInFront);
                uwData.GodRayColor = glm::vec4(godRayColor, 0.0f);
                uwData.GodRayShape = glm::vec4(godRayShape, 0.0f, 0.0f);
                uwData.InverseViewProjection = glm::inverse(viewProjection);
                Renderer3D::UploadUnderwaterFogUBO(uwData);
            }

            // Submit decal draw commands to the DecalRenderPass command bucket
            {
                auto decalView = m_Registry.view<TransformComponent, DecalComponent>();
                for (auto entity : decalView)
                {
                    auto const& [transform, decal] = decalView.get<TransformComponent, DecalComponent>(entity);

                    // Clamp decal size to a minimum epsilon to prevent singular
                    // (non-invertible) transforms that produce NaN/Inf.
                    constexpr f32 minSize = 1e-4f;
                    glm::vec3 safeSize = glm::max(decal.m_Size, glm::vec3(minSize));

                    // Build scaled transform for the decal projection box
                    glm::mat4 decalTransform = transform.GetTransform() *
                                               glm::scale(glm::mat4(1.0f), safeSize);
                    glm::mat4 inverseDecalTransform = glm::inverse(decalTransform);

                    // Resolve albedo texture ID (fallback to white if none assigned).
                    // Emissive-mode decals reuse the primary slot for the emissive
                    // texture (DecalShader samples the same TEX_USER_0 binding).
                    RendererID albedoTextureID = 0;
                    if (decal.m_Mode == DecalMode::Emissive)
                    {
                        // Emissive-mode decals reuse the primary slot for the
                        // emissive texture (DecalShader samples the same
                        // TEX_USER_0 binding). When no emissive texture is
                        // assigned, leave the slot at 0 — do NOT fall back to
                        // the albedo texture (it would project the diffuse
                        // colour into the emissive G-Buffer channel, painting
                        // unintended self-illumination onto the surface).
                        if (decal.m_EmissiveTexture)
                            albedoTextureID = decal.m_EmissiveTexture->GetRendererID();
                    }
                    else if (decal.m_AlbedoTexture)
                    {
                        albedoTextureID = decal.m_AlbedoTexture->GetRendererID();
                    }
                    else
                    {
                        auto whiteTexture = Renderer3D::GetWhiteTexture();
                        if (whiteTexture)
                        {
                            albedoTextureID = whiteTexture->GetRendererID();
                        }
                    }

                    // Optional normal / RMA textures. Only meaningful in the matching mode;
                    // otherwise pass 0 and the dispatcher will skip the bind.
                    RendererID normalTextureID = (decal.m_Mode == DecalMode::Normal && decal.m_NormalTexture)
                                                     ? decal.m_NormalTexture->GetRendererID()
                                                     : 0u;
                    RendererID rmaTextureID = (decal.m_Mode == DecalMode::RMA && decal.m_RMATexture)
                                                  ? decal.m_RMATexture->GetRendererID()
                                                  : 0u;

                    glm::vec4 decalParams = glm::vec4(
                        decal.m_FadeDistance, decal.m_NormalAngleThreshold, 0.0f, 0.0f);

                    auto* packet = Renderer3D::DrawDecal(
                        decalTransform,
                        inverseDecalTransform,
                        decal.m_Color,
                        decalParams,
                        albedoTextureID,
                        normalTextureID,
                        rmaTextureID,
                        static_cast<DrawDecalCommand::DecalMode>(decal.m_Mode),
                        decal.m_Transparent,
                        static_cast<i32>(std::to_underlying(entity)));

                    if (packet)
                    {
                        Renderer3D::SubmitDecalPacket(packet);
                    }
                }
            }
        }

        // Shadow pass setup for mesh entity traversal. Gate on AnyShadowsRequested()
        // (not just the global IsEnabled() toggle) so no casters are built when no
        // light casts shadows this frame — see issue #522 and the terrain block above.
        const bool meshHasActiveShadows = Renderer3D::IsShadowPassAvailable() && Renderer3D::GetShadowMap().IsEnabled() &&
                                          Renderer3D::GetShadowMap().AnyShadowsRequested();

        // Draw mesh entities (skip animated entities - they're rendered separately)
        {
            auto view = m_Registry.view<TransformComponent, MeshComponent>();
            for (auto entity : view)
            {
                // Skip entities with SkeletonComponent - they're rendered by the animated mesh path
                if (m_Registry.all_of<SkeletonComponent>(entity))
                {
                    continue;
                }

                const auto& mesh = view.get<MeshComponent>(entity);

                if (!mesh.m_MeshSource)
                {
                    continue;
                }

                const glm::mat4 worldTransform = GetWorldTransform(entity);

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Check if this entity should cast shadows. Alpha-masked /
                // blended materials are excluded for the same reason as the
                // ModelComponent path below: the shared shadow-depth shader
                // doesn't sample the albedo alpha, so otherwise see-through
                // geometry would project as solid silhouettes.
                bool castsShadow = meshHasActiveShadows &&
                                   (!m_Registry.all_of<MaterialComponent>(entity) ||
                                    (!m_Registry.get<MaterialComponent>(entity).m_Material.GetFlag(MaterialFlag::DisableShadowCasting) &&
                                     m_Registry.get<MaterialComponent>(entity).m_Material.GetAlphaMode() == AlphaMode::Opaque));

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(std::to_underlying(entity));

                // Get LOD group if present and enabled
                const LODGroup* lodGroup = nullptr;
                if (m_Registry.all_of<LODGroupComponent>(entity))
                {
                    const auto& lodComp = m_Registry.get<LODGroupComponent>(entity);
                    if (lodComp.m_Enabled)
                    {
                        lodGroup = &lodComp.m_LODGroup;
                    }
                }

                // Draw each submesh with entity ID
                if (mesh.m_MeshSource && !mesh.m_MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < mesh.m_MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(mesh.m_MeshSource, i);
                        if (auto* packet = Renderer3D::DrawMesh(submesh, worldTransform, material, true, entityID, lodGroup); packet)
                            Renderer3D::SubmitPacket(packet);

                        // Shadow caster for this submesh
                        if (castsShadow && submesh)
                        {
                            auto va = submesh->GetVertexArray();
                            if (va)
                            {
                                Renderer3D::AddMeshShadowCaster(
                                    va->GetRendererID(), submesh->GetIndexCount(), submesh->GetBaseIndex(),
                                    worldTransform, GetShadowVaoID(submesh),
                                    submesh->GetTransformedBoundingBox(worldTransform));
                            }
                        }
                    }
                }
            }
        }

        // Cloth soft-body render pass (issue #460). Each live cloth owns a deforming
        // MeshSource whose vertices are the world-space particle positions read back from
        // Jolt each tick. The mesh is built lazily here (it needs a GL context, so headless
        // ticks never reach this render path), then its VBO is re-uploaded every frame and
        // drawn at the identity transform — the particle positions are already in world
        // space. SetPreOptimized keeps the grid-index ↔ VBO-slot mapping stable so the raw
        // readback order can be uploaded directly.
        if (!m_ClothRuntime.empty())
        {
            for (auto& [entityID, state] : m_ClothRuntime)
            {
                // Use the cloth entity's authored MaterialComponent if present, else the
                // shared default (mirrors the MeshComponent path above). Force TwoSided:
                // a cloth is a thin sheet, so both faces must draw — otherwise back-facing
                // triangles are culled and the drape looks torn / invisible from behind.
                Entity clothEntity = GetEntityByUUID(entityID);
                Material clothMaterial = (clothEntity && clothEntity.HasComponent<MaterialComponent>())
                                             ? clothEntity.GetComponent<MaterialComponent>().m_Material
                                             : GetDefaultMaterial();
                clothMaterial.SetFlag(MaterialFlag::TwoSided, true);
                const sizet count = state.m_Positions.size();
                if (count == 0 || state.m_Normals.size() != count || state.m_Columns < 2 || state.m_Rows < 2)
                    continue;

                if (!state.m_RenderMesh)
                {
                    std::vector<Vertex> vertices(count);
                    for (u32 row = 0; row < state.m_Rows; ++row)
                    {
                        for (u32 col = 0; col < state.m_Columns; ++col)
                        {
                            const u32 i = row * state.m_Columns + col;
                            vertices[i].Position = state.m_Positions[i];
                            vertices[i].Normal = state.m_Normals[i];
                            vertices[i].TexCoord = glm::vec2(
                                static_cast<f32>(col) / static_cast<f32>(state.m_Columns - 1),
                                static_cast<f32>(row) / static_cast<f32>(state.m_Rows - 1));
                        }
                    }
                    std::vector<u32> indices = BuildClothGridIndices(state.m_Columns, state.m_Rows);

                    Ref<MeshSource> meshSource = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));
                    meshSource->SetPreOptimized(true); // preserve grid-index ↔ VBO-slot mapping
                    Submesh submesh;
                    submesh.m_VertexCount = static_cast<u32>(count);
                    submesh.m_IndexCount = static_cast<u32>(meshSource->GetIndices().Num());
                    meshSource->AddSubmesh(submesh);
                    meshSource->Build();
                    state.m_RenderMesh = meshSource;
                }
                else
                {
                    auto& verts = state.m_RenderMesh->GetVertices();
                    if (static_cast<sizet>(verts.Num()) == count)
                    {
                        for (sizet i = 0; i < count; ++i)
                        {
                            verts.GetData()[i].Position = state.m_Positions[i];
                            verts.GetData()[i].Normal = state.m_Normals[i];
                        }
                        // Non-const Ref copy: GetVertexBuffer() returns a const Ref&, whose
                        // operator-> yields a const VertexBuffer* — SetData is non-const.
                        Ref<VertexBuffer> vb = state.m_RenderMesh->GetVertexBuffer();
                        vb->SetData(VertexData{ verts.GetData(), static_cast<u32>(static_cast<sizet>(verts.Num()) * sizeof(Vertex)) });
                    }
                }

                auto clothMesh = Ref<Mesh>::Create(state.m_RenderMesh, 0);
                if (auto* packet = Renderer3D::DrawMesh(clothMesh, glm::mat4(1.0f), clothMaterial, false, -1, nullptr); packet)
                    Renderer3D::SubmitPacket(packet);
            }
        }

        // Draw entity-owned dense instance batches (InstancedMeshComponent).
        // Each entity holds its own world-space InstanceData[] and renders in
        // a single DrawMeshInstanced packet per submesh (chunked at the
        // dispatcher's MaxMeshInstances cap when the count exceeds it).
        // Per-instance Color/Custom/EntityID survive to the shader via the
        // InstanceData overload of Renderer3D::DrawMeshInstanced.
        {
            auto view = m_Registry.view<InstancedMeshComponent>();
            for (auto entity : view)
            {
                auto& imc = view.get<InstancedMeshComponent>(entity);
                if (!imc.MeshSource)
                    continue;

                // Combine inline + asset-backed placements into one contiguous
                // buffer. The merge result is cached on the component itself
                // (`imc._MergedCache`) and reused frame-to-frame as long as
                // the inline size, asset handle, and asset's instance count /
                // data pointer all match the cached fingerprint — saves the
                // 224 B / instance memcpy for steady-state scatter scenes.
                const std::vector<InstanceData>* assetInstances = nullptr;
                if (imc.PlacementAssetHandle != 0)
                {
                    if (auto placement = AssetManager::GetAsset<InstancePlacementAsset>(imc.PlacementAssetHandle))
                        assetInstances = &placement->GetInstances();
                }

                const sizet inlineCount = imc.Instances.size();
                const sizet assetCount = assetInstances ? assetInstances->size() : 0;
                if (inlineCount + assetCount == 0)
                    continue;

                const InstanceData* instData = nullptr;
                sizet totalCount = 0;
                if (assetCount == 0)
                {
                    // Inline-only fast path — the InstancedMeshComponent's
                    // own `Instances` vector is already the contiguous buffer
                    // the submission needs. No copy and no cache involvement.
                    instData = imc.Instances.data();
                    totalCount = inlineCount;
                }
                else
                {
                    auto& cache = imc._MergedCache;
                    const InstanceData* currentAssetDataPtr = assetInstances->data();
                    const bool cacheValid = cache.InlineSize == inlineCount &&
                                            cache.PlacementHandle == imc.PlacementAssetHandle &&
                                            cache.AssetSize == assetCount &&
                                            cache.AssetDataPtr == currentAssetDataPtr &&
                                            cache.Data.size() == (inlineCount + assetCount);
                    if (!cacheValid)
                    {
                        cache.Data.clear();
                        cache.Data.reserve(inlineCount + assetCount);
                        cache.Data.insert(cache.Data.end(), imc.Instances.begin(), imc.Instances.end());
                        cache.Data.insert(cache.Data.end(), assetInstances->begin(), assetInstances->end());
                        cache.InlineSize = inlineCount;
                        cache.PlacementHandle = imc.PlacementAssetHandle;
                        cache.AssetSize = assetCount;
                        cache.AssetDataPtr = currentAssetDataPtr;
                    }
                    instData = cache.Data.data();
                    totalCount = cache.Data.size();
                }

                const Material& material = imc.OverrideMaterial
                                               ? *imc.OverrideMaterial.Raw()
                                               : (m_Registry.all_of<MaterialComponent>(entity)
                                                      ? m_Registry.get<MaterialComponent>(entity).m_Material
                                                      : GetDefaultMaterial());

                const u64 ownerKey = static_cast<u64>(std::to_underlying(entity));

                const bool castsShadow = imc.CastShadows && meshHasActiveShadows &&
                                         material.GetAlphaMode() == AlphaMode::Opaque &&
                                         !material.GetFlag(MaterialFlag::DisableShadowCasting);

                if (!imc.MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < imc.MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(imc.MeshSource, i);
                        auto* packet = Renderer3D::DrawMeshInstanced(
                            submesh,
                            std::span<const InstanceData>(instData, totalCount),
                            material, true, ownerKey);
                        if (packet)
                            Renderer3D::SubmitPacket(packet);

                        // Shadow casters: one entry per instance per submesh.
                        // ShadowRenderPass auto-batches identical (vao, mesh)
                        // pairs in its own bucket so this stays O(1) draws on
                        // the shadow side even at N instances. Submitting per
                        // instance (rather than one combined caster) lets the
                        // shadow path do its own per-instance frustum cull
                        // against each cascade.
                        if (castsShadow && submesh)
                        {
                            if (auto va = submesh->GetVertexArray())
                            {
                                const u32 shadowVao = GetShadowVaoID(submesh);
                                for (sizet k = 0; k < totalCount; ++k)
                                {
                                    Renderer3D::AddMeshShadowCaster(
                                        va->GetRendererID(), submesh->GetIndexCount(),
                                        submesh->GetBaseIndex(), instData[k].Transform, shadowVao,
                                        submesh->GetTransformedBoundingBox(instData[k].Transform));
                                }
                            }
                        }
                    }
                }
            }
        }

        // Draw submesh entities (if they have their own transforms)
        {
            auto view = m_Registry.view<TransformComponent, SubmeshComponent>();
            for (auto entity : view)
            {
                const auto& submesh = view.get<SubmeshComponent>(entity);

                if (!submesh.m_Mesh || !submesh.m_Visible)
                {
                    continue;
                }

                const glm::mat4 worldTransform = GetWorldTransform(entity);

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Exclude alpha-masked/blended materials: see comment on the
                // MeshComponent branch above (shared shadow-depth shader has
                // no alpha sampling).
                bool castsShadow = meshHasActiveShadows &&
                                   (!m_Registry.all_of<MaterialComponent>(entity) ||
                                    (!m_Registry.get<MaterialComponent>(entity).m_Material.GetFlag(MaterialFlag::DisableShadowCasting) &&
                                     m_Registry.get<MaterialComponent>(entity).m_Material.GetAlphaMode() == AlphaMode::Opaque));

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(std::to_underlying(entity));

                // Get LOD group if present and enabled
                const LODGroup* lodGroup = nullptr;
                if (m_Registry.all_of<LODGroupComponent>(entity))
                {
                    const auto& lodComp = m_Registry.get<LODGroupComponent>(entity);
                    if (lodComp.m_Enabled)
                    {
                        lodGroup = &lodComp.m_LODGroup;
                    }
                }

                if (auto* packet = Renderer3D::DrawMesh(submesh.m_Mesh, worldTransform, material, true, entityID, lodGroup); packet)
                    Renderer3D::SubmitPacket(packet);

                // Shadow caster for this submesh entity
                if (castsShadow && submesh.m_Mesh)
                {
                    auto va = submesh.m_Mesh->GetVertexArray();
                    if (va)
                    {
                        Renderer3D::AddMeshShadowCaster(
                            va->GetRendererID(), submesh.m_Mesh->GetIndexCount(), submesh.m_Mesh->GetBaseIndex(),
                            worldTransform, GetShadowVaoID(submesh.m_Mesh),
                            submesh.m_Mesh->GetTransformedBoundingBox(worldTransform));
                    }
                }
            }
        }

        // Draw model entities (full models with materials from file)
        {
            auto view = m_Registry.view<TransformComponent, ModelComponent>();
            for (auto entity : view)
            {
                const auto& model = view.get<ModelComponent>(entity);

                if (!model.m_Model || !model.m_Visible)
                {
                    continue;
                }

                // Model::DrawParallel uses the model's own materials loaded from file
                // Pass entity ID for mouse picking support
                const auto modelTransform = GetWorldTransform(entity);
                model.m_Model->DrawParallel(modelTransform, static_cast<int>(std::to_underlying(entity)));

                // Submit each submesh as a shadow caster — Model::DrawParallel
                // only enqueues color draws, so without this loop ModelComponent
                // meshes never reach ShadowRenderPass.
                if (meshHasActiveShadows)
                {
                    const auto& meshes = model.m_Model->GetMeshes();
                    const auto& materials = model.m_Model->GetMaterials();
                    for (const auto& submesh : meshes)
                    {
                        if (!submesh)
                            continue;

                        if (const u32 matIdx = submesh->GetSubmesh().m_MaterialIndex; matIdx < materials.size() && materials[matIdx])
                        {
                            const auto& mat = *materials[matIdx];
                            if (mat.GetFlag(MaterialFlag::DisableShadowCasting))
                                continue;
                            // ShadowDepth.glsl has no UV/albedo sampling, so an
                            // alpha-masked banner would otherwise project its
                            // full geometry as a solid shadow. Skip until a
                            // separate alpha-aware shadow shader exists.
                            if (mat.GetAlphaMode() != AlphaMode::Opaque)
                                continue;
                        }

                        auto va = submesh->GetVertexArray();
                        if (!va)
                            continue;

                        Renderer3D::AddMeshShadowCaster(
                            va->GetRendererID(), submesh->GetIndexCount(), submesh->GetBaseIndex(),
                            modelTransform, GetShadowVaoID(submesh),
                            submesh->GetTransformedBoundingBox(modelTransform));
                    }
                }
            }
        }

        // Draw animated mesh entities (entities with MeshComponent + SkeletonComponent)
        {
            auto view = m_Registry.view<TransformComponent, MeshComponent, SkeletonComponent>();
            for (auto entity : view)
            {
                const auto [mesh, skeleton] = view.get<MeshComponent, SkeletonComponent>(entity);

                if (!mesh.m_MeshSource || !skeleton.m_Skeleton)
                {
                    continue;
                }

                const glm::mat4 worldTransform = GetWorldTransform(entity);

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Get bone matrices from skeleton
                const auto& boneMatrices = skeleton.m_Skeleton->m_FinalBoneMatrices;
                const auto& prevBoneMatrices = skeleton.m_Skeleton->m_PrevFinalBoneMatrices;

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(std::to_underlying(entity));

                // Exclude alpha-masked/blended materials (see MeshComponent
                // branch comment above for the underlying shader limitation).
                bool castsShadow = meshHasActiveShadows &&
                                   (!m_Registry.all_of<MaterialComponent>(entity) ||
                                    (!m_Registry.get<MaterialComponent>(entity).m_Material.GetFlag(MaterialFlag::DisableShadowCasting) &&
                                     m_Registry.get<MaterialComponent>(entity).m_Material.GetAlphaMode() == AlphaMode::Opaque));

                // Draw each submesh as an animated mesh
                if (!mesh.m_MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < mesh.m_MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(mesh.m_MeshSource, i);
                        auto* packet = Renderer3D::DrawAnimatedMesh(submesh, worldTransform, material, boneMatrices, prevBoneMatrices, false, entityID);
                        if (packet)
                        {
                            Renderer3D::SubmitPacket(packet);

                            // Shadow caster reuses bone data from the scene packet
                            if (castsShadow && submesh)
                            {
                                auto va = submesh->GetVertexArray();
                                if (va)
                                {
                                    const auto* cmd = packet->GetCommandData<DrawMeshCommand>();
                                    if (cmd)
                                    {
                                        Renderer3D::AddSkinnedShadowCaster(
                                            va->GetRendererID(), submesh->GetIndexCount(), submesh->GetBaseIndex(),
                                            worldTransform,
                                            cmd->boneBufferOffset, cmd->boneCount,
                                            submesh->GetTransformedBoundingBox(worldTransform));
                                    }
                                    else
                                    {
                                        OLO_CORE_WARN("DrawMeshCommand is null for animated mesh shadow caster");
                                    }
                                }
                            }
                        }
                    }
                }

                // Draw skeleton visualization if enabled
                if (m_SkeletonVisualization.ShowSkeleton && skeleton.m_Skeleton)
                {
                    Renderer3D::DrawSkeleton(
                        *skeleton.m_Skeleton,
                        worldTransform,
                        m_SkeletonVisualization.ShowBones,
                        m_SkeletonVisualization.ShowJoints,
                        m_SkeletonVisualization.JointSize,
                        m_SkeletonVisualization.BoneThickness);
                }
            }
        }

        // Draw tile renderer entities (grid of instanced mesh tiles)
        {
            auto view = m_Registry.view<TransformComponent, TileRendererComponent>();
            for (auto entity : view)
            {
                const auto& tileComp = view.get<TileRendererComponent>(entity);

                if (!tileComp.TileMesh || !tileComp.TileMesh->IsValid())
                {
                    continue;
                }

                i32 entityID = static_cast<i32>(std::to_underlying(entity));
                glm::mat4 baseTransform = GetWorldTransform(entity);

                for (u32 z = 0; z < tileComp.Height; ++z)
                {
                    for (u32 x = 0; x < tileComp.Width; ++x)
                    {
                        u32 cellIndex = z * tileComp.Width + x;
                        u8 matIdx = (cellIndex < tileComp.MaterialIDs.size())
                                        ? tileComp.MaterialIDs[cellIndex]
                                        : static_cast<u8>(0);

                        const Material& material = (matIdx < tileComp.Materials.size())
                                                       ? tileComp.Materials[matIdx]
                                                       : GetDefaultMaterial();

                        glm::mat4 tileTransform = glm::translate(
                            baseTransform,
                            glm::vec3(static_cast<f32>(x) * tileComp.TileSize, 0.0f, static_cast<f32>(z) * tileComp.TileSize));

                        if (auto* packet = Renderer3D::DrawMesh(tileComp.TileMesh, tileTransform, material, true, entityID, nullptr); packet)
                            Renderer3D::SubmitPacket(packet);

                        // Shadow caster for this tile — Opaque only, see
                        // MeshComponent branch comment above.
                        if (meshHasActiveShadows && !material.GetFlag(MaterialFlag::DisableShadowCasting) &&
                            material.GetAlphaMode() == AlphaMode::Opaque)
                        {
                            auto va = tileComp.TileMesh->GetVertexArray();
                            if (va)
                            {
                                Renderer3D::AddMeshShadowCaster(
                                    va->GetRendererID(), tileComp.TileMesh->GetIndexCount(), tileComp.TileMesh->GetBaseIndex(),
                                    tileTransform, GetShadowVaoID(tileComp.TileMesh),
                                    tileComp.TileMesh->GetTransformedBoundingBox(tileTransform));
                            }
                        }
                    }
                }
            }
        }
    }

    void Scene::RenderScene3D(EditorCamera const& camera)
    {
        OLO_PROFILE_FUNCTION();

        Renderer3D::BeginScene(camera);

        // Render skybox from EnvironmentMapComponent (first one found)
        LoadAndRenderSkybox();
        ApplyReflectionProbeOverride(camera.GetPosition());

        ProcessScene3DSharedLogic(
            camera.GetViewMatrix(),
            camera.GetProjection(),
            camera.GetPosition(),
            camera.GetNearClip(),
            camera.GetFarClip());

        if (m_ShowGrid)
        {
            Renderer3D::DrawInfiniteGrid(m_GridSpacing);
        }

        // Draw world axis helper at origin
        if (m_ShowWorldAxisHelper)
        {
            Renderer3D::DrawWorldAxisHelper(3.0f);
        }

        // Draw light visualization gizmos
        if (m_ShowLightGizmos)
        {
            {
                auto view = m_Registry.view<TransformComponent, DirectionalLightComponent>();
                for (auto entity : view)
                {
                    const auto& [tc, dirLight] = view.get<TransformComponent, DirectionalLightComponent>(entity);

                    // Draw the directional light gizmo with arrow and sun icon
                    Renderer3D::DrawDirectionalLightGizmo(
                        tc.Translation,
                        dirLight.m_Direction,
                        dirLight.m_Color,
                        dirLight.m_Intensity);
                }
            }
            {
                auto view = m_Registry.view<TransformComponent, PointLightComponent>();
                for (auto entity : view)
                {
                    const auto& [tc, pointLight] = view.get<TransformComponent, PointLightComponent>(entity);

                    // Draw the point light gizmo with range sphere
                    Renderer3D::DrawPointLightGizmo(
                        tc.Translation,
                        pointLight.m_Range,
                        pointLight.m_Color,
                        true // Show range sphere
                    );
                }
            }
            {
                auto view = m_Registry.view<TransformComponent, SpotLightComponent>();
                for (auto entity : view)
                {
                    const auto& [tc, spotLight] = view.get<TransformComponent, SpotLightComponent>(entity);

                    // Draw the spot light gizmo with cone visualization
                    Renderer3D::DrawSpotLightGizmo(
                        tc.Translation,
                        spotLight.m_Direction,
                        spotLight.m_Range,
                        spotLight.m_InnerCutoff,
                        spotLight.m_OuterCutoff,
                        spotLight.m_Color);
                }
            }
            {
                auto view = m_Registry.view<TransformComponent, SphereAreaLightComponent>();
                for (auto entity : view)
                {
                    const auto& [tc, areaLight] = view.get<TransformComponent, SphereAreaLightComponent>(entity);

                    Renderer3D::DrawSphereAreaLightGizmo(
                        tc.Translation,
                        areaLight.m_Radius,
                        areaLight.m_Range,
                        areaLight.m_Color,
                        areaLight.m_Intensity);
                }
            }
            {
                // Reflection probe gizmos: a small marker sphere at the probe
                // position + a wireframe sphere at the influence radius.
                // Colour-codes baked state: cyan = baked, dim grey = pending.
                auto view = m_Registry.view<TransformComponent, ReflectionProbeComponent>();
                for (auto entity : view)
                {
                    const auto& [tc, probe] = view.get<TransformComponent, ReflectionProbeComponent>(entity);
                    if (!probe.m_Active)
                    {
                        continue;
                    }

                    bool const hasBake = probe.m_BakedEnvironment && probe.m_BakedEnvironment->HasIBL();
                    glm::vec3 const markerColor = hasBake
                                                      ? glm::vec3(0.3f, 0.9f, 1.0f)  // cyan: baked
                                                      : glm::vec3(0.5f, 0.5f, 0.5f); // grey: not yet baked
                    glm::vec3 const influenceColor = hasBake
                                                         ? glm::vec3(0.3f, 0.9f, 1.0f) * 0.6f
                                                         : glm::vec3(0.5f, 0.5f, 0.5f) * 0.6f;

                    // Small marker at probe origin (0.3m matches the issue
                    // spec; the actual chrome-PBR sphere reuse of s_Data.SphereMesh
                    // is deferred — wireframe is enough to indicate position).
                    Renderer3D::DrawSphereColliderGizmo(tc.Translation, 0.3f, markerColor);
                    // Influence radius — wireframe so it doesn't occlude the
                    // contained geometry.
                    Renderer3D::DrawSphereColliderGizmo(tc.Translation, probe.m_InfluenceRadius, influenceColor);
                }
            }
        }

        // Draw audio source gizmos
        {
            auto view = m_Registry.view<TransformComponent, AudioSourceComponent>();
            for (auto entity : view)
            {
                const auto& [tc, audioSource] = view.get<TransformComponent, AudioSourceComponent>(entity);

                // Only draw gizmo if spatialization is enabled
                if (audioSource.Config.Spatialization)
                {
                    Renderer3D::DrawAudioSourceGizmo(
                        tc.Translation,
                        audioSource.Config.MinDistance,
                        audioSource.Config.MaxDistance,
                        glm::vec3(0.2f, 0.6f, 1.0f) // Blue color for audio
                    );
                }
            }
        }

        // Draw fog volume gizmos
        {
            auto view = m_Registry.view<TransformComponent, FogVolumeComponent>();
            for (auto entity : view)
            {
                const auto& [tc, fogVol] = view.get<TransformComponent, FogVolumeComponent>(entity);
                if (!fogVol.m_Enabled)
                {
                    continue;
                }

                glm::vec3 gizmoColor = fogVol.m_Color * 0.8f + glm::vec3(0.2f); // Brightened fog color for visibility
                glm::quat rotation = tc.GetRotation();

                switch (fogVol.m_Shape)
                {
                    case FogVolumeShape::Box:
                        Renderer3D::DrawBoxColliderGizmo(tc.Translation, fogVol.m_Extents, rotation, gizmoColor);
                        break;
                    case FogVolumeShape::Sphere:
                        Renderer3D::DrawSphereColliderGizmo(tc.Translation, fogVol.m_Extents.x, gizmoColor);
                        break;
                    case FogVolumeShape::Cylinder:
                        // Approximate cylinder with capsule gizmo (radius + half-height)
                        Renderer3D::DrawCapsuleColliderGizmo(tc.Translation, fogVol.m_Extents.x, fogVol.m_Extents.y, rotation, gizmoColor);
                        break;
                    default:
                        OLO_CORE_ASSERT(false, "Unknown FogVolumeShape");
                        break;
                }
            }
        }

        // Draw streaming volume gizmos (sphere radius indicators)
        {
            auto view = m_Registry.view<TransformComponent, StreamingVolumeComponent>();
            for (auto entity : view)
            {
                const auto& [tc, vol] = view.get<TransformComponent, StreamingVolumeComponent>(entity);

                // Load radius: cyan sphere
                Renderer3D::DrawSphereColliderGizmo(tc.Translation, vol.LoadRadius,
                                                    glm::vec3(0.2f, 0.8f, 0.9f));

                // Unload radius: dimmed orange sphere
                Renderer3D::DrawSphereColliderGizmo(tc.Translation, vol.UnloadRadius,
                                                    glm::vec3(0.9f, 0.5f, 0.2f));
            }
        }

        // Draw light probe volume gizmos (wireframe box around probe grid bounds)
        {
            auto view = m_Registry.view<LightProbeVolumeComponent>();
            for (auto entity : view)
            {
                auto const& vol = view.get<LightProbeVolumeComponent>(entity);
                if (!vol.m_Active)
                {
                    continue;
                }

                glm::vec3 const center = (vol.m_BoundsMin + vol.m_BoundsMax) * 0.5f;
                glm::vec3 const halfExtents = (vol.m_BoundsMax - vol.m_BoundsMin) * 0.5f;
                glm::vec3 const gizmoColor(0.2f, 0.8f, 0.4f); // Green-ish for probe volumes
                Renderer3D::DrawBoxColliderGizmo(center, halfExtents, glm::quat(1, 0, 0, 0), gizmoColor);

                // Draw debug probe spheres at each grid position when enabled
                if (vol.m_ShowDebugProbes)
                {
                    // Try to get baked data for color-coding by validity
                    Ref<LightProbeVolumeAsset> probeAsset;
                    if (vol.m_BakedDataAsset != 0)
                    {
                        probeAsset = AssetManager::GetAsset<LightProbeVolumeAsset>(vol.m_BakedDataAsset);
                    }

                    glm::vec3 const extent = vol.m_BoundsMax - vol.m_BoundsMin;
                    for (i32 z = 0; z < vol.m_Resolution.z; ++z)
                    {
                        for (i32 y = 0; y < vol.m_Resolution.y; ++y)
                        {
                            for (i32 x = 0; x < vol.m_Resolution.x; ++x)
                            {
                                glm::vec3 const t(
                                    vol.m_Resolution.x > 1 ? static_cast<f32>(x) / static_cast<f32>(vol.m_Resolution.x - 1) : 0.5f,
                                    vol.m_Resolution.y > 1 ? static_cast<f32>(y) / static_cast<f32>(vol.m_Resolution.y - 1) : 0.5f,
                                    vol.m_Resolution.z > 1 ? static_cast<f32>(z) / static_cast<f32>(vol.m_Resolution.z - 1) : 0.5f);
                                glm::vec3 const probePos = vol.m_BoundsMin + t * extent;

                                // Color-code: green = valid, red = invalid, yellow = no baked data
                                glm::vec3 probeColor(0.8f, 0.8f, 0.2f); // Yellow = no baked data
                                if (probeAsset && probeAsset->HasBakedData())
                                {
                                    i32 const idx = vol.GridIndex(x, y, z);
                                    i32 const baseOffset = idx * static_cast<i32>(SH_COEFFICIENT_COUNT);
                                    if (baseOffset < static_cast<i32>(probeAsset->CoefficientData.size()))
                                    {
                                        f32 const validity = probeAsset->CoefficientData[baseOffset].w;
                                        if (validity > 0.5f)
                                        {
                                            // Valid probe: use dominant SH color (DC term = coefficient 0)
                                            glm::vec3 const dc(
                                                probeAsset->CoefficientData[baseOffset].x,
                                                probeAsset->CoefficientData[baseOffset].y,
                                                probeAsset->CoefficientData[baseOffset].z);
                                            f32 const maxVal = glm::max(dc.x, glm::max(dc.y, dc.z));
                                            probeColor = maxVal > 0.001f ? dc / maxVal : glm::vec3(0.2f, 0.8f, 0.2f);
                                        }
                                        else
                                        {
                                            probeColor = glm::vec3(0.8f, 0.2f, 0.2f); // Red = invalid
                                        }
                                    }
                                }

                                Renderer3D::DrawSphereColliderGizmo(probePos, 0.15f, probeColor);
                            }
                        }
                    }
                }
            }
        }

        // Draw camera frustum gizmos for scene cameras (only in editor mode)
        if (m_ShowCameraFrustums)
        {
            auto view = m_Registry.view<TransformComponent, CameraComponent>();
            for (auto entity : view)
            {
                const auto& cameraComp = view.get<CameraComponent>(entity);
                const SceneCamera& sceneCamera = cameraComp.Camera;
                const glm::mat4 worldTransform = GetWorldTransform(entity);

                // Aspect ratio: cameras flagged FixedAspectRatio use their own
                // projection aspect (set externally and not driven by the
                // editor viewport). Otherwise default to the editor viewport
                // aspect so the gizmo matches what the camera would actually
                // render into the main viewport.
                f32 aspectRatio;
                if (const f32 cameraAspect = sceneCamera.GetAspectRatio(); cameraComp.FixedAspectRatio && cameraAspect > 0.0f)
                {
                    aspectRatio = cameraAspect;
                }
                else
                {
                    aspectRatio = (m_ViewportHeight > 0) ? static_cast<f32>(m_ViewportWidth) / static_cast<f32>(m_ViewportHeight) : 1.778f;
                }

                if (sceneCamera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                {
                    Renderer3D::DrawCameraFrustum(
                        worldTransform,
                        sceneCamera.GetPerspectiveVerticalFOV(),
                        aspectRatio,
                        sceneCamera.GetPerspectiveNearClip(),
                        sceneCamera.GetPerspectiveFarClip(),
                        glm::vec3(0.9f, 0.9f, 0.3f), // Yellow-ish color for frustum
                        true,                        // isPerspective
                        0.0f                         // orthoSize (not used for perspective)
                    );
                }
                else
                {
                    // Orthographic camera
                    Renderer3D::DrawCameraFrustum(
                        worldTransform,
                        0.0f, // fov (not used for ortho)
                        aspectRatio,
                        sceneCamera.GetOrthographicNearClip(),
                        sceneCamera.GetOrthographicFarClip(),
                        glm::vec3(0.3f, 0.9f, 0.9f), // Cyan color for ortho frustum
                        false,                       // isPerspective
                        sceneCamera.GetOrthographicSize());
                }
            }
        }

        // Draw 3D collider gizmos (green wireframes)
        {
            // Box colliders
            auto boxView = m_Registry.view<TransformComponent, BoxCollider3DComponent>();
            for (auto entity : boxView)
            {
                const auto& [tc, boxCollider] = boxView.get<TransformComponent, BoxCollider3DComponent>(entity);
                glm::vec3 position = tc.Translation + boxCollider.m_Offset;
                glm::quat rotation = tc.GetRotation();
                Renderer3D::DrawBoxColliderGizmo(position, boxCollider.m_HalfExtents, rotation);
            }

            // Sphere colliders
            auto sphereView = m_Registry.view<TransformComponent, SphereCollider3DComponent>();
            for (auto entity : sphereView)
            {
                const auto& [tc, sphereCollider] = sphereView.get<TransformComponent, SphereCollider3DComponent>(entity);
                glm::vec3 position = tc.Translation + sphereCollider.m_Offset;
                Renderer3D::DrawSphereColliderGizmo(position, sphereCollider.m_Radius);
            }

            // Capsule colliders
            auto capsuleView = m_Registry.view<TransformComponent, CapsuleCollider3DComponent>();
            for (auto entity : capsuleView)
            {
                const auto& [tc, capsuleCollider] = capsuleView.get<TransformComponent, CapsuleCollider3DComponent>(entity);
                glm::vec3 position = tc.Translation + capsuleCollider.m_Offset;
                glm::quat rotation = tc.GetRotation();
                Renderer3D::DrawCapsuleColliderGizmo(position, capsuleCollider.m_Radius, capsuleCollider.m_HalfHeight, rotation);
            }
        }

        // Draw world-space AABB gizmos around mesh entities. Same box the
        // frustum culler uses, so a missing wireframe here means the entity
        // also fails the visibility test.
        if (Renderer3D::GetRendererSettings().ShowBoundingBoxes)
        {
            const glm::vec3 bboxColor(0.3f, 0.9f, 1.0f);
            const glm::quat noRotation(1.0f, 0.0f, 0.0f, 0.0f);

            auto drawWorldAABB = [noRotation, bboxColor](const BoundingBox& worldAABB)
            {
                // Skip degenerate boxes — any non-positive axis means either a
                // default-constructed box, a flat-plane mesh (one zero axis),
                // a line/point (two or three zero axes), or an inverted box.
                // None render as a meaningful wireframe. A flat plane's "AABB"
                // is just the plane itself, so hiding it costs nothing useful.
                if (const glm::vec3 size = worldAABB.GetSize(); size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
                    return;
                Renderer3D::DrawBoxColliderGizmo(worldAABB.GetCenter(), worldAABB.GetExtents(), noRotation, bboxColor);
            };

            // Static MeshComponent entities — animated ones (SkeletonComponent)
            // are skipped because their bone gizmos already convey extents,
            // and their pre-skinning AABB doesn't reflect the posed silhouette.
            {
                auto view = m_Registry.view<TransformComponent, MeshComponent>();
                for (auto entity : view)
                {
                    if (m_Registry.all_of<SkeletonComponent>(entity))
                        continue;
                    const auto& mc = view.get<MeshComponent>(entity);
                    if (!mc.m_MeshSource)
                        continue;
                    drawWorldAABB(mc.m_MeshSource->GetBoundingBox().Transform(GetWorldTransform(entity)));
                }
            }

            // ModelComponent entities
            {
                auto view = m_Registry.view<TransformComponent, ModelComponent>();
                for (auto entity : view)
                {
                    const auto& modelComp = view.get<ModelComponent>(entity);
                    if (!modelComp.m_Model || !modelComp.m_Visible)
                        continue;
                    drawWorldAABB(modelComp.m_Model->GetTransformedBoundingBox(GetWorldTransform(entity)));
                }
            }
        }

        // Set particle render callback — executed by ParticleRenderPass during graph execution
        Renderer3D::SetParticleRenderCallback([this, &camera]()
                                              {
                                                  ParticleBatchRenderer::BeginBatch(camera);

                                                  glm::vec3 camPos = camera.GetPosition();

                                                  RenderParticleSystems(camPos, camera.GetNearClip(), camera.GetFarClip());

                                                  // Render snow ejecta particles
                                                  SnowEjectaSystem::Render();

                                                  // Render precipitation particles
                                                  PrecipitationSystem::Render();

                                                  ParticleBatchRenderer::EndBatch(); });

        Renderer3D::EndScene();
    }

    void Scene::RenderScene3D(Camera const& camera, const glm::mat4& cameraTransform)
    {
        OLO_PROFILE_FUNCTION();

        Renderer3D::BeginScene(camera, cameraTransform);

        // Extract camera parameters from primary SceneCamera (base Camera lacks near/far)
        glm::vec3 cameraPosition = glm::vec3(cameraTransform[3]);
        glm::mat4 viewMatrix = glm::inverse(cameraTransform);
        f32 cameraNearClip = 0.1f;
        f32 cameraFarClip = 1000.0f;
        {
            for (const auto camView = m_Registry.view<CameraComponent>(); const auto entity : camView)
            {
                if (const auto& cam = camView.get<CameraComponent>(entity); cam.Primary)
                {
                    if (cam.Camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
                    {
                        cameraNearClip = cam.Camera.GetOrthographicNearClip();
                        cameraFarClip = cam.Camera.GetOrthographicFarClip();
                    }
                    else
                    {
                        cameraNearClip = cam.Camera.GetPerspectiveNearClip();
                        cameraFarClip = cam.Camera.GetPerspectiveFarClip();
                    }
                    break;
                }
            }
            Renderer3D::SetCameraClipPlanes(cameraNearClip, cameraFarClip);
        }

        // Render skybox from EnvironmentMapComponent (first one found)
        LoadAndRenderSkybox();
        ApplyReflectionProbeOverride(cameraPosition);

        ProcessScene3DSharedLogic(
            viewMatrix,
            camera.GetProjection(),
            cameraPosition,
            cameraNearClip,
            cameraFarClip);

        // Set particle render callback — executed by ParticleRenderPass during graph execution
        Renderer3D::SetParticleRenderCallback([this, &camera, cameraTransform, cameraNearClip, cameraFarClip, cameraPosition]()
                                              {
                                                  ParticleBatchRenderer::BeginBatch(camera, cameraTransform);
                                                  RenderParticleSystems(cameraPosition, cameraNearClip, cameraFarClip);

                                                  // Render snow ejecta particles
                                                  SnowEjectaSystem::Render();

                                                  // Render precipitation particles
                                                  PrecipitationSystem::Render();

                                                  ParticleBatchRenderer::EndBatch(); });

        Renderer3D::EndScene();
    }

    void Scene::RenderParticleSystems(const glm::vec3& camPos, f32 nearClip, f32 farClip)
    {
        OLO_PROFILE_FUNCTION();

        // Sort particle systems back-to-front for correct alpha blending.
        // Reuses the ParticleSystemComponent owning group (issue #443); a group
        // exposes an exact size(), so reserve is precise (not a hint).
        auto psView = m_Registry.group<ParticleSystemComponent>(entt::get<TransformComponent>);
        std::vector<std::pair<f32, entt::entity>> sortedSystems;
        sortedSystems.reserve(psView.size());
        for (auto entity : psView)
        {
            const auto& tc = psView.get<TransformComponent>(entity);
            f32 dist = glm::length2(glm::vec3(tc.Translation) - camPos);
            sortedSystems.emplace_back(dist, entity);
        }
        std::ranges::sort(sortedSystems,
                          [](const auto& a, const auto& b)
                          { return a.first > b.first; });

        for (const auto& [dist, entity] : sortedSystems)
        {
            auto& psc = psView.get<ParticleSystemComponent>(entity);
            auto& sys = psc.System;

            // Frustum cull entire emitter via its bounding sphere
            ++Renderer3D::GetStats().TotalEmitters;
            if (Renderer3D::IsFrustumCullingEnabled() && !Renderer3D::IsVisibleInFrustum(sys.GetBoundingSphere()))
            {
                ++Renderer3D::GetStats().CulledEmitters;
                continue;
            }

            glm::vec3 offset = (sys.SimulationSpace == ParticleSpace::Local) ? sys.GetEmitterPosition() : glm::vec3(0.0f);

            const std::vector<u32>* sorted = nullptr;
            if (sys.DepthSortEnabled && sys.BlendMode != ParticleBlendMode::Additive)
            {
                sys.SortByDepth(camPos);
                sorted = &sys.GetSortedIndices();
            }

            const ModuleTextureSheetAnimation* sheet = sys.TextureSheetModule.Enabled ? &sys.TextureSheetModule : nullptr;

            // Enable/disable soft particles per system
            {
                SoftParticleParams softParams;
                if (auto sceneDepthTextureID = Renderer3D::ResolveFrameGraphTexture(ResourceNames::SceneDepth); sceneDepthTextureID != 0)
                {
                    i32 viewportWidth = 0;
                    i32 viewportHeight = 0;
                    glGetTextureLevelParameteriv(sceneDepthTextureID, 0, GL_TEXTURE_WIDTH, &viewportWidth);
                    glGetTextureLevelParameteriv(sceneDepthTextureID, 0, GL_TEXTURE_HEIGHT, &viewportHeight);

                    softParams.Enabled = sys.SoftParticlesEnabled;
                    softParams.Distance = sys.SoftParticleDistance;
                    softParams.DepthTextureID = sceneDepthTextureID;
                    softParams.NearClip = nearClip;
                    softParams.FarClip = farClip;
                    softParams.ViewportSize = { static_cast<f32>(viewportWidth), static_cast<f32>(viewportHeight) };
                }
                ParticleBatchRenderer::SetSoftParticleParams(softParams);
            }

            // Flush pending instances before changing blend mode
            ParticleBatchRenderer::Flush();
            SetParticleBlendMode(sys.BlendMode);

            // GPU rendering path — uses indirect draw from SSBO data
            if (sys.UseGPU && sys.GetGPUSystem())
            {
                ParticleBatchRenderer::RenderGPUBillboards(*sys.GetGPUSystem(), psc.Texture);
            }
            else if (sys.RenderMode == ParticleRenderMode::Mesh)
            {
                ParticleBatchRenderer::Flush();
                ParticleRenderer::RenderParticlesMesh(sys.GetPool(), psc.ParticleMesh, psc.Texture, offset, static_cast<int>(std::to_underlying(entity)), sorted);
            }
            else if (sys.RenderMode == ParticleRenderMode::StretchedBillboard)
            {
                ParticleRenderer::RenderParticlesStretched(sys.GetPool(), psc.Texture, 1.0f, offset, static_cast<int>(std::to_underlying(entity)), sorted, sheet);
            }
            else
            {
                ParticleRenderer::RenderParticlesBillboard(sys.GetPool(), psc.Texture, offset, static_cast<int>(std::to_underlying(entity)), sorted, sheet);
            }

            // Render child systems
            for (sizet c = 0; c < psc.ChildSystems.size(); ++c)
            {
                auto& childSys = psc.ChildSystems[c];
                const std::vector<u32>* childSorted = nullptr;
                if (childSys.DepthSortEnabled && childSys.BlendMode != ParticleBlendMode::Additive)
                {
                    childSys.SortByDepth(camPos);
                    childSorted = &childSys.GetSortedIndices();
                }
                ParticleBatchRenderer::Flush();
                SetParticleBlendMode(childSys.BlendMode);
                Ref<Texture2D> childTex = (c < psc.ChildTextures.size()) ? psc.ChildTextures[c] : nullptr;
                ParticleRenderer::RenderParticlesBillboard(childSys.GetPool(), childTex, offset, static_cast<int>(std::to_underlying(entity)), childSorted, nullptr);
            }

            // Trail rendering via dedicated trail shader in ParticleBatchRenderer
            if (sys.TrailModule.Enabled)
            {
                ParticleBatchRenderer::Flush();
                TrailRenderer::RenderTrails(sys.GetPool(), sys.GetTrailData(), sys.TrailModule, camPos, psc.Texture, offset, static_cast<int>(std::to_underlying(entity)));
                ParticleBatchRenderer::FlushTrails();
            }

            RestoreDefaultBlendMode();
        }
    }

} // namespace OloEngine

// Hand-written OnComponentAdded specializations that do real init work and are
// therefore excluded from the generated no-op list (kComponentsCustomOnAdd in
// tools/OloHeaderTool/main.cpp). The no-op add specializations are generated and
// #include'd inside the OloEngine namespace earlier in this file.
template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CameraComponent>([[maybe_unused]] OloEngine::Entity entity, OloEngine::CameraComponent& component)
{
    if ((m_ViewportWidth > 0) && (m_ViewportHeight > 0))
    {
        component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
    }
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::LocalizedTextComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::LocalizedTextComponent& component)
{
    // Force a re-sync on the next LocalizationSystem tick so a freshly-added
    // component picks up the active locale's text without waiting for a
    // locale change. UINT64_MAX is a guaranteed-mismatch sentinel: the
    // LocalizationManager generation starts at 0 and only bumps forward,
    // so it can never equal max() in any realistic run. Setting to 0 here
    // would silently fail when the manager itself is still at 0 (fresh
    // state, nothing has triggered a bump yet).
    m_LocalizationGeneration = std::numeric_limits<u64>::max();
}

// Rigidbody3DComponent specialization is defined inside the OloEngine
// namespace earlier in this file (look for the runtime-add hook that creates
// a Jolt body when physics is already running).

// CharacterController3DComponent specialization is defined inside the
// OloEngine namespace earlier in this file (look for the runtime-add hook
// that creates a JoltCharacterController when physics is already running).

// ============================================================================
// OnComponentRemoved specializations. The pure no-ops are generated by
// OloHeaderTool (the #include below); the hand-written ones here release an
// external resource (Box2D body, audio SoundGraph source, video decode thread)
// or drop cached runtime state. The 3D-physics teardown specializations live in
// the OloEngine namespace earlier in this file, alongside their add hooks.
// ============================================================================

namespace OloEngine
{

#define OLO_ON_COMPONENT_REMOVED_NOOP(T) \
    template<>                           \
    void Scene::OnComponentRemoved<T>(Entity, T&) {}

    // Generated no-op OnComponentRemoved specializations — the mirror of the
    // OnComponentAdded include earlier in this file. Components whose removal
    // releases an external resource or drops cached runtime state are hand-written:
    // the 3D-physics ones in the specialization block above, the rest just below.
#include "OloEngine/Scene/Generated/OnComponentRemoved.Generated.inl"

    // Specialisation: when a Rigidbody2DComponent is removed at runtime,
    // the Box2D world must release the body. Without this hook, the body
    // leaks and the per-tick sync loop later reads a stale b2BodyId.
    template<>
    void Scene::OnComponentRemoved<Rigidbody2DComponent>(Entity, Rigidbody2DComponent& component)
    {
        if (b2World_IsValid(m_PhysicsWorld) && b2Body_IsValid(component.RuntimeBody))
        {
            b2DestroyBody(component.RuntimeBody);
            component.RuntimeBody = b2_nullBodyId;
        }
    }

    template<>
    void Scene::OnComponentRemoved<AudioSoundGraphComponent>(Entity, AudioSoundGraphComponent& component)
    {
        // Mirror the runtime-stop teardown sequence (Scene.cpp ~L781:
        // Stop → ReleaseResources → null). Without ReleaseResources the
        // SoundGraphSource stays attached to ma_engine until the Ref destructor
        // drops it, which races against the audio callback. Explicit teardown
        // detaches the source synchronously before the registry erases the
        // component struct.
        if (component.Sound)
        {
            component.Sound->Stop();
            component.Sound->ReleaseResources();
            component.Sound = nullptr;
        }
    }
    // Explicitly unload the player so its decode thread is joined synchronously before the
    // registry erases the component (rather than waiting on the Ref destructor).
    template<>
    void Scene::OnComponentRemoved<VideoOverlayComponent>(Entity, VideoOverlayComponent& component)
    {
        if (component.Player)
        {
            component.Player->Unload();
            component.Player = nullptr;
        }
    }
    template<>
    void Scene::OnComponentRemoved<VideoSurfaceComponent>(Entity, VideoSurfaceComponent& component)
    {
        if (component.Player)
        {
            component.Player->Unload();
            component.Player = nullptr;
        }
    }
    OLO_ON_COMPONENT_REMOVED_NOOP(Skeleton)

#undef OLO_ON_COMPONENT_REMOVED_NOOP

    // Specialisation: when a SpringBoneComponent is removed, drop the cached
    // runtime simulation state so a re-added component starts fresh from the
    // animated pose instead of popping from stale Verlet history.
    // (Defined after the SpringBoneStateComponent no-op above so the
    // RemoveComponent instantiation sees that explicit specialisation.)
    template<>
    void Scene::OnComponentRemoved<SpringBoneComponent>(Entity entity, SpringBoneComponent& /*component*/)
    {
        if (entity.HasComponent<SpringBoneStateComponent>())
        {
            entity.RemoveComponent<SpringBoneStateComponent>();
        }
    }

    // Specialisation: when a NoiseAnimationComponent is removed, drop the cached
    // runtime phase accumulator so a re-added component restarts its noise phase
    // from zero. (Defined after the NoiseAnimationStateComponent no-op above so
    // the RemoveComponent instantiation sees that explicit specialisation.)
    template<>
    void Scene::OnComponentRemoved<NoiseAnimationComponent>(Entity entity, NoiseAnimationComponent& /*component*/)
    {
        if (entity.HasComponent<NoiseAnimationStateComponent>())
        {
            entity.RemoveComponent<NoiseAnimationStateComponent>();
        }
    }

} // namespace OloEngine
