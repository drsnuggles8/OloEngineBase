#include "OloEnginePCH.h"
#include "FrameDataBuffer.h"

#include <cstring>
#include <thread>

namespace OloEngine
{
    FrameDataBuffer::FrameDataBuffer(sizet boneCapacity, sizet transformCapacity)
    {
        m_BoneMatrices.resize(boneCapacity);
        m_Transforms.resize(transformCapacity);
        m_BoneMatrixOffset = 0;
        m_TransformOffset = 0;
    }

    void FrameDataBuffer::Reset()
    {
        // Just reset offsets - no need to clear data
        // Thread-safe: acquire both locks to prevent races with allocations
        {
            std::lock_guard<std::mutex> boneLock(m_BoneMutex);
            m_BoneMatrixOffset = 0;
        }
        {
            std::lock_guard<std::mutex> transformLock(m_TransformMutex);
            m_TransformOffset = 0;
        }

        // Reset parallel submission state
        m_ParallelSubmissionActive = false;
        m_NextWorkerIndex.store(0, std::memory_order_relaxed);

        // Reset worker scratch buffers
        for (auto& scratch : m_WorkerScratchBuffers)
        {
            scratch.Reset();
        }

        // Clear worker thread mapping
        {
            std::lock_guard<std::mutex> lock(m_WorkerMapMutex);
            m_ThreadToWorkerIndex.clear();
        }
    }

    u32 FrameDataBuffer::AllocateBoneMatrices(u32 count)
    {
        if (count == 0)
            return 0;

        std::lock_guard<std::mutex> lock(m_BoneMutex);

        u32 offset = m_BoneMatrixOffset;

        // Check for overflow before addition
        if (count > static_cast<u32>(m_BoneMatrices.size()) - offset)
        {
            OLO_CORE_ERROR("FrameDataBuffer: Bone matrix buffer overflow! Requested {} matrices at offset {}, capacity {}",
                           count, offset, m_BoneMatrices.size());
            return UINT32_MAX;
        }

        m_BoneMatrixOffset = offset + count;
        return offset;
    }

    u32 FrameDataBuffer::AllocateTransforms(u32 count)
    {
        if (count == 0)
            return 0;

        std::lock_guard<std::mutex> lock(m_TransformMutex);

        u32 offset = m_TransformOffset;

        // Check for overflow before addition
        if (count > static_cast<u32>(m_Transforms.size()) - offset)
        {
            OLO_CORE_ERROR("FrameDataBuffer: Transform buffer overflow! Requested {} transforms at offset {}, capacity {}",
                           count, offset, m_Transforms.size());
            return UINT32_MAX;
        }

        m_TransformOffset = offset + count;
        return offset;
    }

    glm::mat4* FrameDataBuffer::GetBoneMatrixPtr(u32 offset)
    {
        std::lock_guard<std::mutex> lock(m_BoneMutex);
        if (offset >= m_BoneMatrices.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid bone matrix offset {}", offset);
            return nullptr;
        }
        return &m_BoneMatrices[offset];
    }

    const glm::mat4* FrameDataBuffer::GetBoneMatrixPtr(u32 offset) const
    {
        std::lock_guard<std::mutex> lock(m_BoneMutex);
        if (offset >= m_BoneMatrices.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid bone matrix offset {}", offset);
            return nullptr;
        }
        return &m_BoneMatrices[offset];
    }

    glm::mat4* FrameDataBuffer::GetTransformPtr(u32 offset)
    {
        std::lock_guard<std::mutex> lock(m_TransformMutex);
        if (offset >= m_Transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid transform offset {}", offset);
            return nullptr;
        }
        return &m_Transforms[offset];
    }

    const glm::mat4* FrameDataBuffer::GetTransformPtr(u32 offset) const
    {
        std::lock_guard<std::mutex> lock(m_TransformMutex);
        if (offset >= m_Transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: Invalid transform offset {}", offset);
            return nullptr;
        }
        return &m_Transforms[offset];
    }

    void FrameDataBuffer::WriteBoneMatrices(u32 offset, const glm::mat4* data, u32 count)
    {
        std::lock_guard<std::mutex> lock(m_BoneMutex);
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
        std::lock_guard<std::mutex> lock(m_TransformMutex);
        if (offset + count > m_Transforms.size())
        {
            OLO_CORE_ERROR("FrameDataBuffer: WriteTransforms out of bounds: offset={}, count={}, capacity={}",
                           offset, count, m_Transforms.size());
            return;
        }
        std::memcpy(&m_Transforms[offset], data, count * sizeof(glm::mat4));
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

        // Reset worker index counter
        m_NextWorkerIndex.store(0, std::memory_order_relaxed);

        // Clear worker thread mapping
        {
            std::lock_guard<std::mutex> lock(m_WorkerMapMutex);
            m_ThreadToWorkerIndex.clear();
        }

        m_ParallelSubmissionActive = true;
    }

    std::pair<u32, WorkerScratchBuffer*> FrameDataBuffer::RegisterAndGetScratchBuffer()
    {
        OLO_PROFILE_FUNCTION();

        std::thread::id threadId = std::this_thread::get_id();

        // Hold lock for entire registration path to fix TOCTOU race condition
        std::lock_guard<std::mutex> lock(m_WorkerMapMutex);

        // Check if thread is already registered
        auto it = m_ThreadToWorkerIndex.find(threadId);
        if (it != m_ThreadToWorkerIndex.end())
        {
            u32 workerIndex = it->second;
            return { workerIndex, &m_WorkerScratchBuffers[workerIndex] };
        }

        // Register new worker - check bounds before atomic increment
        u32 currentIndex = m_NextWorkerIndex.load(std::memory_order_relaxed);
        if (currentIndex >= MAX_FRAME_DATA_WORKERS)
        {
            OLO_CORE_ERROR("FrameDataBuffer: Too many worker threads! Max is {}",
                           MAX_FRAME_DATA_WORKERS);
            return { 0, nullptr };
        }

        // Now safe to increment since we're within bounds
        u32 workerIndex = m_NextWorkerIndex.fetch_add(1, std::memory_order_relaxed);
        m_ThreadToWorkerIndex[threadId] = workerIndex;

        return { workerIndex, &m_WorkerScratchBuffers[workerIndex] };
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

        WorkerScratchBuffer& scratch = m_WorkerScratchBuffers[workerIndex];

        // Ensure capacity
        if (scratch.transformCount + count > scratch.transforms.size())
        {
            sizet newCapacity = std::max(
                scratch.transforms.size() * 2,
                static_cast<sizet>(scratch.transformCount + count));
            scratch.transforms.resize(newCapacity);
        }

        u32 localOffset = scratch.transformCount;
        scratch.transformCount += count;

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

        OLO_CORE_TRACE("FrameDataBuffer: Merged {} bones and {} transforms from scratch buffers",
                       totalBones, totalTransforms);
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

    i32 FrameDataBuffer::GetCurrentWorkerIndex() const
    {
        std::thread::id threadId = std::this_thread::get_id();

        std::lock_guard<std::mutex> lock(m_WorkerMapMutex);
        auto it = m_ThreadToWorkerIndex.find(threadId);
        if (it != m_ThreadToWorkerIndex.end())
        {
            return static_cast<i32>(it->second);
        }
        return -1;
    }

} // namespace OloEngine
