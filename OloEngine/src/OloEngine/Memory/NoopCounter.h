#pragma once
n // @file NoopCounter.h
// @brief No-operation counter for release builds
//
// Provides a fake counter that performs no actual operations.
// Used in shipping/distribution builds where tracking overhead
// should be eliminated but the API needs to remain consistent.
//
// Ported from Unreal Engine's NoopCounter.h

#include "OloEngine/Core/Base.h"
#include <atomic>

    namespace OloEngine
{
    // @class FNoopCounter
    // @brief Fake thread-safe counter with no actual operations
    //
    // Used to avoid cluttering code with #ifdefs when counters are
    // only used for debugging. All operations are no-ops that return 0.
    class FNoopCounter
    {
      public:
        using IntegerType = i32;

        FNoopCounter() = default;
        FNoopCounter(const FNoopCounter& /*Other*/) {}
        FNoopCounter(i32 /*Value*/) {}

        i32 Increment()
        {
            return 0;
        }

        i32 Add(i32 /*Amount*/)
        {
            return 0;
        }

        i32 Decrement()
        {
            return 0;
        }

        i32 Subtract(i32 /*Amount*/)
        {
            return 0;
        }

        i32 Set(i32 /*Value*/)
        {
            return 0;
        }

        i32 Reset()
        {
            return 0;
        }

        i32 GetValue() const
        {
            return 0;
        }
    };

    // @class FAtomicCounter
    // @brief Thread-safe counter using std::atomic
    //
    // Provides the same interface as FNoopCounter but with actual
    // atomic operations. Use this for debug/development builds where
    // tracking allocation counts is useful.
    class FAtomicCounter
    {
      public:
        using IntegerType = i32;

        FAtomicCounter() : m_Counter(0) {}
        FAtomicCounter(const FAtomicCounter& Other) : m_Counter(Other.m_Counter.load(std::memory_order_relaxed)) {}
        explicit FAtomicCounter(i32 Value) : m_Counter(Value) {}

        FAtomicCounter& operator=(const FAtomicCounter& Other)
        {
            m_Counter.store(Other.m_Counter.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }

        // @brief Atomically increment the counter
        // @return The new value after increment
        i32 Increment()
        {
            return m_Counter.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        // @brief Atomically add to the counter
        // @param Amount The amount to add
        // @return The previous value before addition
        i32 Add(i32 Amount)
        {
            return m_Counter.fetch_add(Amount, std::memory_order_relaxed);
        }

        // @brief Atomically decrement the counter
        // @return The new value after decrement
        i32 Decrement()
        {
            return m_Counter.fetch_sub(1, std::memory_order_relaxed) - 1;
        }

        // @brief Atomically subtract from the counter
        // @param Amount The amount to subtract
        // @return The previous value before subtraction
        i32 Subtract(i32 Amount)
        {
            return m_Counter.fetch_sub(Amount, std::memory_order_relaxed);
        }

        // @brief Atomically set the counter to a value
        // @param Value The value to set
        // @return The previous value
        i32 Set(i32 Value)
        {
            return m_Counter.exchange(Value, std::memory_order_relaxed);
        }

        // @brief Atomically reset the counter to zero
        // @return The previous value
        i32 Reset()
        {
            return m_Counter.exchange(0, std::memory_order_relaxed);
        }

        // @brief Get the current counter value
        // @return The current value
        i32 GetValue() const
        {
            return m_Counter.load(std::memory_order_relaxed);
        }

      private:
        std::atomic<i32> m_Counter;
    };

} // namespace OloEngine
