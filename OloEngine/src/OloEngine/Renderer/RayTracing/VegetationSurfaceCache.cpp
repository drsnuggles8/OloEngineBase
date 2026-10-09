#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"
#include "OloEngine/Renderer/RayTracing/VegetationDiagnostics.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"

#include <span>
#include <cstring>
#include <type_traits>

namespace OloEngine::RayTracing
{
    namespace VegetationDiagnostics
    {
        namespace
        {
            bool s_ForceDetailed = false;
            u32 s_BudgetDivisor = 1u;
        } // namespace
        void SetForceDetailed(bool enabled)
        {
            s_ForceDetailed = enabled;
        }
        bool GetForceDetailed()
        {
            return s_ForceDetailed;
        }
        void SetBudgetDivisor(u32 divisor)
        {
            s_BudgetDivisor = std::clamp(divisor, 1u, 1024u);
        }
        u32 GetBudgetDivisor()
        {
            return s_BudgetDivisor;
        }
    } // namespace VegetationDiagnostics
    namespace
    {
        struct alignas(16) DeformParams
        {
            glm::mat4 Model{ 1.0f };
            glm::uvec2 Rest{};
            glm::uvec2 Jobs{};
            glm::uvec2 Tasks{};
            u32 TaskCount = 0u;
            u32 Padding = 0u;
        };
        static_assert(sizeof(DeformParams) == 96u);
        struct alignas(8) DispatchGroup
        {
            glm::uvec2 Rows{};
            glm::uvec2 Output{};
            u32 VertexCount = 0u;
            u32 PlantCount = 0u;
        };
        static_assert(sizeof(DispatchGroup) == 24u);

        glm::uvec2 Address(u64 value)
        {
            return { static_cast<u32>(value), static_cast<u32>(value >> 32u) };
        }

        // Eight bytes a step (#1533). Byte-at-a-time FNV over every plant row
        // cost 9.4 ms a frame once the showcase lawn's 216k plants were queued
        // for reflections; the value only has to change when the state does.
        struct Hash
        {
            u64 Value = 1469598103934665603ull;
            void MixBytes(const void* data, sizet size)
            {
                const auto* bytes = static_cast<const std::byte*>(data);
                sizet offset = 0u;
                for (; offset + sizeof(u64) <= size; offset += sizeof(u64))
                {
                    u64 word = 0u;
                    std::memcpy(&word, bytes + offset, sizeof(u64));
                    Step(word);
                }
                if (offset < size)
                {
                    u64 tail = 0u;
                    std::memcpy(&tail, bytes + offset, size - offset);
                    Step(tail ^ (static_cast<u64>(size - offset) << 56u));
                }
            }
            template<typename T>
            void Mix(const T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                MixBytes(&value, sizeof(T));
            }
            void Step(u64 word)
            {
                Value = (Value ^ word) * 1099511628211ull;
                Value ^= Value >> 29u;
            }
        };

        f32 WindTime(const VegetationSurfaceInput& input)
        {
            const auto& wind = input.Wind;
            const bool legacyField = wind.WindWeights.x + wind.WindWeights.y + wind.WindWeights.z <= 0.0f &&
                                     wind.WindFlags.w > 0.5f && wind.ImpostorParams1.x <= 0.5f;
            return legacyField ? wind.WindClock.x : wind.Time;
        }

        // The rows sent, or the count of the rows held for a keyed group.
        u32 PlantCount(const VegetationSurfaceInput& input)
        {
            return input.Rows.IsEmpty() ? input.HeldPlantCount : static_cast<u32>(input.Rows.Num());
        }

        bool ValidInput(const VegetationSurfaceInput& input)
        {
            if (!input.Rest || input.Rest->GetDeviceAddress() == 0u || input.VertexCount == 0u ||
                PlantCount(input) == 0u || PlantCount(input) > VegetationPolicy::CardPlantsPerGroup ||
                (input.Rows.IsEmpty() && input.ContentKey == 0u) ||
                input.Parts.IsEmpty() ||
                !std::isfinite(WindTime(input)) || !std::isfinite(input.DistanceToView) ||
                !std::isfinite(input.DetailedDistance) || !std::isfinite(input.VelocityBound) ||
                input.VelocityBound < 0.0f)
                return false;
            for (const auto& lane : { input.Wind.WindDirection, input.Wind.WindGust,
                                      input.Wind.WindClock, input.Wind.WindWeights, input.Wind.WindFlags })
                for (u32 i = 0u; i < 4u; ++i)
                    if (!std::isfinite(lane[i]))
                        return false;
            if (!std::isfinite(input.Wind.WindStrength) || !std::isfinite(input.Wind.WindSpeed) ||
                input.Wind.WindWeights.x < 0.0f || input.Wind.WindWeights.x > 1.0f)
                return false;
            u64 sourceIndices = 0u;
            for (i32 p = 0; p < input.Parts.Num(); ++p)
            {
                const auto& part = input.Parts[p];
                // A handful of parts: a pairwise slot check, no set per group.
                for (i32 earlier = 0; earlier < p; ++earlier)
                    if (input.Parts[earlier].Slot == part.Slot)
                        return false;
                if (part.Indices.IsEmpty() || part.Indices.Num() % 3u != 0u ||
                    static_cast<u64>(part.Indices.Num()) > std::numeric_limits<u32>::max() - sourceIndices)
                    return false;
                sourceIndices += part.Indices.Num();
                for (const u32 index : part.Indices)
                    if (index >= input.VertexCount)
                        return false;
            }
            for (const auto& row : input.Rows)
            {
                for (const auto& lane : { row.PositionScale, row.RotationHeight, row.ColorAlpha })
                    for (u32 i = 0u; i < 4u; ++i)
                        if (!std::isfinite(lane[i]))
                            return false;
                if (row.PositionScale.w <= 0.0f || row.RotationHeight.y <= 0.0f)
                    return false;
            }
            for (u32 column = 0u; column < 4u; ++column)
                for (u32 row = 0u; row < 4u; ++row)
                    if (!std::isfinite(input.WorldTransform[column][row]))
                        return false;
            return true;
        }
    } // namespace

    VegetationSurfaceCache::VegetationSurfaceCache() = default;
    VegetationSurfaceCache::~VegetationSurfaceCache() = default;

    void VegetationSurfaceCache::SetEnabled(bool enabled)
    {
        if (m_Enabled && !enabled)
            Shutdown();
        m_Enabled = enabled;
    }

    void VegetationSurfaceCache::Shutdown()
    {
        m_Jobs.Reset();
        m_Inputs.Reset();
        m_Entries.clear();
        m_Shader.Reset();
        m_Params.Reset();
        m_Wind.Reset();
        m_Stats = {};
        m_StagedBytes = 0u;
        m_Enabled = false;
        m_PreviousComplete = true;
        m_PreviousLeftOut = false;
        m_PreviousSignature = 0u;
        m_BuildDebt = {};
    }

    void VegetationSurfaceCache::CountPressure(VegetationPressure pressure, u32 groups)
    {
        if (pressure != VegetationPressure::None && pressure != VegetationPressure::Count)
            m_Stats.Pressure[static_cast<sizet>(pressure)] += groups;
    }

    void VegetationSurfaceCache::CountCastingPlan(const VegetationPolicy::CastingPlan& plan, f32 nearestFallback, f32 frameSeconds)
    {
        if (!m_Enabled)
            return;
        m_Stats.CastingDemandBuilds += static_cast<f32>(plan.DemandBuilds);
        m_Stats.CastingDemandVertices += static_cast<f32>(plan.DemandVertices);
        m_Stats.CastingDemandTriangles += static_cast<f32>(plan.DemandTriangles);
        m_Stats.PlanFrameSeconds = frameSeconds;
        m_Stats.CastingGroupsPlanned += plan.Requested + plan.Fallbacks + plan.Out;
        m_Stats.CastingFallbackGroups += plan.Fallbacks;
        m_Stats.CastingGroupsLeftOut += plan.Out;
        if (plan.Fallbacks > 0u)
            m_Stats.NearestCastingFallback = m_Stats.NearestCastingFallback > 0.0f ? std::min(m_Stats.NearestCastingFallback, nearestFallback)
                                                                                   : nearestFallback;
        VegetationPolicy::Accumulate(m_Stats.Recovery, plan.Recovery);
        // The groups left out are refused by the producer (Refuse), which
        // counts their pressure; the fallbacks are counted here.
        CountPressure(plan.Pressure, plan.Fallbacks);
    }

    void VegetationSurfaceCache::RollbackJobs()
    {
        for (const auto& job : m_Jobs)
            if (auto entry = m_Entries.find(job.Identity); entry != m_Entries.end())
                entry->second.Valid = false;
        m_Jobs.Reset();
    }

    void VegetationSurfaceCache::BeginFrame()
    {
        RollbackJobs();
        ++m_Frame;
        m_Inputs.Reset();
        m_PreviousComplete = m_Stats.Complete;
        m_PreviousLeftOut = m_Stats.BeyondReflectionBudget > 0u;
        m_StagedBytes = 0u;
        m_Stats = {};
        for (const auto& [key, entry] : m_Entries)
        {
            static_cast<void>(key);
            m_Stats.ResidentBytes += entry.Bytes;
        }
    }

    bool VegetationSurfaceCache::HoldsContent(u64 owner, u64 firstPlantId, const Ref<VertexBuffer>& rest, u64 contentKey) const
    {
        if (!rest || contentKey == 0u)
            return false;
        const auto found = m_Entries.find(Key{ owner, firstPlantId, RHI::HashKey(rest->GetRHIHandle()) });
        return found != m_Entries.end() && found->second.ContentKey == contentKey;
    }

    void VegetationSurfaceCache::Refuse(bool castsShadows, VegetationPressure pressure)
    {
        if (!m_Enabled)
            return;
        ++m_Stats.GroupsRequested;
        ++m_Stats.Refused;
        m_Stats.Complete = false;
        m_Stats.CastersComplete = m_Stats.CastersComplete && !castsShadows;
        CountPressure(pressure);
    }

    void VegetationSurfaceCache::CountBeyondReflectionBudget(u32 groups, u32 plants, f32 reach, f32 detailReach)
    {
        if (!m_Enabled)
            return;
        m_Stats.BeyondReflectionBudget += groups;
        m_Stats.PlantsBeyondReflectionBudget += plants;
        m_Stats.ReflectionReach = std::max(m_Stats.ReflectionReach, reach);
        m_Stats.ReflectionDetailReach = std::max(m_Stats.ReflectionDetailReach, detailReach);
    }

    void VegetationSurfaceCache::Queue(VegetationSurfaceInput input)
    {
        if (!m_Enabled)
            return;
        ++m_Stats.GroupsRequested;
        if (!ValidInput(input))
        {
            ++m_Stats.Refused;
            m_Stats.Complete = false;
            m_Stats.CastersComplete = m_Stats.CastersComplete && !input.CastShadows;
            CountPressure(VegetationPressure::InvalidContent);
            return;
        }
        u64 sourceIndices = 0u;
        for (const auto& part : input.Parts)
            sourceIndices += part.Indices.Num();
        const u64 stagedBytes = PlantCount(input) * (static_cast<u64>(input.VertexCount) * sizeof(Vertex) +
                                                     sourceIndices * sizeof(u32) + sizeof(FoliageInstanceData));
        if (!HasQueueCapacity() || stagedBytes > VegetationPolicy::GeometryBytes - m_StagedBytes ||
            input.AccelerationBytes > VegetationPolicy::AccelerationStructureBytes - m_Stats.StagedAccelerationBytes)
        {
            ++m_Stats.Refused;
            m_Stats.Complete = false;
            m_Stats.CastersComplete = m_Stats.CastersComplete && !input.CastShadows;
            if (static_cast<u32>(m_Inputs.Num()) >= VegetationPolicy::ResidentGroups)
                CountPressure(VegetationPressure::ResidentGroups);
            else if (input.AccelerationBytes > VegetationPolicy::AccelerationStructureBytes - m_Stats.StagedAccelerationBytes)
                CountPressure(VegetationPressure::AccelerationMemory);
            else
                CountPressure(VegetationPressure::GeometryMemory);
            return;
        }
        m_StagedBytes += stagedBytes;
        m_Stats.StagedAccelerationBytes += input.AccelerationBytes;
        m_Inputs.Add(std::move(input));
    }

    bool VegetationSurfaceCache::EnsureShader()
    {
        if (!m_Shader)
            m_Shader = ComputeShader::Create("assets/shaders/VegetationDeformToBuffer.comp");
        if (!m_Shader || !m_Shader->IsValid())
            return false;
        if (!m_Params)
            m_Params = UniformBuffer::Create(sizeof(DeformParams), ShaderBindingLayout::UBO_RAY_TRACING);
        if (!m_Wind)
            m_Wind = UniformBuffer::Create(sizeof(ShaderBindingLayout::FoliageUBO), ShaderBindingLayout::UBO_FOLIAGE);
        return m_Params && m_Wind;
    }

    void VegetationSurfaceCache::FinishExtraction(GPUScene& scene)
    {
        OLO_PERF_SCOPE_AUTO("Vegetation::FinishExtraction");
        if (!m_Enabled)
            return;
        for (const auto& input : m_Inputs)
            if (auto found = m_Entries.find(Key{ input.Owner, input.FirstPlantId, RHI::HashKey(input.Rest->GetRHIHandle()) }); found != m_Entries.end())
                found->second.LastSeen = m_Frame;
        // A retired group is not a history reset by itself (#1354): a camera
        // move re-slices plants into new groups at the same tier, and the
        // representation signature below says whether the traced plants or
        // their tiers actually changed.
        std::erase_if(m_Entries, [this](const auto& item)
                      {
            if (item.second.LastSeen == m_Frame)
                return false;
            m_Stats.ResidentBytes -= item.second.Bytes;
            ++m_Stats.GroupsRetired;
            return true; });
        u64 signature = 0u;
        // Casting groups first: shadow rays need every one of them, and a
        // reflection-only group can keep its snapshot when its refresh does not
        // fit (below), so it must not take the budget a caster needs (#1533).
        // Then new groups, then oldest snapshots. Stable identity breaks ties
        // so generator iteration order cannot decide which work gets a budget.
        // Sorted as an index with each key read once: a comparator that looked
        // up both entries, over inputs that are a kilobyte each to move, cost
        // milliseconds at the showcase lawn's thousand groups.
        struct Order
        {
            Key Identity;
            f32 SnapshotTime = 0.0f;
            u32 Index = 0u;
            bool Casts = false;
        };
        TArray<Order> order;
        order.Reserve(m_Inputs.Num());
        for (i32 i = 0; i < m_Inputs.Num(); ++i)
        {
            const auto& input = m_Inputs[i];
            const Key key{ input.Owner, input.FirstPlantId, RHI::HashKey(input.Rest->GetRHIHandle()) };
            const auto found = m_Entries.find(key);
            const f32 time = found == m_Entries.end() || !found->second.Valid ? -std::numeric_limits<f32>::infinity()
                                                                              : found->second.SnapshotTime;
            order.Add({ key, time, static_cast<u32>(i), input.CastShadows });
        }
        std::sort(order.GetData(), order.GetData() + order.Num(), [](const Order& a, const Order& b)
                  {
            if (a.Casts != b.Casts)
                return a.Casts;
            return a.SnapshotTime < b.SnapshotTime || (!(b.SnapshotTime < a.SnapshotTime) && a.Identity < b.Identity); });
        VegetationFrameBudget budget;
        budget.Charge(m_BuildDebt);
        // The stress lever (VegetationDiagnostics::SetBudgetDivisor): all but
        // 1/divisor of the frame is spent before anything is reserved.
        if (const u32 divisor = VegetationDiagnostics::GetBudgetDivisor(); divisor > 1u)
            budget.Charge({ VegetationPolicy::UpdatesPerFrame - VegetationPolicy::UpdatesPerFrame / divisor,
                            VegetationPolicy::VerticesPerFrame - VegetationPolicy::VerticesPerFrame / divisor,
                            VegetationPolicy::TrianglesPerFrame - VegetationPolicy::TrianglesPerFrame / divisor });
        m_Stats.CarriedBuilds = m_BuildDebt.Builds;
        m_BuildDebt = {};
        const bool shaderReady = m_Inputs.IsEmpty() || EnsureShader();
        for (const Order& next : order)
        {
            const auto& input = m_Inputs[static_cast<i32>(next.Index)];
            const Key key{ input.Owner, input.FirstPlantId, RHI::HashKey(input.Rest->GetRHIHandle()) };
            const u32 plants = PlantCount(input);
            const u64 vertices = static_cast<u64>(input.VertexCount) * plants;
            u64 indexCount = 0u;
            for (const auto& part : input.Parts)
                indexCount += static_cast<u64>(part.Indices.Num()) * plants;
            const u64 bytes = vertices * sizeof(Vertex) + indexCount * sizeof(u32) + plants * sizeof(FoliageInstanceData);
            auto found = m_Entries.find(key);
            const u64 oldBytes = found == m_Entries.end() ? 0u : found->second.Bytes;
            const auto refuse = [this, casts = input.CastShadows](VegetationPressure pressure)
            {
                ++m_Stats.Refused;
                m_Stats.Complete = false;
                m_Stats.CastersComplete = m_Stats.CastersComplete && !casts;
                CountPressure(pressure);
            };
            if (!shaderReady)
            {
                refuse(VegetationPressure::ProducerFailure);
                continue;
            }
            if (found == m_Entries.end() && m_Entries.size() >= VegetationPolicy::ResidentGroups)
            {
                refuse(VegetationPressure::ResidentGroups);
                continue;
            }
            if (bytes > VegetationPolicy::GeometryBytes - (m_Stats.ResidentBytes - oldBytes))
            {
                refuse(VegetationPressure::GeometryMemory);
                continue;
            }
            if (vertices > std::numeric_limits<u32>::max() / sizeof(Vertex) ||
                indexCount > std::numeric_limits<u32>::max() / sizeof(u32))
            {
                refuse(VegetationPressure::InvalidContent);
                continue;
            }

            Hash state;
            state.Mix(RHI::HashKey(input.Rest->GetRHIHandle()));
            state.Mix(input.VertexCount);
            // The producer's key stands for the rows when it gives one;
            // otherwise the whole rows, contiguous (their colour lane is state).
            if (input.ContentKey != 0u)
                state.Mix(input.ContentKey);
            else
                state.MixBytes(input.Rows.GetData(), static_cast<sizet>(input.Rows.Num()) * sizeof(FoliageInstanceData));
            state.Mix(plants);
            for (const auto& part : input.Parts)
            {
                state.Mix(part.Slot);
                state.Mix(part.Indices.Num());
                state.MixBytes(part.Indices.GetData(), static_cast<sizet>(part.Indices.Num()) * sizeof(u32));
            }
            Hash parameters;
            parameters.Mix(input.WorldTransform);
            parameters.Mix(input.Wind.WindDirection);
            parameters.Mix(input.Wind.WindGust);
            parameters.Mix(input.Wind.WindWeights);
            parameters.Mix(input.Wind.WindStrength);
            parameters.Mix(input.Wind.WindSpeed);
            parameters.Mix(input.Wind.WindFlags.w);
            parameters.Mix(input.Wind.MeshParams);
            parameters.Mix(input.Wind.ImpostorParams1);
            parameters.Mix(input.VelocityBound);
            for (const auto& part : input.Parts)
            {
                parameters.Mix(part.Material.m_BaseColorFactor);
                parameters.Mix(part.Material.m_RoughnessFactor);
                parameters.Mix(part.Material.m_AlphaCutoff);
                parameters.Mix(part.Material.m_AlphaMode);
                parameters.Mix(RHI::HashKey(part.Material.m_Albedo.m_Handle));
            }

            const f32 time = WindTime(input);
            const f32 ageLimit = VegetationPolicy::ProxyAgeLimit(input.VelocityBound);
            bool proxy = !VegetationDiagnostics::GetForceDetailed() &&
                         input.DistanceToView > input.DetailedDistance && ageLimit > 0.0f;
            // Shift the refresh grid by canonical identity to spread refits and
            // periodic full rebuilds; never shift the actual sampled wind time.
            const double phase = static_cast<double>(FoliageWindPhase(input.FirstPlantId)) / 6.2831853;
            const double bucketValue = proxy ? std::floor(static_cast<double>(time) / ageLimit + phase) : 0.0;
            if (bucketValue < static_cast<double>(std::numeric_limits<i64>::min()) ||
                bucketValue >= static_cast<double>(std::numeric_limits<i64>::max()))
                proxy = false;
            const i64 bucket = proxy ? static_cast<i64>(bucketValue) : 0;
            const bool changed = found == m_Entries.end() || found->second.StateHash != state.Value;
            // Rows held, not sent: only for a group resident under this key.
            if (input.Rows.IsEmpty() && (changed || found->second.ContentKey != input.ContentKey))
            {
                OLO_CORE_WARN("[RayTracing] a vegetation group named rows the cache does not hold; refused");
                refuse(VegetationPressure::InvalidContent);
                continue;
            }
            const bool parametersChanged = found != m_Entries.end() && found->second.ParameterHash != parameters.Value;
            const bool reset = changed || parametersChanged || found->second.Proxy != proxy || !input.HistoryContinuous;
            const bool unchangedTime = !changed && Math::BitwiseEqual(time, found->second.SnapshotTime);
            const bool reuse = !reset && found->second.Valid &&
                               (unchangedTime || (proxy && found->second.SnapshotBucket == bucket &&
                                                  VegetationPolicy::CanReuseSnapshot(time, found->second.SnapshotTime, input.VelocityBound, true)));
            if (!reuse)
                ++(changed ? m_Stats.RefreshNew : reset              ? m_Stats.RefreshReset
                                              : !found->second.Valid ? m_Stats.RefreshInvalid
                                                                     : m_Stats.RefreshDue);
            // A group whose refresh does not fit this frame (#1533, #1354):
            //  1. HOLDS its snapshot while that is inside the declared error
            //     bound (VegetationPolicy::CanHoldSnapshot): the proxies'
            //     deadline for a caster, the looser reflection bound otherwise.
            //     Reduced cadence, never an obsolete pose.
            //  2. A reflection-only group with nothing holdable is DEFERRED:
            //     left out of the scene this frame and counted. Its plants are
            //     missing from reflections for a frame; refusing them would
            //     take the TLAS from every reflection ray instead.
            //  3. A caster with nothing holdable is refused: a missing caster
            //     leaks light, so shadow rays fall back to the raster.
            // Oldest snapshots are served first, so whatever was held or
            // deferred is first in line next frame.
            bool stale = false;
            // Charged as the backend will charge the builds: one per part, each
            // with the group's whole vertex stream (VegetationFrameBudget).
            const u32 parts = static_cast<u32>(input.Parts.Num());
            if (!reuse && !budget.Reserve(vertices * parts, indexCount / 3u, parts))
            {
                const VegetationPressure pressure = m_Stats.CarriedBuilds > 0u ? VegetationPressure::BuildDebt : VegetationPressure::FrameWork;
                VegetationPolicy::Accumulate(m_Stats.Recovery, { 0u, 0u, vertices * parts, indexCount / 3u, parts });
                const bool holdable = !reset && found->second.Valid &&
                                      VegetationPolicy::CanHoldSnapshot(time, found->second.SnapshotTime, input.VelocityBound, input.CastShadows);
                CountPressure(pressure);
                if (!holdable)
                {
                    if (!input.CastShadows)
                    {
                        ++m_Stats.DeferredReflectionGroups;
                        m_Stats.DeferredReflectionPlants += plants;
                        continue;
                    }
                    ++m_Stats.Refused;
                    m_Stats.Complete = false;
                    m_Stats.CastersComplete = false;
                    continue;
                }
                stale = true;
            }
            if (changed)
            {
                Entry replacement;
                replacement.Rest = input.Rest;
                replacement.Rows = VertexBuffer::Create(input.Rows.GetData(), static_cast<u32>(input.Rows.Num() * sizeof(FoliageInstanceData)));
                replacement.Output = VertexBuffer::Create(static_cast<u32>(vertices * sizeof(Vertex)));
                TArray<u32> indices;
                indices.Reserve(static_cast<sizet>(indexCount));
                for (const auto& part : input.Parts)
                    for (u32 plant = 0u; plant < plants; ++plant)
                        for (const u32 index : part.Indices)
                            indices.Add(index + plant * input.VertexCount);
                replacement.Indices = IndexBuffer::Create(indices.GetData(), static_cast<u32>(indices.Num()));
                if (!replacement.Rows || !replacement.Output || !replacement.Indices ||
                    replacement.Rows->GetDeviceAddress() == 0u || replacement.Output->GetDeviceAddress() == 0u ||
                    replacement.Indices->GetDeviceAddress() == 0u)
                {
                    refuse(VegetationPressure::ProducerFailure);
                    continue;
                }
                replacement.Bytes = bytes;
                replacement.ContentKey = input.ContentKey;
                if (found == m_Entries.end())
                    ++m_Stats.GroupsCreated;
                m_Stats.ResidentBytes = m_Stats.ResidentBytes - oldBytes + bytes;
                found = m_Entries.insert_or_assign(key, std::move(replacement)).first;
            }
            Entry& entry = found->second;
            entry.LastSeen = m_Frame;
            entry.StateHash = state.Value;
            entry.ParameterHash = parameters.Value;
            entry.Proxy = proxy;
            // A parameter edit or a wind discontinuity is a reset; a new or
            // changed group is one only if the signature below says so.
            m_Stats.HistoryReset |= parametersChanged || !input.HistoryContinuous;
            if (proxy)
                ++m_Stats.ProxyGroups;
            else
                ++m_Stats.DetailedGroups;
            if (stale)
                ++(input.CastShadows ? m_Stats.CadenceHolds : m_Stats.StaleReflectionSnapshots);
            if (reuse || stale)
                ++m_Stats.SnapshotsReused;
            else
            {
                if (entry.Revision == std::numeric_limits<u32>::max())
                {
                    entry.Valid = false;
                    refuse(VegetationPressure::ProducerFailure);
                    continue;
                }
                ++entry.Revision;
                entry.SnapshotTime = time;
                entry.SnapshotBucket = bucket;
                entry.Valid = true;
                m_Jobs.Add({ key, input.Wind,
                             MakeModelRelative(input.WorldTransform, scene.GetRenderOrigin()),
                             input.VertexCount, plants });
            }
            u32 firstIndex = 0u;
            for (const auto& part : input.Parts)
            {
                const u32 partIndices = static_cast<u32>(part.Indices.Num() * plants);
                const GPUSceneGeometryKey geometryKey{ RHI::HashKey(entry.Output->GetRHIHandle()),
                                                       RHI::HashKey(entry.Indices->GetRHIHandle()), part.Slot };
                const GPUSceneMaterialKey materialKey{ RHI::HashKey(input.Rest->GetRHIHandle()), part.Slot,
                                                       std::to_underlying(GPUSceneMaterialSource::Foliage) };
                scene.ExtractMaterial(materialKey, part.Material);
                scene.ExtractGeometry(geometryKey, {
                                                       .m_VertexBuffer = entry.Output->GetRHIHandle(),
                                                       .m_IndexBuffer = entry.Indices->GetRHIHandle(),
                                                       .m_VertexAddress = entry.Output->GetDeviceAddress(),
                                                       .m_IndexAddress = entry.Indices->GetDeviceAddress(),
                                                       .m_VertexFormat = std::to_underlying(GPUSceneVertexFormat::OloVertex),
                                                       .m_IndexFormat = std::to_underlying(GPUSceneIndexFormat::UInt32),
                                                       .m_FirstIndex = firstIndex,
                                                       .m_IndexCount = partIndices,
                                                       .m_VertexCount = static_cast<u32>(vertices),
                                                       .m_Flags = GPUSceneGeometryFlagDeformed | GPUSceneGeometryFlagVegetation,
                                                   });
                scene.ExtractInstance({ input.Owner, geometryKey, input.FirstPlantId },
                                      {
                                          .m_WorldTransform = input.WorldTransform,
                                          .m_Material = materialKey,
                                          .m_VisibilityMask = input.CastShadows ? GPUSceneInstanceInput{}.m_VisibilityMask
                                                                                : kVisibilityMaskNoShadowCast,
                                          .m_Flags = GPUSceneInstanceFlagAnimated,
                                          .m_DeformedContentRevision = entry.Revision,
                                      });
                firstIndex += partIndices;
            }
            m_Stats.PlantsRepresented += plants;
            // The far groups' fairness: how old a published snapshot gets,
            // and the displacement its velocity bound allows.
            const f32 age = std::max(time - entry.SnapshotTime, 0.0f);
            if (age > m_Stats.OldestSnapshotAge)
            {
                m_Stats.OldestSnapshotAge = age;
                m_Stats.OldestSnapshotError = age * input.VelocityBound;
            }
            // Order-free over plants: each plant's term scaled by an odd
            // factor naming its tier, so regrouping sums to the same value.
            Hash tier;
            tier.Mix(RHI::HashKey(input.Rest->GetRHIHandle()));
            tier.Mix(input.VertexCount);
            tier.Mix(static_cast<u32>(proxy) | (static_cast<u32>(input.CastShadows) << 1u));
            tier.Mix(input.ContentGeneration);
            const u64 plantSet = input.PlantSetSum != 0u ? input.PlantSetSum
                                                         : VegetationPlantTerm(input.FirstPlantId) * static_cast<u64>(plants);
            signature += plantSet * (tier.Value | 1u);
        }
        m_Inputs.Reset();
        m_Stats.HistoryReset |= signature != m_PreviousSignature;
        m_PreviousSignature = signature;
        m_Stats.HistoryReset |= m_Stats.Complete != m_PreviousComplete;
        const bool leftOut = m_Stats.BeyondReflectionBudget > 0u;
        if (leftOut && !m_PreviousLeftOut)
        {
            OLO_CORE_INFO("[RayTracing] vegetation that casts no shadow is held for reflections out to {:.1f} m: {} "
                          "groups ({} plants) past the vegetation budget are left out of the ray-traced scene",
                          m_Stats.ReflectionReach, m_Stats.BeyondReflectionBudget, m_Stats.PlantsBeyondReflectionBudget);
        }
        else if (!leftOut && m_PreviousLeftOut)
        {
            OLO_CORE_INFO("[RayTracing] all the vegetation reflections read fits the vegetation budget again");
        }
    }

    u32 VegetationSurfaceCache::Dispatch()
    {
        OLO_PERF_SCOPE_AUTO("Vegetation::Dispatch");
        if (m_Jobs.IsEmpty())
            return 0u;
        if (!m_Enabled || !EnsureShader())
        {
            m_Stats.Complete = false;
            m_Stats.ProducerFailed = true;
            m_Stats.Refused += static_cast<u32>(m_Jobs.Num());
            CountPressure(VegetationPressure::ProducerFailure, static_cast<u32>(m_Jobs.Num()));
            RollbackJobs();
            return 0u;
        }
        // Protect previous-frame AS/hit reads and refits before rewriting an
        // output in place. The Vulkan backend lowers this to ALL_COMMANDS and
        // MEMORY_READ|MEMORY_WRITE; the producer-to-AS edge follows dispatch.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
        TArray<Batch> batches;
        for (const auto& job : m_Jobs)
        {
            const u64 owner = job.Identity[0], restHandle = job.Identity[2];
            auto batch = std::find_if(batches.begin(), batches.end(), [&](const Batch& candidate)
                                      { return candidate.Owner == owner && candidate.RestHandle == restHandle &&
                                               Math::BitwiseEqual(candidate.Parameters->Wind, job.Wind) &&
                                               Math::BitwiseEqual(candidate.Parameters->Model, job.Model); });
            if (batch == batches.end())
            {
                batches.Add({ owner, restHandle, &job, {} });
                batch = std::prev(batches.end());
            }
            batch->Jobs.Add(&job);
        }
        for (const auto& batch : batches)
        {
            TArray<DispatchGroup> groups;
            TArray<glm::uvec2> tasks;
            u32 batchVertices = 0u;
            for (const auto* job : batch.Jobs)
            {
                auto& entry = m_Entries.at(job->Identity);
                const u32 group = static_cast<u32>(groups.Num());
                groups.Add({ Address(entry.Rows->GetDeviceAddress()), Address(entry.Output->GetDeviceAddress()),
                             job->VertexCount, job->PlantCount });
                const u32 vertices = job->VertexCount * job->PlantCount;
                for (u32 firstVertex = 0u; firstVertex < vertices; firstVertex += 64u)
                    tasks.Emplace(group, firstVertex);
                batchVertices += vertices;
            }
            const u32 groupBytes = static_cast<u32>(groups.Num() * sizeof(DispatchGroup));
            const u32 taskBytes = static_cast<u32>(tasks.Num() * sizeof(glm::uvec2));
            auto groupBuffer = StorageBuffer::Create(groupBytes, StorageBuffer::kNoBinding);
            auto taskBuffer = StorageBuffer::Create(taskBytes, StorageBuffer::kNoBinding);
            if (!groupBuffer || !taskBuffer || groupBuffer->GetDeviceAddress() == 0u || taskBuffer->GetDeviceAddress() == 0u)
            {
                m_Stats.Complete = false;
                m_Stats.ProducerFailed = true;
                m_Stats.HistoryReset = true;
                m_Stats.Refused += static_cast<u32>(batch.Jobs.Num());
                CountPressure(VegetationPressure::ProducerFailure, static_cast<u32>(batch.Jobs.Num()));
                for (const auto* job : batch.Jobs)
                    m_Entries.at(job->Identity).Valid = false;
                continue;
            }
            groupBuffer->SetData(groups.GetData(), groupBytes);
            taskBuffer->SetData(tasks.GetData(), taskBytes);
            const auto& parameters = *batch.Parameters;
            const auto& entry = m_Entries.at(parameters.Identity);
            const DeformParams params{ parameters.Model, Address(entry.Rest->GetDeviceAddress()),
                                       Address(groupBuffer->GetDeviceAddress()), Address(taskBuffer->GetDeviceAddress()),
                                       static_cast<u32>(tasks.Num()), 0u };
            // Bind BEFORE every dispatch (#1437). SetData only writes the
            // buffer; the dispatch reads whatever currently OCCUPIES the
            // binding point, and both points are shared. UBO_RAY_TRACING (65)
            // has six other tenants, and DeformedSurfaceCache binds its own
            // 48-byte block there one pass earlier whenever an animated
            // surface deforms. Without this bind the shader read that block
            // as its 96-byte one: Rest, Jobs and Tasks came from past its end,
            // and the device faulted on a READ of 0x10000000000 in
            // RayTracingScenePass. UBO_FOLIAGE is shared with the raster
            // foliage path in the same way.
            m_Params->Bind();
            m_Wind->Bind();
            m_Params->SetData(&params, sizeof(params));
            m_Wind->SetData(&parameters.Wind, sizeof(parameters.Wind));
            m_Shader->Bind();
            const u64 recordedBefore = m_Shader->GetRecordedDispatchCount();
            RenderCommand::DispatchCompute(params.TaskCount, 1u, 1u);
            if (m_Shader->GetRecordedDispatchCount() == recordedBefore)
            {
                m_Stats.Complete = false;
                m_Stats.ProducerFailed = true;
                m_Stats.HistoryReset = true;
                m_Stats.Refused += static_cast<u32>(batch.Jobs.Num());
                CountPressure(VegetationPressure::ProducerFailure, static_cast<u32>(batch.Jobs.Num()));
                for (const auto* job : batch.Jobs)
                    m_Entries.at(job->Identity).Valid = false;
                continue;
            }
            m_Stats.Dispatched += static_cast<u32>(batch.Jobs.Num());
            ++m_Stats.DispatchBatches;
            m_Stats.VerticesDeformed += batchVertices;
        }
        m_Jobs.Reset();
        return m_Stats.Dispatched;
    }
} // namespace OloEngine::RayTracing
