#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLContext.h"

#include "OloEngine/Debug/CrashReporter.h"
#include "OloEngine/Renderer/DisplayOutput.h"

#include <GLFW/glfw3.h>
#include <glad/gl.h>

namespace OloEngine
{
// GL_COMPLETION_STATUS_ARB / GL_COMPLETION_STATUS_KHR — same value (0x91B1)
#ifndef GL_COMPLETION_STATUS_ARB
#define GL_COMPLETION_STATUS_ARB 0x91B1
#endif

    ParallelShaderCompileSupport OpenGLContext::s_ParallelShaderCompile = ParallelShaderCompileSupport::None;

    OpenGLContext::OpenGLContext(GLFWwindow* const windowHandle)
        : m_WindowHandle(windowHandle)
    {
        OLO_CORE_ASSERT(windowHandle, "Window handle is null!");
    }

    void OpenGLContext::Init()
    {
        OLO_PROFILE_FUNCTION();

        GLFWAPI::glfwMakeContextCurrent(m_WindowHandle);
        const int version = ::gladLoadGL(reinterpret_cast<GLADloadfunc>(GLFWAPI::glfwGetProcAddress));
        OLO_CORE_ASSERT(version, "Failed to initialize Glad!");

        // Convert GLubyte* to std::string before logging
        const std::string vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const std::string renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const std::string glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));

        OLO_CORE_INFO("OpenGL Info:");
        OLO_CORE_INFO("  Vendor: {0}", vendor);
        OLO_CORE_INFO("  Renderer: {0}", renderer);
        OLO_CORE_INFO("  Version: {0}", glVersion);

        // Provide GPU info to the crash reporter for diagnostics
        CrashReporter::SetGPUInfo(fmt::format("{} — {} ({})", renderer, glVersion, vendor));

        // Display output (#1353): this backend presents SDR only, and says why.
        // GLFW creates the default framebuffer through WGL/GLX with no colour
        // space to ask for: WGL_ARB_pixel_format_float gives float bits, never
        // an HDR10 or scRGB signal the compositor would treat as such. The
        // display is still queried, so the report names what an HDR-capable
        // backend (Vulkan) would get on this monitor.
        PublishDisplayOutput();

        OLO_CORE_ASSERT(GLAD_VERSION_MAJOR(version) == 4 && GLAD_VERSION_MINOR(version) >= 5, "OloEngine requires at least OpenGL version 4.5!");

        // Detect GL_ARB/KHR_parallel_shader_compile and configure max threads
        DetectParallelShaderCompile();
    }

    void OpenGLContext::DetectParallelShaderCompile()
    {
        OLO_PROFILE_FUNCTION();

        GLint numExtensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);

        bool hasARB = false;
        bool hasKHR = false;
        for (GLint i = 0; i < numExtensions; ++i)
        {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (!ext)
                continue;
            if (std::string_view(ext) == "GL_ARB_parallel_shader_compile")
                hasARB = true;
            else if (std::string_view(ext) == "GL_KHR_parallel_shader_compile")
                hasKHR = true;
            else
            {
                // No additional handling required.
            }
        }

        if (hasARB)
        {
            s_ParallelShaderCompile = ParallelShaderCompileSupport::ARB;

            // glMaxShaderCompilerThreadsARB(0xFFFFFFFF) = implementation max
            auto fn = reinterpret_cast<void(GLAD_API_PTR*)(GLuint)>(
                GLFWAPI::glfwGetProcAddress("glMaxShaderCompilerThreadsARB"));
            if (fn)
                fn(0xFFFFFFFF);

            OLO_CORE_INFO("  GL_ARB_parallel_shader_compile: SUPPORTED (async linking enabled)");
        }
        else if (hasKHR)
        {
            s_ParallelShaderCompile = ParallelShaderCompileSupport::KHR;

            auto fn = reinterpret_cast<void(GLAD_API_PTR*)(GLuint)>(
                GLFWAPI::glfwGetProcAddress("glMaxShaderCompilerThreadsKHR"));
            if (fn)
                fn(0xFFFFFFFF);

            OLO_CORE_INFO("  GL_KHR_parallel_shader_compile: SUPPORTED (async linking enabled)");
        }
        else
        {
            s_ParallelShaderCompile = ParallelShaderCompileSupport::None;
            OLO_CORE_INFO("  Parallel shader compile: NOT AVAILABLE (using batched fallback)");
        }
    }

    void OpenGLContext::SwapBuffers()
    {
        OLO_PROFILE_FUNCTION();

        GLFWAPI::glfwSwapBuffers(m_WindowHandle);
        // A preference change or a display change (another monitor, an OS HDR
        // toggle) re-runs the selection, so the panel and the log stay current.
        if (m_DisplaySettingsGeneration != DisplayOutput::GetSettingsGeneration() ||
            PollPlatformDisplayChange(m_WindowHandle))
            PublishDisplayOutput();
    }

    void OpenGLContext::PublishDisplayOutput()
    {
        DisplayCapabilities caps;
        caps.NoRouteReason = "OpenGL presents through a WGL/GLX default framebuffer with no swapchain colour space; "
                             "use --rhi=vulkan for HDR output";
        QueryPlatformDisplayInfo(m_WindowHandle, caps);
        const DisplayOutputSelection selection = SelectDisplayOutputMode(DisplayOutput::GetSettings().Preference, caps);
        DisplayOutput::PublishActive(selection, caps);
        m_DisplaySettingsGeneration = DisplayOutput::GetSettingsGeneration();
        OLO_CORE_INFO("Display output: {} ({}); display {}, OS HDR {}", ToString(selection.Mode), *selection.Reason,
                      caps.DisplayName.IsEmpty() ? "unknown" : *caps.DisplayName, ToString(caps.OsHdr));
    }
} // namespace OloEngine
