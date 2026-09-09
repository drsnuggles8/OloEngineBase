#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanQueueSelection.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"

#include <bit>

namespace OloEngine::VulkanQueueSelection
{
    std::string_view Describe(const AsyncComputeUnavailableReason reason)
    {
        switch (reason)
        {
            case AsyncComputeUnavailableReason::None:
                return "available";
            case AsyncComputeUnavailableReason::NoDeviceQueueFamilies:
                return "the driver reported no queue families";
            case AsyncComputeUnavailableReason::NoComputeOnlyFamily:
                return "no compute-capable queue family without graphics";
            case AsyncComputeUnavailableReason::FamilyHasNoQueues:
                return "the compute-only queue family exposes no queues";
            case AsyncComputeUnavailableReason::FamilyHasNoTimestamps:
                return "the compute-only queue family cannot write GPU timestamps";
            case AsyncComputeUnavailableReason::CommandPoolCreationFailed:
                return "the compute queue's command pool could not be created";
            case AsyncComputeUnavailableReason::DisabledByLever:
                return "disabled by OLO_VK_ASYNC_COMPUTE=0";
            case AsyncComputeUnavailableReason::NoDevice:
                return "no Vulkan device";
        }
        return "unknown";
    }

    AsyncComputeSelection SelectAsyncComputeFamily(const std::span<const VkQueueFamilyProperties> families,
                                                   const u32 graphicsFamily)
    {
        AsyncComputeSelection selection{};
        if (families.empty())
        {
            selection.Reason = AsyncComputeUnavailableReason::NoDeviceQueueFamilies;
            return selection;
        }

        // Two distinct negative answers, and they are worth telling apart: a
        // device with no compute-only family at all is ordinary hardware the
        // engine must serve, while a compute-only family that exposes zero
        // queues is a driver oddity worth naming in the log rather than
        // reporting as "this GPU has no async compute".
        selection.Reason = AsyncComputeUnavailableReason::NoComputeOnlyFamily;

        u32 bestBitCount = 0;
        for (u32 index = 0; index < static_cast<u32>(families.size()); ++index)
        {
            if (index == graphicsFamily)
                continue;

            const VkQueueFlags flags = families[index].queueFlags;
            if ((flags & VK_QUEUE_COMPUTE_BIT) == 0)
                continue;
            // The engine's own graphics family is excluded above by index, but
            // a device may expose several graphics families; none of them is a
            // separate hardware pipe for this purpose.
            if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0)
                continue;

            if (families[index].queueCount == 0)
            {
                // Remember the oddity, but keep looking — another compute-only
                // family may be usable.
                if (!selection.Found)
                    selection.Reason = AsyncComputeUnavailableReason::FamilyHasNoQueues;
                continue;
            }
            if (families[index].timestampValidBits == 0)
            {
                // Same shape as the queueCount case: a specific, nameable
                // reason rather than "this GPU has no async compute". See the
                // header for why timestamps are a hard requirement.
                if (!selection.Found)
                    selection.Reason = AsyncComputeUnavailableReason::FamilyHasNoTimestamps;
                continue;
            }

            // Fewest capability bits wins: the most dedicated pipe. Ties go to
            // the lowest index, which `>=` below preserves because the first
            // candidate is accepted and later equals are rejected.
            const auto bitCount = static_cast<u32>(std::popcount(static_cast<u32>(flags)));
            if (selection.Found && bitCount >= bestBitCount)
                continue;

            selection.Found = true;
            selection.FamilyIndex = index;
            selection.TimestampValidBits = families[index].timestampValidBits;
            selection.Reason = AsyncComputeUnavailableReason::None;
            bestBitCount = bitCount;
        }

        return selection;
    }

    CrossQueueBufferSharing::CrossQueueBufferSharing()
    {
        const VulkanDevice* device = VulkanDevice::Get();
        if (device == nullptr || !device->HasAsyncComputeQueue())
        {
            return;
        }
        m_Families[0] = device->GetQueueFamily();
        m_Families[1] = device->GetAsyncComputeQueueFamily();
        // The selection guarantees the two differ, but a create-info naming the
        // same family twice is invalid usage
        // (VUID-VkBufferCreateInfo-sharingMode-01419), so do not take that on
        // trust at the one place it would become a driver's problem.
        m_Count = (m_Families[0] != m_Families[1]) ? 2u : 0u;
    }

    void CrossQueueBufferSharing::ApplyTo(VkBufferCreateInfo& info) const
    {
        if (m_Count < 2u)
        {
            // Single-queue backend: leave whatever the caller set (EXCLUSIVE).
            return;
        }
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = m_Count;
        info.pQueueFamilyIndices = m_Families;
    }
} // namespace OloEngine::VulkanQueueSelection

#endif // OLO_WITH_VULKAN
