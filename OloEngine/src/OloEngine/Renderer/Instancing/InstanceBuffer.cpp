#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    namespace
    {
        constexpr u32 kMinInitialCapacity = 1;

        constexpr u32 BytesFor(u32 instanceCount)
        {
            return instanceCount * static_cast<u32>(sizeof(InstanceData));
        }
    } // namespace

    InstanceBuffer::InstanceBuffer(u32 initialCapacity)
        : m_Capacity(std::max(initialCapacity, kMinInitialCapacity))
    {
        m_Storage = StorageBuffer::Create(BytesFor(m_Capacity),
                                          ShaderBindingLayout::SSBO_INSTANCE_DATA,
                                          StorageBufferUsage::DynamicDraw);
    }

    void InstanceBuffer::EnsureCapacity(u32 requiredCount)
    {
        if (requiredCount <= m_Capacity)
            return;

        // Grow geometrically (doubling); fall back to exact size if doubling
        // is still insufficient (would only happen for unusually large
        // requested counts compared to the current capacity).
        u32 newCapacity = m_Capacity * 2;
        if (newCapacity < requiredCount)
            newCapacity = requiredCount;

        m_Storage->Resize(BytesFor(newCapacity));
        m_Capacity = newCapacity;
    }

    void InstanceBuffer::Upload(std::span<const InstanceData> instances)
    {
        const u32 count = static_cast<u32>(instances.size());
        EnsureCapacity(count);

        if (count > 0)
        {
            m_Storage->SetData(instances.data(), BytesFor(count), 0);
        }
        m_Count = count;
    }

    void InstanceBuffer::UploadRange(u32 offset, std::span<const InstanceData> data)
    {
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
