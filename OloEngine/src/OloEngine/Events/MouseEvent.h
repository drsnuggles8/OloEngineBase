#pragma once

#include "OloEngine/Events/Event.h"
#include "OloEngine/Core/MouseCodes.h"

namespace OloEngine
{
	class MouseMovedEvent : public Event
	{
	public:
		MouseMovedEvent(const f32 x, const f32 y)
			: m_MouseX(x), m_MouseY(y) {}

		[[nodiscard("Store this!")]] f32 GetX() const { return m_MouseX; }
		[[nodiscard("Store this!")]] f32 GetY() const { return m_MouseY; }

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseMovedEvent: " << m_MouseX << ", " << m_MouseY;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseMoved)
		EVENT_CLASS_CATEGORY(EventCategory::Mouse | EventCategory::Input)
	private:
		f32 m_MouseX;
		f32 m_MouseY;
	};

	class MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(const f32 xOffset, const f32 yOffset)
			: m_XOffset(xOffset), m_YOffset(yOffset) {}

		[[nodiscard("Store this!")]] f32 GetXOffset() const { return m_XOffset; }
		[[nodiscard("Store this!")]] f32 GetYOffset() const { return m_YOffset; }

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseScrolledEvent: " << GetXOffset() << ", " << GetYOffset();
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseScrolled)
		EVENT_CLASS_CATEGORY(EventCategory::Mouse | EventCategory::Input)
	private:
		f32 m_XOffset;
		f32 m_YOffset;
	};

	class MouseButtonEvent : public Event
	{
	public:
		[[nodiscard("Store this!")]] MouseCode GetMouseButton() const { return m_Button; }

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
