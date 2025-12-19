// ExtendedTaskPriority.h - Extended task priority system with named thread support
// Ported from UE5.7 Tasks/TaskPrivate.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/LowLevelTask.h"

namespace OloEngine::Tasks
{
    // @enum EExtendedTaskPriority
    // @brief Extended task priorities including inline execution and named threads
    //
    // These priorities extend the basic ETaskPriority with special execution modes:
    // - None: Use the regular task priority
    // - Inline: Execute immediately on the calling thread (no scheduling)
    // - TaskEvent: Optimized for synchronization events (no execution body)
    // - Named thread priorities: For integration with game/render thread model
    enum class EExtendedTaskPriority : i8
    {
        None,      ///< Use regular task priority
        Inline,    ///< Execute inline without scheduling
        TaskEvent, ///< Task event - optimized for events, skips scheduling

        // Named thread support (for integration with game/render thread model)
        GameThreadNormalPri,
        GameThreadHiPri,
        GameThreadNormalPriLocalQueue,
        GameThreadHiPriLocalQueue,

        RenderThreadNormalPri,
        RenderThreadHiPri,
        RenderThreadNormalPriLocalQueue,
        RenderThreadHiPriLocalQueue,

        RHIThreadNormalPri,
        RHIThreadHiPri,
        RHIThreadNormalPriLocalQueue,
        RHIThreadHiPriLocalQueue,

        Count
    };

    // @brief Convert extended priority to string
    inline const char* ToString(EExtendedTaskPriority ExtendedPriority)
    {
        if (ExtendedPriority < EExtendedTaskPriority::None || ExtendedPriority >= EExtendedTaskPriority::Count)
        {
            return nullptr;
        }

        static const char* ExtendedTaskPriorityToStr[] = {
            "None",
            "Inline",
            "TaskEvent",

            "GameThreadNormalPri",
            "GameThreadHiPri",
            "GameThreadNormalPriLocalQueue",
            "GameThreadHiPriLocalQueue",

            "RenderThreadNormalPri",
            "RenderThreadHiPri",
            "RenderThreadNormalPriLocalQueue",
            "RenderThreadHiPriLocalQueue",

            "RHIThreadNormalPri",
            "RHIThreadHiPri",
            "RHIThreadNormalPriLocalQueue",
            "RHIThreadHiPriLocalQueue"
        };
        return ExtendedTaskPriorityToStr[static_cast<i32>(ExtendedPriority)];
    }

    // @brief Parse string to extended priority
    inline bool ToExtendedTaskPriority(const char* ExtendedPriorityStr, EExtendedTaskPriority& OutExtendedPriority)
    {
        if (!ExtendedPriorityStr)
        {
            return false;
        }

        // Case-insensitive comparison helper
        auto StrCmpI = [](const char* a, const char* b) -> bool
        {
            if (!a || !b)
                return false;
            while (*a && *b)
            {
                char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
                if (ca != cb)
                    return false;
                ++a;
                ++b;
            }
            return *a == *b;
        };

#define CONVERT_EXTENDED_TASK_PRIORITY(PriorityName)                                 \
    if (StrCmpI(ExtendedPriorityStr, ToString(EExtendedTaskPriority::PriorityName))) \
    {                                                                                \
        OutExtendedPriority = EExtendedTaskPriority::PriorityName;                   \
        return true;                                                                 \
    }

        CONVERT_EXTENDED_TASK_PRIORITY(None);
        CONVERT_EXTENDED_TASK_PRIORITY(Inline);
        CONVERT_EXTENDED_TASK_PRIORITY(TaskEvent);

        CONVERT_EXTENDED_TASK_PRIORITY(GameThreadNormalPri);
        CONVERT_EXTENDED_TASK_PRIORITY(GameThreadHiPri);
        CONVERT_EXTENDED_TASK_PRIORITY(GameThreadNormalPriLocalQueue);
        CONVERT_EXTENDED_TASK_PRIORITY(GameThreadHiPriLocalQueue);

        CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadNormalPri);
        CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadHiPri);
        CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadNormalPriLocalQueue);
        CONVERT_EXTENDED_TASK_PRIORITY(RenderThreadHiPriLocalQueue);

        CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadNormalPri);
        CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadHiPri);
        CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadNormalPriLocalQueue);
        CONVERT_EXTENDED_TASK_PRIORITY(RHIThreadHiPriLocalQueue);

#undef CONVERT_EXTENDED_TASK_PRIORITY

        return false;
    }

    // @brief Check if the extended priority is for a named thread
    inline bool IsNamedThreadPriority(EExtendedTaskPriority ExtendedPriority)
    {
        return ExtendedPriority >= EExtendedTaskPriority::GameThreadNormalPri;
    }

    // @brief Check if the extended priority should execute inline
    inline bool IsInlinePriority(EExtendedTaskPriority ExtendedPriority)
    {
        return ExtendedPriority == EExtendedTaskPriority::Inline;
    }

    // @brief Check if the extended priority is for a task event
    inline bool IsTaskEventPriority(EExtendedTaskPriority ExtendedPriority)
    {
        return ExtendedPriority == EExtendedTaskPriority::TaskEvent;
    }

} // namespace OloEngine::Tasks
