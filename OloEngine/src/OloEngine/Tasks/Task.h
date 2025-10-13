#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "TaskPriority.h"

#include <atomic>
#include <functional>

namespace OloEngine
{
    /**
     * @brief Task execution state
     * 
     * State transitions are atomic and follow this pattern:
     * Ready -> Scheduled -> Running -> Completed
     * 
     * Retraction optimization allows: Scheduled -> Ready -> Running -> Completed
     */
    enum class ETaskState : u8
    {
        Ready,      ///< Task created, ready to launch
        Scheduled,  ///< Task queued for execution (in local or global queue)
        Running,    ///< Task currently executing
        Completed   ///< Task finished execution
    };

    /**
     * @brief Base class for all tasks in the task system
     * 
     * Tasks represent units of work that can be executed asynchronously by worker threads.
     * They support:
     * - Type-erased callables (lambdas, functions, functors)
     * - Small task optimization (inline storage for captures <= 64 bytes)
     * - Atomic state transitions
     * - Priority-based scheduling
     * - Debug naming for profiling
     * 
     * Tasks are reference-counted and should be managed via Ref<Task>.
     */
    class Task : public RefCounted
    {
    public:
        /**
         * @brief Virtual destructor for polymorphic cleanup
         */
        virtual ~Task() = default;

        /**
         * @brief Execute the task body
         * 
         * This is called by worker threads when the task is dequeued.
         * The state must be Running when this is called.
         */
        virtual void Execute() = 0;

        /**
         * @brief Get the current execution state
         * @return Current task state
         */
        ETaskState GetState() const 
        { 
            return m_State.load(std::memory_order_acquire); 
        }

        /**
         * @brief Check if the task has completed execution
         * @return True if task is in Completed state
         */
        bool IsCompleted() const 
        { 
            return GetState() == ETaskState::Completed; 
        }

        /**
         * @brief Get the task priority
         * @return Task priority level
         */
        ETaskPriority GetPriority() const 
        { 
            return m_Priority; 
        }

        /**
         * @brief Get the debug name for profiling and logging
         * @return Debug name string (may be nullptr)
         */
        const char* GetDebugName() const 
        { 
            return m_DebugName; 
        }

        /**
         * @brief Attempt to transition task state
         * @param expected The expected current state (updated on failure)
         * @param desired The desired new state
         * @return True if transition succeeded, false otherwise
         */
        bool TryTransitionState(ETaskState& expected, ETaskState desired)
        {
            return m_State.compare_exchange_strong(expected, desired, 
                std::memory_order_acq_rel, std::memory_order_acquire);
        }

        /**
         * @brief Force set the task state (used internally)
         * @param state The new state
         */
        void SetState(ETaskState state)
        {
            m_State.store(state, std::memory_order_release);
        }

    protected:
        /**
         * @brief Protected constructor - use ExecutableTask to create tasks
         * @param debugName Debug name for profiling (lifetime must exceed task)
         * @param priority Task priority level
         */
        Task(const char* debugName, ETaskPriority priority)
            : m_DebugName(debugName)
            , m_Priority(priority)
            , m_State(ETaskState::Ready)
        {
        }

    protected:
        const char* m_DebugName;                    ///< Debug name (not owned, must be string literal or long-lived)
        ETaskPriority m_Priority;                   ///< Task priority level
        std::atomic<ETaskState> m_State;            ///< Current execution state
    };

    /**
     * @brief Template task implementation with type-erased callable
     * 
     * This template wraps any callable (lambda, function, functor) and provides
     * small task optimization to avoid heap allocations for small captures.
     * 
     * @tparam Callable The callable type (lambda, std::function, etc.)
     */
    template<typename Callable>
    class ExecutableTask : public Task
    {
    public:
        /**
         * @brief Size threshold for inline storage optimization
         * 
         * Captures <= 64 bytes are stored inline, avoiding heap allocation.
         * This covers ~80% of typical task lambdas in practice.
         */
        static constexpr sizet InlineStorageSize = 64;

        /**
         * @brief Create an executable task from a callable
         * @param debugName Debug name for profiling
         * @param priority Task priority
         * @param func The callable to execute (moved into task)
         */
        template<typename F>
        ExecutableTask(const char* debugName, ETaskPriority priority, F&& func)
            : Task(debugName, priority)
            , m_UsesInlineStorage(sizeof(Callable) <= InlineStorageSize)
        {
            if (m_UsesInlineStorage)
            {
                // Small functor - use inline storage (no heap allocation)
                new (m_InlineStorage) Callable(std::forward<F>(func));
            }
            else
            {
                // Large functor - heap allocate
                m_HeapAllocated = new Callable(std::forward<F>(func));
            }
        }

        /**
         * @brief Destructor - clean up callable storage
         */
        ~ExecutableTask() override
        {
            if (m_UsesInlineStorage)
            {
                // Call destructor for inline-stored callable
                reinterpret_cast<Callable*>(m_InlineStorage)->~Callable();
            }
            else
            {
                // Delete heap-allocated callable
                delete m_HeapAllocated;
            }
        }

        /**
         * @brief Execute the wrapped callable
         */
        void Execute() override
        {
            // Note: Can't use OLO_PROFILE_SCOPE with runtime string
            // Tracy integration will be added in Phase 7 with proper handling
            
            if (m_UsesInlineStorage)
            {
                (*reinterpret_cast<Callable*>(m_InlineStorage))();
            }
            else
            {
                (*m_HeapAllocated)();
            }
        }

        /**
         * @brief Check if this task uses inline storage
         * @return True if using inline storage, false if heap-allocated
         */
        bool UsesInlineStorage() const { return m_UsesInlineStorage; }

    private:
        union
        {
            alignas(Callable) u8 m_InlineStorage[InlineStorageSize];  ///< Inline storage for small callables
            Callable* m_HeapAllocated;                                 ///< Heap storage for large callables
        };
        
        bool m_UsesInlineStorage;  ///< True if using inline storage, false if heap
    };

    /**
     * @brief Create a task from any callable
     * 
     * This is a convenience factory function that deduces the callable type
     * and creates the appropriate ExecutableTask.
     * 
     * @tparam Callable The callable type (deduced)
     * @param debugName Debug name for profiling and logging
     * @param priority Task priority level
     * @param func The callable to execute
     * @return Reference-counted task pointer
     */
    template<typename Callable>
    Ref<Task> CreateTask(const char* debugName, ETaskPriority priority, Callable&& func)
    {
        using DecayedCallable = std::decay_t<Callable>;
        return Ref<ExecutableTask<DecayedCallable>>::Create(
            debugName, 
            priority, 
            std::forward<Callable>(func)
        );
    }

} // namespace OloEngine
