#include "OloEnginePCH.h"
#include "TaskEvent.h"
#include "TaskScheduler.h"
#include "TaskWait.h"

namespace OloEngine
{
    TaskEvent::TaskEvent(const char* debugName)
    {
        // Create an empty task that does nothing
        // The event is "triggered" when this task completes
        m_EventTask = CreateTask(debugName, ETaskPriority::Normal, []() {
            // Empty - event task does nothing
        });
    }

    void TaskEvent::AddPrerequisites(std::initializer_list<Ref<Task>> prerequisites)
    {
        for (const auto& prereq : prerequisites)
        {
            AddPrerequisite(prereq);
        }
    }

    void TaskEvent::AddPrerequisite(Ref<Task> prerequisite)
    {
        if (m_EventTask)
        {
            m_EventTask->AddPrerequisite(prerequisite);
        }
    }

    void TaskEvent::Trigger()
    {
        if (!m_EventTask || m_EventTask->IsCompleted())
            return;

        // If event has no outstanding prerequisites, launch it now
        if (m_EventTask->ArePrerequisitesComplete())
        {
            if (TaskScheduler::IsInitialized())
            {
                TaskScheduler::Get().LaunchTask(m_EventTask);
            }
        }
        // Otherwise, it will be launched automatically by Task::OnCompleted()
        // when the last prerequisite completes
    }

    void TaskEvent::Wait()
    {
        if (!m_EventTask)
            return;

        // If already triggered, return immediately
        if (IsTriggered())
            return;

        // Trigger the event if it hasn't been triggered yet
        // (in case user forgot to call Trigger())
        Trigger();

        // Use the hybrid wait strategy from TaskWait
        TaskWait::Wait(m_EventTask);
    }

} // namespace OloEngine
