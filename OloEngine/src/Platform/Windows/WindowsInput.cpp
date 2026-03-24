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

    void Input::Update()
    {
        auto* const nativeWindow = Application::Get().GetWindow().GetNativeWindow();
        if (!nativeWindow)
        {
            std::memcpy(s_PreviousKeys, s_CurrentKeys, sizeof(s_CurrentKeys));
            std::memset(s_CurrentKeys, 0, sizeof(s_CurrentKeys));
            return;
        }

        auto* const window = static_cast<GLFWwindow*>(nativeWindow);
        std::memcpy(s_PreviousKeys, s_CurrentKeys, sizeof(s_CurrentKeys));
        for (i32 key = GLFW_KEY_SPACE; key < s_MaxKeys; ++key)
        {
            s_CurrentKeys[key] = (GLFWAPI::glfwGetKey(window, key) == GLFW_PRESS);
        }
    }

    bool Input::IsKeyPressed(const KeyCode key)
    {
        auto* const nativeWindow = Application::Get().GetWindow().GetNativeWindow();
        if (!nativeWindow)
        {
            const auto k = static_cast<i32>(key);
            return (k >= 0 && k < s_MaxKeys) && s_CurrentKeys[k];
        }
        auto* const window = static_cast<GLFWwindow*>(nativeWindow);
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
        auto* const nativeWindow = Application::Get().GetWindow().GetNativeWindow();
        if (!nativeWindow)
        {
            return false;
        }
        auto* const window = static_cast<GLFWwindow*>(nativeWindow);
        const auto state = GLFWAPI::glfwGetMouseButton(window, static_cast<i32>(button));
        return GLFW_PRESS == state;
    }

    glm::vec2 Input::GetMousePosition()
    {
        auto* const nativeWindow = Application::Get().GetWindow().GetNativeWindow();
        if (!nativeWindow)
        {
            return { 0.0f, 0.0f };
        }
        auto* const window = static_cast<GLFWwindow*>(nativeWindow);
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
} // namespace OloEngine
