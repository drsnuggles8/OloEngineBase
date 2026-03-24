#pragma once

#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <glm/glm.hpp>

namespace OloEngine
{
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

        // Called once per frame from Application::Run() to snapshot keyboard state
        static void Update();
    };
} // namespace OloEngine
