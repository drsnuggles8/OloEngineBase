#include "OloEnginePCH.h"
#include "Task.h"
#include "TaskScheduler.h"

namespace OloEngine
{
    // Task base class implementation
    // Most functionality is in the header due to templates,
    // but we include this file for potential future non-template methods

    void Task::AddPrerequisite(Ref<Task> prerequisite)
    {
        OLO_CORE_ASSERT(prerequisite, "Cannot add null prerequisite");
        
        // If prerequisite is already completed, don't add dependency
        if (prerequisite->IsCompleted())
        {
            return;
        }

        // Increment our prerequisite counter
        m_PrerequisiteCount.fetch_add(1, std::memory_order_release);

        // Add this task to the prerequisite's subsequent list
        {
            std::lock_guard<std::mutex> lock(prerequisite->m_SubsequentsMutex);
            prerequisite->m_Subsequents.push_back(Ref<Task>(this));
        }

        // Race condition check: prerequisite might have completed between the initial
        // IsCompleted() check and adding ourselves to the subsequents list. If so,
        // OnCompleted() has already run and won't decrement our count, so we need to
        // manually decrement it and potentially launch ourselves.
        if (prerequisite->IsCompleted())
        {
            i32 prevCount = m_PrerequisiteCount.fetch_sub(1, std::memory_order_acq_rel);
            
            // If this was the last prerequisite, we're ready to launch
            if (prevCount == 1 && GetState() == ETaskState::Ready)
            {
                if (TaskScheduler::IsInitialized())
                {
                    TaskScheduler::Get().LaunchTask(Ref<Task>(this));
                }
            }
        }
    }

    void Task::OnCompleted()
    {
        // Notify all subsequent tasks that we've completed
        std::vector<Ref<Task>> subsequents;
        
        {
            std::lock_guard<std::mutex> lock(m_SubsequentsMutex);
            subsequents = std::move(m_Subsequents);
            m_Subsequents.clear();
        }

        // Decrement prerequisite count for each subsequent task
        for (auto& subsequent : subsequents)
        {
            i32 prevCount = subsequent->m_PrerequisiteCount.fetch_sub(1, std::memory_order_acq_rel);
            
            // If this was the last prerequisite, the task is ready to schedule
            if (prevCount == 1)  // Was 1, now 0
            {
                // Task is ready - launch it through the scheduler
                // LaunchTask will handle the Ready->Scheduled transition
                if (TaskScheduler::IsInitialized())
                {
                    TaskScheduler::Get().LaunchTask(subsequent);
                }
            }
        }
    }

} // namespace OloEngine
