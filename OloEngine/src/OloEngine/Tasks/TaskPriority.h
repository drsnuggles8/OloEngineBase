#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /**
     * @brief Task priority levels for scheduling
     * 
     * Tasks with higher priority are scheduled before lower priority tasks.
     * Priority affects which worker pool executes the task and queue selection.
     */
    enum class ETaskPriority : u8
    {
        High,       ///< Time-critical tasks (rendering, input, high-frequency updates)
        Normal,     ///< Default priority for most game logic and general work
        Background, ///< Low priority batch work (asset loading, background processing)
        
        Count       ///< Total number of priority levels (not a valid priority)
    };

    /**
     * @brief Get the index for priority-based queue/pool arrays
     * @param priority The task priority
     * @return Zero-based index for the priority level
     */
    inline u32 GetPriorityIndex(ETaskPriority priority)
    {
        OLO_CORE_ASSERT(priority < ETaskPriority::Count, "Invalid task priority");
        return static_cast<u32>(priority);
    }

    /**
     * @brief Get human-readable name for a priority level
     * @param priority The task priority
     * @return String name of the priority
     */
    inline const char* GetPriorityName(ETaskPriority priority)
    {
        switch (priority)
        {
            case ETaskPriority::High:       return "High";
            case ETaskPriority::Normal:     return "Normal";
            case ETaskPriority::Background: return "Background";
            default:                        return "Unknown";
        }
    }

} // namespace OloEngine
