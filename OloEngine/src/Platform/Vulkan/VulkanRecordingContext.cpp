#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanRecordingContext.h"

namespace OloEngine
{
    namespace
    {
        // One pointer per thread. A worker thread sets it for the duration of
        // one RecordParallel item; the render thread sets it too while it
        // executes items itself (ParallelFor's calling thread takes the top
        // worker slot), which is why Ctx() cannot simply test "am I the
        // render thread".
        thread_local VulkanRecordingContext* t_CurrentWorkerContext = nullptr;
    } // namespace

    VulkanRecordingContext* CurrentVulkanWorkerContext()
    {
        return t_CurrentWorkerContext;
    }

    bool ClaimParallelWriter(std::atomic<u64>& stamp, const char* objectKind)
    {
        const VulkanRecordingContext* worker = CurrentVulkanWorkerContext();
        if (worker == nullptr)
        {
            return true;
        }
        const u64 mine = (worker->RegionId << 32u) | worker->ItemIndex;
        u64 previous = stamp.load(std::memory_order_relaxed);
        for (;;)
        {
            if (previous == mine)
            {
                return true;
            }
            if (previous != 0u && (previous >> 32u) == worker->RegionId)
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

    ScopedVulkanWorkerContext::ScopedVulkanWorkerContext(VulkanRecordingContext* context)
        : m_Guard(t_CurrentWorkerContext, context)
    {
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
