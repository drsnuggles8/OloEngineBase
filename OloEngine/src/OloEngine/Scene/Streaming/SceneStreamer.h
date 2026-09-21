#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "StreamingRegion.h"

#include <glm/glm.hpp>
#include <unordered_map>

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
    };

    template<>
    struct TIsTriviallyRelocatable<SceneStreamerConfig>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::LoadRadius)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::UnloadRadius)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::MaxLoadedRegions)> &&
                                      TIsTriviallyRelocatable_V<decltype(SceneStreamerConfig::RegionDirectory)>;
    };

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
        void LoadRegion(RegionID regionId);
        void UnloadRegion(RegionID regionId);

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

      private:
        void DiscoverRegions();
        void RequestRegionLoad(RegionID id);
        void ProcessCompletedLoads();
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
            Tasks::TTask<bool> Task;
            Ref<StreamingRegion> Region;
        };
        friend struct TIsTriviallyRelocatable<PendingLoad>;
        TArray<PendingLoad> m_PendingLoads;

        mutable FMutex m_RegionMutex; // Protects m_Regions
        u64 m_CurrentFrame = 0;
        UUID m_ActivationEntityId{}; // 0 = use primary camera
    };
    template<>
    struct TIsTriviallyRelocatable<SceneStreamer::PendingLoad>
    {
        using Load = SceneStreamer::PendingLoad;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Load::RegionId)> &&
                                      TIsTriviallyRelocatable_V<decltype(Load::Task)> &&
                                      TIsTriviallyRelocatable_V<decltype(Load::Region)>;
    };
} // namespace OloEngine
