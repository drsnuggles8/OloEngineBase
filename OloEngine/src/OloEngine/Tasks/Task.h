#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "TaskPriority.h"

#include <atomic>
#include <functional>
#include <unordered_set>
#include <concepts>

namespace OloEngine
{
    /**
     * @brief Concept for valid task callables
     * 
     * A task callable must be:
     * - Invocable with no arguments
     * - Return void (tasks don't return values, use shared state instead)
     * - Move-constructible (to be stored in the task)
     * 
     * This provides clear compile-time errors when users pass invalid callables.
     * 
     * Examples:
     * - Valid:   []() { DoWork(); }
     * - Valid:   void MyFunction() { }
     * - Invalid: []() { return 42; }  // Must return void
     * - Invalid: [](int x) { }        // Must take no arguments
     */
    template<typename Func>
    concept TaskCallable = requires {
        requires std::invocable<Func>;                           // Can be called with no args
        requires std::same_as<std::invoke_result_t<Func>, void>; // Must return void
        requires std::move_constructible<Func>;                   // Can be moved into task
    };
    /**
     * @brief Task execution state
     * 
     * State transitions are atomic and follow this pattern:
     * Ready -> Scheduled -> Running -> Completed
     * 
     * Retraction optimization allows: Scheduled -> Ready -> Running -> Completed
     * Cancellation paths: Ready -> Cancelled, Scheduled -> Cancelled
     * Tasks cannot be cancelled once Running (will complete normally)
     */
    enum class ETaskState : u8
    {
        Ready,      ///< Task created, ready to launch
        Scheduled,  ///< Task queued for execution (in local or global queue)
        Running,    ///< Task currently executing
        Completed,  ///< Task finished execution
        Cancelled   ///< Task was cancelled before execution
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
         * @brief Check if the task has been cancelled
         * @return True if task is in Cancelled state
         */
        bool IsCancelled() const
        {
            return GetState() == ETaskState::Cancelled;
        }

        /**
         * @brief Check if the task is done (completed or cancelled)
         * @return True if task is in Completed or Cancelled state
         */
        bool IsDone() const
        {
            ETaskState state = GetState();
            return state == ETaskState::Completed || state == ETaskState::Cancelled;
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
         * 
         * Validates that the transition follows the state machine:
         * Ready -> Scheduled -> Running -> Completed
         * 
         * Invalid transitions (e.g., Ready->Running, Completed->Running) are rejected.
         * 
         * @param expected The expected current state (updated on failure)
         * @param desired The desired new state
         * @return True if transition succeeded, false otherwise
         */
        bool TryTransitionState(ETaskState& expected, ETaskState desired)
        {
            // Validate transition is legal according to state machine
            if (!IsValidTransition(expected, desired))
            {
                // Don't update expected - the caller's expected state is wrong
                return false;
            }
            
            // Attempt atomic transition
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

        /**
         * @brief Add a prerequisite task that must complete before this task can run
         * 
         * This increments the prerequisite counter. When all prerequisites complete,
         * they will decrement the counter. When it reaches zero, the task is ready to schedule.
         * 
         * @param prerequisite The task that must complete first
         */
        void AddPrerequisite(Ref<Task> prerequisite);

        /**
         * @brief Get the current prerequisite count
         * @return Number of incomplete prerequisites
         */
        i32 GetPrerequisiteCount() const
        {
            return m_PrerequisiteCount.load(std::memory_order_acquire);
        }

        /**
         * @brief Check if all prerequisites are completed
         * @return True if no outstanding prerequisites
         */
        bool ArePrerequisitesComplete() const
        {
            return GetPrerequisiteCount() == 0;
        }

        /**
         * @brief Called when this task completes - notifies all subsequent tasks
         * 
         * This is called internally after the task Execute() completes.
         * It decrements prerequisite counters of all dependent tasks.
         */
        void OnCompleted();

        /**
         * @brief Cancel the task if not yet running
         * 
         * Attempts to cancel the task. Tasks can only be cancelled if they are in
         * Ready or Scheduled state. Once a task is Running, it cannot be cancelled
         * and will complete normally.
         * 
         * When a task is cancelled:
         * - Its state transitions to Cancelled
         * - Dependent tasks (subsequents) are notified (prerequisites decremented)
         * - The task is treated as "done" (IsDone() returns true)
         * - Execute() will never be called
         * 
         * @return True if task was successfully cancelled, false if already running/completed
         */
        bool Cancel();

    private:
        /**
         * @brief Check if adding a prerequisite would create a circular dependency
         * 
         * Uses DFS to detect if there's a path from prerequisite back to this task.
         * Only checks in debug builds for performance reasons.
         * 
         * @param prerequisite The task to check
         * @return True if adding would create a cycle
         */
        bool WouldCreateCycle(Ref<Task> prerequisite) const;

        /**
         * @brief Helper for cycle detection - DFS traversal
         * @param current Current task being visited
         * @param target Target task we're looking for
         * @param visited Set of visited tasks to prevent infinite loops
         * @return True if path from current to target exists
         */
        static bool HasPathTo(const Task* current, const Task* target, std::unordered_set<const Task*>& visited);

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

        /**
         * @brief Validate state transition follows state machine rules
         * 
         * Valid transitions:
         * - Ready -> Scheduled
         * - Ready -> Running (retraction fast-path - task pulled from queue and executed inline)
         * - Ready -> Cancelled (cancellation)
         * - Scheduled -> Running
         * - Scheduled -> Cancelled (cancellation)
         * - Scheduled -> Ready (retraction)
         * - Running -> Completed
         * 
         * All other transitions are invalid.
         * 
         * @param from Current state
         * @param to Desired state
         * @return True if transition is valid
         */
        static bool IsValidTransition(ETaskState from, ETaskState to)
        {
            switch (from)
            {
                case ETaskState::Ready:
                    return to == ETaskState::Scheduled 
                        || to == ETaskState::Running      // Retraction fast-path
                        || to == ETaskState::Cancelled;
                
                case ETaskState::Scheduled:
                    return to == ETaskState::Running 
                        || to == ETaskState::Cancelled 
                        || to == ETaskState::Ready;
                
                case ETaskState::Running:
                    return to == ETaskState::Completed;
                
                case ETaskState::Completed:
                case ETaskState::Cancelled:
                    return false;  // Cannot transition from terminal states
                
                default:
                    return false;
            }
        }

    protected:
        const char* m_DebugName;                    ///< Debug name (not owned, must be string literal or long-lived)
        ETaskPriority m_Priority;                   ///< Task priority level
        std::atomic<ETaskState> m_State;            ///< Current execution state

        // Dependency tracking (Phase 4)
        std::atomic<i32> m_PrerequisiteCount{0};    ///< Number of incomplete prerequisites
        std::vector<Ref<Task>> m_Subsequents;          ///< Tasks that depend on this one
        mutable std::mutex m_SubsequentsMutex;      ///< Protects m_Subsequents vector (mutable for const correctness)
    };

    /**
     * @brief Template task implementation with type-erased callable
     * 
     * This template wraps any callable (lambda, function, functor) and provides
     * small task optimization to avoid heap allocations for small captures.
     * 
     * The callable must satisfy the TaskCallable concept (invocable, returns void, move-constructible).
     * 
     * @tparam Callable The callable type (lambda, std::function, etc.)
     */
    template<TaskCallable Callable>
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
     * The callable must satisfy the TaskCallable concept:
     * - Invocable with no arguments
     * - Returns void (not int, bool, etc.)
     * - Move-constructible
     * 
     * @tparam Callable The callable type (deduced, must satisfy TaskCallable)
     * @param debugName Debug name for profiling and logging
     * @param priority Task priority level
     * @param func The callable to execute
     * @return Reference-counted task pointer
     * 
     * Example usage:
     * @code
     * // Valid:
     * auto task1 = CreateTask("MyTask", ETaskPriority::Normal, []() { DoWork(); });
     * auto task2 = CreateTask("MyTask", ETaskPriority::High, &MyFunction);
     * 
     * // Invalid (compile error with clear message):
     * auto task3 = CreateTask("Bad", ETaskPriority::Normal, []() { return 42; });  // Must return void
     * auto task4 = CreateTask("Bad", ETaskPriority::Normal, [](int x) { });        // Must take no args
     * @endcode
     */
    template<TaskCallable Callable>
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
