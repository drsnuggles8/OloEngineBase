#pragma once

#include "OloEngine/Events/Event.h"

#include <sstream>
#include <string>
#include <utility>

namespace OloEngine
{
    // Fired by LocalizationManager when the active locale changes. UI panels,
    // dialogue runners, and any other listener that caches resolved text
    // should refresh on this event.
    class LocaleChangedEvent : public Event
    {
      public:
        LocaleChangedEvent(std::string previousLocale, std::string newLocale)
            : m_PreviousLocale(std::move(previousLocale)), m_NewLocale(std::move(newLocale))
        {
        }

        [[nodiscard("Store this!")]] const std::string& GetPreviousLocale() const
        {
            return m_PreviousLocale;
        }

        [[nodiscard("Store this!")]] const std::string& GetNewLocale() const
        {
            return m_NewLocale;
        }

        [[nodiscard("Store this!")]] std::string ToString() const override
        {
            std::stringstream ss;
            ss << "LocaleChangedEvent: " << m_PreviousLocale << " -> " << m_NewLocale;
            return ss.str();
        }

        EVENT_CLASS_TYPE(LocaleChanged)
        EVENT_CLASS_CATEGORY(EventCategory::Application)

      private:
        std::string m_PreviousLocale;
        std::string m_NewLocale;
    };
} // namespace OloEngine
