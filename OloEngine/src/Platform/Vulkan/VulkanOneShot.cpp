#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanAddressCommands.h"
#include "Platform/Vulkan/VulkanTrackedAllocation.h"
#include "Platform/Vulkan/VulkanOneShot.h"

#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"

#include <cstring>

namespace OloEngine
{
    namespace VulkanOneShot
    {
#ifndef OLO_DIST
        namespace
        {
            // See the header: test-only fault injection for the layout-desync
            // tenants. Render thread only, like the rest of the backend.
            u32 s_FailNextSubmits = 0;
            u32 s_FailNextFenceWaits = 0;
        } // namespace

        void SetFailNextSubmitsForTesting(u32 count)
        {
            s_FailNextSubmits = count;
        }

        u32 GetPendingFailNextSubmitsForTesting()
        {
            return s_FailNextSubmits;
        }

        void SetFailNextFenceWaitsForTesting(u32 count)
        {
            s_FailNextFenceWaits = count;
        }

        u32 GetPendingFailNextFenceWaitsForTesting()
        {
            return s_FailNextFenceWaits;
        }
#endif

        bool Submit(const char* what, const std::function<void(VkCommandBuffer)>& record, Outcome* outOutcome)
        {
            OLO_PROFILE_FUNCTION();

            // Every early return below leaves this at NotSubmitted, which is
            // the truth for all of them: none reached vkQueueSubmit2.
            const auto setOutcome = [outOutcome](Outcome outcome)
            {
                if (outOutcome != nullptr)
                    *outOutcome = outcome;
            };
            setOutcome(Outcome::NotSubmitted);

#ifndef OLO_DIST
            if (s_FailNextSubmits > 0)
            {
                --s_FailNextSubmits;
                OLO_CORE_WARN("VulkanOneShot::Submit({}): failing by test injection", what);
                return false;
            }
#endif

            // A one-shot submits ahead of the frame's primary and advances the
            // executed layouts through a process-wide scope stack — neither is
            // defined from a RecordParallel item (amendment (92) rule 7).
            if (CurrentVulkanWorkerContext() != nullptr)
            {
                OLO_CORE_ASSERT(false, "VulkanOneShot::Submit from a RecordParallel item");
                OLO_CORE_ERROR("VulkanOneShot::Submit({}): refused on a parallel-recording worker context", what);
                return false;
            }

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

            // Everything recorded here is submitted immediately, i.e. it
            // executes BEFORE the frame command buffer that may still be
            // recording. Layout writes made inside therefore advance the
            // EXECUTED layout as well as the recorded one (issue #800) — but
            // only once this buffer has really reached the queue, so the scope
            // spans the submit and is promoted by MarkSubmitted below. Every
            // early return between here and there discards the writes.
            VulkanImageLayoutTracker::ImmediateExecutionScope immediate;
            // One-shots record blits, copies and clears, which run as
            // driver-internal shaders reading descriptors from the bound heap's
            // reserved range — bound before the first command, like every other
            // command buffer the backend records (#1511).
            (void)VulkanResourceHeap::Get().CmdBindIfCreated(cmd);
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
                // ACCEPTED BY THE QUEUE. That, not the fence wait, is what
                // makes this buffer's transitions real: queue submissions
                // execute in submit order, so every later submission already
                // sees the layouts recorded here whether or not the host
                // managed to observe the fence. Promoting only after a
                // successful wait left the tracker claiming the OLD layout for
                // work that was certain to run — the same wrong-oldLayout
                // desync the rest of #803 is about, just reached by a timeout.
                immediate.MarkSubmitted();
                setOutcome(Outcome::Submitted);

                // Blocking wait is the point (see the header): load-time GL
                // uploads were synchronous, and callers destroy staging
                // buffers immediately after this returns.
                u64 timeoutNs = 10'000'000'000ull; // 10 s — an upload that slow is a hang
#ifndef OLO_DIST
                // The injected timeout waits for real with a zero budget, so
                // the drain below runs against work that may still be pending.
                const bool injectTimeout = s_FailNextFenceWaits > 0;
                if (injectTimeout)
                {
                    --s_FailNextFenceWaits;
                    timeoutNs = 0u;
                }
#endif
                VkResult waited = vkWaitForFences(device->GetDevice(), 1u, &fence, VK_TRUE, timeoutNs);
#ifndef OLO_DIST
                if (injectTimeout)
                {
                    OLO_CORE_WARN("VulkanOneShot::Submit({}): forcing a fence-wait timeout by test injection", what);
                    waited = VK_TIMEOUT;
                }
#endif
                if (waited != VK_SUCCESS)
                {
                    OLO_CORE_ERROR("VulkanOneShot::Submit({}): fence wait failed (VkResult {})", what,
                                   static_cast<int>(waited));
                    ok = false;
                }
                else
                {
                    setOutcome(Outcome::Completed);
                }

                // A submission whose wait FAILED may still be executing, and
                // destroying its fence (VUID-vkDestroyFence-fence-01120) or
                // freeing its pending command buffer (VUID-vkFreeCommandBuffers-
                // pCommandBuffers-00047) under it is invalid usage, as is the
                // caller destroying the staging buffer it copies from. So wait
                // it out before releasing, as VulkanContext's flush does. A
                // truly hung GPU becomes DEVICE_LOST through the OS watchdog,
                // which ends this wait and makes the release legal. The call
                // still reports the failed wait: the result stays false/Submitted.
                if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST)
                {
                    OLO_CORE_ERROR("VulkanOneShot::Submit({}): waiting for completion or device loss before "
                                   "releasing the submission",
                                   what);
                    VkResult drained = vkWaitForFences(device->GetDevice(), 1u, &fence, VK_TRUE, UINT64_MAX);
                    if (drained != VK_SUCCESS && drained != VK_ERROR_DEVICE_LOST)
                    {
                        // An error wait proves nothing about the fence. An
                        // idle queue does: every submission on it, this one
                        // included, has retired.
                        OLO_CORE_ERROR("VulkanOneShot::Submit({}): drain wait failed (VkResult {}); waiting for "
                                       "the queue to go idle",
                                       what, static_cast<int>(drained));
                        drained = vkQueueWaitIdle(device->GetQueue());
                    }
                    if (drained != VK_SUCCESS && drained != VK_ERROR_DEVICE_LOST)
                    {
                        // Nothing proves the work retired: leak the fence and
                        // the command buffer rather than release them in
                        // flight, and tell the caller to keep what it read.
                        OLO_CORE_ERROR("VulkanOneShot::Submit({}): queue idle wait failed (VkResult {}); leaking "
                                       "the fence and command buffer",
                                       what, static_cast<int>(drained));
                        setOutcome(Outcome::DrainFailed);
                        return false;
                    }
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
            stagingInfo.usage = VulkanAddressCommands::kStagingSrcUsage;
            stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo stagingAlloc{};
            stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
            stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation stagingAllocation = VK_NULL_HANDLE;
            VmaAllocationInfo stagingOut{};
            if (TrackedVmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                                       &stagingOut) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("{}: staging buffer allocation failed ({} bytes)", what, sizeBytes);
                return false;
            }

            std::memcpy(stagingOut.pMappedData, data, sizeBytes);
            vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, sizeBytes);
            const VkDeviceAddress stagingAddress =
                VulkanAddressCommands::QueryAddress(device->GetDevice(), staging);
            const VkDeviceAddress dstAddress = VulkanAddressCommands::QueryAddress(device->GetDevice(), dst);
            if (dstAddress == 0)
            {
                // The header's documented precondition, enforced rather than
                // assumed: `dst` must carry SHADER_DEVICE_ADDRESS_BIT, which is
                // the one requirement the address form added. Every engine
                // buffer family sets it, so reaching this means a new caller
                // did not — and a copy to address 0 loses the device silently.
                OLO_CORE_ERROR("{}: destination buffer has no device address "
                               "(missing VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) — upload dropped",
                               what);
                TrackedVmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
                return false;
            }

            Outcome outcome = Outcome::NotSubmitted;
            const bool ok = Submit(what,
                                   [&](VkCommandBuffer cmd)
                                   {
                                       // `dst` is any caller-supplied buffer —
                                       // vertex, index, storage, raw — and the
                                       // families differ on STORAGE_BUFFER_BIT,
                                       // so the storage usage is genuinely
                                       // unknown here (VulkanAddressCommands).
                                       VulkanAddressCommands::CmdCopyRange(
                                           cmd, stagingAddress,
                                           VulkanAddressCommands::StorageUsage::Absent, dstAddress + dstOffset,
                                           VulkanAddressCommands::StorageUsage::Unknown, sizeBytes);

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
                                   },
                                   &outcome);

            // The copy may still be reading the staging buffer: leak it with
            // the fence and command buffer Submit already leaked.
            if (outcome != Outcome::DrainFailed)
                TrackedVmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
            return ok;
        }
    } // namespace VulkanOneShot
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
