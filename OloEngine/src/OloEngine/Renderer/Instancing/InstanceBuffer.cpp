#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <limits>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kMinInitialCapacity = 1;
        constexpr u32 kMaxInstanceCapacity = std::numeric_limits<u32>::max() / sizeof(InstanceData);

        constexpr u32 BytesFor(u32 instanceCount)
        {
            return instanceCount * static_cast<u32>(sizeof(InstanceData));
        }
    } // namespace

    InstanceBuffer::InstanceBuffer(u32 initialCapacity)
        : m_Capacity(initialCapacity <= kMaxInstanceCapacity ? std::max(initialCapacity, kMinInitialCapacity)
                                                             : kMinInitialCapacity)
    {
        if (initialCapacity > kMaxInstanceCapacity)
            OLO_CORE_ERROR("InstanceBuffer: initial capacity {} exceeds representable byte size — using one instance",
                           initialCapacity);
        m_Storage = StorageBuffer::Create(BytesFor(m_Capacity),
                                          ShaderBindingLayout::SSBO_INSTANCE_DATA,
                                          StorageBufferUsage::DynamicDrawExactUpload);
    }

    void InstanceBuffer::EnsureCapacity(u32 requiredCount)
    {
        if (requiredCount <= m_Capacity)
            return;
        if (requiredCount > kMaxInstanceCapacity)
        {
            OLO_CORE_ERROR("InstanceBuffer::EnsureCapacity: {} instances overflow the u32 buffer size — refusing growth",
                           requiredCount);
            return;
        }

        // Grow geometrically (doubling); fall back to exact size if doubling
        // is still insufficient (would only happen for unusually large
        // requested counts compared to the current capacity).
        u32 newCapacity = m_Capacity <= kMaxInstanceCapacity / 2 ? m_Capacity * 2 : kMaxInstanceCapacity;
        if (newCapacity < requiredCount)
            newCapacity = requiredCount;

        m_Storage->Resize(BytesFor(newCapacity));
        // Resize may refuse work on a parallel recorder. Keep the published
        // capacity tied to the allocation that actually exists.
        if (m_Storage->GetSize() == BytesFor(newCapacity))
            m_Capacity = newCapacity;
    }

    void InstanceBuffer::Upload(std::span<const InstanceData> instances)
    {
        if (instances.size() > kMaxInstanceCapacity)
        {
            OLO_CORE_ERROR("InstanceBuffer::Upload: {} instances overflow the u32 buffer size — refusing upload",
                           instances.size());
            m_Count = 0;
            return;
        }
        const u32 count = static_cast<u32>(instances.size());
        EnsureCapacity(count);
        if (count > m_Capacity)
        {
            m_Count = 0;
            return;
        }

        if (count > 0)
        {
            m_Storage->SetData(instances.data(), BytesFor(count), 0);
        }
        m_Count = count;
    }

    void InstanceBuffer::UploadRange(u32 offset, std::span<const InstanceData> data)
    {
        if (offset > m_Capacity || data.size() > m_Capacity - offset)
        {
            OLO_CORE_ERROR("InstanceBuffer::UploadRange: range {}+{} exceeds capacity {} — refusing upload", offset,
                           data.size(), m_Capacity);
            m_Count = 0;
            return;
        }
        const u32 count = static_cast<u32>(data.size());
        if (count == 0)
            return;

        OLO_CORE_ASSERT(offset + count <= m_Capacity,
                        "InstanceBuffer::UploadRange exceeds capacity; call EnsureCapacity() first");

        m_Storage->SetData(data.data(), BytesFor(count), BytesFor(offset));
    }

    void InstanceBuffer::Bind() const
    {
        m_Storage->Bind();
    }

    u32 InstanceBuffer::GetSizeBytes() const
    {
        return BytesFor(m_Capacity);
    }
} // namespace OloEngine
