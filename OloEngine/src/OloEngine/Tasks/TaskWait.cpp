#include "OloEnginePCH.h"
#include "TaskWait.h"
#include "TaskScheduler.h"
#include "TaskEvent.h"
#include "WorkerThread.h"
#include "OversubscriptionScope.h"

#include <thread>

#ifdef _MSC_VER
#include <intrin.h>  // For _mm_pause
#else
#include <emmintrin.h>  // For _mm_pause on other compilers
#endif

namespace OloEngine
{
    // Thread-local pointer to current worker thread (if any)
    thread_local WorkerThread* t_CurrentWorker = nullptr;

    // Allow WorkerThread to set the thread-local pointer
    void SetCurrentWorkerThread(WorkerThread* worker)
    {
        t_CurrentWorker = worker;
    }

    WorkerThread* GetCurrentWorkerThread()
    {
        return t_CurrentWorker;
    }

    namespace TaskWait
    {
        bool TryRetractAndExecute(Ref<Task> task)
        {
            OLO_PROFILE_FUNCTION();
            
            if (!task)
                return false;

            // If already done (completed or cancelled), nothing to do
            if (task->IsDone())
            {
                return true;
            }

            // Try to transition from Scheduled back to Ready
            // This prevents the worker thread from executing it
            ETaskState expected = ETaskState::Scheduled;
            if (!task->TryTransitionState(expected, ETaskState::Ready))
            {
                // Task is not in Scheduled state - either already Running or Completed
                // or never was scheduled (in Ready state with prerequisites)
                return false;
            }

            // Successfully retracted - now execute inline
            // Use optimized path: Ready -> Running (skipping Scheduled state)
            // This is safe because we've pulled the task from the queue and no
            // other thread can access it (worker threads check Scheduled state)
            expected = ETaskState::Ready;
            if (!task->TryTransitionState(expected, ETaskState::Running))
            {
                // State changed between our retraction and now - extremely unlikely
                // Another thread would need to interact with a task we just retracted
                return false;
            }

            // Execute the task with exception handling
            try
            {
                task->Execute();
            }
            catch (...)
            {
                // Exception caught - task will still be marked as completed
            }

            // Mark as completed
            task->SetState(ETaskState::Completed);

            // Notify scheduler for statistics
            if (TaskScheduler::IsInitialized())
            {
                TaskScheduler::Get().OnTaskCompleted();
            }

            // Notify dependent tasks
            task->OnCompleted();

            return true;
        }

        void Wait(Ref<Task> task)
        {
            OLO_PROFILE_FUNCTION();
            
            if (!task)
                return;

            // Strategy 1: Try retraction first (best case - no wait at all)
            if (TryRetractAndExecute(task))
            {
                return;
            }

            // Check if we're on a worker thread
            WorkerThread* currentWorker = GetCurrentWorkerThread();
            
            if (currentWorker)
            {
                // Indicate we're blocking - may spawn standby worker to prevent deadlock
                OversubscriptionScope oversubScope;
                
                // Strategy 2: Execute other tasks while waiting (keep worker productive)
                u32 spinCount = 0;
                const u32 maxSpinBeforeWork = 40;
                
                while (!task->IsDone())
                {
                    // Try to find other work to do
                    Ref<Task> otherTask = currentWorker->FindWorkPublic();
                    if (otherTask)
                    {
                        // Found work - execute it and reset spin count
                        currentWorker->ExecuteTaskPublic(otherTask);
                        spinCount = 0;
                    }
                    else
                    {
                        // No work found - spin briefly
                        if (++spinCount < maxSpinBeforeWork)
                        {
                            _mm_pause();
                        }
                        else
                        {
                            // Been spinning too long - break out and do blocking wait
                            break;
                        }
                    }
                }
            }

            // Strategy 3: Fall back to spin-then-sleep wait
            if (!task->IsDone())
            {
                // Brief spin
                for (u32 i = 0; i < 40 && !task->IsDone(); ++i)
                {
                    _mm_pause();
                }
                
                // Yield if still not done
                while (!task->IsDone())
                {
                    std::this_thread::yield();
                }
            }
        }

        void WaitForAll(const std::vector<Ref<Task>>& tasks)
        {
            OLO_PROFILE_FUNCTION();
            
            if (tasks.empty())
                return;

            // Quick check: if all tasks are already done, return immediately
            bool allDone = true;
            for (const auto& task : tasks)
            {
                if (task && !task->IsDone())
                {
                    allDone = false;
                    break;
                }
            }
            
            if (allDone)
                return;

            // Create an event that depends on all tasks
            TaskEvent allCompleteEvent("WaitForAll");
            for (const auto& task : tasks)
            {
                if (task)
                {
                    allCompleteEvent.AddPrerequisite(task);
                }
            }

            // Trigger and wait for the event
            allCompleteEvent.Trigger();
            
            // CRITICAL FIX: The event might have triggered immediately if all prerequisites
            // were already done. But we still need to ensure ALL original tasks are complete.
            // Check again and wait for any tasks that aren't done yet.
            for (const auto& task : tasks)
            {
                if (task && !task->IsDone())
                {
                    Wait(task);
                }
            }
        }

        void WaitForAll(std::initializer_list<Ref<Task>> tasks)
        {
            WaitForAll(std::vector<Ref<Task>>(tasks));
        }

        i32 WaitForAny(const std::vector<Ref<Task>>& tasks)
        {
            OLO_PROFILE_FUNCTION();
            
            if (tasks.empty())
                return -1;

            // Quick check: see if any task is already done
            for (sizet i = 0; i < tasks.size(); ++i)
            {
                if (tasks[i] && tasks[i]->IsDone())
                {
                    return static_cast<i32>(i);
                }
            }

            // Check if we're on a worker thread
            WorkerThread* currentWorker = GetCurrentWorkerThread();
            
            if (currentWorker)
            {
                // Indicate we're blocking - may spawn standby worker to prevent deadlock
                OversubscriptionScope oversubScope;
                
                // Strategy: Execute other tasks while waiting
                u32 spinCount = 0;
                const u32 maxSpinBeforeWork = 40;
                
                while (true)
                {
                    // Check if any task completed
                    for (sizet i = 0; i < tasks.size(); ++i)
                    {
                        if (tasks[i] && tasks[i]->IsDone())
                        {
                            return static_cast<i32>(i);
                        }
                    }
                    
                    // Try to find other work to do
                    Ref<Task> otherTask = currentWorker->FindWorkPublic();
                    if (otherTask)
                    {
                        // Found work - execute it and reset spin count
                        currentWorker->ExecuteTaskPublic(otherTask);
                        spinCount = 0;
                    }
                    else
                    {
                        // No work found - spin briefly
                        if (++spinCount < maxSpinBeforeWork)
                        {
                            _mm_pause();
                        }
                        else
                        {
                            // Been spinning too long - break out and do blocking wait
                            break;
                        }
                    }
                }
            }

            // Fall back to spin-then-yield wait
            // Brief spin
            for (u32 spinIter = 0; spinIter < 40; ++spinIter)
            {
                // Check if any task completed
                for (sizet i = 0; i < tasks.size(); ++i)
                {
                    if (tasks[i] && tasks[i]->IsDone())
                    {
                        return static_cast<i32>(i);
                    }
                }
                _mm_pause();
            }
            
            // Yield loop until any task completes
            while (true)
            {
                // Check if any task completed
                for (sizet i = 0; i < tasks.size(); ++i)
                {
                    if (tasks[i] && tasks[i]->IsDone())
                    {
                        return static_cast<i32>(i);
                    }
                }
                std::this_thread::yield();
            }
        }

        i32 WaitForAny(std::initializer_list<Ref<Task>> tasks)
        {
            return WaitForAny(std::vector<Ref<Task>>(tasks));
        }

    } // namespace TaskWait

} // namespace OloEngine
