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

		OLO_CORE_INFO("OpenGL Info:");
		OLO_CORE_INFO("  Vendor: {0}", glGetString(GL_VENDOR));
		OLO_CORE_INFO("  Renderer: {0}", glGetString(GL_RENDERER));
		OLO_CORE_INFO("  Version: {0}", glGetString(GL_VERSION));

		OLO_CORE_ASSERT(GLAD_VERSION_MAJOR(version) == 4 && GLAD_VERSION_MINOR(version) >= 5, "OloEngine requires at least OpenGL version 4.5!");
	}

	void OpenGLContext::SwapBuffers()
	{
		OLO_PROFILE_FUNCTION();

		GLFWAPI::glfwSwapBuffers(m_WindowHandle);
	}
}
