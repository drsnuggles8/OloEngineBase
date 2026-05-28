#include "OloEnginePCH.h"
#include "OloEngine/Events/Event.h"

namespace OloEngine
{
    EventCategory operator|(const EventCategory lhs, const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            std::to_underlying(lhs) |
            std::to_underlying(rhs));
    }

    EventCategory operator&(const EventCategory lhs, const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            std::to_underlying(lhs) &
            std::to_underlying(rhs));
    }

    EventCategory operator^(const EventCategory lhs, const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            std::to_underlying(lhs) ^
            std::to_underlying(rhs));
    }

    EventCategory operator~(const EventCategory rhs)
    {
        return static_cast<EventCategory>(
            ~std::to_underlying(rhs));
    }

    EventCategory& operator|=(EventCategory& lhs, const EventCategory rhs)
    {
        lhs = static_cast<EventCategory>(
            std::to_underlying(lhs) |
            std::to_underlying(rhs));

        return lhs;
    }

    EventCategory& operator&=(EventCategory& lhs, const EventCategory rhs)
    {
        lhs = static_cast<EventCategory>(
            std::to_underlying(lhs) &
            std::to_underlying(rhs));

        return lhs;
    }

    EventCategory& operator^=(EventCategory& lhs, const EventCategory rhs)
    {
        lhs = static_cast<EventCategory>(
            std::to_underlying(lhs) ^
            std::to_underlying(rhs));

        return lhs;
    }
} // namespace OloEngine
