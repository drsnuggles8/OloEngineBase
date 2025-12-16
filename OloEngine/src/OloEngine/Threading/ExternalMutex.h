// ExternalMutex.h - External state mutex
// Ported from UE5.7 UE::TExternalMutex

#pragma once

/**
 * @file ExternalMutex.h
 * @brief A 2-bit mutex with externally stored state
 *
 * This allows embedding lock state in an existing atomic variable,
 * useful when you want to add locking to a data structure without
 * additional memory overhead.
 *
 * Ported from Unreal Engine 5.7
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Threading/IntrusiveMutex.h"

#include <atomic>

namespace OloEngine
{
    /**
     * @class TExternalMutex
     * @brief A 2-bit mutex, with its state stored externally, that is not fair and does not support recursive locking.
     *
     * The 2 bits referenced by IsLockedFlag and MayHaveWaitingLockFlag must be initialized to 0 by the owner of
     * the state prior to using it as an external mutex.
     *
     * It is valid to construct more than one TExternalMutex for a given state and to use them concurrently.
     * It is valid to use TExternalMutex exclusively as a temporary, e.g., TExternalMutex(State).Lock();
     *
     * @tparam ParamsType Parameter type that satisfies CIntrusiveMutexParams concept
     *
     * Example usage:
     * @code
     *     struct FMyParams
     *     {
     *         inline constexpr static uint8 IsLockedFlag = 1 << 0;
     *         inline constexpr static uint8 MayHaveWaitingLockFlag = 1 << 1;
     *     };
     *
     *     std::atomic<uint8> MyState{0};
     *     
     *     // Can use as temporary
     *     TExternalMutex<FMyParams>(MyState).Lock();
     *     // ... critical section ...
     *     TExternalMutex<FMyParams>(MyState).Unlock();
     *     
     *     // Or construct once and reuse
     *     TExternalMutex<FMyParams> MyMutex(MyState);
     *     MyMutex.Lock();
     *     // ... critical section ...
     *     MyMutex.Unlock();
     * @endcode
     */
    template <CIntrusiveMutexParams ParamsType>
    class TExternalMutex final
    {
        using StateType = TIntrusiveMutexStateType_T<ParamsType>;

    public:
        TExternalMutex(const TExternalMutex&) = delete;
        TExternalMutex& operator=(const TExternalMutex&) = delete;

        /**
         * @brief Construct an external mutex referencing the given state
         * @param InState Atomic variable containing the lock bits
         */
        OLO_FINLINE constexpr explicit TExternalMutex(std::atomic<StateType>& InState)
            : m_State(InState)
        {
        }

        /**
         * @brief Check if the mutex is currently locked
         * @return True if locked
         */
        [[nodiscard]] OLO_FINLINE bool IsLocked() const
        {
            return TIntrusiveMutex<ParamsType>::IsLocked(m_State);
        }

        /**
         * @brief Attempt to acquire the lock without blocking
         * @return True if the lock was acquired
         */
        [[nodiscard]] OLO_FINLINE bool TryLock()
        {
            return TIntrusiveMutex<ParamsType>::TryLock(m_State);
        }

        /**
         * @brief Acquire the lock, blocking if necessary
         */
        OLO_FINLINE void Lock()
        {
            TIntrusiveMutex<ParamsType>::Lock(m_State);
        }

        /**
         * @brief Release the lock
         */
        OLO_FINLINE void Unlock()
        {
            TIntrusiveMutex<ParamsType>::Unlock(m_State);
        }

    private:
        std::atomic<StateType>& m_State;
    };

    namespace Private
    {
        /**
         * @struct FExternalMutexParams
         * @brief Default parameters for FExternalMutex
         */
        struct FExternalMutexParams
        {
            inline constexpr static u8 IsLockedFlag = 1 << 0;
            inline constexpr static u8 MayHaveWaitingLockFlag = 1 << 1;
        };

    } // namespace Private

    /**
     * @brief Default external mutex using u8 state
     * @deprecated Use TExternalMutex or TIntrusiveMutex directly
     */
    using FExternalMutex [[deprecated("Use TExternalMutex or TIntrusiveMutex.")]] = TExternalMutex<Private::FExternalMutexParams>;

} // namespace OloEngine

