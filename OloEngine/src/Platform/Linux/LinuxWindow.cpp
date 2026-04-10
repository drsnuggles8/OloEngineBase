#include "OloEnginePCH.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Events/MouseEvent.h"
#include "OloEngine/Events/KeyEvent.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLContext.h"
#include "Platform/Linux/LinuxWindow.h"

#include <algorithm>
#include <stdexcept>

namespace OloEngine
{

    f32 Window::s_HighDPIScaleFactor = 1.0f;

    static u8 s_GLFWWindowCount = 0;

    static void GLFWErrorCallback(int error, const char* description)
    {
        OLO_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
    }

    LinuxWindow::LinuxWindow(const WindowProps& props)
    {
        OLO_PROFILE_FUNCTION();

        Init(props);
    }

    LinuxWindow::~LinuxWindow()
    {
        OLO_PROFILE_FUNCTION();

        Shutdown();
    }

    void LinuxWindow::Init(const WindowProps& props)
    {
        OLO_PROFILE_FUNCTION();

        m_Data.Title = props.Title;
        m_Data.Width = props.Width;
        m_Data.Height = props.Height;

        OLO_CORE_INFO("Creating window {0}, ({1}, {2})", props.Title, props.Width, props.Height);

        if (0 == s_GLFWWindowCount)
        {
            OLO_PROFILE_SCOPE("glfwInit");

            GLFWAPI::glfwSetErrorCallback(GLFWErrorCallback);
            const int success = GLFWAPI::glfwInit();
            OLO_CORE_ASSERT(success, "Could not initialize GLFW!");
        }

        {
            OLO_PROFILE_SCOPE("glfwCreateWindow");

            s_HighDPIScaleFactor = 1.0f;
            GLFWAPI::glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);

            if (GLFWmonitor* monitor = GLFWAPI::glfwGetPrimaryMonitor())
            {
                f32 xscale{};
                f32 yscale{};
                GLFWAPI::glfwGetMonitorContentScale(monitor, &xscale, &yscale);

                if ((xscale > 1.0f) || (yscale > 1.0f))
                {
                    s_HighDPIScaleFactor = std::max(xscale, yscale);
                    GLFWAPI::glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
                }
            }

#if defined(OLO_DEBUG)
            if (Renderer::GetAPI() == RendererAPI::API::OpenGL)
            {
                GLFWAPI::glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
            }
#endif

            m_Window = GLFWAPI::glfwCreateWindow(static_cast<int>(props.Width), static_cast<int>(props.Height), m_Data.Title.c_str(), nullptr, nullptr);
            if (!m_Window)
            {
                throw std::runtime_error("glfwCreateWindow failed!");
            }
            ++s_GLFWWindowCount;
        }

        m_Context = GraphicsContext::Create(m_Window);
        if (!m_Context)
        {
            GLFWAPI::glfwDestroyWindow(m_Window);
            m_Window = nullptr;
            --s_GLFWWindowCount;
            throw std::runtime_error("Failed to create graphics context!");
        }
        m_Context->Init();

        GLFWAPI::glfwSetWindowUserPointer(m_Window, &m_Data);
        SetVSync(false);

        // Set GLFW callbacks
        GLFWAPI::glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* const window, const int width, const int height)
                                           {
            WindowData& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));
            data.Width = static_cast<unsigned int>(std::max(0, width));
            data.Height = static_cast<unsigned int>(std::max(0, height));

            WindowResizeEvent event(static_cast<int>(data.Width), static_cast<int>(data.Height));
            data.EventCallback(event); });

        GLFWAPI::glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* const window)
                                            {
            WindowData const& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));
            WindowCloseEvent event;
            data.EventCallback(event); });

        GLFWAPI::glfwSetKeyCallback(m_Window, [](GLFWwindow* const window, const int key, const int, const int action, const int)
                                    {
            WindowData const& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));

            switch (action)
            {
                case GLFW_PRESS:
                {
                    KeyPressedEvent event(static_cast<OloEngine::KeyCode>(key), false);
                    data.EventCallback(event);
                    break;
                }
                case GLFW_RELEASE:
                {
                    KeyReleasedEvent event(static_cast<OloEngine::KeyCode>(key));
                    data.EventCallback(event);
                    break;
                }
                case GLFW_REPEAT:
                {
                    KeyPressedEvent event(static_cast<OloEngine::KeyCode>(key), true);
                    data.EventCallback(event);
                    break;
                }
            } });

        GLFWAPI::glfwSetCharCallback(m_Window, [](GLFWwindow* const window, const unsigned int keycode)
                                     {
            WindowData const& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));

            KeyTypedEvent event(static_cast<OloEngine::KeyCode>(keycode));
            data.EventCallback(event); });

        GLFWAPI::glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* const window, const int button, const int action, const int)
                                            {
            WindowData const& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));

            switch (action)
            {
                case GLFW_PRESS:
                {
                    MouseButtonPressedEvent event(static_cast<OloEngine::MouseCode>(button));
                    data.EventCallback(event);
                    break;
                }
                case GLFW_RELEASE:
                {
                    MouseButtonReleasedEvent event(static_cast<OloEngine::MouseCode>(button));
                    data.EventCallback(event);
                    break;
                }
            } });

        GLFWAPI::glfwSetScrollCallback(m_Window, [](GLFWwindow* const window, const f64 xOffset, const f64 yOffset)
                                       {
            WindowData const& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));

            MouseScrolledEvent event(static_cast<f32>(xOffset), static_cast<f32>(yOffset));
            data.EventCallback(event); });

        GLFWAPI::glfwSetCursorPosCallback(m_Window, [](GLFWwindow* const window, const f64 xPos, const f64 yPos)
                                          {
            WindowData const& data = *static_cast<WindowData*>(GLFWAPI::glfwGetWindowUserPointer(window));

            MouseMovedEvent event(static_cast<f32>(xPos), static_cast<f32>(yPos));
            data.EventCallback(event); });
    }

    void LinuxWindow::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Window)
        {
            GLFWAPI::glfwDestroyWindow(m_Window);
            m_Window = nullptr;
            --s_GLFWWindowCount;
        }

        if (0 == s_GLFWWindowCount)
        {
            GLFWAPI::glfwTerminate();
        }
    }

    void LinuxWindow::OnUpdate()
    {
        OLO_PROFILE_FUNCTION();

        PollEvents();
        SwapBuffers();
    }

    void LinuxWindow::PollEvents()
    {
        OLO_PROFILE_FUNCTION();

        GLFWAPI::glfwPollEvents();
    }

    void LinuxWindow::SwapBuffers()
    {
        OLO_PROFILE_FUNCTION();

        m_Context->SwapBuffers();
    }

    void LinuxWindow::SetVSync(const bool enabled)
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

    void LinuxWindow::SetTitle(const std::string& title)
    {
        m_Data.Title = title;
        GLFWAPI::glfwSetWindowTitle(m_Window, title.c_str());
    }

    u32 LinuxWindow::GetFramebufferWidth() const
    {
        int width{};
        int height{};
        GLFWAPI::glfwGetFramebufferSize(m_Window, &width, &height);
        return static_cast<u32>(width);
    }

    u32 LinuxWindow::GetFramebufferHeight() const
    {
        int width{};
        int height{};
        GLFWAPI::glfwGetFramebufferSize(m_Window, &width, &height);
        return static_cast<u32>(height);
    }

} // namespace OloEngine
