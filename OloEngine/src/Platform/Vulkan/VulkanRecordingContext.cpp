#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanRecordingContext.h"
#include "Platform/Vulkan/VulkanBarrierLowering.h"

namespace OloEngine
{
    namespace
    {
        // One pointer per thread. A worker thread sets it for the duration of
        // one RecordParallel item; the render thread sets it too while it
        // executes items itself (ParallelFor's calling thread takes the top
        // worker slot), which is why Ctx() cannot simply test "am I the
        // render thread".
        thread_local VulkanWorkerRecordingContext* t_CurrentWorkerContext = nullptr;
    } // namespace

    VulkanWorkerRecordingContext* CurrentVulkanWorkerContext()
    {
        return t_CurrentWorkerContext;
    }

    bool ClaimParallelWriter(std::atomic<u64>& stamp, const char* objectKind)
    {
        const VulkanWorkerRecordingContext* worker = CurrentVulkanWorkerContext();
        if (worker == nullptr)
        {
            return true;
        }
        // The token's region half is 32 bits wide, so the serial is folded
        // into 1..2^32-1 (never 0, the unowned sentinel) and compared at that
        // width — a full-u64 comparison would silently stop matching once
        // the serial passed 2^32.
        const u64 region = (worker->RegionId % 0xFFFFFFFFull) + 1ull;
        const u64 mine = (region << 32u) | worker->ItemIndex;
        u64 previous = stamp.load(std::memory_order_relaxed);
        for (;;)
        {
            if (previous == mine)
            {
                return true;
            }
            if (previous != 0u && (previous >> 32u) == region)
            {
                // Another item of THIS region wrote the object first. Its
                // token stays; this item is refused, now and on every retry.
                OLO_CORE_ERROR("[RHI/Vulkan] {} written by RecordParallel items {} and {} in one region — give each "
                               "item its own object (amendment (92) rule 6); the second write is dropped",
                               objectKind, static_cast<u32>(previous & 0xFFFFFFFFu), worker->ItemIndex);
                OLO_CORE_ASSERT(false, "two RecordParallel items wrote one buffer object");
                return false;
            }
            // Unowned, or owned by an earlier region: take it.
            if (stamp.compare_exchange_weak(previous, mine, std::memory_order_relaxed))
            {
                return true;
            }
        }
    }

    void VulkanRecordingContext::RecordBarrier(const VkDependencyInfo& dep) const
    {
        if (Cmd == VK_NULL_HANDLE)
        {
            return;
        }
        if (!OnComputeOnlyQueue)
        {
            // The overwhelmingly common path: nothing to clamp, nothing to copy.
            vkCmdPipelineBarrier2(Cmd, &dep);
            return;
        }

        // #808. A compute-only queue family supports a subset of the pipeline
        // stages, and a scope naming any other stage is invalid usage even
        // when the barrier is otherwise correct. Rather than mask the offending
        // bits out — which can leave an access mask with no stage to hang on,
        // itself a VUID violation — substitute the conservative whole-pipeline
        // scope. It over-synchronises a handful of barriers at a queue
        // boundary that is already paying for a semaphore, and it cannot be
        // wrong.
        const VkPipelineStageFlags2 supported = VulkanBarrierLowering::ComputeQueueStageMask();
        const auto clamp = [supported](VkPipelineStageFlags2& stage, VkAccessFlags2& access)
        {
            // An empty scope (a release's destination half, an acquire's
            // source half) is legal on every queue and MUST stay empty: that
            // emptiness is what makes an ownership transfer a transfer.
            if (stage == VK_PIPELINE_STAGE_2_NONE || (stage & ~supported) == 0)
            {
                return;
            }
            stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        };

        std::vector<VkMemoryBarrier2> memory(dep.pMemoryBarriers, dep.pMemoryBarriers + dep.memoryBarrierCount);
        std::vector<VkBufferMemoryBarrier2> buffers(dep.pBufferMemoryBarriers,
                                                    dep.pBufferMemoryBarriers + dep.bufferMemoryBarrierCount);
        std::vector<VkImageMemoryBarrier2> images(dep.pImageMemoryBarriers,
                                                  dep.pImageMemoryBarriers + dep.imageMemoryBarrierCount);
        for (auto& barrier : memory)
        {
            clamp(barrier.srcStageMask, barrier.srcAccessMask);
            clamp(barrier.dstStageMask, barrier.dstAccessMask);
        }
        for (auto& barrier : buffers)
        {
            clamp(barrier.srcStageMask, barrier.srcAccessMask);
            clamp(barrier.dstStageMask, barrier.dstAccessMask);
        }
        for (auto& barrier : images)
        {
            clamp(barrier.srcStageMask, barrier.srcAccessMask);
            clamp(barrier.dstStageMask, barrier.dstAccessMask);
        }

        VkDependencyInfo clamped = dep;
        clamped.memoryBarrierCount = static_cast<u32>(memory.size());
        clamped.pMemoryBarriers = memory.empty() ? nullptr : memory.data();
        clamped.bufferMemoryBarrierCount = static_cast<u32>(buffers.size());
        clamped.pBufferMemoryBarriers = buffers.empty() ? nullptr : buffers.data();
        clamped.imageMemoryBarrierCount = static_cast<u32>(images.size());
        clamped.pImageMemoryBarriers = images.empty() ? nullptr : images.data();
        vkCmdPipelineBarrier2(Cmd, &clamped);
    }

    ScopedVulkanWorkerContext::ScopedVulkanWorkerContext(VulkanWorkerRecordingContext* context)
        : m_Guard(t_CurrentWorkerContext, context)
    {
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
