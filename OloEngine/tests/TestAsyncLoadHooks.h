#pragma once

// Shared by the issue #1365 load-cancellation tests (RuntimeAssetLoadCancelTest,
// Streaming/SceneStreamerBudgetTest): a test-side event, the state a load hook
// touches, and the task-scheduler bootstrap.
//
// Hook state lives behind a shared_ptr that every hook captures BY VALUE. A hook
// runs on a worker that may still be blocked when a failed assertion returns
// from the test body; state on the test's stack would then be destroyed under
// it, and the fixture's shutdown would wait on a worker touching freed memory.

#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace OloEngine::Tests
{
    // A failure bound, never a synchronisation mechanism.
    inline constexpr std::chrono::milliseconds kLoadHookFailAfter{ 10000 };

    // A one-shot manual-reset event. std primitives on purpose: the thing under
    // test is the engine's task stack, not these.
    class Signal
    {
      public:
        void Set()
        {
            {
                std::lock_guard lock(m_Mutex);
                m_Set = true;
            }
            m_Cv.notify_all();
        }
        bool WaitFor(std::chrono::milliseconds timeout = kLoadHookFailAfter)
        {
            std::unique_lock lock(m_Mutex);
            return m_Cv.wait_for(lock, timeout, [this]
                                 { return m_Set; });
        }

      private:
        std::mutex m_Mutex;
        std::condition_variable m_Cv;
        bool m_Set = false;
    };

    struct LoadHookState
    {
        Signal WorkerInside; // the worker reached the blocking hook
        Signal Release;      // the test lets the worker go on
        std::atomic<int> Started{ 0 };
        std::atomic<bool> ProducedResult{ false };
        // Ordering evidence for the shutdown tests: each side takes the next number.
        std::atomic<int> Sequence{ 0 };
        std::atomic<int> WorkerExitedAt{ -1 };
        std::atomic<int> ShutdownReturnedAt{ -1 };

        // Block the calling worker until Release (or the failure bound).
        void Block()
        {
            WorkerInside.Set();
            Release.WaitFor();
        }
    };

    inline std::shared_ptr<LoadHookState> MakeLoadHookState()
    {
        return std::make_shared<LoadHookState>();
    }

    // The FunctionalTest way (notes-core-and-threading.md §5), with no matching
    // StopWorkers. StartWorkers runs on every call because suites such as
    // TaskSystemTest stop the workers in their teardown; it is a no-op while
    // workers are running.
    inline void EnsureTaskSchedulerStarted()
    {
        static const bool s_GameThreadAttached = []
        {
            LowLevelTasks::InitGameThreadId();
            Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
            return true;
        }();
        (void)s_GameThreadAttached;
        LowLevelTasks::FScheduler::Get().StartWorkers();
    }
} // namespace OloEngine::Tests
