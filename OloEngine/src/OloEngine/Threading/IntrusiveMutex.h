// IntrusiveMutex.h - Generic intrusive mutex template
// Ported from UE5.7 UE::TIntrusiveMutex

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/ParkingLot.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/Task/Oversubscription.h"
#include <atomic>
#include <type_traits>
#include <concepts>

namespace OloEngine
{
    // Helper to get state type from params
    template<typename ParamsType>
    using TIntrusiveMutexStateType_T = std::decay_t<decltype(ParamsType::IsLockedFlag)>;

    // Concept for intrusive mutex parameters
    template<typename ParamsType, typename StateType = TIntrusiveMutexStateType_T<ParamsType>>
    concept CIntrusiveMutexParams = requires {
        requires std::integral<StateType>;

        // Required: constexpr static StateType IsLockedFlag = ...;
        // Flag that is set in the state when the mutex is locked.
        { ParamsType::IsLockedFlag } -> std::convertible_to<StateType>;

        // Required: constexpr static StateType MayHaveWaitingLockFlag = ...;
        // Flag that is set in the state when a thread may be waiting to lock the mutex.
        { ParamsType::MayHaveWaitingLockFlag } -> std::convertible_to<StateType>;
    };

    // A 2-bit intrusive mutex that is not fair and does not support recursive locking.
    //
    // All bits of the state referenced by IsLockedFlag, IsLockedMask, and MayHaveWaitingLockFlag
    // must be initialized to 0 or to values that are consistent with the functions being called.
    template<CIntrusiveMutexParams ParamsType>
    class TIntrusiveMutex
    {
        TIntrusiveMutex() = delete;

        using StateType = TIntrusiveMutexStateType_T<ParamsType>;

        static constexpr StateType IsLockedFlag = ParamsType::IsLockedFlag;
        static constexpr StateType MayHaveWaitingLockFlag = ParamsType::MayHaveWaitingLockFlag;

        static constexpr StateType IsLockedMask = []
        {
            if constexpr (requires { ParamsType::IsLockedMask; })
            {
                return ParamsType::IsLockedMask;
            }
            else
            {
                return ParamsType::IsLockedFlag;
            }
        }();

        static constexpr i32 SpinLimit = []
        {
            if constexpr (requires { ParamsType::SpinLimit; })
            {
                return ParamsType::SpinLimit;
            }
            else
            {
                return 40;
            }
        }();

        static_assert(IsLockedFlag && (IsLockedFlag & (IsLockedFlag - 1)) == 0, "IsLockedFlag must be one bit.");
        static_assert(MayHaveWaitingLockFlag && (MayHaveWaitingLockFlag & (MayHaveWaitingLockFlag - 1)) == 0, "MayHaveWaitingLockFlag must be one bit.");
        static_assert(IsLockedFlag != MayHaveWaitingLockFlag, "IsLockedFlag and MayHaveWaitingLockFlag must be different bits.");
        static_assert((IsLockedMask & IsLockedFlag) == IsLockedFlag, "IsLockedMask must contain IsLockedFlag.");
        static_assert((IsLockedMask & MayHaveWaitingLockFlag) == 0, "IsLockedMask must not contain MayHaveWaitingLockFlag.");
        static_assert(SpinLimit >= 0, "SpinLimit must be non-negative.");

        OLO_FINLINE static const void* GetWaitAddress(const std::atomic<StateType>& State)
        {
            if constexpr (requires { ParamsType::GetWaitAddress; })
            {
                return ParamsType::GetWaitAddress(State);
            }
            else
            {
                return &State;
            }
        }

      public:
        [[nodiscard]] OLO_FINLINE static bool IsLocked(const std::atomic<StateType>& State)
        {
            return !!(State.load(std::memory_order_relaxed) & IsLockedFlag);
        }

        [[nodiscard]] OLO_FINLINE static bool TryLock(std::atomic<StateType>& State)
        {
            StateType Expected = State.load(std::memory_order_relaxed);
            return !(Expected & IsLockedMask) &&
                   State.compare_exchange_strong(Expected, Expected | IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed);
        }

        OLO_FINLINE static void Lock(std::atomic<StateType>& State)
        {
            StateType Expected = State.load(std::memory_order_relaxed) & ~IsLockedMask & ~MayHaveWaitingLockFlag;
            if (OLO_LIKELY(State.compare_exchange_weak(Expected, Expected | IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed)))
            {
                return;
            }
            LockSlow(State);
        }

        OLO_FINLINE static void LockLoop(std::atomic<StateType>& State)
        {
            i32 SpinCount = 0;
            for (StateType CurrentState = State.load(std::memory_order_relaxed);;)
            {
                // Try to acquire the lock if it was unlocked, even if there are waiting threads.
                // Acquiring the lock despite the waiting threads means that this lock is not FIFO and thus not fair.
                if (OLO_LIKELY(!(CurrentState & IsLockedMask)))
                {
                    if (OLO_LIKELY(State.compare_exchange_weak(CurrentState, CurrentState | IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed)))
                    {
                        return;
                    }
                    continue;
                }

                // Spin up to the spin limit while there are no waiting threads.
                if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag) && SpinCount < SpinLimit))
                {
                    FPlatformProcess::Yield();
                    ++SpinCount;
                    CurrentState = State.load(std::memory_order_relaxed);
                    continue;
                }

                // Store that there are waiting threads. Restart if the state has changed since it was loaded.
                if (OLO_LIKELY(!(CurrentState & MayHaveWaitingLockFlag)))
                {
                    if (OLO_UNLIKELY(!State.compare_exchange_weak(CurrentState, CurrentState | MayHaveWaitingLockFlag, std::memory_order_relaxed)))
                    {
                        continue;
                    }
                    CurrentState |= MayHaveWaitingLockFlag;
                }

                // Do not enter oversubscription during a wait on a mutex since the wait is generally too short
                // for it to matter and it can worsen performance a lot for heavily contended locks.
                LowLevelTasks::Private::FOversubscriptionAllowedScope _(false);

                // Wait if the state has not changed. Either way, loop back and try to acquire the lock after trying to wait.
                // Match UE5.7: use lambda capture by reference
                ParkingLot::Wait(GetWaitAddress(State), [&State]
                                 {
					const StateType NewState = State.load(std::memory_order_relaxed);
					return (NewState & IsLockedMask) && (NewState & MayHaveWaitingLockFlag); }, nullptr);
                CurrentState = State.load(std::memory_order_relaxed);
            }
        }

        OLO_FINLINE static void Unlock(std::atomic<StateType>& State)
        {
            // Unlock immediately to allow other threads to acquire the lock while this thread looks for a thread to wake.
            const StateType LastState = State.fetch_sub(IsLockedFlag, std::memory_order_release);
            if (OLO_LIKELY(!(LastState & MayHaveWaitingLockFlag)))
            {
                return;
            }
            UnlockSlow(State);
        }

        OLO_FINLINE static void WakeWaitingThread(std::atomic<StateType>& State)
        {
            // Match UE5.7: use lambda capture by reference
            ParkingLot::WakeOne(GetWaitAddress(State), [&State](ParkingLot::FWakeState WakeState) -> u64
                                {
				if (!WakeState.bDidWake)
				{
					// Keep the flag until no thread wakes, otherwise shared locks may win before
					// an exclusive lock has a chance.
					State.fetch_and(static_cast<StateType>(~MayHaveWaitingLockFlag), std::memory_order_relaxed);
				}
				return 0; });
        }

        [[nodiscard]] OLO_FINLINE static bool TryWakeWaitingThread(std::atomic<StateType>& State)
        {
            bool bDidWake = false;
            // Match UE5.7: use lambda capture by reference
            ParkingLot::WakeOne(GetWaitAddress(State), [&State, &bDidWake](ParkingLot::FWakeState WakeState) -> u64
                                {
				if (!WakeState.bDidWake)
				{
					// Keep the flag until no thread wakes, otherwise shared locks may win before
					// an exclusive lock has a chance.
					State.fetch_and(static_cast<StateType>(~MayHaveWaitingLockFlag), std::memory_order_relaxed);
				}
				bDidWake = WakeState.bDidWake;
				return 0; });
            return bDidWake;
        }

      private:
        OLO_NOINLINE static void LockSlow(std::atomic<StateType>& State)
        {
            LockLoop(State);
        }

        OLO_NOINLINE static void UnlockSlow(std::atomic<StateType>& State)
        {
            WakeWaitingThread(State);
        }
    };

} // namespace OloEngine
