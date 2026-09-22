#include "OloEnginePCH.h"
#include "SceneStreamer.h"
#include "StreamingRegionSerializer.h"
#include "StreamingVolumeComponent.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Threading/UniqueLock.h"

// Box2D
#include <box2d/box2d.h>

#include <algorithm>
#include <filesystem>

namespace OloEngine
{
    [[nodiscard("Store this!")]] static b2BodyType ToBox2DBodyType(const Rigidbody2DComponent::BodyType bodyType)
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

    SceneStreamer::~SceneStreamer()
    {
        Shutdown();
    }

    void SceneStreamer::Initialize(Scene* scene, const SceneStreamerConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        m_Scene = scene;
        m_Config = config;
        m_CurrentFrame = 0;
        m_PendingLoads.Reset();

        DiscoverRegions();
    }

    void SceneStreamer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        // Wait for pending loads to complete
        for (const auto& pending : m_PendingLoads)
        {
            if (pending.Task.IsValid())
            {
                pending.Task.Wait();
            }
        }
        m_PendingLoads.Reset();

        // Unload all ready regions
        TArray<RegionID> toUnload;
        {
            TUniqueLock<FMutex> lock(m_RegionMutex);
            for (auto& [id, region] : m_Regions)
            {
                if (region->m_State == StreamingRegion::State::Ready)
                {
                    toUnload.Add(id);
                }
            }
        }

        for (auto id : toUnload)
        {
            UnloadRegion(id);
        }

        TUniqueLock<FMutex> lock(m_RegionMutex);
        m_Regions.clear();
        m_Scene = nullptr;
    }

    void SceneStreamer::DiscoverRegions()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Config.RegionDirectory.IsEmpty())
        {
            return;
        }

        std::filesystem::path regionDir(m_Config.RegionDirectory.ToStdString());
        if (!std::filesystem::exists(regionDir))
        {
            OLO_CORE_WARN("SceneStreamer: Region directory does not exist: {0}", m_Config.RegionDirectory.ToView());
            return;
        }

        TUniqueLock<FMutex> lock(m_RegionMutex);

        for (const auto& entry : std::filesystem::directory_iterator(regionDir))
        {
            if (entry.path().extension() != ".oloregion")
            {
                continue;
            }

            auto data = StreamingRegionSerializer::ParseRegionFile(entry.path());
            if (!data || !data["Region"])
            {
                continue;
            }

            auto meta = StreamingRegionSerializer::ReadMetadata(data);

            auto region = Ref<StreamingRegion>::Create();
            region->m_RegionID = meta.RegionID;
            region->m_Name = meta.Name;
            region->m_SourcePath = entry.path();
            region->m_BoundsMin = meta.BoundsMin;
            region->m_BoundsMax = meta.BoundsMax;
            region->m_State = StreamingRegion::State::Unloaded;

            m_Regions[meta.RegionID] = region;

            OLO_CORE_TRACE("SceneStreamer: Discovered region '{0}' (ID: {1})", meta.Name, static_cast<u64>(meta.RegionID));
        }
    }

    void SceneStreamer::Update(const glm::vec3& activationPoint, u64 frameNumber)
    {
        OLO_PROFILE_FUNCTION();

        m_CurrentFrame = frameNumber;

        // Sync config from Scene's authoritative StreamingSettings each frame
        if (m_Scene)
        {
            const auto& ss = m_Scene->m_StreamingSettings;
            m_Config.LoadRadius = ss.DefaultLoadRadius;
            m_Config.UnloadRadius = ss.DefaultUnloadRadius;
            m_Config.MaxLoadedRegions = ss.MaxLoadedRegions;
        }

        ProcessCompletedLoads();

        if (!m_Scene)
        {
            return;
        }

        // Query all streaming volume entities for distance-based activation
        auto view = m_Scene->GetAllEntitiesWith<StreamingVolumeComponent, TransformComponent>();
        for (auto&& [e, vol, tc] : view.each())
        {
            if (vol.ActivationMode == StreamingActivationMode::Manual)
            {
                continue;
            }

            // Squared-distance comparison (no sqrt)
            glm::vec3 volumeCenter = tc.Translation;
            f32 distSq = glm::dot(activationPoint - volumeCenter, activationPoint - volumeCenter);

            RegionID regionId(static_cast<u64>(vol.RegionAssetHandle));

            TDynamicUniqueLock<FMutex> lock(m_RegionMutex);
            auto it = m_Regions.find(regionId);
            if (it == m_Regions.end())
            {
                continue;
            }

            auto& region = it->second;

            if (distSq < vol.LoadRadius * vol.LoadRadius && region->m_State == StreamingRegion::State::Unloaded)
            {
                region->m_LastUsedFrame = frameNumber;
                RequestRegionLoad(regionId);
            }
            else if (distSq > vol.UnloadRadius * vol.UnloadRadius && region->m_State == StreamingRegion::State::Ready)
            {
                // Release lock before UnloadRegion (it acquires lock internally)
                lock.Unlock();
                UnloadRegion(regionId);
                vol.IsLoaded = false;
            }
            else if (region->m_State == StreamingRegion::State::Ready)
            {
                region->m_LastUsedFrame = frameNumber;
            }
            else
            {
                // No additional handling required.
            }
        }

        EvictOverBudget();
    }

    void SceneStreamer::LoadRegion(RegionID regionId)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_RegionMutex);
        auto it = m_Regions.find(regionId);
        if (it == m_Regions.end())
        {
            OLO_CORE_WARN("SceneStreamer: Region not found for manual load: {0}", static_cast<u64>(regionId));
            return;
        }

        if (it->second->m_State != StreamingRegion::State::Unloaded)
        {
            return;
        }

        RequestRegionLoad(regionId);
    }

    void SceneStreamer::UnloadRegion(RegionID regionId)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_RegionMutex);
        auto it = m_Regions.find(regionId);
        if (it == m_Regions.end() || it->second->m_State != StreamingRegion::State::Ready)
        {
            return;
        }

        auto& region = it->second;
        region->m_State = StreamingRegion::State::Unloading;

        // Physics bodies must be destroyed BEFORE entity destruction
        // 3D (Jolt)
        if (auto* jolt = m_Scene->GetPhysicsScene(); jolt)
        {
            for (auto uuid : region->m_EntityUUIDs)
            {
                if (auto entity = m_Scene->TryGetEntityWithUUID(uuid); entity)
                {
                    jolt->DestroyBody(*entity);
                }
            }
        }

        // 2D (Box2D)
        if (b2World_IsValid(m_Scene->m_PhysicsWorld))
        {
            for (auto uuid : region->m_EntityUUIDs)
            {
                if (auto entity = m_Scene->TryGetEntityWithUUID(uuid); entity)
                {
                    if (entity->HasComponent<Rigidbody2DComponent>())
                    {
                        auto& rb2d = entity->GetComponent<Rigidbody2DComponent>();
                        if (b2Body_IsValid(rb2d.RuntimeBody))
                        {
                            b2DestroyBody(rb2d.RuntimeBody);
                            rb2d.RuntimeBody = b2_nullBodyId;
                        }
                    }
                }
            }
        }

        // Destroy entities from scene
        for (auto uuid : region->m_EntityUUIDs)
        {
            if (auto entity = m_Scene->TryGetEntityWithUUID(uuid); entity)
            {
                m_Scene->DestroyEntity(*entity);
            }
        }

        region->m_EntityUUIDs.Reset();
        region->m_RawData.reset();
        region->m_State = StreamingRegion::State::Unloaded;

        OLO_CORE_TRACE("SceneStreamer: Unloaded region '{0}'", region->m_Name.ToView());
    }

    void SceneStreamer::SetActivationEntity(UUID entityId)
    {
        m_ActivationEntityId = entityId;
    }

    u32 SceneStreamer::GetLoadedRegionCount() const
    {
        TUniqueLock<FMutex> lock(m_RegionMutex);
        u32 count = 0;
        for (auto& [id, region] : m_Regions)
        {
            if (region->m_State == StreamingRegion::State::Ready)
            {
                ++count;
            }
        }
        return count;
    }

    u32 SceneStreamer::GetPendingLoadCount() const
    {
        return static_cast<u32>(m_PendingLoads.Num());
    }

    void SceneStreamer::RequestRegionLoad(RegionID id)
    {
        // Caller must hold m_RegionMutex
        auto it = m_Regions.find(id);
        if (it == m_Regions.end())
        {
            return;
        }

        auto& region = it->second;
        region->m_State = StreamingRegion::State::Loading;
        auto path = region->m_SourcePath;

        auto task = Tasks::Launch(
            "SceneRegionLoad",
            [region, path, &mutex = m_RegionMutex]() mutable -> bool
            {
                OLO_PROFILE_SCOPE("StreamingRegion::Parse");
                auto data = StreamingRegionSerializer::ParseRegionFile(path);
                if (!data || !data["Region"])
                {
                    return false;
                }
                TUniqueLock<FMutex> lock(mutex);
                region->m_RawData = std::move(data);
                region->m_State = StreamingRegion::State::Loaded;
                return true;
            },
            Tasks::ETaskPriority::BackgroundNormal);

        m_PendingLoads.Add(PendingLoad{ id, std::move(task), region });

        OLO_CORE_TRACE("SceneStreamer: Requested load for region '{0}'", region->m_Name.ToView());
    }

    void SceneStreamer::ProcessCompletedLoads()
    {
        OLO_PROFILE_FUNCTION();

        if (m_PendingLoads.IsEmpty())
        {
            return;
        }

        for (i32 index = 0; index < m_PendingLoads.Num();)
        {
            if (!m_PendingLoads[index].Task.IsCompleted())
            {
                ++index;
                continue;
            }

            bool success = m_PendingLoads[index].Task.GetResult();
            auto& region = m_PendingLoads[index].Region;

            // Read state and move raw data under mutex (written by worker thread under same lock)
            StreamingRegion::State regionState;
            YAML::Node rawData;
            {
                TUniqueLock<FMutex> lock(m_RegionMutex);
                regionState = region->m_State;
                rawData = std::move(region->m_RawData);
            }

            if (success && regionState == StreamingRegion::State::Loaded && m_Scene)
            {
                // Main-thread entity instantiation
                Ref<Scene> sceneRef{ m_Scene };
                SceneSerializer serializer{ sceneRef };
                if (auto entitiesNode = rawData["Entities"])
                {
                    auto createdUUIDs = serializer.DeserializeAdditive(entitiesNode);
                    region->m_EntityUUIDs.Reset(static_cast<i32>(createdUUIDs.Num()));
                    for (const UUID uuid : createdUUIDs)
                    {
                        region->m_EntityUUIDs.Add(uuid);
                    }
                }

                // Initialize subsystems for new entities
                InitializeStreamedEntities(region->m_EntityUUIDs);

                {
                    TUniqueLock<FMutex> lock(m_RegionMutex);
                    region->m_State = StreamingRegion::State::Ready;
                }

                // Update volume component IsLoaded flag
                auto volView = m_Scene->GetAllEntitiesWith<StreamingVolumeComponent>();
                for (auto&& [ve, vol] : volView.each())
                {
                    if (RegionID(static_cast<u64>(vol.RegionAssetHandle)) == m_PendingLoads[index].RegionId)
                    {
                        vol.IsLoaded = true;
                    }
                }

                OLO_CORE_TRACE("SceneStreamer: Region '{0}' is now Ready ({1} entities)",
                               region->m_Name.ToView(), region->m_EntityUUIDs.Num());
            }
            else if (!success)
            {
                OLO_CORE_ERROR("SceneStreamer: Failed to load region '{0}'", region->m_Name.ToView());
                TUniqueLock<FMutex> lock(m_RegionMutex);
                region->m_State = StreamingRegion::State::Unloaded;
            }
            else
            {
                // No additional handling required.
            }

            m_PendingLoads.RemoveAt(index);
        }
    }

    void SceneStreamer::EvictOverBudget()
    {
        OLO_PROFILE_FUNCTION();

        TDynamicUniqueLock<FMutex> lock(m_RegionMutex);

        // Count ready regions
        u32 readyCount = 0;
        for (auto& [id, region] : m_Regions)
        {
            if (region->m_State == StreamingRegion::State::Ready)
            {
                ++readyCount;
            }
        }

        if (readyCount <= m_Config.MaxLoadedRegions)
        {
            return;
        }

        // Collect ready regions sorted by LRU frame
        struct ReadyRegion
        {
            RegionID Id;
            u64 LastUsedFrame;
        };
        TArray<ReadyRegion> sortedRegions;
        sortedRegions.Reserve(static_cast<i32>(readyCount));
        for (auto& [id, region] : m_Regions)
        {
            if (region->m_State == StreamingRegion::State::Ready)
            {
                sortedRegions.Add(ReadyRegion{ id, region->m_LastUsedFrame });
            }
        }

        std::ranges::sort(sortedRegions,
                          [](const auto& a, const auto& b)
                          { return a.LastUsedFrame < b.LastUsedFrame; });

        // Evict oldest until under budget
        u32 toEvict = readyCount - m_Config.MaxLoadedRegions;
        for (u32 i = 0; i < toEvict && i < static_cast<u32>(sortedRegions.Num()); ++i)
        {
            // Release lock before UnloadRegion (it acquires lock internally)
            lock.Unlock();
            UnloadRegion(sortedRegions[i].Id);
            lock.Lock();
        }
    }

    void SceneStreamer::InitializeStreamedEntities(const TArray<UUID>& entityUUIDs) const
    {
        OLO_PROFILE_FUNCTION();

        for (auto uuid : entityUUIDs)
        {
            auto optEntity = m_Scene->TryGetEntityWithUUID(uuid);
            if (!optEntity)
            {
                continue;
            }
            Entity entity = *optEntity;

            // 1. Physics bodies (after ALL components deserialized)
            // 3D (Jolt)
            if (entity.HasComponent<Rigidbody3DComponent>())
            {
                if (auto* jolt = m_Scene->GetPhysicsScene(); jolt)
                {
                    jolt->CreateBody(entity);
                }
            }

            // 2D (Box2D)
            if (entity.HasComponent<Rigidbody2DComponent>() && b2World_IsValid(m_Scene->m_PhysicsWorld))
            {
                auto const& transform = entity.GetComponent<TransformComponent>();
                auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

                b2BodyDef bodyDef = b2DefaultBodyDef();
                bodyDef.type = ToBox2DBodyType(rb2d.Type);
                bodyDef.position = { transform.Translation.x, transform.Translation.y };
                bodyDef.rotation = b2MakeRot(transform.GetRotationEuler().z);

                b2BodyId body = b2CreateBody(m_Scene->m_PhysicsWorld, &bodyDef);
                b2Body_SetFixedRotation(body, rb2d.FixedRotation);
                rb2d.RuntimeBody = body;

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

            // 2. Audio sources
            if (entity.HasComponent<AudioSourceComponent>() && entity.HasComponent<TransformComponent>())
            {
                auto& ac = entity.GetComponent<AudioSourceComponent>();
                if (ac.Source)
                {
                    const auto& tc = entity.GetComponent<TransformComponent>();
                    ac.Source->SetConfig(ac.GetConfig());
                    ac.Source->SetPosition(tc.Translation);
                    if (ac.GetConfig().PlayOnAwake)
                    {
                        ac.Source->Play();
                    }
                }
            }

            // 3. Scripts (C# via Mono)
            if (entity.HasComponent<ScriptComponent>() && m_Scene->IsRunning())
            {
                ScriptEngine::OnCreateEntity(entity);
            }

            // 4. Animation state
            if (entity.HasComponent<AnimationStateComponent>())
            {
                auto& animState = entity.GetComponent<AnimationStateComponent>();
                if (animState.m_CurrentClip)
                {
                    animState.m_IsPlaying = true;
                    animState.m_CurrentTime = 0.0f;
                }
            }
        }
    }
} // namespace OloEngine
