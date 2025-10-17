#pragma once

#include "OloEngine/Core/Base.h"
#include <expected>

namespace OloEngine
{
    /**
     * @brief Error codes for queue operations
     * 
     * Used with std::expected to provide better error reporting than bool returns.
     * Enables callers to distinguish between different failure reasons.
     */
    enum class QueueError : u8
    {
        NullTask,           ///< Attempted to push a null task pointer
        QueueFull,          ///< Queue has reached maximum capacity
        AllocationFailed,   ///< Failed to allocate memory for queue node
        
        // Future error codes can be added here without breaking existing code
    };

    /**
     * @brief Convert QueueError to human-readable string
     * 
     * @param error Error code to convert
     * @return String description of the error
     */
    inline const char* QueueErrorToString(QueueError error)
    {
        switch (error)
        {
            case QueueError::NullTask:           return "Null task pointer";
            case QueueError::QueueFull:          return "Queue is full";
            case QueueError::AllocationFailed:   return "Failed to allocate queue node";
            default:                             return "Unknown queue error";
        }
    }

} // namespace OloEngine
