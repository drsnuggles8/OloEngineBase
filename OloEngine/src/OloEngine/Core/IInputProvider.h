#pragma once

#include "OloEngine/Core/GamepadCodes.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

namespace OloEngine
{
    class IInputProvider
    {
      public:
        virtual ~IInputProvider() = default;

        [[nodiscard]] virtual bool IsKeyPressed(KeyCode key) const = 0;
        [[nodiscard]] virtual bool IsMouseButtonPressed(MouseCode button) const = 0;

        // Gamepad queries (default implementations return false/0 so existing providers don't break)
        [[nodiscard]] virtual bool IsGamepadButtonPressed(GamepadButton button, i32 gamepadIndex = 0) const
        {
            (void)button;
            (void)gamepadIndex;
            return false;
        }
        [[nodiscard]] virtual f32 GetGamepadAxis(GamepadAxis axis, i32 gamepadIndex = 0) const
        {
            (void)axis;
            (void)gamepadIndex;
            return 0.0f;
        }
    };

} // namespace OloEngine
