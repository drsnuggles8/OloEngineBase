#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLDebug.h"

#include <glad/gl.h>

#include <cstring>
#include <sstream>

// Stack trace support - available when <stacktrace> header is present (C++23).
// MSVC reports __cplusplus as 199711L unless /Zc:__cplusplus is set, so check
// _MSVC_LANG as well — without it this path silently compiles out on MSVC.
#if __has_include(<stacktrace>) && (__cplusplus >= 202302L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202302L))
#include <stacktrace>
#define OLO_HAS_STACKTRACE 1
#else
#define OLO_HAS_STACKTRACE 0
#endif

namespace OloEngine
{
    // Log the native call stack that produced a GL ERROR-type debug message.
    // The debug context is synchronous (GL_DEBUG_OUTPUT_SYNCHRONOUS, see
    // OpenGLRendererAPI::Init), so the callback runs on the thread that issued
    // the offending GL call and the capture pins the exact bind/draw site —
    // this is what turns a "some earlier test corrupted shared renderer state"
    // hunt (issue #505) into a file:line answer. Errors are rare and already
    // log at ERROR/CRITICAL, so the symbolization cost only hits broken runs.
    static void LogGLErrorCallStack()
    {
#if OLO_HAS_STACKTRACE
        const auto stack = std::stacktrace::current(2, 24); // skip this fn + the driver callback thunk
        std::ostringstream oss;
        for (const auto& frame : stack)
        {
            oss << "\n    " << std::to_string(frame);
        }
        OLO_CORE_ERROR("GL error call stack (synchronous debug context):{0}", oss.str());
#endif
    }

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
        // 131168: "The drawbuffer supplied (...) is currently bound to NONE"
        //         NVIDIA emits this as low-value chatter during resize /
        //         partial-MRT clear transitions. It is noisy and has not
        //         correlated with a real rendering fault in our pipeline.
        // The last two fire because our framebuffers use mixed attachments (e.g., RGBA8 color +
        // R32I entity ID) and the NVIDIA debug layer checks aggregate blend state rather than
        // per-buffer state managed via glEnablei/glDisablei.
        constexpr unsigned int suppressedIDs[] = { 131185, 131204, 131220, 131140, 131168 };
        for (const auto suppressed : suppressedIDs)
        {
            if (id == suppressed)
            {
                return;
            }
        }

        // RenderGraph async batches are pushed as KHR_debug application markers
        // (e.g. "AsyncBatch[0]") to aid GPU capture tools. Logging them at
        // info level floods OloEngine.log and hides actionable diagnostics.
        if (source == GL_DEBUG_SOURCE_APPLICATION && message != nullptr)
        {
            constexpr const char* asyncBatchPrefix = "AsyncBatch[";
            if (std::strncmp(message, asyncBatchPrefix, std::strlen(asyncBatchPrefix)) == 0)
                return;
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
                    if (type == GL_DEBUG_TYPE_ERROR)
                    {
                        LogGLErrorCallStack();
                    }
                    return;
                case GL_DEBUG_SEVERITY_MEDIUM:
                    OLO_CORE_ERROR("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    if (type == GL_DEBUG_TYPE_ERROR)
                    {
                        LogGLErrorCallStack();
                    }
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
