#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Task/Task.h"
#include "StreamingRegion.h"

#include <glm/glm.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Scene;

    using RegionID = UUID;

    struct SceneStreamerConfig
    {
        f32 LoadRadius = 200.0f;     // Distance to start loading
        f32 UnloadRadius = 250.0f;   // Distance to trigger unload (hysteresis)
        u32 MaxLoadedRegions = 16;   // LRU budget
        std::string RegionDirectory; // Path to .oloregion files
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
            std::lock_guard lock(m_RegionMutex);
            return m_Regions;
        }

      private:
        void DiscoverRegions();
        void RequestRegionLoad(RegionID id);
        void ProcessCompletedLoads();
        void EvictOverBudget();
        void InitializeStreamedEntities(const std::vector<UUID>& entityUUIDs);

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
        std::vector<PendingLoad> m_PendingLoads;

        mutable std::mutex m_RegionMutex; // Protects m_Regions
        u64 m_CurrentFrame = 0;
        UUID m_ActivationEntityId{}; // 0 = use primary camera
    };
} // namespace OloEngine
