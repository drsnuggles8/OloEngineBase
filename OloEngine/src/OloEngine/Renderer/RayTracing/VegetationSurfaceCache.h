#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/RayTracing/VegetationPolicy.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <map>
#include <tuple>
#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class GPUScene;
    class IndexBuffer;
    class UniformBuffer;
    class VertexBuffer;
} // namespace OloEngine

namespace OloEngine::RayTracing
{
    // Copied during canonical extraction. Buffer references keep the source
    // alive through graph execution; rows are a projection of stable plant IDs.
    struct VegetationSurfacePart
    {
        u32 Slot = 0u;
        std::vector<u32> Indices;
        GPUSceneMaterialInput Material;
    };

    struct VegetationSurfaceInput
    {
        u64 Owner = 0u;
        u64 FirstPlantId = 0u;
        Ref<VertexBuffer> Rest;
        u32 VertexCount = 0u;
        std::vector<VegetationSurfacePart> Parts;
        std::vector<FoliageInstanceData> Rows;
        glm::mat4 WorldTransform{ 1.0f };
        ShaderBindingLayout::FoliageUBO Wind{};
        f32 DistanceToView = 0.0f;
        f32 DetailedDistance = 0.0f;
        f32 VelocityBound = 0.0f;
        bool HistoryContinuous = false;
    };

    struct VegetationSurfaceStats
    {
        u32 GroupsRequested = 0u;
        u32 DetailedGroups = 0u;
        u32 ProxyGroups = 0u;
        u32 SnapshotsReused = 0u;
        u32 Dispatched = 0u;
        u32 DispatchBatches = 0u;
        u32 VerticesDeformed = 0u;
        u32 Refused = 0u;
        u32 PlantsRepresented = 0u;
        u64 ResidentBytes = 0u;
        bool HistoryReset = false;
        bool ProducerFailed = false;
        bool Complete = true;
    };

    // Render-thread producer. FinishExtraction reserves bounded work in oldest
    // snapshot order, stages canonical records and retires absent groups.
    // Dispatch runs in RayTracingScenePass, before any BLAS can read its output.
    class VegetationSurfaceCache
    {
      public:
        VegetationSurfaceCache();
        ~VegetationSurfaceCache();
        VegetationSurfaceCache(const VegetationSurfaceCache&) = delete;
        VegetationSurfaceCache& operator=(const VegetationSurfaceCache&) = delete;

        void SetEnabled(bool enabled);
        void Shutdown();
        void BeginFrame();
        void Queue(VegetationSurfaceInput input);
        void FinishExtraction(GPUScene& scene);
        [[nodiscard]] u32 Dispatch();
        [[nodiscard]] bool IsEnabled() const
        {
            return m_Enabled;
        }
        [[nodiscard]] bool HasWork() const
        {
            return !m_Jobs.empty();
        }
        [[nodiscard]] bool HasQueueCapacity() const
        {
            return m_Inputs.size() < VegetationPolicy::ResidentGroups && m_StagedBytes < VegetationPolicy::GeometryBytes;
        }
        [[nodiscard]] const VegetationSurfaceStats& GetStats() const
        {
            return m_Stats;
        }

      private:
        using Key = std::tuple<u64, u64, u64>;
        struct Entry
        {
            Ref<VertexBuffer> Rest;
            Ref<VertexBuffer> Rows;
            Ref<VertexBuffer> Output;
            Ref<IndexBuffer> Indices;
            u64 StateHash = 0u;
            u64 ParameterHash = 0u;
            u64 Bytes = 0u;
            u64 LastSeen = 0u;
            u32 Revision = 0u;
            f32 SnapshotTime = 0.0f;
            i64 SnapshotBucket = 0;
            bool Proxy = false;
            bool Valid = false;
        };
        struct Job
        {
            Key Identity;
            ShaderBindingLayout::FoliageUBO Wind;
            glm::mat4 Model{ 1.0f };
            u32 VertexCount = 0u;
            u32 PlantCount = 0u;
        };

        [[nodiscard]] bool EnsureShader();
        void RollbackJobs();
        bool m_Enabled = false;
        bool m_PreviousComplete = true;
        u64 m_Frame = 0u;
        u64 m_StagedBytes = 0u;
        VegetationSurfaceStats m_Stats;
        std::map<Key, Entry> m_Entries;
        std::vector<VegetationSurfaceInput> m_Inputs;
        std::vector<Job> m_Jobs;
        Ref<ComputeShader> m_Shader;
        Ref<UniformBuffer> m_Params;
        Ref<UniformBuffer> m_Wind;
    };
} // namespace OloEngine::RayTracing
