#include "OloEnginePCH.h"
#include "OloEngine/Core/Input.h"

#include "OloEngine/Core/Application.h"
#include <GLFW/glfw3.h>

namespace OloEngine
{
    // Track previous-frame key state for just-pressed / just-released detection
    static constexpr i32 s_MaxKeys = GLFW_KEY_LAST + 1;
    static bool s_CurrentKeys[s_MaxKeys]{};
    static bool s_PreviousKeys[s_MaxKeys]{};

    namespace
    {
        // Returns the GLFW window if both Application and its Window are
        // available, otherwise nullptr. Functional tests drive Scene ticks
        // without constructing an Application, so Application::s_Instance is
        // null — calling Application::Get() then would dereference a null
        // pointer and Application::GetWindow() would SEGV reading m_Window
        // at a small offset from zero. Routing through TryGet() lets each
        // Input method fall back to its cached / no-input behaviour safely.
        GLFWwindow* TryGetGlfwWindow()
        {
            auto* const app = Application::TryGet();
            if (!app)
                return nullptr;
            return static_cast<GLFWwindow*>(app->GetWindow().GetNativeWindow());
        }

        // Fallback for headless / no-window paths: report the cached
        // s_CurrentKeys state, returning false for out-of-range key codes.
        bool IsKeyCached(const KeyCode key)
        {
            const auto k = static_cast<i32>(key);
            return (k >= 0 && k < s_MaxKeys) && s_CurrentKeys[k];
        }
    } // namespace

    void Input::Update()
    {
        auto* const window = TryGetGlfwWindow();
        if (!window)
        {
            std::memcpy(s_PreviousKeys, s_CurrentKeys, sizeof(s_CurrentKeys));
            std::memset(s_CurrentKeys, 0, sizeof(s_CurrentKeys));
            return;
        }

        std::memcpy(s_PreviousKeys, s_CurrentKeys, sizeof(s_CurrentKeys));
        for (i32 key = GLFW_KEY_SPACE; key < s_MaxKeys; ++key)
        {
            s_CurrentKeys[key] = (GLFWAPI::glfwGetKey(window, key) == GLFW_PRESS);
        }
    }

    bool Input::IsKeyPressed(const KeyCode key)
    {
        auto* const window = TryGetGlfwWindow();
        if (!window)
            return IsKeyCached(key);
        const auto state = GLFWAPI::glfwGetKey(window, static_cast<i32>(key));
        return GLFW_PRESS == state;
    }

    bool Input::IsKeyJustPressed(const KeyCode key)
    {
        const auto k = static_cast<i32>(key);
        if (k < 0 || k >= s_MaxKeys)
        {
            return false;
        }
        return s_CurrentKeys[k] && !s_PreviousKeys[k];
    }

    bool Input::IsKeyJustReleased(const KeyCode key)
    {
        const auto k = static_cast<i32>(key);
        if (k < 0 || k >= s_MaxKeys)
        {
            return false;
        }
        return !s_CurrentKeys[k] && s_PreviousKeys[k];
    }

    bool Input::IsMouseButtonPressed(const MouseCode button)
    {
        auto* const window = TryGetGlfwWindow();
        if (!window)
            return false;
        const auto state = GLFWAPI::glfwGetMouseButton(window, static_cast<i32>(button));
        return GLFW_PRESS == state;
    }

    glm::vec2 Input::GetMousePosition()
    {
        auto* const window = TryGetGlfwWindow();
        if (!window)
            return { 0.0f, 0.0f };
        f64 xpos{};
        f64 ypos{};
        GLFWAPI::glfwGetCursorPos(window, &xpos, &ypos);

        return { static_cast<f32>(xpos), static_cast<f32>(ypos) };
    }

    f32 Input::GetMouseX()
    {
        return GetMousePosition().x;
    }

    f32 Input::GetMouseY()
    {
        return GetMousePosition().y;
    }

    void Input::SetCursorMode(const CursorMode mode)
    {
        auto* const window = TryGetGlfwWindow();
        if (!window)
            return;
        i32 glfwMode = GLFW_CURSOR_NORMAL;
        switch (mode)
        {
            case CursorMode::Normal:
                glfwMode = GLFW_CURSOR_NORMAL;
                break;
            case CursorMode::Hidden:
                glfwMode = GLFW_CURSOR_HIDDEN;
                break;
            case CursorMode::Locked:
                glfwMode = GLFW_CURSOR_DISABLED;
                break;
            default:
                // A future CursorMode must be handled explicitly rather than
                // silently falling through to the GLFW_CURSOR_NORMAL fallback.
                OLO_CORE_ASSERT(false, "Input::SetCursorMode: unhandled CursorMode {}", static_cast<int>(mode));
                glfwMode = GLFW_CURSOR_NORMAL;
                break;
        }
        GLFWAPI::glfwSetInputMode(window, GLFW_CURSOR, glfwMode);
    }

    CursorMode Input::GetCursorMode()
    {
        auto* const window = TryGetGlfwWindow();
        if (!window)
            return CursorMode::Normal;
        switch (GLFWAPI::glfwGetInputMode(window, GLFW_CURSOR))
        {
            case GLFW_CURSOR_DISABLED:
                return CursorMode::Locked;
            case GLFW_CURSOR_HIDDEN:
                return CursorMode::Hidden;
            default:
                return CursorMode::Normal;
        }
    }
} // namespace OloEngine
