#include "OloEnginePCH.h"
#include "Scene.h"
#include "Entity.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Components.h"
#include "Prefab.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/InstancePlacementAsset.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/ProceduralSky.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"
#include "OloEngine/Animation/BoneEntityUtils.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSystem.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationGraphAsset.h"
#include "OloEngine/Animation/AnimationGraphSystem.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/UI/UILayoutSystem.h"
#include "OloEngine/UI/UIRenderer.h"
#include "OloEngine/UI/UIInputSystem.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Localization/LocalizationSystem.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/Particle/ParticleRenderer.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Particle/TrailRenderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"
#include "OloEngine/Terrain/TerrainData.h"
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
#include "OloEngine/Gameplay/Inventory/InventorySystem.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Audio/AudioEvents/AudioEventsManager.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Project/Project.h"

#include <glm/glm.hpp>
#include <ranges>

// Box2D
#include <box2d/box2d.h>

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/MotionType.h>

namespace OloEngine
{
    static void DrawTextWithShadow(const TextComponent& text, const TransformComponent& transform, int entityID)
    {
        if (text.DropShadow)
        {
            glm::mat4 shadowTransform = glm::translate(transform.GetTransform(), glm::vec3(text.ShadowDistance, -text.ShadowDistance, 0.0f));
            Renderer2D::DrawString(text.TextString, text.FontAsset, shadowTransform, { text.ShadowColor, text.Kerning, text.LineSpacing, text.MaxWidth }, entityID);
        }
        Renderer2D::DrawString(text.TextString, transform.GetTransform(), text, entityID);
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
        : m_JoltScene(std::make_unique<JoltScene>(this))
    {
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
        // destroy this entity, invalidating the handle.
        UUID entityUUID = entity.GetUUID();

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

        m_Registry.destroy(entity);
        m_EntityMap.Remove(entityUUID);

        m_RuntimeSnowPrevPositions.Remove(entityUUID);
        m_EditorSnowPrevPositions.Remove(entityUUID);
    }

    void Scene::InitDialogueSystem()
    {
        m_DialogueSystem = std::make_unique<DialogueSystem>(this);
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
                const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
                ac.Listener->SetConfig(ac.Config);
                ac.Listener->SetPosition(tc.Translation);
                ac.Listener->SetDirection(-forward);
                break;
            }
        }

        for (auto sourceView = m_Registry.group<AudioSourceComponent>(entt::get<TransformComponent>); auto&& [e, ac, tc] : sourceView.each())
        {
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
                const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
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

        // Reset animation-time history so the first runtime frame seeds itself
        // (see OnUpdateRender's m_LastAnimationTime < 0.0f branch). Without
        // this, a stale value carried over from a previous edit-mode session
        // produces a huge dt on the first runtime frame, which shows up as
        // bogus motion vectors in TAA / motion blur for the wind / water /
        // foliage shaders that consume PrevAnimationTime.
        m_LastAnimationTime = -1.0f;

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
                }

                OLO_CORE_INFO("[Scene] Auto-baking NavMesh for {} agent(s)...",
                              std::distance(agentView.begin(), agentView.end()));

                NavMeshSettings settings;
                auto navMesh = NavMeshGenerator::Generate(this, settings, boundsMin, boundsMax);
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
    }

    void Scene::OnSimulationStart()
    {
        // Same reset as OnRuntimeStart — simulation mode also re-baselines the
        // animation clock so first-frame velocity reprojection isn't bogus.
        m_LastAnimationTime = -1.0f;

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

    void Scene::OnUpdateRuntime(Timestep const ts)
    {
        PerformanceProfiler* perfProfiler = nullptr;
        if (auto* app = Application::TryGet())
        {
            perfProfiler = app->GetPerformanceProfiler();
        }
        OLO_PERF_SCOPE("Scene::OnUpdateRuntime", perfProfiler);
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

        if (!m_IsPaused || m_StepFrames-- > 0)
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

            // Update dialogue system
            if (m_DialogueSystem)
            {
                OLO_PROFILE_SCOPE("DialogueSystem::Update");
                m_DialogueSystem->Update(ts);
            }

            // Update animations
            {
                auto animView = m_Registry.view<AnimationStateComponent, SkeletonComponent>();
                for (auto e : animView)
                {
                    auto& animState = animView.get<AnimationStateComponent>(e);
                    auto& skelComp = animView.get<SkeletonComponent>(e);

                    if (animState.m_IsPlaying && animState.m_CurrentClip && skelComp.m_Skeleton)
                    {
                        IKTargetComponent tempIk;
                        Entity entity = { e, this };
                        const IKTargetComponent* ikTarget = ResolveIKTargets(entity, tempIk) ? &tempIk : nullptr;
                        auto const& entityTransform = entity.GetComponent<TransformComponent>().GetTransform();
                        Animation::AnimationSystem::Update(animState, *skelComp.m_Skeleton, ts.GetSeconds(), ikTarget, entityTransform);

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

            // Evaluate morph targets for all entities with active weights
            // This runs after animation update so keyframe-driven weights are applied first.
            // Morph deformation happens before skeletal skinning (morph first, then skin).
            {
                OLO_PROFILE_SCOPE("Morph Target Evaluation");
                auto morphView = m_Registry.view<MorphTargetComponent, MeshComponent>();
                for (auto e : morphView)
                {
                    auto& morphComp = morphView.get<MorphTargetComponent>(e);

                    auto& meshComp = morphView.get<MeshComponent>(e);
                    if (!meshComp.m_MeshSource)
                        continue;

                    // Auto-populate MorphTargets from MeshSource if not already set
                    if (!morphComp.MorphTargets && meshComp.m_MeshSource->HasMorphTargets())
                    {
                        morphComp.MorphTargets = meshComp.m_MeshSource->GetMorphTargets();
                    }

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
                        continue;
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
                        continue;

                    // Evaluate morph deformation
                    std::vector<glm::vec3> outPositions;
                    std::vector<glm::vec3> outNormals;
                    if (MorphTargetSystem::EvaluateMorphTargets(morphComp,
                                                                morphComp.BasePositions, morphComp.BaseNormals,
                                                                outPositions, outNormals))
                    {
                        // Write deformed data back into MeshSource vertices
                        auto& mutableVerts = meshSource->GetVertices();
                        for (u32 i = 0; i < static_cast<u32>(outPositions.size()) && i < static_cast<u32>(mutableVerts.Num()); ++i)
                        {
                            mutableVerts[i].Position = outPositions[i];
                            mutableVerts[i].Normal = outNormals[i];
                        }

                        // Re-upload vertex data to the GPU
                        auto& vb = const_cast<Ref<VertexBuffer>&>(meshSource->GetVertexBuffer());
                        vb->SetData({ mutableVerts.GetData(), static_cast<u32>(mutableVerts.Num() * sizeof(Vertex)) });
                    }
                    morphComp.WasMorphActive = true;
                }
            }

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
                        auto const& graphEntityTransform = graphEntity.GetComponent<TransformComponent>().GetTransform();
                        Animation::AnimationGraphSystem::Update(graphComp, *skelComp.m_Skeleton, ts.GetSeconds(), graphIkTarget, graphEntityTransform);
                    }
                }
            }

            // Physics
            {
                const i32 velocityIterations = 6;
                // const i32 positionIterations = 2; // TODO: Use this parameter when implementing position iterations
                // Guard against ticking before OnPhysics2DStart created a world —
                // the Jolt path below already does this; matching it keeps the
                // tick safe for headless tests / minimal scenes that never opt
                // in to 2D physics.
                if (b2World_IsValid(m_PhysicsWorld))
                {
                    b2World_Step(m_PhysicsWorld, ts.GetSeconds(), velocityIterations);
                }

                // Update 3D physics
                if (m_JoltScene)
                {
                    m_JoltScene->Simulate(ts.GetSeconds());
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

                // Retrieve transforms from Jolt 3D physics
                for (const auto view = m_Registry.view<Rigidbody3DComponent, TransformComponent>(); const auto e : view)
                {
                    Entity entity = { e, this };
                    auto& transform = entity.GetComponent<TransformComponent>();
                    const auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();

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

            // Update navigation / pathfinding
            NavigationSystem::OnUpdate(this, ts.GetSeconds());

            // Update AI (behavior trees and state machines)
            AISystem::OnUpdate(this, ts.GetSeconds());

            // Update inventory system (pickups, despawn)
            InventorySystem::OnUpdate(this, ts.GetSeconds());

            // Update quest system (timers, conditions)
            QuestSystem::OnUpdate(this, ts.GetSeconds());

            // Update gameplay ability system (abilities, effects, cooldowns)
            GameplayAbilitySystem::OnUpdate(this, ts.GetSeconds());

            auto listenerView = m_Registry.group<AudioListenerComponent>(entt::get<TransformComponent>);
            for (auto&& [e, ac, tc] : listenerView.each())
            {
                if (ac.Active)
                {
                    const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
                    const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
                    ac.Listener->SetPosition(tc.Translation);
                    ac.Listener->SetDirection(-forward);
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
                    const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
                    ac.Source->SetPosition(tc.Translation);
                    ac.Source->SetDirection(forward);
                }
            }

            // Process audio events queue
            if (m_AudioEventsManager)
            {
                m_AudioEventsManager->Update(ts);
            }

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

                for (auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>(); auto entity : view)
                {
                    const auto& transform = view.get<TransformComponent>(entity);
                    auto& psc = view.get<ParticleSystemComponent>(entity);
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

            // Process snow deformer entities — submit deformation stamps and emit ejecta
            ProcessSnowDeformers(ts, m_RuntimeSnowPrevPositions);
        }

        // Find the primary camera
        Camera const* mainCamera = nullptr;
        glm::mat4 cameraTransform;
        {
            for (const auto view = m_Registry.view<TransformComponent, CameraComponent>(); const auto entity : view)
            {
                auto& transform = view.get<TransformComponent>(entity);
                const auto& camera = view.get<CameraComponent>(entity);

                if (camera.Primary)
                {
                    // FPS fly-camera controls: WASD/QE movement + mouse look
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
                    break;
                }
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
            UIInputSystem::ProcessInput(*this, mousePos, mouseDown, mousePressed);
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
                                                                 const auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);
                                                                 Renderer2D::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(std::to_underlying(entity)));
                                                             }

                                                             for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
                                                             {
                                                                 const auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);
                                                                 Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(std::to_underlying(entity)));
                                                             }

                                                             for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
                                                             {
                                                                 const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);
                                                                 DrawTextWithShadow(text, transform, static_cast<int>(std::to_underlying(entity)));
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
                    const auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);
                    Renderer2D::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(std::to_underlying(entity)));
                }

                for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
                {
                    const auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);
                    Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(std::to_underlying(entity)));
                }

                for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
                {
                    const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);
                    DrawTextWithShadow(text, transform, static_cast<int>(std::to_underlying(entity)));
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

            // Physics
            {
                const i32 velocityIterations = 6;
                // const i32 positionIterations = 2; // TODO: Use this parameter when implementing position iterations
                // Guard against ticking before OnPhysics2DStart created a world —
                // the Jolt path below already does this; matching it keeps the
                // tick safe for headless tests / minimal scenes that never opt
                // in to 2D physics.
                if (b2World_IsValid(m_PhysicsWorld))
                {
                    b2World_Step(m_PhysicsWorld, ts.GetSeconds(), velocityIterations);
                }

                // Update 3D physics
                if (m_JoltScene)
                {
                    m_JoltScene->Simulate(ts.GetSeconds());
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

                // Retrieve transforms from Jolt 3D physics
                for (const auto view = m_Registry.view<Rigidbody3DComponent, TransformComponent>(); const auto e : view)
                {
                    Entity entity = { e, this };
                    auto& transform = entity.GetComponent<TransformComponent>();
                    const auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();

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
        }

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
        // Scene streaming update (editor preview)
        if (m_SceneStreamer)
        {
            ++m_StreamingFrameCounter;
            m_SceneStreamer->Update(camera.GetPosition(), m_StreamingFrameCounter);
        }

        // Update particle systems so they preview in the editor
        {
            const glm::vec3 camPos = camera.GetPosition();
            for (auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>(); auto entity : view)
            {
                const auto& transform = view.get<TransformComponent>(entity);
                auto& psc = view.get<ParticleSystemComponent>(entity);
                psc.System.UpdateLOD(camPos, transform.Translation);
                psc.System.Update(ts, transform.Translation, glm::vec3(0.0f), transform.GetRotation());

                // Process sub-emitter triggers for child systems
                ProcessChildSubEmitters(psc, ts, transform.Translation);
            }
        }

        // Process snow deformer entities in editor preview
        ProcessSnowDeformers(ts, m_EditorSnowPrevPositions);

        // Update animations so they preview in the editor (IK responds to target movement)
        {
            auto animView = m_Registry.view<AnimationStateComponent, SkeletonComponent>();
            for (auto e : animView)
            {
                auto& animState = animView.get<AnimationStateComponent>(e);
                auto& skelComp = animView.get<SkeletonComponent>(e);

                if (animState.m_IsPlaying && animState.m_CurrentClip && skelComp.m_Skeleton)
                {
                    IKTargetComponent tempIk;
                    Entity entity = { e, this };
                    const IKTargetComponent* ikTarget = ResolveIKTargets(entity, tempIk) ? &tempIk : nullptr;
                    auto const& entityTransform = entity.GetComponent<TransformComponent>().GetTransform();
                    Animation::AnimationSystem::Update(animState, *skelComp.m_Skeleton, ts.GetSeconds(), ikTarget, entityTransform);

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
                auto& morphComp = morphView.get<MorphTargetComponent>(e);
                auto& meshComp = morphView.get<MeshComponent>(e);
                if (!meshComp.m_MeshSource)
                    continue;

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
                    continue;
                }

                auto& meshSource = meshComp.m_MeshSource;
                auto& vertices = meshSource->GetVertices();

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
                    continue;

                std::vector<glm::vec3> outPositions;
                std::vector<glm::vec3> outNormals;
                if (MorphTargetSystem::EvaluateMorphTargets(morphComp,
                                                            morphComp.BasePositions, morphComp.BaseNormals,
                                                            outPositions, outNormals))
                {
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
                                                                 const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);
                                                                 DrawTextWithShadow(text, transform, static_cast<int>(std::to_underlying(entity)));
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

    // Animation/ECS explicit specializations
    template<>
    void Scene::OnComponentAdded<MeshComponent>(Entity, MeshComponent&) {}
    template<>
    void Scene::OnComponentAdded<InstancedMeshComponent>(Entity, InstancedMeshComponent&) {}
    template<>
    void Scene::OnComponentAdded<ModelComponent>(Entity, ModelComponent&) {}
    template<>
    void Scene::OnComponentAdded<SubmeshComponent>(Entity, SubmeshComponent&) {}
    template<>
    void Scene::OnComponentAdded<AnimationStateComponent>(Entity, AnimationStateComponent&) {}
    template<>
    void Scene::OnComponentAdded<MorphTargetComponent>(Entity, MorphTargetComponent&) {}
    template<>
    void Scene::OnComponentAdded<Skeleton>(Entity, Skeleton&) {}
    // If you use SkeletonComponent as a struct, add this too:
    template<>
    void Scene::OnComponentAdded<SkeletonComponent>(Entity, SkeletonComponent&) {}
    template<>
    void Scene::OnComponentAdded<MaterialComponent>(Entity, MaterialComponent&) {}
    template<>
    void Scene::OnComponentAdded<DirectionalLightComponent>(Entity, DirectionalLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<PointLightComponent>(Entity, PointLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<SphereAreaLightComponent>(Entity, SphereAreaLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<SpotLightComponent>(Entity, SpotLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<EnvironmentMapComponent>(Entity, EnvironmentMapComponent&) {}
    template<>
    void Scene::OnComponentAdded<ProceduralSkyComponent>(Entity, ProceduralSkyComponent&) {}
    template<>
    void Scene::OnComponentAdded<TerrainComponent>(Entity, TerrainComponent&) {}
    template<>
    void Scene::OnComponentAdded<FoliageComponent>(Entity, FoliageComponent&) {}
    template<>
    void Scene::OnComponentAdded<WaterComponent>(Entity, WaterComponent&) {}
    template<>
    void Scene::OnComponentAdded<SnowDeformerComponent>(Entity, SnowDeformerComponent&) {}
    template<>
    void Scene::OnComponentAdded<FogVolumeComponent>(Entity, FogVolumeComponent&) {}
    template<>
    void Scene::OnComponentAdded<DecalComponent>(Entity, DecalComponent&) {}
    template<>
    void Scene::OnComponentAdded<LODGroupComponent>(Entity, LODGroupComponent&) {}
    template<>
    void Scene::OnComponentAdded<LightProbeComponent>(Entity, LightProbeComponent&) {}
    template<>
    void Scene::OnComponentAdded<LightProbeVolumeComponent>(Entity, LightProbeVolumeComponent&) {}
    template<>
    void Scene::OnComponentAdded<ReflectionProbeComponent>(Entity, ReflectionProbeComponent&) {}
    template<>
    void Scene::OnComponentAdded<DialogueComponent>(Entity, DialogueComponent&) {}
    template<>
    void Scene::OnComponentAdded<DialogueStateComponent>(Entity, DialogueStateComponent&) {}
    template<>
    void Scene::OnComponentAdded<NavMeshBoundsComponent>(Entity, NavMeshBoundsComponent&) {}
    template<>
    void Scene::OnComponentAdded<NavAgentComponent>(Entity, NavAgentComponent&) {}
    template<>
    void Scene::OnComponentAdded<AnimationGraphComponent>(Entity, AnimationGraphComponent&) {}
    template<>
    void Scene::OnComponentAdded<BehaviorTreeComponent>(Entity, BehaviorTreeComponent&) {}
    template<>
    void Scene::OnComponentAdded<StateMachineComponent>(Entity, StateMachineComponent&) {}
    template<>
    void Scene::OnComponentAdded<TileRendererComponent>(Entity, TileRendererComponent&) {}

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
        if (m_JoltScene && component.m_RuntimeBodyToken != 0)
        {
            m_JoltScene->DestroyBody(entity);
            component.m_RuntimeBodyToken = 0;
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

        return true;
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
                shapeDef.friction = bc2d.Friction;
                shapeDef.restitution = bc2d.Restitution;

                b2Polygon polygon = b2MakeOffsetBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y,
                                                    { bc2d.Offset.x, bc2d.Offset.y }, 0.0f);
                b2CreatePolygonShape(body, &shapeDef, &polygon);
            }

            if (entity.HasComponent<CircleCollider2DComponent>())
            {
                auto const& cc2d = entity.GetComponent<CircleCollider2DComponent>();

                b2ShapeDef shapeDef = b2DefaultShapeDef();
                shapeDef.density = cc2d.Density;
                shapeDef.friction = cc2d.Friction;
                shapeDef.restitution = cc2d.Restitution;

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

    void Scene::OnPhysics3DStart()
    {
        // Ensure JoltScene was properly initialized in constructor
        OLO_CORE_ASSERT(m_JoltScene, "JoltScene should be initialized in constructor");

        m_JoltScene->Initialize();

        if (!m_JoltScene->IsInitialized())
        {
            OLO_CORE_ERROR("Failed to initialize 3D physics system");
            return;
        }

        // Create physics bodies for all entities with Rigidbody3DComponent
        auto view = m_Registry.view<Rigidbody3DComponent, TransformComponent>();
        for (auto entity : view)
        {
            Entity ent = { entity, this };

            // Create the physics body - JoltScene will handle shape creation based on components
            auto body = m_JoltScene->CreateBody(ent);
            if (body)
            {
                auto& rb3d = ent.GetComponent<Rigidbody3DComponent>();
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
    }

    void Scene::OnPhysics3DStop()
    {
        // Early return if JoltScene is null to avoid null dereference
        if (!m_JoltScene)
        {
            return;
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

        m_JoltScene->Shutdown();
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
                const auto [transform, sprite] = view.get<TransformComponent, SpriteRendererComponent>(entity);

                Renderer2D::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(std::to_underlying(entity)));
            }
        }

        // Draw circles
        {
            for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
            {
                const auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

                Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(std::to_underlying(entity)));
            }
        }

        // Draw text
        {
            for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
            {
                const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);
                DrawTextWithShadow(text, transform, static_cast<int>(std::to_underlying(entity)));
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

                // Detect dirtiness via parameter hash; rebake on change. The
                // bake is expensive (six cubemap face renders + IBL convolve)
                // so we deliberately gate on the hash rather than rebake every
                // frame.
                PreethamParameters params;
                params.SunDirection = sky.m_SunDirection;
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

                // Also set single-light for backward compatibility with CommandDispatch
                if (lightIndex == 0)
                {
                    Light light;
                    light.Type = LightType::Directional;
                    light.Direction = dirLight.m_Direction;
                    light.Ambient = dirLight.m_Color * 0.1f;
                    light.Diffuse = dirLight.m_Color * dirLight.m_Intensity;
                    light.Specular = dirLight.m_Color * dirLight.m_Intensity;
                    Renderer3D::SetLight(light);
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

            // Collect point lights
            u32 pointShadowIndex = 0;
            auto pointLightView = m_Registry.view<TransformComponent, PointLightComponent>();
            for (auto entity : pointLightView)
            {
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    break;
                }

                const auto& [transform, pointLight] = pointLightView.get<TransformComponent, PointLightComponent>(entity);

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
            for (auto entity : spotLightView)
            {
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    break;
                }

                const auto& [transform, spotLight] = spotLightView.get<TransformComponent, SpotLightComponent>(entity);

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
                    data.Direction = glm::vec4(spotLight.m_Direction, static_cast<f32>(spotShadowIndex));
                    ++spotShadowIndex;
                }
                else
                {
                    data.Direction = glm::vec4(spotLight.m_Direction, -1.0f);
                }

                ++lightIndex;
            }

            // Collect sphere area lights. Packed into MultiLightData with the
            // SPHERE_AREA_LIGHT type tag (w=3) and the emitter sphere radius
            // stored in SpotParams.z — see PBRCommon.glsl for the decoder side.
            auto sphereAreaLightView = m_Registry.view<TransformComponent, SphereAreaLightComponent>();
            for (auto entity : sphereAreaLightView)
            {
                if (lightIndex >= static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS))
                {
                    break;
                }

                const auto& [transform, areaLight] = sphereAreaLightView.get<TransformComponent, SphereAreaLightComponent>(entity);

                auto& data = multiLightData.Lights[lightIndex];
                data.Position = glm::vec4(transform.Translation, 3.0f); // w=3 for sphere area
                data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, -1.0f);   // no shadow path yet
                data.Color = glm::vec4(areaLight.m_Color, areaLight.m_Intensity);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, areaLight.m_Range);
                data.SpotParams = glm::vec4(0.0f, 0.0f, areaLight.m_Radius, 3.0f); // type = SPHERE_AREA_LIGHT = 3

                ++lightIndex;
            }

            multiLightData.LightCount = lightIndex;
            multiLightData.MaxLights = static_cast<i32>(UBOStructures::MultiLightUBO::MAX_LIGHTS);
            multiLightData.DirectionalLightCount = directionalLightCount;
            Renderer3D::UploadMultiLightUBO(multiLightData, lightIndex);

            // Gather lights for Forward+ tile-based culling
            Renderer3D::GetForwardPlus().GatherLights(*this);

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
                        spotLight.m_Direction,
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
                                terrain.m_TerrainData->GenerateProcedural(
                                    terrain.m_ProceduralResolution,
                                    terrain.m_ProceduralSeed,
                                    terrain.m_ProceduralOctaves,
                                    terrain.m_ProceduralFrequency,
                                    1.0f,
                                    terrain.m_ProceduralLacunarity,
                                    terrain.m_ProceduralPersistence);
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
                    }

                    // Build / rebuild terrain material texture arrays
                    if (terrain.m_MaterialNeedsRebuild && terrain.m_Material && terrain.m_Material->GetLayerCount() > 0)
                    {
                        terrain.m_Material->BuildTextureArrays();
                        terrain.m_Material->LoadSplatmaps();
                        terrain.m_MaterialNeedsRebuild = false;
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
                const bool hasActiveShadows = Renderer3D::IsShadowPassAvailable() && Renderer3D::GetShadowMap().IsEnabled();

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
                auto waterView = m_Registry.view<TransformComponent, WaterComponent>();
                for (auto entity : waterView)
                {
                    auto const& [transform, water] = waterView.get<TransformComponent, WaterComponent>(entity);
                    if (!water.m_Enabled)
                    {
                        continue;
                    }

                    // Lazy mesh initialization / rebuild
                    if (water.m_NeedsRebuild || !water.m_WaterMesh)
                    {
                        auto const sizeX = std::isfinite(water.m_WorldSizeX)
                                               ? std::clamp(water.m_WorldSizeX, 0.1f, 10000.0f)
                                               : 100.0f;
                        auto const sizeZ = std::isfinite(water.m_WorldSizeZ)
                                               ? std::clamp(water.m_WorldSizeZ, 0.1f, 10000.0f)
                                               : 100.0f;
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
                    f32 waveH = safeAmplitude * 2.0f; // Conservative height estimate
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

        // Shadow pass setup for mesh entity traversal
        const bool meshHasActiveShadows = Renderer3D::IsShadowPassAvailable() && Renderer3D::GetShadowMap().IsEnabled();

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

                const auto [transform, mesh] = view.get<TransformComponent, MeshComponent>(entity);

                if (!mesh.m_MeshSource)
                {
                    continue;
                }

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
                        if (auto* packet = Renderer3D::DrawMesh(submesh, transform.GetTransform(), material, true, entityID, lodGroup); packet)
                            Renderer3D::SubmitPacket(packet);

                        // Shadow caster for this submesh
                        if (castsShadow && submesh)
                        {
                            auto va = submesh->GetVertexArray();
                            if (va)
                            {
                                Renderer3D::AddMeshShadowCaster(
                                    va->GetRendererID(), submesh->GetIndexCount(), submesh->GetBaseIndex(),
                                    transform.GetTransform(), GetShadowVaoID(submesh),
                                    submesh->GetTransformedBoundingBox(transform.GetTransform()));
                            }
                        }
                    }
                }
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
                const auto [transform, submesh] = view.get<TransformComponent, SubmeshComponent>(entity);

                if (!submesh.m_Mesh || !submesh.m_Visible)
                {
                    continue;
                }

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

                if (auto* packet = Renderer3D::DrawMesh(submesh.m_Mesh, transform.GetTransform(), material, true, entityID, lodGroup); packet)
                    Renderer3D::SubmitPacket(packet);

                // Shadow caster for this submesh entity
                if (castsShadow && submesh.m_Mesh)
                {
                    auto va = submesh.m_Mesh->GetVertexArray();
                    if (va)
                    {
                        Renderer3D::AddMeshShadowCaster(
                            va->GetRendererID(), submesh.m_Mesh->GetIndexCount(), submesh.m_Mesh->GetBaseIndex(),
                            transform.GetTransform(), GetShadowVaoID(submesh.m_Mesh),
                            submesh.m_Mesh->GetTransformedBoundingBox(transform.GetTransform()));
                    }
                }
            }
        }

        // Draw model entities (full models with materials from file)
        {
            auto view = m_Registry.view<TransformComponent, ModelComponent>();
            for (auto entity : view)
            {
                const auto [transform, model] = view.get<TransformComponent, ModelComponent>(entity);

                if (!model.m_Model || !model.m_Visible)
                {
                    continue;
                }

                // Model::DrawParallel uses the model's own materials loaded from file
                // Pass entity ID for mouse picking support
                const auto modelTransform = transform.GetTransform();
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
                const auto [transform, mesh, skeleton] = view.get<TransformComponent, MeshComponent, SkeletonComponent>(entity);

                if (!mesh.m_MeshSource || !skeleton.m_Skeleton)
                {
                    continue;
                }

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
                        auto* packet = Renderer3D::DrawAnimatedMesh(submesh, transform.GetTransform(), material, boneMatrices, prevBoneMatrices, false, entityID);
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
                                            transform.GetTransform(),
                                            cmd->boneBufferOffset, cmd->boneCount,
                                            submesh->GetTransformedBoundingBox(transform.GetTransform()));
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
                        transform.GetTransform(),
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
                const auto [transform, tileComp] = view.get<TransformComponent, TileRendererComponent>(entity);

                if (!tileComp.TileMesh || !tileComp.TileMesh->IsValid())
                {
                    continue;
                }

                i32 entityID = static_cast<i32>(std::to_underlying(entity));
                glm::mat4 baseTransform = transform.GetTransform();

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
                const auto& [transform, cameraComp] = view.get<TransformComponent, CameraComponent>(entity);
                const SceneCamera& sceneCamera = cameraComp.Camera;

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
                        transform.GetTransform(),
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
                        transform.GetTransform(),
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
                    const auto& [tc, mc] = view.get<TransformComponent, MeshComponent>(entity);
                    if (!mc.m_MeshSource)
                        continue;
                    drawWorldAABB(mc.m_MeshSource->GetBoundingBox().Transform(tc.GetTransform()));
                }
            }

            // ModelComponent entities
            {
                auto view = m_Registry.view<TransformComponent, ModelComponent>();
                for (auto entity : view)
                {
                    const auto& [tc, modelComp] = view.get<TransformComponent, ModelComponent>(entity);
                    if (!modelComp.m_Model || !modelComp.m_Visible)
                        continue;
                    drawWorldAABB(modelComp.m_Model->GetTransformedBoundingBox(tc.GetTransform()));
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

        // Sort particle systems back-to-front for correct alpha blending
        auto psView = m_Registry.view<TransformComponent, ParticleSystemComponent>();
        std::vector<std::pair<f32, entt::entity>> sortedSystems;
        sortedSystems.reserve(psView.size_hint());
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

// Template specializations for component callbacks
template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::IDComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::IDComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TransformComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TransformComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CameraComponent>([[maybe_unused]] OloEngine::Entity entity, OloEngine::CameraComponent& component)
{
    if ((m_ViewportWidth > 0) && (m_ViewportHeight > 0))
    {
        component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
    }
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ScriptComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ScriptComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::LuaScriptComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::LuaScriptComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::SpriteRendererComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::SpriteRendererComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CircleRendererComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CircleRendererComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TagComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TagComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::Rigidbody2DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::Rigidbody2DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::BoxCollider2DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::BoxCollider2DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CircleCollider2DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CircleCollider2DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TextComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TextComponent& component)
{
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

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::AudioSourceComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::AudioSourceComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::AudioListenerComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::AudioListenerComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::RelationshipComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::RelationshipComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::PrefabComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::PrefabComponent& component)
{
}

// Rigidbody3DComponent specialization is defined inside the OloEngine
// namespace earlier in this file (look for the runtime-add hook that creates
// a Jolt body when physics is already running).

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::BoxCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::BoxCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::SphereCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::SphereCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CapsuleCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CapsuleCollider3DComponent& component)
{
}

// CharacterController3DComponent specialization is defined inside the
// OloEngine namespace earlier in this file (look for the runtime-add hook
// that creates a JoltCharacterController when physics is already running).

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::MeshCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::MeshCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ConvexMeshCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ConvexMeshCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TriangleMeshCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TriangleMeshCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UICanvasComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UICanvasComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIRectTransformComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIRectTransformComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIResolvedRectComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIResolvedRectComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIImageComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIImageComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIPanelComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIPanelComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UITextComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UITextComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIButtonComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIButtonComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UISliderComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UISliderComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UICheckboxComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UICheckboxComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIProgressBarComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIProgressBarComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIWorldAnchorComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIWorldAnchorComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIInputFieldComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIInputFieldComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIScrollViewComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIScrollViewComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIDropdownComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIDropdownComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIGridLayoutComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIGridLayoutComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIToggleComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIToggleComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ParticleSystemComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ParticleSystemComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::StreamingVolumeComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::StreamingVolumeComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::NetworkIdentityComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::NetworkIdentityComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::NetworkInterestComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::NetworkInterestComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::PhaseComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::PhaseComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::InstancePortalComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::InstancePortalComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::NetworkLODComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::NetworkLODComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::InventoryComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::InventoryComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ItemPickupComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ItemPickupComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ItemContainerComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ItemContainerComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::QuestJournalComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::QuestJournalComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::QuestGiverComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::QuestGiverComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::AbilityComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::AbilityComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::NameplateComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::NameplateComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::IKTargetComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::IKTargetComponent& component)
{
}

// ============================================================================
// OnComponentRemoved specialisations — exhaustive no-op list mirroring the
// OnComponentAdded set above. Component types whose removal needs to release
// external resources (Jolt body, Box2D body, etc.) are specialised inside
// the OloEngine namespace earlier in this file.
// ============================================================================

namespace OloEngine
{

#define OLO_ON_COMPONENT_REMOVED_NOOP(T) \
    template<>                           \
    void Scene::OnComponentRemoved<T>(Entity, T&) {}

    OLO_ON_COMPONENT_REMOVED_NOOP(IDComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(TransformComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(TagComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(RelationshipComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(CameraComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ScriptComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(LuaScriptComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SpriteRendererComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(CircleRendererComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(TextComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(LocalizedTextComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(MeshComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(InstancedMeshComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ModelComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SubmeshComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(AnimationStateComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(MorphTargetComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SkeletonComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(MaterialComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(DirectionalLightComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(PointLightComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SpotLightComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SphereAreaLightComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(EnvironmentMapComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ProceduralSkyComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(TerrainComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(FoliageComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(WaterComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SnowDeformerComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(FogVolumeComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(DecalComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(LODGroupComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(LightProbeComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(LightProbeVolumeComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ReflectionProbeComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(DialogueComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(DialogueStateComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(NavMeshBoundsComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(NavAgentComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(AnimationGraphComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(BehaviorTreeComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(StateMachineComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(TileRendererComponent)

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

    OLO_ON_COMPONENT_REMOVED_NOOP(BoxCollider2DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(CircleCollider2DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(BoxCollider3DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(SphereCollider3DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(CapsuleCollider3DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(MeshCollider3DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ConvexMeshCollider3DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(TriangleMeshCollider3DComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(AudioSourceComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(AudioListenerComponent)
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
    OLO_ON_COMPONENT_REMOVED_NOOP(PrefabComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UICanvasComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIRectTransformComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIResolvedRectComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIImageComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIPanelComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UITextComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIButtonComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UISliderComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UICheckboxComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIProgressBarComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIWorldAnchorComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIInputFieldComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIScrollViewComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIDropdownComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIGridLayoutComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(UIToggleComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ParticleSystemComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(StreamingVolumeComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(NetworkIdentityComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(NetworkInterestComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(PhaseComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(InstancePortalComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(NetworkLODComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(InventoryComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ItemPickupComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(ItemContainerComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(QuestJournalComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(QuestGiverComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(AbilityComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(NameplateComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(IKTargetComponent)
    OLO_ON_COMPONENT_REMOVED_NOOP(Skeleton)

#undef OLO_ON_COMPONENT_REMOVED_NOOP

} // namespace OloEngine
