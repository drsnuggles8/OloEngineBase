// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Events/Event.h"

namespace OloEngine {

	EventCategory operator |(EventCategory lhs, EventCategory rhs)
	{
		return static_cast<EventCategory> (
			static_cast<std::underlying_type<EventCategory>::type>(lhs) |
			static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);
	}

	EventCategory operator &(EventCategory lhs, EventCategory rhs)
	{
		return static_cast<EventCategory> (
			static_cast<std::underlying_type<EventCategory>::type>(lhs) &
			static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);
	}

	EventCategory operator ^(EventCategory lhs, EventCategory rhs)
	{
		return static_cast<EventCategory> (
			static_cast<std::underlying_type<EventCategory>::type>(lhs) ^
			static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);
	}

	EventCategory operator ~(EventCategory rhs)
	{
		return static_cast<EventCategory> (
			~static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);
	}

	EventCategory& operator |=(EventCategory& lhs, EventCategory rhs)
	{
		lhs = static_cast<EventCategory> (
			static_cast<std::underlying_type<EventCategory>::type>(lhs) |
			static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);

		return lhs;
	}

	EventCategory& operator &=(EventCategory& lhs, EventCategory rhs)
	{
		lhs = static_cast<EventCategory> (
			static_cast<std::underlying_type<EventCategory>::type>(lhs) &
			static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);

		return lhs;
	}

	EventCategory& operator ^=(EventCategory& lhs, EventCategory rhs)
	{
		lhs = static_cast<EventCategory> (
			static_cast<std::underlying_type<EventCategory>::type>(lhs) ^
			static_cast<std::underlying_type<EventCategory>::type>(rhs)
			);

		return lhs;
	}

}
