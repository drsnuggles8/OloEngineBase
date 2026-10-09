#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/RayTracing/VegetationPolicy.h"
#include "OloEngine/Renderer/VertexBuffer.h"
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
        /// The layer's CastShadows (#1533). False stages the group's instances
        /// out of the shadow-caster mask lane, so shadow rays pass through what
        /// the raster tier does not cast while reflections still see it.
        bool CastShadows = true;
        /// What the group's structures will occupy, one BLAS per part, as the
        /// device sizes them (RayTracingScene::EstimateDeformedBlasBytes). The
        /// cache refuses a group past VegetationPolicy::AccelerationStructureBytes,
        /// the cap the backend enforces by dropping builds (#1533). 0: unknown.
        u64 AccelerationBytes = 0u;
        /// What the rows stand for (#1533): a key the producer derives from the
        /// plants and from everything it writes into their rows. 0: none, and
        /// the cache hashes the rows themselves. With a key the cache takes it
        /// as the rows' state, and for a group it already holds under that key
        /// (HoldsContent) the producer may send no rows at all and name the
        /// plant count instead -- 216k rows a frame for the showcase lawn.
        u64 ContentKey = 0u;
        u32 HeldPlantCount = 0u;
        /// The plants' identity, order-free (#1354): the sum of
        /// VegetationPlantTerm over the group's plant ids. The cache folds it
        /// with the tier into a signature of the traced representation, so
        /// plants regrouped at the same tier (a camera move re-slices them)
        /// read as no change, and a tier switch reads as one. 0: derived from
        /// FirstPlantId and the plant count.
        u64 PlantSetSum = 0u;
    };

    /// One plant's share of VegetationSurfaceInput::PlantSetSum.
    [[nodiscard]] constexpr u64 VegetationPlantTerm(u64 plantId) noexcept
    {
        u64 x = plantId + 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31u);
    }

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
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::HistoryContinuous)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::CastShadows)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::AccelerationBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::ContentKey)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::HeldPlantCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RayTracing::VegetationSurfaceInput::PlantSetSum)>::Value;
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
        /// Reflection-only groups left out of the scene because they did not
        /// fit the budget nearest first (VegetationPolicy::ChooseReflectionTiers),
        /// and their plants. Counted, not refused (#1533).
        u32 BeyondReflectionBudget = 0u;
        u32 PlantsBeyondReflectionBudget = 0u;
        /// The farthest admitted reflection-only group's nearest plant, metres:
        /// how far out reflections still hold vegetation. 0 with none admitted.
        f32 ReflectionReach = 0.0f;
        /// The same for the groups traced as the authored mesh rather than
        /// cards: how far out reflections hold the full plant.
        f32 ReflectionDetailReach = 0.0f;
        /// Acceleration-structure bytes the queued groups will occupy, by the
        /// device's own sizing; at most VegetationPolicy::AccelerationStructureBytes.
        u64 StagedAccelerationBytes = 0u;
        /// Reflection-only groups whose refresh did not fit this frame's budget
        /// and kept their last snapshot instead of being refused.
        u32 StaleReflectionSnapshots = 0u;
        /// Builds the backend did not record last frame, charged to this
        /// frame's budget before any refresh (VegetationBuildDebt).
        u32 CarriedBuilds = 0u;

        // --- Requested against realized quality (#1354) -----------------
        /// Casting groups the producer planned, how many it traces as their
        /// complete lower-cost tier (cards) instead of the requested one, and
        /// how many fit at no tier (refused: the casters are incomplete and
        /// shadow rays fall back to the raster).
        u32 CastingGroupsPlanned = 0u;
        u32 CastingFallbackGroups = 0u;
        u32 CastingGroupsLeftOut = 0u;
        /// The nearest casting group traced below its requested tier, metres;
        /// 0 with none. Near (hero) quality holds while this stays far out.
        f32 NearestCastingFallback = 0.0f;
        /// Casting groups whose refresh did not fit and that kept a snapshot
        /// inside the proxy deadline (VegetationPolicy::CanHoldSnapshot).
        u32 CadenceHolds = 0u;
        /// Reflection-only groups with no usable snapshot whose build did not
        /// fit: left out of the scene this frame and counted, not refused.
        u32 DeferredReflectionGroups = 0u;
        u32 DeferredReflectionPlants = 0u;
        /// The oldest snapshot published this frame, seconds, and the world
        /// displacement its velocity bound allows, metres: the far groups'
        /// fairness, bounded by CanHoldSnapshot for every published group.
        f32 OldestSnapshotAge = 0.0f;
        f32 OldestSnapshotError = 0.0f;
        /// What restoring the requested quality would cost: the difference
        /// for every group below its requested tier, the requested cost of
        /// every group left out, and the refresh of every held or deferred one.
        VegetationPolicy::GroupCost Recovery;
        /// Groups each pressure source pushed below their request this frame.
        std::array<u32, static_cast<sizet>(VegetationPressure::Count)> Pressure{};
        [[nodiscard]] VegetationPressure DominantPressure() const
        {
            sizet dominant = 0u;
            for (sizet i = 1u; i < Pressure.size(); ++i)
                if (Pressure[i] > Pressure[dominant])
                    dominant = i;
            return Pressure[dominant] == 0u ? VegetationPressure::None : static_cast<VegetationPressure>(dominant);
        }
        /// Groups first seen and groups retired this frame: the churn a camera
        /// move causes by re-slicing plants. Before #1354 any of it reset the
        /// RT and TAA histories; now only a representation change does.
        u32 GroupsCreated = 0u;
        u32 GroupsRetired = 0u;
        /// The traced representation changed: plants entered or left the
        /// scene, changed tier or time resolution, or lost wind continuity
        /// (#1354). Plants regrouped at the same tier are not a change.
        bool HistoryReset = false;
        bool ProducerFailed = false;
        /// Every queued group is resident and current.
        bool Complete = true;
        /// Every group of a layer that CASTS is: what shadow rays need (#1533).
        bool CastersComplete = true;
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
        /// A group the producer could not even describe, refused as Queue
        /// refuses an invalid input; `castsShadows` says whose completeness it
        /// costs (#1533).
        void Refuse(bool castsShadows, VegetationPressure pressure = VegetationPressure::InvalidContent);
        /// What the producer's casting plan chose (#1354), counted under the
        /// pressure that forced its first fallback.
        void CountCastingPlan(const VegetationPolicy::CastingPlan& plan, f32 nearestFallback);
        /// Reflection-only groups left out by ChooseReflectionTiers, and the
        /// reach of the ones admitted, at any tier and as the mesh.
        void CountBeyondReflectionBudget(u32 groups, u32 plants, f32 reach, f32 detailReach);
        [[nodiscard]] u64 GetStagedBytes() const
        {
            return m_StagedBytes;
        }
        /// The groups queued so far this frame: each takes one of
        /// VegetationPolicy::ResidentGroups.
        [[nodiscard]] u32 GetQueuedGroups() const
        {
            return static_cast<u32>(m_Inputs.Num());
        }
        /// Whether this group's rows are resident under `contentKey`, so its
        /// input may carry HeldPlantCount instead of rows.
        [[nodiscard]] bool HoldsContent(u64 owner, u64 firstPlantId, const Ref<VertexBuffer>& rest, u64 contentKey) const;
        [[nodiscard]] u64 GetStagedAccelerationBytes() const
        {
            return m_Stats.StagedAccelerationBytes;
        }
        /// What the ray-traced scene could not build this frame; the next
        /// FinishExtraction charges it to the frame budget (#1533).
        void ChargeBuildDebt(const VegetationBuildDebt& debt)
        {
            m_BuildDebt = debt;
        }
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
            u64 ContentKey = 0u;
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
        void CountPressure(VegetationPressure pressure, u32 groups = 1u);
        bool m_Enabled = false;
        // The last frame's representation signature (HistoryReset).
        u64 m_PreviousSignature = 0u;
        bool m_PreviousComplete = true;
        // Whether last frame left reflection-only groups out, so the log says it
        // when that starts and when it stops, not every frame (#1533).
        bool m_PreviousLeftOut = false;
        VegetationBuildDebt m_BuildDebt;
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
