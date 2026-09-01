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
    } // namespace

    struct GPUScene::Impl
    {
        struct GeometrySlot
        {
            GPUSceneGeometryKey m_Key;
            GPUSceneGeometryInput m_Input;
            u32 m_Generation = 1;
            bool m_Live = false;
        };

        struct InstanceSlot
        {
            GPUSceneInstanceKey m_Key;
            GPUSceneInstanceInput m_Input;
            u32 m_Generation = 1;
            bool m_Live = false;
        };

        struct RetiredSlot
        {
            u32 m_Index = 0;
            u64 m_ReadyFrame = 0;
        };

        u64 m_OwnerToken = 0;
        u64 m_FrameNumber = 0;
        bool m_HasOwner = false;
        glm::vec3 m_RenderOrigin{ 0.0f };
        bool m_Extracting = false;
        std::chrono::steady_clock::time_point m_ExtractionStart;

        std::map<GPUSceneGeometryKey, GPUSceneGeometryInput> m_StagedGeometries;
        std::map<GPUSceneInstanceKey, GPUSceneInstanceInput> m_StagedInstances;

        std::map<GPUSceneGeometryKey, GPUSceneHandle> m_GeometryHandles;
        std::map<GPUSceneInstanceKey, GPUSceneHandle> m_InstanceHandles;
        std::vector<GeometrySlot> m_GeometrySlots;
        std::vector<InstanceSlot> m_InstanceSlots;
        std::vector<GPUSceneGeometry> m_GeometryRecords;
        std::vector<GPUSceneInstance> m_InstanceRecords;
        std::set<u32> m_FreeGeometrySlots;
        std::set<u32> m_FreeInstanceSlots;
        std::vector<RetiredSlot> m_RetiredGeometrySlots;
        std::vector<RetiredSlot> m_RetiredInstanceSlots;
        std::set<u32> m_PendingDirtyGeometrySlots;
        std::set<u32> m_PendingDirtyInstanceSlots;
        std::array<u32, GPUSceneUnsupportedCategoryCount> m_UnsupportedCounts{};

        Ref<StorageBuffer> m_GeometryBuffer;
        Ref<StorageBuffer> m_InstanceBuffer;
        u32 m_GeometryBufferCapacity = 0;
        u32 m_InstanceBufferCapacity = 0;
        bool m_UploadPending = false;
        GPUSceneFrameUpdate m_LastFrameUpdate;
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
        OLO_CORE_ASSERT(!m_Impl->m_Extracting,
                        "GPUScene::BeginExtraction called twice before EndExtraction");
        if (m_Impl->m_HasOwner && m_Impl->m_OwnerToken != ownerToken)
        {
            Reset();
        }

        ++m_Impl->m_FrameNumber;
        const auto releaseRetired = [frameNumber = m_Impl->m_FrameNumber](auto& retiredSlots,
                                                                          auto& freeSlots)
        {
            std::erase_if(retiredSlots,
                          [frameNumber, &freeSlots](const Impl::RetiredSlot& retired)
                          {
                              if (retired.m_ReadyFrame > frameNumber)
                              {
                                  return false;
                              }
                              freeSlots.insert(retired.m_Index);
                              return true;
                          });
        };
        releaseRetired(m_Impl->m_RetiredGeometrySlots, m_Impl->m_FreeGeometrySlots);
        releaseRetired(m_Impl->m_RetiredInstanceSlots, m_Impl->m_FreeInstanceSlots);

        m_Impl->m_OwnerToken = ownerToken;
        m_Impl->m_HasOwner = true;
        m_Impl->m_RenderOrigin = renderOrigin;
        m_Impl->m_StagedGeometries.clear();
        m_Impl->m_StagedInstances.clear();
        m_Impl->m_UnsupportedCounts.fill(0);
        m_Impl->m_ExtractionStart = std::chrono::steady_clock::now();
        m_Impl->m_Extracting = true;
    }

    void GPUScene::ExtractGeometry(const GPUSceneGeometryKey& key, const GPUSceneGeometryInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractGeometry requires BeginExtraction");
        m_Impl->m_StagedGeometries.insert_or_assign(key, input);
    }

    void GPUScene::ExtractInstance(const GPUSceneInstanceKey& key, const GPUSceneInstanceInput& input)
    {
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::ExtractInstance requires BeginExtraction");
        m_Impl->m_StagedInstances.insert_or_assign(key, input);
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
        OLO_CORE_ASSERT(m_Impl->m_Extracting, "GPUScene::EndExtraction requires BeginExtraction");

        std::set<u32> dirtyGeometrySlots = std::move(m_Impl->m_PendingDirtyGeometrySlots);
        std::set<u32> dirtyInstanceSlots = std::move(m_Impl->m_PendingDirtyInstanceSlots);
        m_Impl->m_PendingDirtyGeometrySlots.clear();
        m_Impl->m_PendingDirtyInstanceSlots.clear();

        for (auto it = m_Impl->m_GeometryHandles.begin(); it != m_Impl->m_GeometryHandles.end();)
        {
            if (m_Impl->m_StagedGeometries.contains(it->first))
            {
                ++it;
                continue;
            }

            auto& slot = m_Impl->m_GeometrySlots[it->second.m_Index];
            slot.m_Live = false;
            if (AdvanceGeneration(slot.m_Generation))
            {
                m_Impl->m_RetiredGeometrySlots.push_back(Impl::RetiredSlot{
                    .m_Index = it->second.m_Index,
                    .m_ReadyFrame = GPUSceneAllocationPolicy::RetirementReadyFrame(m_Impl->m_FrameNumber),
                });
            }
            auto& tombstone = m_Impl->m_GeometryRecords[it->second.m_Index];
            tombstone = GPUSceneGeometry{};
            tombstone.Generation = slot.m_Generation;
            dirtyGeometrySlots.insert(it->second.m_Index);
            it = m_Impl->m_GeometryHandles.erase(it);
        }

        for (const auto& [key, input] : m_Impl->m_StagedGeometries)
        {
            u32 index = 0;
            if (const auto existing = m_Impl->m_GeometryHandles.find(key);
                existing != m_Impl->m_GeometryHandles.end())
            {
                index = existing->second.m_Index;
            }
            else if (!m_Impl->m_FreeGeometrySlots.empty())
            {
                const auto freeIt = m_Impl->m_FreeGeometrySlots.begin();
                index = *freeIt;
                m_Impl->m_FreeGeometrySlots.erase(freeIt);
            }
            else
            {
                index = static_cast<u32>(m_Impl->m_GeometrySlots.size());
                m_Impl->m_GeometrySlots.emplace_back();
                m_Impl->m_GeometryRecords.emplace_back();
            }

            auto& slot = m_Impl->m_GeometrySlots[index];
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

            if (!slot.m_Live || !RecordsEqual(m_Impl->m_GeometryRecords[index], record))
            {
                dirtyGeometrySlots.insert(index);
            }
            slot.m_Key = key;
            slot.m_Input = input;
            m_Impl->m_GeometryRecords[index] = record;
            const bool wasLive = slot.m_Live;
            slot.m_Live = true;
            if (!wasLive)
            {
                m_Impl->m_GeometryHandles.emplace(key, GPUSceneHandle{ index, slot.m_Generation });
            }
        }

        for (auto it = m_Impl->m_InstanceHandles.begin(); it != m_Impl->m_InstanceHandles.end();)
        {
            if (m_Impl->m_StagedInstances.contains(it->first))
            {
                ++it;
                continue;
            }

            auto& slot = m_Impl->m_InstanceSlots[it->second.m_Index];
            slot.m_Live = false;
            if (AdvanceGeneration(slot.m_Generation))
            {
                m_Impl->m_RetiredInstanceSlots.push_back(Impl::RetiredSlot{
                    .m_Index = it->second.m_Index,
                    .m_ReadyFrame = GPUSceneAllocationPolicy::RetirementReadyFrame(m_Impl->m_FrameNumber),
                });
            }
            auto& tombstone = m_Impl->m_InstanceRecords[it->second.m_Index];
            tombstone = GPUSceneInstance{};
            tombstone.StableIndex = it->second.m_Index;
            tombstone.Generation = slot.m_Generation;
            dirtyInstanceSlots.insert(it->second.m_Index);
            it = m_Impl->m_InstanceHandles.erase(it);
        }

        for (const auto& [key, input] : m_Impl->m_StagedInstances)
        {
            u32 index = 0;
            bool wasLive = false;
            glm::mat4 previousWorldTransform = input.m_WorldTransform;
            if (const auto existing = m_Impl->m_InstanceHandles.find(key);
                existing != m_Impl->m_InstanceHandles.end())
            {
                index = existing->second.m_Index;
                wasLive = true;
                previousWorldTransform = m_Impl->m_InstanceSlots[index].m_Input.m_WorldTransform;
            }
            else if (!m_Impl->m_FreeInstanceSlots.empty())
            {
                const auto freeIt = m_Impl->m_FreeInstanceSlots.begin();
                index = *freeIt;
                m_Impl->m_FreeInstanceSlots.erase(freeIt);
            }
            else
            {
                index = static_cast<u32>(m_Impl->m_InstanceSlots.size());
                m_Impl->m_InstanceSlots.emplace_back();
                m_Impl->m_InstanceRecords.emplace_back();
            }

            auto& slot = m_Impl->m_InstanceSlots[index];
            const auto geometry = m_Impl->m_GeometryHandles.find(key.m_Geometry);
            const GPUSceneHandle geometryHandle = geometry != m_Impl->m_GeometryHandles.end()
                                                      ? geometry->second
                                                      : GPUSceneHandle{};

            GPUSceneInstance record{};
            record.CurrentTransform = EncodeTransform(input.m_WorldTransform, m_Impl->m_RenderOrigin);
            record.PreviousTransform = EncodeTransform(previousWorldTransform, m_Impl->m_RenderOrigin);
            record.GeometryIndex = geometryHandle.m_Index;
            record.GeometryGeneration = geometryHandle.m_Generation;
            record.MaterialIndex = input.m_MaterialIndex;
            record.StableIndex = index;
            record.VisibilityMask = input.m_VisibilityMask;
            record.Flags = input.m_Flags | GPUSceneInstanceFlagActive;
            record.Generation = slot.m_Generation;

            if (!slot.m_Live || !RecordsEqual(m_Impl->m_InstanceRecords[index], record))
            {
                dirtyInstanceSlots.insert(index);
            }
            slot.m_Key = key;
            slot.m_Input = input;
            m_Impl->m_InstanceRecords[index] = record;
            slot.m_Live = true;
            if (!wasLive)
            {
                m_Impl->m_InstanceHandles.emplace(key, GPUSceneHandle{ index, slot.m_Generation });
            }
        }

        m_Impl->m_Extracting = false;
        const auto extractionEnd = std::chrono::steady_clock::now();
        const f64 extractionTimeMs =
            std::chrono::duration<f64, std::milli>(extractionEnd - m_Impl->m_ExtractionStart).count();
        const u32 unsupportedTotal = std::accumulate(m_Impl->m_UnsupportedCounts.begin(),
                                                     m_Impl->m_UnsupportedCounts.end(), 0u);
        m_Impl->m_LastFrameUpdate = GPUSceneFrameUpdate{
            .m_InstanceDirtyRanges = CoalesceDirtyRanges(dirtyInstanceSlots),
            .m_GeometryDirtyRanges = CoalesceDirtyRanges(dirtyGeometrySlots),
            .m_Stats = {
                .m_LiveInstances = static_cast<u32>(m_Impl->m_InstanceHandles.size()),
                .m_InstanceSlotCount = static_cast<u32>(m_Impl->m_InstanceSlots.size()),
                .m_InstanceBufferCapacity = m_Impl->m_InstanceBufferCapacity,
                .m_LiveGeometries = static_cast<u32>(m_Impl->m_GeometryHandles.size()),
                .m_GeometrySlotCount = static_cast<u32>(m_Impl->m_GeometrySlots.size()),
                .m_GeometryBufferCapacity = m_Impl->m_GeometryBufferCapacity,
                .m_FreeInstanceSlots = static_cast<u32>(m_Impl->m_FreeInstanceSlots.size()),
                .m_FreeGeometrySlots = static_cast<u32>(m_Impl->m_FreeGeometrySlots.size()),
                .m_RetiredInstanceSlots = static_cast<u32>(m_Impl->m_RetiredInstanceSlots.size()),
                .m_RetiredGeometrySlots = static_cast<u32>(m_Impl->m_RetiredGeometrySlots.size()),
                .m_UnsupportedTotal = unsupportedTotal,
                .m_ExtractionTimeMs = extractionTimeMs,
                .m_UnsupportedCounts = m_Impl->m_UnsupportedCounts,
            },
        };
        m_Impl->m_UploadPending = true;
        return m_Impl->m_LastFrameUpdate;
    }

    void GPUScene::InitializeGPU(u32 initialInstanceCapacity, u32 initialGeometryCapacity)
    {
        if (HasGPUResources())
        {
            return;
        }

        m_Impl->m_InstanceBufferCapacity = std::max(initialInstanceCapacity, 1u);
        m_Impl->m_GeometryBufferCapacity = std::max(initialGeometryCapacity, 1u);
        m_Impl->m_InstanceBuffer = StorageBuffer::Create(
            BytesForRecords<GPUSceneInstance>(m_Impl->m_InstanceBufferCapacity),
            GPUSceneBindingLayout::Instances, StorageBufferUsage::DynamicDraw);
        m_Impl->m_GeometryBuffer = StorageBuffer::Create(
            BytesForRecords<GPUSceneGeometry>(m_Impl->m_GeometryBufferCapacity),
            GPUSceneBindingLayout::Geometries, StorageBufferUsage::DynamicDraw);
        m_Impl->m_InstanceBuffer->Unbind();
        m_Impl->m_GeometryBuffer->Unbind();

        // Fresh storage has undefined contents, including after a renderer
        // restart. Seed every allocated CPU slot, including free tombstones,
        // and keep that intent pending if extraction happens before Upload().
        const auto instanceRecordCount = static_cast<u32>(m_Impl->m_InstanceRecords.size());
        for (u32 index = 0; index < instanceRecordCount; ++index)
        {
            m_Impl->m_PendingDirtyInstanceSlots.insert(index);
        }
        const auto geometryRecordCount = static_cast<u32>(m_Impl->m_GeometryRecords.size());
        for (u32 index = 0; index < geometryRecordCount; ++index)
        {
            m_Impl->m_PendingDirtyGeometrySlots.insert(index);
        }
        m_Impl->m_LastFrameUpdate.m_InstanceDirtyRanges =
            CoalesceDirtyRanges(m_Impl->m_PendingDirtyInstanceSlots);
        m_Impl->m_LastFrameUpdate.m_GeometryDirtyRanges =
            CoalesceDirtyRanges(m_Impl->m_PendingDirtyGeometrySlots);
        m_Impl->m_LastFrameUpdate.m_Stats.m_InstanceBufferCapacity = m_Impl->m_InstanceBufferCapacity;
        m_Impl->m_LastFrameUpdate.m_Stats.m_GeometryBufferCapacity = m_Impl->m_GeometryBufferCapacity;
        m_Impl->m_UploadPending = true;
    }

    void GPUScene::Upload()
    {
        OLO_CORE_ASSERT(HasGPUResources(), "GPUScene::Upload requires InitializeGPU");
        OLO_CORE_ASSERT(!m_Impl->m_Extracting, "GPUScene::Upload cannot run during extraction");
        if (!HasGPUResources() || !m_Impl->m_UploadPending)
        {
            return;
        }

        std::vector<GPUSceneDirtyRange> instanceRanges = m_Impl->m_LastFrameUpdate.m_InstanceDirtyRanges;
        std::vector<GPUSceneDirtyRange> geometryRanges = m_Impl->m_LastFrameUpdate.m_GeometryDirtyRanges;
        u32 growthEvents = 0;

        const auto requiredInstances = static_cast<u32>(m_Impl->m_InstanceRecords.size());
        if (requiredInstances > m_Impl->m_InstanceBufferCapacity)
        {
            m_Impl->m_InstanceBufferCapacity =
                GPUSceneAllocationPolicy::GrowCapacity(m_Impl->m_InstanceBufferCapacity, requiredInstances);
            m_Impl->m_InstanceBuffer->Resize(BytesForRecords<GPUSceneInstance>(m_Impl->m_InstanceBufferCapacity));
            m_Impl->m_InstanceBuffer->Unbind();
            instanceRanges = FullRange(requiredInstances);
            ++growthEvents;
        }

        const auto requiredGeometries = static_cast<u32>(m_Impl->m_GeometryRecords.size());
        if (requiredGeometries > m_Impl->m_GeometryBufferCapacity)
        {
            m_Impl->m_GeometryBufferCapacity =
                GPUSceneAllocationPolicy::GrowCapacity(m_Impl->m_GeometryBufferCapacity, requiredGeometries);
            m_Impl->m_GeometryBuffer->Resize(BytesForRecords<GPUSceneGeometry>(m_Impl->m_GeometryBufferCapacity));
            m_Impl->m_GeometryBuffer->Unbind();
            geometryRanges = FullRange(requiredGeometries);
            ++growthEvents;
        }

        u64 uploadBytes = 0;
        for (const GPUSceneDirtyRange& range : geometryRanges)
        {
            OLO_CORE_ASSERT(static_cast<u64>(range.m_FirstIndex) + range.m_Count <= m_Impl->m_GeometryRecords.size(),
                            "GPUScene geometry dirty range exceeds allocated slots");
            const u32 sizeBytes = BytesForRecords<GPUSceneGeometry>(range.m_Count);
            const u32 offsetBytes = BytesForRecords<GPUSceneGeometry>(range.m_FirstIndex);
            m_Impl->m_GeometryBuffer->SetData(m_Impl->m_GeometryRecords.data() + range.m_FirstIndex,
                                              sizeBytes, offsetBytes);
            uploadBytes += sizeBytes;
        }
        for (const GPUSceneDirtyRange& range : instanceRanges)
        {
            OLO_CORE_ASSERT(static_cast<u64>(range.m_FirstIndex) + range.m_Count <= m_Impl->m_InstanceRecords.size(),
                            "GPUScene instance dirty range exceeds allocated slots");
            const u32 sizeBytes = BytesForRecords<GPUSceneInstance>(range.m_Count);
            const u32 offsetBytes = BytesForRecords<GPUSceneInstance>(range.m_FirstIndex);
            m_Impl->m_InstanceBuffer->SetData(m_Impl->m_InstanceRecords.data() + range.m_FirstIndex,
                                              sizeBytes, offsetBytes);
            uploadBytes += sizeBytes;
        }

        m_Impl->m_LastFrameUpdate.m_Stats.m_UploadBytes = uploadBytes;
        m_Impl->m_LastFrameUpdate.m_Stats.m_BufferGrowthEvents = growthEvents;
        m_Impl->m_LastFrameUpdate.m_Stats.m_InstanceBufferCapacity = m_Impl->m_InstanceBufferCapacity;
        m_Impl->m_LastFrameUpdate.m_Stats.m_GeometryBufferCapacity = m_Impl->m_GeometryBufferCapacity;
        m_Impl->m_UploadPending = false;
    }

    void GPUScene::Bind() const
    {
        OLO_CORE_ASSERT(HasGPUResources(), "GPUScene::Bind requires InitializeGPU");
        if (HasGPUResources())
        {
            m_Impl->m_GeometryBuffer->Bind();
            m_Impl->m_InstanceBuffer->Bind();
        }
    }

    void GPUScene::Shutdown()
    {
        Reset();
        if (m_Impl->m_GeometryBuffer)
        {
            m_Impl->m_GeometryBuffer->Unbind();
        }
        if (m_Impl->m_InstanceBuffer)
        {
            m_Impl->m_InstanceBuffer->Unbind();
        }
        m_Impl->m_GeometryBuffer.Reset();
        m_Impl->m_InstanceBuffer.Reset();
        m_Impl->m_GeometryBufferCapacity = 0;
        m_Impl->m_InstanceBufferCapacity = 0;
        m_Impl->m_LastFrameUpdate.m_Stats.m_GeometryBufferCapacity = 0;
        m_Impl->m_LastFrameUpdate.m_Stats.m_InstanceBufferCapacity = 0;
        m_Impl->m_UploadPending = false;
    }

    bool GPUScene::HasGPUResources() const
    {
        return m_Impl->m_InstanceBuffer && m_Impl->m_GeometryBuffer;
    }

    RHI::ResourceHandle GPUScene::GetInstanceBufferHandle() const
    {
        return m_Impl->m_InstanceBuffer ? m_Impl->m_InstanceBuffer->GetRHIHandle() : RHI::ResourceHandle{};
    }

    RHI::ResourceHandle GPUScene::GetGeometryBufferHandle() const
    {
        return m_Impl->m_GeometryBuffer ? m_Impl->m_GeometryBuffer->GetRHIHandle() : RHI::ResourceHandle{};
    }

    GPUSceneHandle GPUScene::FindGeometry(const GPUSceneGeometryKey& key) const
    {
        const auto found = m_Impl->m_GeometryHandles.find(key);
        return found != m_Impl->m_GeometryHandles.end() ? found->second : GPUSceneHandle{};
    }

    GPUSceneHandle GPUScene::FindInstance(const GPUSceneInstanceKey& key) const
    {
        const auto found = m_Impl->m_InstanceHandles.find(key);
        return found != m_Impl->m_InstanceHandles.end() ? found->second : GPUSceneHandle{};
    }

    bool GPUScene::IsGeometryHandleLive(GPUSceneHandle handle) const
    {
        if (!handle.IsValid() || handle.m_Index >= m_Impl->m_GeometrySlots.size())
        {
            return false;
        }
        const auto& slot = m_Impl->m_GeometrySlots[handle.m_Index];
        return slot.m_Live && slot.m_Generation == handle.m_Generation;
    }

    bool GPUScene::IsInstanceHandleLive(GPUSceneHandle handle) const
    {
        if (!handle.IsValid() || handle.m_Index >= m_Impl->m_InstanceSlots.size())
        {
            return false;
        }
        const auto& slot = m_Impl->m_InstanceSlots[handle.m_Index];
        return slot.m_Live && slot.m_Generation == handle.m_Generation;
    }

    const GPUSceneGeometry* GPUScene::GetGeometryRecord(GPUSceneHandle handle) const
    {
        return IsGeometryHandleLive(handle) ? &m_Impl->m_GeometryRecords[handle.m_Index] : nullptr;
    }

    const GPUSceneInstance* GPUScene::GetInstanceRecord(GPUSceneHandle handle) const
    {
        return IsInstanceHandleLive(handle) ? &m_Impl->m_InstanceRecords[handle.m_Index] : nullptr;
    }

    const GPUSceneFrameUpdate& GPUScene::GetLastFrameUpdate() const
    {
        return m_Impl->m_LastFrameUpdate;
    }

    void GPUScene::Reset()
    {
        for (const auto& [key, handle] : m_Impl->m_InstanceHandles)
        {
            (void)key;
            auto& slot = m_Impl->m_InstanceSlots[handle.m_Index];
            slot.m_Live = false;
            if (AdvanceGeneration(slot.m_Generation))
            {
                m_Impl->m_RetiredInstanceSlots.push_back(Impl::RetiredSlot{
                    .m_Index = handle.m_Index,
                    .m_ReadyFrame = GPUSceneAllocationPolicy::RetirementReadyFrame(m_Impl->m_FrameNumber),
                });
            }
            auto& tombstone = m_Impl->m_InstanceRecords[handle.m_Index];
            tombstone = GPUSceneInstance{};
            tombstone.StableIndex = handle.m_Index;
            tombstone.Generation = slot.m_Generation;
            m_Impl->m_PendingDirtyInstanceSlots.insert(handle.m_Index);
        }
        for (const auto& [key, handle] : m_Impl->m_GeometryHandles)
        {
            (void)key;
            auto& slot = m_Impl->m_GeometrySlots[handle.m_Index];
            slot.m_Live = false;
            if (AdvanceGeneration(slot.m_Generation))
            {
                m_Impl->m_RetiredGeometrySlots.push_back(Impl::RetiredSlot{
                    .m_Index = handle.m_Index,
                    .m_ReadyFrame = GPUSceneAllocationPolicy::RetirementReadyFrame(m_Impl->m_FrameNumber),
                });
            }
            auto& tombstone = m_Impl->m_GeometryRecords[handle.m_Index];
            tombstone = GPUSceneGeometry{};
            tombstone.Generation = slot.m_Generation;
            m_Impl->m_PendingDirtyGeometrySlots.insert(handle.m_Index);
        }

        m_Impl->m_OwnerToken = 0;
        m_Impl->m_HasOwner = false;
        m_Impl->m_Extracting = false;
        m_Impl->m_StagedGeometries.clear();
        m_Impl->m_StagedInstances.clear();
        m_Impl->m_GeometryHandles.clear();
        m_Impl->m_InstanceHandles.clear();
        m_Impl->m_UnsupportedCounts.fill(0);
        m_Impl->m_LastFrameUpdate = GPUSceneFrameUpdate{
            .m_InstanceDirtyRanges = CoalesceDirtyRanges(m_Impl->m_PendingDirtyInstanceSlots),
            .m_GeometryDirtyRanges = CoalesceDirtyRanges(m_Impl->m_PendingDirtyGeometrySlots),
            .m_Stats = {
                .m_InstanceSlotCount = static_cast<u32>(m_Impl->m_InstanceSlots.size()),
                .m_InstanceBufferCapacity = m_Impl->m_InstanceBufferCapacity,
                .m_GeometrySlotCount = static_cast<u32>(m_Impl->m_GeometrySlots.size()),
                .m_GeometryBufferCapacity = m_Impl->m_GeometryBufferCapacity,
                .m_FreeInstanceSlots = static_cast<u32>(m_Impl->m_FreeInstanceSlots.size()),
                .m_FreeGeometrySlots = static_cast<u32>(m_Impl->m_FreeGeometrySlots.size()),
                .m_RetiredInstanceSlots = static_cast<u32>(m_Impl->m_RetiredInstanceSlots.size()),
                .m_RetiredGeometrySlots = static_cast<u32>(m_Impl->m_RetiredGeometrySlots.size()),
            },
        };
        m_Impl->m_UploadPending = true;
    }
} // namespace OloEngine
