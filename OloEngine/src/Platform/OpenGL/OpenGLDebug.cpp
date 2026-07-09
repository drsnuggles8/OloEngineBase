#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLDebug.h"

#include <glad/gl.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

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
    namespace
    {
        std::atomic<u32> s_GLErrorCount{ 0 };

        std::mutex s_PerfThrottleMutex;
        std::unordered_map<unsigned int, u64> s_PerfMessageCounts;

        // Program-id -> shader-name registry. Function-local and intentionally
        // leaked: OpenGLShader destructors call UnregisterGLProgramLabel and
        // may run during static destruction (a namespace-scope map/mutex here
        // could already be destroyed by then — use-after-destruction AV at
        // process exit).
        struct ProgramLabelRegistry
        {
            std::mutex Mutex;
            std::unordered_map<u32, std::string> Labels;
        };

        [[nodiscard]] ProgramLabelRegistry& GetProgramLabelRegistry()
        {
            static auto* s_Registry = new ProgramLabelRegistry();
            return *s_Registry;
        }

        // Resolve a driver message that references a program by raw id
        // ("... in program 172 ...") to the shader name registered at link
        // time, so the log line is actionable without a separate id->name
        // hunt through trace logs. Returns an empty string when the message
        // names no program or the program wasn't registered.
        [[nodiscard]] std::string ResolveProgramLabelSuffix(const char* message)
        {
            if (message == nullptr)
                return {};

            const char* found = std::strstr(message, "program ");
            if (found == nullptr)
                return {};

            const char* digits = found + std::strlen("program ");
            if (*digits < '0' || *digits > '9')
                return {};

            u32 programID = 0;
            while (*digits >= '0' && *digits <= '9')
            {
                programID = programID * 10 + static_cast<u32>(*digits - '0');
                ++digits;
            }

            const std::string label = GetGLProgramLabel(programID);
            if (label.empty())
                return {};

            return " [program " + std::to_string(programID) + " = '" + label + "']";
        }

        // GL_DEBUG_TYPE_PERFORMANCE notifications can repeat the same message ID
        // every single frame (issue #551 - e.g. NVIDIA id 131186 "buffer migrated
        // VIDEO->HOST" on a per-frame GPU->CPU readback), flooding OloEngine.log at
        // hundreds of lines/sec - the same log-spam-stalls-main-thread failure mode
        // as #524. Log the first 10 occurrences of a given id, then every 10th up to
        // 100, every 100th up to 1000, etc. - keeps the warning visible without the
        // per-frame flood. Returns the running occurrence count for this id so the
        // caller can report it ("x123") in the (possibly suppressed) log line.
        [[nodiscard]] bool ShouldLogThrottledPerfMessage(unsigned int id, u64& outOccurrence)
        {
            std::lock_guard lock(s_PerfThrottleMutex);
            const u64 count = ++s_PerfMessageCounts[id];
            outOccurrence = count;

            if (count <= 10)
            {
                return true;
            }

            u64 step = 10;
            while (step * 10 <= count)
            {
                step *= 10;
            }
            return (count % step) == 0;
        }
    } // namespace

    void RegisterGLProgramLabel(u32 programID, std::string_view name)
    {
        if (programID == 0 || name.empty())
            return;
        auto& registry = GetProgramLabelRegistry();
        std::lock_guard lock(registry.Mutex);
        registry.Labels[programID] = std::string(name);
    }

    void UnregisterGLProgramLabel(u32 programID)
    {
        auto& registry = GetProgramLabelRegistry();
        std::lock_guard lock(registry.Mutex);
        registry.Labels.erase(programID);
    }

    std::string GetGLProgramLabel(u32 programID)
    {
        auto& registry = GetProgramLabelRegistry();
        std::lock_guard lock(registry.Mutex);
        auto it = registry.Labels.find(programID);
        return it != registry.Labels.end() ? it->second : std::string{};
    }

    u32 GetGLErrorCount()
    {
        return s_GLErrorCount.load(std::memory_order_relaxed);
    }

    void ResetGLErrorCount()
    {
        s_GLErrorCount.store(0, std::memory_order_relaxed);
    }

    // Log the native call stack that produced a shader-recompile performance
    // warning (NVIDIA id 131218). The debug context is synchronous, so the
    // callback runs inside the GL call the driver performed the JIT recompile
    // under — note that is a driver flush point (empirically glClear, see
    // docs/agent-rules/gl-clear-program-revalidation.md), not necessarily the
    // call that *caused* the mismatch. Capped to the first few occurrences:
    // symbolization costs milliseconds and the warning is one-time-per-program
    // in practice, but a pathological per-frame recompile loop must not turn
    // into a per-frame stack capture.
    static void LogShaderRecompileCallStack(u64 occurrence)
    {
#if OLO_HAS_STACKTRACE
        constexpr u64 kMaxStackCaptures = 8;
        if (occurrence > kMaxStackCaptures)
            return;

        const auto stack = std::stacktrace::current(2, 24); // skip this fn + the driver callback thunk
        std::ostringstream oss;
        for (const auto& frame : stack)
        {
            oss << "\n    " << std::to_string(frame);
        }
        OLO_CORE_WARN("Shader-recompile draw-site call stack (synchronous debug context):{0}", oss.str());
#else
        (void)occurrence;
#endif
    }

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

        if (type == GL_DEBUG_TYPE_ERROR)
        {
            s_GLErrorCount.fetch_add(1, std::memory_order_relaxed);
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
            u64 occurrence = 0;
            const bool shouldLog = ShouldLogThrottledPerfMessage(id, occurrence);

            // Performance messages are usually informational, not errors
            switch (severity)
            {
                case GL_DEBUG_SEVERITY_HIGH:
                    if (shouldLog)
                        OLO_CORE_ERROR("OpenGL performance issue (source: {0}, id: {1}, occurrence: {2}): {3}{4}", sourceStr, id, occurrence, message, ResolveProgramLabelSuffix(message));
                    return;
                case GL_DEBUG_SEVERITY_MEDIUM:
                case GL_DEBUG_SEVERITY_LOW:
                    if (shouldLog)
                    {
                        OLO_CORE_WARN("OpenGL performance warning (source: {0}, id: {1}, occurrence: {2}): {3}{4}", sourceStr, id, occurrence, message, ResolveProgramLabelSuffix(message));
                        // NVIDIA 131218: a driver-side shader JIT recompile.
                        // The GL call it happened under is the actionable
                        // datum — log it.
                        if (id == 131218)
                            LogShaderRecompileCallStack(occurrence);
                    }
                    return;
                case GL_DEBUG_SEVERITY_NOTIFICATION:
                    if (shouldLog)
                        OLO_CORE_TRACE("OpenGL performance hint (source: {0}, id: {1}, occurrence: {2}): {3}{4}", sourceStr, id, occurrence, message, ResolveProgramLabelSuffix(message));
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
                    break;
                case GL_DEBUG_SEVERITY_MEDIUM:
                    OLO_CORE_ERROR("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    break;
                case GL_DEBUG_SEVERITY_LOW:
                    OLO_CORE_WARN("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    return;
                case GL_DEBUG_SEVERITY_NOTIFICATION:
                    OLO_CORE_INFO("OpenGL debug message (source: {0}, type: {1}, id: {2}): {3}", sourceStr, typeStr, id, message);
                    return;
                default:
                    OLO_CORE_ASSERT(false, "Unknown severity level!");
                    return;
            }

            // Reached only for HIGH / MEDIUM (the severities that log at
            // CRITICAL / ERROR above): capture the offending call site once
            // for both paths.
            if (type == GL_DEBUG_TYPE_ERROR)
            {
                LogGLErrorCallStack();
            }
            return;
        }

        OLO_CORE_ASSERT(false, "Unknown severity level!");
    }

} // namespace OloEngine
