// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

// @file WindowsEvent.h
// @brief Windows-specific implementation of FEvent
// 
// Implements the Windows version of the FEvent interface using
// native Win32 event handles.
// 
// Ported from Unreal Engine's Windows/WindowsEvent.h

#include "OloEngine/HAL/Event.h"

#include <Windows.h>

namespace OloEngine
{
    // @class FEventWin
    // @brief Windows implementation of FEvent interface
    // 
    // Uses Win32 CreateEvent/SetEvent/ResetEvent/WaitForSingleObject
    class FEventWin : public FEvent
    {
    public:
        // @brief Default constructor
        FEventWin()
            : m_Event(nullptr)
            , m_ManualReset(false)
        {
        }

        // @brief Virtual destructor
        ~FEventWin() override
        {
            if (m_Event != nullptr)
            {
                CloseHandle(m_Event);
            }
        }

        // FEvent interface

        // @brief Creates the Windows event
        // @param bIsManualReset Whether the event requires manual reseting
        // @return true if the event was created successfully
        [[deprecated("Direct creation of FEvent is discouraged. Use FEventRef instead.")]]
        bool Create(bool bIsManualReset = false) override
        {
            // Create the event and default it to non-signaled
            m_Event = CreateEventW(nullptr, bIsManualReset, FALSE, nullptr);
            m_ManualReset = bIsManualReset;

            return m_Event != nullptr;
        }

        // @brief Whether the event needs manual reset
        // @return true if manual reset is required
        bool IsManualReset() override
        {
            return m_ManualReset;
        }

        // @brief Triggers (signals) the event
        void Trigger() override
        {
            TriggerForStats();
            OLO_CORE_ASSERT(m_Event != nullptr, "Event handle is null");
            SetEvent(m_Event);
        }

        // @brief Resets the event to non-signaled state
        void Reset() override
        {
            ResetForStats();
            OLO_CORE_ASSERT(m_Event != nullptr, "Event handle is null");
            ResetEvent(m_Event);
        }

        // @brief Waits for the event to be triggered
        // @param WaitTime The time to wait in milliseconds
        // @param bIgnoreThreadIdleStats Ignored in OloEngine
        // @return true if the event was signaled, false on timeout
        bool Wait(u32 WaitTime, bool bIgnoreThreadIdleStats = false) override
        {
            WaitForStats();

            OLO_CORE_ASSERT(m_Event != nullptr, "Event handle is null");

            // Note: In UE5.7 this also uses FOversubscriptionScope to let the
            // scheduler know one of its threads might be waiting. We include that
            // behavior here as well if using the task scheduler.
            // For now, just do the wait.
            return (WaitForSingleObject(m_Event, WaitTime) == WAIT_OBJECT_0);
        }

    private:
        // @brief Handle to the Win32 event
        HANDLE m_Event;

        // @brief Whether manual reset is required
        bool m_ManualReset;
    };

} // namespace OloEngine
