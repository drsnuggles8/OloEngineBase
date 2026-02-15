#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;

    class UIInputSystem
    {
      public:
        // Process UI input for the current frame.
        // mousePos: mouse position in viewport pixel coordinates (top-left origin).
        // mouseDown: whether the primary mouse button is currently held.
        // mousePressed: whether the primary mouse button was just pressed this frame.
        // scrollDelta: mouse scroll wheel delta (positive = up/away from user).
        static void ProcessInput(Scene& scene, const glm::vec2& mousePos, bool mouseDown, bool mousePressed, f32 scrollDelta = 0.0f);
    };
} // namespace OloEngine
