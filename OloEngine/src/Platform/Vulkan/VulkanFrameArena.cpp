#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <cstring>

namespace OloEngine
{
    VulkanFrameArena& VulkanFrameArena::Get()
    {
        static auto* s_Instance = new VulkanFrameArena(); // deliberately leaked
        return *s_Instance;
    }

    bool VulkanFrameArena::EnsureBuffers()
    {
        if (m_Slots[0].Buffer != VK_NULL_HANDLE)
        {
            return true;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("VulkanFrameArena: Allocate with no live VulkanDevice — root-data "
                              "allocations will fail until a device is up");
            }
            return false;
        }

        for (Slot& slot : m_Slots)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = kSlotCapacityBytes;
            // The one buffer serves every root-data consumer: UBO-shaped and
            // SSBO-shaped block mappings (INDIRECT_ADDRESS reads), GPU-written
            // indirect args (§4.2), and plain device-address dereferences.
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            // Persistently mapped, sequential-write: the CPU writes root
            // structs directly into GPU-visible memory. DEVICE_LOCAL is
            // preferred (ReBAR/UMA — the ADR 0010 driver floor makes this the
            // expected placement); VMA falls back to plain host-visible when
            // the BAR window can't take it, which is slower but correct.
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

            VmaAllocationInfo resultInfo{};
            const VkResult result = vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo,
                                                    &slot.Buffer, &slot.Allocation, &resultInfo);
            if (result != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanFrameArena: vmaCreateBuffer failed (VkResult {}) — releasing partial slots",
                               static_cast<int>(result));
                ReleaseBuffers();
                return false;
            }
            slot.Mapped = resultInfo.pMappedData;

            VkBufferDeviceAddressInfo addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addressInfo.buffer = slot.Buffer;
            slot.BaseAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
            slot.Cursor = 0;

            OLO_CORE_ASSERT(slot.Mapped != nullptr, "VulkanFrameArena: VMA_ALLOCATION_CREATE_MAPPED_BIT yielded no mapping");
            OLO_CORE_ASSERT(slot.BaseAddress != 0, "VulkanFrameArena: vkGetBufferDeviceAddress returned 0");
        }
        return true;
    }

    void VulkanFrameArena::BeginFrame(u32 frameSlot)
    {
        OLO_CORE_ASSERT(frameSlot < kFramesInFlight, "VulkanFrameArena: frame slot out of range");
        m_CurrentSlot = frameSlot % kFramesInFlight;
        m_Slots[m_CurrentSlot].Cursor = 0;
        m_AllocationsThisFrame = 0;
    }

    VulkanFrameArenaAllocation VulkanFrameArena::Allocate(u64 sizeBytes, u64 alignment)
    {
        OLO_CORE_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0,
                        "VulkanFrameArena: alignment must be a power of two");
        if (!EnsureBuffers() || sizeBytes == 0)
        {
            return {};
        }

        Slot& slot = m_Slots[m_CurrentSlot];
        const u64 aligned = (slot.Cursor + (alignment - 1)) & ~(alignment - 1);
        if (aligned + sizeBytes > kSlotCapacityBytes)
        {
            ++m_OverflowCount;
            if (!m_OverflowWarned)
            {
                m_OverflowWarned = true;
                OLO_CORE_ERROR("VulkanFrameArena: slot {} overflow ({} B requested at cursor {} of {} B) — "
                               "root-data allocations are being DROPPED this frame",
                               m_CurrentSlot, sizeBytes, aligned, kSlotCapacityBytes);
            }
            return {};
        }

        slot.Cursor = aligned + sizeBytes;
        ++m_AllocationsThisFrame;
        return {
            .Cpu = static_cast<u8*>(slot.Mapped) + aligned,
            .Gpu = slot.BaseAddress + aligned,
            .Offset = aligned,
        };
    }

    VulkanFrameArenaAllocation VulkanFrameArena::Push(const void* data, u64 sizeBytes, u64 alignment)
    {
        VulkanFrameArenaAllocation allocation = Allocate(sizeBytes, alignment);
        if (allocation.IsValid() && data != nullptr)
        {
            std::memcpy(allocation.Cpu, data, sizeBytes);
        }
        return allocation;
    }

    void VulkanFrameArena::ReleaseBuffers()
    {
        for (Slot& slot : m_Slots)
        {
            if (slot.Buffer != VK_NULL_HANDLE || slot.Allocation != VK_NULL_HANDLE)
            {
                VulkanDeferredReclaim::Get().Enqueue(slot.Buffer, slot.Allocation);
            }
            slot = {};
        }
        m_CurrentSlot = 0;
        m_AllocationsThisFrame = 0;
    }

    u64 VulkanFrameArena::GetCurrentSlotUsedBytes() const
    {
        return m_Slots[m_CurrentSlot].Cursor;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
