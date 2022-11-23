#pragma once

#include "OloEngine/Events/Event.h"
#include "OloEngine/Core/MouseCodes.h"

namespace OloEngine {
	class MouseMovedEvent : public Event
	{
	public:
		MouseMovedEvent(const float x, const float y)
			: m_MouseX(x), m_MouseY(y) {}

		[[nodiscard("This returns m_MouseX, you probably wanted another function!")]] float GetX() const { return m_MouseX; }
		[[nodiscard("This returns m_MouseY, you probably wanted another function!")]] float GetY() const { return m_MouseY; }

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseMovedEvent: " << m_MouseX << ", " << m_MouseY;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseMoved)
		EVENT_CLASS_CATEGORY(EventCategory::Mouse | EventCategory::Input)
	private:
		float m_MouseX;
		float m_MouseY;
	};

	class MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(const float xOffset, const float yOffset)
			: m_XOffset(xOffset), m_YOffset(yOffset) {}

		[[nodiscard("This returns m_XOffset, you probably wanted another function!")]] float GetXOffset() const { return m_XOffset; }
		[[nodiscard("This returns y_Offset, you probably wanted another function!")]] float GetYOffset() const { return m_YOffset; }

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseScrolledEvent: " << GetXOffset() << ", " << GetYOffset();
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseScrolled)
		EVENT_CLASS_CATEGORY(EventCategory::Mouse | EventCategory::Input)
	private:
		float m_XOffset;
		float m_YOffset;
	};

	class MouseButtonEvent : public Event
	{
	public:
		[[nodiscard("This returns m_Button, you probably wanted another function!")]] MouseCode GetMouseButton() const { return m_Button; }

		EVENT_CLASS_CATEGORY(EventCategory::Mouse | EventCategory::Input | EventCategory::MouseButton)
	protected:
		explicit MouseButtonEvent(const MouseCode button)
			: m_Button(button) {}

		MouseCode m_Button;
	};

	class MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		explicit MouseButtonPressedEvent(const MouseCode button)
			: MouseButtonEvent(button) {}

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonPressedEvent: " << m_Button;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		explicit MouseButtonReleasedEvent(const MouseCode button)
			: MouseButtonEvent(button) {}

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonReleasedEvent: " << m_Button;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonReleased)
	};

}
