#pragma once

#include <functional>
#include <string>
#include <sstream>

#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine {
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
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled
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
								virtual EventType GetEventType() const override { return GetStaticType(); }\
								virtual const char* GetName() const override { return #type; }
// TODO(olbu): Refactor these with functions instead of macros
#define EVENT_CLASS_CATEGORY(category) virtual EventCategory GetCategoryFlags() const override { return (category); }

	class Event
	{
	public:
		virtual ~Event() = default;

		bool Handled = false;

		[[nodiscard("This returns EventType::type, you probably wanted some other function!")]] virtual EventType GetEventType() const = 0;
		[[nodiscard("This returns #type, you probably wanted some other function!")]] virtual const char* GetName() const = 0;
		[[nodiscard("This returns (category), you probably wanted some other function!")]] virtual EventCategory GetCategoryFlags() const = 0;
		[[nodiscard("This returns GetName as string, you probably wanted some other function!")]] virtual std::string ToString() const { return GetName(); }

		[[nodiscard("This returns a boolean whether the category is in the category, you probably wanted some other function!")]] bool IsInCategory(EventCategory const category) const
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

