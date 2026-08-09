#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanResourceHeap.h"
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
        if (m_Buffer != VK_NULL_HANDLE)
        {
            return true;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
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

        VmaAllocationInfo resultInfo{};
        if (vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &m_Buffer, &m_Allocation, &resultInfo) !=
            VK_SUCCESS)
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

        OLO_CORE_ASSERT(m_Mapped != nullptr, "VulkanResourceHeap: mapped pointer missing");
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

    bool VulkanResourceHeap::WriteSampledImage(u32 slot, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
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
        resourceInfo.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        resourceInfo.data.pImage = &imageInfo;

        VkHostAddressRangeEXT dst{};
        dst.address = static_cast<u8*>(m_Mapped) + m_SlotRegionOffset + slot * m_DescriptorStride;
        dst.size = static_cast<sizet>(m_DescriptorStride);

        const VkResult result = vkWriteResourceDescriptorsEXT(device->GetDevice(), 1, &resourceInfo, &dst);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanResourceHeap: vkWriteResourceDescriptorsEXT failed (slot {}, VkResult {})", slot,
                           static_cast<int>(result));
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
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
