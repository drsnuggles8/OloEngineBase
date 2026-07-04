#include "OloEnginePCH.h"
#include "FrameDataBuffer.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <cstring>

namespace OloEngine
{
    FrameDataBuffer::FrameDataBuffer(sizet boneCapacity, sizet transformCapacity, sizet entityIDCapacity)
    {
        m_BoneMatrices.resize(boneCapacity);
        m_Transforms.resize(transformCapacity);
        m_EntityIDs.resize(entityIDCapacity);
        m_Colors.resize(DEFAULT_COLOR_CAPACITY);
        m_Customs.resize(DEFAULT_CUSTOM_CAPACITY);
        m_BoneMatrixOffset = 0;
        m_TransformOffset = 0;
        m_EntityIDOffset = 0;
        m_ColorOffset = 0;
        m_CustomOffset = 0;
        m_MaterialData.resize(MAX_MATERIAL_DATA_PER_FRAME);
    }

    void FrameDataBuffer::Reset()
    {
        // Just reset offsets - no need to clear data
        // Thread-safe: acquire both locks to prevent races with allocations
        {
            TUniqueLock<FMutex> boneLock(m_BoneMutex);
            m_BoneMatrixOffset = 0;
            m_BoneOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> transformLock(m_TransformMutex);
            m_TransformOffset = 0;
            m_TransformOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> entityIDLock(m_EntityIDMutex);
            m_EntityIDOffset = 0;
            m_EntityIDOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> colorLock(m_ColorMutex);
            m_ColorOffset = 0;
            m_ColorOverflowLogged = false;
        }
        {
            TUniqueLock<FMutex> customLock(m_CustomMutex);
            m_CustomOffset = 0;
            m_CustomOverflowLogged = false;
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

    u32 FrameDataBuffer::AllocateBoneMatrices(u32 count)
    {
        if (count == 0)
            return 0;

        TUniqueLock<FMutex> lock(m_BoneMutex);

        u32 offset = m_BoneMatrixOffset;

        // Check for overflow before addition
        if (count > static_cast<u32>(m_BoneMatrices.size()) - offset)
        {
            if (!m_BoneOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Bone matrix buffer overflow! Requested {} matrices at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, m_BoneMatrices.size());
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
        if (count > static_cast<u32>(m_Transforms.size()) - offset)
        {
            if (!m_TransformOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Transform buffer overflow! Requested {} transforms at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, m_Transforms.size());
                m_TransformOverflowLogged = true;
            }
            return UINT32_MAX;
        }

        m_TransformOffset = offset + count;
        return offset;
    }

    glm::mat4* FrameDataBuffer::GetBoneMatrixPtr(u32 offset)
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        if (offset >= m_BoneMatrices.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid bone matrix offset {}", offset);
            return nullptr;
        }
        return &m_BoneMatrices[offset];
    }

    const glm::mat4* FrameDataBuffer::GetBoneMatrixPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        if (offset >= m_BoneMatrices.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid bone matrix offset {}", offset);
            return nullptr;
        }
        return &m_BoneMatrices[offset];
    }

    glm::mat4* FrameDataBuffer::GetTransformPtr(u32 offset)
    {
        TUniqueLock<FMutex> lock(m_TransformMutex);
        if (offset >= m_Transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid transform offset {}", offset);
            return nullptr;
        }
        return &m_Transforms[offset];
    }

    const glm::mat4* FrameDataBuffer::GetTransformPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_TransformMutex);
        if (offset >= m_Transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid transform offset {}", offset);
            return nullptr;
        }
        return &m_Transforms[offset];
    }

    void FrameDataBuffer::WriteBoneMatrices(u32 offset, const glm::mat4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_BoneMutex);
        if (offset + count > m_BoneMatrices.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteBoneMatrices out of bounds: offset={}, count={}, capacity={}",
                           offset, count, m_BoneMatrices.size());
            return;
        }
        std::memcpy(&m_BoneMatrices[offset], data, count * sizeof(glm::mat4));
    }

    void FrameDataBuffer::WriteTransforms(u32 offset, const glm::mat4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_TransformMutex);
        if (offset + count > m_Transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteTransforms out of bounds: offset={}, count={}, capacity={}",
                           offset, count, m_Transforms.size());
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
        if (count > static_cast<u32>(m_EntityIDs.size()) - offset)
        {
            if (!m_EntityIDOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: EntityID buffer overflow! Requested {} ids at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, m_EntityIDs.size());
                m_EntityIDOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_EntityIDOffset = offset + count;
        return offset;
    }

    i32* FrameDataBuffer::GetEntityIDPtr(u32 offset)
    {
        TUniqueLock<FMutex> lock(m_EntityIDMutex);
        if (offset >= m_EntityIDs.size())
            return nullptr;
        return &m_EntityIDs[offset];
    }

    const i32* FrameDataBuffer::GetEntityIDPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_EntityIDMutex);
        if (offset >= m_EntityIDs.size())
            return nullptr;
        return &m_EntityIDs[offset];
    }

    void FrameDataBuffer::WriteEntityIDs(u32 offset, const i32* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_EntityIDMutex);
        if (offset + count > m_EntityIDs.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteEntityIDs out of bounds: offset={}, count={}, capacity={}",
                           offset, count, m_EntityIDs.size());
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
        if (count > static_cast<u32>(m_Colors.size()) - offset)
        {
            if (!m_ColorOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Color buffer overflow! Requested {} at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, m_Colors.size());
                m_ColorOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_ColorOffset = offset + count;
        return offset;
    }

    glm::vec4* FrameDataBuffer::GetColorPtr(u32 offset)
    {
        TUniqueLock<FMutex> lock(m_ColorMutex);
        if (offset >= m_Colors.size())
            return nullptr;
        return &m_Colors[offset];
    }

    const glm::vec4* FrameDataBuffer::GetColorPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_ColorMutex);
        if (offset >= m_Colors.size())
            return nullptr;
        return &m_Colors[offset];
    }

    void FrameDataBuffer::WriteColors(u32 offset, const glm::vec4* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_ColorMutex);
        if (offset + count > m_Colors.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteColors out of bounds: offset={}, count={}, capacity={}",
                           offset, count, m_Colors.size());
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
        if (count > static_cast<u32>(m_Customs.size()) - offset)
        {
            if (!m_CustomOverflowLogged)
            {
                OLO_CORE_ERROR("FrameDataBuffer: Custom buffer overflow! Requested {} at offset {}, capacity {}. "
                               "Subsequent overflows this frame will be silent.",
                               count, offset, m_Customs.size());
                m_CustomOverflowLogged = true;
            }
            return UINT32_MAX;
        }
        m_CustomOffset = offset + count;
        return offset;
    }

    f32* FrameDataBuffer::GetCustomPtr(u32 offset)
    {
        TUniqueLock<FMutex> lock(m_CustomMutex);
        if (offset >= m_Customs.size())
            return nullptr;
        return &m_Customs[offset];
    }

    const f32* FrameDataBuffer::GetCustomPtr(u32 offset) const
    {
        TUniqueLock<FMutex> lock(m_CustomMutex);
        if (offset >= m_Customs.size())
            return nullptr;
        return &m_Customs[offset];
    }

    void FrameDataBuffer::WriteCustoms(u32 offset, const f32* data, u32 count)
    {
        TUniqueLock<FMutex> lock(m_CustomMutex);
        if (offset + count > m_Customs.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteCustoms out of bounds: offset={}, count={}, capacity={}",
                           offset, count, m_Customs.size());
            return;
        }
        std::memcpy(&m_Customs[offset], data, count * sizeof(f32));
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
        if (scratch.boneCount + count > scratch.bones.size())
        {
            sizet newCapacity = std::max(
                scratch.bones.size() * 2,
                static_cast<sizet>(scratch.boneCount + count));
            scratch.bones.resize(newCapacity);
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
        if (newCount > scratch.transforms.size())
        {
            // Use size_t to avoid overflow in capacity calculation
            sizet newCapacity = std::max(
                scratch.transforms.size() * 2,
                static_cast<sizet>(newCount));
            scratch.transforms.resize(newCapacity);
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
        if (localOffset + count > scratch.bones.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer::WriteBoneMatricesParallel: Write out of bounds! offset={}, count={}, capacity={}",
                           localOffset, count, scratch.bones.size());
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
        if (localOffset + count > scratch.transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer::WriteTransformsParallel: Write out of bounds! offset={}, count={}, capacity={}",
                           localOffset, count, scratch.transforms.size());
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
        if (m_BoneMatrixOffset + totalBones > m_BoneMatrices.size())
        {
            m_BoneMatrices.resize(m_BoneMatrixOffset + totalBones);
        }
        if (m_TransformOffset + totalTransforms > m_Transforms.size())
        {
            m_Transforms.resize(m_TransformOffset + totalTransforms);
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
                            scratch.bones.data(),
                            scratch.boneCount * sizeof(glm::mat4));
                currentBoneOffset += scratch.boneCount;
            }

            if (scratch.transformCount > 0)
            {
                scratch.globalTransformOffset = currentTransformOffset;
                std::memcpy(&m_Transforms[currentTransformOffset],
                            scratch.transforms.data(),
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
