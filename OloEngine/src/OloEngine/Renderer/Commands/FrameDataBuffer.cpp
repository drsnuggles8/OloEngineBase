#include "OloEnginePCH.h"
#include "FrameDataBuffer.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Renderer/Commands/CommandLifecycle.h"

#include <cstring>

namespace OloEngine
{
    FrameDataBuffer::FrameDataBuffer(sizet boneCapacity, sizet transformCapacity, sizet entityIDCapacity)
    {
        m_BoneMatrices.SetNum(static_cast<i64>(boneCapacity), EAllowShrinking::No);
        m_Transforms.SetNum(static_cast<i64>(transformCapacity), EAllowShrinking::No);
        m_EntityIDs.SetNum(static_cast<i64>(entityIDCapacity), EAllowShrinking::No);
        m_Colors.SetNum(static_cast<i64>(DEFAULT_COLOR_CAPACITY), EAllowShrinking::No);
        m_Customs.SetNum(static_cast<i64>(DEFAULT_CUSTOM_CAPACITY), EAllowShrinking::No);
        m_GPUSceneRefs.SetNum(static_cast<i64>(DEFAULT_GPU_SCENE_REF_CAPACITY), EAllowShrinking::No);
        m_BoneMatrixOffset = 0;
        m_TransformOffset = 0;
        m_EntityIDOffset = 0;
        m_ColorOffset = 0;
        m_CustomOffset = 0;
        m_GPUSceneRefOffset = 0;
        m_MaterialData.SetNum(static_cast<i64>(MAX_MATERIAL_DATA_PER_FRAME), EAllowShrinking::No);
    }

    void FrameDataBuffer::Reset()
    {
        // Just reset offsets - no need to clear data
        // Thread-safe: acquire both locks to prevent races with allocations
        {
            TUniqueLock<FMutex> boneLock(m_BoneMutex);
            m_BoneMatrixOffset = 0;
            m_PublishedBoneMatrices = 0;
            m_BoneOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> transformLock(m_TransformMutex);
            m_TransformOffset = 0;
            m_PublishedTransforms = 0;
            m_TransformOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> entityIDLock(m_EntityIDMutex);
            m_EntityIDOffset = 0;
            m_PublishedEntityIDs = 0;
            m_EntityIDOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> colorLock(m_ColorMutex);
            m_ColorOffset = 0;
            m_PublishedColors = 0;
            m_ColorOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> customLock(m_CustomMutex);
            m_CustomOffset = 0;
            m_PublishedCustoms = 0;
            m_CustomOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> gpuSceneRefLock(m_GPUSceneRefMutex);
            m_GPUSceneRefOffset = 0;
            m_PublishedGPUSceneRefs = 0;
            m_GPUSceneRefOverflowLogged = false;
        }

        // Reset parallel submission state
        m_ParallelSubmissionActive = false;

        // Reset render state table
        {
            TUniqueLock<FMutex> renderStateLock(m_RenderStateMutex);
            m_RenderStateCount = 0;
            m_RenderStateOverflowLogged = false;
        }

        // Reset material data table
        {
            TUniqueLock<FMutex> materialDataLock(m_MaterialDataMutex);
            m_MaterialDataCount = 0;
            m_MaterialDataOverflowLogged = false;
        }

        // Reset worker scratch buffers
        for (auto& scratch : m_WorkerScratchBuffers)
        {
            scratch.Reset();
        }
    }

    void FrameDataBuffer::PublishForReplay()
    {
        // Each watermark is moved under its stream's own lock, the same lock
        // every Allocate and Write of that stream takes, so a write racing
        // this call is either entirely before the watermark moved (legal) or
        // is checked against the new one.
        {
            TUniqueLock<FMutex> lock(m_BoneMutex);
            m_PublishedBoneMatrices = m_BoneMatrixOffset;
        }
        {
            TUniqueLock<FMutex> lock(m_TransformMutex);
            m_PublishedTransforms = m_TransformOffset;
        }
        {
            TUniqueLock<FMutex> lock(m_EntityIDMutex);
            m_PublishedEntityIDs = m_EntityIDOffset;
        }
        {
            TUniqueLock<FMutex> lock(m_ColorMutex);
            m_PublishedColors = m_ColorOffset;
        }
        {
            TUniqueLock<FMutex> lock(m_CustomMutex);
            m_PublishedCustoms = m_CustomOffset;
        }
        {
            TUniqueLock<FMutex> lock(m_GPUSceneRefMutex);
            m_PublishedGPUSceneRefs = m_GPUSceneRefOffset;
        }
    }

    u32 FrameDataBuffer::GetPublishedTransformCount() const
    {
        TUniqueLock<FMutex> lock(m_TransformMutex);
        return m_PublishedTransforms;
    }

    u32 FrameDataBuffer::GetPublishedBoneMatrixCount() const
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        return m_PublishedBoneMatrices;
    }

    bool FrameDataBuffer::RefusePublishedWrite(u32 offset, u32 published, const char* where)
    {
        // Allocations are bump-only, so everything below the watermark was
        // handed out before the replay that published it, and a packet in that
        // replay may be reading it. Everything at or above it is new.
        if (offset >= published) [[likely]]
            return false;
        CommandLifecycle::ReportViolation(CommandLifecycle::Violation::PayloadWrittenAfterPublish, where);
        return true;
    }

    u32 FrameDataBuffer::AllocateBoneMatrices(u32 count)
    {
        if (count == 0)
            return 0;

        TUniqueLock<FMutex> lock(m_BoneMutex);

        u32 offset = m_BoneMatrixOffset;

        // Check for overflow before addition
        if (count > static_cast<u32>(static_cast<sizet>(m_BoneMatrices.Num())) - offset)
        {
            if (!m_BoneOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Bone matrix buffer overflow! Requested {} matrices at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, static_cast<sizet>(m_BoneMatrices.Num()));
                m_BoneOverflowLogged = true;
            }
            return UINT32_MAX;
        }

        m_BoneMatrixOffset = offset + count;
        return offset;
    }

    u32 FrameDataBuffer::AllocateTransforms(u32 count)
    {
        if (count == 0)
            return 0;

        TUniqueLock<FMutex> lock(m_TransformMutex);

        u32 offset = m_TransformOffset;

        // Check for overflow before addition
        if (count > static_cast<u32>(static_cast<sizet>(m_Transforms.Num())) - offset)
        {
            if (!m_TransformOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Transform buffer overflow! Requested {} transforms at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, static_cast<sizet>(m_Transforms.Num()));
                m_TransformOverflowLogged = true;
            }
            return UINT32_MAX;
        }

        m_TransformOffset = offset + count;
        return offset;
    }

    const glm::mat4* FrameDataBuffer::GetBoneMatrixPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        if (offset >= static_cast<sizet>(m_BoneMatrices.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid bone matrix offset {}", offset);
            return nullptr;
        }
        return &m_BoneMatrices[offset];
    }

    const glm::mat4* FrameDataBuffer::GetBoneMatrixRange(u32 offset, u32 count) const
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        // u64 arithmetic so an offset + count that wraps u32 cannot pass the
        // test it is supposed to fail.
        if (count == 0 || static_cast<u64>(offset) + static_cast<u64>(count) > static_cast<u64>(static_cast<sizet>(m_BoneMatrices.Num())))
        {
            OLO_CORE_ERROR("FrameDataBuffer: Bone matrix range [{}, {}) is outside the buffer ({} matrices)",
                           offset, static_cast<u64>(offset) + static_cast<u64>(count), static_cast<sizet>(m_BoneMatrices.Num()));
            return nullptr;
        }
        return &m_BoneMatrices[offset];
    }

    const glm::mat4* FrameDataBuffer::GetTransformPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_TransformMutex);
        if (offset >= static_cast<sizet>(m_Transforms.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid transform offset {}", offset);
            return nullptr;
        }
        return &m_Transforms[offset];
    }

    void FrameDataBuffer::WriteBoneMatrices(u32 offset, const glm::mat4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        if (RefusePublishedWrite(offset, m_PublishedBoneMatrices, "FrameDataBuffer::WriteBoneMatrices"))
            return;
        if (offset + count > static_cast<sizet>(m_BoneMatrices.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteBoneMatrices out of bounds: offset={}, count={}, capacity={}",
                           offset, count, static_cast<sizet>(m_BoneMatrices.Num()));
            return;
        }
        std::memcpy(&m_BoneMatrices[offset], data, count * sizeof(glm::mat4));
    }

    void FrameDataBuffer::WriteTransforms(u32 offset, const glm::mat4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_TransformMutex);
        if (RefusePublishedWrite(offset, m_PublishedTransforms, "FrameDataBuffer::WriteTransforms"))
            return;
        if (offset + count > static_cast<sizet>(m_Transforms.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteTransforms out of bounds: offset={}, count={}, capacity={}",
                           offset, count, static_cast<sizet>(m_Transforms.Num()));
            return;
        }
        std::memcpy(&m_Transforms[offset], data, count * sizeof(glm::mat4));
    }

    u32 FrameDataBuffer::AllocateEntityIDs(u32 count)
    {
        if (count == 0)
            return 0;
        TUniqueLock<FMutex> lock(m_EntityIDMutex);
        u32 offset = m_EntityIDOffset;
        if (count > static_cast<u32>(static_cast<sizet>(m_EntityIDs.Num())) - offset)
        {
            if (!m_EntityIDOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: EntityID buffer overflow! Requested {} ids at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, static_cast<sizet>(m_EntityIDs.Num()));
                m_EntityIDOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_EntityIDOffset = offset + count;
        return offset;
    }

    const i32* FrameDataBuffer::GetEntityIDPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_EntityIDMutex);
        if (offset >= static_cast<sizet>(m_EntityIDs.Num()))
            return nullptr;
        return &m_EntityIDs[offset];
    }

    void FrameDataBuffer::WriteEntityIDs(u32 offset, const i32* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_EntityIDMutex);
        if (RefusePublishedWrite(offset, m_PublishedEntityIDs, "FrameDataBuffer::WriteEntityIDs"))
            return;
        if (offset + count > static_cast<sizet>(m_EntityIDs.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteEntityIDs out of bounds: offset={}, count={}, capacity={}",
                           offset, count, static_cast<sizet>(m_EntityIDs.Num()));
            return;
        }
        std::memcpy(&m_EntityIDs[offset], data, count * sizeof(i32));
    }

    u32 FrameDataBuffer::AllocateColors(u32 count)
    {
        if (count == 0)
            return 0;
        TUniqueLock<FMutex> lock(m_ColorMutex);
        u32 offset = m_ColorOffset;
        if (count > static_cast<u32>(static_cast<sizet>(m_Colors.Num())) - offset)
        {
            if (!m_ColorOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Color buffer overflow! Requested {} at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, static_cast<sizet>(m_Colors.Num()));
                m_ColorOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_ColorOffset = offset + count;
        return offset;
    }

    const glm::vec4* FrameDataBuffer::GetColorPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_ColorMutex);
        if (offset >= static_cast<sizet>(m_Colors.Num()))
            return nullptr;
        return &m_Colors[offset];
    }

    void FrameDataBuffer::WriteColors(u32 offset, const glm::vec4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_ColorMutex);
        if (RefusePublishedWrite(offset, m_PublishedColors, "FrameDataBuffer::WriteColors"))
            return;
        if (offset + count > static_cast<sizet>(m_Colors.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteColors out of bounds: offset={}, count={}, capacity={}",
                           offset, count, static_cast<sizet>(m_Colors.Num()));
            return;
        }
        std::memcpy(&m_Colors[offset], data, count * sizeof(glm::vec4));
    }

    u32 FrameDataBuffer::AllocateCustoms(u32 count)
    {
        if (count == 0)
            return 0;
        TUniqueLock<FMutex> lock(m_CustomMutex);
        u32 offset = m_CustomOffset;
        if (count > static_cast<u32>(static_cast<sizet>(m_Customs.Num())) - offset)
        {
            if (!m_CustomOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Custom buffer overflow! Requested {} at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, static_cast<sizet>(m_Customs.Num()));
                m_CustomOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_CustomOffset = offset + count;
        return offset;
    }

    const f32* FrameDataBuffer::GetCustomPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_CustomMutex);
        if (offset >= static_cast<sizet>(m_Customs.Num()))
            return nullptr;
        return &m_Customs[offset];
    }

    void FrameDataBuffer::WriteCustoms(u32 offset, const f32* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_CustomMutex);
        if (RefusePublishedWrite(offset, m_PublishedCustoms, "FrameDataBuffer::WriteCustoms"))
            return;
        if (offset + count > static_cast<sizet>(m_Customs.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteCustoms out of bounds: offset={}, count={}, capacity={}",
                           offset, count, static_cast<sizet>(m_Customs.Num()));
            return;
        }
        std::memcpy(&m_Customs[offset], data, count * sizeof(f32));
    }

    u32 FrameDataBuffer::AllocateGPUSceneRefs(u32 count)
    {
        TUniqueLock<FMutex> lock(m_GPUSceneRefMutex);
        const u32 offset = m_GPUSceneRefOffset;
        if (count > static_cast<u32>(static_cast<sizet>(m_GPUSceneRefs.Num())) - offset)
        {
            if (!m_GPUSceneRefOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: GPU Scene reference buffer overflow! Requested {} at offset {}, capacity {}. Subsequent overflows this frame will be silent.", count, offset, static_cast<sizet>(m_GPUSceneRefs.Num()));
                m_GPUSceneRefOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_GPUSceneRefOffset = offset + count;
        return offset;
    }

    const glm::uvec4* FrameDataBuffer::GetGPUSceneRefPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_GPUSceneRefMutex);
        return offset < static_cast<sizet>(m_GPUSceneRefs.Num()) ? &m_GPUSceneRefs[offset] : nullptr;
    }

    void FrameDataBuffer::WriteGPUSceneRefs(u32 offset, const glm::uvec4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_GPUSceneRefMutex);
        if (RefusePublishedWrite(offset, m_PublishedGPUSceneRefs, "FrameDataBuffer::WriteGPUSceneRefs"))
            return;
        if (offset + count > static_cast<sizet>(m_GPUSceneRefs.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteGPUSceneRefs out of bounds: offset={}, count={}, capacity={}", offset, count, static_cast<sizet>(m_GPUSceneRefs.Num()));
            return;
        }
        std::memcpy(&m_GPUSceneRefs[offset], data, count * sizeof(glm::uvec4));
    }

    // ========================================================================
    // RenderState Table Implementation
    // ========================================================================

    u16 FrameDataBuffer::AllocateRenderState(const PODRenderState& state)
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_RenderStateMutex);

        // Dedup: linear scan for matching state (N is small, typically 2-5)
        for (u16 i = 0; i < m_RenderStateCount; ++i)
        {
            if (m_RenderStates[i] == state)
            {
                return i;
            }
        }

        // New unique state — allocate
        if (m_RenderStateCount >= MAX_RENDER_STATES_PER_FRAME)
        {
            if (!m_RenderStateOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: RenderState table overflow! Max {} unique states per frame. "
                               "Subsequent overflows this frame will be silent.",
                               MAX_RENDER_STATES_PER_FRAME);
                m_RenderStateOverflowLogged = true;
            }
            return INVALID_RENDER_STATE_INDEX;
        }

        u16 index = m_RenderStateCount;
        m_RenderStates[index] = state;
        ++m_RenderStateCount;

        return index;
    }

    const PODRenderState& FrameDataBuffer::GetRenderState(u16 index) const
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_RenderStateMutex);
        if (index >= m_RenderStateCount)
        {
            OLO_CORE_ERROR("FrameDataBuffer::GetRenderState: index {} out of range (count {}), returning default", index, m_RenderStateCount);
            static const PODRenderState s_Default{};
            return s_Default;
        }
        return m_RenderStates[index];
    }

    // ========================================================================
    // MaterialData Table Implementation
    // ========================================================================

    u16 FrameDataBuffer::AllocateMaterialData(const PODMaterialData& data)
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_MaterialDataMutex);

        // Dedup: linear scan (N is typically 10-100 unique materials per frame)
        for (u16 i = 0; i < m_MaterialDataCount; ++i)
        {
            if (m_MaterialData[i] == data)
            {
                return i;
            }
        }

        // New unique material — allocate
        if (m_MaterialDataCount >= MAX_MATERIAL_DATA_PER_FRAME)
        {
            if (!m_MaterialDataOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: MaterialData table overflow! Max {} unique materials per frame. "
                               "Subsequent overflows this frame will be silent.",
                               MAX_MATERIAL_DATA_PER_FRAME);
                m_MaterialDataOverflowLogged = true;
            }
            return INVALID_MATERIAL_DATA_INDEX;
        }

        u16 index = m_MaterialDataCount;
        m_MaterialData[index] = data;
        ++m_MaterialDataCount;

        return index;
    }

    const PODMaterialData& FrameDataBuffer::GetMaterialData(u16 index) const
    {
        OLO_PROFILE_FUNCTION();
        TUniqueLock<FMutex> lock(m_MaterialDataMutex);
        if (index >= m_MaterialDataCount)
        {
            OLO_CORE_ERROR("FrameDataBuffer::GetMaterialData: index {} out of range (count {}), returning default", index, m_MaterialDataCount);
            static const PODMaterialData s_Default{};
            return s_Default;
        }
        return m_MaterialData[index];
    }

    // Static manager implementation
    FrameDataBuffer* FrameDataBufferManager::s_Buffer = nullptr;

    void FrameDataBufferManager::Init()
    {
        OLO_CORE_ASSERT(!s_Buffer, "FrameDataBufferManager already initialized!");
        s_Buffer = new FrameDataBuffer();
        OLO_CORE_INFO("FrameDataBuffer initialized with {} bone capacity, {} transform capacity",
                      s_Buffer->GetBoneMatrixCapacity(), s_Buffer->GetTransformCapacity());
    }

    void FrameDataBufferManager::Shutdown()
    {
        delete s_Buffer;
        s_Buffer = nullptr;
    }

    FrameDataBuffer& FrameDataBufferManager::Get()
    {
        OLO_CORE_ASSERT(s_Buffer, "FrameDataBufferManager not initialized!");
        return *s_Buffer;
    }

    // ========================================================================
    // Thread-Local Scratch Buffer Implementation
    // ========================================================================

    void FrameDataBuffer::PrepareForParallelSubmission()
    {
        OLO_PROFILE_FUNCTION();

        // Reset all scratch buffers
        for (auto& scratch : m_WorkerScratchBuffers)
        {
            scratch.Reset();
        }

        m_ParallelSubmissionActive = true;
    }

    u32 FrameDataBuffer::AllocateBoneMatricesParallel(u32 workerIndex, u32 count)
    {
        OLO_CORE_ASSERT(workerIndex < MAX_FRAME_DATA_WORKERS,
                        "FrameDataBuffer: Invalid worker index!");
        OLO_CORE_ASSERT(m_ParallelSubmissionActive,
                        "FrameDataBuffer: Not in parallel submission mode!");

        WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];

        // Ensure capacity
        if (scratch.boneCount + count > static_cast<sizet>(scratch.bones.Num()))
        {
            sizet newCapacity = std::max(
                static_cast<sizet>(scratch.bones.Num()) * 2,
                static_cast<sizet>(scratch.boneCount + count));
            scratch.bones.SetNum(static_cast<i64>(newCapacity), EAllowShrinking::No);
        }

        u32 localOffset = scratch.boneCount;
        scratch.boneCount += count;

        return localOffset;
    }

    u32 FrameDataBuffer::AllocateTransformsParallel(u32 workerIndex, u32 count)
    {
        OLO_CORE_ASSERT(workerIndex < MAX_FRAME_DATA_WORKERS,
                        "FrameDataBuffer: Invalid worker index!");
        OLO_CORE_ASSERT(m_ParallelSubmissionActive,
                        "FrameDataBuffer: Not in parallel submission mode!");

        // Sanity check: count must be positive and not overflow
        if (count == 0)
        {
            OLO_CORE_ERROR("FrameDataBuffer::AllocateTransformsParallel: Count is zero!");
            return UINT32_MAX;
        }
        if (count > WORKER_SCRATCH_TRANSFORM_CAPACITY)
        {
            OLO_CORE_ERROR(
                "FrameDataBuffer::AllocateTransformsParallel: Requested {} transforms exceeds max per-worker limit {}",
                count, WORKER_SCRATCH_TRANSFORM_CAPACITY);
            return UINT32_MAX;
        }

        WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];

        // Check for overflow: ensure addition doesn't wrap around
        if (scratch.transformCount > UINT32_MAX - count)
        {
            OLO_CORE_ERROR(
                "FrameDataBuffer::AllocateTransformsParallel: Allocation would overflow (current={}, requested={})",
                scratch.transformCount, count);
            return UINT32_MAX;
        }

        u32 newCount = scratch.transformCount + count;

        // Ensure capacity with safe type conversion
        if (newCount > static_cast<sizet>(scratch.transforms.Num()))
        {
            // Use size_t to avoid overflow in capacity calculation
            sizet newCapacity = std::max(
                static_cast<sizet>(scratch.transforms.Num()) * 2,
                static_cast<sizet>(newCount));
            scratch.transforms.SetNum(static_cast<i64>(newCapacity), EAllowShrinking::No);
        }

        u32 localOffset = scratch.transformCount;
        scratch.transformCount = newCount;

        return localOffset;
    }

    void FrameDataBuffer::WriteBoneMatricesParallel(u32 workerIndex, u32 localOffset,
                                                    const glm::mat4* data, u32 count)
    {
        OLO_CORE_ASSERT(workerIndex < MAX_FRAME_DATA_WORKERS,
                        "FrameDataBuffer: Invalid worker index!");

        WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];

        // Runtime bounds check (works in release builds unlike assertions)
        if (localOffset + count > static_cast<sizet>(scratch.bones.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer::WriteBoneMatricesParallel: Write out of bounds! offset={}, count={}, capacity={}",
                           localOffset, count, static_cast<sizet>(scratch.bones.Num()));
            return;
        }

        std::memcpy(&scratch.bones[localOffset], data, count * sizeof(glm::mat4));
    }

    void FrameDataBuffer::WriteTransformsParallel(u32 workerIndex, u32 localOffset,
                                                  const glm::mat4* data, u32 count)
    {
        OLO_CORE_ASSERT(workerIndex < MAX_FRAME_DATA_WORKERS,
                        "FrameDataBuffer: Invalid worker index!");

        WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];

        // Runtime bounds check (works in release builds unlike assertions)
        if (localOffset + count > static_cast<sizet>(scratch.transforms.Num()))
        {
            OLO_CORE_ERROR("FrameDataBuffer::WriteTransformsParallel: Write out of bounds! offset={}, count={}, capacity={}",
                           localOffset, count, static_cast<sizet>(scratch.transforms.Num()));
            return;
        }

        std::memcpy(&scratch.transforms[localOffset], data, count * sizeof(glm::mat4));
    }

    void FrameDataBuffer::MergeScratchBuffers()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ParallelSubmissionActive)
        {
            OLO_CORE_WARN("FrameDataBuffer::MergeScratchBuffers: Not in parallel submission mode!");
            return;
        }

        // Calculate total sizes needed
        u32 totalBones = 0;
        u32 totalTransforms = 0;

        for (const auto& scratch : m_WorkerScratchBuffers)
        {
            totalBones += scratch.boneCount;
            totalTransforms += scratch.transformCount;
        }

        // Ensure main buffer has capacity
        if (m_BoneMatrixOffset + totalBones > static_cast<sizet>(m_BoneMatrices.Num()))
        {
            m_BoneMatrices.SetNum(static_cast<i64>(m_BoneMatrixOffset + totalBones), EAllowShrinking::No);
        }
        if (m_TransformOffset + totalTransforms > static_cast<sizet>(m_Transforms.Num()))
        {
            m_Transforms.SetNum(static_cast<i64>(m_TransformOffset + totalTransforms), EAllowShrinking::No);
        }

        // Copy scratch buffers into main buffer and record global offsets
        u32 currentBoneOffset = m_BoneMatrixOffset;
        u32 currentTransformOffset = m_TransformOffset;

        for (auto& scratch : m_WorkerScratchBuffers)
        {
            if (scratch.boneCount > 0)
            {
                scratch.globalBoneOffset = currentBoneOffset;
                std::memcpy(&m_BoneMatrices[currentBoneOffset],
                            scratch.bones.GetData(),
                            scratch.boneCount * sizeof(glm::mat4));
                currentBoneOffset += scratch.boneCount;
            }

            if (scratch.transformCount > 0)
            {
                scratch.globalTransformOffset = currentTransformOffset;
                std::memcpy(&m_Transforms[currentTransformOffset],
                            scratch.transforms.GetData(),
                            scratch.transformCount * sizeof(glm::mat4));
                currentTransformOffset += scratch.transformCount;
            }
        }

        // Update main buffer offsets
        m_BoneMatrixOffset = currentBoneOffset;
        m_TransformOffset = currentTransformOffset;

        m_ParallelSubmissionActive = false;
    }

    u32 FrameDataBuffer::GetGlobalBoneOffset(u32 workerIndex, u32 localOffset) const
    {
        OLO_CORE_ASSERT(workerIndex < MAX_FRAME_DATA_WORKERS,
                        "FrameDataBuffer: Invalid worker index!");

        const WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];
        return scratch.globalBoneOffset + localOffset;
    }

    u32 FrameDataBuffer::GetGlobalTransformOffset(u32 workerIndex, u32 localOffset) const
    {
        OLO_CORE_ASSERT(workerIndex < MAX_FRAME_DATA_WORKERS,
                        "FrameDataBuffer: Invalid worker index!");

        const WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];
        return scratch.globalTransformOffset + localOffset;
    }

    std::pair<u32, WorkerScratchBuffer*> FrameDataBuffer::GetScratchBufferByIndex(u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        if (workerIndex >= MAX_FRAME_DATA_WORKERS)
        {
            OLO_CORE_ERROR("FrameDataBuffer::GetScratchBufferByIndex: Invalid worker index {}! Max is {}",
                           workerIndex, MAX_FRAME_DATA_WORKERS - 1);
            return { 0, nullptr };
        }

        // No mutex needed - direct array access with bounds-checked index
        // Each worker only accesses its own slot
        return { workerIndex, &m_WorkerScratchBuffers[workerIndex] };
    }

} // namespace OloEngine
