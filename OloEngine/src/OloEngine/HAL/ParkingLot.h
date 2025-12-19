// ParkingLot.h - Global hash table of wait queues keyed by memory address
// Ported from UE5.7 UE::ParkingLot

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/Templates/FunctionWithContext.h"

namespace OloEngine::ParkingLot
{
    // State returned from Wait operations
    struct FWaitState final
    {
        bool bDidWait = false; // True only if CanWait returned true
        bool bDidWake = false; // True only if a Wake call woke the thread, false for timeouts
        u64 WakeToken = 0;     // Optional value provided by WakeOne callback
    };

    // State passed to OnWakeState callback
    struct FWakeState final
    {
        bool bDidWake = false;           // Did a thread wake up?
        bool bHasWaitingThreads = false; // Does the queue MAYBE have another thread waiting?
    };

    // ============================================================================
    // Private namespace - raw function pointer implementations
    // ============================================================================
    namespace Private
    {
        // Wait until woken by a WakeOne/WakeAll call for the given address.
        //
        // @param Address The address to wait on (used as a hash key)
        // @param CanWait Callback returning true if the thread should wait. Called with bucket locked.
        // @param BeforeWait Optional callback invoked after enqueue but before actual wait.
        // @return Wait state indicating if wait occurred and how it ended
        FWaitState Wait(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext);

        // Wait with timeout until woken by a WakeOne/WakeAll call for the given address.
        //
        // @param Address The address to wait on (used as a hash key)
        // @param CanWait Callback returning true if the thread should wait. Called with bucket locked.
        // @param BeforeWait Optional callback invoked after enqueue but before actual wait.
        // @param WaitTime Relative time after which waiting is automatically canceled.
        // @return Wait state indicating if wait occurred and how it ended
        FWaitState WaitFor(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext,
            FMonotonicTimeSpan WaitTime);

        // Wait with timeout until woken by a WakeOne/WakeAll call for the given address.
        //
        // @param Address The address to wait on (used as a hash key)
        // @param CanWait Callback returning true if the thread should wait. Called with bucket locked.
        // @param BeforeWait Optional callback invoked after enqueue but before actual wait.
        // @param WaitTime Absolute time after which waiting is automatically canceled.
        // @return Wait state indicating if wait occurred and how it ended
        FWaitState WaitUntil(
            const void* Address,
            bool (*CanWait)(void* Context),
            void* CanWaitContext,
            void (*BeforeWait)(void* Context),
            void* BeforeWaitContext,
            FMonotonicTimePoint WaitTime);

        // Wake one thread waiting on the given address.
        //
        // @param Address The address to wake
        // @param OnWakeState Optional callback to get wake token, called with bucket locked
        // @param OnWakeStateContext Context for callback
        void WakeOne(
            const void* Address,
            u64 (*OnWakeState)(void* Context, FWakeState State),
            void* OnWakeStateContext);
    } // namespace Private

    // ============================================================================
    // Public API - TFunctionWithContext wrappers (matching UE5.7 API)
    // ============================================================================

    // Queue the calling thread to wait if CanWait returns true. BeforeWait is only called if CanWait returns true.
    //
    // @param Address      Address to use as the key for the queue. The same address is used to wake the thread.
    // @param CanWait      Function called while the queue is locked. A return of false cancels the wait.
    // @param BeforeWait   Function called after the queue is unlocked and before the thread waits.
    inline FWaitState Wait(const void* Address, TFunctionWithContext<bool()> CanWait, TFunctionWithContext<void()> BeforeWait)
    {
        return Private::Wait(Address, CanWait.GetFunction(), CanWait.GetContext(), BeforeWait.GetFunction(), BeforeWait.GetContext());
    }

    // Queue the calling thread to wait if CanWait returns true (raw function pointer version).
    //
    // @param Address             Address to use as the key for the queue. The same address is used to wake the thread.
    // @param CanWait             Function called while the queue is locked. A return of false cancels the wait.
    // @param CanWaitContext      Context for CanWait callback.
    // @param BeforeWait          Function called after the queue is unlocked and before the thread waits.
    // @param BeforeWaitContext   Context for BeforeWait callback.
    inline FWaitState Wait(
        const void* Address,
        bool (*CanWait)(void* Context),
        void* CanWaitContext,
        void (*BeforeWait)(void* Context),
        void* BeforeWaitContext)
    {
        return Private::Wait(Address, CanWait, CanWaitContext, BeforeWait, BeforeWaitContext);
    }

    // Queue the calling thread to wait if CanWait returns true. BeforeWait is only called if CanWait returns true.
    //
    // @param Address      Address to use as the key for the queue. The same address is used to wake the thread.
    // @param CanWait      Function called while the queue is locked. A return of false cancels the wait.
    // @param BeforeWait   Function called after the queue is unlocked and before the thread waits.
    // @param WaitTime     Relative time after which waiting is automatically canceled and the thread wakes.
    inline FWaitState WaitFor(const void* Address, TFunctionWithContext<bool()> CanWait, TFunctionWithContext<void()> BeforeWait, FMonotonicTimeSpan WaitTime)
    {
        return Private::WaitFor(Address, CanWait.GetFunction(), CanWait.GetContext(), BeforeWait.GetFunction(), BeforeWait.GetContext(), WaitTime);
    }

    // Queue the calling thread to wait if CanWait returns true. BeforeWait is only called if CanWait returns true.
    //
    // @param Address      Address to use as the key for the queue. The same address is used to wake the thread.
    // @param CanWait      Function called while the queue is locked. A return of false cancels the wait.
    // @param BeforeWait   Function called after the queue is unlocked and before the thread waits.
    // @param WaitTime     Absolute time after which waiting is automatically canceled and the thread wakes.
    inline FWaitState WaitUntil(const void* Address, TFunctionWithContext<bool()> CanWait, TFunctionWithContext<void()> BeforeWait, FMonotonicTimePoint WaitTime)
    {
        return Private::WaitUntil(Address, CanWait.GetFunction(), CanWait.GetContext(), BeforeWait.GetFunction(), BeforeWait.GetContext(), WaitTime);
    }

    // Simplified Wait that always waits (no condition check).
    inline FWaitState Wait(const void* Address)
    {
        return Private::Wait(Address, nullptr, nullptr, nullptr, nullptr);
    }

    // Queue the calling thread to wait if CanWait returns true (callable version).
    // This is a convenience overload that converts callables to TFunctionWithContext.
    //
    // @param Address      Address to use as the key for the queue. The same address is used to wake the thread.
    // @param CanWait      Callable returning true if the thread should wait. Called with bucket locked.
    template<typename CanWaitFunc>
    inline FWaitState Wait(const void* Address, CanWaitFunc&& CanWait)
    {
        return Wait(Address, TFunctionWithContext<bool()>(std::forward<CanWaitFunc>(CanWait)), TFunctionWithContext<void()>(nullptr));
    }

    // Wake one thread from the queue of threads waiting on the address.
    //
    // @param Address       Address to use as the key for the queue. Must match the address used in Wait.
    // @param OnWakeState   Function called while the queue is locked. Receives the wake state. Returns WakeToken.
    inline void WakeOne(const void* Address, TFunctionWithContext<u64(FWakeState)> OnWakeState)
    {
        return Private::WakeOne(Address, OnWakeState.GetFunction(), OnWakeState.GetContext());
    }

    // Wake one thread from the queue of threads waiting on the address (raw function pointer version).
    //
    // @param Address             Address to use as the key for the queue. Must match the address used in Wait.
    // @param OnWakeState         Function called while the queue is locked. Receives context and wake state. Returns WakeToken.
    // @param OnWakeStateContext  Context passed to the OnWakeState callback.
    inline void WakeOne(
        const void* Address,
        u64 (*OnWakeState)(void* Context, FWakeState State),
        void* OnWakeStateContext)
    {
        return Private::WakeOne(Address, OnWakeState, OnWakeStateContext);
    }

    // Wake one thread from the queue of threads waiting on the address.
    //
    // @param Address   Address to use as the key for the queue. Must match the address used in Wait.
    // @return The wake state, which includes whether a thread woke up and whether there are more queued.
    FWakeState WakeOne(const void* Address);

    // Wake up to WakeCount threads from the queue of threads waiting on the address.
    //
    // @param Address     Address to use as the key for the queue. Must match the address used in Wait.
    // @param WakeCount   The maximum number of threads to wake.
    // @return The number of threads that this call woke up.
    u32 WakeMultiple(const void* Address, u32 WakeCount);

    // Wake all threads from the queue of threads waiting on the address.
    //
    // @param Address   Address to use as the key for the queue. Must match the address used in Wait.
    // @return The number of threads that this call woke up.
    u32 WakeAll(const void* Address);

    // Reserve space in the parking lot for the expected number of threads.
    // Call this early if you know how many threads will be using the parking lot.
    void Reserve(u32 ThreadCount);

} // namespace OloEngine::ParkingLot
