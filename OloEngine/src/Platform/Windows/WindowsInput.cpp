#include "OloEnginePCH.h"
#include "OloEngine/Core/Input.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/SyntheticInput.h"
#include <GLFW/glfw3.h>

namespace OloEngine
{
    // Track previous-frame key state for just-pressed / just-released detection
    static constexpr i32 s_MaxKeys = GLFW_KEY_LAST + 1;
    static bool s_CurrentKeys[s_MaxKeys]{};
    static bool s_PreviousKeys[s_MaxKeys]{};

    // Typed-character (text-entry) double buffer. The char callback appends to
    // s_TypedCharsPending; Update() rotates it into s_TypedCharsFrame so readers
    // see exactly the codepoints typed during the just-polled frame.
    static std::vector<u32> s_TypedCharsPending;
    static std::vector<u32> s_TypedCharsFrame;

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

    const std::vector<u32>& Input::GetTypedCharacters()
    {
        return s_TypedCharsFrame;
    }

    void Input::OnCharTyped(const u32 codepoint)
    {
        s_TypedCharsPending.push_back(codepoint);
    }

    void Input::Update()
    {
        // Rotate the typed-character buffer: codepoints accumulated since the
        // previous Update() (i.e. during the poll that just ran) become this
        // frame's text input. Done before the window check so headless ticks
        // and tests that pump OnCharTyped/Update directly behave identically.
        s_TypedCharsFrame.swap(s_TypedCharsPending);
        s_TypedCharsPending.clear();

        auto* const window = TryGetGlfwWindow();
        if (!window)
        {
            std::memcpy(s_PreviousKeys, s_CurrentKeys, sizeof(s_CurrentKeys));
            std::memset(s_CurrentKeys, 0, sizeof(s_CurrentKeys));
            // Even with no window a synthetic key can be held (a headless host
            // driving the injection overlay), so it must still show up in the
            // just-pressed / just-released edges below.
            if (SyntheticInput::AnyKeyDown())
            {
                for (i32 key = 0; key < s_MaxKeys; ++key)
                {
                    s_CurrentKeys[key] = SyntheticInput::IsKeyDown(static_cast<KeyCode>(key));
                }
            }
            return;
        }

        std::memcpy(s_PreviousKeys, s_CurrentKeys, sizeof(s_CurrentKeys));
        // Synthetic-down is OR'd over the hardware state (issue #607): an injected
        // key is logically held even though GLFW sees no physical press. Never the
        // reverse — a synthetic "up" must not mask a real key a human is holding.
        const bool anySynthetic = SyntheticInput::AnyKeyDown();
        for (i32 key = GLFW_KEY_SPACE; key < s_MaxKeys; ++key)
        {
            s_CurrentKeys[key] = (GLFWAPI::glfwGetKey(window, key) == GLFW_PRESS) ||
                                 (anySynthetic && SyntheticInput::IsKeyDown(static_cast<KeyCode>(key)));
        }
    }

    bool Input::IsKeyPressed(const KeyCode key)
    {
        if (SyntheticInput::IsKeyDown(key))
            return true;
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
        if (SyntheticInput::IsMouseButtonDown(button))
            return true;
        auto* const window = TryGetGlfwWindow();
        if (!window)
            return false;
        const auto state = GLFWAPI::glfwGetMouseButton(window, static_cast<i32>(button));
        return GLFW_PRESS == state;
    }

    glm::vec2 Input::GetMousePosition()
    {
        // A synthetic cursor override wins outright (it is only ever set while an
        // injected input plan is in flight): the physical cursor is somewhere else
        // entirely, and every consumer here wants the position the editor is being
        // driven from.
        if (glm::vec2 synthetic{ 0.0f }; SyntheticInput::TryGetMousePosition(synthetic))
            return synthetic;

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
