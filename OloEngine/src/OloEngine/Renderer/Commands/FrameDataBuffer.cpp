#include "OloEnginePCH.h"
#include "FrameDataBuffer.h"

#include <cstring>

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

} // namespace OloEngine
