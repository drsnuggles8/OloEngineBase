#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/gl.h>

namespace OloEngine
{
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

		OLO_CORE_ASSERT(GLAD_VERSION_MAJOR(version) == 4 && GLAD_VERSION_MINOR(version) >= 5, "OloEngine requires at least OpenGL version 4.5!");
	}

	void OpenGLContext::SwapBuffers()
	{
		OLO_PROFILE_FUNCTION();

		GLFWAPI::glfwSwapBuffers(m_WindowHandle);
	}
}
