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
        
        // Check for circular dependencies - refuse to add if it would create a cycle
        if (WouldCreateCycle(prerequisite))
        {
            OLO_CORE_ERROR("Circular dependency detected! Task '{}' already depends on '{}'. Refusing to add prerequisite.",
                          prerequisite->GetDebugName() ? prerequisite->GetDebugName() : "unnamed",
                          GetDebugName() ? GetDebugName() : "unnamed");
            return;
        }
        
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

    bool Task::WouldCreateCycle(Ref<Task> prerequisite) const
    {
        // Check if there's a path from this task to the prerequisite
        // If this task already (transitively) depends on prerequisite through its subsequents,
        // then adding prerequisite as a dependency of this would create a cycle
        // Example: if A → B → C (A's subsequents lead to C), then trying to add C as prerequisite
        // of A (C → A) would create cycle: A → B → C → A
        
        std::unordered_set<const Task*> visited;
        return HasPathTo(this, prerequisite.Raw(), visited);
    }

    bool Task::HasPathTo(const Task* current, const Task* target, std::unordered_set<const Task*>& visited)
    {
        // Base case: reached target
        if (current == target)
        {
            return true;
        }

        // Already visited this task - no cycle through this path
        if (visited.count(current) > 0)
        {
            return false;
        }

        // Mark as visited
        visited.insert(current);

        // Check all of current's subsequents (tasks that depend on current)
        // If any of them have a path to target, then current has a path to target
        {
            std::lock_guard<std::mutex> lock(current->m_SubsequentsMutex);
            for (const auto& subsequent : current->m_Subsequents)
            {
                if (HasPathTo(subsequent.Raw(), target, visited))
                {
                    return true;
                }
            }
        }

        return false;
    }

} // namespace OloEngine
