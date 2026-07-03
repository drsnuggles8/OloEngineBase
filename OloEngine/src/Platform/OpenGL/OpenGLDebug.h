#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    void OpenGLMessageCallback(const unsigned int source, const unsigned int type, const unsigned int id, const unsigned int severity, const int, const char* const message, const void* const);

    // Test/diagnostic hook: count of GL_DEBUG_TYPE_ERROR messages the callback
    // has observed (excluding the suppressed noise IDs) since the last
    // ResetGLErrorCount() call. Cheaper and less brittle than scanning
    // Log::Get().GetRecentLogMessages() for message text.
    [[nodiscard]] u32 GetGLErrorCount();
    void ResetGLErrorCount();
} // namespace OloEngine
