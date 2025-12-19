// EventCount.h - Event that avoids missed notifications
// Ported from UE5.7 Async/EventCount.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/HAL/ParkingLot.h"

#include <atomic>
#include <type_traits>

namespace OloEngine
{

    template<typename CounterType>
    class TEventCount;

    // @class TEventCountToken
    // @brief A token used to wait on TEventCount
    //
    // Acquiring a token before checking the condition avoids a race because
    // Wait returns immediately when the token no longer matches the notification count.
    template<typename CounterType>
    class TEventCountToken
    {
        static_assert(std::is_unsigned_v<CounterType>);

      public:
        // @brief Returns true if the token has been assigned by PrepareWait()
        inline explicit operator bool() const
        {
            return !(Value & 1);
        }

      private:
        // Defaults to an odd value, which is never valid to wait on
        CounterType Value = 1;

        friend TEventCount<CounterType>;
    };

    // @class TEventCount
    // @brief A type of event that avoids missed notifications by maintaining a notification count
    //
    // This type of event is suited to waiting on another thread conditionally.
    // Typical usage looks similar to this example:
    //
    // @code
    //     FEventCount Event;
    //     std::atomic<u32> CurrentValue = 0;
    //
    // On the waiting thread:
    //
    //     FEventCountToken Token = Event.PrepareWait();
    //     if (CurrentValue < TargetValue)
    //     {
    //         Event.Wait(Token);
    //     }
    //
    // On the notifying thread:
    //
    //     if (++CurrentValue == TargetValue)
    //     {
    //         Event.Notify();
    //     }
    // @endcode
    //
    // Acquiring a token before checking the condition avoids a race because Wait returns immediately
    // when the token no longer matches the notification count.
    template<typename CounterType>
    class TEventCount
    {
        static_assert(std::is_unsigned_v<CounterType>);

      public:
        constexpr TEventCount() = default;

        TEventCount(const TEventCount&) = delete;
        TEventCount& operator=(const TEventCount&) = delete;

        // @brief Prepare to wait
        //
        // Call this before any logic that must re-execute if the event is notified in the meantime.
        //
        // @return A token to pass to one of the wait functions to abort the wait if the event has been notified since.
        inline TEventCountToken<CounterType> PrepareWait()
        {
            TEventCountToken<CounterType> Token;

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
            // Doing a relaxed load here because fetch_or on x86 cannot return the previous value
            // so when we use the return value of fetch_or, it gets compiled into a compare_exchange.
            // The worst that can happen here is that we get a stale token value and end up
            // not waiting for an iteration. We would then get the proper value on the next iteration.
            Token.Value = m_Count.load(std::memory_order_relaxed) & ~CounterType(1);
            m_Count.fetch_or(1, std::memory_order_acq_rel);
#else
            Token.Value = m_Count.fetch_or(1, std::memory_order_acq_rel) & ~CounterType(1);
#endif
            return Token;
        }

        // @brief Wait until the event is notified
        //
        // Returns immediately if notified since the token was acquired.
        //
        // @param Compare A token acquired from PrepareWait() before checking the conditions for this wait.
        inline void Wait(TEventCountToken<CounterType> Compare)
        {
            if ((m_Count.load(std::memory_order_acquire) & ~CounterType(1)) == Compare.Value)
            {
                struct FWaitContext
                {
                    std::atomic<CounterType>* pCount;
                    CounterType CompareValue;
                };

                FWaitContext Context{ &m_Count, Compare.Value };

                auto CanWaitFunc = [](void* Ctx) -> bool
                {
                    auto* pContext = static_cast<FWaitContext*>(Ctx);
                    return (pContext->pCount->load(std::memory_order_acquire) & ~CounterType(1)) == pContext->CompareValue;
                };

                ParkingLot::Wait(&m_Count, +CanWaitFunc, &Context, nullptr, nullptr);
            }
        }

        // @brief Wait until the event is notified, with timeout
        //
        // Returns immediately if notified since the token was acquired.
        //
        // @param Compare   A token acquired from PrepareWait() before checking the conditions for this wait.
        // @param WaitTime  Relative time after which waiting is automatically canceled and the thread wakes.
        // @return True if the event was notified before the wait time elapsed, otherwise false.
        inline bool WaitFor(TEventCountToken<CounterType> Compare, FMonotonicTimeSpan WaitTime)
        {
            if ((m_Count.load(std::memory_order_acquire) & ~CounterType(1)) == Compare.Value)
            {
                struct FWaitContext
                {
                    std::atomic<CounterType>* pCount;
                    CounterType CompareValue;
                };

                FWaitContext Context{ &m_Count, Compare.Value };

                auto CanWaitFunc = [](void* Ctx) -> bool
                {
                    auto* pContext = static_cast<FWaitContext*>(Ctx);
                    return (pContext->pCount->load(std::memory_order_acquire) & ~CounterType(1)) == pContext->CompareValue;
                };

                const ParkingLot::FWaitState WaitState = ParkingLot::Private::WaitFor(
                    &m_Count,
                    +CanWaitFunc,
                    &Context,
                    nullptr,
                    nullptr,
                    WaitTime);

                // Make sure we return true if we did wait but also if the wait was skipped
                // because the value actually changed before we had the opportunity to wait.
                return WaitState.bDidWake || !WaitState.bDidWait;
            }
            return true;
        }

        // @brief Wait until the event is notified, until absolute time
        //
        // Returns immediately if notified since the token was acquired.
        //
        // @param Compare   A token acquired from PrepareWait() before checking the conditions for this wait.
        // @param WaitTime  Absolute time after which waiting is automatically canceled and the thread wakes.
        // @return True if the event was notified before the wait time elapsed, otherwise false.
        inline bool WaitUntil(TEventCountToken<CounterType> Compare, FMonotonicTimePoint WaitTime)
        {
            if ((m_Count.load(std::memory_order_acquire) & ~CounterType(1)) == Compare.Value)
            {
                struct FWaitContext
                {
                    std::atomic<CounterType>* pCount;
                    CounterType CompareValue;
                };

                FWaitContext Context{ &m_Count, Compare.Value };

                auto CanWaitFunc = [](void* Ctx) -> bool
                {
                    auto* pContext = static_cast<FWaitContext*>(Ctx);
                    return (pContext->pCount->load(std::memory_order_acquire) & ~CounterType(1)) == pContext->CompareValue;
                };

                const ParkingLot::FWaitState WaitState = ParkingLot::Private::WaitUntil(
                    &m_Count,
                    +CanWaitFunc,
                    &Context,
                    nullptr,
                    nullptr,
                    WaitTime);

                // Make sure we return true if we did wait but also if the wait was skipped
                // because the value actually changed before we had the opportunity to wait.
                return WaitState.bDidWake || !WaitState.bDidWait;
            }
            return true;
        }

        // @brief Notifies all waiting threads
        //
        // Any threads that have called PrepareWait() and not yet waited will be notified immediately
        // if they do wait on a token from a call to PrepareWait() that preceded this call.
        inline void Notify()
        {
            // .fetch_add(0, acq_rel) is used to have a StoreLoad barrier,
            // which we can't express in C++. That works by making the load
            // also be store (via RMW) and relying on a StoreStore barrier to
            // get the desired ordering.
            CounterType Value = m_Count.fetch_add(0, std::memory_order_acq_rel);
            if ((Value & 1) && m_Count.compare_exchange_strong(Value, Value + 1, std::memory_order_release))
            {
                ParkingLot::WakeAll(&m_Count);
            }
        }

        /**
         * @brief Notifies all waiting threads (weak version)
         *
         * Any threads that have called PrepareWait() and not yet waited will be notified immediately
         * if they do wait on a token from a call to PrepareWait() that preceded this call.
         *
         * @note This version doesn't provide a memory barrier, you are responsible
         *       for the memory ordering for the value you're synchronizing this eventcount with.
         */
        inline void NotifyWeak()
        {
#if defined(__arm__) || defined(__aarch64__) || defined(_M_ARM) || defined(_M_ARM64)
            // On weakly consistent memory, use the full barrier version
            CounterType Value = m_Count.fetch_add(0, std::memory_order_acq_rel);
#else
            // On x86 and other non weak memory model, the fetch_or inside PrepareWait
            // is a serializing instruction that will flush the store buffer.
            // We can omit the expensive locked op here and just do a relaxed read.
            CounterType Value = m_Count.load(std::memory_order_relaxed);
#endif
            if ((Value & 1) && m_Count.compare_exchange_strong(Value, Value + 1, std::memory_order_release))
            {
                ParkingLot::WakeAll(&m_Count);
            }
        }

      private:
        std::atomic<CounterType> m_Count = 0;
    };

    // Type aliases for common usage
    using FEventCount = TEventCount<u32>;
    using FEventCountToken = TEventCountToken<u32>;

} // namespace OloEngine
