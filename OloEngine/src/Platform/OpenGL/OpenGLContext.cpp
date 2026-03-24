#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLContext.h"

#include "OloEngine/Debug/CrashReporter.h"

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

        OLO_CORE_ASSERT(GLAD_VERSION_MAJOR(version) == 4 && GLAD_VERSION_MINOR(version) >= 5, "OloEngine requires at least OpenGL version 4.5!");

        // Detect GL_ARB/KHR_parallel_shader_compile and configure max threads
        DetectParallelShaderCompile();
    }

    void OpenGLContext::DetectParallelShaderCompile()
    {
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
    }
} // namespace OloEngine
