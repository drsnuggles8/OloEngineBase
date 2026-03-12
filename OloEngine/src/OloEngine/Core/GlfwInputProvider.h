#pragma once

#include "OloEngine/Core/IInputProvider.h"

namespace OloEngine
{
    // Default input provider that delegates to the static Input class (GLFW-backed).
    class GlfwInputProvider final : public IInputProvider
    {
      public:
        [[nodiscard]] bool IsKeyPressed(KeyCode key) const override;
        [[nodiscard]] bool IsMouseButtonPressed(MouseCode button) const override;
    };

} // namespace OloEngine
