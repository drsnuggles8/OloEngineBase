#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanSamplerHeap.h"

#include <shared_mutex>
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <bit>
#include <mutex>

namespace OloEngine
{
    namespace
    {
        // The same rule as the pipeline builder's retired sampler hash: every
        // field vkCreateSampler consumes is key material, floats enter via
        // bit_cast. A state differing only in an unhashed field must not
        // collide onto another state's slot.
        [[nodiscard]] u64 HashCombine(const u64 seed, const u64 value)
        {
            return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        }

        [[nodiscard]] u64 HashSamplerInfo(const VkSamplerCreateInfo& info)
        {
            u64 hash = 0;
            hash = HashCombine(hash, static_cast<u64>(info.flags));
            hash = HashCombine(hash, static_cast<u64>(info.magFilter));
            hash = HashCombine(hash, static_cast<u64>(info.minFilter));
            hash = HashCombine(hash, static_cast<u64>(info.mipmapMode));
            hash = HashCombine(hash, static_cast<u64>(info.addressModeU));
            hash = HashCombine(hash, static_cast<u64>(info.addressModeV));
            hash = HashCombine(hash, static_cast<u64>(info.addressModeW));
            hash = HashCombine(hash, std::bit_cast<u32>(info.mipLodBias));
            hash = HashCombine(hash, static_cast<u64>(info.anisotropyEnable));
            hash = HashCombine(hash, std::bit_cast<u32>(info.maxAnisotropy));
            hash = HashCombine(hash, static_cast<u64>(info.compareEnable));
            hash = HashCombine(hash, static_cast<u64>(info.compareOp));
            hash = HashCombine(hash, std::bit_cast<u32>(info.minLod));
            hash = HashCombine(hash, std::bit_cast<u32>(info.maxLod));
            hash = HashCombine(hash, static_cast<u64>(info.borderColor));
            hash = HashCombine(hash, static_cast<u64>(info.unnormalizedCoordinates));
            return hash == 0 ? 1 : hash;
        }
    } // namespace

    VulkanSamplerHeap& VulkanSamplerHeap::Get()
    {
        static auto* s_Instance = new VulkanSamplerHeap(); // deliberately leaked
        return *s_Instance;
    }

    VkSamplerCreateInfo VulkanSamplerHeap::DefaultSamplerInfo()
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        return info;
    }

    bool VulkanSamplerHeap::EnsureCreated()
    {
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        return EnsureCreatedLocked();
    }

    bool VulkanSamplerHeap::EnsureCreatedLocked()
    {
        // Device liveness FIRST — the VulkanResourceHeap rule: cached state
        // must never outrank "is there a device, and is it mine?".
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
            OLO_CORE_WARN("VulkanSamplerHeap: cached heap belongs to a dead device (missed Release()) — "
                          "dropping stale state and recreating");
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_BaseAddress = 0;
            m_NextSlot = 0;
            m_SlotByHash.clear();
        }

        VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{};
        heapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &heapProps;
        vkGetPhysicalDeviceProperties2(device->GetPhysicalDevice(), &props);

        const VkDeviceSize align = std::max<VkDeviceSize>(heapProps.samplerDescriptorAlignment, 1);
        m_DescriptorStride = ((heapProps.samplerDescriptorSize + align - 1) / align) * align;
        m_ReservedRangeSize = heapProps.minSamplerHeapReservedRange;
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

        // vkCmdBindSamplerHeapEXT requires the range address be a multiple of
        // samplerHeapAlignment — the resource heap's VUID-11235 lesson.
        const VkDeviceSize heapAlignment = std::max<VkDeviceSize>(heapProps.samplerHeapAlignment, 1);

        VmaAllocationInfo resultInfo{};
        if (vmaCreateBufferWithAlignment(device->GetAllocator(), &bufferInfo, &allocInfo, heapAlignment, &m_Buffer,
                                         &m_Allocation, &resultInfo) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanSamplerHeap: heap buffer creation failed ({} B)", m_TotalSize);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return false;
        }
        m_Mapped = resultInfo.pMappedData;

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_BaseAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        if (m_Mapped == nullptr || m_BaseAddress == 0)
        {
            OLO_CORE_ERROR("VulkanSamplerHeap: heap came up unusable (mapped={}, address={:#x}) — releasing",
                           m_Mapped != nullptr, m_BaseAddress);
            vmaDestroyBuffer(device->GetAllocator(), m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_BaseAddress = 0;
            return false;
        }
        m_OwningDevice = device->GetDevice();

        // Query the placement's coherency once: descriptor writes into a
        // NON-coherent mapping must be flushed or the GPU can read stale
        // bytes (the VulkanFrameArena rule).
        VkMemoryPropertyFlags memProps = 0;
        vmaGetAllocationMemoryProperties(device->GetAllocator(), m_Allocation, &memProps);
        m_NeedsFlush = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;

        // Slot 0 = the default sampler, unconditionally: an unstaged binding
        // must sample exactly as the old embedded default did.
        const VkSamplerCreateInfo defaultInfo = DefaultSamplerInfo();
        m_NextSlot = 0;
        m_SlotByHash.clear();
        if (!WriteSampler(DefaultSlot, defaultInfo))
        {
            OLO_CORE_ERROR("VulkanSamplerHeap: default sampler write failed — releasing");
            ReleaseLocked();
            return false;
        }
        m_SlotByHash.emplace(HashSamplerInfo(defaultInfo), DefaultSlot);
        m_NextSlot = 1;

        OLO_CORE_INFO("[Vulkan] Sampler heap created: {} slots x {} B stride (+{} B reserved)", kSlotCapacity,
                      m_DescriptorStride, m_SlotRegionOffset);
        return true;
    }

    u32 VulkanSamplerHeap::GetOrCreateSlot(const VkSamplerCreateInfo& info)
    {
        // Hash outside any lock. The hit path — every BindTexture on every
        // item (#806) — only shares the lock; a miss takes it exclusively so
        // the lookup, the bump and the descriptor write are one step and two
        // threads asking for the same new state cannot both take a slot.
        const u64 hash = HashSamplerInfo(info);
        {
            std::shared_lock readLock(m_Mutex);
            if (m_Buffer != VK_NULL_HANDLE)
            {
                if (const auto it = m_SlotByHash.find(hash); it != m_SlotByHash.end())
                {
                    return it->second;
                }
            }
        }
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        if (!EnsureCreatedLocked())
        {
            return DefaultSlot;
        }
        if (const auto it = m_SlotByHash.find(hash); it != m_SlotByHash.end())
        {
            return it->second;
        }
        if (m_NextSlot >= kSlotCapacity)
        {
            OLO_CORE_ERROR("VulkanSamplerHeap: out of slots ({}) — falling back to the default sampler",
                           kSlotCapacity);
            return DefaultSlot;
        }
        const u32 slot = m_NextSlot;
        if (!WriteSampler(slot, info))
        {
            // Degrade like the other failure arms: DefaultSlot samples
            // linear/clamp (the pre-heap behaviour), whereas InvalidSlot
            // (0xFFFFFFFF) copied into root data indexes past the heap.
            return DefaultSlot;
        }
        ++m_NextSlot;
        m_SlotByHash.emplace(hash, slot);
        return slot;
    }

    bool VulkanSamplerHeap::WriteSampler(const u32 slot, const VkSamplerCreateInfo& info)
    {
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Mapped == nullptr || slot >= kSlotCapacity)
        {
            return false;
        }
        VkHostAddressRangeEXT dst{};
        dst.address = static_cast<u8*>(m_Mapped) + m_SlotRegionOffset + slot * m_DescriptorStride;
        dst.size = static_cast<sizet>(m_DescriptorStride);

        // Descriptors are produced from the CREATE INFO — no VkSampler object
        // ever exists on this backend, mirroring how image descriptors come
        // from VkImageViewCreateInfo with no VkImageView.
        const VkResult result = vkWriteSamplerDescriptorsEXT(device->GetDevice(), 1, &info, &dst);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanSamplerHeap: vkWriteSamplerDescriptorsEXT failed (slot {}, VkResult {})", slot,
                           static_cast<int>(result));
            return false;
        }
        if (m_NeedsFlush)
        {
            vmaFlushAllocation(device->GetAllocator(), m_Allocation,
                               m_SlotRegionOffset + slot * m_DescriptorStride, m_DescriptorStride);
        }
        return true;
    }

    void VulkanSamplerHeap::CmdBind(VkCommandBuffer cmd)
    {
        // The creation half is the only mutation this can trigger, and
        // EnsureCreated takes the lock for it. The fields read below are set
        // once at creation and cleared only by Release (a render-thread
        // teardown path), so they need no lock — the header's contract.
        if (!EnsureCreated() || cmd == VK_NULL_HANDLE)
        {
            return;
        }
        VkBindHeapInfoEXT bindInfo{};
        bindInfo.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
        bindInfo.heapRange = { .address = m_BaseAddress, .size = m_TotalSize };
        bindInfo.reservedRangeOffset = 0;
        bindInfo.reservedRangeSize = m_ReservedRangeSize;
        vkCmdBindSamplerHeapEXT(cmd, &bindInfo);
    }

    void VulkanSamplerHeap::Release()
    {
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        ReleaseLocked();
    }

    void VulkanSamplerHeap::ReleaseLocked()
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
        m_SlotByHash.clear();
        m_OwningDevice = VK_NULL_HANDLE;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
