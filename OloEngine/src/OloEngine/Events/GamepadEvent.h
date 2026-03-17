#pragma once

#include "OloEngine/Events/Event.h"
#include "OloEngine/Core/GamepadCodes.h"

namespace OloEngine
{
    class GamepadButtonPressedEvent : public Event
    {
      public:
        GamepadButtonPressedEvent(i32 gamepadIndex, GamepadButton button)
            : m_GamepadIndex(gamepadIndex), m_Button(button) {}

        [[nodiscard]] i32 GetGamepadIndex() const
        {
            return m_GamepadIndex;
        }
        [[nodiscard]] GamepadButton GetButton() const
        {
            return m_Button;
        }

        [[nodiscard]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "GamepadButtonPressedEvent: Gamepad " << m_GamepadIndex
               << ", Button " << GamepadButtonToString(m_Button);
            return ss.str();
        }

        EVENT_CLASS_TYPE(GamepadButtonPressed)
        EVENT_CLASS_CATEGORY(EventCategory::GamepadCategory | EventCategory::Input)

      private:
        i32 m_GamepadIndex;
        GamepadButton m_Button;
    };

    class GamepadButtonReleasedEvent : public Event
    {
      public:
        GamepadButtonReleasedEvent(i32 gamepadIndex, GamepadButton button)
            : m_GamepadIndex(gamepadIndex), m_Button(button) {}

        [[nodiscard]] i32 GetGamepadIndex() const
        {
            return m_GamepadIndex;
        }
        [[nodiscard]] GamepadButton GetButton() const
        {
            return m_Button;
        }

        [[nodiscard]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "GamepadButtonReleasedEvent: Gamepad " << m_GamepadIndex
               << ", Button " << GamepadButtonToString(m_Button);
            return ss.str();
        }

        EVENT_CLASS_TYPE(GamepadButtonReleased)
        EVENT_CLASS_CATEGORY(EventCategory::GamepadCategory | EventCategory::Input)

      private:
        i32 m_GamepadIndex;
        GamepadButton m_Button;
    };

    class GamepadConnectedEvent : public Event
    {
      public:
        explicit GamepadConnectedEvent(i32 gamepadIndex)
            : m_GamepadIndex(gamepadIndex) {}

        [[nodiscard]] i32 GetGamepadIndex() const
        {
            return m_GamepadIndex;
        }

        [[nodiscard]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "GamepadConnectedEvent: Gamepad " << m_GamepadIndex;
            return ss.str();
        }

        EVENT_CLASS_TYPE(GamepadConnected)
        EVENT_CLASS_CATEGORY(EventCategory::GamepadCategory | EventCategory::Input)

      private:
        i32 m_GamepadIndex;
    };

    class GamepadDisconnectedEvent : public Event
    {
      public:
        explicit GamepadDisconnectedEvent(i32 gamepadIndex)
            : m_GamepadIndex(gamepadIndex) {}

        [[nodiscard]] i32 GetGamepadIndex() const
        {
            return m_GamepadIndex;
        }

        [[nodiscard]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "GamepadDisconnectedEvent: Gamepad " << m_GamepadIndex;
            return ss.str();
        }

        EVENT_CLASS_TYPE(GamepadDisconnected)
        EVENT_CLASS_CATEGORY(EventCategory::GamepadCategory | EventCategory::Input)

      private:
        i32 m_GamepadIndex;
    };

} // namespace OloEngine
