#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/RayTracing/VegetationPolicy.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <map>
#include <array>
#include "OloEngine/Containers/Array.h"

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
        TArray<u32> Indices;
        GPUSceneMaterialInput Material;
    };

} // namespace OloEngine::RayTracing

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<RayTracing::VegetationSurfacePart>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfacePart::Slot)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfacePart::Indices)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfacePart::Material)>::Value;
    };
} // namespace OloEngine

namespace OloEngine::RayTracing
{
    struct VegetationSurfaceInput
    {
        u64 Owner = 0u;
        u64 FirstPlantId = 0u;
        Ref<VertexBuffer> Rest;
        u32 VertexCount = 0u;
        TArray<VegetationSurfacePart> Parts;
        TArray<FoliageInstanceData> Rows;
        glm::mat4 WorldTransform{ 1.0f };
        ShaderBindingLayout::FoliageUBO Wind{};
        f32 DistanceToView = 0.0f;
        f32 DetailedDistance = 0.0f;
        f32 VelocityBound = 0.0f;
        bool HistoryContinuous = false;
    };

} // namespace OloEngine::RayTracing

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<RayTracing::VegetationSurfaceInput>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::Owner)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::FirstPlantId)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::Rest)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::VertexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::Parts)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::Rows)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::WorldTransform)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::Wind)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::DistanceToView)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::DetailedDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::VelocityBound)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::HistoryContinuous)>::Value;
    };
} // namespace OloEngine

namespace OloEngine::RayTracing
{
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
            return !m_Jobs.IsEmpty();
        }
        [[nodiscard]] bool HasQueueCapacity() const
        {
            return static_cast<u32>(m_Inputs.Num()) < VegetationPolicy::ResidentGroups && m_StagedBytes < VegetationPolicy::GeometryBytes;
        }
        [[nodiscard]] const VegetationSurfaceStats& GetStats() const
        {
            return m_Stats;
        }

      private:
        template<typename>
        friend struct OloEngine::TIsTriviallyRelocatable;
        using Key = std::array<u64, 3>;
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

        struct Batch
        {
            u64 Owner = 0u;
            u64 RestHandle = 0u;
            const Job* Parameters = nullptr;
            TArray<const Job*> Jobs;
        };

        [[nodiscard]] bool EnsureShader();
        void RollbackJobs();
        bool m_Enabled = false;
        bool m_PreviousComplete = true;
        u64 m_Frame = 0u;
        u64 m_StagedBytes = 0u;
        VegetationSurfaceStats m_Stats;
        std::map<Key, Entry> m_Entries;
        TArray<VegetationSurfaceInput> m_Inputs;
        TArray<Job> m_Jobs;
        Ref<ComputeShader> m_Shader;
        Ref<UniformBuffer> m_Params;
        Ref<UniformBuffer> m_Wind;
    };
} // namespace OloEngine::RayTracing

namespace OloEngine
{
    template<>
    struct TIsTriviallyRelocatable<RayTracing::VegetationSurfaceCache::Batch>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceCache::Batch::Owner)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceCache::Batch::RestHandle)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceCache::Batch::Parameters)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceCache::Batch::Jobs)>::Value;
    };
} // namespace OloEngine
