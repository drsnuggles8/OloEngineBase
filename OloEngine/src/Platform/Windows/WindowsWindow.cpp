// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/Windows/WindowsWindow.h"

#include "OloEngine/Core/Input.h"

#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/MouseEvent.h"
#include "OloEngine/Events/KeyEvent.h"

#include "OloEngine/Renderer/Renderer.h"

#include "Platform/OpenGL/OpenGLContext.h"

namespace OloEngine {
	static uint8_t s_GLFWWindowCount = 0;

	static void GLFWErrorCallback(int error, const char* description)
	{
		OLO_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
	}

	WindowsWindow::WindowsWindow(const WindowProps& props)
	{
		OLO_PROFILE_FUNCTION();

		Init(props);
	}

	WindowsWindow::~WindowsWindow()
	{
		OLO_PROFILE_FUNCTION();

		Shutdown();
	}

	void WindowsWindow::Init(const WindowProps& props)
	{
		OLO_PROFILE_FUNCTION();

		m_Data.Title = props.Title;
		m_Data.Width = props.Width;
		m_Data.Height = props.Height;

		OLO_CORE_INFO("Creating window {0}, ({1}, {2})", props.Title, props.Width, props.Height);

		if (s_GLFWWindowCount == 0)
		{
			OLO_PROFILE_SCOPE("glfwInit");
			const int success = GLFWAPI::glfwInit();
			OLO_CORE_ASSERT(success, "Could not initialize GLFW!")
				GLFWAPI::glfwSetErrorCallback(GLFWErrorCallback);
		}

		{
			OLO_PROFILE_SCOPE("glfwCreateWindow");

			#if defined(OLO_DEBUG)
						if (Renderer::GetAPI() == RendererAPI::API::OpenGL)
							glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
			#endif

			m_Window = GLFWAPI::glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
			++s_GLFWWindowCount;
		}

		m_Context = GraphicsContext::Create(m_Window);
		m_Context->Init();

		GLFWAPI::glfwSetWindowUserPointer(m_Window, &m_Data);
		SetVSync(true);

		// Set GLFW callbacks
		GLFWAPI::glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* const window, const int width, const int height)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;

			WindowResizeEvent event(width, height);
			data.EventCallback(event);
		});

		GLFWAPI::glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* const window)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);
			WindowCloseEvent event;
			data.EventCallback(event);
		});

		GLFWAPI::glfwSetKeyCallback(m_Window, [](GLFWwindow* const window, const int key, const int scancode, const int action, const int mods)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);

			switch (action)
			{
				case GLFW_PRESS:
				{
					KeyPressedEvent event(key, 0);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					KeyReleasedEvent event(key);
					data.EventCallback(event);
					break;
				}
				case GLFW_REPEAT:
				{
					KeyPressedEvent event(key, 1);
					data.EventCallback(event);
					break;
				}
			}
		});

		GLFWAPI::glfwSetCharCallback(m_Window, [](GLFWwindow* const window, const unsigned int keycode)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);

			KeyTypedEvent event(keycode);
			data.EventCallback(event);
		});

		GLFWAPI::glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* const window, const int button, const int action, const int mods)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);

			switch (action)
			{
				case GLFW_PRESS:
				{
					MouseButtonPressedEvent event(button);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					MouseButtonReleasedEvent event(button);
					data.EventCallback(event);
					break;
				}
			}
		});

		GLFWAPI::glfwSetScrollCallback(m_Window, [](GLFWwindow* const window, const double xOffset, const double yOffset)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);

			MouseScrolledEvent event((float)xOffset, (float)yOffset);
			data.EventCallback(event);
		});

		GLFWAPI::glfwSetCursorPosCallback(m_Window, [](GLFWwindow* const window, const double xPos, const double yPos)
		{
			WindowData& data = *(WindowData*)GLFWAPI::glfwGetWindowUserPointer(window);

			MouseMovedEvent event((float)xPos, (float)yPos);
			data.EventCallback(event);
		});
	}

	void WindowsWindow::Shutdown()
	{
		OLO_PROFILE_FUNCTION();

		GLFWAPI::glfwDestroyWindow(m_Window);
		--s_GLFWWindowCount;

		if (s_GLFWWindowCount == 0)
		{
			GLFWAPI::glfwTerminate();
		}
	}

	void WindowsWindow::OnUpdate()
	{
		OLO_PROFILE_FUNCTION();

		GLFWAPI::glfwPollEvents();
		m_Context->SwapBuffers();
	}

	void WindowsWindow::SetVSync(const bool enabled)
	{
		OLO_PROFILE_FUNCTION();

		if (enabled)
		{
			GLFWAPI::glfwSwapInterval(1);
		}
		else
		{
			GLFWAPI::glfwSwapInterval(0);
		}

		m_Data.VSync = enabled;
	}

	bool WindowsWindow::IsVSync() const
	{
		return m_Data.VSync;
	}

}
