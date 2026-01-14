#include "OloEnginePCH.h"
#include "JoltJobSystemAdapter.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    JoltJobSystemAdapter::JoltJobSystemAdapter(u32 inMaxJobs, u32 inMaxBarriers)
    {
        // Initialize the barrier system (provided by JobSystemWithBarrier base class)
        JPH::JobSystemWithBarrier::Init(inMaxBarriers);

        // Initialize the job free list
        m_Jobs.Init(inMaxJobs, inMaxJobs);

        OLO_CORE_INFO("JoltJobSystemAdapter initialized - MaxJobs: {0}, MaxBarriers: {1}", inMaxJobs, inMaxBarriers);
    }

    JoltJobSystemAdapter::~JoltJobSystemAdapter()
    {
        // Jobs are freed automatically by the free list destructor
    }

    i32 JoltJobSystemAdapter::GetMaxConcurrency() const
    {
        // Return the number of worker threads in the scheduler
        // FScheduler tracks workers; we add +1 for the calling thread that can also execute jobs during WaitForJobs
        return static_cast<i32>(LowLevelTasks::FScheduler::Get().GetNumWorkers()) + 1;
    }

    JPH::JobSystem::JobHandle JoltJobSystemAdapter::CreateJob(const char* inName, JPH::ColorArg inColor,
                                                              const JPH::JobSystem::JobFunction& inJobFunction,
                                                              u32 inNumDependencies)
    {
        // Allocate a job from the free list (returns index)
        u32 index = m_Jobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);

        // Get pointer to the job from index
        JPH::JobSystem::Job* job = &m_Jobs.Get(index);

        // Create a JobHandle
        JPH::JobSystem::JobHandle handle(job);

        // If no dependencies, queue immediately
        if (inNumDependencies == 0)
            QueueJob(job);

        return handle;
    }

    void JoltJobSystemAdapter::QueueJob(JPH::JobSystem::Job* inJob)
    {
        // Add reference since we're queuing the job
        inJob->AddRef();

        // Launch as a high-priority task (physics is critical)
        // Capture inJob by value (ref-counted pointer) to keep it alive
        Tasks::Launch(
            "JoltPhysicsJob",
            [inJob]()
            {
                // Execute the Jolt job
                inJob->Execute();

                // Release our reference
                inJob->Release();
            },
            LowLevelTasks::ETaskPriority::High);
    }

    void JoltJobSystemAdapter::QueueJobs(JPH::JobSystem::Job** inJobs, u32 inNumJobs)
    {
        // Queue all jobs individually
        for (u32 i = 0; i < inNumJobs; ++i)
        {
            QueueJob(inJobs[i]);
        }
    }

    void JoltJobSystemAdapter::FreeJob(JPH::JobSystem::Job* inJob)
    {
        // DestructObject accepts a Job pointer directly and will look up its index internally
        m_Jobs.DestructObject(inJob);
    }

} // namespace OloEngine
