#include "OloEnginePCH.h"
#include "JoltJobSystemAdapter.h"
#include "OloEngine/Core/Log.h"

#include <chrono>
#include <thread>

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
        // Wait for any scheduler-queued QueueJob lambdas to fully return before letting
        // m_Jobs destruct. Jolt's PhysicsSystem::Update barrier-waits on Job::Execute()
        // returning, but our lambda runs `inJob->Release()` after Execute, and Release
        // can call back into FreeJob(this) → m_Jobs.DestructObject. If the destructor
        // tears down m_Jobs first, the worker thread dereferences freed Job memory —
        // exactly the TSan race that bit MultipleScenesCoexistTest, SceneStepAdvances*,
        // TriggerEndEvent*, and CharacterControllerJumps* on Linux CI. Same drain the
        // per-step path runs after each PhysicsSystem::Update (see WaitForOutstandingTasks).
        WaitForOutstandingTasks();
    }

    void JoltJobSystemAdapter::WaitForOutstandingTasks() noexcept
    {
        // Spin-wait until every in-flight QueueJob lambda has decremented m_OutstandingTasks,
        // which it does only after Job::Execute() AND Job::Release() (→ possible FreeJob on
        // m_Jobs) have both fully returned. See the header for why Jolt's barrier alone is
        // not enough. Bounded by a timeout so a wedged worker can't hang the simulation.
        using clock = std::chrono::steady_clock;
        constexpr auto kTimeout = std::chrono::seconds(5);
        const auto deadline = clock::now() + kTimeout;
        while (m_OutstandingTasks.load(std::memory_order_acquire) > 0)
        {
            if (clock::now() > deadline)
            {
                OLO_CORE_ERROR("JoltJobSystemAdapter: timed out waiting for {} outstanding physics job(s) to drain",
                               m_OutstandingTasks.load(std::memory_order_relaxed));
                break;
            }
            std::this_thread::yield();
        }
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

        // Bump the outstanding-tasks counter BEFORE Tasks::Launch — the destructor's
        // wait must observe every in-flight lambda, and the counter has to be live by
        // the time the destructor checks. Decrement happens at the lambda end, after
        // every member access through `this` is done.
        m_OutstandingTasks.fetch_add(1, std::memory_order_release);

        // Launch as a high-priority task (physics is critical).
        // Capture inJob by value (ref-counted pointer) to keep it alive; `this` is
        // kept alive by the destructor wait above.
        Tasks::Launch(
            "JoltPhysicsJob",
            [inJob, this]()
            {
                // Execute the Jolt job
                inJob->Execute();

                // Release our reference. This can call back into FreeJob(this) →
                // m_Jobs, so it must complete before the destructor releases m_Jobs;
                // the outstanding-tasks counter is decremented strictly AFTER this.
                inJob->Release();

                m_OutstandingTasks.fetch_sub(1, std::memory_order_release);
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
