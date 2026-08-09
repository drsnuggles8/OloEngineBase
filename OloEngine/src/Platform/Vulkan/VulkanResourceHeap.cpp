#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanDescriptorHeapBackend.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

namespace OloEngine
{
    VulkanResourceHeap& VulkanResourceHeap::Get()
    {
        static auto* s_Instance = new VulkanResourceHeap(); // deliberately leaked
        return *s_Instance;
    }

    bool VulkanResourceHeap::EnsureCreated()
    {
        // Device liveness FIRST: this is a leaked process-wide singleton, and
        // cached state must never outrank the question "is there a device,
        // and is it the one that created my buffer?" (the caught failure:
        // teardown without Release() left a dead device's handles cached, and
        // the next call dereferenced a null device).
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }
        if (m_Buffer != VK_NULL_HANDLE)
        {
            if (m_OwningDevice == device->GetDevice())
            {
                return true;
            }
            // A different device is live: the cached buffer belongs to a
            // device that shut down without our Release(). Its objects died
            // with that device — freeing them through the NEW allocator would
            // be UB, so drop the state and warn (the leak already happened at
            // the missed Release()).
            OLO_CORE_WARN("VulkanResourceHeap: cached heap belongs to a dead device (missed Release()) — "
                          "dropping stale state and recreating");
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_BaseAddress = 0;
            m_NextSlot = 0;
        }

        // Device heap properties decide every size in the layout.
        VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{};
        heapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &heapProps;
        vkGetPhysicalDeviceProperties2(device->GetPhysicalDevice(), &props);

        // One uniform stride for the slot array: the image descriptor size
        // rounded to its alignment (buffer descriptors are not stored here —
        // §4 addresses buffers by device address, so the resource heap holds
        // only image descriptors, exactly as §1.2a decided).
        const VkDeviceSize align = std::max<VkDeviceSize>(heapProps.imageDescriptorAlignment, 1);
        m_DescriptorStride = ((heapProps.imageDescriptorSize + align - 1) / align) * align;
        m_ReservedRangeSize = heapProps.minResourceHeapReservedRange;
        m_SlotRegionOffset = ((m_ReservedRangeSize + align - 1) / align) * align;
        m_TotalSize = m_SlotRegionOffset + m_DescriptorStride * kSlotCapacity;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = m_TotalSize;
        bufferInfo.usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        // vkCmdBindResourceHeapEXT requires heapRange.address to be a
        // multiple of resourceHeapAlignment (VUID-11235) — a plain
        // vmaCreateBuffer only honours the buffer's own memory requirement,
        // and a suballocation that lands 16-aligned binds a misaligned heap.
        // Latent through Phase 6 (the pilot's allocation happened to land
        // aligned); surfaced the moment the allocation order changed.
        const VkDeviceSize heapAlignment = std::max<VkDeviceSize>(heapProps.resourceHeapAlignment, 1);

        VmaAllocationInfo resultInfo{};
        if (vmaCreateBufferWithAlignment(device->GetAllocator(), &bufferInfo, &allocInfo, heapAlignment, &m_Buffer,
                                         &m_Allocation, &resultInfo) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanResourceHeap: heap buffer creation failed ({} B)", m_TotalSize);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return false;
        }
        m_Mapped = resultInfo.pMappedData;

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_BaseAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        // Real failure checks, not assert-only (asserts compile out in
        // release): a null mapping would flow into pointer arithmetic in
        // WriteSampledImage, a zero address into CmdBind's heap range.
        if (m_Mapped == nullptr || m_BaseAddress == 0)
        {
            OLO_CORE_ERROR("VulkanResourceHeap: heap came up unusable (mapped={}, address={:#x}) — releasing",
                           m_Mapped != nullptr, m_BaseAddress);
            vmaDestroyBuffer(device->GetAllocator(), m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_BaseAddress = 0;
            return false;
        }
        m_OwningDevice = device->GetDevice();
        OLO_CORE_INFO("[Vulkan] Resource heap created: {} slots x {} B stride (+{} B reserved)", kSlotCapacity,
                      m_DescriptorStride, m_SlotRegionOffset);
        return true;
    }

    u32 VulkanResourceHeap::AllocateSlot()
    {
        if (!EnsureCreated())
        {
            return InvalidSlot;
        }
        if (m_NextSlot >= kSlotCapacity)
        {
            OLO_CORE_ERROR("VulkanResourceHeap: out of slots ({})", kSlotCapacity);
            return InvalidSlot;
        }
        return m_NextSlot++;
    }

    bool VulkanResourceHeap::ReserveSlotRange(const u32 count)
    {
        if (!EnsureCreated() || count > kSlotCapacity)
        {
            return false;
        }
        if (m_ReservedSlots == count)
        {
            return true; // idempotent re-install
        }
        if (m_NextSlot > m_ReservedSlots)
        {
            // Dynamic allocation already ran past the previous reservation —
            // a retroactive widen would alias live slots.
            if (count > m_NextSlot)
            {
                OLO_CORE_ERROR("VulkanResourceHeap: reserve of {} slots after dynamic allocation reached {} — "
                               "install the engine heap before any draw-path slot use",
                               count, m_NextSlot);
                return false;
            }
        }
        m_ReservedSlots = count;
        m_NextSlot = std::max(m_NextSlot, count);
        return true;
    }

    bool VulkanResourceHeap::WriteSampledImage(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
    {
        return WriteImageDescriptor(slot, viewInfo, layout, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }

    bool VulkanResourceHeap::WriteStorageImage(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
    {
        return WriteImageDescriptor(slot, viewInfo, layout, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }

    bool VulkanResourceHeap::WriteImageDescriptor(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout,
                                                  VkDescriptorType type)
    {
        if (!EnsureCreated() || slot >= kSlotCapacity || viewInfo.image == VK_NULL_HANDLE)
        {
            return false;
        }
        auto* device = VulkanDevice::Get();

        // The descriptor is produced from the view DESCRIPTION — under
        // VK_EXT_descriptor_heap no VkImageView object exists for sampled use
        // (view objects remain only where dynamic rendering wants attachment
        // views). One less object lifetime to track per texture.
        VkImageDescriptorInfoEXT imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT;
        imageInfo.pView = &viewInfo;
        imageInfo.layout = layout;

        VkResourceDescriptorInfoEXT resourceInfo{};
        resourceInfo.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT;
        resourceInfo.type = type;
        resourceInfo.data.pImage = &imageInfo;

        VkHostAddressRangeEXT dst{};
        dst.address = static_cast<u8*>(m_Mapped) + m_SlotRegionOffset + slot * m_DescriptorStride;
        dst.size = static_cast<sizet>(m_DescriptorStride);

        const VkResult result = vkWriteResourceDescriptorsEXT(device->GetDevice(), 1, &resourceInfo, &dst);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanResourceHeap: vkWriteResourceDescriptorsEXT failed (slot {}, type {}, VkResult {})",
                           slot, static_cast<int>(type), static_cast<int>(result));
            return false;
        }
        return true;
    }

    void VulkanResourceHeap::CmdBind(VkCommandBuffer cmd)
    {
        if (!EnsureCreated() || cmd == VK_NULL_HANDLE)
        {
            return;
        }
        VkBindHeapInfoEXT bindInfo{};
        bindInfo.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
        bindInfo.heapRange = { .address = m_BaseAddress, .size = m_TotalSize };
        bindInfo.reservedRangeOffset = 0;
        bindInfo.reservedRangeSize = m_ReservedRangeSize;
        vkCmdBindResourceHeapEXT(cmd, &bindInfo);
    }

    void VulkanResourceHeap::Release()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
        }
        m_Buffer = VK_NULL_HANDLE;
        m_Allocation = VK_NULL_HANDLE;
        m_Mapped = nullptr;
        m_BaseAddress = 0;
        m_NextSlot = 0;
        m_ReservedSlots = 0;
        m_OwningDevice = VK_NULL_HANDLE;
        // Slot indices are meaningless once the heap they index is gone —
        // the amendment (33) family: state must not outlive what gives it
        // meaning. The backend's null images die with the heap too (the
        // descriptors pointing at them just did).
        VulkanDescriptorSlotCache::Get().Reset();
        VulkanDescriptorHeapBackend::Get().ReleaseDeviceObjects();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
