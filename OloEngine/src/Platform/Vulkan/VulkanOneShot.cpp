#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanOneShot.h"

#include <cstring>

namespace OloEngine
{
    namespace VulkanOneShot
    {
        bool Submit(const char* what, const std::function<void(VkCommandBuffer)>& record)
        {
            OLO_PROFILE_FUNCTION();

            auto* device = VulkanDevice::Get();
            if (device == nullptr)
            {
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): no live VulkanDevice", what);
                return false;
            }

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = device->GetCommandPool();
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1u;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(device->GetDevice(), &allocInfo, &cmd) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): vkAllocateCommandBuffers failed", what);
                return false;
            }

            // From here on every failure path must free `cmd`.
            const auto freeCmd = [&]()
            {
                vkFreeCommandBuffers(device->GetDevice(), device->GetCommandPool(), 1u, &cmd);
            };

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): vkBeginCommandBuffer failed", what);
                freeCmd();
                return false;
            }

            record(cmd);

            if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): vkEndCommandBuffer failed", what);
                freeCmd();
                return false;
            }

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            if (vkCreateFence(device->GetDevice(), &fenceInfo, nullptr, &fence) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): vkCreateFence failed", what);
                freeCmd();
                return false;
            }

            VkCommandBufferSubmitInfo cmdInfo{};
            cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmdInfo.commandBuffer = cmd;

            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1u;
            submit.pCommandBufferInfos = &cmdInfo;

            bool ok = vkQueueSubmit2(device->GetQueue(), 1u, &submit, fence) == VK_SUCCESS;
            if (!ok)
            {
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): vkQueueSubmit2 failed", what);
            }
            else
            {
                // Blocking wait is the point (see the header): load-time GL
                // uploads were synchronous, and callers destroy staging
                // buffers immediately after this returns.
                constexpr u64 kTimeoutNs = 10'000'000'000ull; // 10 s — an upload that slow is a hang
                const VkResult waited = vkWaitForFences(device->GetDevice(), 1u, &fence, VK_TRUE, kTimeoutNs);
                if (waited != VK_SUCCESS)
                {
                    OLO_CORE_ERROR("VulkanOneShot::Submit({}): fence wait failed (VkResult {})", what,
                                   static_cast<int>(waited));
                    ok = false;
                }
            }

            vkDestroyFence(device->GetDevice(), fence, nullptr);
            freeCmd();
            return ok;
        }

        bool UploadToBuffer(VkBuffer dst, u64 dstOffset, const void* data, u64 sizeBytes, const char* what)
        {
            auto* device = VulkanDevice::Get();
            if (device == nullptr || dst == VK_NULL_HANDLE || data == nullptr || sizeBytes == 0)
            {
                return false;
            }

            VkBufferCreateInfo stagingInfo{};
            stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            stagingInfo.size = sizeBytes;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo stagingAlloc{};
            stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
            stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation stagingAllocation = VK_NULL_HANDLE;
            VmaAllocationInfo stagingOut{};
            if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                                &stagingOut) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("{}: staging buffer allocation failed ({} bytes)", what, sizeBytes);
                return false;
            }

            std::memcpy(stagingOut.pMappedData, data, sizeBytes);
            vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, sizeBytes);

            const bool ok = Submit(what,
                                   [&](VkCommandBuffer cmd)
                                   {
                                       VkBufferCopy region{};
                                       region.srcOffset = 0;
                                       region.dstOffset = dstOffset;
                                       region.size = sizeBytes;
                                       vkCmdCopyBuffer(cmd, staging, dst, 1u, &region);

                                       VkBufferMemoryBarrier2 barrier{};
                                       barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                                       barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                                       barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                                       barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                                       barrier.dstAccessMask =
                                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                                       barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                       barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                       barrier.buffer = dst;
                                       barrier.offset = dstOffset;
                                       barrier.size = sizeBytes;

                                       VkDependencyInfo dep{};
                                       dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                                       dep.bufferMemoryBarrierCount = 1u;
                                       dep.pBufferMemoryBarriers = &barrier;
                                       vkCmdPipelineBarrier2(cmd, &dep);
                                   });

            vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
            return ok;
        }
    } // namespace VulkanOneShot
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
