#include "OloEnginePCH.h"
#include "OloEngine/Events/Event.h"

namespace OloEngine
{
    EventCategory operator|(const EventCategory lhs, const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            static_cast<std::underlying_type_t<EventCategory>>(lhs) |
            static_cast<std::underlying_type_t<EventCategory>>(rhs));
    }

    EventCategory operator&(const EventCategory lhs, const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            static_cast<std::underlying_type_t<EventCategory>>(lhs) &
            static_cast<std::underlying_type_t<EventCategory>>(rhs));
    }

    EventCategory operator^(const EventCategory lhs, const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            static_cast<std::underlying_type_t<EventCategory>>(lhs) ^
            static_cast<std::underlying_type_t<EventCategory>>(rhs));
    }

    EventCategory operator~(const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            ~static_cast<std::underlying_type_t<EventCategory>>(rhs));
    }

    EventCategory& operator|=(EventCategory& lhs, const EventCategory rhs)
    {
        lhs = static_cast<EventCategory>(
            static_cast<std::underlying_type_t<EventCategory>>(lhs) |
            static_cast<std::underlying_type_t<EventCategory>>(rhs));

        return lhs;
    }

    EventCategory& operator&=(EventCategory& lhs, const EventCategory rhs)
    {
        lhs = static_cast<EventCategory>(
            static_cast<std::underlying_type_t<EventCategory>>(lhs) &
            static_cast<std::underlying_type_t<EventCategory>>(rhs));

        return lhs;
    }

    EventCategory& operator^=(EventCategory& lhs, const EventCategory rhs)
    {
        lhs = static_cast<EventCategory>(
            static_cast<std::underlying_type_t<EventCategory>>(lhs) ^
            static_cast<std::underlying_type_t<EventCategory>>(rhs));

        return lhs;
    }
} // namespace OloEngine
