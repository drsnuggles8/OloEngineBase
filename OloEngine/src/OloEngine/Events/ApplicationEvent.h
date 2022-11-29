#pragma once

#include "OloEngine/Events/Event.h"

namespace OloEngine
{
	class WindowResizeEvent : public Event
	{
	public:
		WindowResizeEvent(const unsigned int width, const unsigned int height)
			: m_Width(width), m_Height(height) {}

		[[nodiscard("Store this!")]] unsigned int GetWidth()  const { return m_Width; }
		[[nodiscard("Store this!")]] unsigned int GetHeight() const { return m_Height; }

		[[nodiscard("Store this!")]] std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowResizeEvent: " << m_Width << ", " << m_Height;
			return ss.str();
		}

		EVENT_CLASS_TYPE(WindowResize)
		EVENT_CLASS_CATEGORY(EventCategory::Application)
	private:
		unsigned int m_Width;
		unsigned int m_Height;
	};

	class WindowCloseEvent : public Event
	{
	public:
		WindowCloseEvent() = default;

		EVENT_CLASS_TYPE(WindowClose)
		EVENT_CLASS_CATEGORY(EventCategory::Application)
	};

	class AppTickEvent : public Event
	{
	public:
		AppTickEvent() = default;

		EVENT_CLASS_TYPE(AppTick)
		EVENT_CLASS_CATEGORY(EventCategory::Application)
	};

	class AppUpdateEvent : public Event
	{
	public:
		AppUpdateEvent() = default;

		EVENT_CLASS_TYPE(AppUpdate)
		EVENT_CLASS_CATEGORY(EventCategory::Application)
	};

	class AppRenderEvent : public Event
	{
	public:
		AppRenderEvent() = default;

		EVENT_CLASS_TYPE(AppRender)
		EVENT_CLASS_CATEGORY(EventCategory::Application)
	};
}
