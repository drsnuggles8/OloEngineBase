#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLDebug.h"

#include <glad/gl.h>

namespace OloEngine
{
    void OpenGLMessageCallback(
        const unsigned int source,
        const unsigned int type,
        const unsigned int id,
        const unsigned int severity,
        const int,
        const char* const message,
        const void* const)
    {
        // Suppress non-significant or misleading NVIDIA driver debug messages:
        // 131185: Buffer detailed info (memory usage notifications)
        // 131204: Texture state usage warning
        // 131220: "A fragment program/shader is required to correctly render to an integer framebuffer"
        // 131140: "Blending/Dithering is enabled, but is not supported for integer framebuffers"
        // The last two fire because our framebuffers use mixed attachments (e.g., RGBA8 color +
        // R32I entity ID) and the NVIDIA debug layer checks aggregate blend state rather than
        // per-buffer state managed via glEnablei/glDisablei.
        constexpr unsigned int suppressedIDs[] = { 131185, 131204, 131220, 131140 };
        for (const auto suppressed : suppressedIDs)
        {
            if (id == suppressed)
            {
                return;
            }
        }

        std::string sourceStr;
        switch (source)
        {
            case GL_DEBUG_SOURCE_API:
                sourceStr = "API";
                break;
            case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
                sourceStr = "WINDOW_SYSTEM";
                break;
            case GL_DEBUG_SOURCE_SHADER_COMPILER:
                sourceStr = "SHADER_COMPILER";
                break;
            case GL_DEBUG_SOURCE_THIRD_PARTY:
                sourceStr = "THIRD_PARTY";
                break;
            case GL_DEBUG_SOURCE_APPLICATION:
                sourceStr = "APPLICATION";
                break;
            case GL_DEBUG_SOURCE_OTHER:
                sourceStr = "OTHER";
                break;
        }
        std::string typeStr;
        switch (type)
        {
            case GL_DEBUG_TYPE_ERROR:
                typeStr = "ERROR";
                break;
            case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
                typeStr = "DEPRECATED_BEHAVIOR";
                break;
            case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
                typeStr = "UNDEFINED_BEHAVIOR";
                break;
            case GL_DEBUG_TYPE_PORTABILITY:
                typeStr = "PORTABILITY";
                break;
            case GL_DEBUG_TYPE_PERFORMANCE:
                typeStr = "PERFORMANCE";
                break;
            case GL_DEBUG_TYPE_OTHER:
                typeStr = "OTHER";
                break;
            case GL_DEBUG_TYPE_MARKER:
                typeStr = "MARKER";
                break;
        }
        // Map severity and type to appropriate log levels
        // Performance and portability messages should generally be less severe
        if (type == GL_DEBUG_TYPE_PERFORMANCE)
        {
            // Performance messages are usually informational, not errors
            switch (severity)
            {
                case GL_DEBUG_SEVERITY_HIGH:
                    OLO_CORE_ERROR("OpenGL performance issue (source: {0}, id: {1}): {2}", sourceStr, id, message);
                    return;
                case GL_DEBUG_SEVERITY_MEDIUM:
                case GL_DEBUG_SEVERITY_LOW:
                    OLO_CORE_WARN("OpenGL performance warning (source: {0}, id: {1}): {2}", sourceStr, id, message);
                    return;
                case GL_DEBUG_SEVERITY_NOTIFICATION:
                    OLO_CORE_TRACE("OpenGL performance hint (source: {0}, id: {1}): {2}", sourceStr, id, message);
                    return;
            }
        }
        else if (type == GL_DEBUG_TYPE_PORTABILITY)
        {
            // Portability issues are usually warnings
            switch (severity)
            {
                case GL_DEBUG_SEVERITY_HIGH:
                    OLO_CORE_WARN("OpenGL portability issue (source: {0}, id: {1}): {2}", sourceStr, id, message);
                    return;
                default:
                    OLO_CORE_INFO("OpenGL portability note (source: {0}, id: {1}): {2}", sourceStr, id, message);
                    return;
            }
        }
        else
        {
            // For errors, deprecated behavior, undefined behavior, etc. - use full severity
            switch (severity)
            {
                case GL_DEBUG_SEVERITY_HIGH:
                    OLO_CORE_CRITICAL("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    return;
                case GL_DEBUG_SEVERITY_MEDIUM:
                    OLO_CORE_ERROR("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    return;
                case GL_DEBUG_SEVERITY_LOW:
                    OLO_CORE_WARN("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    return;
                case GL_DEBUG_SEVERITY_NOTIFICATION:
                    OLO_CORE_INFO("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    return;
            }
        }

        OLO_CORE_ASSERT(false, "Unknown severity level!");
    }

} // namespace OloEngine
