// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file Event.h
 * @brief Interface for waitable events
 * 
 * This interface has platform-specific implementations that are used to wait
 * for another thread to signal that it is ready for the waiting thread to do
 * some work. It can also be used for telling groups of threads to exit.
 * 
 * Consider using FEventRef as a safer and more convenient alternative.
 * 
 * Ported from Unreal Engine's HAL/Event.h
 */

#include "OloEngine/Core/Base.h"

#include <atomic>
#include <memory>
#include <chrono>
#include <limits>

namespace OloEngine
{
    /**
     * @enum EEventMode
     * @brief Specifies the event reset mode
     */
    enum class EEventMode
    {
        AutoReset,   ///< Event is automatically reset after a successful wait
        ManualReset  ///< Event must be manually reset
    };

    /**
     * @class FEvent
     * @brief Abstract interface for waitable events
     * 
     * This interface has platform-specific implementations that are used to wait
     * for another thread to signal that it is ready for the waiting thread to do
     * some work.
     */
    class FEvent
    {
    public:
        /**
         * @brief Creates the event
         * 
         * Manually reset events stay triggered until reset.
         * 
         * @param bIsManualReset Whether the event requires manual reseting or not
         * @return true if the event was created, false otherwise
         * 
         * @deprecated Direct creation of FEvent is discouraged for performance reasons.
         *             Please use the event pool via FEventRef.
         */
        [[deprecated("Direct creation of FEvent is discouraged. Use FEventRef instead.")]]
        virtual bool Create(bool bIsManualReset = false) = 0;

        /**
         * @brief Whether the signaled state of this event needs to be reset manually
         * @return true if the state requires manual resetting, false otherwise
         * @see Reset
         */
        virtual bool IsManualReset() = 0;

        /**
         * @brief Triggers the event so any waiting threads are activated
         * @see IsManualReset, Reset
         */
        virtual void Trigger() = 0;

        /**
         * @brief Resets the event to an untriggered (waitable) state
         * @see IsManualReset, Trigger
         */
        virtual void Reset() = 0;

        /**
         * @brief Waits the specified amount of time for the event to be triggered
         * 
         * A wait time of MAX_uint32 is treated as infinite wait.
         * 
         * @param WaitTime The time to wait (in milliseconds)
         * @param bIgnoreThreadIdleStats If true, ignores ThreadIdleStats
         * @return true if the event was triggered, false if the wait timed out
         */
        virtual bool Wait(u32 WaitTime, bool bIgnoreThreadIdleStats = false) = 0;

        /**
         * @brief Waits an infinite amount of time for the event to be triggered
         * @return true if the event was triggered
         */
        bool Wait()
        {
            return Wait(std::numeric_limits<u32>::max());
        }

        /**
         * @brief Waits the specified duration for the event to be triggered
         * 
         * @tparam Rep Duration representation type
         * @tparam Period Duration period type
         * @param WaitTime The time to wait
         * @param bIgnoreThreadIdleStats If true, ignores ThreadIdleStats
         * @return true if the event was triggered, false if the wait timed out
         */
        template<typename Rep, typename Period>
        bool Wait(const std::chrono::duration<Rep, Period>& WaitTime, bool bIgnoreThreadIdleStats = false)
        {
            auto Millis = std::chrono::duration_cast<std::chrono::milliseconds>(WaitTime).count();
            u32 WaitTimeMs = static_cast<u32>(std::clamp<i64>(Millis, 0, std::numeric_limits<u32>::max()));
            return Wait(WaitTimeMs, bIgnoreThreadIdleStats);
        }

        /** @brief Default constructor */
        FEvent()
            : m_EventId(0)
            , m_EventStartCycles(0)
        {
        }

        /** @brief Virtual destructor */
        virtual ~FEvent() = default;

        /**
         * @brief Advances stats associated with this event
         * 
         * Used to monitor wait->trigger history.
         */
        void AdvanceStats();

    protected:
        /** @brief Sends to the stats a special message which encodes a wait for the event */
        void WaitForStats();

        /** @brief Sends to the stats a special message which encodes a trigger for the event */
        void TriggerForStats();

        /** @brief Resets start cycles to 0 */
        void ResetForStats();

        /** @brief Counter used to generate a unique id for the events */
        static std::atomic<u32> s_EventUniqueId;

        /** @brief A unique id for this event */
        u32 m_EventId;

        /** @brief Greater than 0 if the event called wait */
        std::atomic<u32> m_EventStartCycles;
    };

    // Forward declaration
    template<EEventMode PoolType>
    class TEventPool;

    /**
     * @class FEventRef
     * @brief RAII-style pooled FEvent
     * 
     * Non-copyable, non-movable.
     * Automatically returns the event to the pool on destruction.
     */
    class FEventRef final
    {
    public:
        /**
         * @brief Construct a new event reference
         * @param Mode The event mode (AutoReset or ManualReset)
         */
        explicit FEventRef(EEventMode Mode = EEventMode::AutoReset);

        /** @brief Destructor - returns event to pool */
        ~FEventRef();

        // Non-copyable, non-movable
        FEventRef(const FEventRef&) = delete;
        FEventRef& operator=(const FEventRef&) = delete;
        FEventRef(FEventRef&&) = delete;
        FEventRef& operator=(FEventRef&&) = delete;

        /**
         * @brief Access the underlying event
         * @return Pointer to the FEvent
         */
        FEvent* operator->() const
        {
            return m_Event;
        }

        /**
         * @brief Get the underlying event
         * @return Pointer to the FEvent
         */
        FEvent* Get() const
        {
            return m_Event;
        }

    private:
        FEvent* m_Event;
    };

    /**
     * @class FSharedEventRef
     * @brief RAII-style shared and pooled FEvent
     * 
     * Unlike FEventRef, this can be copied and shared between multiple owners.
     * The event is returned to the pool when the last reference is destroyed.
     */
    class FSharedEventRef final
    {
    public:
        /**
         * @brief Construct a new shared event reference
         * @param Mode The event mode (AutoReset or ManualReset)
         */
        explicit FSharedEventRef(EEventMode Mode = EEventMode::AutoReset);

        // Copyable
        FSharedEventRef(const FSharedEventRef&) = default;
        FSharedEventRef& operator=(const FSharedEventRef&) = default;

        // Movable
        FSharedEventRef(FSharedEventRef&&) noexcept = default;
        FSharedEventRef& operator=(FSharedEventRef&&) noexcept = default;

        /**
         * @brief Access the underlying event
         * @return Pointer to the FEvent
         */
        FEvent* operator->() const
        {
            return m_Ptr.get();
        }

        /**
         * @brief Get the underlying event
         * @return Pointer to the FEvent
         */
        FEvent* Get() const
        {
            return m_Ptr.get();
        }

    private:
        // Custom deleter that returns event to pool
        struct FEventPoolDeleter
        {
            EEventMode Mode;
            void operator()(FEvent* Event) const;
        };

        std::shared_ptr<FEvent> m_Ptr;
    };

} // namespace OloEngine
