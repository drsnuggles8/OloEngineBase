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

    ScopedVulkanWorkerContext::ScopedVulkanWorkerContext(VulkanRecordingContext* context)
        : m_Guard(t_CurrentWorkerContext, context)
    {
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
