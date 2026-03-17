#include "OloEnginePCH.h"
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <GLFW/glfw3.h>

namespace OloEngine
{
    Gamepad::Gamepad(i32 joystickId)
        : m_JoystickId(joystickId)
    {
    }

    void Gamepad::Update()
    {
        OLO_PROFILE_FUNCTION();

        m_PreviousButtons = m_CurrentButtons;
        m_HadInput = false;

        if (m_JoystickId < 0)
        {
            m_Connected = false;
            return;
        }

        if (!glfwJoystickPresent(m_JoystickId) || !glfwJoystickIsGamepad(m_JoystickId))
        {
            if (m_Connected)
            {
                OLO_CORE_INFO("Gamepad {} disconnected", m_JoystickId);
            }
            m_Connected = false;
            m_Name.clear();
            m_CurrentButtons.fill(false);
            m_Axes.fill(0.0f);
            return;
        }

        bool firstConnect = false;
        if (!m_Connected)
        {
            const char* name = glfwGetGamepadName(m_JoystickId);
            m_Name = name ? name : "Unknown Gamepad";
            m_Connected = true;
            firstConnect = true;
            OLO_CORE_INFO("Gamepad {} connected: {}", m_JoystickId, m_Name);
        }

        GLFWgamepadstate state{};
        if (glfwGetGamepadState(m_JoystickId, &state))
        {
            for (u32 i = 0; i < ButtonCount; ++i)
            {
                m_CurrentButtons[i] = (state.buttons[i] == GLFW_PRESS);
                if (m_CurrentButtons[i] != m_PreviousButtons[i])
                {
                    m_HadInput = true;
                }
            }

            for (u32 i = 0; i < AxisCount; ++i)
            {
                f32 newAxis = state.axes[i];
                if (!firstConnect && std::abs(newAxis - m_Axes[i]) > 0.01f)
                {
                    m_HadInput = true;
                }
                m_Axes[i] = newAxis;
            }
        }
    }

    bool Gamepad::IsButtonPressed(GamepadButton button) const
    {
        auto idx = static_cast<u32>(button);
        return (idx < ButtonCount) && m_CurrentButtons[idx];
    }

    bool Gamepad::IsButtonJustPressed(GamepadButton button) const
    {
        auto idx = static_cast<u32>(button);
        return (idx < ButtonCount) && m_CurrentButtons[idx] && !m_PreviousButtons[idx];
    }

    bool Gamepad::IsButtonJustReleased(GamepadButton button) const
    {
        auto idx = static_cast<u32>(button);
        return (idx < ButtonCount) && !m_CurrentButtons[idx] && m_PreviousButtons[idx];
    }

    f32 Gamepad::GetAxis(GamepadAxis axis) const
    {
        auto idx = static_cast<u32>(axis);
        return (idx < AxisCount) ? m_Axes[idx] : 0.0f;
    }

    glm::vec2 Gamepad::GetLeftStick() const
    {
        return { GetAxis(GamepadAxis::LeftX), GetAxis(GamepadAxis::LeftY) };
    }

    glm::vec2 Gamepad::GetRightStick() const
    {
        return { GetAxis(GamepadAxis::RightX), GetAxis(GamepadAxis::RightY) };
    }

    glm::vec2 Gamepad::GetLeftStickDeadzone(f32 deadzone) const
    {
        return ApplyRadialDeadzone(GetLeftStick(), deadzone);
    }

    glm::vec2 Gamepad::GetRightStickDeadzone(f32 deadzone) const
    {
        return ApplyRadialDeadzone(GetRightStick(), deadzone);
    }

} // namespace OloEngine
