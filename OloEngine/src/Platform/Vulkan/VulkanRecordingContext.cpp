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

    ScopedVulkanWorkerContext::ScopedVulkanWorkerContext(VulkanWorkerRecordingContext* context)
        : m_Guard(t_CurrentWorkerContext, context)
    {
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
