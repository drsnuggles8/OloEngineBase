#pragma once

#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // How the OS mouse cursor behaves. `Locked` is the FPS mode: the cursor is
    // hidden and pinned, and GetMousePosition() reports unbounded virtual motion
    // (no window-edge stall), which is what mouse-look wants.
    enum class CursorMode
    {
        Normal = 0, // visible, moves freely
        Hidden,     // hidden over the window, but not locked
        Locked      // hidden + locked (FPS look)
    };

    class Input
    {
      public:
        static bool IsKeyPressed(KeyCode key);
        static bool IsKeyJustPressed(KeyCode key);
        static bool IsKeyJustReleased(KeyCode key);

        static bool IsMouseButtonPressed(MouseCode button);
        static glm::vec2 GetMousePosition();
        static f32 GetMouseX();
        static f32 GetMouseY();

        // Cursor capture (no-op when there is no window, e.g. headless tests).
        static void SetCursorMode(CursorMode mode);
        static CursorMode GetCursorMode();

        // Called once per frame from Application::Run() to snapshot keyboard state
        static void Update();
    };
} // namespace OloEngine
