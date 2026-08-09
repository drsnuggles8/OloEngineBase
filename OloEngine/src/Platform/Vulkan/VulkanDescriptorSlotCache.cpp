#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"

#include "Platform/Vulkan/VulkanResourceHeap.h"

namespace OloEngine
{
    VulkanDescriptorSlotCache& VulkanDescriptorSlotCache::Get()
    {
        static auto* s_Instance = new VulkanDescriptorSlotCache(); // deliberately leaked
        return *s_Instance;
    }

    u64 VulkanDescriptorSlotCache::HashKey(VkImage image, const VkImageViewCreateInfo& viewInfo, VkDescriptorType type,
                                           VkImageLayout layout)
    {
        // FNV-1a over the fields that make two views distinct descriptors.
        // Component swizzles are deliberately omitted — nothing in the engine
        // authors swizzled views; if one ever does, add the four components
        // here (a missed field folds two DIFFERENT views into one slot, which
        // renders wrong, so the field list is the contract).
        constexpr u64 kOffset = 1469598103934665603ull;
        constexpr u64 kPrime = 1099511628211ull;
        u64 hash = kOffset;
        const auto mix = [&hash](u64 value)
        {
            hash ^= value;
            hash *= kPrime;
        };
        mix(reinterpret_cast<std::uintptr_t>(image));
        mix(static_cast<u64>(viewInfo.viewType));
        mix(static_cast<u64>(viewInfo.format));
        mix(static_cast<u64>(viewInfo.subresourceRange.aspectMask));
        mix(static_cast<u64>(viewInfo.subresourceRange.baseMipLevel));
        mix(static_cast<u64>(viewInfo.subresourceRange.levelCount));
        mix(static_cast<u64>(viewInfo.subresourceRange.baseArrayLayer));
        mix(static_cast<u64>(viewInfo.subresourceRange.layerCount));
        mix(static_cast<u64>(type));
        mix(static_cast<u64>(layout));
        return hash;
    }

    u32 VulkanDescriptorSlotCache::AcquireSlot(VkImage image, const VkImageViewCreateInfo& viewInfo,
                                               VkDescriptorType type, VkImageLayout layout)
    {
        auto& heap = VulkanResourceHeap::Get();
        if (image == VK_NULL_HANDLE)
        {
            return VulkanResourceHeap::InvalidSlot;
        }

        const u64 key = HashKey(image, viewInfo, type, layout);
        if (const auto it = m_SlotByKey.find(key); it != m_SlotByKey.end())
        {
            return it->second;
        }

        u32 slot = VulkanResourceHeap::InvalidSlot;
        if (!m_FreeSlots.empty())
        {
            slot = m_FreeSlots.back();
            m_FreeSlots.pop_back();
        }
        else
        {
            slot = heap.AllocateSlot();
        }
        if (slot == VulkanResourceHeap::InvalidSlot)
        {
            return VulkanResourceHeap::InvalidSlot;
        }

        const bool written = (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                                 ? heap.WriteStorageImage(slot, viewInfo, layout)
                                 : heap.WriteSampledImage(slot, viewInfo, layout);
        if (!written)
        {
            m_FreeSlots.push_back(slot);
            return VulkanResourceHeap::InvalidSlot;
        }

        m_SlotByKey[key] = slot;
        m_KeysByImage[image].push_back(key);
        return slot;
    }

    void VulkanDescriptorSlotCache::ReleaseSlotsForImage(VkImage image)
    {
        const auto it = m_KeysByImage.find(image);
        if (it == m_KeysByImage.end())
        {
            return;
        }
        for (const u64 key : it->second)
        {
            if (const auto slotIt = m_SlotByKey.find(key); slotIt != m_SlotByKey.end())
            {
                m_FreeSlots.push_back(slotIt->second);
                m_SlotByKey.erase(slotIt);
            }
        }
        m_KeysByImage.erase(it);
    }

    void VulkanDescriptorSlotCache::Reset()
    {
        m_SlotByKey.clear();
        m_KeysByImage.clear();
        m_FreeSlots.clear();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
