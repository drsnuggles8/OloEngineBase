#pragma once

#include "OloEngine/Events/Event.h"
#include "OloEngine/Core/KeyCodes.h"

namespace OloEngine
{
    class KeyEvent : public Event
    {
      public:
        [[nodiscard("Store this!")]] KeyCode GetKeyCode() const
        {
            return m_KeyCode;
        }

        EVENT_CLASS_CATEGORY(EventCategory::Keyboard | EventCategory::Input)
      protected:
        explicit KeyEvent(const KeyCode keycode)
            : m_KeyCode(keycode) {}

        KeyCode m_KeyCode;
    };

    class KeyPressedEvent : public KeyEvent
    {
      public:
        KeyPressedEvent(const KeyCode keycode, bool isRepeat = false)
            : KeyEvent(keycode), m_IsRepeat(isRepeat) {}

        [[nodiscard("Store this!")]] bool IsRepeat() const
        {
            return m_IsRepeat;
        }

        [[nodiscard("Store this!")]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyPressedEvent: " << m_KeyCode << " (repeat = " << m_IsRepeat << ")";
            return ss.str();
        }

        EVENT_CLASS_TYPE(KeyPressed)
      private:
        bool m_IsRepeat;
    };

    class KeyReleasedEvent : public KeyEvent
    {
      public:
        explicit KeyReleasedEvent(const KeyCode keycode)
            : KeyEvent(keycode) {}

        [[nodiscard("Store this!")]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyReleasedEvent: " << m_KeyCode;
            return ss.str();
        }

        EVENT_CLASS_TYPE(KeyReleased)
    };

    class KeyTypedEvent : public KeyEvent
    {
      public:
        explicit KeyTypedEvent(const KeyCode keycode)
            : KeyEvent(keycode) {}

        [[nodiscard("Store this!")]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyTypedEvent: " << m_KeyCode;
            return ss.str();
        }

        EVENT_CLASS_TYPE(KeyTyped)
    };
} // namespace OloEngine
