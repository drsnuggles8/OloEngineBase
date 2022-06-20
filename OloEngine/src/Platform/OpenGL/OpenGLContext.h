#pragma once

#include "OloEngine/Renderer/GraphicsContext.h"

struct GLFWwindow;

namespace OloEngine {

	class OpenGLContext : public GraphicsContext
	{
	public:
		explicit OpenGLContext(GLFWwindow* windowHandle);

		void Init() override;
		void SwapBuffers() override;
	private:
		GLFWwindow* m_WindowHandle;
	};

}
