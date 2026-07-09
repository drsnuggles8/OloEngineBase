#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>

namespace OloEngine
{
    void OpenGLMessageCallback(const unsigned int source, const unsigned int type, const unsigned int id, const unsigned int severity, const int, const char* const message, const void* const);

    // Test/diagnostic hook: count of GL_DEBUG_TYPE_ERROR messages the callback
    // has observed (excluding the suppressed noise IDs) since the last
    // ResetGLErrorCount() call. Cheaper and less brittle than scanning
    // Log::Get().GetRecentLogMessages() for message text.
    [[nodiscard]] u32 GetGLErrorCount();
    void ResetGLErrorCount();

    // CPU-side program-id -> shader-name registry, maintained by
    // OpenGLShader/OpenGLComputeShader at link/delete time. The debug callback
    // uses it to resolve driver messages that reference a raw program id
    // (e.g. NVIDIA id 131218 "Vertex shader in program 172 is being recompiled
    // based on GL state") to the shader's name. This must be a CPU registry:
    // KHR_debug makes calling GL (glGetObjectLabel) from inside the callback
    // undefined behavior.
    void RegisterGLProgramLabel(u32 programID, std::string_view name);
    void UnregisterGLProgramLabel(u32 programID);
    [[nodiscard]] std::string GetGLProgramLabel(u32 programID);
} // namespace OloEngine
