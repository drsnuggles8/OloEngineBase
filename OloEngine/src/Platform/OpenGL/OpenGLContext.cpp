#include "OloEnginePCH.h"
#include "OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <GL/GL.h>

namespace OloEngine {

	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		OLO_CORE_ASSERT(windowHandle, "Window handle is null!")
	}

	void OpenGLContext::Init()
	{
		glfwMakeContextCurrent(m_WindowHandle);
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		OLO_CORE_ASSERT(status, "Failed to initialize Glad!");

		OLO_CORE_INFO("OpenGL Info:");
		OLO_CORE_INFO("  Vendor: {0}", glGetString(GL_VENDOR));
		OLO_CORE_INFO("  Renderer: {0}", glGetString(GL_RENDERER));
		OLO_CORE_INFO("  Version: {0}", glGetString(GL_VERSION));

	#ifdef OLO_ENABLE_ASSERTS
			int versionMajor;
			int versionMinor;
			glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
			glGetIntegerv(GL_MINOR_VERSION, &versionMinor);

			OLO_CORE_ASSERT(versionMajor > 4 || (versionMajor == 4 && versionMinor >= 5), "OloEngine requires at least OpenGL version 4.5!");
	#endif
	}

	void OpenGLContext::SwapBuffers()
	{
		glfwSwapBuffers(m_WindowHandle);
	}

}