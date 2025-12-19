// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

// @file Event.cpp
// @brief Implementation of FEvent base class and FEventRef
//
// Contains the implementations for event statistics tracking and
// the RAII FEventRef wrapper.
//
// Ported from Unreal Engine's HAL/Event.cpp and related files

#include "OloEngine/HAL/Event.h"
#include "OloEngine/HAL/EventPool.h"

#ifdef OLO_PLATFORM_WINDOWS
#include "OloEngine/HAL/Windows/WindowsEvent.h"
#endif

namespace OloEngine
{
    // Static member initialization
    std::atomic<u32> FEvent::s_EventUniqueId{ 0 };

    void FEvent::AdvanceStats()
    {
        // Stats tracking would be implemented here
        // For now, this is a placeholder
    }

    void FEvent::WaitForStats()
    {
        // Record that a wait has started
        m_EventStartCycles.fetch_add(1, std::memory_order_relaxed);
    }

    void FEvent::TriggerForStats()
    {
        // Record that the event was triggered
        // In UE5.7 this sends stats messages - we just track the cycle
    }

    void FEvent::ResetForStats()
    {
        // Reset the cycle counter
        m_EventStartCycles.store(0, std::memory_order_relaxed);
    }

    // Platform-specific event creation
    FEvent* CreateSynchEvent(EEventMode Mode)
    {
        FEvent* Event = nullptr;

#ifdef OLO_PLATFORM_WINDOWS
        Event = new FEventWin();
#else
        static_assert(false, "Platform-specific event implementation required");
#endif

        // Create the event
        OLO_DISABLE_DEPRECATION_WARNINGS
        if (!Event->Create(Mode == EEventMode::ManualReset))
            OLO_RESTORE_DEPRECATION_WARNINGS
            {
                delete Event;
                Event = nullptr;
            }

        return Event;
    }

    // FEventRef implementation
    FEventRef::FEventRef(EEventMode Mode)
        : m_Event(nullptr)
    {
        if (Mode == EEventMode::AutoReset)
        {
            m_Event = TEventPool<EEventMode::AutoReset>::Get().GetEventFromPool();
        }
        else
        {
            m_Event = TEventPool<EEventMode::ManualReset>::Get().GetEventFromPool();
        }
    }

    FEventRef::~FEventRef()
    {
        if (m_Event != nullptr)
        {

            // Try to access the vtable to see if the object is valid
            bool isManualReset = false;
            try
            {
                isManualReset = m_Event->IsManualReset();
            }
            catch (...)
            {
                return;
            }

            if (isManualReset)
            {
                auto& pool = TEventPool<EEventMode::ManualReset>::Get();
                pool.ReturnToPool(m_Event);
            }
            else
            {
                TEventPool<EEventMode::AutoReset>::Get().ReturnToPool(m_Event);
            }
        }
    }

    // FSharedEventRef implementation
    void FSharedEventRef::FEventPoolDeleter::operator()(FEvent* Event) const
    {
        if (Event != nullptr)
        {
            if (Mode == EEventMode::ManualReset)
            {
                TEventPool<EEventMode::ManualReset>::Get().ReturnToPool(Event);
            }
            else
            {
                TEventPool<EEventMode::AutoReset>::Get().ReturnToPool(Event);
            }
        }
    }

    FSharedEventRef::FSharedEventRef(EEventMode Mode)
    {
        FEvent* Event = nullptr;
        if (Mode == EEventMode::AutoReset)
        {
            Event = TEventPool<EEventMode::AutoReset>::Get().GetEventFromPool();
        }
        else
        {
            Event = TEventPool<EEventMode::ManualReset>::Get().GetEventFromPool();
        }
        m_Ptr = std::shared_ptr<FEvent>(Event, FEventPoolDeleter{ Mode });
    }

} // namespace OloEngine
