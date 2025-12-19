// CancellationToken.h - Cooperative task cancellation support
// Ported from UE5.7 Tasks/Task.h

#pragma once

#include "OloEngine/Core/Base.h"

#include <atomic>

namespace OloEngine::Tasks
{
    // @class FCancellationToken
    // @brief Support for canceling tasks mid-execution
    // 
    // Usage:
    // @code
    // FCancellationToken Token;
    // Launch("MyTask", [&Token]() {
    //     for (int i = 0; i < 1000000; ++i)
    //     {
    //         if (Token.IsCanceled()) return;
    //         // ... do work ...
    //     }
    // });
    // 
    // // Later:
    // Token.Cancel();
    // @endcode
    // 
    // Notes:
    // - It's the user's decision and responsibility to manage cancellation token lifetime
    // - It's the user's responsibility to check the cancellation token and return early
    // - There's no way to cancel a task to skip its execution completely
    // - Waiting for a canceled task blocks until execution is complete (just returns early)
    // - Canceling a task doesn't affect its subsequents unless they share the same token
    class FCancellationToken
    {
    public:
        FCancellationToken() = default;

        // Non-copyable but movable
        FCancellationToken(const FCancellationToken&) = delete;
        FCancellationToken& operator=(const FCancellationToken&) = delete;
        FCancellationToken(FCancellationToken&&) = default;
        FCancellationToken& operator=(FCancellationToken&&) = default;

        // @brief Request cancellation
        // 
        // This is a cooperative cancellation - the task must check IsCanceled()
        // and honor the cancellation request.
        void Cancel()
        {
            m_bCanceled.store(true, std::memory_order_relaxed);
        }

        // @brief Check if cancellation has been requested
        // @return true if Cancel() has been called
        bool IsCanceled() const
        {
            return m_bCanceled.load(std::memory_order_relaxed);
        }

        // @brief Reset the cancellation state
        // 
        // Allows reuse of the token for a new task.
        void Reset()
        {
            m_bCanceled.store(false, std::memory_order_relaxed);
        }

    private:
        std::atomic<bool> m_bCanceled{false};
    };

    // @class FCancellationTokenScope
    // @brief RAII scope for setting the current thread's cancellation token
    // 
    // This allows nested task code to check for cancellation without
    // explicitly passing the token through the call stack.
    // 
    // Example:
    // @code
    // FCancellationToken Token;
    // Launch("MyTask", [&Token]()
    // {
    //     FCancellationTokenScope Scope(Token);
    //     DoWork(); // Can call FCancellationTokenScope::IsCurrentWorkCanceled()
    // });
    // @endcode
    class FCancellationTokenScope
    {
    public:
        explicit FCancellationTokenScope(FCancellationToken& CancellationToken)
        {
            SetToken(&CancellationToken);
        }

        explicit FCancellationTokenScope(FCancellationToken* CancellationToken)
        {
            SetToken(CancellationToken);
        }

        ~FCancellationTokenScope()
        {
            if (m_bHasActiveScope)
            {
                s_CurrentToken = nullptr;
            }
        }

        // Non-copyable
        FCancellationTokenScope(const FCancellationTokenScope&) = delete;
        FCancellationTokenScope& operator=(const FCancellationTokenScope&) = delete;

        // @brief Get the current thread's cancellation token
        // @return The current cancellation token, or nullptr if none is set
        static FCancellationToken* GetCurrentCancellationToken()
        {
            return s_CurrentToken;
        }

        // @brief Check if the current work has been canceled
        // 
        // Convenience function that checks the current thread's cancellation token.
        // 
        // @return true if there's a current token and it has been canceled
        static bool IsCurrentWorkCanceled()
        {
            if (FCancellationToken* CurrentToken = GetCurrentCancellationToken())
            {
                return CurrentToken->IsCanceled();
            }
            return false;
        }

    private:
        void SetToken(FCancellationToken* CancellationToken)
        {
            if (CancellationToken)
            {
                if (s_CurrentToken != CancellationToken)
                {
                    OLO_CORE_ASSERT(s_CurrentToken == nullptr, 
                        "Nested cancellation token scopes with different tokens are not supported");
                    s_CurrentToken = CancellationToken;
                    m_bHasActiveScope = true;
                }
            }
        }

        bool m_bHasActiveScope = false;
        static inline thread_local FCancellationToken* s_CurrentToken = nullptr;
    };

} // namespace OloEngine::Tasks
