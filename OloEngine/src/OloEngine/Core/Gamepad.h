#pragma once

#include "OloEngine/Core/GamepadCodes.h"

#include <glm/glm.hpp>

#include <array>
#include <string>

namespace OloEngine
{
    // Applies a radial dead zone to a 2D stick input, remapping the range so
    // the edge of the dead zone maps to 0 and full deflection maps to 1.
    inline glm::vec2 ApplyRadialDeadzone(glm::vec2 input, f32 deadzone)
    {
        f32 magnitude = glm::length(input);
        if (magnitude < deadzone)
        {
            return { 0.0f, 0.0f };
        }

        glm::vec2 direction = input / magnitude;
        f32 remapped = (magnitude - deadzone) / (1.0f - deadzone);
        return direction * glm::clamp(remapped, 0.0f, 1.0f);
    }

    class Gamepad
    {
      public:
        static constexpr u32 ButtonCount = static_cast<u32>(GamepadButton::Count);
        static constexpr u32 AxisCount = static_cast<u32>(GamepadAxis::Count);

        explicit Gamepad(i32 joystickId = -1);

        // Poll state from GLFW each frame
        void Update();

        // Button state
        [[nodiscard]] bool IsButtonPressed(GamepadButton button) const;
        [[nodiscard]] bool IsButtonJustPressed(GamepadButton button) const;
        [[nodiscard]] bool IsButtonJustReleased(GamepadButton button) const;

        // Axis state (raw)
        [[nodiscard]] f32 GetAxis(GamepadAxis axis) const;
        [[nodiscard]] glm::vec2 GetLeftStick() const;
        [[nodiscard]] glm::vec2 GetRightStick() const;

        // Axis state with dead zone applied
        [[nodiscard]] glm::vec2 GetLeftStickDeadzone(f32 deadzone = 0.15f) const;
        [[nodiscard]] glm::vec2 GetRightStickDeadzone(f32 deadzone = 0.15f) const;

        // Metadata
        [[nodiscard]] bool IsConnected() const
        {
            return m_Connected;
        }
        [[nodiscard]] const std::string& GetName() const
        {
            return m_Name;
        }
        [[nodiscard]] i32 GetJoystickId() const
        {
            return m_JoystickId;
        }

        // Returns true if any button or axis changed this frame (useful for device detection)
        [[nodiscard]] bool HadInputThisFrame() const
        {
            return m_HadInput;
        }

      private:
        i32 m_JoystickId = -1;
        std::string m_Name;
        std::array<bool, ButtonCount> m_CurrentButtons{};
        std::array<bool, ButtonCount> m_PreviousButtons{};
        std::array<f32, AxisCount> m_Axes{};
        bool m_Connected = false;
        bool m_HadInput = false;
    };

} // namespace OloEngine
