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
#include "Platform/Windows/WindowsEvent.h"
#elif defined(OLO_PLATFORM_LINUX)
#include "Platform/Linux/LinuxEvent.h"
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
#elif defined(OLO_PLATFORM_LINUX)
        Event = new FEventLinux();
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
        if (m_Event == nullptr)
            return;

        // Extract the raw pointer and null the member *before* any code that
        // can throw, so an exception from IsManualReset() can't leave both
        // a populated m_Event and an unowned heap allocation. The catch
        // below uses the saved local to delete the event itself.
        FEvent* event = m_Event;
        m_Event = nullptr;

        // Try to access the vtable to see if the object is valid
        bool isManualReset = false;
        try
        {
            isManualReset = event->IsManualReset();
        }
        catch (...)
        {
            delete event;
            return;
        }

        // TryGet (not Get) — this destructor is reachable from the static
        // destruction chain (FScheduler::s_Singleton → m_WorkerEvents.Reset →
        // ~FWaitEvent → ~FEventRef). The TLazySingleton<TEventPool> may
        // already have torn itself down in a different translation unit; in
        // that case Get() dereferences a null Ptr and the subsequent
        // m_Pool.Push SEGVs at zero-page. Fall back to deleting the event
        // directly — the pool's own destructor would have done the same.
        if (isManualReset)
        {
            if (auto* pool = TEventPool<EEventMode::ManualReset>::TryGet())
            {
                pool->ReturnToPool(event);
                return;
            }
        }
        else
        {
            if (auto* pool = TEventPool<EEventMode::AutoReset>::TryGet())
            {
                pool->ReturnToPool(event);
                return;
            }
        }
        delete event;
    }

    // FSharedEventRef implementation
    void FSharedEventRef::FEventPoolDeleter::operator()(FEvent* Event) const
    {
        if (Event == nullptr)
            return;

        // See ~FEventRef above for why TryGet is required here.
        if (Mode == EEventMode::ManualReset)
        {
            if (auto* pool = TEventPool<EEventMode::ManualReset>::TryGet())
            {
                pool->ReturnToPool(Event);
                return;
            }
        }
        else
        {
            if (auto* pool = TEventPool<EEventMode::AutoReset>::TryGet())
            {
                pool->ReturnToPool(Event);
                return;
            }
        }
        delete Event;
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
