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

    const char* ToString(StreamingRegion::State state) noexcept
    {
        switch (state)
        {
            case StreamingRegion::State::Unloaded:
                return "Unloaded";
            case StreamingRegion::State::Loading:
                return "Loading";
            case StreamingRegion::State::Ready:
                return "Ready";
            case StreamingRegion::State::Unloading:
                return "Unloading";
        }
        return "Unknown";
    }

    const char* ToString(EStreamingAdmissionStatus status) noexcept
    {
        switch (status)
        {
            case EStreamingAdmissionStatus::None:
                return "None";
            case EStreamingAdmissionStatus::Deferred:
                return "Deferred";
            case EStreamingAdmissionStatus::Rejected:
                return "Rejected";
        }
        return "Unknown";
    }

    const char* ToString(EStreamingAdmissionReason reason) noexcept
    {
        switch (reason)
        {
            case EStreamingAdmissionReason::None:
                return "None";
            case EStreamingAdmissionReason::ResidentBudget:
                return "ResidentBudget";
            case EStreamingAdmissionReason::FrameBudget:
                return "FrameBudget";
            case EStreamingAdmissionReason::LargerThanResidentBudget:
                return "LargerThanResidentBudget";
        }
        return "Unknown";
    }

    const char* ToString(EStreamingAdmission admission) noexcept
    {
        switch (admission)
        {
            case EStreamingAdmission::Admitted:
                return "Admitted";
            case EStreamingAdmission::NotRequestable:
                return "NotRequestable";
            case EStreamingAdmission::Deferred:
                return "Deferred";
            case EStreamingAdmission::Rejected:
                return "Rejected";
        }
        return "Unknown";
    }

    SceneStreamerConfig MakeSceneStreamerConfig(const StreamingSettings& settings)
    {
        SceneStreamerConfig config;
        config.LoadRadius = settings.DefaultLoadRadius;
        config.UnloadRadius = settings.DefaultUnloadRadius;
        config.MaxLoadedRegions = settings.MaxLoadedRegions;
        config.RegionDirectory = settings.RegionDirectory;
        config.MaxResidentBytes = StreamingBudgetMegabytesToBytes(settings.MaxResidentMegabytes);
        config.MaxAdmittedBytesPerFrame = StreamingBudgetMegabytesToBytes(settings.MaxAdmittedMegabytesPerFrame);
        return config;
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
        m_AbandonedLoads.Reset();
        m_Stats = {};
        m_AdmittedBytesThisFrame = 0;
        m_AdmittedRegionsThisFrame = 0;

        DiscoverRegions();
    }

    void SceneStreamer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        // Withdraw every load a worker has not started, then wait for every task,
        // live and abandoned, so none outlives the streamer. A withdrawn load's body
        // still runs, sees its Cancelled ticket and returns without parsing.
        for (const auto& pending : m_PendingLoads)
        {
            auto expected = FAssetLoadTicket::EState::Queued;
            pending.Ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Cancelled);
        }
        for (const auto& pending : m_PendingLoads)
        {
            if (pending.Task.IsValid())
            {
                pending.Task.Wait();
            }
        }
        for (const auto& abandoned : m_AbandonedLoads)
        {
            if (abandoned.Task.IsValid())
            {
                abandoned.Task.Wait();
            }
        }
        m_PendingLoads.Reset();
        m_AbandonedLoads.Reset();
        {
            // Regions whose load was withdrawn above never reached Ready; put them back.
            TUniqueLock<FMutex> lock(m_RegionMutex);
            for (auto& [id, region] : m_Regions)
            {
                if (region->m_State == StreamingRegion::State::Loading)
                {
                    region->m_State = StreamingRegion::State::Unloaded;
                }
            }
        }

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

            // The file's size is the region's byte estimate: known before any load,
            // and proportional to what instantiating it costs. An unreadable size is
            // unknown, not zero.
            std::error_code sizeError;
            const auto fileBytes = std::filesystem::file_size(entry.path(), sizeError);
            region->m_EstimatedSize = (!sizeError && fileBytes > 0) ? FAssetByteSize::Estimate(static_cast<u64>(fileBytes))
                                                                    : FAssetByteSize::Unknown();

            m_Regions[meta.RegionID] = region;

            OLO_CORE_TRACE("SceneStreamer: Discovered region '{0}' (ID: {1})", meta.Name, static_cast<u64>(meta.RegionID));
        }
    }

    void SceneStreamer::Update(const glm::vec3& activationPoint, u64 frameNumber)
    {
        OLO_PROFILE_FUNCTION();

        if (frameNumber != m_CurrentFrame)
        {
            m_AdmittedBytesThisFrame = 0;
            m_AdmittedRegionsThisFrame = 0;
        }
        m_CurrentFrame = frameNumber;

        // Sync config from Scene's authoritative StreamingSettings each frame
        if (m_Scene)
        {
            const SceneStreamerConfig synced = MakeSceneStreamerConfig(m_Scene->m_StreamingSettings);
            m_Config.LoadRadius = synced.LoadRadius;
            m_Config.UnloadRadius = synced.UnloadRadius;
            m_Config.MaxLoadedRegions = synced.MaxLoadedRegions;
            m_Config.MaxResidentBytes = synced.MaxResidentBytes;
            m_Config.MaxAdmittedBytesPerFrame = synced.MaxAdmittedBytesPerFrame;
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

            const bool insideLoad = distSq < vol.LoadRadius * vol.LoadRadius;
            const bool outsideUnload = distSq > vol.UnloadRadius * vol.UnloadRadius;
            if (insideLoad && region->m_State == StreamingRegion::State::Unloaded)
            {
                region->m_LastUsedFrame = frameNumber;
                RequestRegionLoad(regionId);
            }
            else if (outsideUnload && region->m_State == StreamingRegion::State::Ready)
            {
                // Release lock before UnloadRegion (it acquires lock internally)
                lock.Unlock();
                UnloadRegion(regionId);
                vol.IsLoaded = false;
            }
            else if (outsideUnload && region->m_State == StreamingRegion::State::Loading)
            {
                // Left the radius before its load finished: nothing wants it now.
                lock.Unlock();
                CancelRegionLoad(regionId);
            }
            else if (!insideLoad && region->m_State == StreamingRegion::State::Unloaded)
            {
                // A deferred or rejected request that nothing wants any more.
                region->m_AdmissionStatus = EStreamingAdmissionStatus::None;
                region->m_AdmissionReason = EStreamingAdmissionReason::None;
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

    EStreamingAdmission SceneStreamer::LoadRegion(RegionID regionId)
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_RegionMutex);
        auto it = m_Regions.find(regionId);
        if (it == m_Regions.end())
        {
            OLO_CORE_WARN("SceneStreamer: Region not found for manual load: {0}", static_cast<u64>(regionId));
            return EStreamingAdmission::NotRequestable;
        }

        if (it->second->m_State != StreamingRegion::State::Unloaded)
        {
            return EStreamingAdmission::NotRequestable;
        }

        return RequestRegionLoad(regionId);
    }

    void SceneStreamer::UnloadRegion(RegionID regionId)
    {
        OLO_PROFILE_FUNCTION();

        TDynamicUniqueLock<FMutex> lock(m_RegionMutex);
        auto it = m_Regions.find(regionId);
        if (it != m_Regions.end() && it->second->m_State == StreamingRegion::State::Loading)
        {
            // Unloading a region that is still loading means its load is unwanted.
            lock.Unlock();
            CancelRegionLoad(regionId);
            return;
        }
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

    FSceneStreamingStats SceneStreamer::GetStats() const
    {
        FSceneStreamingStats stats = m_Stats;
        stats.MaxLoadedRegions = m_Config.MaxLoadedRegions;
        stats.MaxResidentBytes = m_Config.MaxResidentBytes;
        stats.MaxAdmittedBytesPerFrame = m_Config.MaxAdmittedBytesPerFrame;
        stats.AdmittedBytesThisFrame = m_AdmittedBytesThisFrame;
        stats.PendingLoads = static_cast<u32>(m_PendingLoads.Num());
        stats.AbandonedLoadsRunning = 0;
        for (const auto& abandoned : m_AbandonedLoads)
        {
            if (!abandoned.Task.IsCompleted())
            {
                ++stats.AbandonedLoadsRunning;
            }
        }

        TUniqueLock<FMutex> lock(m_RegionMutex);
        for (const auto& [id, region] : m_Regions)
        {
            if (region->m_State == StreamingRegion::State::Ready)
            {
                ++stats.LoadedRegions;
                stats.ResidentBytes.Add(region->m_EstimatedSize);
            }
            else if (region->m_State == StreamingRegion::State::Loading)
            {
                stats.PendingBytes.Add(region->m_EstimatedSize);
            }

            if (region->m_AdmissionStatus == EStreamingAdmissionStatus::Deferred)
            {
                ++stats.DeferredRegions;
            }
            else if (region->m_AdmissionStatus == EStreamingAdmissionStatus::Rejected)
            {
                ++stats.RejectedRegions;
            }
        }
        return stats;
    }

    void SceneStreamer::SetTestHooks(FRegionLoadTestHooks hooks)
    {
        m_TestHooks = std::move(hooks);
    }

    u64 SceneStreamer::CommittedBytesLocked() const
    {
        // Caller holds m_RegionMutex. Loading regions count: their bytes are on the
        // way, and admitting against Ready alone would let a burst of requests
        // overshoot the budget before any of them lands.
        u64 committed = 0;
        for (const auto& [id, region] : m_Regions)
        {
            if (region->m_State == StreamingRegion::State::Ready || region->m_State == StreamingRegion::State::Loading)
            {
                committed += region->m_EstimatedSize.GetBytes().value_or(0);
            }
        }
        return committed;
    }

    EStreamingAdmission SceneStreamer::AdmitLocked(StreamingRegion& region)
    {
        // Caller holds m_RegionMutex.
        auto refuse = [this, &region](EStreamingAdmissionStatus status, EStreamingAdmissionReason reason)
        {
            region.m_AdmissionStatus = status;
            region.m_AdmissionReason = reason;
            if (status == EStreamingAdmissionStatus::Rejected)
            {
                ++m_Stats.RejectedRequests;
                if (!region.m_WarnedAdmission)
                {
                    region.m_WarnedAdmission = true;
                    OLO_CORE_WARN("SceneStreamer: Region '{0}' rejected: its estimate of {1} bytes exceeds the resident byte budget of {2} bytes",
                                  region.m_Name.ToView(), region.m_EstimatedSize.GetBytes().value_or(0), m_Config.MaxResidentBytes);
                }
                return EStreamingAdmission::Rejected;
            }
            ++m_Stats.DeferredRequests;
            return EStreamingAdmission::Deferred;
        };

        const std::optional<u64> bytes = region.m_EstimatedSize.GetBytes();

        if (m_Config.MaxResidentBytes != 0)
        {
            if (!bytes)
            {
                // No figure to check. Admitting keeps the region loadable; the counter
                // and the one-time warning keep the gap in the budget visible.
                ++m_Stats.AdmittedUnknownSize;
                if (!region.m_WarnedAdmission)
                {
                    region.m_WarnedAdmission = true;
                    OLO_CORE_WARN("SceneStreamer: Region '{0}' has no byte estimate; admitted without a resident budget check",
                                  region.m_Name.ToView());
                }
            }
            else if (*bytes > m_Config.MaxResidentBytes)
            {
                return refuse(EStreamingAdmissionStatus::Rejected, EStreamingAdmissionReason::LargerThanResidentBudget);
            }
            else if (CommittedBytesLocked() + *bytes > m_Config.MaxResidentBytes)
            {
                return refuse(EStreamingAdmissionStatus::Deferred, EStreamingAdmissionReason::ResidentBudget);
            }
        }

        // The first region of a frame is always admitted, so a region larger than the
        // per-frame budget still loads.
        if (m_Config.MaxAdmittedBytesPerFrame != 0 && bytes && m_AdmittedRegionsThisFrame > 0 &&
            m_AdmittedBytesThisFrame + *bytes > m_Config.MaxAdmittedBytesPerFrame)
        {
            return refuse(EStreamingAdmissionStatus::Deferred, EStreamingAdmissionReason::FrameBudget);
        }

        m_AdmittedBytesThisFrame += bytes.value_or(0);
        ++m_AdmittedRegionsThisFrame;
        region.m_AdmissionStatus = EStreamingAdmissionStatus::None;
        region.m_AdmissionReason = EStreamingAdmissionReason::None;
        return EStreamingAdmission::Admitted;
    }

    EStreamingAdmission SceneStreamer::RequestRegionLoad(RegionID id)
    {
        // Caller must hold m_RegionMutex
        auto it = m_Regions.find(id);
        if (it == m_Regions.end())
        {
            return EStreamingAdmission::NotRequestable;
        }

        auto& region = it->second;
        if (const EStreamingAdmission admission = AdmitLocked(*region); admission != EStreamingAdmission::Admitted)
        {
            return admission;
        }

        region->m_State = StreamingRegion::State::Loading;

        // The worker gets copies of everything it needs and never touches the region
        // or the streamer: its only output is its task result.
        auto ticket = Ref<FAssetLoadTicket>::Create();
        auto body = [ticket, path = region->m_SourcePath, id, onStarted = m_TestHooks.OnLoadStarted,
                     onFinished = m_TestHooks.OnLoadFinished]() -> FRegionLoadResult
        {
            auto expected = FAssetLoadTicket::EState::Queued;
            if (!ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Running))
            {
                return {};
            }

            OLO_PROFILE_SCOPE("StreamingRegion::Parse");
            if (onStarted)
            {
                onStarted(id);
            }
            FRegionLoadResult result;
            result.Data = StreamingRegionSerializer::ParseRegionFile(path);
            result.Success = result.Data && result.Data["Region"];
            if (onFinished)
            {
                onFinished(id);
            }
            return result;
        };

        Tasks::TTask<FRegionLoadResult> task;
        if (m_TestHooks.StartGate.has_value())
        {
            task = Tasks::Launch("SceneRegionLoad", std::move(body), Tasks::Prerequisites(*m_TestHooks.StartGate),
                                 Tasks::ETaskPriority::BackgroundNormal);
        }
        else
        {
            task = Tasks::Launch("SceneRegionLoad", std::move(body), Tasks::ETaskPriority::BackgroundNormal);
        }

        m_PendingLoads.Add(PendingLoad{ id, std::move(task), region, std::move(ticket) });

        OLO_CORE_TRACE("SceneStreamer: Requested load for region '{0}'", region->m_Name.ToView());
        return EStreamingAdmission::Admitted;
    }

    EAssetLoadCancelResult SceneStreamer::CancelRegionLoad(RegionID regionId)
    {
        OLO_PROFILE_FUNCTION();

        for (i32 index = 0; index < m_PendingLoads.Num(); ++index)
        {
            if (m_PendingLoads[index].RegionId != regionId)
            {
                continue;
            }

            PendingLoad& load = m_PendingLoads[index];
            EAssetLoadCancelResult result;
            auto expected = FAssetLoadTicket::EState::Queued;
            if (load.Ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Cancelled))
            {
                // Never parses. Its (now trivial) body must still run before the task
                // handle is dropped, so it waits with the abandoned loads.
                ++m_Stats.CancelledBeforeStart;
                result = EAssetLoadCancelResult::CancelledBeforeStart;
                m_AbandonedLoads.Add(std::move(load));
            }
            else if (load.Task.IsCompleted())
            {
                ++m_Stats.DiscardedCompleted;
                result = EAssetLoadCancelResult::DiscardedCompleted;
            }
            else
            {
                // Mid-parse. Out of m_PendingLoads, ProcessCompletedLoads never sees it.
                ++m_Stats.AbandonedInFlight;
                result = EAssetLoadCancelResult::AbandonedInFlight;
                m_AbandonedLoads.Add(std::move(load));
            }
            m_PendingLoads.RemoveAt(index);

            TUniqueLock<FMutex> lock(m_RegionMutex);
            if (auto it = m_Regions.find(regionId); it != m_Regions.end() && it->second->m_State == StreamingRegion::State::Loading)
            {
                it->second->m_State = StreamingRegion::State::Unloaded;
            }
            OLO_CORE_TRACE("SceneStreamer: Cancelled load for region {0}: {1}", static_cast<u64>(regionId), ToString(result));
            return result;
        }

        return EAssetLoadCancelResult::NotPending;
    }

    void SceneStreamer::ReapAbandonedLoads()
    {
        for (i32 index = m_AbandonedLoads.Num() - 1; index >= 0; --index)
        {
            if (!m_AbandonedLoads[index].Task.IsCompleted())
            {
                continue;
            }
            if (m_AbandonedLoads[index].Ticket->State.load() == FAssetLoadTicket::EState::Running)
            {
                ++m_Stats.AbandonedResultsDropped;
            }
            m_AbandonedLoads.RemoveAtSwap(index);
        }
    }

    void SceneStreamer::ProcessCompletedLoads()
    {
        OLO_PROFILE_FUNCTION();

        ReapAbandonedLoads();

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

            // Only a load still in m_PendingLoads gets here: a cancelled one was moved
            // out, so its result is never instantiated.
            const FRegionLoadResult& loadResult = m_PendingLoads[index].Task.GetResult();
            auto& region = m_PendingLoads[index].Region;

            StreamingRegion::State regionState;
            {
                TUniqueLock<FMutex> lock(m_RegionMutex);
                regionState = region->m_State;
            }

            if (loadResult.Success && regionState == StreamingRegion::State::Loading && m_Scene)
            {
                // Main-thread entity instantiation
                Ref<Scene> sceneRef{ m_Scene };
                SceneSerializer serializer{ sceneRef };
                if (auto entitiesNode = loadResult.Data["Entities"])
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
            else
            {
                if (!loadResult.Success)
                {
                    OLO_CORE_ERROR("SceneStreamer: Failed to load region '{0}'", region->m_Name.ToView());
                }
                TUniqueLock<FMutex> lock(m_RegionMutex);
                if (region->m_State == StreamingRegion::State::Loading)
                {
                    region->m_State = StreamingRegion::State::Unloaded;
                }
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

        u64 committedBytes = CommittedBytesLocked();
        auto overCount = [&]
        { return readyCount > m_Config.MaxLoadedRegions; };
        auto overBytes = [&]
        { return m_Config.MaxResidentBytes != 0 && committedBytes > m_Config.MaxResidentBytes; };

        if (!overCount() && !overBytes())
        {
            return;
        }

        // Collect ready regions sorted by LRU frame
        struct ReadyRegion
        {
            RegionID Id;
            u64 LastUsedFrame;
            u64 Bytes;
        };
        TArray<ReadyRegion> sortedRegions;
        sortedRegions.Reserve(static_cast<i32>(readyCount));
        for (auto& [id, region] : m_Regions)
        {
            if (region->m_State == StreamingRegion::State::Ready)
            {
                sortedRegions.Add(ReadyRegion{ id, region->m_LastUsedFrame, region->m_EstimatedSize.GetBytes().value_or(0) });
            }
        }

        std::ranges::sort(sortedRegions,
                          [](const auto& a, const auto& b)
                          { return a.LastUsedFrame < b.LastUsedFrame; });

        // Evict oldest until both the count and the byte budget hold. Loading regions
        // count against the bytes but cannot be evicted, so the loop may run out of
        // Ready regions first; admission keeps that from happening in steady state.
        for (const ReadyRegion& victim : sortedRegions)
        {
            const bool count = overCount();
            if (!count && !overBytes())
            {
                break;
            }
            ++(count ? m_Stats.EvictedForCount : m_Stats.EvictedForBytes);

            // Release lock before UnloadRegion (it acquires lock internally)
            lock.Unlock();
            UnloadRegion(victim.Id);
            lock.Lock();

            --readyCount;
            committedBytes -= std::min(committedBytes, victim.Bytes);
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
