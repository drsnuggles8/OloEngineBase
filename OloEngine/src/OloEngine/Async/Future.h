// Future.h - Promise-based async patterns
// Ported from UE5.7 Async/Future.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Templates/FunctionRef.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <atomic>
#include <memory>
#include <type_traits>

namespace OloEngine
{
    // Forward declarations
    template<typename ResultType>
    class TFuture;
    template<typename ResultType>
    class TSharedFuture;
    template<typename ResultType>
    class TPromise;

    /**
     * @class FFutureState
     * @brief Base class for the internal state of asynchronous return values
     */
    class FFutureState
    {
      public:
        FFutureState() = default;

        FFutureState(TUniqueFunction<void()>&& InCompletionCallback)
            : m_CompletionCallback(MoveTemp(InCompletionCallback))
        {
        }

        virtual ~FFutureState() = default;

        /**
         * @brief Check whether the asynchronous result has been set
         */
        bool IsComplete() const
        {
            return m_bComplete.load(std::memory_order_acquire);
        }

        /**
         * @brief Block until the future result is available
         * @param Duration Maximum time to wait
         * @return true if result is available, false on timeout
         */
        bool WaitFor(FMonotonicTimeSpan Duration) const
        {
            if (IsComplete())
            {
                return true;
            }
            return m_CompletionEvent.WaitFor(Duration);
        }

        /**
         * @brief Block indefinitely until the future result is available
         */
        void Wait() const
        {
            if (!IsComplete())
            {
                m_CompletionEvent.Wait();
            }
        }

        /**
         * @brief Set a continuation to be called on completion
         */
        void SetContinuation(TUniqueFunction<void()>&& Continuation)
        {
            bool bShouldJustRun = IsComplete();
            if (!bShouldJustRun)
            {
                TUniqueLock<FMutex> Lock(m_Mutex);
                bShouldJustRun = IsComplete();
                if (!bShouldJustRun)
                {
                    m_CompletionCallback = MoveTemp(Continuation);
                }
            }
            if (bShouldJustRun && Continuation)
            {
                Continuation();
            }
        }

      protected:
        void MarkComplete()
        {
            TUniqueFunction<void()> Continuation;
            {
                TUniqueLock<FMutex> Lock(m_Mutex);
                Continuation = MoveTemp(m_CompletionCallback);
                m_bComplete.store(true, std::memory_order_release);
            }
            m_CompletionEvent.Notify();

            if (Continuation)
            {
                Continuation();
            }
        }

      private:
        mutable FMutex m_Mutex;
        TUniqueFunction<void()> m_CompletionCallback;
        mutable FManualResetEvent m_CompletionEvent;
        std::atomic<bool> m_bComplete{ false };
    };

    /**
     * @class TFutureState
     * @brief Typed internal state for futures with a result value
     */
    template<typename ResultType>
    class TFutureState : public FFutureState
    {
      public:
        TFutureState() = default;

        TFutureState(TUniqueFunction<void()>&& CompletionCallback)
            : FFutureState(MoveTemp(CompletionCallback))
        {
        }

        ~TFutureState()
        {
            if (IsComplete() && m_bHasResult)
            {
                reinterpret_cast<ResultType*>(&m_Result)->~ResultType();
            }
        }

        /**
         * @brief Get the result (blocks until available)
         */
        ResultType& GetResult()
        {
            Wait();
            OLO_CORE_ASSERT(m_bHasResult, "Future result not set");
            return *reinterpret_cast<ResultType*>(&m_Result);
        }

        const ResultType& GetResult() const
        {
            return const_cast<TFutureState*>(this)->GetResult();
        }

        /**
         * @brief Set the result and notify waiters
         */
        template<typename... Args>
        void EmplaceResult(Args&&... InArgs)
        {
            new (&m_Result) ResultType(Forward<Args>(InArgs)...);
            m_bHasResult = true;
            MarkComplete();
        }

      private:
        alignas(ResultType) u8 m_Result[sizeof(ResultType)];
        bool m_bHasResult = false;
    };

    // Specialization for void
    template<>
    class TFutureState<void> : public FFutureState
    {
      public:
        TFutureState() = default;

        TFutureState(TUniqueFunction<void()>&& CompletionCallback)
            : FFutureState(MoveTemp(CompletionCallback))
        {
        }

        void GetResult()
        {
            Wait();
        }

        void EmplaceResult()
        {
            MarkComplete();
        }
    };

    /**
     * @class TFuture
     * @brief A future represents an asynchronous result that will be available later
     *
     * Use TFuture to receive the result of an asynchronous operation.
     * The result can be retrieved with Get() which blocks until available.
     *
     * Example:
     * @code
     * TPromise<int> Promise;
     * TFuture<int> Future = Promise.GetFuture();
     *
     * // On another thread or later:
     * Promise.SetValue(42);
     *
     * // This blocks until value is set:
     * int Result = Future.Get();
     * @endcode
     */
    template<typename ResultType>
    class TFuture
    {
      public:
        TFuture() = default;

        TFuture(const TFuture&) = delete;
        TFuture& operator=(const TFuture&) = delete;

        TFuture(TFuture&& Other) noexcept
            : m_State(MoveTemp(Other.m_State))
        {
        }

        TFuture& operator=(TFuture&& Other) noexcept
        {
            m_State = MoveTemp(Other.m_State);
            return *this;
        }

        /**
         * @brief Check if this future is valid
         */
        bool IsValid() const
        {
            return m_State != nullptr;
        }

        /**
         * @brief Check if the result is ready
         */
        bool IsReady() const
        {
            return IsValid() && m_State->IsComplete();
        }

        /**
         * @brief Get the result as const reference (blocks until available)
         * @note Not equivalent to std::future::get(). The future remains valid.
         */
        const ResultType& Get() const
        {
            OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid future");
            return m_State->GetResult();
        }

        /**
         * @brief Get the result as mutable reference (blocks until available)
         * @note Not equivalent to std::future::get(). The future remains valid.
         */
        ResultType& GetMutable()
        {
            OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid future");
            return m_State->GetResult();
        }

        /**
         * @brief Consumes the future's result and invalidates the future
         * @return The result by value
         * @note Equivalent to std::future::get(). Invalidates the future.
         */
        ResultType Consume()
        {
            TFuture Local(MoveTemp(*this));
            return MoveTemp(Local.GetMutable());
        }

        /**
         * @brief Moves this future's state into a shared future
         * @return The shared future object
         */
        TSharedFuture<ResultType> Share();

        /**
         * @brief Wait for the result with timeout
         * @return true if ready, false on timeout
         */
        bool WaitFor(FMonotonicTimeSpan Duration) const
        {
            return IsValid() && m_State->WaitFor(Duration);
        }

        /**
         * @brief Wait for the result until a specific time point
         * @param Time The time point to wait until
         * @return true if ready, false on timeout
         */
        bool WaitUntil(FMonotonicTimePoint Time) const
        {
            FMonotonicTimeSpan Remaining = Time - FMonotonicTimePoint::Now();
            if (Remaining <= FMonotonicTimeSpan::Zero())
            {
                return IsReady();
            }
            return WaitFor(Remaining);
        }

        /**
         * @brief Wait indefinitely for the result
         */
        void Wait() const
        {
            if (IsValid())
            {
                m_State->Wait();
            }
        }

        /**
         * @brief Reset the future, removing any continuation and invalidating it
         */
        void Reset()
        {
            if (IsValid())
            {
                m_State->SetContinuation(nullptr);
                m_State.reset();
            }
        }

        /**
         * @brief Set a continuation to be called when result is ready
         * @param Func A function taking TFuture<ResultType>
         */
        template<typename FuncType>
        auto Then(FuncType&& Func)
        {
            using NextResultType = std::invoke_result_t<FuncType, TFuture<ResultType>>;

            TPromise<NextResultType> NextPromise;
            TFuture<NextResultType> NextFuture = NextPromise.GetFuture();

            auto StateCapture = m_State;
            m_State->SetContinuation(
                [Promise = MoveTemp(NextPromise),
                 State = MoveTemp(StateCapture),
                 Continuation = Forward<FuncType>(Func)]() mutable
                {
                    TFuture<ResultType> CompletedFuture(MoveTemp(State));
                    if constexpr (std::is_void_v<NextResultType>)
                    {
                        Continuation(MoveTemp(CompletedFuture));
                        Promise.SetValue();
                    }
                    else
                    {
                        Promise.SetValue(Continuation(MoveTemp(CompletedFuture)));
                    }
                });

            // Invalidate this future after setting continuation
            m_State.reset();
            return NextFuture;
        }

        /**
         * @brief Convenience wrapper for Then that takes a function accepting ResultType
         * @param Func A function taking ResultType (by value via Consume)
         */
        template<typename FuncType>
        auto Next(FuncType&& Func)
        {
            return Then([Continuation = Forward<FuncType>(Func)](TFuture<ResultType> Self) mutable
                        { return Continuation(Self.Consume()); });
        }

      private:
        template<typename>
        friend class TPromise;
        template<typename>
        friend class TSharedFuture;

        explicit TFuture(std::shared_ptr<TFutureState<ResultType>> InState)
            : m_State(MoveTemp(InState))
        {
        }

        std::shared_ptr<TFutureState<ResultType>> m_State;
    };

    // Specialization for void
    template<>
    class TFuture<void>
    {
      public:
        TFuture() = default;

        TFuture(const TFuture&) = delete;
        TFuture& operator=(const TFuture&) = delete;

        TFuture(TFuture&& Other) noexcept
            : m_State(MoveTemp(Other.m_State))
        {
        }

        TFuture& operator=(TFuture&& Other) noexcept
        {
            m_State = MoveTemp(Other.m_State);
            return *this;
        }

        bool IsValid() const
        {
            return m_State != nullptr;
        }
        bool IsReady() const
        {
            return IsValid() && m_State->IsComplete();
        }

        void Get() const
        {
            OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid future");
            m_State->Wait();
        }

        void Consume()
        {
            TFuture Local(MoveTemp(*this));
            Local.Get();
        }

        TSharedFuture<void> Share();

        bool WaitFor(FMonotonicTimeSpan Duration) const
        {
            return IsValid() && m_State->WaitFor(Duration);
        }

        bool WaitUntil(FMonotonicTimePoint Time) const
        {
            FMonotonicTimeSpan Remaining = Time - FMonotonicTimePoint::Now();
            if (Remaining <= FMonotonicTimeSpan::Zero())
            {
                return IsReady();
            }
            return WaitFor(Remaining);
        }

        void Wait() const
        {
            if (IsValid())
            {
                m_State->Wait();
            }
        }

        void Reset()
        {
            if (IsValid())
            {
                m_State->SetContinuation(nullptr);
                m_State.reset();
            }
        }

        template<typename FuncType>
        auto Then(FuncType&& Func)
        {
            using NextResultType = std::invoke_result_t<FuncType, TFuture<void>>;

            TPromise<NextResultType> NextPromise;
            TFuture<NextResultType> NextFuture = NextPromise.GetFuture();

            auto StateCapture = m_State;
            m_State->SetContinuation(
                [Promise = MoveTemp(NextPromise),
                 State = MoveTemp(StateCapture),
                 Continuation = Forward<FuncType>(Func)]() mutable
                {
                    TFuture<void> CompletedFuture(MoveTemp(State));
                    if constexpr (std::is_void_v<NextResultType>)
                    {
                        Continuation(MoveTemp(CompletedFuture));
                        Promise.SetValue();
                    }
                    else
                    {
                        Promise.SetValue(Continuation(MoveTemp(CompletedFuture)));
                    }
                });

            m_State.reset();
            return NextFuture;
        }

        template<typename FuncType>
        auto Next(FuncType&& Func)
        {
            return Then([Continuation = Forward<FuncType>(Func)](TFuture<void> Self) mutable
                        {
                Self.Consume();
                return Continuation(); });
        }

      private:
        template<typename>
        friend class TPromise;
        template<typename>
        friend class TSharedFuture;

        explicit TFuture(std::shared_ptr<TFutureState<void>> InState)
            : m_State(MoveTemp(InState))
        {
        }

        std::shared_ptr<TFutureState<void>> m_State;
    };

    /**
     * @class TSharedFuture
     * @brief A shared future that can be copied and shared between multiple consumers
     *
     * Unlike TFuture which is move-only, TSharedFuture can be copied and allows
     * multiple threads to wait on the same result.
     *
     * Example:
     * @code
     * TFuture<int> Future = GetAsyncResult();
     * TSharedFuture<int> SharedFuture = Future.Share();
     *
     * // Both threads can now wait on the same result
     * TSharedFuture<int> Copy = SharedFuture;
     * @endcode
     */
    template<typename ResultType>
    class TSharedFuture
    {
      public:
        TSharedFuture() = default;

        /**
         * @brief Create from a TFuture (takes ownership)
         */
        TSharedFuture(TFuture<ResultType>&& Future)
            : m_State(MoveTemp(Future.m_State))
        {
        }

        // Copyable
        TSharedFuture(const TSharedFuture&) = default;
        TSharedFuture& operator=(const TSharedFuture&) = default;

        // Movable
        TSharedFuture(TSharedFuture&& Other) noexcept = default;
        TSharedFuture& operator=(TSharedFuture&& Other) noexcept = default;

        /**
         * @brief Check if this shared future is valid
         */
        bool IsValid() const
        {
            return m_State != nullptr;
        }

        /**
         * @brief Check if the result is ready
         */
        bool IsReady() const
        {
            return IsValid() && m_State->IsComplete();
        }

        /**
         * @brief Get the result as const reference (blocks until available)
         * @note The shared future remains valid after this call
         */
        const ResultType& Get() const
        {
            OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid shared future");
            return m_State->GetResult();
        }

        /**
         * @brief Wait for the result with timeout
         * @return true if ready, false on timeout
         */
        bool WaitFor(FMonotonicTimeSpan Duration) const
        {
            return IsValid() && m_State->WaitFor(Duration);
        }

        /**
         * @brief Wait for the result until a specific time point
         * @param Time The time point to wait until
         * @return true if ready, false on timeout
         */
        bool WaitUntil(FMonotonicTimePoint Time) const
        {
            FMonotonicTimeSpan Remaining = Time - FMonotonicTimePoint::Now();
            if (Remaining <= FMonotonicTimeSpan::Zero())
            {
                return IsReady();
            }
            return WaitFor(Remaining);
        }

        /**
         * @brief Wait indefinitely for the result
         */
        void Wait() const
        {
            if (IsValid())
            {
                m_State->Wait();
            }
        }

      private:
        std::shared_ptr<TFutureState<ResultType>> m_State;
    };

    // Specialization for void
    template<>
    class TSharedFuture<void>
    {
      public:
        TSharedFuture() = default;

        TSharedFuture(TFuture<void>&& Future)
            : m_State(MoveTemp(Future.m_State))
        {
        }

        TSharedFuture(const TSharedFuture&) = default;
        TSharedFuture& operator=(const TSharedFuture&) = default;
        TSharedFuture(TSharedFuture&& Other) noexcept = default;
        TSharedFuture& operator=(TSharedFuture&& Other) noexcept = default;

        bool IsValid() const
        {
            return m_State != nullptr;
        }
        bool IsReady() const
        {
            return IsValid() && m_State->IsComplete();
        }

        void Get() const
        {
            OLO_CORE_ASSERT(IsValid(), "Cannot get result from invalid shared future");
            m_State->Wait();
        }

        bool WaitFor(FMonotonicTimeSpan Duration) const
        {
            return IsValid() && m_State->WaitFor(Duration);
        }

        bool WaitUntil(FMonotonicTimePoint Time) const
        {
            FMonotonicTimeSpan Remaining = Time - FMonotonicTimePoint::Now();
            if (Remaining <= FMonotonicTimeSpan::Zero())
            {
                return IsReady();
            }
            return WaitFor(Remaining);
        }

        void Wait() const
        {
            if (IsValid())
            {
                m_State->Wait();
            }
        }

      private:
        std::shared_ptr<TFutureState<void>> m_State;
    };

    // Implement Share() methods now that TSharedFuture is defined
    template<typename ResultType>
    TSharedFuture<ResultType> TFuture<ResultType>::Share()
    {
        return TSharedFuture<ResultType>(MoveTemp(*this));
    }

    inline TSharedFuture<void> TFuture<void>::Share()
    {
        return TSharedFuture<void>(MoveTemp(*this));
    }

    /**
     * @class TPromise
     * @brief A promise is used to set the result of a TFuture
     *
     * Example:
     * @code
     * TPromise<int> Promise;
     * TFuture<int> Future = Promise.GetFuture();
     *
     * // Later, set the result:
     * Promise.SetValue(42);
     * @endcode
     */
    template<typename ResultType>
    class TPromise
    {
      public:
        TPromise()
            : m_State(std::make_shared<TFutureState<ResultType>>())
        {
        }

        TPromise(const TPromise&) = delete;
        TPromise& operator=(const TPromise&) = delete;

        TPromise(TPromise&& Other) noexcept
            : m_State(MoveTemp(Other.m_State))
        {
        }

        TPromise& operator=(TPromise&& Other) noexcept
        {
            m_State = MoveTemp(Other.m_State);
            return *this;
        }

        /**
         * @brief Get the future associated with this promise
         *
         * This should only be called once per promise.
         */
        TFuture<ResultType> GetFuture()
        {
            OLO_CORE_ASSERT(m_State, "Promise already moved");
            return TFuture<ResultType>(m_State);
        }

        /**
         * @brief Set the result value
         */
        template<typename T>
        void SetValue(T&& Value)
        {
            OLO_CORE_ASSERT(m_State, "Promise already moved");
            m_State->EmplaceResult(Forward<T>(Value));
        }

        /**
         * @brief Emplace the result with constructor arguments
         */
        template<typename... Args>
        void EmplaceValue(Args&&... InArgs)
        {
            OLO_CORE_ASSERT(m_State, "Promise already moved");
            m_State->EmplaceResult(Forward<Args>(InArgs)...);
        }

      private:
        std::shared_ptr<TFutureState<ResultType>> m_State;
    };

    // Specialization for void
    template<>
    class TPromise<void>
    {
      public:
        TPromise()
            : m_State(std::make_shared<TFutureState<void>>())
        {
        }

        TPromise(const TPromise&) = delete;
        TPromise& operator=(const TPromise&) = delete;

        TPromise(TPromise&& Other) noexcept
            : m_State(MoveTemp(Other.m_State))
        {
        }

        TPromise& operator=(TPromise&& Other) noexcept
        {
            m_State = MoveTemp(Other.m_State);
            return *this;
        }

        TFuture<void> GetFuture()
        {
            OLO_CORE_ASSERT(m_State, "Promise already moved");
            return TFuture<void>(m_State);
        }

        void SetValue()
        {
            OLO_CORE_ASSERT(m_State, "Promise already moved");
            m_State->EmplaceResult();
        }

      private:
        std::shared_ptr<TFutureState<void>> m_State;
    };

    /**
     * @brief Create a future that is immediately ready with a value
     */
    template<typename ResultType>
    TFuture<std::decay_t<ResultType>> MakeReadyFuture(ResultType&& Value)
    {
        TPromise<std::decay_t<ResultType>> Promise;
        Promise.SetValue(Forward<ResultType>(Value));
        return Promise.GetFuture();
    }

    /**
     * @brief Create a void future that is immediately ready
     */
    inline TFuture<void> MakeReadyFuture()
    {
        TPromise<void> Promise;
        Promise.SetValue();
        return Promise.GetFuture();
    }

} // namespace OloEngine
