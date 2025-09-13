#pragma once

#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Core/Base.h"

#include <functional>
#include <string>
#include <sstream>

namespace OloEngine
{
	// Events in OloEngine are currently blocking, meaning when an event occurs it
	// immediately gets dispatched and must be dealt with right then and there.
	// For the future, a better strategy might be to buffer events in an event
	// bus and process them during the "event" part of the update stage.

	enum class EventType
	{
		None = 0,
		WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMoved,
		AppTick, AppUpdate, AppRender,
		KeyPressed, KeyReleased, KeyTyped,
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled,
		// Editor/Engine custom events
		AssetReloaded
	};

	enum class EventCategory
	{
		None = 0,
		Application = BIT(0),
		Input = BIT(1),
		Keyboard = BIT(2),
		Mouse = BIT(3),
		MouseButton = BIT(4)
	};

	EventCategory operator |(EventCategory lhs, EventCategory rhs);

	EventCategory operator &(EventCategory lhs, EventCategory rhs);

	EventCategory operator ^(EventCategory lhs, EventCategory rhs);

	EventCategory operator ~(EventCategory rhs);

	EventCategory& operator |=(EventCategory& lhs, EventCategory rhs);

	EventCategory& operator &=(EventCategory& lhs, EventCategory rhs);

	EventCategory& operator ^=(EventCategory& lhs, EventCategory rhs);

#define EVENT_CLASS_TYPE(type) static EventType GetStaticType() { return EventType::type; }\
								EventType GetEventType() const override { return GetStaticType(); }\
								const char* GetName() const override { return #type; }

// NOTE: These macros provide boilerplate code for event classes.
// Future refactoring could use CRTP (Curiously Recurring Template Pattern) to eliminate macros
#define EVENT_CLASS_CATEGORY(category) EventCategory GetCategoryFlags() const override { return (category); }

	class Event
	{
	public:
		virtual ~Event() = default;

		bool Handled = false;

		[[nodiscard("Store this!")]] virtual EventType GetEventType() const = 0;
		[[nodiscard("Store this!")]] virtual const char* GetName() const = 0;
		[[nodiscard("Store this!")]] virtual EventCategory GetCategoryFlags() const = 0;
		[[nodiscard("Store this!")]] virtual std::string ToString() const { return GetName(); }

		[[nodiscard("Store this!")]] bool IsInCategory(EventCategory const category) const
		{
			return static_cast<bool>(GetCategoryFlags() & category);
		}
	};

	class EventDispatcher
	{
	public:
		explicit EventDispatcher(Event& event)
			: m_Event(event)
		{
		}

		// F will be deduced by the compiler
		template<typename T, typename F>
		bool Dispatch(const F& func)
		{
			if ((!m_Event.Handled) && (m_Event.GetEventType() == T::GetStaticType()))
			{
				m_Event.Handled = func(dynamic_cast<T&>(m_Event));
				return true;
			}
			return false;
		}
	private:
		Event& m_Event;
	};

	inline std::ostream& operator<<(std::ostream& os, const Event& e)
	{
		return os << e.ToString();
	}
}
