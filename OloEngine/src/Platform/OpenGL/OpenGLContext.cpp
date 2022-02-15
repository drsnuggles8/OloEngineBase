// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace OloEngine {

	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		OLO_CORE_ASSERT(windowHandle, "Window handle is null!")
	}

	void OpenGLContext::Init()
	{
		OLO_PROFILE_FUNCTION();

		glfwMakeContextCurrent(m_WindowHandle);
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		OLO_CORE_ASSERT(status, "Failed to initialize Glad!");

		OLO_CORE_INFO("OpenGL Info:");
		OLO_CORE_INFO("  Vendor: {0}", glGetString(GL_VENDOR));
		OLO_CORE_INFO("  Renderer: {0}", glGetString(GL_RENDERER));
		OLO_CORE_INFO("  Version: {0}", glGetString(GL_VERSION));

		OLO_CORE_ASSERT(GLVersion.major > 4 || (GLVersion.major == 4 && GLVersion.minor >= 5), "OloEngine requires at least OpenGL version 4.5!");

	}

	void OpenGLContext::SwapBuffers()
	{
		OLO_PROFILE_FUNCTION();

		glfwSwapBuffers(m_WindowHandle);
	}

}
