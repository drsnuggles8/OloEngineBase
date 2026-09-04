#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDeferredReclaim.h"

#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"

#include <exception>
#include <vector>

namespace OloEngine
{
    VulkanDeferredReclaim& VulkanDeferredReclaim::Get()
    {
        static auto* s_Instance = new VulkanDeferredReclaim(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanDeferredReclaim::Push(const Entry& entry) noexcept
    {
        try
        {
            m_Entries.push_back(entry);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanDeferredReclaim: enqueue failed ({}) — the object leaks until process exit", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanDeferredReclaim: enqueue failed (unknown exception) — the object leaks until "
                           "process exit");
        }
    }

    void VulkanDeferredReclaim::Enqueue(VkImage image, VmaAllocation allocation) noexcept
    {
        if (image == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .Image = image, .Allocation = allocation, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkBuffer buffer, VmaAllocation allocation) noexcept
    {
        if (buffer == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .Buffer = buffer, .Allocation = allocation, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkSemaphore semaphore) noexcept
    {
        if (semaphore == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .Semaphore = semaphore, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkPipeline pipeline) noexcept
    {
        if (pipeline == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .Pipeline = pipeline, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkImageView view) noexcept
    {
        if (view == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .View = view, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkQueryPool queryPool) noexcept
    {
        if (queryPool == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .QueryPool = queryPool, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkAccelerationStructureKHR accelerationStructure) noexcept
    {
        if (accelerationStructure == VK_NULL_HANDLE)
        {
            return;
        }
        Push({ .AccelerationStructure = accelerationStructure, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::DestroyEntry(const Entry& entry)
    {
        // Image metadata retires at ACTUAL destroy time — a barrier emitted for
        // a still-enqueued image must keep resolving until the image is gone.
        if (entry.Image != VK_NULL_HANDLE)
        {
            VulkanImageInfoRegistry::Get().Unregister(entry.Image);
            // Same reasoning for the layout rows: retiring them any earlier
            // would strand a barrier mid-flight, and never retiring them grows
            // the map for the process lifetime.
            VulkanImageLayoutTracker::ForgetImageEverywhere(entry.Image);
            // And the per-cascade depth views framebuffers cached over this
            // image — they are enqueued here, i.e. strictly BEFORE the
            // vmaDestroyImage below, so no view outlives its image.
            VulkanFramebuffer::ReleaseCachedDepthViewsForImage(entry.Image);
            // Cached heap slots free HERE, not at enqueue: the generation wait
            // this queue already performed is what makes immediate slot reuse
            // safe (no in-flight frame can still index them).
            VulkanDescriptorSlotCache::Get().ReleaseSlotsForImage(entry.Image);
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            // Shutdown teardown race: the device (and with it the allocator)
            // is already gone. Dropping the entry leaks at process exit, which
            // beats calling into a destroyed allocator.
            OLO_CORE_WARN("VulkanDeferredReclaim: dropping a reclaim entry — VulkanDevice already shut down");
            return;
        }

        if (entry.Image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(device->GetAllocator(), entry.Image, entry.Allocation);
        }
        else if (entry.Buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device->GetAllocator(), entry.Buffer, entry.Allocation);
        }
        else if (entry.Semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device->GetDevice(), entry.Semaphore, nullptr);
        }
        else if (entry.Pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device->GetDevice(), entry.Pipeline, nullptr);
        }
        else if (entry.View != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device->GetDevice(), entry.View, nullptr);
        }
        else if (entry.QueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(device->GetDevice(), entry.QueryPool, nullptr);
        }
        else if (entry.AccelerationStructure != VK_NULL_HANDLE)
        {
            // volk leaves this null on a device without
            // VK_KHR_acceleration_structure, and an entry can only exist if
            // the extension was enabled — but the device may have been torn
            // down and re-created without it between enqueue and drain.
            if (vkDestroyAccelerationStructureKHR != nullptr)
            {
                vkDestroyAccelerationStructureKHR(device->GetDevice(), entry.AccelerationStructure, nullptr);
            }
        }
        else if (entry.Allocation != VK_NULL_HANDLE)
        {
            vmaFreeMemory(device->GetAllocator(), entry.Allocation);
        }
    }

    void VulkanDeferredReclaim::DestroyEntryGuarded(const Entry& entry) noexcept
    {
        try
        {
            DestroyEntry(entry);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanDeferredReclaim: destroying a reclaim entry threw ({}) — dropping it and "
                           "continuing the drain",
                           e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanDeferredReclaim: destroying a reclaim entry threw (unknown exception) — dropping "
                           "it and continuing the drain");
        }
    }

    void VulkanDeferredReclaim::NotifyFrameCompleted() noexcept
    {
        ++m_Generation;

        std::erase_if(m_Entries, [this](const Entry& entry)
                      {
            if (m_Generation - entry.EnqueuedAtGeneration < kFramesInFlight)
            {
                return false;
            }
            DestroyEntryGuarded(entry);
            return true; });
    }

    void VulkanDeferredReclaim::FlushAll() noexcept
    {
        // Caller guarantees vkDeviceWaitIdle has already been done.
        for (const auto& entry : m_Entries)
        {
            DestroyEntryGuarded(entry);
        }
        m_Entries.clear();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
