#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanBufferResources.h"

#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace OloEngine
{
    namespace
    {
        // Kept in sync with the sibling anonymous-namespace copies (trivially
        // small — not worth a shared header).
        void VkCheck(VkResult result, const char* what)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("Vulkan buffers: ") + what + " failed (VkResult " +
                                         std::to_string(static_cast<int>(result)) + ")");
            }
        }

        template<typename T>
        [[nodiscard]] u64 VkHandleToU64(T handle)
        {
            if constexpr (std::is_pointer_v<T>)
            {
                return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
            }
            else
            {
                return static_cast<u64>(handle);
            }
        }

        struct CreatedBuffer
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            void* Mapped = nullptr;
            bool NeedsFlush = false;
        };

        // Create a persistent buffer that PREFERS a host-writable device-local
        // placement (ReBAR/UMA — the common desktop case) and falls back to
        // plain device-local, in which case Mapped stays null and uploads go
        // through a staged one-shot copy.
        [[nodiscard]] CreatedBuffer CreatePersistentBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const char* what)
        {
            auto* device = VulkanDevice::Get();
            OLO_CORE_ASSERT(device != nullptr, "CreatePersistentBuffer requires a live VulkanDevice");
            if (device == nullptr)
            {
                throw std::runtime_error(std::string(what) + ": no live VulkanDevice");
            }

            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = std::max<VkDeviceSize>(size, 1u);
            bufferInfo.usage = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

            CreatedBuffer out;
            VmaAllocationInfo outInfo{};
            VkCheck(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &out.Buffer, &out.Allocation,
                                    &outInfo),
                    what);

            VkMemoryPropertyFlags memProps = 0;
            vmaGetAllocationMemoryProperties(device->GetAllocator(), out.Allocation, &memProps);
            if ((memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
            {
                out.Mapped = outInfo.pMappedData;
                out.NeedsFlush = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
            }
            return out;
        }

    } // namespace

    // =========================================================================
    // VulkanRootObjectRegistry
    // =========================================================================

    VulkanRootObjectRegistry& VulkanRootObjectRegistry::Get()
    {
        static auto* s_Instance = new VulkanRootObjectRegistry(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanRootObjectRegistry::Register(RHI::ResourceHandle handle, VulkanRootObjectKind kind, void* object)
    {
        if (!handle.IsValid() || object == nullptr)
        {
            return;
        }
        m_Entries[Key(handle)] = Entry{ .Kind = kind, .Object = object };
    }

    void VulkanRootObjectRegistry::Unregister(RHI::ResourceHandle handle)
    {
        m_Entries.erase(Key(handle));
    }

    const VulkanRootObjectRegistry::Entry* VulkanRootObjectRegistry::Lookup(RHI::ResourceHandle handle) const
    {
        const auto it = m_Entries.find(Key(handle));
        return it != m_Entries.end() ? &it->second : nullptr;
    }

    // =========================================================================
    // VulkanUniformBuffer
    // =========================================================================

    VulkanUniformBuffer::VulkanUniformBuffer(u32 size, u32 binding)
        : m_AllocatedSize(size), m_Binding(binding)
    {
        OLO_PROFILE_FUNCTION();
        EnsureShadow(size);
        m_RHIHandle.Adopt(RHI::ResourceKind::Buffer, 0u, RHI::Backend::Vulkan);
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::UniformBuffer, this);
        // GL twins occupy their binding point from creation (glBindBufferBase
        // in the ctor); mirror that so a pass that never re-Bind()s still
        // resolves.
        VulkanBindingState::Get().SetUniformBuffer(m_Binding, this);
    }

    VulkanUniformBuffer::~VulkanUniformBuffer()
    {
        VulkanBindingState::Get().ClearBuffer(this);
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
        // m_RHIHandle retires via RAII; the base class frees the shadow.
    }

    void VulkanUniformBuffer::Bind() const
    {
        VulkanBindingState::Get().SetUniformBuffer(m_Binding, const_cast<VulkanUniformBuffer*>(this));
    }

    void VulkanUniformBuffer::EnsureShadow(u32 requiredSize)
    {
        if (requiredSize == 0 || requiredSize <= m_Size)
        {
            return;
        }
        auto* newBuf = new u8[requiredSize]{};
        if (m_LocalData)
        {
            std::memcpy(newBuf, m_LocalData, m_Size);
            delete[] static_cast<u8*>(m_LocalData);
        }
        m_LocalData = newBuf;
        m_Size = requiredSize;
    }

    void VulkanUniformBuffer::SetData(const UniformData& data)
    {
        if (data.data == nullptr || data.size == 0)
        {
            return;
        }
        EnsureShadow(data.offset + data.size);
        // The base convenience SetData(void*,size,offset) already wrote this
        // exact range into m_LocalData before calling us — re-copying is a
        // same-bytes memcpy. Direct SetData(UniformData) callers bypass the
        // base, so this copy is what keeps the shadow authoritative.
        std::memcpy(static_cast<u8*>(m_LocalData) + data.offset, data.data, data.size);
        ++m_DataVersion;
    }

    VkDeviceAddress VulkanUniformBuffer::GetRootDataAddress()
    {
        auto& arena = VulkanFrameArena::Get();
        const u64 generation = arena.GetFrameGeneration();
        if (m_PushedVersion == m_DataVersion && m_PushedFrameGeneration == generation && m_CurrentAddress != 0)
        {
            return m_CurrentAddress;
        }

        if (m_LocalData == nullptr || m_Size == 0)
        {
            return 0;
        }

        // 256 is the spec's maximum minUniformBufferOffsetAlignment — always a
        // legal uniform-block address on every device.
        const auto allocation = arena.Push(m_LocalData, m_Size, 256);
        if (!allocation.IsValid())
        {
            return 0; // arena overflow — caller drops the draw (arena contract)
        }

        m_CurrentAddress = allocation.Gpu;
        m_PushedVersion = m_DataVersion;
        m_PushedFrameGeneration = generation;
        return m_CurrentAddress;
    }

    // =========================================================================
    // VulkanVertexBuffer
    // =========================================================================

    VulkanVertexBuffer::VulkanVertexBuffer(u32 size)
        : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();
        CreateBuffer(nullptr);
    }

    VulkanVertexBuffer::VulkanVertexBuffer(const void* data, u32 size)
        : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();
        CreateBuffer(data);
    }

    VulkanVertexBuffer::~VulkanVertexBuffer()
    {
        try
        {
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanVertexBuffer: release failed ({}) — leaking the buffer until process exit",
                           e.what());
        }
    }

    void VulkanVertexBuffer::CreateBuffer(const void* initialData)
    {
        // STORAGE_BUFFER: §5 vertex pulling reads the stream through the
        // reserved binding-57 SSBO block. VERTEX_BUFFER kept for a potential
        // future fixed-function fallback; it costs nothing.
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        const CreatedBuffer created = CreatePersistentBuffer(m_Size, usage, "vmaCreateBuffer (VulkanVertexBuffer)");
        m_Buffer = created.Buffer;
        m_Allocation = created.Allocation;
        m_Mapped = created.Mapped;
        m_NeedsFlush = created.NeedsFlush;

        auto* device = VulkanDevice::Get();
        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);

        if (initialData != nullptr && m_Size > 0)
        {
            SetData(VertexData{ .data = initialData, .size = m_Size });
        }
    }

    void VulkanVertexBuffer::ReleaseBuffer()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_DeviceAddress = 0;
        }
    }

    void VulkanVertexBuffer::SetData(const VertexData& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.data == nullptr || data.size == 0)
        {
            return;
        }
        if (data.size > m_Size)
        {
            OLO_CORE_ERROR("VulkanVertexBuffer::SetData: {} bytes exceeds the buffer's {} bytes — dropping the upload",
                           data.size, m_Size);
            return;
        }

        // NOTE: mesh data is upload-once at load time. A per-frame rewrite of
        // a buffer the PREVIOUS frame's submission still reads would race it —
        // that streaming shape (Renderer2D batches, video) gets a ring in its
        // own wave; nothing in Waves A/B streams vertex data.
        if (m_Mapped != nullptr)
        {
            std::memcpy(m_Mapped, data.data, data.size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(VulkanDevice::Get()->GetAllocator(), m_Allocation, 0, data.size);
            }
            return;
        }

        VulkanOneShot::UploadToBuffer(m_Buffer, 0, data.data, data.size, "VulkanVertexBuffer::SetData");
    }

    // =========================================================================
    // VulkanIndexBuffer
    // =========================================================================

    VulkanIndexBuffer::VulkanIndexBuffer(const u32* indices, u32 count)
        : m_Count(count)
    {
        OLO_PROFILE_FUNCTION();

        const u64 sizeBytes = static_cast<u64>(count) * sizeof(u32);
        const VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        const CreatedBuffer created =
            CreatePersistentBuffer(sizeBytes, usage, "vmaCreateBuffer (VulkanIndexBuffer)");
        m_Buffer = created.Buffer;
        m_Allocation = created.Allocation;

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);

        if (indices != nullptr && sizeBytes > 0)
        {
            if (created.Mapped != nullptr)
            {
                std::memcpy(created.Mapped, indices, sizeBytes);
                if (created.NeedsFlush)
                {
                    vmaFlushAllocation(VulkanDevice::Get()->GetAllocator(), m_Allocation, 0, sizeBytes);
                }
            }
            else
            {
                VulkanOneShot::UploadToBuffer(m_Buffer, 0, indices, sizeBytes, "VulkanIndexBuffer upload");
            }
        }
    }

    VulkanIndexBuffer::~VulkanIndexBuffer()
    {
        try
        {
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanIndexBuffer: release failed ({}) — leaking the buffer until process exit",
                           e.what());
        }
    }

    void VulkanIndexBuffer::ReleaseBuffer()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
        }
    }

    // =========================================================================
    // VulkanVertexArray
    // =========================================================================

    VulkanVertexArray::VulkanVertexArray()
    {
        m_RHIHandle.Adopt(RHI::ResourceKind::VertexArray, 0u, RHI::Backend::Vulkan);
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::VertexArray, this);
    }

    VulkanVertexArray::~VulkanVertexArray()
    {
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
    }

    void VulkanVertexArray::AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer)
    {
        OLO_CORE_ASSERT(vertexBuffer != nullptr, "VulkanVertexArray::AddVertexBuffer: null buffer");
        m_VertexBuffers.push_back(vertexBuffer);
    }

    void VulkanVertexArray::AddInstanceBuffer(const Ref<VertexBuffer>& vertexBuffer)
    {
        // Stored for layout introspection parity with the GL twin, but the
        // pull path never reads it: per-instance data travels the InstanceData
        // SSBO (glsl-shaders.md §6a). GetPullVertexBuffer() ignores it by
        // taking the FIRST buffer.
        OLO_CORE_ASSERT(vertexBuffer != nullptr, "VulkanVertexArray::AddInstanceBuffer: null buffer");
        m_VertexBuffers.push_back(vertexBuffer);
    }

    void VulkanVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
    {
        m_IndexBuffer = indexBuffer;
    }

    const VulkanVertexBuffer* VulkanVertexArray::GetPullVertexBuffer() const
    {
        return GetPullVertexBuffer(0u);
    }

    const VulkanVertexBuffer* VulkanVertexArray::GetPullVertexBuffer(sizet streamIndex) const
    {
        if (streamIndex >= m_VertexBuffers.size())
        {
            return nullptr;
        }
        // Safe: on the Vulkan backend every VertexBuffer the factory hands out
        // is a VulkanVertexBuffer.
        return static_cast<const VulkanVertexBuffer*>(m_VertexBuffers[streamIndex].Raw());
    }

    const VulkanIndexBuffer* VulkanVertexArray::GetVulkanIndexBuffer() const
    {
        return static_cast<const VulkanIndexBuffer*>(m_IndexBuffer.Raw());
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
