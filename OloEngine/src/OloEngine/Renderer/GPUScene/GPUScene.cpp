#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Math/Math.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

namespace OloEngine
{
    static_assert(GPUSceneAllocationPolicy::RetirementFrameCount ==
                  FrameResourceManager::NUM_BUFFERED_FRAMES);

    namespace
    {
        [[nodiscard]] bool AdvanceGeneration(u32& generation)
        {
            const u32 next = GPUSceneAllocationPolicy::NextGeneration(generation);
            if (next == 0)
            {
                return false;
            }
            generation = next;
            return true;
        }

        [[nodiscard]] GPUSceneTransform EncodeTransform(const glm::mat4& worldTransform,
                                                        const glm::vec3& renderOrigin)
        {
            const glm::mat4 relative = MakeModelRelative(worldTransform, renderOrigin);
            return GPUSceneTransform{
                .Row0 = { relative[0][0], relative[1][0], relative[2][0], relative[3][0] },
                .Row1 = { relative[0][1], relative[1][1], relative[2][1], relative[3][1] },
                .Row2 = { relative[0][2], relative[1][2], relative[2][2], relative[3][2] },
            };
        }

        template<typename T>
        [[nodiscard]] bool RecordsEqual(const T& lhs, const T& rhs)
        {
            return Math::BitwiseEqual(lhs, rhs);
        }

        [[nodiscard]] std::vector<GPUSceneDirtyRange> CoalesceDirtyRanges(const std::set<u32>& indices)
        {
            std::vector<GPUSceneDirtyRange> ranges;
            for (u32 index : indices)
            {
                if (!ranges.empty() && ranges.back().m_FirstIndex + ranges.back().m_Count == index)
                {
                    ++ranges.back().m_Count;
                    continue;
                }
                ranges.push_back(GPUSceneDirtyRange{ index, 1 });
            }
            return ranges;
        }

        template<typename T>
        [[nodiscard]] u32 BytesForRecords(u32 count)
        {
            OLO_CORE_ASSERT(count <= std::numeric_limits<u32>::max() / sizeof(T),
                            "GPUScene record buffer exceeds the 32-bit StorageBuffer size contract");
            return count * static_cast<u32>(sizeof(T));
        }

        [[nodiscard]] std::vector<GPUSceneDirtyRange> FullRange(u32 count)
        {
            return count > 0 ? std::vector<GPUSceneDirtyRange>{ GPUSceneDirtyRange{ 0, count } }
                             : std::vector<GPUSceneDirtyRange>{};
        }

        struct RetiredSlot
        {
            u32 m_Index = 0;
            u64 m_ReadyFrame = 0;
        };

        // A dead slot's record: default bytes, the slot's own index where the
        // record carries one, and the generation the slot advanced to, so a
        // consumer holding the old handle reads a mismatch rather than stale data.
        template<typename Record>
        void MakeTombstone(Record& record, u32 index, u32 generation)
        {
            record = Record{};
            if constexpr (requires { record.StableIndex; })
            {
                record.StableIndex = index;
            }
            record.Generation = generation;
        }

        // The compatibility rule of the kinds whose identity never changes in
        // place (geometries, lights, instances): every edit keeps the slot.
        struct AlwaysCompatible
        {
            template<typename Record>
            [[nodiscard]] bool operator()(const Record&, const Record&) const
            {
                return true;
            }
        };

        // One registry per record kind. Every kind shares the allocation,
        // retirement, dirty-tracking, upload and reset machinery; only the
        // per-kind encoding differs and is passed into CommitStaged. The table
        // owns its dirty set: every path that changes a record writes
        // m_PendingDirtySlots, and EndExtraction takes the coalesced ranges.
        template<typename Key, typename Input, typename Record>
        struct RecordTable
        {
            struct Slot
            {
                Key m_Key{};
                Input m_Input{};
                u32 m_Generation = 1;
                bool m_Live = false;
            };

            u32 m_Binding = 0;
            std::map<Key, Input> m_Staged;
            std::map<Key, GPUSceneHandle> m_Handles;
            std::vector<Slot> m_Slots;
            std::vector<Record> m_Records;
            std::set<u32> m_FreeSlots;
            std::vector<RetiredSlot> m_RetiredSlots;
            std::set<u32> m_PendingDirtySlots;
            Ref<StorageBuffer> m_Buffer;
            u32 m_BufferCapacity = 0;

            // Frame start: retired slots whose buffered frames completed return
            // to the free set, and the previous frame's staging is dropped.
            void BeginFrame(u64 frameNumber)
            {
                std::erase_if(m_RetiredSlots,
                              [this, frameNumber](const RetiredSlot& retired)
                              {
                                  if (retired.m_ReadyFrame > frameNumber)
                                  {
                                      return false;
                                  }
                                  m_FreeSlots.insert(retired.m_Index);
                                  return true;
                              });
                m_Staged.clear();
            }

            // Kills a slot: tombstone, generation advance, and frame-safe
            // retirement while the generation is representable. A slot whose
            // generation is exhausted is never reused, and its tombstone keeps
            // the exhausted generation: a consumer must test the record's
            // Active flag as well as its generation.
            void Kill(u32 index, u64 frameNumber)
            {
                auto& slot = m_Slots[index];
                slot.m_Live = false;
                if (AdvanceGeneration(slot.m_Generation))
                {
                    m_RetiredSlots.push_back(RetiredSlot{
                        .m_Index = index,
                        .m_ReadyFrame = GPUSceneAllocationPolicy::RetirementReadyFrame(frameNumber),
                    });
                }
                MakeTombstone(m_Records[index], index, slot.m_Generation);
                m_PendingDirtySlots.insert(index);
            }

            void RemoveUnstaged(u64 frameNumber)
            {
                for (auto it = m_Handles.begin(); it != m_Handles.end();)
                {
                    if (m_Staged.contains(it->first))
                    {
                        ++it;
                        continue;
                    }
                    Kill(it->second.m_Index, frameNumber);
                    it = m_Handles.erase(it);
                }
            }

            // Reuse the key's slot, else the lowest free slot, else append.
            [[nodiscard]] u32 Acquire(const Key& key)
            {
                if (const auto existing = m_Handles.find(key); existing != m_Handles.end())
                {
                    return existing->second.m_Index;
                }
                if (!m_FreeSlots.empty())
                {
                    const auto freeIt = m_FreeSlots.begin();
                    const u32 index = *freeIt;
                    m_FreeSlots.erase(freeIt);
                    return index;
                }
                const auto index = static_cast<u32>(m_Slots.size());
                m_Slots.emplace_back();
                m_Records.emplace_back();
                return index;
            }

            // The in-place incompatible edit: the key keeps its slot and the
            // slot's generation advances, so every handle and every dependent
            // record (an instance's MaterialGeneration) goes stale at once.
            // False when the generation is exhausted; the caller then kills the
            // slot and re-acquires a fresh one for the key.
            [[nodiscard]] bool AdvanceLiveGeneration(u32 index, const Key& key)
            {
                auto& slot = m_Slots[index];
                if (!AdvanceGeneration(slot.m_Generation))
                {
                    return false;
                }
                m_Handles[key] = GPUSceneHandle{ index, slot.m_Generation };
                return true;
            }

            void Commit(u32 index, const Key& key, const Input& input, const Record& record)
            {
                auto& slot = m_Slots[index];
                const bool wasLive = slot.m_Live;
                if (!wasLive || !RecordsEqual(m_Records[index], record))
                {
                    m_PendingDirtySlots.insert(index);
                }
                slot.m_Key = key;
                slot.m_Input = input;
                slot.m_Live = true;
                m_Records[index] = record;
                // A live slot's handle is already current (AdvanceLiveGeneration
                // rewrote it if the generation moved); only a fresh slot pays
                // the map insert, which keeps the per-record frame cost at one
                // lookup for the steady state.
                if (!wasLive)
                {
                    m_Handles.emplace(key, GPUSceneHandle{ index, slot.m_Generation });
                }
            }

            // Commit every staged key in key order. `encode(key, input, index,
            // slot)` builds the record for the slot the key acquired; `compatible`
            // decides whether a live slot keeps its generation. A staged edit
            // the rule rejects advances the generation in place before the
            // record is stored.
            template<typename Encode, typename Compatible>
            void CommitStaged(u64 frameNumber, Encode&& encode, Compatible&& compatible)
            {
                for (const auto& [key, input] : m_Staged)
                {
                    u32 index = Acquire(key);
                    Record record = encode(key, input, index, m_Slots[index]);
                    if (m_Slots[index].m_Live && !compatible(m_Records[index], record))
                    {
                        if (AdvanceLiveGeneration(index, key))
                        {
                            record.Generation = m_Slots[index].m_Generation;
                        }
                        else
                        {
                            // Generation exhausted: the slot dies for good and
                            // the key moves to a fresh slot in this same commit.
                            Kill(index, frameNumber);
                            m_Handles.erase(key);
                            index = Acquire(key);
                            record = encode(key, input, index, m_Slots[index]);
                        }
                    }
                    Commit(index, key, input, record);
                }
            }

            [[nodiscard]] std::vector<GPUSceneDirtyRange> TakeDirtyRanges()
            {
                return CoalesceDirtyRanges(std::exchange(m_PendingDirtySlots, {}));
            }

            [[nodiscard]] std::vector<GPUSceneDirtyRange> PendingDirtyRanges() const
            {
                return CoalesceDirtyRanges(m_PendingDirtySlots);
            }

            [[nodiscard]] GPUSceneHandle Find(const Key& key) const
            {
                const auto found = m_Handles.find(key);
                return found != m_Handles.end() ? found->second : GPUSceneHandle{};
            }

            [[nodiscard]] bool IsLive(GPUSceneHandle handle) const
            {
                if (!handle.IsValid() || handle.m_Index >= m_Slots.size())
                {
                    return false;
                }
                const auto& slot = m_Slots[handle.m_Index];
                return slot.m_Live && slot.m_Generation == handle.m_Generation;
            }

            [[nodiscard]] const Record* Get(GPUSceneHandle handle) const
            {
                return IsLive(handle) ? &m_Records[handle.m_Index] : nullptr;
            }

            void InitializeGPU(u32 capacity)
            {
                m_BufferCapacity = std::max(capacity, 1u);
                m_Buffer = StorageBuffer::Create(BytesForRecords<Record>(m_BufferCapacity), m_Binding,
                                                 StorageBufferUsage::DynamicDraw);
                m_Buffer->Unbind();

                // Fresh storage has undefined contents, including after a
                // renderer restart. Seed every allocated CPU slot, including
                // free tombstones, and keep that intent pending if extraction
                // happens before Upload().
                const auto recordCount = static_cast<u32>(m_Records.size());
                for (u32 index = 0; index < recordCount; ++index)
                {
                    m_PendingDirtySlots.insert(index);
                }
            }

            // Growth resizes the buffer in place (the RHI identity survives) and
            // uploads every record. Resize binds and unbinds the aliased slot,
            // which is why every consumer of these slots binds per pass.
            [[nodiscard]] u64 Upload(const std::vector<GPUSceneDirtyRange>& ranges, u32& growthEvents)
            {
                std::vector<GPUSceneDirtyRange> grown;
                const std::vector<GPUSceneDirtyRange>* toUpload = &ranges;
                const auto required = static_cast<u32>(m_Records.size());
                if (required > m_BufferCapacity)
                {
                    m_BufferCapacity = GPUSceneAllocationPolicy::GrowCapacity(m_BufferCapacity, required);
                    m_Buffer->Resize(BytesForRecords<Record>(m_BufferCapacity));
                    m_Buffer->Unbind();
                    grown = FullRange(required);
                    toUpload = &grown;
                    ++growthEvents;
                }

                u64 uploadBytes = 0;
                for (const GPUSceneDirtyRange& range : *toUpload)
                {
                    OLO_CORE_ASSERT(static_cast<u64>(range.m_FirstIndex) + range.m_Count <= m_Records.size(),
                                    "GPUScene dirty range exceeds allocated slots");
                    const u32 sizeBytes = BytesForRecords<Record>(range.m_Count);
                    const u32 offsetBytes = BytesForRecords<Record>(range.m_FirstIndex);
                    m_Buffer->SetData(m_Records.data() + range.m_FirstIndex, sizeBytes, offsetBytes);
                    uploadBytes += sizeBytes;
                }
                return uploadBytes;
            }

            void Shutdown()
            {
                if (m_Buffer)
                {
                    m_Buffer->Unbind();
                }
                m_Buffer.Reset();
                m_BufferCapacity = 0;
            }

            void ResetLive(u64 frameNumber)
            {
                for (const auto& [key, handle] : m_Handles)
                {
                    (void)key;
                    Kill(handle.m_Index, frameNumber);
                }
                m_Handles.clear();
                m_Staged.clear();
            }

            [[nodiscard]] RHI::ResourceHandle BufferHandle() const
            {
                return m_Buffer ? m_Buffer->GetRHIHandle() : RHI::ResourceHandle{};
            }

            // Registry counters only; Upload() fills m_UploadBytes afterwards.
            [[nodiscard]] GPUSceneKindStats Stats() const
            {
                return GPUSceneKindStats{
                    .m_Live = static_cast<u32>(m_Handles.size()),
                    .m_SlotCount = static_cast<u32>(m_Slots.size()),
                    .m_BufferCapacity = m_BufferCapacity,
                    .m_FreeSlots = static_cast<u32>(m_FreeSlots.size()),
                    .m_RetiredSlots = static_cast<u32>(m_RetiredSlots.size()),
                };
            }
        };
    } // namespace

    struct GPUScene::Impl
    {
        u64 m_OwnerToken = 0;
        u64 m_FrameNumber = 0;
        bool m_HasOwner = false;
        glm::vec3 m_RenderOrigin{ 0.0f };
        bool m_Extracting = false;
        std::chrono::steady_clock::time_point m_ExtractionStart;

        RecordTable<GPUSceneGeometryKey, GPUSceneGeometryInput, GPUSceneGeometry> m_Geometries;
        RecordTable<GPUSceneInstanceKey, GPUSceneInstanceInput, GPUSceneInstance> m_Instances;
        RecordTable<GPUSceneMaterialKey, GPUSceneMaterialInput, GPUSceneMaterial> m_Materials;
        RecordTable<GPUSceneLightKey, GPUSceneLightInput, GPUSceneLight> m_Lights;
        RecordTable<GPUSceneEnvironmentKey, GPUSceneEnvironmentInput, GPUSceneEnvironment> m_Environments;
        std::array<u32, GPUSceneUnsupportedCategoryCount> m_UnsupportedCounts{};

        bool m_UploadPending = false;
        GPUSceneFrameUpdate m_LastFrameUpdate;

        Impl()
        {
            m_Geometries.m_Binding = GPUSceneBindingLayout::Geometries;
            m_Instances.m_Binding = GPUSceneBindingLayout::Instances;
            m_Materials.m_Binding = GPUSceneBindingLayout::Materials;
            m_Lights.m_Binding = GPUSceneBindingLayout::Lights;
            m_Environments.m_Binding = GPUSceneBindingLayout::Environments;
        }

        // Every per-kind fan-out goes through here so a sixth kind cannot be
        // missed by one of them. Only EndExtraction's commit order (materials
        // before the instances that reference them) is spelled out by hand.
        template<typename Function>
        void ForEachTable(Function&& function)
        {
            function(m_Geometries);
            function(m_Instances);
            function(m_Materials);
            function(m_Lights);
            function(m_Environments);
        }

        template<typename Function>
        void ForEachTable(Function&& function) const
        {
            function(m_Geometries);
            function(m_Instances);
            function(m_Materials);
            function(m_Lights);
            function(m_Environments);
        }

        [[nodiscard]] bool HasGPUResources() const
        {
            bool all = true;
            ForEachTable([&all](const auto& table)
                         { all = all && table.m_Buffer; });
            return all;
        }

        // Registry counters. Upload bytes, timing and the unsupported counts
        // are filled by the caller that knows them.
        [[nodiscard]] GPUSceneFrameStats BuildStats() const
        {
            GPUSceneFrameStats stats{};
            stats.m_Instances = m_Instances.Stats();
            stats.m_Geometries = m_Geometries.Stats();
            stats.m_Materials = m_Materials.Stats();
            stats.m_Lights = m_Lights.Stats();
            stats.m_Environments = m_Environments.Stats();
            return stats;
        }

        void RefreshCapacityStats()
        {
            auto& stats = m_LastFrameUpdate.m_Stats;
            stats.m_Instances.m_BufferCapacity = m_Instances.m_BufferCapacity;
            stats.m_Geometries.m_BufferCapacity = m_Geometries.m_BufferCapacity;
            stats.m_Materials.m_BufferCapacity = m_Materials.m_BufferCapacity;
            stats.m_Lights.m_BufferCapacity = m_Lights.m_BufferCapacity;
            stats.m_Environments.m_BufferCapacity = m_Environments.m_BufferCapacity;
        }

        // Outside a frame (InitializeGPU, Reset) the pending dirty slots are
        // published as the last frame update without being consumed; the next
        // EndExtraction takes them together with that frame's edits.
        void PublishPendingDirtyRanges()
        {
            m_LastFrameUpdate.m_InstanceDirtyRanges = m_Instances.PendingDirtyRanges();
            m_LastFrameUpdate.m_GeometryDirtyRanges = m_Geometries.PendingDirtyRanges();
            m_LastFrameUpdate.m_MaterialDirtyRanges = m_Materials.PendingDirtyRanges();
            m_LastFrameUpdate.m_LightDirtyRanges = m_Lights.PendingDirtyRanges();
            m_LastFrameUpdate.m_EnvironmentDirtyRanges = m_Environments.PendingDirtyRanges();
        }
    };

    GPUScene::GPUScene()
        : m_Impl(std::make_unique<Impl>())
    {
    }

    GPUScene::~GPUScene() = default;
    GPUScene::GPUScene(GPUScene&&) noexcept = default;
    auto GPUScene::operator=(GPUScene&&) noexcept -> GPUScene& = default;

    void GPUScene::BeginExtraction(u64 ownerToken, const glm::vec3& renderOrigin)
    {
        auto& impl = *m_Impl;
        OLO_CORE_ASSERT(!impl.m_Extracting, "GPUScene::BeginExtraction called twice before EndExtraction");
        if (impl.m_HasOwner && impl.m_OwnerToken != ownerToken)
        {
            Reset();
        }

        ++impl.m_FrameNumber;
        impl.ForEachTable([frameNumber = impl.m_FrameNumber](auto& table)
                          { table.BeginFrame(frameNumber); });

        impl.m_OwnerToken = ownerToken;
        impl.m_HasOwner = true;
        impl.m_RenderOrigin = renderOrigin;
        impl.m_UnsupportedCounts.fill(0);
        impl.m_ExtractionStart = std::chrono::steady_clock::now();
        impl.m_Extracting = true;
    }

    void GPUScene::ExtractGeometry(const GPUSceneGeometryKey& key, const GPUSceneGeometryInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractGeometry requires BeginExtraction");
        m_Impl->m_Geometries.m_Staged.insert_or_assign(key, input);
    }

    void GPUScene::ExtractInstance(const GPUSceneInstanceKey& key, const GPUSceneInstanceInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractInstance requires BeginExtraction");
        m_Impl->m_Instances.m_Staged.insert_or_assign(key, input);
    }

    void GPUScene::ExtractMaterial(const GPUSceneMaterialKey& key, const GPUSceneMaterialInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractMaterial requires BeginExtraction");
        m_Impl->m_Materials.m_Staged.insert_or_assign(key, input);
    }

    void GPUScene::ExtractLight(const GPUSceneLightKey& key, const GPUSceneLightInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractLight requires BeginExtraction");
        m_Impl->m_Lights.m_Staged.insert_or_assign(key, input);
    }

    void GPUScene::ExtractEnvironment(const GPUSceneEnvironmentKey& key, const GPUSceneEnvironmentInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractEnvironment requires BeginExtraction");
        m_Impl->m_Environments.m_Staged.insert_or_assign(key, input);
    }

    bool GPUScene::IsMaterialStaged(const GPUSceneMaterialKey& key) const
    {
        return m_Impl->m_Extracting && m_Impl->m_Materials.m_Staged.contains(key);
    }

    void GPUScene::ReportUnsupported(GPUSceneUnsupportedCategory category, u32 count)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ReportUnsupported requires BeginExtraction");
        const auto index = static_cast<sizet>(category);
        OLO_CORE_ASSERT(index < m_Impl->m_UnsupportedCounts.size(), "Invalid GPUScene unsupported category");
        m_Impl->m_UnsupportedCounts[index] += count;
    }

    GPUSceneFrameUpdate GPUScene::EndExtraction()
    {
        auto& impl = *m_Impl;
        OLO_CORE_ASSERT(impl.m_Extracting, "GPUScene::EndExtraction requires BeginExtraction");

        const u64 frameNumber = impl.m_FrameNumber;
        impl.ForEachTable([frameNumber](auto& table)
                          { table.RemoveUnstaged(frameNumber); });

        impl.m_Geometries.CommitStaged(
            frameNumber,
            [](const GPUSceneGeometryKey&, const GPUSceneGeometryInput& input, u32, const auto& slot)
            {
                GPUSceneGeometry record{};
                record.VertexBufferIndex = input.m_VertexBuffer.Index;
                record.VertexBufferGeneration = input.m_VertexBuffer.Generation;
                record.IndexBufferIndex = input.m_IndexBuffer.Index;
                record.IndexBufferGeneration = input.m_IndexBuffer.Generation;
                record.VertexAddress = input.m_VertexAddress;
                record.IndexAddress = input.m_IndexAddress;
                record.VertexFormat = input.m_VertexFormat;
                record.IndexFormat = input.m_IndexFormat;
                record.FirstIndex = input.m_FirstIndex;
                record.IndexCount = input.m_IndexCount;
                record.BaseVertex = input.m_BaseVertex;
                record.VertexCount = input.m_VertexCount;
                record.Generation = slot.m_Generation;
                record.Flags = input.m_Flags | GPUSceneGeometryFlagActive;
                return record;
            },
            AlwaysCompatible{});

        // Materials and environments before instances: an instance resolves
        // its material slot and generation from this frame's commit.
        impl.m_Materials.CommitStaged(
            frameNumber,
            [](const GPUSceneMaterialKey&, const GPUSceneMaterialInput& input, u32 index, const auto& slot)
            { return EncodeGPUSceneMaterial(input, index, slot.m_Generation); },
            [](const GPUSceneMaterial& previous, const GPUSceneMaterial& next)
            { return IsCompatibleGPUSceneMaterialEdit(previous, next); });

        impl.m_Lights.CommitStaged(
            frameNumber,
            [origin = impl.m_RenderOrigin](const GPUSceneLightKey&, const GPUSceneLightInput& input, u32 index,
                                           const auto& slot)
            { return EncodeGPUSceneLight(input, origin, index, slot.m_Generation); },
            AlwaysCompatible{});

        impl.m_Environments.CommitStaged(
            frameNumber,
            [](const GPUSceneEnvironmentKey&, const GPUSceneEnvironmentInput& input, u32 index, const auto& slot)
            { return EncodeGPUSceneEnvironment(input, index, slot.m_Generation); },
            [](const GPUSceneEnvironment& previous, const GPUSceneEnvironment& next)
            { return IsCompatibleGPUSceneEnvironmentEdit(previous, next); });

        impl.m_Instances.CommitStaged(
            frameNumber,
            [&impl](const GPUSceneInstanceKey& key, const GPUSceneInstanceInput& input, u32 index, const auto& slot)
            {
                // A live slot's stored input is last frame's transform; a fresh
                // or reused slot has no history and starts static.
                const glm::mat4& previousWorldTransform =
                    slot.m_Live ? slot.m_Input.m_WorldTransform : input.m_WorldTransform;
                const GPUSceneHandle geometryHandle = impl.m_Geometries.Find(key.m_Geometry);
                const GPUSceneHandle materialHandle = impl.m_Materials.Find(input.m_Material);

                GPUSceneInstance record{};
                record.CurrentTransform = EncodeTransform(input.m_WorldTransform, impl.m_RenderOrigin);
                record.PreviousTransform = EncodeTransform(previousWorldTransform, impl.m_RenderOrigin);
                record.GeometryIndex = geometryHandle.m_Index;
                record.GeometryGeneration = geometryHandle.m_Generation;
                record.MaterialIndex = materialHandle.m_Index;
                record.MaterialGeneration = materialHandle.m_Generation;
                record.StableIndex = index;
                record.VisibilityMask = input.m_VisibilityMask;
                record.Flags = input.m_Flags | GPUSceneInstanceFlagActive;
                record.Generation = slot.m_Generation;
                return record;
            },
            AlwaysCompatible{});

        impl.m_Extracting = false;
        const auto extractionEnd = std::chrono::steady_clock::now();
        const f64 extractionTimeMs =
            std::chrono::duration<f64, std::milli>(extractionEnd - impl.m_ExtractionStart).count();
        const u32 unsupportedTotal =
            std::accumulate(impl.m_UnsupportedCounts.begin(), impl.m_UnsupportedCounts.end(), 0u);

        GPUSceneFrameStats stats = impl.BuildStats();
        stats.m_UnsupportedTotal = unsupportedTotal;
        stats.m_ExtractionTimeMs = extractionTimeMs;
        stats.m_UnsupportedCounts = impl.m_UnsupportedCounts;
        impl.m_LastFrameUpdate = GPUSceneFrameUpdate{
            .m_InstanceDirtyRanges = impl.m_Instances.TakeDirtyRanges(),
            .m_GeometryDirtyRanges = impl.m_Geometries.TakeDirtyRanges(),
            .m_MaterialDirtyRanges = impl.m_Materials.TakeDirtyRanges(),
            .m_LightDirtyRanges = impl.m_Lights.TakeDirtyRanges(),
            .m_EnvironmentDirtyRanges = impl.m_Environments.TakeDirtyRanges(),
            .m_Stats = stats,
        };
        impl.m_UploadPending = true;
        return impl.m_LastFrameUpdate;
    }

    void GPUScene::InitializeGPU(const GPUSceneCapacities& capacities)
    {
        if (HasGPUResources())
        {
            return;
        }

        m_Impl->m_Instances.InitializeGPU(capacities.m_Instances);
        m_Impl->m_Geometries.InitializeGPU(capacities.m_Geometries);
        m_Impl->m_Materials.InitializeGPU(capacities.m_Materials);
        m_Impl->m_Lights.InitializeGPU(capacities.m_Lights);
        m_Impl->m_Environments.InitializeGPU(capacities.m_Environments);

        m_Impl->PublishPendingDirtyRanges();
        m_Impl->RefreshCapacityStats();
        m_Impl->m_UploadPending = true;
    }

    void GPUScene::Upload()
    {
        auto& impl = *m_Impl;
        OLO_CORE_ASSERT(HasGPUResources(), "GPUScene::Upload requires InitializeGPU");
        OLO_CORE_ASSERT(!impl.m_Extracting, "GPUScene::Upload cannot run during extraction");
        if (!HasGPUResources() || !impl.m_UploadPending)
        {
            return;
        }

        auto& update = impl.m_LastFrameUpdate;
        auto& stats = update.m_Stats;
        u32 growthEvents = 0;
        stats.m_Geometries.m_UploadBytes = impl.m_Geometries.Upload(update.m_GeometryDirtyRanges, growthEvents);
        stats.m_Materials.m_UploadBytes = impl.m_Materials.Upload(update.m_MaterialDirtyRanges, growthEvents);
        stats.m_Lights.m_UploadBytes = impl.m_Lights.Upload(update.m_LightDirtyRanges, growthEvents);
        stats.m_Environments.m_UploadBytes =
            impl.m_Environments.Upload(update.m_EnvironmentDirtyRanges, growthEvents);
        stats.m_Instances.m_UploadBytes = impl.m_Instances.Upload(update.m_InstanceDirtyRanges, growthEvents);
        stats.m_UploadBytes = stats.m_Instances.m_UploadBytes + stats.m_Geometries.m_UploadBytes +
                              stats.m_Materials.m_UploadBytes + stats.m_Lights.m_UploadBytes +
                              stats.m_Environments.m_UploadBytes;
        stats.m_BufferGrowthEvents = growthEvents;
        impl.RefreshCapacityStats();
        impl.m_UploadPending = false;
    }

    void GPUScene::Bind() const
    {
        OLO_CORE_ASSERT(HasGPUResources(), "GPUScene::Bind requires InitializeGPU");
        if (HasGPUResources())
        {
            m_Impl->ForEachTable([](const auto& table)
                                 { table.m_Buffer->Bind(); });
        }
    }

    void GPUScene::Shutdown()
    {
        Reset();
        m_Impl->ForEachTable([](auto& table)
                             { table.Shutdown(); });
        m_Impl->RefreshCapacityStats();
        m_Impl->m_UploadPending = false;
    }

    bool GPUScene::HasGPUResources() const
    {
        return m_Impl->HasGPUResources();
    }

    RHI::ResourceHandle GPUScene::GetInstanceBufferHandle() const
    {
        return m_Impl->m_Instances.BufferHandle();
    }

    RHI::ResourceHandle GPUScene::GetGeometryBufferHandle() const
    {
        return m_Impl->m_Geometries.BufferHandle();
    }

    RHI::ResourceHandle GPUScene::GetMaterialBufferHandle() const
    {
        return m_Impl->m_Materials.BufferHandle();
    }

    RHI::ResourceHandle GPUScene::GetLightBufferHandle() const
    {
        return m_Impl->m_Lights.BufferHandle();
    }

    RHI::ResourceHandle GPUScene::GetEnvironmentBufferHandle() const
    {
        return m_Impl->m_Environments.BufferHandle();
    }

    GPUSceneHandle GPUScene::FindGeometry(const GPUSceneGeometryKey& key) const
    {
        return m_Impl->m_Geometries.Find(key);
    }

    GPUSceneHandle GPUScene::FindInstance(const GPUSceneInstanceKey& key) const
    {
        return m_Impl->m_Instances.Find(key);
    }

    GPUSceneHandle GPUScene::FindMaterial(const GPUSceneMaterialKey& key) const
    {
        return m_Impl->m_Materials.Find(key);
    }

    GPUSceneHandle GPUScene::FindLight(const GPUSceneLightKey& key) const
    {
        return m_Impl->m_Lights.Find(key);
    }

    GPUSceneHandle GPUScene::FindEnvironment(const GPUSceneEnvironmentKey& key) const
    {
        return m_Impl->m_Environments.Find(key);
    }

    bool GPUScene::IsGeometryHandleLive(GPUSceneHandle handle) const
    {
        return m_Impl->m_Geometries.IsLive(handle);
    }

    bool GPUScene::IsInstanceHandleLive(GPUSceneHandle handle) const
    {
        return m_Impl->m_Instances.IsLive(handle);
    }

    bool GPUScene::IsMaterialHandleLive(GPUSceneHandle handle) const
    {
        return m_Impl->m_Materials.IsLive(handle);
    }

    bool GPUScene::IsLightHandleLive(GPUSceneHandle handle) const
    {
        return m_Impl->m_Lights.IsLive(handle);
    }

    bool GPUScene::IsEnvironmentHandleLive(GPUSceneHandle handle) const
    {
        return m_Impl->m_Environments.IsLive(handle);
    }

    const GPUSceneGeometry* GPUScene::GetGeometryRecord(GPUSceneHandle handle) const
    {
        return m_Impl->m_Geometries.Get(handle);
    }

    const GPUSceneInstance* GPUScene::GetInstanceRecord(GPUSceneHandle handle) const
    {
        return m_Impl->m_Instances.Get(handle);
    }

    const GPUSceneMaterial* GPUScene::GetMaterialRecord(GPUSceneHandle handle) const
    {
        return m_Impl->m_Materials.Get(handle);
    }

    const GPUSceneLight* GPUScene::GetLightRecord(GPUSceneHandle handle) const
    {
        return m_Impl->m_Lights.Get(handle);
    }

    const GPUSceneEnvironment* GPUScene::GetEnvironmentRecord(GPUSceneHandle handle) const
    {
        return m_Impl->m_Environments.Get(handle);
    }

    const GPUSceneFrameUpdate& GPUScene::GetLastFrameUpdate() const
    {
        return m_Impl->m_LastFrameUpdate;
    }

    void GPUScene::Reset()
    {
        auto& impl = *m_Impl;
        impl.ForEachTable([frameNumber = impl.m_FrameNumber](auto& table)
                          { table.ResetLive(frameNumber); });

        impl.m_OwnerToken = 0;
        impl.m_HasOwner = false;
        impl.m_Extracting = false;
        impl.m_UnsupportedCounts.fill(0);
        impl.m_LastFrameUpdate = GPUSceneFrameUpdate{};
        impl.m_LastFrameUpdate.m_Stats = impl.BuildStats();
        impl.PublishPendingDirtyRanges();
        impl.m_UploadPending = true;
    }
} // namespace OloEngine
