// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file EventPool.h
 * @brief Pool of FEvent instances for efficient reuse
 *
 * This template class manages a pool of FEvent instances to avoid
 * the overhead of constantly creating and destroying event objects.
 *
 * Ported from Unreal Engine's HAL/EventPool.h
 */

#include "OloEngine/HAL/Event.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Memory/LockFreeList.h"
#include "OloEngine/Misc/LazySingleton.h"

namespace OloEngine
{
    // Forward declaration — must precede template usage for GCC
    FEvent* CreateSynchEvent(EEventMode Mode);

    /**
     * @class TEventPool
     * @brief Template class for a pool of FEvent objects
     * @tparam Mode The event mode (AutoReset or ManualReset)
     */
    template<EEventMode Mode>
    class TEventPool
    {
      public:
        /** @brief Destructor - destroys all pooled events */
        ~TEventPool()
        {
            EmptyPool();
        }

        /**
         * @brief Gets a pooled event or creates a new one
         * @return A pointer to an FEvent instance
         */
        FEvent* GetEventFromPool()
        {
            FEvent* Event = m_Pool.Pop();

            if (Event == nullptr)
            {
                Event = CreateSynchEvent(Mode);
            }
            else
            {
                // Ensure event is reset when retrieved from pool
                Event->Reset();
            }

            return Event;
        }

        /**
         * @brief Returns an event to the pool for reuse
         * @param Event The event to return to the pool
         */
        void ReturnToPool(FEvent* Event)
        {
            OLO_CORE_ASSERT(Event != nullptr, "Cannot return null event to pool");
            Event->Reset();
            m_Pool.Push(Event);
        }

        /**
         * @brief Destroys all events in the pool
         */
        void EmptyPool()
        {
            while (FEvent* Event = m_Pool.Pop())
            {
                delete Event;
            }
        }

        /**
         * @brief Gets the singleton instance of the event pool
         * @return Reference to the event pool singleton
         */
        static TEventPool& Get()
        {
            return TLazySingleton<TEventPool>::Get();
        }

        /**
         * @brief Gets the singleton instance if it has not been torn down
         * @return Pointer to the event pool singleton, or nullptr if it has
         *         already been destroyed (e.g. called from another static
         *         object's destructor running after this singleton's).
         *
         * Use from destructors that may run during program exit — calling
         * Get() after TearDown returns a reference to destroyed memory and
         * any subsequent member access SEGVs at zero-page.
         */
        static TEventPool* TryGet()
        {
            return TLazySingleton<TEventPool>::TryGet();
        }

        /**
         * @brief Tears down the singleton instance
         */
        static void TearDown()
        {
            return TLazySingleton<TEventPool>::TearDown();
        }

      private:
        /** @brief Lock-free list of available events */
        TLockFreePointerListUnordered<FEvent, 0> m_Pool;
    };

} // namespace OloEngine
