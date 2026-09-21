#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"
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
#include <unordered_set>

namespace OloEngine::RayTracing
{
    namespace VegetationDiagnostics
    {
        namespace
        {
            bool s_ForceDetailed = false;
        }
        void SetForceDetailed(bool enabled)
        {
            s_ForceDetailed = enabled;
        }
        bool GetForceDetailed()
        {
            return s_ForceDetailed;
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

        struct Hash
        {
            u64 Value = 1469598103934665603ull;
            template<typename T>
            void Mix(const T& value)
            {
                for (const std::byte byte : std::as_bytes(std::span(&value, 1u)))
                {
                    Value ^= std::to_integer<u8>(byte);
                    Value *= 1099511628211ull;
                }
            }
        };

        f32 WindTime(const VegetationSurfaceInput& input)
        {
            const auto& wind = input.Wind;
            const bool legacyField = wind.WindWeights.x + wind.WindWeights.y + wind.WindWeights.z <= 0.0f &&
                                     wind.WindFlags.w > 0.5f && wind.ImpostorParams1.x <= 0.5f;
            return legacyField ? wind.WindClock.x : wind.Time;
        }

        bool ValidInput(const VegetationSurfaceInput& input)
        {
            if (!input.Rest || input.Rest->GetDeviceAddress() == 0u || input.VertexCount == 0u ||
                input.Rows.IsEmpty() || static_cast<u32>(input.Rows.Num()) > VegetationPolicy::CardPlantsPerGroup ||
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
            std::unordered_set<u32> slots;
            for (const auto& part : input.Parts)
            {
                if (part.Indices.IsEmpty() || part.Indices.Num() % 3u != 0u || !slots.insert(part.Slot).second ||
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
        m_StagedBytes = 0u;
        m_Stats = {};
        for (const auto& [key, entry] : m_Entries)
        {
            static_cast<void>(key);
            m_Stats.ResidentBytes += entry.Bytes;
        }
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
            return;
        }
        u64 sourceIndices = 0u;
        for (const auto& part : input.Parts)
            sourceIndices += part.Indices.Num();
        const u64 stagedBytes = input.Rows.Num() * (static_cast<u64>(input.VertexCount) * sizeof(Vertex) +
                                                    sourceIndices * sizeof(u32) + sizeof(FoliageInstanceData));
        if (!HasQueueCapacity() || stagedBytes > VegetationPolicy::GeometryBytes - m_StagedBytes)
        {
            ++m_Stats.Refused;
            m_Stats.Complete = false;
            return;
        }
        m_StagedBytes += stagedBytes;
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
        if (!m_Enabled)
            return;
        for (const auto& input : m_Inputs)
            if (auto found = m_Entries.find(Key{ input.Owner, input.FirstPlantId, RHI::HashKey(input.Rest->GetRHIHandle()) }); found != m_Entries.end())
                found->second.LastSeen = m_Frame;
        std::erase_if(m_Entries, [this](const auto& item)
                      {
            if (item.second.LastSeen == m_Frame)
                return false;
            m_Stats.ResidentBytes -= item.second.Bytes;
            m_Stats.HistoryReset = true;
            return true; });
        // New groups first, then oldest snapshots. Stable identity breaks ties
        // so generator iteration order cannot decide which work gets a budget.
        std::sort(m_Inputs.begin(), m_Inputs.end(), [this](const auto& a, const auto& b)
                  {
            const Key ak{ a.Owner, a.FirstPlantId, RHI::HashKey(a.Rest->GetRHIHandle()) }, bk{ b.Owner, b.FirstPlantId, RHI::HashKey(b.Rest->GetRHIHandle()) };
            const auto ai = m_Entries.find(ak), bi = m_Entries.find(bk);
            const f32 at = ai == m_Entries.end() || !ai->second.Valid ? -std::numeric_limits<f32>::infinity() : ai->second.SnapshotTime;
            const f32 bt = bi == m_Entries.end() || !bi->second.Valid ? -std::numeric_limits<f32>::infinity() : bi->second.SnapshotTime;
            return at < bt || (!(bt < at) && ak < bk); });
        VegetationFrameBudget budget;
        const bool shaderReady = m_Inputs.IsEmpty() || EnsureShader();
        for (const auto& input : m_Inputs)
        {
            const Key key{ input.Owner, input.FirstPlantId, RHI::HashKey(input.Rest->GetRHIHandle()) };
            const u64 vertices = static_cast<u64>(input.VertexCount) * input.Rows.Num();
            u64 indexCount = 0u;
            for (const auto& part : input.Parts)
                indexCount += static_cast<u64>(part.Indices.Num()) * input.Rows.Num();
            const u64 bytes = vertices * sizeof(Vertex) + indexCount * sizeof(u32) + input.Rows.Num() * sizeof(FoliageInstanceData);
            auto found = m_Entries.find(key);
            const u64 oldBytes = found == m_Entries.end() ? 0u : found->second.Bytes;
            const auto refuse = [this]()
            { ++m_Stats.Refused; m_Stats.Complete = false; };
            if (!shaderReady || (found == m_Entries.end() && m_Entries.size() >= VegetationPolicy::ResidentGroups) || bytes > VegetationPolicy::GeometryBytes - (m_Stats.ResidentBytes - oldBytes) ||
                vertices > std::numeric_limits<u32>::max() / sizeof(Vertex) ||
                indexCount > std::numeric_limits<u32>::max() / sizeof(u32))
            {
                refuse();
                continue;
            }

            Hash state;
            state.Mix(RHI::HashKey(input.Rest->GetRHIHandle()));
            state.Mix(input.VertexCount);
            for (const auto& row : input.Rows)
            {
                state.Mix(row.PositionScale);
                state.Mix(row.RotationHeight);
            }
            for (const auto& part : input.Parts)
            {
                state.Mix(part.Slot);
                state.Mix(part.Indices.Num());
                for (const auto index : part.Indices)
                    state.Mix(index);
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
            const bool reset = changed || found->second.ParameterHash != parameters.Value ||
                               found->second.Proxy != proxy || !input.HistoryContinuous;
            const bool unchangedTime = !changed && Math::BitwiseEqual(time, found->second.SnapshotTime);
            const bool reuse = !reset && found->second.Valid &&
                               (unchangedTime || (proxy && found->second.SnapshotBucket == bucket &&
                                                  VegetationPolicy::CanReuseSnapshot(time, found->second.SnapshotTime, input.VelocityBound, true)));
            if (!reuse && !budget.Reserve(vertices, indexCount / 3u))
            {
                refuse();
                continue;
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
                    for (u32 plant = 0u; plant < static_cast<u32>(input.Rows.Num()); ++plant)
                        for (const u32 index : part.Indices)
                            indices.Add(index + plant * input.VertexCount);
                replacement.Indices = IndexBuffer::Create(indices.GetData(), static_cast<u32>(indices.Num()));
                if (!replacement.Rows || !replacement.Output || !replacement.Indices ||
                    replacement.Rows->GetDeviceAddress() == 0u || replacement.Output->GetDeviceAddress() == 0u ||
                    replacement.Indices->GetDeviceAddress() == 0u)
                {
                    refuse();
                    continue;
                }
                replacement.Bytes = bytes;
                m_Stats.ResidentBytes = m_Stats.ResidentBytes - oldBytes + bytes;
                found = m_Entries.insert_or_assign(key, std::move(replacement)).first;
            }
            Entry& entry = found->second;
            entry.LastSeen = m_Frame;
            entry.StateHash = state.Value;
            entry.ParameterHash = parameters.Value;
            entry.Proxy = proxy;
            m_Stats.HistoryReset |= reset;
            if (proxy)
                ++m_Stats.ProxyGroups;
            else
                ++m_Stats.DetailedGroups;
            if (reuse)
                ++m_Stats.SnapshotsReused;
            else
            {
                if (entry.Revision == std::numeric_limits<u32>::max())
                {
                    entry.Valid = false;
                    refuse();
                    continue;
                }
                ++entry.Revision;
                entry.SnapshotTime = time;
                entry.SnapshotBucket = bucket;
                entry.Valid = true;
                m_Jobs.Add({ key, input.Wind,
                             MakeModelRelative(input.WorldTransform, scene.GetRenderOrigin()),
                             input.VertexCount, static_cast<u32>(input.Rows.Num()) });
            }
            u32 firstIndex = 0u;
            for (const auto& part : input.Parts)
            {
                const u32 partIndices = static_cast<u32>(part.Indices.Num() * input.Rows.Num());
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
                scene.ExtractInstance({ input.Owner, geometryKey, input.FirstPlantId }, {
                                                                                            .m_WorldTransform = input.WorldTransform,
                                                                                            .m_Material = materialKey,
                                                                                            .m_Flags = GPUSceneInstanceFlagAnimated,
                                                                                            .m_DeformedContentRevision = entry.Revision,
                                                                                        });
                firstIndex += partIndices;
            }
            m_Stats.PlantsRepresented += static_cast<u32>(input.Rows.Num());
        }
        m_Inputs.Reset();
        m_Stats.HistoryReset |= m_Stats.Complete != m_PreviousComplete;
    }

    u32 VegetationSurfaceCache::Dispatch()
    {
        if (m_Jobs.IsEmpty())
            return 0u;
        if (!m_Enabled || !EnsureShader())
        {
            m_Stats.Complete = false;
            m_Stats.ProducerFailed = true;
            m_Stats.Refused += static_cast<u32>(m_Jobs.Num());
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
