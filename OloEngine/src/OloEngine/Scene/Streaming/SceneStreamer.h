#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/AssetByteSize.h"
#include "OloEngine/Asset/AssetSystem/AssetLoadTicket.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "StreamingRegion.h"
#include "StreamingSettings.h"

#include <glm/glm.hpp>
#include <functional>
#include <optional>
#include <unordered_map>

#pragma warning(push)
#pragma warning(disable : 4275)
#include <yaml-cpp/yaml.h>
#pragma warning(pop)

namespace OloEngine
{
    class Scene;

    using RegionID = UUID;

    struct SceneStreamerConfig
    {
        f32 LoadRadius = 200.0f;   // Distance to start loading
        f32 UnloadRadius = 250.0f; // Distance to trigger unload (hysteresis)
        u32 MaxLoadedRegions = 16; // LRU budget
        FString RegionDirectory;   // Path to .oloregion files
        // Byte budgets (issue #1365); 0 = no byte budget. See StreamingSettings.
        u64 MaxResidentBytes = 0;
        u64 MaxAdmittedBytesPerFrame = 0;
    };

    template<>
    struct TIsTriviallyRelocatable<SceneStreamerConfig>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::LoadRadius)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::UnloadRadius)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::MaxLoadedRegions)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::RegionDirectory)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::MaxResidentBytes)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::MaxAdmittedBytesPerFrame)>;
    };

    // The one conversion from the scene's authored settings to the streamer's config.
    [[nodiscard]] SceneStreamerConfig MakeSceneStreamerConfig(const StreamingSettings& settings);

    /**
     * @brief What happened to one region load request (issue #1365).
     */
    enum class EStreamingAdmission : u8
    {
        Admitted,       // a load task was launched
        NotRequestable, // unknown region, or not Unloaded (already loading or loaded)
        Deferred,       // would cross a byte budget now; see the region's admission reason
        Rejected        // can never fit the resident byte budget
    };

    [[nodiscard]] const char* ToString(EStreamingAdmission admission) noexcept;

    /**
     * @brief The streaming report: counts, bytes, and what admission and
     *        cancellation did, so a region that is not loaded is answerable.
     *
     * Byte totals sum each region's m_EstimatedSize; their UnknownCount says how
     * many regions contributed no figure. Cumulative counters count requests, so
     * a proximity region deferred for ten frames adds ten to DeferredRequests;
     * DeferredRegions and RejectedRegions are the regions in that state now.
     */
    struct FSceneStreamingStats
    {
        u32 LoadedRegions = 0;
        u32 MaxLoadedRegions = 0;
        u32 PendingLoads = 0;
        u32 AbandonedLoadsRunning = 0;
        FAssetByteTotal ResidentBytes; // Ready regions
        FAssetByteTotal PendingBytes;  // Loading regions
        u64 MaxResidentBytes = 0;          // 0 = no byte budget
        u64 MaxAdmittedBytesPerFrame = 0;  // 0 = no per-frame budget
        u64 AdmittedBytesThisFrame = 0;

        u32 DeferredRegions = 0;
        u32 RejectedRegions = 0;
        u64 DeferredRequests = 0;
        u64 RejectedRequests = 0;
        u64 AdmittedUnknownSize = 0; // admitted under a byte budget with no estimate to check

        u64 CancelledBeforeStart = 0;
        u64 AbandonedInFlight = 0;
        u64 DiscardedCompleted = 0;
        u64 AbandonedResultsDropped = 0;

        u64 EvictedForCount = 0;
        u64 EvictedForBytes = 0;
    };

    /**
     * @brief Test seam on the region load worker; see FRuntimeAssetLoadTestHooks,
     *        which this mirrors. The hooks are captured by value when a load is
     *        launched, so the worker never reads streamer state.
     */
    struct FRegionLoadTestHooks
    {
        std::optional<Tasks::FTaskEvent> StartGate;
        std::function<void(RegionID)> OnLoadStarted;
        std::function<void(RegionID)> OnLoadFinished;
    };

    /**
     * @brief Distance-driven region streaming.
     *
     * ## Region load cancellation (issue #1365)
     *
     * The load worker parses the .oloregion file and returns the YAML as its task
     * result; it never writes to the region or the streamer. Only
     * ProcessCompletedLoads() instantiates a result, and only for a load still in
     * m_PendingLoads. CancelRegionLoad() takes the load out of m_PendingLoads and
     * puts the region back to Unloaded, so a later-arriving result is never
     * instantiated: a load not yet started never parses (ticket Queued ->
     * Cancelled), a running one finishes into m_AbandonedLoads and is dropped when
     * reaped, a finished-but-unprocessed one is dropped at once. UnloadRegion() on
     * a Loading region, and a proximity region leaving its unload radius while
     * Loading, cancel the load. Shutdown() cancels the unstarted loads and waits
     * for every task, live and abandoned.
     *
     * All of m_PendingLoads, m_AbandonedLoads and m_Stats are main-thread state.
     */
    class SceneStreamer
    {
      public:
        SceneStreamer() = default;
        ~SceneStreamer();

        void Initialize(Scene* scene, const SceneStreamerConfig& config);
        void Shutdown();

        // Called each frame (runtime + editor)
        void Update(const glm::vec3& activationPoint, u64 frameNumber);

        // Explicit load/unload for Manual activation mode + scripting
        EStreamingAdmission LoadRegion(RegionID regionId);
        void UnloadRegion(RegionID regionId);

        // Withdraw or abandon a region's in-flight load; see the class comment.
        EAssetLoadCancelResult CancelRegionLoad(RegionID regionId);

        // Override the default activation entity (0 = use primary camera)
        void SetActivationEntity(UUID entityId);

        // Accessors for editor panel
        [[nodiscard]] const SceneStreamerConfig& GetConfig() const
        {
            return m_Config;
        }
        [[nodiscard]] SceneStreamerConfig& GetConfig()
        {
            return m_Config;
        }
        [[nodiscard]] u32 GetLoadedRegionCount() const;
        [[nodiscard]] u32 GetPendingLoadCount() const;
        [[nodiscard]] std::unordered_map<RegionID, Ref<StreamingRegion>> GetRegions() const
        {
            TUniqueLock<FMutex> lock(m_RegionMutex);
            return m_Regions;
        }
        [[nodiscard]] FSceneStreamingStats GetStats() const;

        void SetTestHooks(FRegionLoadTestHooks hooks);

      private:
        struct FRegionLoadResult
        {
            bool Success = false;
            YAML::Node Data;
        };

        void DiscoverRegions();
        // Caller holds m_RegionMutex.
        EStreamingAdmission RequestRegionLoad(RegionID id);
        [[nodiscard]] EStreamingAdmission AdmitLocked(StreamingRegion& region);
        [[nodiscard]] u64 CommittedBytesLocked() const;
        void ProcessCompletedLoads();
        void ReapAbandonedLoads();
        void EvictOverBudget();
        void InitializeStreamedEntities(const TArray<UUID>& entityUUIDs) const;

        Scene* m_Scene = nullptr;
        SceneStreamerConfig m_Config;

        // Region registry (discovered from disk, keyed by RegionID)
        std::unordered_map<RegionID, Ref<StreamingRegion>> m_Regions;

        // In-flight async loads
        struct PendingLoad
        {
            RegionID RegionId;
            Tasks::TTask<FRegionLoadResult> Task;
            Ref<StreamingRegion> Region;
            Ref<FAssetLoadTicket> Ticket;
        };
        friend struct TIsTriviallyRelocatable<PendingLoad>;
        TArray<PendingLoad> m_PendingLoads;
        TArray<PendingLoad> m_AbandonedLoads;

        FSceneStreamingStats m_Stats; // cumulative counters only; GetStats() fills the rest
        u64 m_AdmittedBytesThisFrame = 0;
        u32 m_AdmittedRegionsThisFrame = 0;
        FRegionLoadTestHooks m_TestHooks;

        mutable FMutex m_RegionMutex; // Protects m_Regions and each region's state fields
        u64 m_CurrentFrame = 0;
        UUID m_ActivationEntityId{}; // 0 = use primary camera
    };
    template<>
    struct TIsTriviallyRelocatable<SceneStreamer::PendingLoad>
    {
        using Load = SceneStreamer::PendingLoad;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Load::RegionId)> &&
                                      TIsTriviallyRelocatable_V<decltype(Load::Task)> &&
                                      TIsTriviallyRelocatable_V<decltype(Load::Region)> &&
                                      TIsTriviallyRelocatable_V<decltype(Load::Ticket)>;
    };
} // namespace OloEngine
